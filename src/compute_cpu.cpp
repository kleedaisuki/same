#include "same/compute.hpp"
#include <blake3.h>
#include <algorithm>
namespace same {
namespace {
class CpuHasher final : public Hasher {
    blake3_hasher state_{};
public:
    CpuHasher() { blake3_hasher_init(&state_); }
    void update(std::span<const std::byte> bytes) override {
        if (!bytes.empty()) blake3_hasher_update(&state_, bytes.data(), bytes.size());
    }
    Digest finish() override {
        Digest result{};
        blake3_hasher_finalize(&state_, result.data(), result.size());
        return result;
    }
};
class CpuCompute final : public Compute {
public:
    std::unique_ptr<Hasher> hasher() override { return std::make_unique<CpuHasher>(); }
    bool equal(std::span<const std::byte> a, std::span<const std::byte> b) override {
        return std::equal(a.begin(), a.end(), b.begin(), b.end());
    }
    std::string name() const override { return "cpu"; }
};
}
std::unique_ptr<Compute> make_cpu_compute() { return std::make_unique<CpuCompute>(); }
std::string hex_digest(const Digest& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(64, '0');
    for (std::size_t i = 0; i < digest.size(); ++i) {
        result[i * 2] = digits[digest[i] >> 4];
        result[i * 2 + 1] = digits[digest[i] & 15];
    }
    return result;
}
}
