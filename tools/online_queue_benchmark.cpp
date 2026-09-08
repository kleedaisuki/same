/** @file
 * @brief 真实 Resources 队列的剖析增量成本；无文件 I/O、无 GPU。
 * Profiling overhead through the real Resources queue, without file I/O or GPU.
 * Usage: online_queue_benchmark [jobs_per_batch=50000] [trials=7]
 * Samples are explicitly synthetic 1 ms model-update inputs, NOT measured service latency.
 * 样本明确采用合成的 1 ms 更新输入，不得解释为真实任务服务时间。
 */
#include "same/resources.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <future>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
/// 原始队列端到端成本及模型计数；初始化在计时外。
/// Raw end-to-end queue costs and model counts; initialization is outside timing.
struct Result {
    double ns_per_job{};
    std::uint64_t jobs{}, checksum{}, samples{};
};
/// 返回确定性工作结果与实际抽样决定。 / Return work result and actual sampling decision.
struct TaskResult {
    std::uint64_t value;
    bool sampled;
};
/// 严格限制命令行运行范围。 / Strictly bound command-line run sizes.
std::uint64_t argument(const char* text, std::uint64_t maximum) {
    const std::string value(text);
    std::size_t consumed = 0;
    if (value.empty() || value.front() == '-')
        throw std::runtime_error("arguments must be positive integers");
    const auto number = std::stoull(value, &consumed);
    if (!number || consumed != value.size() || number > maximum)
        throw std::runtime_error("argument outside supported range");
    return number;
}
/// 提交/执行/收取/空闲屏障全部计时，只有64个 future 同时存活。
/// Time submission/execution/collection/idle barrier with at most 64 retained futures.
Result run(std::size_t workers, bool pgo, bool eligible, std::uint64_t batch_jobs) {
    same::Config config;
    config.workers = workers;
    config.backend = "cpu";
    config.pgo = pgo;
    config.block_bytes = 1024;
    config.queue_capacity = 64;
    same::Resources resources(config);
    std::array<std::future<TaskResult>, 64> ring;
    const auto bytes = eligible ? 64ULL * 1024 * 1024 : 4096ULL;
    const auto route =
        eligible ? same::detail::HashRoute::cpu_preferred : same::detail::HashRoute::cpu_only;
    std::uint64_t jobs = 0, checksum = 0, expected = 0;
    const auto begin = std::chrono::steady_clock::now();
    auto end = begin;
    do {
        for (std::uint64_t i = 0; i < batch_jobs; ++i) {
            auto& future = ring[i % ring.size()];
            if (future.valid()) {
                const auto result = future.get();
                checksum += result.value;
                expected += result.sampled;
            }
            const auto input = jobs + i;
            future = resources.submit_hash(
                [input, bytes, eligible](same::Worker& worker) {
                    const auto value = (input * 6364136223846793005ULL + bytes) ^ (input >> 7);
                    // 与生产路径相同的线程本地抽样序号；报告实际样本数。
                    // Production-style worker-local sequence; report actual sample count.
                    const bool sampled = worker.profile_enabled &&
                                         (eligible || (++worker.profile_sequence & 63) == 0);
                    if (sampled)
                        worker.sample = {bytes, 1.0, false, true};
                    return TaskResult{value, sampled};
                },
                route, bytes);
        }
        for (auto& future : ring) {
            if (future.valid()) {
                const auto result = future.get();
                checksum += result.value;
                expected += result.sampled;
            }
        }
        resources.wait_idle();
        jobs += batch_jobs;
        end = std::chrono::steady_clock::now();
    } while (end - begin < std::chrono::milliseconds(100));
    const auto stats = resources.profile_snapshot();
    if (stats.samples != expected)
        throw std::runtime_error("model samples differ from deterministic sampling oracle");
    return {std::chrono::duration<double, std::nano>(end - begin).count() /
                static_cast<double>(jobs),
            jobs, checksum, stats.samples};
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc > 3)
            throw std::runtime_error("usage: online_queue_benchmark [jobs_per_batch] [trials]");
        const auto batch = argc > 1 ? argument(argv[1], 10000000) : 50000;
        const auto trials = argc > 2 ? argument(argv[2], 1000) : 7;
        // 预热独立于原始 CSV；成对交替 PGO 顺序降低系统漂移偏差。
        // Warmup is separate from raw CSV; paired alternating PGO order reduces drift bias.
        std::uint64_t warmup_checksum = 0;
        for (const auto workers : {1U, 4U})
            for (const bool eligible : {false, true})
                for (const bool pgo : {false, true})
                    warmup_checksum += run(workers, pgo, eligible, batch).checksum;
        std::cerr << "warmup_checksum=" << warmup_checksum
                  << "; samples=synthetic_1ms; no_gpu_no_io; whole_queue_contention_included\n";
        std::cout << "trial,workers,payload,pgo,jobs,ns_per_job,checksum,synthetic_samples\n"
                  << std::setprecision(17);
        for (std::uint64_t trial = 0; trial < trials; ++trial) {
            for (const auto workers : {1U, 4U}) {
                for (const bool eligible : {false, true}) {
                    for (unsigned order = 0; order < 2; ++order) {
                        const bool pgo = (trial + order) % 2 != 0;
                        const auto result = run(workers, pgo, eligible, batch);
                        std::cout << trial << ',' << workers << ','
                                  << (eligible ? "eligible_64MiB" : "small_4KiB") << ',' << pgo
                                  << ',' << result.jobs << ',' << result.ns_per_job << ','
                                  << result.checksum << ',' << result.samples << '\n';
                    }
                }
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
