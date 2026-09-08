#pragma once
#include "same/compute.hpp"
#include "same/config.hpp"
#include "same/detail/dispatch.hpp"
#include "same/detail/online_model.hpp"
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
namespace detail {
/// 可注入设备创建器；返回空表示不可用。 / Injectable device factory; null means unavailable.
using CudaFactory = std::function<std::unique_ptr<Compute>(std::size_t, std::size_t)>;
/// 比较完成成本；等待与服务时间必须非负且有限，误差不是置信区间。
/// Compare completion costs; waits/service must be nonnegative finite, errors are not confidence
/// bounds.
inline bool gpu_finishes_first(OnlineModel::Prediction cpu, OnlineModel::Prediction gpu,
                               double cpu_wait, double gpu_wait) noexcept {
    return gpu_wait + gpu.ms + gpu.error_ms < 0.95 * (cpu_wait + cpu.ms) - cpu.error_ms;
}
} // namespace detail
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
    /// 文件资格仅取显式大小下界，与缓冲容量无关。 / Eligibility is the explicit size floor,
    /// independent of buffers.
    std::size_t gpu_floor{};
    /// 实际后端哈希尝试数（错误重试也计入）。 / Actual backend hash attempts, including retries.
    std::uint64_t cpu_hashes{}, gpu_hashes{};
    /// 非拥有计数器，Resources 保证其比工作线程存活更久。 / Non-owning counter kept alive by
    /// Resources until workers stop.
    std::atomic<std::size_t>* fallbacks;
    /// Actual successful read bytes, including backend retries; worker-thread-only writes.
    /// 实际成功读取字节，包含后端重试；仅所属工作线程写入。
    std::uint64_t hash_bytes{0}, compare_bytes{0};

    /// 成功任务的固定大小观测槽；仅所属线程写，完成锁内消费。
    /// Fixed-size successful-task sample; worker writes, completion lock consumes.
    struct Sample {
        /// 实际输入字节和已有计时（毫秒）。 / Input bytes and existing duration in ms.
        std::uint64_t bytes{};
        double service_ms{};
        /// 实际后端与完整成功标记。 / Actual backend and complete-success marker.
        bool gpu{}, valid{};
    } sample;
    /// 关闭 PGO 时不采样；小任务的有界采样序号由所属线程维护。
    /// No sampling with PGO disabled; worker-owned bounded small-task sampling sequence.
    bool profile_enabled{};
    std::uint64_t profile_sequence{};
    /// 锁内预测的完成时刻，零表示没有可靠估计。 / Locked predicted finish, zero if unknown.
    double predicted_finish_ms{};

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
    /// 注入设备工厂供确定性验证。 / Inject a device factory for deterministic validation.
    Resources(const Config& config, detail::CudaFactory factory);
    /// 等待排队及执行中任务归零。 / Wait until queued and executing tasks are empty.
    void wait_idle();
    /// 空闲后读取溢出 GPU 和回流 CPU 的任务数。 / Read GPU overflow and CPU spill counts after
    /// idle.
    std::pair<std::uint64_t, std::uint64_t> route_counts() const {
        return {gpu_overflow_jobs_, cpu_spill_jobs_};
    }
    /// 排空已接收任务并等待所有工作线程。 / Drain admitted tasks and join every worker.
    ~Resources();
    /// 线程及队列具有唯一所有权。 / Threads and queue have unique ownership.
    Resources(const Resources&) = delete;
    /// 禁止复制同步状态。 / Synchronization state cannot be copied.
    Resources& operator=(const Resources&) = delete;

    /// 有界阻塞提交；返回的 future 保存结果或任务异常。 / Bounded blocking submission; the future
    /// holds the result or task exception. prefer_gpu pins the live automatic GPU service;
    /// fallback is CPU-reclaimable. prefer_gpu 固定存活的自动 GPU 服务，失败后 CPU 可回收。
    template <class F>
    auto submit(F&& operation, bool prefer_gpu = false)
        -> std::future<std::invoke_result_t<F, Worker&>> {
        return submit_impl(std::forward<F>(operation), prefer_gpu, detail::HashRoute::cpu_only);
    }
    /** 有资格的哈希优先送 GPU 通道，但 CPU 可在普通队列空时领取。
     * Eligible hashes prefer the GPU lane; CPU workers steal when their normal queue is empty.
     * 与固定通道 submit(..., true) 不同，这不会将整批哈希串行化；总队列容量不变。
     * Unlike pinned submit(..., true), this does not serialize a batch; capacity stays shared.
     * @code
     * auto result = resources.submit_hash(operation, file_size >= gpu_floor);
     * @endcode
     */
    template <class F>
    auto submit_hash(F&& operation, bool gpu_eligible)
        -> std::future<std::invoke_result_t<F, Worker&>> {
        return submit_hash(std::forward<F>(operation), gpu_eligible
                                                           ? detail::HashRoute::gpu_preferred
                                                           : detail::HashRoute::cpu_only);
    }
    /// 按载荷偏好提交；CPU-only 永不进入自动 GPU 服务。 / Submit by payload preference;
    /// CPU-only work never enters the automatic GPU service.
    template <class F>
    auto submit_hash(F&& operation, detail::HashRoute route, std::uint64_t bytes = 0)
        -> std::future<std::invoke_result_t<F, Worker&>> {
        return submit_impl(std::forward<F>(operation), false, route, bytes);
    }
    /** 使用独立缓冲同步验证一次，不运行性能校准。 / Validate synchronously once using
     * independent buffers without performance calibration. 不得并发调用本函数或读取统计。
     * Do not call this method concurrently with itself or statistics access.
     */
    void prepare_auto(const Config& config, std::uint64_t pending_bytes, std::size_t pending_files);
    /// 构造或惰性探测实际启用的 GPU 数，支持并发读取，不扣除运行时降级。
    /// GPU lanes enabled at construction or lazy probing; concurrent reads are safe, runtime
    /// fallback is not subtracted.
    std::size_t gpu_workers() const {
        return gpu_workers_.load(std::memory_order_relaxed);
    }
    /// 自动服务实际输入块；未启用时返回 CPU 配置块。 / Auto service block, or configured CPU block.
    std::size_t gpu_block_bytes() const {
        return gpu_worker_ ? gpu_worker_->first.size() : workers_.front()->first.size();
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
        if (gpu_worker_) {
            hashed += gpu_worker_->hash_bytes;
            compared += gpu_worker_->compare_bytes;
        }
        return {hashed, compared};
    }
    /// 所有任务完成后读取策略计数，不得并发访问。 / Read policy counts only after all jobs finish.
    std::uint64_t cpu_routed_hashes() const {
        std::uint64_t result = 0;
        for (const auto& worker : workers_)
            result += worker->cpu_routed_hashes;
        if (gpu_worker_)
            result += gpu_worker_->cpu_routed_hashes;
        return result;
    }
    /// 同步探测完成后读取；不得与 prepare_auto 并发访问。 / Read after synchronous probing;
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
        if (gpu_worker_) {
            cpu += gpu_worker_->cpu_hashes;
            gpu += gpu_worker_->gpu_hashes;
        }
        return {cpu, gpu};
    }

    /// 空闲后读取连续剖析快照；不得与任务并发。 / Read profiling snapshot after idle only.
    auto profile_snapshot() const {
        return model_.snapshot();
    }
    /// 空闲后读取实际探索任务数。 / Actual exploration jobs after idle.
    std::uint64_t exploration_jobs() const {
        return exploration_jobs_;
    }
    /// 固定剖析开关。 / Frozen profiling switch.
    bool profiling_enabled() const {
        return pgo_;
    }
    /// 空闲后读取服务存活状态。 / Read service liveness after idle.
    bool gpu_service_enabled() const {
        return preferred_enabled_;
    }

