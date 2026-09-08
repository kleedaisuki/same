#pragma once
#include "same/compute.hpp"
#include <array>
#include <chrono>
#include <span>

namespace same::detail {
/// 载荷约束与偏好分离；CPU-only 不得因 CPU 忙而卸载。
/// Separate payload eligibility from preference; CPU-only never spills to a busy CPU's GPU.
enum class HashRoute { cpu_only, cpu_preferred, gpu_preferred };

/// 自动选择的保守证据，不是所有文件大小/并发度的性能保证。
/// Conservative auto-dispatch evidence, not a guarantee for every size or concurrency.
struct DispatchEvidence {
    /// 校准结论与服务状态；adaptive 表示按载荷和忙闲分流。
    /// Calibration/service state; adaptive routes by payload and live occupancy.
    enum class Decision {
        untested,
        deferred,
        cpu,
        gpu,
        failed,
        adaptive
    } decision{Decision::untested};
    /// 两类摘要均完整验证并计时；设备可用性不等同于任何一类性能胜出。
    /// Both shapes were fully validated/timed; availability is distinct from winning either shape.
    bool calibration_complete{};
    /// 独立的短块/长流偏好，单块不胜不得否决长流胜出。
    /// Independent block/stream preferences; losing one block must not veto a winning stream.
    bool block_gpu_preferred{}, stream_gpu_preferred{};
    /// 实际块和四块流的 CPU/GPU 中位毫秒，包含摘要创建、传输及完成。
    /// Median CPU/GPU milliseconds for one block and four blocks, including
    /// creation/transfers/finish.
    double cpu_block_ms{}, gpu_block_ms{}, cpu_stream_ms{}, gpu_stream_ms{};
    /// 保留样本极值用于检查抖动。 / Retain timing extremes for jitter inspection.
    double cpu_stream_fastest_ms{}, gpu_stream_slowest_ms{};
    /// 串行校准时间（含预热），属于 scan 中的 setup_ms，不是额外可相加阶段。
    /// Serial calibration including warmup; part of scan setup_ms, not an additional phase.
    double elapsed_ms{};
    /// 旧混合/摊销字段保持零以兼容统计；setup_ms 仍记录全部设置成本。
    /// Retired mixed/amortization fields stay zero for metric compatibility; setup_ms remains real.
    double mixed_cpu_ms{}, mixed_gpu_ms{}, expected_saving_ms{}, setup_ms{};
};

/// 默认下界16MiB；长类为64MiB或四个GPU块的较大值。
/// Default floor is 16MiB; long class starts at max(64MiB, four GPU blocks).
/// 未知形状保持CPU偏好；饱和溢出仍可用，下界不得绕过。
/// Unknown shapes prefer CPU but permit saturation spill; the floor is never bypassed.
HashRoute classify_hash(std::uint64_t bytes, std::size_t floor, std::size_t gpu_block,
                        const DispatchEvidence& evidence);

/// 最慢GPU样本仍须比最快CPU快20%；拒绝零、负值及非有限测量。
/// Slowest GPU must beat fastest CPU by 20%; reject invalid or nonfinite timings.
bool stable_gpu_win(const std::array<double, 3>& cpu, const std::array<double, 3>& gpu);

/// 用独立GPU缓冲校准一块和四块输入，CPU按cpu_block_bytes实际切分。
/// Probe one/four GPU blocks; CPU uses its actual update size, zero means the whole input.
/// 调用方独占这两个后端及缓冲；完整摘要不一致直接抛错。
/// Exclusively own these backends/buffer; full digest mismatches throw.
/// 两秒软期限不能中断驱动；超时结果不标记calibration_complete。
/// The soft deadline cannot interrupt a driver; expiry leaves calibration incomplete.
DispatchEvidence
calibrate_dispatch(Compute& cpu, Compute& gpu, std::span<std::byte> scratch,
                   std::chrono::milliseconds budget = std::chrono::milliseconds(2000),
                   std::size_t cpu_block_bytes = 0);
} // namespace same::detail
