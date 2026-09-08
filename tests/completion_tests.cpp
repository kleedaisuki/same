/** @file 完成顺序、接收上限和异常生命周期回归。
 * Completion order, admission bounds and exception lifetime regressions. */
#include "same/detail/completion_jobs.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>

using namespace std::chrono_literals;
/// 断言在发布构建中仍有效。 / Assertions remain active in release builds.
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
/// 退出作用域时放行工作任务，避免失败断言阻塞线程池析构。
/// Release blocked workers on scope exit, including assertion failures.
struct Gate {
    /// 生产者与消费者共享的单次门闩。 / One-shot shared latch.
    std::promise<void> release;
    std::shared_future<void> wait = release.get_future().share();
    /// 自动放行；门闩只能由析构触发。 / Destruction is the sole release operation.
    ~Gate() {
        release.set_value();
    }
};
/// 确定性验证后提交任务先被收取，而非依赖文件大小或调度时间。
/// Deterministically collect a later submission first, without file-size timing assumptions.
void completion_order(same::Resources& resources) {
    same::detail::CompletionJobs<int> jobs(resources, 2);
    std::future<int> received;
    bool ready = false;
    {
        Gate gate;
        std::promise<void> started;
        auto start = started.get_future();
        jobs.submit([wait = gate.wait, &started](same::Worker&) {
            started.set_value();
            wait.wait();
            return 1;
        });
        start.wait();
        jobs.submit([](same::Worker&) { return 2; });
        received = std::async(std::launch::async, [&] { return jobs.next(); });
        ready = received.wait_for(2s) == std::future_status::ready;
    }
    const auto first = received.get();
    const auto second = jobs.next();
    require(ready && first == 2 && second == 1, "completion order regressed to submission order");
    require(jobs.pending() == 0, "pending count after draining");
}
/// 容量包含执行中与完成未收取任务；收取后可反复复用单槽。
/// Admission includes running and unconsumed completed work; one slot is reusable.
void single_slot(same::Resources& resources) {
    same::detail::CompletionJobs<int> jobs(resources, 1);
    for (int i = 0; i < 100; ++i) {
        jobs.submit([i](same::Worker&) { return i; });
        bool rejected = false;
        try {
            jobs.submit([](same::Worker&) { return -1; });
        } catch (const std::logic_error&) {
            rejected = true;
        }
        require(rejected && jobs.pending() == 1, "admission exceeded ring capacity");
        require(jobs.next() == i, "single-slot result lost");
    }
    bool rejected = false;
    try {
        (void)jobs.next();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    require(rejected, "empty receive must not hang");
    rejected = false;
    try {
        same::detail::CompletionJobs<int> invalid(resources, 0);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "zero capacity accepted");
}
/// 哈希提示保持完成环容量、移动捕获及异常传播，不依赖 GPU 是否存在。
/// Hash hints preserve ring bounds, move-only captures and exceptions without requiring a GPU.
void hash_admission(same::Resources& resources) {
    same::detail::CompletionJobs<int> jobs(resources, 1);
    for (const std::uint64_t bytes : {0ULL, 4096ULL}) {
        jobs.submit_hash([value = std::make_unique<int>(42)](same::Worker&) { return *value; },
                         bytes);
        bool rejected = false;
        try {
            jobs.submit_hash([](same::Worker&) { return -1; }, bytes);
        } catch (const std::logic_error&) {
            rejected = true;
        }
        require(rejected && jobs.pending() == 1, "hash admission exceeded capacity");
        require(jobs.next() == 42, "hash move-only capture lost");
        jobs.submit_hash([](same::Worker&) -> int { throw std::runtime_error("hash failure"); },
                         bytes);
        bool failed = false;
        try {
            (void)jobs.next();
        } catch (const std::runtime_error&) {
            failed = true;
        }
        require(failed && jobs.pending() == 0, "hash failure did not release capacity");
    }
}
/// 异常结果消费后销毁协调器，在途任务仍拥有完成存储。
/// Destroy the coordinator after an exception while another task retains completion storage.
void exception_lifetime(same::Resources& resources) {
    std::promise<void> finished;
    auto done = finished.get_future();
    bool threw = false;
    {
        Gate gate;
        {
            same::detail::CompletionJobs<int> jobs(resources, 2);
            std::promise<void> started;
            auto start = started.get_future();
            jobs.submit([wait = gate.wait, &started, &finished](same::Worker&) {
                started.set_value();
                wait.wait();
                finished.set_value();
                return 7;
            });
            start.wait();
            jobs.submit([](same::Worker&) -> int { throw std::runtime_error("injected"); });
            try {
                (void)jobs.next();
            } catch (const std::runtime_error&) {
                threw = true;
            }
            require(jobs.pending() == 1, "exception did not release admission slot");
        }
    }
    done.wait();
    require(threw, "worker exception was swallowed");
}
/// 延迟设备初始化异常必须通过完成环传播，不能执行回调或遗失完成通知。
/// Lazy startup exceptions must publish completion without running callback side effects.
void startup_exception_completion(bool allocation_failure) {
    same::Config config;
    config.workers = 1;
    config.queue_capacity = 1;
    config.backend = "cuda";
    config.gpu_min_bytes = 0;
    config.memory_bytes = config.device_memory_bytes = 128 * 1024 * 1024;
    same::Resources resources(
        config, [allocation_failure](std::size_t, std::size_t) -> std::unique_ptr<same::Compute> {
            if (allocation_failure)
                throw std::bad_alloc();
            throw std::runtime_error("injected lazy initialization failure");
        });
    same::detail::CompletionJobs<int> jobs(resources, 1);
    std::atomic<unsigned> callbacks{};
    jobs.submit_hash(
        [&](same::Worker&) {
            ++callbacks;
            return 7;
        },
        std::uint64_t{4096});
    bool correct = false;
    try {
        (void)jobs.next();
    } catch (const std::bad_alloc&) {
        correct = allocation_failure;
    } catch (const std::runtime_error& error) {
        correct = !allocation_failure &&
                  std::string(error.what()) == "injected lazy initialization failure";
    }
    require(correct && callbacks == 0 && jobs.pending() == 0,
            "startup exception lost completion or executed callback side effects");
    resources.wait_idle();
}
/// CPU 实例避免测试结果依赖 CUDA 可用性。 / CPU instance makes tests independent of CUDA.
int main() {
    try {
        startup_exception_completion(false);
        startup_exception_completion(true);
        same::Config config;
        config.workers = 2;
        config.queue_capacity = 2;
        config.backend = "cpu";
        config.block_bytes = 1024;
        config.memory_bytes = 16384;
        same::Resources resources(config);
        completion_order(resources);
        single_slot(resources);
        hash_admission(resources);
        exception_lifetime(resources);
        std::cout << "completion order, bounds and exception lifetime passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
