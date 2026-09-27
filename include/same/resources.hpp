#pragma once
#include "same/compute.hpp"
#include "same/config.hpp"
#include "same/detail/online_model.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace same {
namespace detail {
/// 可注入设备创建器；返回空表示不可用。 / Injectable device factory; null means unavailable.
/// 冷启动历史估计；零表示未知。 / Historical cold-start estimate; zero means unknown.
using SetupPriorLoader = std::function<double(BackendKind, const DeviceProfile&)>;
/// 轻量设备探测，不得创建上下文。 / Lightweight probe must not create a context.
using IgpuProbe = std::function<std::optional<DeviceProfile>(std::size_t, std::size_t)>;
using ModelPriorLoader = std::function<OnlineModel::State(BackendKind, const DeviceProfile&)>;
/// 可注入设备创建器。 / Injectable device factory.
using CudaFactory = std::function<std::unique_ptr<Compute>(std::size_t, std::size_t)>;
/// 路由参数的唯一来源，运行记录直接引用。 / Single source for routing policy and run snapshots.
struct RoutingParameters {
    /// 保守完成时间比较的 CPU 系数。 / CPU coefficient in conservative finish comparison.
    static constexpr double cpu_advantage_factor = 0.95;
    /// 每区间的探索机会间隔，CPU/GPU 交替。 / Per-band opportunities alternate CPU/GPU.
    static constexpr std::uint64_t exploration_period = 64;
    /// 未知区间初始 GPU 探索上限。 / Initial GPU explorations per unknown band.
    static constexpr unsigned initial_gpu_explorations = 2;
    /// 小任务采样间隔。 / Small-task sampling period.
    static constexpr std::uint64_t small_sample_period = 64;
    /// 未知模型的初始 GPU 偏好和独立更新块。 / Initial preference floor and separate GPU block.
    static constexpr std::uint64_t static_gpu_floor_bytes = 64ULL * 1024 * 1024;
    static constexpr std::size_t gpu_block_bytes = 16 * 1024 * 1024;
};
/// 比较本地服务成本与残差；工作已领取，不假设独立 GPU 队列。
/// Compare local service cost and residual; work is already dequeued, without an independent GPU
/// queue.
inline bool gpu_service_wins(OnlineModel::Prediction cpu, OnlineModel::Prediction gpu) noexcept {
    return gpu.ms + gpu.error_ms < RoutingParameters::cpu_advantage_factor * cpu.ms - cpu.error_ms;
}
} // namespace detail
/// 固定线程独占后端、缓冲和模型；中央管理器不读写运行中的学习状态。
/// A fixed thread exclusively owns its backends, buffers and model; the manager never learns for
/// it.
struct Worker {
    /// 稳定工作线程编号。 / Stable worker identity.
    std::size_t index{};
    /// 当前任务输入；选择 GPU 时与私有 GPU 缓冲交换所有权。
    /// Active task inputs; ownership swaps with private GPU buffers on GPU selection.
    std::vector<std::byte> first, second, gpu_first, gpu_second;
    /// 当前后端、CPU 策略后端、停放的 GPU/CPU 后端；先析构后端再释放缓冲。
    /// Selected, CPU-policy and parked backends; destruction precedes backing buffers.
    std::unique_ptr<Compute> compute, cpu_compute, gpu_compute, igpu_compute;
    /// 仅所属线程预测和观测，不加锁。 / Owner-thread-only prediction and observation, without
    /// locks.
    detail::OnlineModel model;
    /// 冻结上下文保证预测与训练使用同一特征。 / Freeze features shared by prediction and training.
    detail::OnlineModel::Context decision_context{};
    /// 完成时核实实际重叠后写入训练特征；决策预测仍使用原始上下文。
    /// Training context includes observed overlap; decision prediction keeps its original context.
    detail::OnlineModel::Context learning_context{};
    detail::OnlineModel::Prediction decision_prediction{};
    std::uint64_t contention_epoch{};
    std::size_t contention_at_start{};
    BackendKind decision_backend{BackendKind::cpu};
    std::array<DeviceProfile, 3> devices{};
    std::array<bool, 3> prior_loaded{};
    /// 每后端固定/模型/探索选择次数，以及候选不可用次数。
    /// Per-backend fixed/model/exploration decisions and unavailable exclusions.
    std::array<std::array<std::uint64_t, 3>, 3> decisions{};
    std::array<std::uint64_t, 3> excluded{};
    bool decision_active{}, gpu_counted{};
    std::array<detail::OnlineModel::Context, 3> candidate_contexts{};
    /// 成功任务的已有计时；所属线程在回调后消费。 / Existing successful-task timing consumed after
    /// callback.
    struct Sample {
        /// 输入和完整服务时间（毫秒）。 / Input bytes and complete service time in milliseconds.
        std::uint64_t bytes{};
        double service_ms{};
        /// 兼容 gpu 标记仅代表 CUDA；backend 才是三设备身份。
        /// Legacy gpu flag means CUDA only; backend identifies all three devices.
        bool gpu{}, valid{};
        /// 三设备归属。 / Three-device attribution.
        BackendKind backend{BackendKind::cpu};
    } sample;
    /// 本地采样开关和小任务序号。 / Local sampling switch and small-task sequence.
    /// 核显后端归所属线程，与 CPU 共用该线程的输入缓冲。
    /// The iGPU backend is owner-local and reuses its owner's CPU input buffers.
    bool igpu_selected{}, igpu_attempted{}, igpu_enabled{}, igpu_retired{};
    bool profile_enabled{};
    std::uint64_t profile_sequence{};
    /// 冻结的文件资格、实际缓冲和显存配额。 / Frozen file floor, buffer sizes and device quota.
    std::size_t gpu_floor{}, cpu_block_bytes{}, gpu_block_bytes{}, device_budget_bytes{};
    std::size_t igpu_block_bytes{}, igpu_budget_bytes{};
    /// 仅所属线程更新的惰性生命周期状态。 / Owner-thread-only lazy lifecycle state.
    bool gpu_attempted{}, gpu_enabled{}, gpu_selected{}, gpu_retired{}, gpu_reuses_cpu_buffers{};
    /// 初始化时间累计，非性能校准。 / Accumulated initialization time, not performance calibration.
    double setup_ms{}, igpu_setup_ms{};
    /// 错误在回调内部重新抛出，保证完成队列能发布失败。
    /// Rethrow errors inside callbacks so completion queues can publish failures.
    std::exception_ptr startup_error;
    /// 当前选择的非拥有指针，用于拒绝后端替换后的观测。 / Selected non-owning pointer rejects
    /// replaced-backend samples.
    const Compute* selected_backend{};
    /// 原有跨线程重试总计，生命周期由 Resources 保证。 / Existing shared retry total, owned by
    /// Resources.
    std::atomic<std::size_t>* fallbacks{};
    /// 本地实际尝试、读取、降级和初始化失败。 / Local attempts, read bytes, fallbacks and setup
    /// failures.
    std::uint64_t igpu_hashes{}, igpu_hash_bytes{}, igpu_busy{}, igpu_selections{};
    std::uint64_t igpu_contended_samples{};
    std::uint64_t cpu_hashes{}, gpu_hashes{}, cpu_routed_hashes{}, hash_bytes{}, compare_bytes{};
    std::uint64_t cpu_hash_bytes{}, gpu_hash_bytes{}, fallback_count{}, gpu_init_failures{};
    /// 本地选择原因，探索单列。 / Local selection reasons with separate exploration count.
    std::uint64_t size_cpu{}, static_cpu{}, static_gpu{}, model_cpu{}, model_gpu{},
        unavailable_cpu{};
    std::uint64_t exploration_jobs{};
    /// 首次 GPU 初始化期间继续 CPU 的任务数；不是设备失败。
    /// Jobs continuing on CPU during first GPU initialization; not device failures.
    std::uint64_t cold_start_cpu{};
    /// 本地有界探索；不同工作线程不消费彼此的机会。 / Local bounded exploration; workers never
    /// consume peers' opportunities.
    std::array<std::uint64_t, 32> arrivals{};
    std::array<unsigned char, 32> initial_cpu{}, initial_gpu{}, initial_igpu{};
    /// 兼容 CUDA 争用统计，仅作观测，不是并发闸门。
    /// Legacy CUDA contention observations, never a concurrency gate.
    std::size_t gpu_inflight_at_selection{}, gpu_peak_concurrency{};
    std::uint64_t contended_samples{};

