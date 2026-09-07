#pragma once
#include "same/compute.hpp"
#include <array>
#include <chrono>
#include <span>

namespace same::detail {
/// 自动选择的保守证据，不是所有文件大小/并发度的性能保证。
/// Conservative auto-dispatch evidence, not a guarantee for every size or concurrency.
struct DispatchEvidence {
    /// 显式、延迟探测、CPU、GPU、失败五种互斥状态。
    /// Mutually exclusive explicit, deferred, CPU, GPU, and failed-probe decisions.
    enum class Decision { untested, deferred, cpu, gpu, failed } decision{Decision::untested};
    /// 实际块和八块流的 CPU/GPU 中位毫秒，包含摘要创建、传输及完成。
    /// Median CPU/GPU milliseconds for one block and eight blocks, including
    /// creation/transfers/finish.
    double cpu_block_ms{}, gpu_block_ms{}, cpu_stream_ms{}, gpu_stream_ms{};
    /// 串行校准时间（含预热），属于 scan 中的 setup_ms，不是额外可相加阶段。
    /// Serial calibration including warmup; part of scan setup_ms, not an additional phase.
    double elapsed_ms{};
    /// 实际混合池样本、预测待处理批次节省及全部设置费用。
    /// Actual mixed-pool samples, predicted pending-batch savings and total setup cost.
    double mixed_cpu_ms{}, mixed_gpu_ms{}, expected_saving_ms{}, setup_ms{};
};

/// 每一对样本都须至少快 20%，拒绝零、负值及非有限测量。
/// Every sample pair must win by at least 20%; reject zero, negative or nonfinite timings.
bool stable_gpu_win(const std::array<double, 3>& cpu, const std::array<double, 3>& gpu);

/** 使用实际工作缓冲区与后端校准，复用输入八次而不分配八倍内存。
 * Calibrate real worker buffers/backends; repeat input eight times without allocating eight
 * buffers. 调用方独占后端和缓冲区；修改输入，不读取用户文件。摘要不一致属于错误，不隐藏。 Call
 * in an exclusive idle phase; mutates scratch, never reads user files. Digest mismatch is an error.
 * 软预算在完整操作后检查，超时选 CPU；无法中断阻塞的驱动调用。
 * Check the soft budget after complete operations and choose CPU on expiry; driver calls cannot be
 * interrupted.
 */
DispatchEvidence
calibrate_dispatch(Compute& cpu, Compute& gpu, std::span<std::byte> scratch,
                   std::chrono::milliseconds budget = std::chrono::milliseconds(2000));
} // namespace same::detail
