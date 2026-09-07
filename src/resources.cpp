#include "same/resources.hpp"
#include <stdexcept>

namespace same {
Resources::Resources(const Config& config) : capacity_(config.queue_capacity) {
    config.validate();
    for (std::size_t i = 0; i < config.workers; ++i) {
        auto worker = std::make_unique<Worker>();
        worker->first.resize(config.block_bytes);
        worker->second.resize(config.block_bytes);
        worker->fallbacks = &fallbacks_;
        if (config.backend != "cpu")
            worker->compute =
                try_cuda_compute(config.block_bytes, config.device_memory_bytes / config.workers);
        if (worker->compute)
            ++gpu_workers_;
        else
            worker->compute = make_cpu_compute();
        workers_.push_back(std::move(worker));
    }
    try {
        for (auto& worker : workers_)
            threads_.emplace_back([this, ptr = worker.get()] { run(*ptr); });
    } catch (...) {
        close();
        throw;
    }
}
Resources::~Resources() {
    close();
}
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
void Resources::enqueue(std::function<void(Worker&)> task) {
    std::unique_lock lock(mutex_);
    space_.wait(lock, [this] { return closed_ || queue_.size() < capacity_; });
    if (closed_)
        throw std::runtime_error("resource manager is closed");
    queue_.push_back(std::move(task));
    ready_.notify_one();
}
void Resources::run(Worker& worker) {
    for (;;) {
        std::function<void(Worker&)> task;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, [this] { return closed_ || !queue_.empty(); });
            if (queue_.empty())
                return;
            task = std::move(queue_.front());
            queue_.pop_front();
            space_.notify_one();
        }
        task(worker);
    }
}
} // namespace same