    /// 回调应通过 execute 接收初始化失败。 / Callbacks receive startup failure through execute.
    void rethrow_startup() const {
        if (startup_error)
            std::rethrow_exception(startup_error);
    }
    /** 后端错误后完整 CPU 重试一次；初始化的非设备错误不被掩盖。
     * Retry the complete operation once on CPU after backend error; do not hide non-device setup
     * errors. operation 必须重新打开输入、重建摘要。 / operation must reopen inputs and recreate
     * its digest.
     */
    template <class F> auto execute(F&& operation) -> std::invoke_result_t<F> {
        rethrow_startup();
        try {
            return operation();
        } catch (const ComputeError&) {
            compute = make_cpu_compute();
            if (igpu_selected)
                igpu_retired = true;
            else {
                gpu_retired = true;
                gpu_enabled = false;
            }
            ++fallback_count;
            if (fallbacks)
                ++*fallbacks;
            return operation();
        }
    }
};

/// 空闲后复制的逐工作线程剖析；模型参数不用于其他工作线程的路由。
/// Per-worker idle-time export; model parameters never route another worker's tasks.
struct WorkerProfile {
    /// 工作线程编号、本地观测和本地参数。 / Worker identity, local observations and local
    /// parameters.
    std::size_t index{};
    detail::OnlineModel::Snapshot snapshot;
    detail::OnlineModel::Parameters parameters;
    /// 本轮增量不包含共享先验。 / Run delta excludes the shared prior.
    detail::OnlineModel::State prior{}, delta{};
    std::array<DeviceProfile, 3> devices{};
    detail::OnlineModel::Context decision_context{};
    detail::OnlineModel::Context learning_context{};
    detail::OnlineModel::Prediction decision_prediction{};
    BackendKind decision_backend{BackendKind::cpu};
    std::array<std::array<std::uint64_t, 3>, 3> decisions{};
    std::array<std::uint64_t, 3> excluded{};
    /// 固定资源配额与惰性初始化状态。 / Fixed resource quotas and lazy initialization state.
    std::size_t cpu_block_bytes{}, gpu_block_bytes{}, device_budget_bytes{};
    std::size_t igpu_block_bytes{}, igpu_budget_bytes{};
    double setup_ms{}, igpu_setup_ms{};
    bool gpu_attempted{}, gpu_enabled{}, igpu_attempted{}, igpu_enabled{};
    /// 实际工作及错误计数。 / Actual work and failure counters.
    std::uint64_t igpu_hashes{}, igpu_hash_bytes{}, igpu_busy{}, igpu_selections{};
    std::uint64_t igpu_contended_samples{};
    std::uint64_t cpu_hashes{}, gpu_hashes{}, cpu_routed_hashes{}, hash_bytes{}, compare_bytes{};
    std::uint64_t cpu_hash_bytes{}, gpu_hash_bytes{}, fallbacks{}, gpu_init_failures{};
    /// 后端选择原因。 / Backend selection reasons.
    std::uint64_t size_cpu{}, static_cpu{}, static_gpu{}, model_cpu{}, model_gpu{},
        unavailable_cpu{};
    std::uint64_t exploration_jobs{};
    /// 首次 GPU 初始化期间继续 CPU 的任务数；不是设备失败。
    /// Jobs continuing on CPU during first GPU initialization; not device failures.
    std::uint64_t cold_start_cpu{};
    /// 兼容 CUDA 争用统计。 / Legacy CUDA contention observations.
    std::size_t gpu_inflight_at_selection{}, gpu_peak_concurrency{};
    std::uint64_t contended_samples{};
};

