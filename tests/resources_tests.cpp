/** @file
 * @brief 统一有界队列、异常及关闭契约。 / Unified bounded queue, exceptions and shutdown.
 */
#include "same/resources.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
using namespace std::chrono_literals;
/// 保留失败上下文。 / Preserve failure context.
void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
/// 验证容量一背压及任务异常不终止工作线程。 / Verify capacity-one pressure and exception survival.
int main() {
    try {
        same::Config cfg;
        cfg.workers = 1;
        cfg.queue_capacity = 1;
        cfg.backend = "cpu";
        cfg.block_bytes = 1024;
        cfg.memory_bytes = 8192;
        same::Resources pool(cfg);
        std::promise<void> started, release;
        auto gate = release.get_future().share();
        auto first = pool.submit([&](same::Worker&) {
            started.set_value();
            gate.wait();
            return 1;
        });
        const bool entered = started.get_future().wait_for(5s) == std::future_status::ready;
        if (!entered) {
            release.set_value();
            require(false, "worker did not start");
        }
        auto second = pool.submit([](same::Worker&) { return 2; });
        std::promise<void> submitting;
        auto producer = std::async(std::launch::async, [&] {
            submitting.set_value();
            return pool.submit([](same::Worker&) { return 3; }).get();
        });
        submitting.get_future().wait();
        const bool blocked = producer.wait_for(50ms) == std::future_status::timeout;
        release.set_value();
        require(blocked, "capacity-one queue did not apply backpressure");
        require(first.get() == 1 && second.get() == 2 && producer.get() == 3, "accepted task lost");
        auto failure =
            pool.submit([](same::Worker&) -> int { throw std::runtime_error("task failure"); });
        bool threw = false;
        try {
            (void)failure.get();
        } catch (const std::runtime_error&) {
            threw = true;
        }
        require(threw, "future lost task exception");
        require(pool.submit([](same::Worker&) { return 4; }).get() == 4,
                "worker died after exception");
        std::future<int> drained;
        {
            same::Resources temporary(cfg);
            drained = temporary.submit_hash([](same::Worker&) { return 9; }, 1048576);
        }
        require(drained.get() == 9, "destruction lost accepted hash task");
        {
            auto closing_pool = std::make_unique<same::Resources>(cfg);
            std::promise<void> entered_close, release_close;
            auto close_gate = release_close.get_future().share();
            auto active = closing_pool->submit([&](same::Worker&) {
                entered_close.set_value();
                close_gate.wait();
                return 7;
            });
            const bool entered =
                entered_close.get_future().wait_for(5s) == std::future_status::ready;
            auto queued = closing_pool->submit_hash([](same::Worker&) { return 8; }, 1048576);
            auto closing = std::async(
                std::launch::async, [owned = std::move(closing_pool)]() mutable { owned.reset(); });
            const bool waiting = closing.wait_for(50ms) == std::future_status::timeout;
            release_close.set_value();
            closing.get();
            require(entered && waiting && active.get() == 7 && queued.get() == 8,
                    "shutdown did not drain in-flight and queued work");
        }
        cfg.memory_bytes = 1;
        threw = false;
        try {
            same::Resources invalid(cfg);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        require(threw, "impossible host budget was accepted");
        std::cout << "unified queue, exception and drain contracts passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
