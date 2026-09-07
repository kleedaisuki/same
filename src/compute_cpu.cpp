#include "same/compute.hpp"
#include <algorithm>
#include <blake3.h>
namespace same {
namespace {
/// 官方 BLAKE3 实现的独占状态适配器。 / Exclusive-state adapter for the official BLAKE3
/// implementation.
class CpuHasher final : public Hasher {
    /// 保存已吸收输入的增量状态。 / Incremental state of absorbed input.
    blake3_hasher state_{};

public:
    /// 初始化非密钥模式。 / Initialize unkeyed hashing.
    CpuHasher() {
        blake3_hasher_init(&state_);
    }
    /// 吸收输入；避免向 C 接口传入空 span 的空指针。 / Absorb input, avoiding a null empty-span
    /// pointer in the C API.
    void update(std::span<const std::byte> bytes) override {
        if (!bytes.empty())
            blake3_hasher_update(&state_, bytes.data(), bytes.size());
    }
    /// 非破坏性地提取前 32 字节输出。 / Non-destructively extract the first 32 output bytes.
    Digest finish() override {
        Digest result{};
        blake3_hasher_finalize(&state_, result.data(), result.size());
        return result;
    }
};
/// 无共享临时状态的 CPU 计算实现。 / CPU implementation without shared scratch state.
class CpuCompute final : public Compute {
public:
    /// 为每个摘要创建独立状态。 / Allocate independent state for each digest.
    std::unique_ptr<Hasher> hasher() override {
        return std::make_unique<CpuHasher>();
    }
    /// 四迭代器重载同时校验长度与内容。 / The four-iterator overload checks both length and
    /// content.
    bool equal(std::span<const std::byte> a, std::span<const std::byte> b) override {
        return std::equal(a.begin(), a.end(), b.begin(), b.end());
    }
    /// 标识实际采用的后端。 / Identify the active backend.
    std::string name() const override {
        return "cpu";
    }
};
} // namespace
/// 构造 CPU 后端，不申请设备资源。 / Construct a CPU backend without device resources.
std::unique_ptr<Compute> make_cpu_compute() {
    return std::make_unique<CpuCompute>();
}
/// 高半字节在前，保持摘要的字节顺序。 / Emit high nibbles first while preserving digest byte order.
std::string hex_digest(const Digest& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(64, '0');
    for (std::size_t i = 0; i < digest.size(); ++i) {
        result[i * 2] = digits[digest[i] >> 4];
        result[i * 2 + 1] = digits[digest[i] & 15];
    }
    return result;
}
} // namespace same
