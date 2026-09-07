#pragma once
#include "same/compute.hpp"
#include "same/config.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

namespace same {
struct Worker {
    std::vector<std::byte> first, second;
    std::unique_ptr<Compute> compute;
    std::atomic<std::size_t>* fallbacks;

    // Retry the complete operation, never continue a partially computed digest.
    // 整体重试操作，绝不继续使用计算失败后的部分摘要。
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

// A single admission point for hashing and comparison. Payload buffers are
// reserved before threads start; queue capacity provides producer backpressure.
// 哈希和比较共用入口；启动线程前预留数据缓冲，队列容量提供生产者背压。
class Resources {
public:
    explicit Resources(const Config& config);
    ~Resources();
    Resources(const Resources&) = delete;
    Resources& operator=(const Resources&) = delete;

    template <class F> auto submit(F&& operation) -> std::future<std::invoke_result_t<F, Worker&>> {
        using Task = std::packaged_task<std::invoke_result_t<F, Worker&>(Worker&)>;
        auto task = std::make_shared<Task>(std::forward<F>(operation));
        auto future = task->get_future();
        enqueue([task](Worker& worker) { (*task)(worker); });
        return future;
    }
    std::size_t gpu_workers() const {
        return gpu_workers_;
    }
    std::size_t fallbacks() const {
        return fallbacks_.load();
    }

private:
    void enqueue(std::function<void(Worker&)> task);
    void run(Worker& worker);
    void close();
    std::atomic<std::size_t> fallbacks_{0};
    std::size_t gpu_workers_{};
    std::size_t capacity_;
    std::vector<std::unique_ptr<Worker>> workers_;
    std::vector<std::thread> threads_;
    std::deque<std::function<void(Worker&)>> queue_;
    std::mutex mutex_;
    std::condition_variable ready_, space_;
    bool closed_{false};
};
} // namespace same
