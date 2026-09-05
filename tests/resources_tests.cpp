#include "same/resources.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace std::chrono_literals;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
int main() {
    try {
        same::Config config;
        config.workers = 1; config.queue_capacity = 1; config.backend = "cpu";
        config.block_bytes = 1024; config.memory_bytes = 8192;
        same::Resources resources(config);
        std::promise<void> release, started;
        auto gate = release.get_future().share();
        auto first = resources.submit([&](same::Worker&) { started.set_value(); gate.wait(); return 1; });
        started.get_future().wait();
        auto second = resources.submit([](same::Worker&) { return 2; });
        auto producer = std::async(std::launch::async, [&] {
            return resources.submit([](same::Worker&) { return 3; }).get();
        });
        const bool blocked = producer.wait_for(50ms) == std::future_status::timeout;
        release.set_value();
        require(blocked, "producer must encounter backpressure");
        require(first.get() == 1 && second.get() == 2 && producer.get() == 3, "jobs lost");
        auto failure = resources.submit([](same::Worker&) -> int { throw std::runtime_error("job failure"); });
        bool threw = false;
        try { (void)failure.get(); } catch (const std::runtime_error&) { threw = true; }
        require(threw, "exceptions must propagate through futures");
        require(resources.submit([](same::Worker&) { return 4; }).get() == 4, "worker must survive task exceptions");
        auto fallback = resources.submit([](same::Worker& worker) {
            int calls = 0;
            return worker.execute([&] { if (!calls++) throw same::ComputeError("injected CUDA error"); return calls; });
        });
        require(fallback.get() == 2 && resources.fallbacks() == 1, "whole operation must retry on CPU");
        std::future<int> drained;
        {
            same::Resources temporary(config);
            drained = temporary.submit([](same::Worker&) { return 9; });
        }
        require(drained.get() == 9, "destructor must drain jobs");
        config.memory_bytes = 1;
        threw = false;
        try { same::Resources invalid(config); } catch (const std::runtime_error&) { threw = true; }
        require(threw, "impossible memory reservation must fail");
        std::cout << "bounded queue, lifecycle, exceptions and fallback passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
