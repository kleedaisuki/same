#include "same/resources.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <limits>
#include <stdexcept>

namespace same {
/// 在启动线程前完成资源创建，避免任务观察到半初始化上下文。 / Finish resource creation before any
/// task can observe a partially initialized context.
Resources::Resources(const Config& config) : Resources(config, try_cuda_compute) {}
/// 工厂注入不改变显式后端或缓冲契约。 / Factory injection preserves explicit backend/buffer
/// contracts.
Resources::Resources(const Config& config, detail::CudaFactory factory)
    : cuda_factory_(std::move(factory)), capacity_(config.queue_capacity),
      memory_bytes_(config.memory_bytes), device_memory_bytes_(config.device_memory_bytes),
      gpu_min_bytes_(config.gpu_min_bytes) {
    config.validate();
    pgo_ = config.pgo;
    auto_mode_ = config.backend == "auto";
    if (auto_mode_)
        dispatch_.decision = detail::DispatchEvidence::Decision::deferred;
    for (std::size_t i = 0; i < config.workers; ++i) {
        auto worker = std::make_unique<Worker>();
        worker->cpu_compute = make_cpu_compute();
        worker->first.resize(config.block_bytes);
        worker->second.resize(config.block_bytes);
        worker->fallbacks = &fallbacks_;
        worker->gpu_floor = config.gpu_min_bytes;
        worker->profile_enabled = pgo_;
        if (config.backend == "cuda")
            worker->compute =
                cuda_factory_(config.block_bytes, config.device_memory_bytes / config.workers);
        if (worker->compute) {
            try {
                worker->compute->prepare_input(worker->first);
            } catch (const ComputeError&) {
                worker->compute.reset();
            }
        }
        if (worker->compute)
            ++gpu_workers_;
        else
            worker->compute = make_cpu_compute();
        workers_.push_back(std::move(worker));
    }
    // 若线程创建中途失败，先停止并 join 已启动线程再传播异常。 / Join already started threads
    // before propagating a startup failure.
    threads_.reserve(config.workers + 1);
    try {
        for (auto& worker : workers_)
            threads_.emplace_back([this, ptr = worker.get()] { run(*ptr); });
    } catch (...) {
        close();
        throw;
    }
}
/// 先停止所有线程，再释放后端及其注册缓冲。 / Stop all threads before backends and registered
/// buffers.
Resources::~Resources() {
    close();
}
/// 关闭入口并唤醒全部等待者，保留队列直到消费者排空。 / Close admission and wake all waiters;
/// retain queued work until consumers drain it.
void Resources::close() {
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
    }
    ready_.notify_all();
    space_.notify_all();
    for (auto& thread : threads_)
        if (thread.joinable())
            thread.join();
}
/// 活动计数覆盖 future 就绪到工作线程返回的窗口。 / Activity counts cover the interval between
/// future readiness and worker return.
void Resources::wait_idle() {
    std::unique_lock lock(mutex_);
    space_.wait(lock, [this] {
        return queue_.empty() && gpu_queue_.empty() && hash_queue_.empty() &&
               cpu_hash_queue_.empty() && !cpu_active_ && !gpu_active_;
    });
}
/// 所有类别共享背压；在同一锁下检查关闭状态并入队。 / All classes share backpressure; check
/// closure and enqueue under the same lock.
void Resources::enqueue(std::function<void(Worker&)> task, bool pinned, detail::HashRoute route,
                        std::uint64_t bytes) {
    std::unique_lock lock(mutex_);
    space_.wait(lock, [this] {
        return closed_ ||
               queue_.size() + gpu_queue_.size() + hash_queue_.size() + cpu_hash_queue_.size() <
                   capacity_;
    });
    if (closed_)
        throw std::runtime_error("resource manager is closed");
    if (bytes && bytes < gpu_min_bytes_)
        route = detail::HashRoute::cpu_only;
    auto* queue = &queue_;
    // 保留探测期间的哈希类别，服务发布后可直接接手排队载荷，无需迁移任务。
    // Preserve hash classes during probing so the published service can consume pending work.
    if (pinned && preferred_enabled_)
        queue = &gpu_queue_;
    else if (route == detail::HashRoute::gpu_preferred)
        queue = &hash_queue_;
    else if (route == detail::HashRoute::cpu_preferred)
        queue = &cpu_hash_queue_;
    unsigned char explore = 0;
    if (pgo_ && bytes && route != detail::HashRoute::cpu_only) {
        const auto arrival = ++arrivals_[(std::bit_width(bytes) - 1) / 2];
        if (arrival % 64 == 0)
            explore = (arrival / 64) % 2 ? 2 : 1;
    }
    queue->push_back({std::move(task), bytes, route, explore});
    ready_.notify_all();
}
/// 单调时钟只用于合格任务的排队预测。 / Monotonic time only for eligible scheduling.
static double scheduler_now_ms() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
/// 预计完成时间加残差余量；CPU 多通道取最早可用者。
/// Predicted completion plus residual margin; CPU uses its earliest available lane.
bool Resources::prefer_gpu(const QueuedTask& task) const {
    if (!pgo_ || !task.bytes)
        return task.route == detail::HashRoute::gpu_preferred || cpu_active_ == workers_.size();
    const auto cpu = model_.predict(false, task.bytes);
    const auto gpu = model_.predict(true, task.bytes);
    if (task.explore == 2 && !gpu_active_)
        return true;
    if (task.explore == 1 && cpu_active_ < workers_.size())
        return false;
    if (!cpu.known || !gpu.known) {
        if (gpu_active_)
            return false;
        const auto band = (std::bit_width(task.bytes) - 1) / 2;
        return exploration_[band] < 2 &&
               (cpu_active_ == workers_.size() || task.route == detail::HashRoute::gpu_preferred);
    }
    const double now = scheduler_now_ms();
    if (gpu_active_ && gpu_worker_->predicted_finish_ms <= now)
        return false;
    double cpu_wait = 0;
    if (cpu_active_ == workers_.size()) {
        cpu_wait = std::numeric_limits<double>::infinity();
        for (const auto& lane : workers_) {
            if (lane->predicted_finish_ms <= now)
                return true;
            cpu_wait = std::min(cpu_wait, lane->predicted_finish_ms - now);
        }
    }
    const double gpu_wait = !gpu_active_ ? 0
                            : gpu_worker_->predicted_finish_ms > 0
                                ? std::max(0.0, gpu_worker_->predicted_finish_ms - now)
                                : gpu.ms;
    // 5% 滞回及观测残差抑制噪声；误差不是统计置信区间。
    // Five-percent hysteresis plus observed residual suppress noise; not a confidence interval.
    return detail::gpu_finishes_first(cpu, gpu, cpu_wait, gpu_wait);
}
/// 普通任务不读取时钟或模型；旧无大小接口保持原有队列契约。
/// Ordinary tasks never read clocks/models; unsized legacy APIs preserve queue contracts.
std::deque<Resources::QueuedTask>* Resources::runnable_queue(bool gpu) {
    if (gpu) {
        if (!preferred_enabled_)
            return nullptr;
        if (!gpu_queue_.empty())
            return &gpu_queue_;
    } else {
        if (!queue_.empty())
            return &queue_;
        if (!preferred_enabled_ && !gpu_queue_.empty())
            return &gpu_queue_;
    }
    for (auto* queue :
         {gpu ? &hash_queue_ : &cpu_hash_queue_, gpu ? &cpu_hash_queue_ : &hash_queue_}) {
        if (queue->empty())
            continue;
        const auto& task = queue->front();
        if (!preferred_enabled_)
            return gpu ? nullptr : queue;
        if (!task.bytes || !pgo_) {
            const bool preferred = queue == &hash_queue_;
            if (gpu ? preferred || cpu_active_ == workers_.size()
                    : !preferred || gpu_active_ || queue->size() > 1)
                return queue;
            continue;
        }
        if (prefer_gpu(task) == gpu)
            return queue;
        // 多任务不能因单个空闲 GPU 的预约而闲置整个 CPU 池。
        // Reserving one task for an idle GPU must not idle the CPU pool behind a backlog.
        if (!gpu && !gpu_active_ && queue->size() > 1)
            return queue;
    }
    return nullptr;
}
/// 所有可执行性决定与领取使用同一队列锁。 / Eligibility/dequeue share the queue lock.
bool Resources::can_run(bool gpu) {
    return runnable_queue(gpu) != nullptr;
}
/// 领取时记录服务预测；普通任务不付出时钟/预测成本。
/// Record prediction on dequeue; ordinary work pays no clock/prediction cost.
Resources::QueuedTask Resources::take_task(Worker& worker, bool gpu,
                                           std::deque<QueuedTask>& selected) {
    auto* source = &selected;
    const bool take_back = !gpu && preferred_enabled_ && !gpu_active_ && source != &queue_ &&
                           source != &gpu_queue_ && source->size() > 1 &&
                           prefer_gpu(source->front());
    auto task = std::move(take_back ? source->back() : source->front());
    if (take_back)
        source->pop_back();
    else
        source->pop_front();
    worker.predicted_finish_ms = 0;
    if (pgo_ && task.bytes && task.route != detail::HashRoute::cpu_only) {
        const auto prediction = model_.predict(gpu, task.bytes);
        if (prediction.known)
            worker.predicted_finish_ms = scheduler_now_ms() + prediction.ms;
    }
    if (gpu) {
        if (pgo_ && task.bytes &&
            (task.explore == 2 || !model_.predict(false, task.bytes).known ||
             !model_.predict(true, task.bytes).known)) {
            ++exploration_jobs_;
            auto& count = exploration_[(std::bit_width(task.bytes) - 1) / 2];
            if (count < 2)
                ++count;
        }
        gpu_active_ = true;
        gpu_overflow_jobs_ += source == &cpu_hash_queue_;
    } else {
        if (pgo_ && task.explore == 1)
            ++exploration_jobs_;
        ++cpu_active_;
        cpu_spill_jobs_ += source == &hash_queue_;
    }
    return task;
}
/// 锁外执行用户任务；任务完成后发布活动状态及设备退役，再唤醒等待者。 / Execute user work
/// outside the lock; publish completion/device retirement before waking waiters.
void Resources::run(Worker& worker, bool gpu) {
    for (;;) {
        QueuedTask task;
        {
            std::unique_lock lock(mutex_);
            const auto ready = [this, gpu] {
                return can_run(gpu) || (gpu && !preferred_enabled_) ||
                       (closed_ && queue_.empty() && gpu_queue_.empty() && hash_queue_.empty() &&
                        cpu_hash_queue_.empty());
            };
            // 到期仍忙的 GPU 不得让 CPU 无限等待；仅合格队列使用定时唤醒。
            // An overdue busy GPU cannot stall CPU forever; timed wake only for eligible queues.
            if (pgo_ && !gpu && gpu_active_ && (!hash_queue_.empty() || !cpu_hash_queue_.empty())) {
                using Milliseconds = std::chrono::duration<double, std::milli>;
                const std::chrono::time_point<std::chrono::steady_clock, Milliseconds> deadline(
                    Milliseconds(gpu_worker_->predicted_finish_ms));
                ready_.wait_until(lock, deadline, ready);
            } else if (pgo_ && gpu && cpu_active_ == workers_.size() &&
                       (!hash_queue_.empty() || !cpu_hash_queue_.empty())) {
                double finish = std::numeric_limits<double>::infinity();
                for (const auto& lane : workers_)
                    finish = std::min(finish, lane->predicted_finish_ms);
                using Milliseconds = std::chrono::duration<double, std::milli>;
                const std::chrono::time_point<std::chrono::steady_clock, Milliseconds> deadline{
                    Milliseconds(finish)};
                if (finish > 0 && finish > scheduler_now_ms())
                    ready_.wait_until(lock, deadline, ready);
                else if (!ready())
                    ready_.wait(lock);
            } else if (!ready()) {
                ready_.wait(lock);
            }
            auto* source = runnable_queue(gpu);
            if (!source) {
                if ((gpu && !preferred_enabled_) ||
                    (closed_ && queue_.empty() && gpu_queue_.empty() && hash_queue_.empty() &&
                     cpu_hash_queue_.empty()))
                    return;
                continue;
            }
            task = take_task(worker, gpu, *source);
        }
        space_.notify_one();
        ready_.notify_all();
        // execute() 替换后端的地址就是本服务发生降级的证据，不依赖名称或其他线程的计数。
        // execute() replacing this backend identifies service failure without names/shared counts.
        const auto* backend = worker.compute.get();
        if (pgo_)
            worker.sample = {};
        task.operation(worker);
        {
            std::lock_guard lock(mutex_);
            if (pgo_ && worker.sample.valid && worker.compute.get() == backend)
                model_.observe(worker.sample.gpu, worker.sample.bytes, worker.sample.service_ms);
            worker.predicted_finish_ms = 0;
            if (gpu) {
                gpu_active_ = false;
                if (worker.compute.get() != backend)
                    preferred_enabled_ = false;
            } else {
                --cpu_active_;
            }
        }
        space_.notify_all();
        ready_.notify_all();
    }
}
/// 独立缓冲隔离在途 CPU 工作；仅验证设备，性能模型由真实任务学习。
/// Independent buffers isolate CPU work; validate only, learning performance from real tasks.
void Resources::prepare_auto(const Config& config, std::uint64_t pending_bytes,
                             std::size_t pending_files) {
    if (!auto_mode_ || auto_attempted_ || !pending_files || !pending_bytes)
        return;
    config.validate();
    // 校验原始资源契约，不能通过探测配置增大预算或悄悄改变资格下界。
    // Enforce the original resource contract: probing cannot enlarge budgets or change eligibility.
    if (config.workers != workers_.size() || config.block_bytes != workers_.front()->first.size() ||
        config.memory_bytes != memory_bytes_ ||
        config.device_memory_bytes != device_memory_bytes_ ||
        config.gpu_min_bytes != gpu_min_bytes_)
        throw std::invalid_argument("prepare_auto configuration differs from resources");
    auto_attempted_ = true;
    dispatch_.decision = detail::DispatchEvidence::Decision::cpu;
    const auto start = std::chrono::steady_clock::now();
    const auto elapsed = [&] {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count();
    };
    // 保持旧的最小内存配置有效；额外服务绝不挤掉或超配 CPU 缓冲。
    // Keep legacy minimum budgets valid; the service never displaces or overbooks CPU buffers.
    const auto per_worker = 2 * config.block_bytes + config.block_bytes / 32 + 4096;
    const auto remaining_memory = config.memory_bytes - per_worker * workers_.size();
    const auto service_cost = [](std::size_t block) { return 2 * block + block / 32 + 4096; };
    auto block = std::size_t{16 * 1024 * 1024};
    if (service_cost(block) > remaining_memory)
        block = std::min(config.block_bytes, block);
    if (service_cost(block) > remaining_memory)
        return;
    // 无设备时不要分配额外主机缓冲；预算检查仍在探测之前。
    // Avoid extra host buffers when no device exists; budget checks still precede probing.
    auto compute = cuda_factory_(block, config.device_memory_bytes);
    if (!compute) {
        dispatch_.setup_ms = elapsed();
        return;
    }
    auto worker = std::make_unique<Worker>();
    worker->first.resize(block);
    worker->second.resize(block);
    worker->cpu_compute = make_cpu_compute();
    worker->fallbacks = &fallbacks_;
    // GPU 输入容量不是资格下界；中等载荷在 CPU 饱和时必须真正使用 GPU。
    // GPU input capacity is not eligibility: medium CPU-overflow jobs must actually use the GPU.
    worker->gpu_floor = config.gpu_min_bytes;
    worker->profile_enabled = pgo_;
    worker->compute = std::move(compute);
    try {
        worker->compute->prepare_input(worker->first);
        // 启动只验证设备，不运行合成性能校准；在线学习使用真实成功文件。
        // Startup validates the device only; online learning uses successful real files, not
        // synthetic timing.
        const auto input = std::span<const std::byte>(worker->first)
                               .first(std::min<std::size_t>(worker->first.size(), 64 * 1024));
        auto cpu = worker->cpu_compute->hasher();
        auto gpu = worker->compute->hasher();
        cpu->update(input);
        gpu->update(input);
        if (cpu->finish() != gpu->finish())
            throw std::runtime_error("GPU correctness validation mismatch");
        dispatch_.device_validated = true;
    } catch (const ComputeError&) {
        dispatch_.decision = detail::DispatchEvidence::Decision::failed;
        dispatch_.stop_reason = detail::DispatchEvidence::StopReason::device_error;
    }
    dispatch_.setup_ms = elapsed();
    if (dispatch_.decision == detail::DispatchEvidence::Decision::failed ||
        !dispatch_.device_validated)
        return;
    dispatch_.decision = detail::DispatchEvidence::Decision::adaptive;
    // 发布与线程创建同锁；创建失败还原，不留下无法消费的专属队列。
    // Publish under the startup lock; rollback on failure leaves no unserviceable pinned queue.
    {
        std::lock_guard lock(mutex_);
        if (closed_)
            throw std::runtime_error("resource manager is closed");
        gpu_worker_ = std::move(worker);
        preferred_enabled_ = true;
        try {
            threads_.emplace_back([this] { run(*gpu_worker_, true); });
        } catch (...) {
            preferred_enabled_ = false;
            gpu_worker_.reset();
            dispatch_.decision = detail::DispatchEvidence::Decision::cpu;
            throw;
        }
        gpu_workers_ = 1;
    }
    ready_.notify_all();
}
} // namespace same
