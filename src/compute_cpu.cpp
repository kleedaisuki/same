#include "same/compute.hpp"
#include <algorithm>
#include <blake3.h>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#elif defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
namespace same {
namespace {
#if defined(__linux__) && (defined(__aarch64__) || defined(__arm__))
/// 限量读取公开 CPU 型号字段，不采集序列号或机器 ID。 / Read bounded public CPU model fields,
/// never serial numbers or machine IDs. Preserve heterogeneous core identities without core counts.
std::string arm_cpu_name() {
    std::ifstream file("/proc/cpuinfo", std::ios::binary);
    std::string bytes(256 * 1024, '\0');
    file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(file.gcount()));
    std::istringstream input(bytes);
    std::string line;
    std::set<std::string> models;
    while (std::getline(input, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos || line.size() > 1024)
            continue;
        auto key = line.substr(0, colon);
        key.erase(key.find_last_not_of(" \t") + 1);
        if (key == "CPU implementer" || key == "CPU architecture" || key == "CPU variant" ||
            key == "CPU part" || key == "CPU revision" || key == "model name")
            models.insert(line);
    }
    // 没有可区分芯片的字段时保持未知，调用方禁止跨运行重用。 / Keep unknown without chip fields;
    // callers must disable cross-run reuse for unknown identity.
    bool chip = false;
    std::string result;
    for (const auto& model : models) {
        chip = chip || model.starts_with("CPU part") || model.starts_with("model name");
        result += model + "\n";
    }
    return chip ? result : std::string{};
}
#endif
/// 廉价且缓存的硬件身份；不推测带宽。 / Cheap cached hardware identity without bandwidth guesses.
DeviceProfile cpu_profile() {
    DeviceProfile p;
    p.backend = BackendKind::cpu;
    p.hardware_threads = std::thread::hardware_concurrency();
    p.unified_memory = true;
    p.implementation_version =
        std::string("same-blake3-cpu-v1/") + blake3_version() + "/" + SAME_BLAKE3_BUILD_ID;
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
    p.architecture = sizeof(void*) == 8 ? "x86_64" : "x86";
    auto cpuid = [](unsigned leaf, unsigned* r) {
#if defined(_MSC_VER)
        int values[4];
        __cpuidex(values, static_cast<int>(leaf), 0);
        std::memcpy(r, values, sizeof(values));
#else
        __cpuid_count(leaf, 0, r[0], r[1], r[2], r[3]);
#endif
    };
    unsigned r[4]{};
    cpuid(0, r);
    char vendor[13]{};
    std::memcpy(vendor, r + 1, 4);
    std::memcpy(vendor + 4, r + 3, 4);
    std::memcpy(vendor + 8, r + 2, 4);
    p.vendor = vendor;
    cpuid(0x80000000u, r);
    if (r[0] >= 0x80000004u) {
        char brand[49]{};
        for (unsigned i = 0; i < 3; ++i) {
            cpuid(0x80000002u + i, r);
            std::memcpy(brand + i * 16, r, 16);
        }
        p.device_name = brand;
    }
#elif defined(_M_ARM64) || defined(__aarch64__)
    p.architecture = "aarch64";
#elif defined(__arm__) || defined(_M_ARM)
    p.architecture = "arm";
#else
    p.architecture = "unknown";
#endif
#if defined(__linux__) && (defined(__aarch64__) || defined(__arm__))
    p.device_name = arm_cpu_name();
#endif
#ifdef __APPLE__
    char name[256]{};
    std::size_t size = sizeof(name);
    if (sysctlbyname("machdep.cpu.brand_string", name, &size, nullptr, 0) == 0)
        p.device_name = name;
#endif
    return p;
}
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
    /// 构造期间触发缓存，热路径不再查询。 / Warm identity during construction, not the hot path.
    CpuCompute() {
        (void)profile();
    }
    /// 全进程只查询一次身份。 / Probe identity once per process.
    const DeviceProfile& profile() const override {
        static const auto p = cpu_profile();
        return p;
    }
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
    /// 返回强类型 CPU 身份。 / Return the typed CPU identity.
    BackendKind kind() const override {
        return BackendKind::cpu;
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
