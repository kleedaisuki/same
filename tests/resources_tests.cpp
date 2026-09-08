/** @file
 * @brief 有界队列背压、异常传播、CPU 重试和析构排空。 / Bounded-queue backpressure, exceptions, CPU
 * retries and destructor draining.
 */
#include "same/resources.hpp"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace std::chrono_literals;
/// 断言失败抛出可定位错误。 / Throw a diagnostic on assertion failure.
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
/** 阻塞固定 GPU 通道，验证 CPU 可领取有资格哈希；调用方提供容量一且至少双线程的池。
 * Block the pinned GPU lane and verify CPU stealing; caller supplies capacity one and at least
 * two workers. 自动策略可合理拒绝设备，因此明确记录跳过而不要求硬件必须加速。
 * Auto may legitimately reject the device, so log a skip instead of requiring a performance win.
 */
void verify_gpu_stealing(same::Resources& resources, const char* scenario) {
    if (!resources.gpu_workers()) {
        std::cout << "GPU stealing " << scenario << ": SKIPPED (auto selected CPU)\n";
        return;
    }
    std::promise<void> release_gpu, gpu_started;
    auto gpu_gate = release_gpu.get_future().share();
    auto pinned = resources.submit(
        [gpu_gate, &gpu_started](same::Worker&) {
            gpu_started.set_value();
            gpu_gate.wait();
        },
        true);
    gpu_started.get_future().wait();
    auto stolen =
        resources.submit_hash([](same::Worker& worker) { return worker.compute->name(); }, true);
    const bool ready = stolen.wait_for(2s) == std::future_status::ready;
    release_gpu.set_value();
    pinned.get();
    require(ready && stolen.get() == "cpu", "CPU failed to steal GPU-eligible work");
    std::cout << "GPU stealing " << scenario << ": EXECUTED capacity=1\n";
}
/// 运行本文件全部回归场景，断言失败即返回非零。 / Run all regressions; assertion failures produce a
/// nonzero exit.
int main() {
    try {
        same::Config config;
        config.workers = 1;
        config.queue_capacity = 1;
        config.backend = "cpu";
        config.block_bytes = 1024;
        config.memory_bytes = 8192;
        same::Resources resources(config);
        require(resources.gpu_workers() == 0 &&
                    resources.dispatch_evidence().decision ==
                        same::detail::DispatchEvidence::Decision::untested,
                "explicit CPU must bypass CUDA calibration");
        std::promise<void> release, started;
        auto gate = release.get_future().share();
        auto first = resources.submit([&](same::Worker&) {
            started.set_value();
            gate.wait();
            return 1;
        });
        started.get_future().wait();
        auto second = resources.submit([](same::Worker&) { return 2; });
        auto producer = std::async(std::launch::async, [&] {
            return resources.submit([](same::Worker&) { return 3; }).get();
        });
        const bool blocked = producer.wait_for(50ms) == std::future_status::timeout;
        release.set_value();
        require(blocked, "producer must encounter backpressure");
        require(first.get() == 1 && second.get() == 2 && producer.get() == 3, "jobs lost");
        auto failure =
            resources.submit([](same::Worker&) -> int { throw std::runtime_error("job failure"); });
        bool threw = false;
        try {
            (void)failure.get();
        } catch (const std::runtime_error&) {
            threw = true;
        }
        require(threw, "exceptions must propagate through futures");
        require(resources.submit([](same::Worker&) { return 4; }).get() == 4,
                "worker must survive task exceptions");
        auto fallback = resources.submit([](same::Worker& worker) {
            int calls = 0;
            return worker.execute([&] {
                if (!calls++)
                    throw same::ComputeError("injected CUDA error");
                return calls;
            });
        });
        require(fallback.get() == 2 && resources.fallbacks() == 1,
                "whole operation must retry on CPU");
        std::future<int> drained;
        {
            same::Resources temporary(config);
            drained = temporary.submit([](same::Worker&) { return 9; });
        }
        require(drained.get() == 9, "destructor must drain jobs");
        {
            auto lazy_config = config;
            lazy_config.backend = "auto";
            same::Resources lazy(lazy_config);
            require(lazy.gpu_workers() == 0 &&
                        lazy.dispatch_evidence().decision ==
                            same::detail::DispatchEvidence::Decision::deferred,
                    "auto construction must not probe CUDA");
            require(lazy.submit([](same::Worker& worker) { return worker.compute->name(); }, true)
                            .get() == "cpu",
                    "preferred submission before calibration must remain CPU");
            require(
                lazy.submit_hash([](same::Worker& worker) { return worker.compute->name(); }, true)
                        .get() == "cpu",
                "eligible hashes before probing must remain CPU");
            lazy.prepare_auto(lazy_config, 0, 0);
            require(lazy.dispatch_evidence().decision ==
                        same::detail::DispatchEvidence::Decision::deferred,
                    "empty work must not probe CUDA");
        }
        if (std::getenv("SAME_REQUIRE_CUDA")) {
            same::Config probe;
            probe.backend = "auto";
            probe.workers = 4;
            probe.queue_capacity = 1;
            probe.block_bytes = 16 * 1024 * 1024;
            probe.memory_bytes = 256 * 1024 * 1024;
            probe.device_memory_bytes = 256 * 1024 * 1024;
            same::Resources calibrated(probe);
            calibrated.prepare_auto(probe, 64ULL * 1024 * 1024 * 1024, 8);
            const auto evidence = calibrated.dispatch_evidence();
            require(calibrated.gpu_workers() <= 1, "auto enabled multiple GPU lanes");
            const auto selected =
                calibrated.submit([](same::Worker& worker) { return worker.compute->name(); }, true)
                    .get();
            require(selected == (calibrated.gpu_workers() ? "cuda" : "cpu"),
                    "preferred lane routing failed");
            verify_gpu_stealing(calibrated, "mixed-pool");
            calibrated.prepare_auto(probe, 128ULL * 1024 * 1024 * 1024, 16);
            require(calibrated.dispatch_evidence().setup_ms == evidence.setup_ms,
                    "auto probed twice");
            std::cout << "adaptive capacity=1 cpu_stream_ms=" << evidence.cpu_stream_ms
                      << " gpu_stream_ms=" << evidence.gpu_stream_ms << '\n';
            // 单文件证据不要求混合池提速，但保留另一 CPU 线程来验证窃取。
            // Single-file evidence bypasses mixed-pool speedup, retaining a CPU lane for stealing.
            probe.workers = 2;
            same::Resources serial(probe);
            serial.prepare_auto(probe, 64ULL * 1024 * 1024 * 1024, 1);
            verify_gpu_stealing(serial, "single-file");
            const auto& serial_evidence = serial.dispatch_evidence();
            std::cout << "single-file probe capacity=1 cpu_ms=" << serial_evidence.mixed_cpu_ms
                      << " gpu_ms=" << serial_evidence.mixed_gpu_ms << '\n';
        }
        config.memory_bytes = 1;
        threw = false;
        try {
            same::Resources invalid(config);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        require(threw, "impossible memory reservation must fail");
        std::cout << "bounded queue, lifecycle, exceptions and fallback passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
