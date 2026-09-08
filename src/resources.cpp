#include "same/resources.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

namespace same {
/// 在启动线程前完成资源创建，避免任务观察到半初始化上下文。 / Finish resource creation before any
/// task can observe a partially initialized context.
Resources::Resources(const Config& config) : capacity_(config.queue_capacity) {
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
                try_cuda_compute(config.block_bytes, config.device_memory_bytes / config.workers);
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
    try {
        for (auto& worker : workers_)
            threads_.emplace_back([this, ptr = worker.get()] { run(*ptr); });
    } catch (...) {
        close();
        throw;
    }
}
/// 先 join 再销毁字段，使缓冲和计数器覆盖全部任务生命周期。 / Join before member teardown so
/// buffers and counters outlive all tasks.
Resources::~Resources() {
    close();
}
/// 关闭入口并唤醒两类等待者；消费者排空后退出。 / Close admission and wake both waiter classes;
/// consumers exit after draining.
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
/// 谓词在锁下同时检查关闭和容量，避免丢失唤醒及超量入队。 / Check closure and capacity under lock
/// to avoid lost wakeups and over-admission.
void Resources::enqueue(std::function<void(Worker&)> task, bool prefer_gpu, bool gpu_eligible) {
    std::unique_lock lock(mutex_);
    space_.wait(lock, [this] {
        return closed_ || queue_.size() + gpu_queue_.size() + hash_queue_.size() < capacity_;
    });
    if (closed_)
        throw std::runtime_error("resource manager is closed");
    if (prefer_gpu && preferred_enabled_) {
        gpu_queue_.push_back(std::move(task));
        ready_.notify_all();
    } else if (gpu_eligible && preferred_enabled_) {
        hash_queue_.push_back(std::move(task));
        ready_.notify_all();
    } else {
        queue_.push_back(std::move(task));
        ready_.notify_one();
    }
}
/// 只在出队期间持锁；packaged_task 将用户异常交给 future。 / Lock only while dequeuing;
/// packaged_task transfers user exceptions into its future.
void Resources::run(Worker& worker) {
    for (;;) {
        std::function<void(Worker&)> task;
        {
            std::unique_lock lock(mutex_);
            const bool first = &worker == workers_.front().get();
            ready_.wait(lock, [this, first] {
                return closed_ || !queue_.empty() || !hash_queue_.empty() ||
                       (first && !gpu_queue_.empty());
            });
            // 固定校准任务优先；GPU 优先大哈希，CPU 优先普通工作后窃取。
            // Pinned probes first; GPU prefers eligible hashes, CPUs steal after normal work.
            const auto selected = detail::select_work_queue(first, !gpu_queue_.empty(),
                                                            !hash_queue_.empty(), !queue_.empty());
            if (selected == detail::WorkQueue::none)
                return;
            auto& source = selected == detail::WorkQueue::pinned     ? gpu_queue_
                           : selected == detail::WorkQueue::eligible ? hash_queue_
                                                                     : queue_;
            task = std::move(source.front());
            source.pop_front();
            ++active_;
            space_.notify_one();
        }
        task(worker);
        {
            std::lock_guard lock(mutex_);
            --active_;
        }
        space_.notify_all();
    }
}
/// 使用真实线程和缓冲；第零线程先占位，随后统一释放，容量一也不死锁。
/// Use real threads/buffers; reserve lane zero before releasing the gate, even at capacity one.
double Resources::probe_mixed(std::size_t count, std::size_t jobs, bool gpu,
                              const Digest& expected) {
    std::promise<void> release, first_started;
    auto gate = release.get_future().share();
    std::mutex mutex;
    std::condition_variable ready;
    std::size_t arrived = 0;
    // 工作项不超过真实待处理文件数，每项两块。 / Two blocks per job; never invent more
    // schedulable jobs than pending files.
    std::atomic<std::size_t> next_job{};
    std::vector<std::future<void>> results;
    results.reserve(count);
    auto operation = [&](Worker& worker) {
        auto job = next_job.fetch_add(1);
        {
            std::lock_guard lock(mutex);
            ++arrived;
        }
        ready.notify_one();
        gate.wait();
        auto& compute =
            gpu && &worker == workers_.front().get() ? worker.compute : worker.cpu_compute;
        while (job < jobs) {
            auto hash = compute->hasher();
            for (unsigned block = 0; block < 2; ++block)
                hash->update(worker.first);
            if (hash->finish() != expected)
                throw std::runtime_error("mixed calibration digest mismatch");
            job = next_job.fetch_add(1);
        }
    };
    try {
        results.push_back(submit(
            [&](Worker& worker) {
                first_started.set_value();
                operation(worker);
            },
            true));
        first_started.get_future().wait();
        for (std::size_t i = 1; i < count; ++i)
            results.push_back(submit(operation));
    } catch (...) {
        release.set_value();
        for (auto& result : results)
            result.wait();
        throw;
    }
    {
        std::unique_lock lock(mutex);
        ready.wait(lock, [&] { return arrived == count; });
    }
    const auto start = std::chrono::steady_clock::now();
    release.set_value();
    std::exception_ptr failure;
    for (auto& result : results) {
        try {
            result.get();
        } catch (...) {
            if (!failure)
                failure = std::current_exception();
        }
    }
    // future 就绪略早于任务返回，等待内部活动计数归零才允许重新配置。
    // Futures become ready just before return; wait for active jobs before reconfiguration.
    {
        std::unique_lock lock(mutex_);
        space_.wait(lock, [&] { return active_ == 0; });
    }
    if (failure)
        std::rethrow_exception(failure);
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}
void Resources::prepare_auto(const Config& config, std::uint64_t pending_bytes,
                             std::size_t pending_files) {
    if (!auto_mode_ || auto_attempted_ || !pending_files || !pending_bytes)
        return;
    {
        std::unique_lock lock(mutex_);
        if (closed_ || !queue_.empty() || !gpu_queue_.empty() || !hash_queue_.empty())
            throw std::logic_error("prepare_auto requires drained jobs");
        space_.wait(lock, [&] { return active_ == 0; });
    }
    if (config.workers != workers_.size() || config.block_bytes != workers_.front()->first.size())
        throw std::invalid_argument("prepare_auto configuration differs from resources");
    auto_attempted_ = true;
    dispatch_.decision = detail::DispatchEvidence::Decision::cpu;
    const auto start = std::chrono::steady_clock::now();
    const auto elapsed = [&] {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count();
    };
    auto& first = *workers_.front();
    auto gpu = try_cuda_compute(config.block_bytes, config.device_memory_bytes / config.workers);
    if (!gpu) {
        dispatch_.setup_ms = elapsed();
        return;
    }
    try {
        gpu->prepare_input(first.first);
    } catch (const ComputeError&) {
        dispatch_.decision = detail::DispatchEvidence::Decision::failed;
        gpu.reset();
        dispatch_.setup_ms = elapsed();
        return;
    }
    const auto remaining =
        std::chrono::milliseconds(static_cast<long long>(std::max(0.0, 2000.0 - elapsed())));
    dispatch_ = detail::calibrate_dispatch(*first.cpu_compute, *gpu, first.first, remaining);
    if (dispatch_.decision != detail::DispatchEvidence::Decision::gpu) {
        gpu.reset();
        dispatch_.setup_ms = elapsed();
        return;
    }
    first.compute = std::move(gpu);
    preferred_enabled_ = true;
    /// 内部期限不是用户错误；超时后正常选择 CPU。 / Internal deadline selects CPU, not a user
    /// error.
    struct ProbeExpired {};
    const auto check_budget = [&] {
        if (elapsed() >= 2000)
            throw ProbeExpired{};
    };
    try {
        check_budget();
        double cpu = dispatch_.cpu_stream_fastest_ms, device = dispatch_.gpu_stream_slowest_ms;
        const auto lanes = std::min(pending_files, workers_.size());
        const auto jobs = std::min(pending_files, 4 * lanes);
        if (lanes > 1) {
            for (std::size_t i = 1; i < workers_.size(); ++i)
                std::copy(first.first.begin(), first.first.end(), workers_[i]->first.begin());
            check_budget();
            auto oracle = first.cpu_compute->hasher();
            for (unsigned block = 0; block < 2; ++block)
                oracle->update(first.first);
            const auto expected = oracle->finish();
            check_budget();
            const auto measure = [&](bool use_gpu) {
                check_budget();
                const auto value = probe_mixed(lanes, jobs, use_gpu, expected);
                check_budget();
                return value;
            };
            std::array<double, 3> cpu_samples{}, gpu_samples{};
            for (std::size_t round = 0; round < 3; ++round) {
                if (round % 2 == 0) {
                    cpu_samples[round] = measure(false);
                    gpu_samples[round] = measure(true);
                } else {
                    gpu_samples[round] = measure(true);
                    cpu_samples[round] = measure(false);
                }
            }
            // 保守采用 CPU 最快与混合最慢轮，不用单 GPU 加速比外推并发。
            // Use fastest CPU and slowest mixed rounds, never extrapolate serial GPU speedup.
            cpu = *std::min_element(cpu_samples.begin(), cpu_samples.end());
            device = *std::max_element(gpu_samples.begin(), gpu_samples.end());
        }
        dispatch_.mixed_cpu_ms = cpu;
        dispatch_.mixed_gpu_ms = device;
        // 单文件使用八块串行证据；多文件严格使用已测 jobs × 两块总字节。
        // Serial evidence covers eight blocks; mixed evidence covers exactly jobs times two blocks.
        const double sampled_bytes = static_cast<double>(first.first.size()) *
                                     (lanes == 1 ? 8.0 : 2.0 * static_cast<double>(jobs));
        dispatch_.expected_saving_ms =
            detail::conservative_gpu_saving(cpu, device, sampled_bytes, pending_bytes);
        dispatch_.setup_ms = elapsed();
        if (detail::gpu_setup_amortized(dispatch_.expected_saving_ms, dispatch_.setup_ms)) {
            gpu_workers_ = 1;
            return;
        }
        dispatch_.decision = detail::DispatchEvidence::Decision::cpu;
    } catch (const ProbeExpired&) {
        dispatch_.decision = detail::DispatchEvidence::Decision::cpu;
    } catch (const ComputeError&) {
        dispatch_.decision = detail::DispatchEvidence::Decision::failed;
    } catch (...) {
        preferred_enabled_ = false;
        first.compute = make_cpu_compute();
        throw;
    }
    preferred_enabled_ = false;
    first.compute = make_cpu_compute();
    dispatch_.setup_ms = elapsed();
}
} // namespace same
