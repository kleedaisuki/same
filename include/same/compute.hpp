#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>

namespace same {
/// 可触发整项操作 CPU 重试的后端错误。 / Backend failure eligible for whole-operation CPU retry.
class ComputeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
/// 非密钥模式 BLAKE3 的 256 位摘要，按输出字节序存储。 / Unkeyed BLAKE3 digest in output byte
/// order.
using Digest = std::array<std::uint8_t, 32>;
/**
 * @brief 工作线程独占的增量摘要状态。 / Worker-exclusive incremental digest state.
 * 同一后端及其摘要对象不得并发调用；输入通常由配置的块大小限制。
 * Do not call a backend and its hashers concurrently; input normally follows the configured block
 * size.
 * @code
 * auto compute = make_cpu_compute();
 * auto hash = compute->hasher();
 * hash->update(bytes);
 * auto digest = hash->finish();
 * @endcode
 */
class Hasher {
public:
    /// 释放摘要状态；不隐式完成摘要。 / Release state without implicitly finalizing.
    virtual ~Hasher() = default;
    /// 按顺序追加字节；空输入不改变状态。 / Append bytes in order; empty input is a no-op.
    virtual void update(std::span<const std::byte> bytes) = 0;
    /// 返回当前输入摘要，不重置状态；计算错误抛出 ComputeError。 / Digest current input without
    /// reset; backend errors throw ComputeError.
    virtual Digest finish() = 0;
};
/// 工作线程私有的同步计算后端；调用返回后输入可重用。 / Worker-private synchronous backend; input
/// is reusable on return.
class Compute {
public:
    /// 释放后端持有的资源。 / Release resources owned by the backend.
    virtual ~Compute() = default;
    /// 创建空输入状态；摘要对象可能共享后端临时缓冲。 / Create empty state; hashers may share
    /// backend scratch buffers.
    virtual std::unique_ptr<Hasher> hasher() = 0;
    /// 精确比较长度和所有字节，而非仅比较摘要。 / Compare lengths and every byte, not merely
    /// digests.
    virtual bool equal(std::span<const std::byte> a, std::span<const std::byte> b) = 0;
    /// 返回诊断使用的稳定后端名。 / Return the stable backend name used in diagnostics.
    virtual std::string name() const = 0;
};
/// 创建无需设备初始化的 CPU 后端。 / Create a CPU backend without device initialization.
std::unique_ptr<Compute> make_cpu_compute();
/**
 * @brief 尝试创建 CUDA 后端；不可用或预算不足时返回空。 / Try CUDA; return null if unavailable or
 * the budget is insufficient.
 * @param block_bytes 请求的单缓冲容量。 / Requested capacity of one input buffer.
 * @param device_budget 当前工作线程的显式设备分配预算，不含驱动上下文。 / Per-worker explicit
 * allocation budget, excluding driver context. CPU-only 构建保留相同接口并始终返回空。 / CPU-only
 * builds preserve this interface and always return null.
 */
std::unique_ptr<Compute> try_cuda_compute(std::size_t block_bytes, std::size_t device_budget);
/// 转换为固定 64 位小写十六进制字符串。 / Encode as exactly 64 lowercase hexadecimal characters.
std::string hex_digest(const Digest& digest);
} // namespace same
