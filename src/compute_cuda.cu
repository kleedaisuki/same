#include "same/compute.hpp"
#include "blake3_scalar.hpp"
#include <cuda_runtime.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>
namespace same {
namespace {
void check(cudaError_t status) {
    if (status != cudaSuccess) throw ComputeError(std::string("CUDA: ") + cudaGetErrorString(status));
}
__global__ void leaf_kernel(const unsigned char* input, std::uint32_t* cvs, std::size_t count, std::uint64_t first) {
    auto i = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i < count) b3::compress(b3::chunk(input + i * 1024, 1024, first + i), cvs + i * 8);
}
__global__ void final_kernel(const unsigned char* input, unsigned length, std::uint64_t index, b3::Output* output) {
    *output = b3::chunk(input, length, index);
}
__global__ void equal_kernel(const unsigned char* a, const unsigned char* b, std::size_t length, int* different) {
    auto i = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    auto stride = static_cast<std::size_t>(gridDim.x) * blockDim.x;
    for (; i < length; i += stride) if (a[i] != b[i]) atomicExch(different, 1);
}
struct Device {
    unsigned char *a = nullptr, *b = nullptr;
    std::uint32_t* cvs = nullptr;
    b3::Output* output = nullptr;
    int* different = nullptr;
    cudaStream_t stream = nullptr;
    std::size_t capacity;
    std::vector<std::array<std::uint32_t, 8>> host_cvs;
    explicit Device(std::size_t cap) : capacity(cap), host_cvs(cap / 1024) {}
    ~Device() {
        if (stream) cudaStreamSynchronize(stream);
        cudaFree(a); cudaFree(b); cudaFree(cvs); cudaFree(output); cudaFree(different);
        if (stream) cudaStreamDestroy(stream);
    }
    void allocate() {
        check(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
        check(cudaMalloc(reinterpret_cast<void**>(&a), capacity));
        check(cudaMalloc(reinterpret_cast<void**>(&b), capacity));
        check(cudaMalloc(reinterpret_cast<void**>(&cvs), host_cvs.size() * 32));
        check(cudaMalloc(reinterpret_cast<void**>(&output), sizeof(b3::Output)));
        check(cudaMalloc(reinterpret_cast<void**>(&different), sizeof(int)));
    }
    void sync() { check(cudaGetLastError()); check(cudaStreamSynchronize(stream)); }
};
class CudaHasher final : public Hasher {
    std::shared_ptr<Device> device_;
    std::array<unsigned char, 1024> pending_{};
    std::size_t length_ = 0, depth_ = 0;
    std::uint64_t chunks_ = 0;
    std::array<std::array<std::uint32_t, 8>, 64> stack_{};
    void push(std::array<std::uint32_t, 8> cv) {
        auto total = ++chunks_;
        while ((total & 1) == 0) {
            b3::compress(b3::parent(stack_[--depth_].data(), cv.data()), cv.data());
            total >>= 1;
        }
        stack_[depth_++] = cv;
    }
    void leaves(const unsigned char* bytes, std::size_t count) {
        auto& d = *device_;
        check(cudaMemcpyAsync(d.a, bytes, count * 1024, cudaMemcpyHostToDevice, d.stream));
        leaf_kernel<<<static_cast<unsigned>((count + 127) / 128), 128, 0, d.stream>>>(d.a, d.cvs, count, chunks_);
        check(cudaMemcpyAsync(d.host_cvs.data(), d.cvs, count * 32, cudaMemcpyDeviceToHost, d.stream));
        d.sync();
        for (std::size_t i = 0; i < count; ++i) push(d.host_cvs[i]);
    }
public:
    explicit CudaHasher(std::shared_ptr<Device> device) : device_(std::move(device)) {}
    void update(std::span<const std::byte> bytes) override {
        auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
        auto size = bytes.size();
        // Keep the final chunk uncommitted: ROOT applies to its output, not its CV.
        // 最后一个块保持未提交状态：ROOT 作用于输出结构而非链值。
        while (size) {
            if (length_ == 1024) { leaves(pending_.data(), 1); length_ = 0; }
            if (length_ == 0 && size > 1024) {
                auto count = std::min((size - 1) / 1024, device_->capacity / 1024);
                leaves(data, count); data += count * 1024; size -= count * 1024;
                continue;
            }
            auto n = std::min(size, 1024 - length_);
            std::memcpy(pending_.data() + length_, data, n);
            length_ += n; data += n; size -= n;
        }
    }
    Digest finish() override {
        auto& d = *device_;
        if (length_) check(cudaMemcpyAsync(d.a, pending_.data(), length_, cudaMemcpyHostToDevice, d.stream));
        final_kernel<<<1, 1, 0, d.stream>>>(d.a, static_cast<unsigned>(length_), chunks_, d.output);
        b3::Output output{};
        check(cudaMemcpyAsync(&output, d.output, sizeof(output), cudaMemcpyDeviceToHost, d.stream));
        d.sync();
        std::array<std::uint32_t, 8> cv{};
        for (auto depth = depth_; depth; --depth) {
            b3::compress(output, cv.data()); output = b3::parent(stack_[depth - 1].data(), cv.data());
        }
        b3::compress(output, cv.data(), true);
        Digest result{};
        for (std::size_t i = 0; i < result.size(); ++i) result[i] = static_cast<std::uint8_t>(cv[i / 4] >> (8 * (i % 4)));
        return result;
    }
};
class CudaCompute final : public Compute {
    std::shared_ptr<Device> device_;
public:
    explicit CudaCompute(std::shared_ptr<Device> device) : device_(std::move(device)) {}
    std::unique_ptr<Hasher> hasher() override { return std::make_unique<CudaHasher>(device_); }
    std::string name() const override { return "cuda"; }
    bool equal(std::span<const std::byte> a, std::span<const std::byte> b) override {
        if (a.size() != b.size()) return false;
        auto& d = *device_;
        for (std::size_t offset = 0; offset < a.size();) {
            auto size = std::min(d.capacity, a.size() - offset);
            check(cudaMemcpyAsync(d.a, a.data() + offset, size, cudaMemcpyHostToDevice, d.stream));
            check(cudaMemcpyAsync(d.b, b.data() + offset, size, cudaMemcpyHostToDevice, d.stream));
            check(cudaMemsetAsync(d.different, 0, sizeof(int), d.stream));
            auto blocks = static_cast<unsigned>(std::min<std::size_t>((size + 255) / 256, 65535));
            equal_kernel<<<blocks, 256, 0, d.stream>>>(d.a, d.b, size, d.different);
            int different = 0;
            check(cudaMemcpyAsync(&different, d.different, sizeof(int), cudaMemcpyDeviceToHost, d.stream));
            d.sync();
            if (different) return false;
            offset += size;
        }
        return true;
    }
};
}
std::unique_ptr<Compute> try_cuda_compute(std::size_t block_bytes, std::size_t device_budget) {
    constexpr auto overhead = sizeof(b3::Output) + sizeof(int);
    // 2 input bytes + 1/32 CV byte per byte; allocation accounting excludes driver context.
    // 每字节两份输入及 1/32 字节链值；预算不包含驱动上下文。
    if (block_bytes < 1024 || device_budget < overhead + 2080) return {};
    auto chunks = std::min(block_bytes / 1024, (device_budget - overhead) / 2080);
    chunks = std::min<std::size_t>(chunks, 65535 * 128);
    int count = 0;
    if (cudaGetDeviceCount(&count) != cudaSuccess || count == 0) { cudaGetLastError(); return {}; }
    try {
        auto device = std::make_shared<Device>(chunks * 1024);
        device->allocate();
        return std::make_unique<CudaCompute>(std::move(device));
    } catch (const ComputeError&) { cudaGetLastError(); return {}; }
}
}