private:
    /// 统一打包与异常传递，仅排队类别不同。 / Shared packaging/exception contract for all routes.
    template <class F>
    auto submit_impl(F&& operation, bool pinned, detail::HashRoute route, std::uint64_t bytes = 0)
        -> std::future<std::invoke_result_t<F, Worker&>> {
        using Task = std::packaged_task<std::invoke_result_t<F, Worker&>(Worker&)>;
        auto task = std::make_shared<Task>(std::forward<F>(operation));
        auto future = task->get_future();
        enqueue([task](Worker& worker) { (*task)(worker); }, pinned, route, bytes);
        return future;
    }
    /// 单次运行的性能选择，不跨机器/驱动/配置持久化。
    /// Per-run performance selection; never persisted across hardware/driver/config changes.
    detail::DispatchEvidence dispatch_;
    /// 仅在现有队列锁内更新的固定空间模型。 / Fixed-space model under the existing queue lock.
    detail::OnlineModel model_;
    /// 构造时冻结，禁用后不预测或学习。 / Frozen at construction; disabled means no
    /// prediction/learning.
    bool pgo_{};
    /// 未知区间最多两次初始探索，要求 CPU 饱和或已有 GPU 形状偏好。
    /// At most two initial unknown-band explorations, requiring saturation or a GPU shape
    /// preference.
    std::array<unsigned char, 32> exploration_{};
    /// 每区间每64个合格任务一次再探索；总次数可观测。
    /// One re-exploration per 64 eligible tasks per band; observable total.
    std::array<std::uint64_t, 32> arrivals_{};
    std::uint64_t exploration_jobs_{};
    /// 任务元数据与原有函数一起入队，无独立分配。 / Metadata inline with the existing callable.
    struct QueuedTask {
        std::function<void(Worker&)> operation;
        std::uint64_t bytes{};
        detail::HashRoute route{detail::HashRoute::cpu_only};
        /// 0=无，1=CPU，2=GPU；仅目标空闲时探索。 / 0=none, 1=CPU, 2=GPU; idle targets only.
        unsigned char explore{};
    };
    /// 锁内比较含等待时间的完成成本；未知模型保持静态偏好。
    /// Compare completion costs including waiting under lock; unknown models use static preference.
    bool prefer_gpu(const QueuedTask& task) const;
    /// 返回可领取队列，空表示等待真实在途任务而非预测计时器。
    /// Return runnable queue; null waits for real in-flight work, never a prediction timer.
    std::deque<QueuedTask>* runnable_queue(bool gpu);

    /// 自动探测状态；服务可用状态由队列锁保护。 / Auto probe state; service availability is locked.
    bool auto_mode_{}, auto_attempted_{}, preferred_enabled_{};
    /// 创建后端，不持有队列锁。 / Backend factory, invoked without the queue lock.
    detail::CudaFactory cuda_factory_;
    /// 等待共享容量后按类别排队。 / Wait for shared capacity and enqueue by class.
    void enqueue(std::function<void(Worker&)> task, bool pinned, detail::HashRoute route,
                 std::uint64_t bytes);
    /// 队列锁下决定当前线程能否领取。 / Decide eligibility under the queue lock.
    bool can_run(bool gpu);
    /// 锁内领取并更新活动/路由计数；必须持有 mutex_ 且 can_run(gpu) 为真。
    /// Dequeue and update activity/routing; requires mutex_ held and can_run(gpu) true.
    QueuedTask take_task(Worker& worker, bool gpu, std::deque<QueuedTask>& source);
    /// 执行任务并发布活动状态，服务错误后退出。 / Execute and publish activity; failed service
    /// exits.
    void run(Worker& worker, bool gpu = false);
    /// 关闭并排空所有已接收任务。 / Close and drain all admitted work.
    void close();
    /// 跨线程重试计数。 / Cross-thread retry counter.
    std::atomic<std::size_t> fallbacks_{0};
    /// 曾启用的 GPU 通道数。 / Number of GPU lanes ever enabled.
    std::atomic<std::size_t> gpu_workers_{0};
    /// 所有队列共享容量。 / Shared capacity across queues.
    std::size_t capacity_;
    /// 构造时冻结的主机/设备预算及 GPU 下界；惰性初始化不得绕过这些契约。
    /// Constructor-frozen host/device budgets and GPU floor; lazy setup cannot bypass these
    /// contracts.
    const std::size_t memory_bytes_, device_memory_bytes_, gpu_min_bytes_;
    /// 构造后不再扩容的 CPU/显式后端上下文。 / Fixed CPU/explicit-backend contexts.
    std::vector<std::unique_ptr<Worker>> workers_;
    /// 独立自动 GPU 缓冲，不替换 CPU 工作者。 / Independent auto GPU buffers, never replacing CPU.
    std::unique_ptr<Worker> gpu_worker_;
    /// 固定线程及至多一个延迟启动的 GPU 线程。 / Fixed threads plus at most one lazy GPU thread.
    std::vector<std::thread> threads_;
    /// 普通、固定 GPU、GPU 优先及 CPU 优先队列，均由 mutex_ 保护。
    /// Ordinary, pinned GPU, GPU-preferred and CPU-preferred queues, protected by mutex_.
    std::deque<QueuedTask> queue_, gpu_queue_, hash_queue_, cpu_hash_queue_;
    /// 锁内活动数量；future 就绪不等于执行器已归还。 / Locked activity; ready futures may precede
    /// return.
    std::size_t cpu_active_{};
    /// 专用 GPU 是否执行任务。 / Whether the dedicated GPU is executing.
    bool gpu_active_{};
    /// 锁内累计实际出队路由。 / Actual dequeue routing totals, updated under lock.
    std::uint64_t gpu_overflow_jobs_{}, cpu_spill_jobs_{};
    /// 保护队列、服务存活及活动状态。 / Protect queues, service liveness and activity.
    std::mutex mutex_;
    /// 任务和活动变化，以及队列空间/空闲屏障。 / Work/activity changes and space/idle barriers.
    std::condition_variable ready_, space_;
    /// 单向关闭，仍排空队列。 / One-way closure that still drains queues.
    bool closed_{false};
};
} // namespace same
