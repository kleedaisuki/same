#include "same/resources.hpp"
#include <algorithm>
#include <chrono>
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
    auto_mode_ = config.backend == "auto";
    if (auto_mode_)
        dispatch_.decision = detail::DispatchEvidence::Decision::deferred;
    for (std::size_t i = 0; i < config.workers; ++i) {
        auto worker = std::make_unique<Worker>();
        worker->cpu_compute = make_cpu_compute();
        worker->first.resize(config.block_bytes);
        worker->second.resize(config.block_bytes);
        worker->fallbacks = &fallbacks_;
        worker->gpu_floor =
            auto_mode_ ? std::max(config.gpu_min_bytes, config.block_bytes) : config.gpu_min_bytes;
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
void Resources::enqueue(std::function<void(Worker&)> task, bool pinned, detail::HashRoute route) {
    std::unique_lock lock(mutex_);
    space_.wait(lock, [this] {
        return closed_ ||
               queue_.size() + gpu_queue_.size() + hash_queue_.size() + cpu_hash_queue_.size() <
                   capacity_;
    });
    if (closed_)
        throw std::runtime_error("resource manager is closed");
    auto* queue = &queue_;
    // 保留探测期间的哈希类别，服务发布后可直接接手排队载荷，无需迁移任务。
    // Preserve hash classes during probing so the published service can consume pending work.
    if (pinned && preferred_enabled_)
        queue = &gpu_queue_;
    else if (route == detail::HashRoute::gpu_preferred)
        queue = &hash_queue_;
    else if (route == detail::HashRoute::cpu_preferred)
        queue = &cpu_hash_queue_;
    queue->push_back(std::move(task));
    ready_.notify_all();
}
/// 只读取锁内快照，空闲 GPU 保留唯一优先任务。 / Read only a locked snapshot; reserve the sole
/// preferred job for an idle GPU.
bool Resources::can_run(bool gpu) const {
    if (gpu)
        return preferred_enabled_ && (!gpu_queue_.empty() || !hash_queue_.empty() ||
                                      (!cpu_hash_queue_.empty() && cpu_active_ == workers_.size()));
    return !queue_.empty() || !cpu_hash_queue_.empty() ||
           (!gpu_queue_.empty() && !preferred_enabled_) ||
           (!hash_queue_.empty() && (!preferred_enabled_ || gpu_active_ || hash_queue_.size() > 1));
}
/// 必须持有 mutex_ 且 can_run(gpu) 为真；领取与活动计数发布不可分割。
/// Requires mutex_ held and can_run(gpu); dequeue and activity publication are indivisible.
std::function<void(Worker&)> Resources::take_task(bool gpu) {
    auto* source = &queue_;
    bool take_back = false;
    if (gpu) {
        source = !gpu_queue_.empty()    ? &gpu_queue_
                 : !hash_queue_.empty() ? &hash_queue_
                                        : &cpu_hash_queue_;
        gpu_active_ = true;
        gpu_overflow_jobs_ += source == &cpu_hash_queue_;
    } else {
        if (queue_.empty())
            source = !cpu_hash_queue_.empty()                     ? &cpu_hash_queue_
                     : !preferred_enabled_ && !gpu_queue_.empty() ? &gpu_queue_
                                                                  : &hash_queue_;
        take_back = source == &hash_queue_ && preferred_enabled_ && !gpu_active_;
        cpu_spill_jobs_ += source == &hash_queue_;
        ++cpu_active_;
    }
    auto task = std::move(take_back ? source->back() : source->front());
    if (take_back)
        source->pop_back();
    else
        source->pop_front();
    return task;
}
/// 锁外执行用户任务；任务完成后发布活动状态及设备退役，再唤醒等待者。 / Execute user work
/// outside the lock; publish completion/device retirement before waking waiters.
void Resources::run(Worker& worker, bool gpu) {
    for (;;) {
        std::function<void(Worker&)> task;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, [this, gpu] {
                return can_run(gpu) || (gpu && !preferred_enabled_) ||
                       (closed_ && (gpu || (queue_.empty() && gpu_queue_.empty() &&
                                            hash_queue_.empty() && cpu_hash_queue_.empty())));
            });
            if (!can_run(gpu))
                return;
            task = take_task(gpu);
        }
        space_.notify_one();
        ready_.notify_all();
        // execute() 替换后端的地址就是本服务发生降级的证据，不依赖名称或其他线程的计数。
        // execute() replacing this backend identifies service failure without names/shared counts.
        const auto* backend = worker.compute.get();
        task(worker);
        {
            std::lock_guard lock(mutex_);
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
/// 独立缓冲保证探测不干扰在途 CPU 工作；完整校准后才发布服务。 / Independent buffers isolate
/// probing from in-flight CPU work; publish the service only after complete calibration.
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
    auto block = std::max<std::size_t>(config.block_bytes, 16 * 1024 * 1024);
    if (service_cost(block) > remaining_memory)
        block = config.block_bytes;
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
    worker->gpu_floor = std::max(config.gpu_min_bytes, config.block_bytes);
    worker->compute = std::move(compute);
    try {
        worker->compute->prepare_input(worker->first);
        const auto remaining =
            std::chrono::milliseconds(static_cast<long long>(std::max(0.0, 2000.0 - elapsed())));
        dispatch_ = detail::calibrate_dispatch(*worker->cpu_compute, *worker->compute,
                                               worker->first, remaining, config.block_bytes);
    } catch (const ComputeError&) {
        dispatch_.decision = detail::DispatchEvidence::Decision::failed;
    }
    dispatch_.setup_ms = elapsed();
    if (dispatch_.decision == detail::DispatchEvidence::Decision::failed ||
        !dispatch_.calibration_complete)
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
