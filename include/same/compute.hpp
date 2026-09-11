#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>

namespace same {
/// 计算设备类别；独显与核显不能共用布尔身份。 / Device kinds distinguish discrete and integrated
/// GPUs.
enum class BackendKind { cpu, cuda, igpu };

/// 固定诊断名称；不分配内存。 / Stable diagnostic name without allocation.
constexpr const char* backend_name(BackendKind kind) noexcept {
    switch (kind) {
    case BackendKind::cpu:
        return "cpu";
    case BackendKind::cuda:
        return "cuda";
    case BackendKind::igpu:
        return "igpu";
    }
    return "cpu";
}

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
    /**
     * @brief 可选地准备调用方拥有的稳定输入缓冲；默认无操作。 / Optionally prepare a caller-owned
     * stable input buffer; the default implementation does nothing.
     * CUDA 对非空范围仅注册一次，相同范围重复调用幂等；不同范围抛 ComputeError。
     * CUDA registers one nonempty range once; repeated identical ranges are idempotent, while a
     * different range throws ComputeError. Empty ranges are no-ops. Registration failure also
     * throws ComputeError, allowing the caller to replace this backend with CPU safely.
     * 空范围无操作；注册失败同样抛 ComputeError，供调用方安全替换成 CPU 后端。
     * 缓冲地址和容量必须保持稳定且存活至本 Compute 及其所有 Hasher 都析构；不得 resize、move
     * 或释放其存储。可在同步 update 返回后改写内容。无需为未注册输入调用此方法。
     * Storage must remain stable and alive until this Compute AND all its Hashers are destroyed;
     * do not resize, move, or free it. Contents may change after synchronous update returns.
     * Unregistered inputs remain supported. Call only when no backend operation is in flight.
     * 只能在没有后端操作进行时调用；不增加额外输入容量或转移缓冲所有权。
     * @code
     * std::vector<std::byte> input(block_bytes); // Must outlive compute and hashers.
     * auto compute = try_cuda_compute(block_bytes, budget);
     * if (!compute) compute =
     * make_cpu_compute();
     * compute->prepare_input(input); auto hasher = compute->hasher();
     * hasher->update(input);
     * @endcode
     */
    virtual void prepare_input(std::span<std::byte>) {}
    /// 创建空输入状态；摘要对象可能共享后端临时缓冲。 / Create empty state; hashers may share
    /// backend scratch buffers.
    virtual std::unique_ptr<Hasher> hasher() = 0;
    /// 精确比较长度和所有字节，而非仅比较摘要。 / Compare lengths and every byte, not merely
    /// digests.
    virtual bool equal(std::span<const std::byte> a, std::span<const std::byte> b) = 0;
    /// 返回诊断使用的稳定后端名。 / Return the stable backend name used in diagnostics.
    virtual std::string name() const = 0;
    /// 强类型路由身份；默认适配现有测试后端，生产后端直接覆盖。
    /// Typed routing identity; the default adapts named test backends, production overrides it.
    virtual BackendKind kind() const {
        const auto id = name();
        return id == "igpu" ? BackendKind::igpu
                            : (id == "cuda" ? BackendKind::cuda : BackendKind::cpu);
    }
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
/**
 * @brief 创建可选的 OpenCL 集成 GPU 后端。 / Create an optional OpenCL integrated-GPU backend.

 * * @param block_bytes 请求的最大输入块。 / Requested maximum input block.
 * @param device_budget
 * 显式 OpenCL 分配配额，须计入共享主存预算。
 * Explicit OpenCL allocation allowance, charged to
 * shared host memory.
 * 未构建、驱动缺失、设备不匹配或配额不足返回空；不选择 OpenCL CPU
 * 作为核显。
 * Return null when unbuilt, unavailable, ineligible or under-budget; never label a
 * CPU as iGPU.
 * 同步输入生命周期与 Compute 相同。 / Retains Compute's synchronous input lifetime
 * contract.
 */
std::unique_ptr<Compute> try_igpu_compute(std::size_t block_bytes, std::size_t device_budget);
/// 转换为固定 64 位小写十六进制字符串。 / Encode as exactly 64 lowercase hexadecimal characters.
std::string hex_digest(const Digest& digest);
} // namespace same
