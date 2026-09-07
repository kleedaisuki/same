#pragma once
#include "same/compute.hpp"
#include "same/config.hpp"
#include "same/detail/dispatch.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace same {
/// 单线程独占的复用缓冲及可降级后端。 / Reusable buffers and fallback-capable backend exclusively
/// owned by one worker thread.
struct Worker {
    /// 两个等容量输入缓冲，任务结束后复用。 / Equal-capacity input buffers reused between tasks.
    std::vector<std::byte> first, second;
    /// 运行期间可能永久替换为 CPU 的当前后端。 / Active backend, possibly permanently replaced with
    /// CPU during execution.
    std::unique_ptr<Compute> compute;
    /// 小文件策略复用的 CPU SIMD 后端，不属于错误降级。
    /// Reusable CPU SIMD backend for small-file policy, not an error fallback.
    std::unique_ptr<Compute> cpu_compute;
    /// 按大小路由到 CPU 的哈希次数，仅工作线程写入。
    /// Hash attempts routed to CPU by size; worker-thread-only writes.
    std::uint64_t cpu_routed_hashes{};
    /// 自动模式要求至少一个完整配置块；显式 CUDA 模式保持用户下界。
    /// Auto requires at least one full configured block; explicit CUDA keeps the user floor.
    std::size_t gpu_floor{};
    /// 实际后端哈希尝试数（错误重试也计入）。 / Actual backend hash attempts, including retries.
    std::uint64_t cpu_hashes{}, gpu_hashes{};
    /// 非拥有计数器，Resources 保证其比工作线程存活更久。 / Non-owning counter kept alive by
    /// Resources until workers stop.
    std::atomic<std::size_t>* fallbacks;
    /// Actual successful read bytes, including backend retries; worker-thread-only writes.
    /// 实际成功读取字节，包含后端重试；仅所属工作线程写入。
    std::uint64_t hash_bytes{0}, compare_bytes{0};

    /**
     * @brief 后端失败时换为 CPU，并完整重试一次。 / Replace a failed backend with CPU and retry
     * once in full. operation 必须可从头重入，重建摘要并重置文件位置；不可捕获旧后端指针。
     * operation must restart its hash and file position and must not retain the old backend
     * pointer. 仅捕获 ComputeError；第二次失败和其他异常交给调用者。 Only ComputeError is caught; a
     * retry failure or other exception propagates.
     */
    template <class F> auto execute(F&& operation) -> std::invoke_result_t<F> {
        try {
            return operation();
        } catch (const ComputeError&) {
            compute = make_cpu_compute();
            ++*fallbacks;
            return operation();
        }
    }
};

/**
 * @brief 哈希和比较共享的有界线程池。 / Bounded worker pool shared by hashing and comparison.
 * 启动前分配缓冲；满队列阻塞生产者。任务不得持有 Worker 引用超过调用期。
 * Buffers are allocated before startup; a full queue blocks producers. Tasks must not retain Worker
 * references. 不应在池内任务中等待向同一满队列提交或等待依赖任务，否则可能死锁。 Do not block pool
 * tasks on submission to this full queue or on dependent pool tasks: this can deadlock.
 * 销毁时排空并等待线程；调用者须保证销毁不与外部成员调用并发。
 * Destruction drains and joins workers; callers must prevent concurrent external member access
 * during destruction.
 * @code
 * Resources resources(config);
 * auto result = resources.submit([](Worker& worker) { return worker.compute->name(); });
 * auto backend = result.get();
 * @endcode
 */
class Resources {
public:
    /// 校验并预分配缓冲；auto 不探测 GPU，显式 cuda 在启动线程前探测。
    /// Reserve validated buffers; auto defers probing, explicit cuda probes before thread startup.
    explicit Resources(const Config& config);
    /// 排空已接收任务并等待所有工作线程。 / Drain admitted tasks and join every worker.
    ~Resources();
    /// 线程及队列具有唯一所有权。 / Threads and queue have unique ownership.
    Resources(const Resources&) = delete;
    /// 禁止复制同步状态。 / Synchronization state cannot be copied.
    Resources& operator=(const Resources&) = delete;

    /// 有界阻塞提交；返回的 future 保存结果或任务异常。 / Bounded blocking submission; the future
    /// holds the result or task exception. prefer_gpu only pins auto-enabled lane zero; otherwise
    /// admission remains generic. prefer_gpu 仅固定已启用的 auto 第零通道，否则仍普通提交。
    template <class F>
    auto submit(F&& operation, bool prefer_gpu = false)
        -> std::future<std::invoke_result_t<F, Worker&>> {
        using Task = std::packaged_task<std::invoke_result_t<F, Worker&>(Worker&)>;
        auto task = std::make_shared<Task>(std::forward<F>(operation));
        auto future = task->get_future();
        enqueue([task](Worker& worker) { (*task)(worker); }, prefer_gpu);
        return future;
    }
    /** 排空调用方全部任务后至多调用一次；不允许并发提交。 / Call at most once after
     * draining all caller jobs; concurrent submission is forbidden. Only pending, unread bytes
     * may amortize setup. 仅尚未读取的待处理字节可摊销初始化成本。
     */
    void prepare_auto(const Config& config, std::uint64_t pending_bytes, std::size_t pending_files);
    /// 构造或惰性探测实际启用的 GPU 数，支持并发读取，不扣除运行时降级。
    /// GPU lanes enabled at construction or lazy probing; concurrent reads are safe, runtime
    /// fallback is not subtracted.
    std::size_t gpu_workers() const {
        return gpu_workers_.load(std::memory_order_relaxed);
    }
    /// 运行时整体重试次数，不包含启动探测失败。 / Runtime retry count, excluding failed startup
    /// probes.
    std::size_t fallbacks() const {
        return fallbacks_.load();
    }

