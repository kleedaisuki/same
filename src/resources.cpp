#include "same/resources.hpp"
#include <stdexcept>

namespace same {
/// 在启动线程前完成资源创建，避免任务观察到半初始化上下文。 / Finish resource creation before any
/// task can observe a partially initialized context.
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
void Resources::enqueue(std::function<void(Worker&)> task) {
    std::unique_lock lock(mutex_);
    space_.wait(lock, [this] { return closed_ || queue_.size() < capacity_; });
    if (closed_)
        throw std::runtime_error("resource manager is closed");
    queue_.push_back(std::move(task));
    ready_.notify_one();
}
/// 只在出队期间持锁；packaged_task 将用户异常交给 future。 / Lock only while dequeuing;
/// packaged_task transfers user exceptions into its future.
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
