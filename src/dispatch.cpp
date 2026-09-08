#include "same/detail/dispatch.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

namespace same::detail {
HashRoute classify_hash(std::uint64_t bytes, std::size_t floor, std::size_t gpu_block,
                        const DispatchEvidence& evidence) {
    if (bytes < floor || !bytes)
        return HashRoute::cpu_only;
    if (!evidence.calibration_complete || !gpu_block)
        return HashRoute::cpu_preferred;
    const bool stream = bytes >= 64ULL * 1024 * 1024 && bytes / gpu_block >= 4;
    const bool wins = stream ? evidence.stream_gpu_preferred : evidence.block_gpu_preferred;
    // 小于校准输入的未测形状不外推单块胜利。 / Do not extrapolate a block win below its input size.
    return wins && bytes >= gpu_block ? HashRoute::gpu_preferred : HashRoute::cpu_preferred;
}

namespace {
using Clock = std::chrono::steady_clock;
/// 墙钟计时包含同步传输和 finish。 / Wall-clock timing includes synchronous transfers and finish.
double milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
/// 每次新摘要，重复输入保留正常流式 update 语义。 / Fresh digest with normal repeated streaming
/// updates.
Digest digest(Compute& compute, std::span<const std::byte> bytes, unsigned blocks,
              std::size_t update_bytes) {
    auto hasher = compute.hasher();
    for (unsigned i = 0; i < blocks; ++i)
        for (std::size_t offset = 0; offset < bytes.size();
             offset += std::min(update_bytes, bytes.size() - offset))
            hasher->update(bytes.subspan(offset, std::min(update_bytes, bytes.size() - offset)));
    return hasher->finish();
}
/// 比较完整 32 字节；不能将不正确的后端当作便宜后端。 / Compare all 32 bytes; incorrect backends
/// are not cheap backends.
double sample(Compute& compute, std::span<const std::byte> bytes, unsigned blocks,
              const Digest& expected, std::size_t update_bytes) {
    const auto start = Clock::now();
    const auto actual = digest(compute, bytes, blocks, update_bytes);
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
    return *std::max_element(gpu.begin(), gpu.end()) <=
           0.8 * *std::min_element(cpu.begin(), cpu.end());
}
/// 采样主体；仅外层将设备错误转换为失败证据。 / Sampling core; only the wrapper converts device
/// errors to failed evidence.
static DispatchEvidence probe(Compute& cpu, Compute& gpu, std::span<std::byte> scratch,
                              std::chrono::milliseconds budget, std::size_t cpu_block_bytes) {
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
    const auto cpu_step = cpu_block_bytes ? cpu_block_bytes : scratch.size();
    for (unsigned blocks : {1U, 4U}) {
        // 独立 CPU 摘要亦是预热；GPU 预热单独验证，不计入选择样本。
        // The independent CPU oracle also warms CPU; validate GPU warmup outside decision samples.
        const auto expected = digest(cpu, scratch, blocks, cpu_step);
        if (expired())
            return result;
        sample(gpu, scratch, blocks, expected, scratch.size());
        if (expired())
            return result;
        const auto timed = [&](Compute& compute, double& value, std::size_t step) {
            value = sample(compute, scratch, blocks, expected, step);
            return !expired();
        };
        std::array<double, 3> cpu_ms{}, gpu_ms{};
        for (std::size_t trial = 0; trial < cpu_ms.size(); ++trial) {
            if (trial % 2 == 0) {
                if (!timed(cpu, cpu_ms[trial], cpu_step) ||
                    !timed(gpu, gpu_ms[trial], scratch.size()))
                    return result;
            } else {
                if (!timed(gpu, gpu_ms[trial], scratch.size()) ||
                    !timed(cpu, cpu_ms[trial], cpu_step))
                    return result;
            }
        }
        if (blocks == 1) {
            result.cpu_block_ms = median(cpu_ms);
            result.gpu_block_ms = median(gpu_ms);
            result.block_gpu_preferred = stable_gpu_win(cpu_ms, gpu_ms);
        } else {
            result.cpu_stream_ms = median(cpu_ms);
            result.gpu_stream_ms = median(gpu_ms);
            result.cpu_stream_fastest_ms = *std::min_element(cpu_ms.begin(), cpu_ms.end());
            result.gpu_stream_slowest_ms = *std::max_element(gpu_ms.begin(), gpu_ms.end());
            result.stream_gpu_preferred = stable_gpu_win(cpu_ms, gpu_ms);
        }
    }
    result.elapsed_ms = milliseconds(start);
    result.calibration_complete = true;
    if (result.block_gpu_preferred || result.stream_gpu_preferred)
        result.decision = DispatchEvidence::Decision::gpu;
    return result;
}
DispatchEvidence calibrate_dispatch(Compute& cpu, Compute& gpu, std::span<std::byte> scratch,
                                    std::chrono::milliseconds budget, std::size_t cpu_block_bytes) {
    const auto start = Clock::now();
    try {
        return probe(cpu, gpu, scratch, budget, cpu_block_bytes);
    } catch (const ComputeError&) {
        DispatchEvidence result;
        result.decision = DispatchEvidence::Decision::failed;
        result.elapsed_ms = milliseconds(start);
        return result;
    }
}
} // namespace same::detail