/** 固定数量同质工作线程、单个有界 FIFO；每个线程自行选择后端。
 * Fixed homogeneous workers and one bounded FIFO; each worker selects its own backend.
 * 模型和设备初始化不持有队列锁。 / Learning and device setup never hold the queue lock.
 * 任务不得等待向同一满队列提交；销毁与外部调用不得并发。
 * Tasks must not wait on submission to this full queue; destruction must not race external calls.
 * @code
 * Resources pool(config);
 * auto result = pool.submit_hash([](Worker& w) { return w.compute->name(); }, 64ULL << 20);
 * auto backend = result.get();
 * pool.wait_idle();
 * @endcode
 */
class Resources {
public:
    /// 冻结预算并启动固定线程；GPU 在所属线程首次需要时初始化。
    /// Freeze budgets and start fixed workers; initialize GPU lazily on its owning thread.
    explicit Resources(const Config& config);
    /// 注入工厂可能被多个所属线程并发调用，必须线程安全。
    /// Injected factories may be invoked concurrently by owners and must be thread-safe.
    Resources(const Config& config, detail::CudaFactory factory);
    /// 独立注入两设备工厂。 / Independently inject both device factories.
    Resources(const Config& config, detail::CudaFactory cuda, detail::CudaFactory igpu,
              detail::ModelPriorLoader prior = {}, detail::SetupPriorLoader setup = {},
              detail::IgpuProbe probe = {});
    /// 只读先验查询在初始化时执行，必须线程安全且不得访问 SQL。
    /// Read-only prior lookup runs at initialization; thread-safe and SQL-free.
    Resources(const Config& config, detail::ModelPriorLoader prior,
              detail::SetupPriorLoader setup = {});
    /// 排空已接收任务并在所属线程销毁设备。 / Drain admitted tasks and destroy devices on owner
    /// threads.
    ~Resources();
    /// 禁止复制同步状态。 / Synchronization state cannot be copied.
    Resources(const Resources&) = delete;
    Resources& operator=(const Resources&) = delete;
    /// 等待所有回调、局部观测和任务清理结束。 / Wait for callbacks, local observations and cleanup.
    void wait_idle();
    /// 通用任务使用 CPU；显式 CUDA 模式保持强制后端语义。
    /// Generic work uses CPU; explicit CUDA retains forced-backend semantics.
    template <class F> auto submit(F&& operation) -> std::future<std::invoke_result_t<F, Worker&>> {
        return submit_impl(std::forward<F>(operation), 0, false);
    }
    /// 仅提交文件大小，不在生产者或队列锁内选择后端。
    /// Submit only file size; neither producer nor queue lock selects the backend.
    template <class F>
    auto submit_hash(F&& operation, std::uint64_t bytes)
        -> std::future<std::invoke_result_t<F, Worker&>> {
        return submit_impl(std::forward<F>(operation), bytes, true, std::nullopt);
    }
    /// 显式训练使用自动模式的真实资源预算，但必须由请求后端完成，不隐式接受 CPU 回退。
    /// Train with automatic-mode budgets while requiring the requested backend; callers must
    /// reject any unavailable-backend fallback before assigning a training sample.
    template <class F>
    auto submit_training_hash(F&& operation, std::uint64_t bytes, BackendKind backend)
        -> std::future<std::invoke_result_t<F, Worker&>> {
        return submit_impl(std::forward<F>(operation), bytes, true, backend);
    }
    /// 曾成功启用的上下文数，支持并发读取。 / Contexts ever enabled; concurrent reads are safe.
    std::size_t gpu_workers() const {
        return gpu_workers_.load(std::memory_order_relaxed);
    }
    /// 固定同质线程数。 / Fixed homogeneous worker count.
    std::size_t worker_count() const {
        return workers_.size();
    }
    /// 自动 GPU 更新块预算；零表示预算不允许。 / Budgeted auto GPU block; zero means unavailable.
    std::size_t gpu_block_bytes() const {
        return gpu_block_bytes_;
    }
    /// 跨线程完整 CPU 重试次数。 / Cross-thread complete CPU retries.
    std::size_t fallbacks() const {
        return fallbacks_.load(std::memory_order_relaxed);
    }
    /// 固定运行开关。 / Frozen per-run profiling switch.
    bool profiling_enabled() const {
        return pgo_;
    }
    /// 以下统计只能在 wait_idle 后读取。 / The following statistics require wait_idle.
    bool gpu_service_enabled() const;
    /// 空闲后读取独立核显状态。 / Read independent iGPU state after idle.
    bool igpu_service_enabled() const;
    /// 空闲后合计实际核显尝试。 / Sum actual iGPU attempts after idle.
    std::uint64_t igpu_hash_attempts() const;
    /// 核显生命周期诊断，空闲后读取。 / iGPU lifecycle diagnostics, read after idle.
    bool igpu_attempted() const;
    bool igpu_retired() const;
    /// 所有工作线程初始化耗时之和，不是墙钟时间。 / Sum worker setup time, not wall time.
    double igpu_setup_ms() const;
    /// 以下冷启动统计在 idle 后读取，时间单位毫秒。 / Cold-start idle-time statistics in
    /// milliseconds.
    const DeviceProfile& igpu_profile() const {
        return igpu_profile_;
    }
    double igpu_discovery_ms() const {
        return igpu_discovery_ms_;
    }
    double igpu_setup_estimate_ms() const {
        return igpu_setup_estimate_ms_;
    }
    std::uint64_t igpu_deferred() const {
        return igpu_deferred_.load(std::memory_order_relaxed);
    }
    double cold_credit_ms() const {
        return cold_credit_ms_.load(std::memory_order_relaxed);
    }
    double cold_spent_ms() const {
        return cold_spent_ms_.load(std::memory_order_relaxed);
    }
    /// 自动 CUDA 冷启动延后次数。 / Deferred automatic CUDA cold starts.
    std::uint64_t cuda_deferred() const {
        return cuda_deferred_.load(std::memory_order_relaxed);
    }
    std::uint64_t exploration_jobs() const;
    std::pair<std::uint64_t, std::uint64_t> read_bytes() const;
    std::pair<std::uint64_t, std::uint64_t> hash_attempts() const;
    std::uint64_t cpu_routed_hashes() const;
    /// 只合并可加计数和直方图；全局误差 EWMA 保持零，不混合局部模型。
    /// Merge additive counters/histograms only; global error EWMA stays zero, never merging local
    /// models.
    detail::OnlineModel::Snapshot profile_snapshot() const;
    /// 导出每个独立模型及其上下文。 / Export each independent model with context.
    std::vector<WorkerProfile> worker_profiles() const;
    /// 兼容 CUDA 文件任务峰值，计数器不是设备闸门。
    /// Legacy CUDA file-task peak; the counter is not a device gate.
    std::size_t gpu_peak_concurrency() const {
        return gpu_peak_.load(std::memory_order_relaxed);
    }
    /// 已准入核显文件任务的峰值，不保证内核真正重叠。
    /// Peak admitted iGPU file tasks, not proof that kernels overlapped physically.
    std::size_t igpu_peak_concurrency() const {
        return igpu_peak_.load(std::memory_order_relaxed);
    }

private:
    /// 操作始终被调用以发布完成；初始化异常在其 execute 内传播。
    /// Always invoke callbacks to publish completion; setup errors propagate within their execute.
    template <class F>
    auto submit_impl(F&& operation, std::uint64_t bytes, bool hash,
                     std::optional<BackendKind> training_backend = std::nullopt)
        -> std::future<std::invoke_result_t<F, Worker&>> {
        using Result = std::invoke_result_t<F, Worker&>;
        auto invoke = [this, op = std::forward<F>(operation), bytes, hash,
                       training_backend](Worker& worker) mutable -> Result {
            if (training_backend)
                select_backend(worker, bytes, hash, training_backend);
            else
                select_backend(worker, bytes, hash);
            if constexpr (std::is_void_v<Result>) {
                std::invoke(op, worker);
                worker.rethrow_startup();
            } else {
                Result result = std::invoke(op, worker);
                worker.rethrow_startup();
                return result;
            }
        };
        auto task = std::make_shared<std::packaged_task<Result(Worker&)>>(std::move(invoke));
        auto future = task->get_future();
        enqueue([task](Worker& worker) { (*task)(worker); });
        return future;
    }
    /// 本地选择类别，不影响队列。 / Local selection reason, never a queue class.
    enum class Reason { size, fixed, model, explore };
    /// 队列仅负责背压、关闭与 FIFO。 / Queue handles backpressure, closure and FIFO only.
    void enqueue(std::function<void(Worker&)> task);
    void run(Worker& worker);
    void close();
    /// 本地成本与有界探索，不读取时钟或其他模型。 / Local costs and bounded exploration; no clock
    /// or peer model reads.
    std::pair<BackendKind, Reason> choose_backend(Worker& worker, std::uint64_t bytes, bool hash,
                                                  const std::array<bool, 3>& candidates,
                                                  std::optional<BackendKind> training_backend);
    /// 取得配置与实际容量组成的上下文，不查询驱动。 / Context from config/cached capacity, no
    /// driver query.
    detail::OnlineModel::Context context(const Worker& worker, BackendKind kind,
                                         std::uint64_t bytes) const;
    /// 初始化时只加载对应设备的先验。 / Load only this device prior at initialization.
    void load_prior(Worker& worker, BackendKind kind, const Compute& compute, std::size_t block);
    bool activate_cuda(Worker& worker, bool training);
    /// 捕获初始化异常但不跳过用户回调。 / Capture setup exceptions without skipping the callback.
    void select_backend(Worker& worker, std::uint64_t bytes, bool hash) noexcept;
    /// 训练专用重载保留旧三参数符号和 Worker 布局。 / Training overload preserves the
    /// existing three-argument symbol and Worker layout.
    void select_backend(Worker& worker, std::uint64_t bytes, bool hash,
                        std::optional<BackendKind> training_backend) noexcept;
    /// 所属线程首次初始化并验证，失败不重新探测。 / Owner-thread one-time
    /// initialization/validation, without repeated probing.
    bool prepare_gpu(Worker& worker);
    /// 所属线程恢复 CPU 缓冲并退休失败上下文。 / Owner restores CPU buffers and retires failed
    /// contexts.
    void finish_task(Worker& worker);
    /// 配置意图不同于实际执行设备；automatic 不是 BackendKind。
    /// Configured intent differs from the selected device; automatic is not a BackendKind.
    enum class BackendPolicy { cpu, automatic, cuda, igpu };
    /// 仅在 Config::validate 后转换；未知值不能默认为 CPU。
    /// Convert only after Config::validate; unknown values must not default to CPU.
    static BackendPolicy parse_backend(std::string_view backend);
    /// 固定配置及集中预算。 / Frozen configuration and centralized budgets.
    bool pgo_{};
    BackendPolicy policy_{BackendPolicy::cpu};
    /// 所属线程私有队列/缓冲；首次启动才共享非阻塞冷启动门。
    /// Owner-private queue/buffers; only cold initialization shares a nonblocking gate.
    bool select_igpu(Worker& worker, bool training);
    bool discover_igpu(Worker& worker, bool training);
    bool admit_cold_igpu(const Worker& worker, bool training) const;
    bool begin_cold(const Worker& worker, double estimate, bool training);
    void end_cold();
    std::atomic<bool> cold_start_busy_{false};
    std::atomic<std::uint64_t> cuda_deferred_{0};
    double igpu_bootstrap_ms_{}, cuda_bootstrap_ms_{};
    void load_prior(Worker& worker, BackendKind kind, const DeviceProfile& profile,
                    std::size_t block);
    detail::SetupPriorLoader setup_loader_;
    detail::IgpuProbe igpu_probe_;
    DeviceProfile igpu_profile_;
    bool igpu_probed_{}, igpu_present_{true};
    double igpu_discovery_ms_{}, igpu_setup_estimate_ms_{};
    std::atomic<double> cold_spent_ms_{0};
    double cold_exploration_fraction_{}, credit_divisor_{};
    std::atomic<std::uint64_t> igpu_deferred_{0};
    std::atomic<double> cold_credit_ms_{0};
    detail::ModelPriorLoader prior_loader_;
    /// 实际执行中的后端数量，用于上下文，不是线程调度锁。
    /// Active backends inform context; these counters are not scheduling locks.
    std::array<std::atomic<std::size_t>, 3> backend_active_{};
    /// CPU/iGPU 共享资源的精确在途计数和后续重叠序号。
    /// Exact CPU/iGPU in-flight count and later-overlap epoch for training labels.
    std::atomic<std::size_t> host_igpu_active_{0};
    std::atomic<std::uint64_t> host_igpu_overlap_epoch_{0}, cuda_overlap_epoch_{0};
    /// 只保护探测；稳态任务无池级核显互斥。 / Probe only; no pool-wide steady-state gate.
    std::atomic<bool> igpu_probe_busy_{false};
    std::atomic<bool> igpu_ready_{false};
    std::size_t igpu_block_bytes_{}, igpu_budget_{};
    detail::CudaFactory igpu_factory_;
    std::size_t capacity_{}, gpu_block_bytes_{}, gpu_device_budget_{};
    detail::CudaFactory cuda_factory_;
    /// 原子量仅统计，不限制 GPU 并发。 / Atomics are statistics only, never GPU concurrency limits.
    std::atomic<std::size_t> fallbacks_{0}, gpu_workers_{0}, gpu_active_{0}, gpu_peak_{0};
    std::atomic<std::size_t> igpu_active_{0}, igpu_peak_{0};
    /// 仅协调第一次驱动冷启动；ready 后不限制上下文创建或 GPU 并发。
    /// Coordinate only the first cold driver initialization; ready never limits contexts or GPU
    /// concurrency.
    enum class Bootstrap { cold, initializing, ready };
    std::atomic<Bootstrap> bootstrap_{Bootstrap::cold};
    /// 固定上下文和线程，销毁线程后再释放 CPU 存储。 / Fixed contexts/threads; CPU storage outlives
    /// joins.
    std::vector<std::unique_ptr<Worker>> workers_;
    std::vector<std::thread> threads_;
    /// 唯一队列及其活动数量。 / The sole queue and active-task count.
    std::deque<std::function<void(Worker&)>> queue_;
    std::size_t active_{};
    std::mutex mutex_;
    std::condition_variable ready_, space_;
    bool closed_{};
};
} // namespace same