    /// Sum after all submitted futures complete; never call concurrently with jobs.
    /// 仅在所有已提交 future 完成后求和，不得与任务并发调用。
    std::pair<std::uint64_t, std::uint64_t> read_bytes() const {
        std::uint64_t hashed = 0, compared = 0;
        for (const auto& worker : workers_) {
            hashed += worker->hash_bytes;
            compared += worker->compare_bytes;
        }
        return {hashed, compared};
    }
    /// 所有任务完成后读取策略计数，不得并发访问。 / Read policy counts only after all jobs finish.
    std::uint64_t cpu_routed_hashes() const {
        std::uint64_t result = 0;
        for (const auto& worker : workers_)
            result += worker->cpu_routed_hashes;
        return result;
    }
    /// 空闲屏障探测后读取；不得与 prepare_auto 并发访问。 / Read after idle-barrier probing;
    /// never access concurrently with prepare_auto.
    const detail::DispatchEvidence& dispatch_evidence() const {
        return dispatch_;
    }
    /// 所有任务完成后读取 CPU/GPU 实际尝试数。 / Read actual CPU/GPU attempts after all jobs
    /// finish.
    std::pair<std::uint64_t, std::uint64_t> hash_attempts() const {
        std::uint64_t cpu = 0, gpu = 0;
        for (const auto& worker : workers_) {
            cpu += worker->cpu_hashes;
            gpu += worker->gpu_hashes;
        }
        return {cpu, gpu};
    }

private:
    /// 单次运行的性能选择，不跨机器/驱动/配置持久化。
    /// Per-run performance selection; never persisted across hardware/driver/config changes.
    detail::DispatchEvidence dispatch_;
    /// 自动模式仅探测一次；preferred 标志仅空闲屏障期间修改。 / Probe auto once; change routing
    /// only at idle barriers.
    bool auto_mode_{}, auto_attempted_{}, preferred_enabled_{};
    /// 等待队列空间；关闭后拒绝新任务。 / Wait for queue space; reject work after closure.
    void enqueue(std::function<void(Worker&)> task, bool prefer_gpu);
    /// 同步启动指定数量的真实工作线程，首线程可用 GPU。 / Gate real workers, optionally GPU on lane
    /// zero.
    double probe_mixed(std::size_t count, std::size_t jobs, bool gpu, const Digest& expected);
    /// 队列锁外执行任务，关闭后继续排空队列。 / Execute outside the queue lock and drain queued
    /// work after closure.
    void run(Worker& worker);
    /// 唤醒等待者并 join；仅由拥有者从非工作线程调用。 / Wake waiters and join; owner calls only
    /// from a non-worker thread.
    void close();
    /// 跨工作线程共享的原子降级计数。 / Atomic fallback count shared across workers.
    std::atomic<std::size_t> fallbacks_{0};
    /// 构造或惰性探测启用的 GPU 数；不扣除任务错误降级。 / GPU lanes enabled at construction
    /// or lazy probing; runtime failure fallback does not decrement this count.
    std::atomic<std::size_t> gpu_workers_{0};
    /// 排队任务上限，不含正在执行的任务。 / Maximum queued tasks, excluding running tasks.
    std::size_t capacity_;
    /// 地址稳定的工作上下文，在线程退出后释放。 / Stable-address worker contexts released after
    /// threads exit.
    std::vector<std::unique_ptr<Worker>> workers_;
    /// 每个线程固定使用一个 Worker。 / Each thread permanently uses one Worker.
    std::vector<std::thread> threads_;
    /// 由 mutex_ 保护的先进先出任务队列。 / FIFO task queue protected by mutex_.
    std::deque<std::function<void(Worker&)>> queue_;
    /// 与普通队列共享总容量，仅第零线程消费。 / Shares total capacity; consumed only by lane zero.
    std::deque<std::function<void(Worker&)>> gpu_queue_;
    /// mutex 下的执行中任务数，处理 future 就绪早于任务返回的短窗口。
    /// Active jobs under mutex, covering the gap between future readiness and task return.
    std::size_t active_{};
    /// 同时保护 queue_ 和 closed_。 / Protects both queue_ and closed_.
    std::mutex mutex_;
    /// 分别通知消费者有任务、生产者有空间或两者关闭。 / Signal work, space, or closure to consumers
    /// and producers.
    std::condition_variable ready_, space_;
    /// 单向关闭状态；关闭不丢弃已排队任务。 / One-way closure flag; closure does not discard queued
    /// tasks.
    bool closed_{false};
};
} // namespace same
