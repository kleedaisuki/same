/** @file
 * 两个同质线程同时拥有真实 CUDA 任务，结果与 CPU 完整摘要一致。
 * Two homogeneous workers admit real CUDA jobs together and match complete CPU digests.
 */
#include "same/resources.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
using namespace std::chrono_literals;

namespace {
/// 将实际后端与完整摘要一起交给验证线程。 / Return actual backend and complete digest.
struct Result {
    std::size_t worker;
    bool gpu;
    same::Digest digest;
};
/// 使用相同逻辑输入反复 update，包含非块对齐尾部。 / Repeat blocks with a nonaligned tail.
same::Digest digest(same::Compute& compute, std::span<std::byte> buffer, unsigned char value) {
    std::fill(buffer.begin(), buffer.end(), std::byte(value));
    auto hasher = compute.hasher();
    for (unsigned i = 0; i < 4; ++i)
        hasher->update(buffer);
    hasher->update(buffer.first(113));
    return hasher->finish();
}
} // namespace

/// 环境要求 CUDA 时不允许静默跳过；CPU-only 仍验证生命周期。 / Required CUDA forbids skips.
int main() {
    try {
        same::Config config;
        config.workers = 2;
        config.queue_capacity = 2;
        config.memory_bytes = config.device_memory_bytes = 128ULL * 1024 * 1024;
        config.gpu_min_bytes = 1;
        config.backend = "auto";
        same::Resources pool(config);
        // 先结束一次共享运行时冷启动；此测试验证就绪后的双设备任务能力。
        // Finish shared-runtime bootstrap first; test concurrent admission after readiness.
        pool.submit_hash([](same::Worker& worker) { return worker.compute->name(); },
                         64ULL * 1024 * 1024)
            .get();
        pool.wait_idle();
        std::promise<void> release, entered[2];
        auto gate = release.get_future().share();
        auto first_entered = entered[0].get_future();
        auto second_entered = entered[1].get_future();
        auto submit = [&](unsigned index) {
            return pool.submit_hash(
                [&, index](same::Worker& worker) {
                    return worker.execute([&] {
                        entered[index].set_value();
                        gate.wait();
                        const bool gpu = worker.compute->name() == "cuda";
                        auto actual = digest(*worker.compute, worker.first,
                                             static_cast<unsigned char>(index + 7));
                        auto expected = digest(*worker.cpu_compute, worker.first,
                                               static_cast<unsigned char>(index + 7));
                        if (actual != expected)
                            throw std::runtime_error("concurrent CUDA digest mismatch");
                        return Result{worker.index, gpu, actual};
                    });
                },
                64ULL * 1024 * 1024);
        };
        auto first = submit(0);
        auto second = submit(1);
        const bool together = first_entered.wait_for(15s) == std::future_status::ready &&
                              second_entered.wait_for(15s) == std::future_status::ready;
        // 先释放再断言，失败也不能让池析构永久等待。 / Release before any failing assertion.
        release.set_value();
        const auto a = first.get();
        const auto b = second.get();
        pool.wait_idle();
        if (!together || a.worker == b.worker || pool.worker_count() != 2)
            throw std::runtime_error("fixed workers did not admit independent jobs");
        if (a.gpu && b.gpu) {
            if (pool.gpu_workers() != 2 || pool.gpu_peak_concurrency() < 2)
                throw std::runtime_error("GPU tasks still serialized by a single-worker gate");
            std::cout << "two CUDA-capable workers; GPU task peak=" << pool.gpu_peak_concurrency()
                      << "; full CPU digest parity passed\n";
        } else if (std::getenv("SAME_REQUIRE_CUDA")) {
            throw std::runtime_error("required two real CUDA backends unavailable");
        } else {
            std::cout << "CUDA unavailable; CPU lifecycle/parity passed (device check skipped)\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
