/** @file
 * @brief 原生 OpenCL 内核的独立差分与生命周期回归。 / Native OpenCL kernel differential/lifetime
 * tests.
 */
#include "same/compute.hpp"
#include "same/detail/opencl_testing.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>
namespace {
/// 始终生效，不受 NDEBUG 影响。 / Always active, independent of NDEBUG.
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
/// 增量输入并验证非破坏性重复终结。 / Stream and verify repeatable non-destructive finish.
same::Digest hash(same::Compute& compute, std::span<const std::byte> bytes, std::size_t step) {
    auto h = compute.hasher();
    h->update({});
    for (std::size_t pos = 0; pos < bytes.size(); pos += step)
        h->update(bytes.subspan(pos, std::min(step, bytes.size() - pos)));
    auto digest = h->finish();
    require(digest == h->finish(), "repeat finish mismatch");
    return digest;
}
/// 覆盖块边界、非二次幂树和继续更新语义。 / Cover block boundaries, irregular trees and continued
/// updates.
void exercise(same::Compute& device) {
    auto cpu = same::make_cpu_compute();
    std::vector<std::byte> bytes(2 * 1024 * 1024 + 113);
    std::uint32_t random = 17;
    for (auto& b : bytes) {
        random = random * 1664525U + 1013904223U;
        b = static_cast<std::byte>(random >> 24);
    }
    require(same::hex_digest(hash(device, {}, 1)) ==
                "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262",
            "empty official vector");
    for (std::size_t size : {1U, 63U, 64U, 65U, 1023U, 1024U, 1025U, 2048U, 2049U, 3072U, 4097U,
                             65535U, 65536U, 65537U, 2097265U}) {
        auto input = std::span<const std::byte>(bytes).first(size);
        auto expected = hash(*cpu, input, size);
        for (std::size_t step : {137U, 1024U, 3917U, 65537U, 4194304U})
            require(hash(device, input, step) == expected, "CPU/OpenCL differential mismatch");
    }
    auto h = device.hasher();
    h->update(std::span<const std::byte>(bytes).first(1025));
    require(h->finish() == hash(*cpu, std::span<const std::byte>(bytes).first(1025), 7001),
            "partial finish");
    h->update(std::span<const std::byte>(bytes).subspan(1025));
    require(h->finish() == hash(*cpu, bytes, 7001), "update after finish");
    require(device.equal({}, {}), "empty equality");
    require(device.equal(bytes, bytes), "same equality");
    auto other = bytes;
    for (auto offset : {std::size_t{0}, std::size_t{1024}, bytes.size() - 1}) {
        other[offset] ^= std::byte{1};
        require(!device.equal(bytes, other), "different byte equality");
        other[offset] ^= std::byte{1};
    }
    require(!device.equal(bytes, std::span<const std::byte>(bytes).first(1024)),
            "different size equality");
    require(same::detail::opencl_kernel_submissions(device) > 0,
            "no native OpenCL kernel executed");
    std::cout << device.name() << " [" << same::detail::opencl_device_name(device) << "]"
              << ": native kernel batches=" << same::detail::opencl_kernel_submissions(device)
              << '\n';
}
} // namespace
/// 环境变量仅控制测试是否允许缺设备；生产选择不读取它们。 / Environment controls tests only, never
/// production selection.
int main() {
    try {
        require(!same::try_igpu_compute(65536, 0), "zero budget accepted");
        const bool required_test = std::getenv("SAME_REQUIRE_OPENCL_TEST_DEVICE") != nullptr;
        const bool required_igpu = std::getenv("SAME_REQUIRE_IGPU") != nullptr;
        auto device = required_test ? same::detail::try_opencl_test_compute(65536, 262144)
                                    : same::try_igpu_compute(65536, 262144);
        if (!device) {
            require(!required_test && !required_igpu, "required OpenCL device unavailable");
            std::cout << "OpenCL iGPU unavailable; optional runtime fallback passed\n";
            return 0;
        }
        require(device->kind() == same::BackendKind::igpu, "wrong backend kind");
        exercise(*device);
        auto survivor = device->hasher();
        std::vector<std::byte> bytes(100003, std::byte{37});
        survivor->update(bytes);
        device.reset();
        auto cpu = same::make_cpu_compute();
        require(survivor->finish() == hash(*cpu, bytes, 7001), "hasher lifetime mismatch");
        std::cout << "OpenCL differential and lifetime tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
