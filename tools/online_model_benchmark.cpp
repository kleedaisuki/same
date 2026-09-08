/** @file
 * @brief 固定输入的路由成本微基准；不代表端到端文件扫描性能。
 * Fixed-input routing-cost microbenchmark, not end-to-end scan performance.
 * Usage: online_model_benchmark [iterations=2000000] [trials=10]
 */
#include "same/detail/online_model.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
/// 研究默认资格门槛。 / Research-default eligibility floor.
constexpr std::uint64_t floor_bytes = 16ULL * 1024 * 1024;
/// 预先生成的确定性输入，时段内不分配内存。
/// Precomputed deterministic inputs; no allocation inside the measured region.
struct Inputs {
    std::array<std::uint64_t, 4096> bytes{}, small_bytes{};
    std::array<double, 4096> duration_ms{};
};
/// 独立测量原始总成本，不减去不稳定的基线。
/// Measure raw total costs independently, without subtracting a noisy baseline.
enum class Case { control, baseline, predict, observe, small_off, small_on };
/// 让每轮的结果在计时区之外可见。 / Make each timed batch observable outside timing.
struct Result {
    double ns_per_job{}, checksum{};
    std::uint64_t jobs{};
};
/// 平衡小文件、资格边界与长流区间。 / Balance small, boundary and long-stream sizes.
Inputs make_inputs() {
    Inputs inputs;
    std::uint64_t state = 0x123456789abcdefULL;
    for (std::size_t i = 0; i < inputs.bytes.size(); ++i) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        inputs.bytes[i] = (1ULL << (10 + i % 21)) + (state & 1023);
        inputs.small_bytes[i] = 1 + (state % (floor_bytes - 1));
        inputs.duration_ms[i] = static_cast<double>(inputs.bytes[i]) / 1048576.0 +
                                static_cast<double>(state & 31) / 1000.0;
    }
    return inputs;
}
/// 播种 CPU/GPU 大小区间；初始化不计入任务路径成本。
/// Seed CPU/GPU size bands; initialization is outside task-path timing.
same::detail::OnlineModel make_model(const Inputs& inputs) {
    same::detail::OnlineModel model;
    for (const auto bytes : inputs.bytes) {
        model.seed(false, bytes, static_cast<double>(bytes) / 1048576.0);
        model.seed(true, bytes, static_cast<double>(bytes) / 2097152.0);
    }
    return model;
}
/// 编译期固定用例消除测试框架自身的每任务分派开销。
/// Compile-time cases eliminate per-task dispatch overhead from the harness itself.
template <Case mode> Result run(const Inputs& inputs, std::uint64_t iterations) {
    auto model = make_model(inputs);
    double checksum = 0;
    std::uint64_t jobs = 0;
    const auto begin = std::chrono::steady_clock::now();
    auto end = begin;
    do {
        for (std::uint64_t i = 0; i < iterations; ++i) {
            const auto index = static_cast<std::size_t>(i & (inputs.bytes.size() - 1));
            const auto bytes = inputs.bytes[index];
            if constexpr (mode == Case::control) {
                // 无路由的循环/读取/校验和对照。 / Loop/load/checksum control without routing.
                checksum += bytes & 1;
            } else if constexpr (mode == Case::baseline) {
                checksum += bytes >= floor_bytes;
            } else if constexpr (mode == Case::predict) {
                const auto prediction = model.predict((i & 1) != 0, bytes);
                checksum += prediction.known ? prediction.ms : -1;
            } else if constexpr (mode == Case::observe) {
                checksum += model.observe((i & 1) != 0, bytes, inputs.duration_ms[index]);
            } else {
                const auto small_bytes = inputs.small_bytes[index];
                if constexpr (mode == Case::small_on) {
                    if (small_bytes >= floor_bytes)
                        checksum += model.predict(false, small_bytes).ms;
                    else
                        checksum += small_bytes != 0;
                } else {
                    checksum += small_bytes != 0;
                }
            }
        }
        jobs += iterations;
        end = std::chrono::steady_clock::now();
    } while (end - begin < std::chrono::milliseconds(100));
    // 观察模型状态，阻止丢弃更新；不包含在计时中。
    // Observe model state to prevent dead-store elimination; outside timing.
    checksum += model.snapshot().samples;
    checksum += model.predict(false, inputs.bytes[0]).ms;
    return {std::chrono::duration<double, std::nano>(end - begin).count() /
                static_cast<double>(jobs),
            checksum, jobs};
}
/// 数字解析严格拒绝截断与无界运行。 / Strict numeric parsing rejects truncation/unbounded runs.
std::uint64_t argument(const char* text, std::uint64_t maximum) {
    const std::string value(text);
    std::size_t consumed = 0;
    if (value.empty() || value.front() == '-')
        throw std::runtime_error("arguments must be positive integers");
    const auto number = std::stoull(value, &consumed);
    if (consumed != value.size() || !number || number > maximum)
        throw std::runtime_error("argument outside supported range");
    return number;
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc > 3)
            throw std::runtime_error("usage: online_model_benchmark [iterations] [trials]");
        const auto iterations = argc > 1 ? argument(argv[1], 1000000000) : 2000000;
        const auto trials = argc > 2 ? argument(argv[2], 1000) : 10;
        const auto inputs = make_inputs();
        using Runner = Result (*)(const Inputs&, std::uint64_t);
        const std::array<Runner, 6> runners{run<Case::control>,   run<Case::baseline>,
                                            run<Case::predict>,   run<Case::observe>,
                                            run<Case::small_off>, run<Case::small_on>};
        constexpr std::array names{"loop_control",
                                   "static_eligibility",
                                   "predict",
                                   "observe",
                                   "small_eligibility_branch_only_off",
                                   "small_eligibility_branch_only_on"};
        // 每个用例先预热；交替顺序减少温度与频率漂移偏差。
        // Warm every case first; alternate order to reduce thermal/frequency drift bias.
        double warmup_checksum = 0;
        for (const auto runner : runners)
            warmup_checksum += runner(inputs, iterations).checksum;
        std::cerr << "warmup_checksum=" << std::setprecision(17) << warmup_checksum << '\n';
        std::cout << "trial,order,case,jobs,ns_per_job,checksum\n" << std::setprecision(17);
        for (std::uint64_t trial = 0; trial < trials; ++trial) {
            for (std::size_t order = 0; order < runners.size(); ++order) {
                const auto index = trial % 2 ? runners.size() - 1 - order : order;
                const auto result = runners[index](inputs, iterations);
                std::cout << trial << ',' << order << ',' << names[index] << ',' << result.jobs
                          << ',' << result.ns_per_job << ',' << result.checksum << '\n';
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
