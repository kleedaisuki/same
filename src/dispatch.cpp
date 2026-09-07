#include "same/detail/dispatch.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

namespace same::detail {
namespace {
using Clock = std::chrono::steady_clock;
/// 墙钟计时包含同步传输和 finish。 / Wall-clock timing includes synchronous transfers and finish.
double milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
/// 每次新摘要，重复输入保留正常流式 update 语义。 / Fresh digest with normal repeated streaming
/// updates.
Digest digest(Compute& compute, std::span<const std::byte> bytes, unsigned blocks) {
    auto hasher = compute.hasher();
    for (unsigned i = 0; i < blocks; ++i)
        hasher->update(bytes);
    return hasher->finish();
}
/// 比较完整 32 字节；不能将不正确的后端当作便宜后端。 / Compare all 32 bytes; incorrect backends
/// are not cheap backends.
double sample(Compute& compute, std::span<const std::byte> bytes, unsigned blocks,
              const Digest& expected) {
    const auto start = Clock::now();
    const auto actual = digest(compute, bytes, blocks);
    const auto elapsed = milliseconds(start);
    if (actual != expected)
        throw std::runtime_error("CUDA calibration digest mismatch");
    return elapsed;
}
/// 固定三次中位数，不受单次极端值决定。 / Median of three, not one outlier.
double median(std::array<double, 3> samples) {
    std::sort(samples.begin(), samples.end());
    return samples[1];
}
} // namespace
bool stable_gpu_win(const std::array<double, 3>& cpu, const std::array<double, 3>& gpu) {
    for (std::size_t i = 0; i < cpu.size(); ++i) {
        if (!std::isfinite(cpu[i]) || !std::isfinite(gpu[i]) || cpu[i] <= 0 || gpu[i] <= 0 ||
            gpu[i] > cpu[i] * 0.8)
            return false;
    }
    return true;
}
/// 采样主体；仅外层将设备错误转换为失败证据。 / Sampling core; only the wrapper converts device
/// errors to failed evidence.
static DispatchEvidence probe(Compute& cpu, Compute& gpu, std::span<std::byte> scratch,
                              std::chrono::milliseconds budget) {
    if (scratch.empty())
        throw std::invalid_argument("calibration requires nonempty scratch");
    DispatchEvidence result;
    result.decision = DispatchEvidence::Decision::cpu;
    const auto start = Clock::now();
    const auto expired = [&] {
        result.elapsed_ms = milliseconds(start);
        return result.elapsed_ms >= static_cast<double>(budget.count());
    };
    if (expired())
        return result;
    for (std::size_t i = 0; i < scratch.size(); ++i)
        scratch[i] = static_cast<std::byte>((i * 17 + i / 251) & 255);
    bool wins = true;
    for (unsigned blocks : {1U, 8U}) {
        // 独立 CPU 摘要亦是预热；GPU 预热单独验证，不计入选择样本。
        // The independent CPU oracle also warms CPU; validate GPU warmup outside decision samples.
        const auto expected = digest(cpu, scratch, blocks);
        if (expired())
            return result;
        sample(gpu, scratch, blocks, expected);
        if (expired())
            return result;
        const auto timed = [&](Compute& compute, double& value) {
            value = sample(compute, scratch, blocks, expected);
            return !expired();
        };
        std::array<double, 3> cpu_ms{}, gpu_ms{};
        for (std::size_t trial = 0; trial < cpu_ms.size(); ++trial) {
            if (trial % 2 == 0) {
                if (!timed(cpu, cpu_ms[trial]) || !timed(gpu, gpu_ms[trial]))
                    return result;
            } else {
                if (!timed(gpu, gpu_ms[trial]) || !timed(cpu, cpu_ms[trial]))
                    return result;
            }
        }
        if (blocks == 1) {
            result.cpu_block_ms = median(cpu_ms);
            result.gpu_block_ms = median(gpu_ms);
        } else {
            result.cpu_stream_ms = median(cpu_ms);
            result.gpu_stream_ms = median(gpu_ms);
        }
        wins = wins && stable_gpu_win(cpu_ms, gpu_ms);
    }
    result.elapsed_ms = milliseconds(start);
    if (wins)
        result.decision = DispatchEvidence::Decision::gpu;
    return result;
}
DispatchEvidence calibrate_dispatch(Compute& cpu, Compute& gpu, std::span<std::byte> scratch,
                                    std::chrono::milliseconds budget) {
    const auto start = Clock::now();
    try {
        return probe(cpu, gpu, scratch, budget);
    } catch (const ComputeError&) {
        DispatchEvidence result;
        result.decision = DispatchEvidence::Decision::failed;
        result.elapsed_ms = milliseconds(start);
        return result;
    }
}
} // namespace same::detail
