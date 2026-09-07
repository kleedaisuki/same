#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>

namespace same {
class ComputeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
using Digest = std::array<std::uint8_t, 32>;
// One instance per worker. Input spans are bounded by the configured block size.
// 每个工作线程独占实例，输入受配置的块大小限制。
class Hasher {
public:
    virtual ~Hasher() = default;
    virtual void update(std::span<const std::byte> bytes) = 0;
    virtual Digest finish() = 0;
};
class Compute {
public:
    virtual ~Compute() = default;
    virtual std::unique_ptr<Hasher> hasher() = 0;
    virtual bool equal(std::span<const std::byte> a, std::span<const std::byte> b) = 0;
    virtual std::string name() const = 0;
};
std::unique_ptr<Compute> make_cpu_compute();
// Null when CUDA cannot be initialized. Per-worker device allocation must fit budget.
// CUDA 不可用时返回空；每个工作线程的显存分配不得超过预算。
std::unique_ptr<Compute> try_cuda_compute(std::size_t block_bytes, std::size_t device_budget);
std::string hex_digest(const Digest& digest);
} // namespace same
