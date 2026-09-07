#include "blake3_scalar.hpp"
#include "same/compute.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <cuda_runtime.h>
#include <limits>
#include <stdexcept>
#include <vector>
namespace same {
namespace {
/// 将 CUDA 返回码统一映射到可降级异常。 / Map CUDA status codes to fallback-eligible exceptions.
void check(cudaError_t status) {
    if (status != cudaSuccess)
        throw ComputeError(std::string("CUDA: ") + cudaGetErrorString(status));
}
/// 每线程处理一个完整 1024 字节叶块，输出八字链值；first 保持全局块索引。 / One full 1024-byte
/// chunk per thread; emit eight-word values using global indices from first.
__global__ void leaf_kernel(const unsigned char* input, std::uint32_t* cvs, std::size_t count,
                            std::uint64_t first) {
    auto i = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i < count)
        b3::compress(b3::chunk(input + i * 1024, 1024, first + i), cvs + i * 8);
}
/// 仅以一个线程启动，保留最终叶块 Output 以便主机应用 ROOT。 / Launch with exactly one thread and
/// retain final leaf Output for host ROOT processing.
__global__ void final_kernel(const unsigned char* input, unsigned length, std::uint64_t index,
                             b3::Output* output) {
    *output = b3::chunk(input, length, index);
}
/// 网格步进覆盖全部字节；不相等标志只能以原子操作从 0 变为 1。 / Grid-stride traversal covers all
/// bytes; atomics only change the mismatch flag from 0 to 1.
__global__ void equal_kernel(const unsigned char* a, const unsigned char* b, std::size_t length,
                             int* different) {
    auto i = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    auto stride = static_cast<std::size_t>(gridDim.x) * blockDim.x;
    for (; i < length; i += stride)
        if (a[i] != b[i])
            atomicExch(different, 1);
}
/**
 * @brief 单个工作线程独占的 CUDA 暂存区。 / CUDA scratch storage exclusive to one worker thread.
 * Compute 与 Hasher 共享所有权但不允许并发访问；最后一个所有者析构时同步并释放。
 * Compute and Hasher share ownership, not concurrent access; the last owner synchronizes and
 * releases resources. allocate 与构造分离，让部分分配失败时也可由析构清理。 Allocation is separate
 * from construction so destruction cleans up partial allocation failures.
 */
struct Device {
    /// 两份输入设备缓冲，均含 capacity 字节。 / Two device input buffers, each capacity bytes.
    unsigned char *a = nullptr, *b = nullptr;
    /// 批量叶节点链值，每 1024 输入字节占 32 字节。 / Batched leaf chaining values: 32 bytes per
    /// 1024 input bytes.
    std::uint32_t* cvs = nullptr;
    /// 最终叶块的未压缩输出描述。 / Uncompressed final-leaf output descriptor.
    b3::Output* output = nullptr;
    /// 比较内核共享的原子不相等标志。 / Atomic mismatch flag shared by comparison threads.
    int* different = nullptr;
    /// 显式非阻塞流；所有传输和内核按流顺序提交。 / Explicit nonblocking stream ordering all
    /// transfers and kernels.
    cudaStream_t stream = nullptr;
    /// 输入容量，至少 1024 且为 1024 的倍数。 / Input capacity, at least 1024 and a multiple of
    /// 1024.
    std::size_t capacity;
    /// 同步后按输入顺序供主机归并的链值副本。 / Host copies reduced in input order after
    /// synchronization.
    std::vector<std::array<std::uint32_t, 8>> host_cvs;
    /// 仅分配主机容器；设备申请由 allocate 完成。 / Allocate only the host container; allocate
    /// handles device storage.
    explicit Device(std::size_t cap) : capacity(cap), host_cvs(cap / 1024) {}
    /// 清理前等待在途操作；析构不传播 CUDA 错误。 / Wait for in-flight work before cleanup;
    /// destruction does not propagate CUDA errors.
    ~Device() {
        if (stream)
            cudaStreamSynchronize(stream);
        cudaFree(a);
        cudaFree(b);
        cudaFree(cvs);
        cudaFree(output);
        cudaFree(different);
        if (stream)
            cudaStreamDestroy(stream);
    }
    /// 在零初始化句柄上逐项申请，供析构处理部分失败。 / Allocate into zero-initialized handles so
    /// destruction handles partial failure.
    void allocate() {
        check(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
        check(cudaMalloc(reinterpret_cast<void**>(&a), capacity));
        check(cudaMalloc(reinterpret_cast<void**>(&b), capacity));
        check(cudaMalloc(reinterpret_cast<void**>(&cvs), host_cvs.size() * 32));
        check(cudaMalloc(reinterpret_cast<void**>(&output), sizeof(b3::Output)));
        check(cudaMalloc(reinterpret_cast<void**>(&different), sizeof(int)));
    }
    /// 检查启动错误并等待流完成，使主机结果和输入复用安全。 / Check launch errors and await
    /// completion for safe host results and input reuse.
    void sync() {
        check(cudaGetLastError());
        check(cudaStreamSynchronize(stream));
    }
};
/// GPU 计算叶节点、CPU 维护有界二叉树的增量摘要。 / Incremental digest with GPU leaves and a
/// bounded CPU binary tree.
class CudaHasher final : public Hasher {
    /// 共享生命周期的工作线程私有暂存区，不提供互斥。 / Worker-private scratch with shared
    /// lifetime, without mutual exclusion.
    std::shared_ptr<Device> device_;
    /// 最后一个叶块保持未提交，即使恰好满 1024 字节。 / Keep the final chunk uncommitted even when
    /// exactly 1024 bytes full.
    std::array<unsigned char, 1024> pending_{};
    /// 待处理字节数及当前栈深度。 / Pending byte count and current stack depth.
    std::size_t length_ = 0, depth_ = 0;
    /// 已提交完整叶块数，同时作为下一个叶块的索引。 / Committed full-chunk count and next chunk
    /// index.
    std::uint64_t chunks_ = 0;
    /// 已完成子树栈；高度对应 chunks_ 的置位，空间随计数位数有界。 / Completed subtree stack;
    /// heights follow set bits of chunks_, bounded by counter width.
    std::array<std::array<std::uint32_t, 8>, 64> stack_{};
    /// 按二进制进位归并等高子树，保持左旧右新顺序。 / Merge equal-height subtrees like binary
    /// carries, preserving old-left/new-right order.
    void push(std::array<std::uint32_t, 8> cv) {
        auto total = ++chunks_;
        while ((total & 1) == 0) {
            b3::compress(b3::parent(stack_[--depth_].data(), cv.data()), cv.data());
            total >>= 1;
        }
        stack_[depth_++] = cv;
    }
    /// count 必须在 [1, capacity / 1024]；同步后按顺序压栈。 / count must be in [1, capacity /
    /// 1024]; synchronize then push in order.
    void leaves(const unsigned char* bytes, std::size_t count) {
        auto& d = *device_;
        check(cudaMemcpyAsync(d.a, bytes, count * 1024, cudaMemcpyHostToDevice, d.stream));
        leaf_kernel<<<static_cast<unsigned>((count + 127) / 128), 128, 0, d.stream>>>(
            d.a, d.cvs, count, chunks_);
        check(cudaMemcpyAsync(d.host_cvs.data(), d.cvs, count * 32, cudaMemcpyDeviceToHost,
                              d.stream));
        d.sync();
        for (std::size_t i = 0; i < count; ++i)
            push(d.host_cvs[i]);
    }

public:
    /// 新建空摘要并延长暂存区生命周期。 / Start an empty digest and retain scratch lifetime.
    explicit CudaHasher(std::shared_ptr<Device> device) : device_(std::move(device)) {}
    /// 分批吸收任意长度输入，仅提交确定不是最终块的完整叶块。 / Absorb input in batches, committing
    /// only full chunks known not to be final.
    void update(std::span<const std::byte> bytes) override {
        auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
        auto size = bytes.size();
        // Keep the final chunk uncommitted: ROOT applies to its output, not its CV.
        // 最后一个块保持未提交状态：ROOT 作用于输出结构而非链值。
        while (size) {
            if (length_ == 1024) {
                leaves(pending_.data(), 1);
                length_ = 0;
            }
            if (length_ == 0 && size > 1024) {
                auto count = std::min((size - 1) / 1024, device_->capacity / 1024);
                leaves(data, count);
                data += count * 1024;
                size -= count * 1024;
                continue;
            }
            auto n = std::min(size, 1024 - length_);
            std::memcpy(pending_.data() + length_, data, n);
            length_ += n;
            data += n;
            size -= n;
        }
    }
    /// 不修改摘要栈；由最终叶块向左归并，再以 ROOT 输出小端 32 字节。 / Preserve the stack, fold
    /// left subtrees into the final leaf, then emit 32 little-endian ROOT bytes.
    Digest finish() override {
        auto& d = *device_;
        if (length_)
            check(cudaMemcpyAsync(d.a, pending_.data(), length_, cudaMemcpyHostToDevice, d.stream));
        final_kernel<<<1, 1, 0, d.stream>>>(d.a, static_cast<unsigned>(length_), chunks_, d.output);
        b3::Output output{};
        check(cudaMemcpyAsync(&output, d.output, sizeof(output), cudaMemcpyDeviceToHost, d.stream));
        d.sync();
        std::array<std::uint32_t, 8> cv{};
        for (auto depth = depth_; depth; --depth) {
            b3::compress(output, cv.data());
            output = b3::parent(stack_[depth - 1].data(), cv.data());
        }
        b3::compress(output, cv.data(), true);
        Digest result{};
        for (std::size_t i = 0; i < result.size(); ++i)
            result[i] = static_cast<std::uint8_t>(cv[i / 4] >> (8 * (i % 4)));
        return result;
    }
};
/// 单工作线程同步接口；摘要与比较复用同一设备缓冲。 / Single-worker synchronous interface sharing
/// device buffers between hashing and comparison.
class CudaCompute final : public Compute {
    /// 共享生命周期的工作线程私有暂存区，不提供互斥。 / Worker-private scratch with shared
    /// lifetime, without mutual exclusion.
    std::shared_ptr<Device> device_;

public:
    /// 接管已成功申请的设备资源共享所有权。 / Retain shared ownership of successfully allocated
    /// device resources.
    explicit CudaCompute(std::shared_ptr<Device> device) : device_(std::move(device)) {}
    /// 创建独立树状态，但共享设备暂存区，不能并行调用。 / Create independent tree state but shared
    /// scratch; calls must not overlap.
    std::unique_ptr<Hasher> hasher() override {
        return std::make_unique<CudaHasher>(device_);
    }
    /// 报告实际 CUDA 后端。 / Report the actual CUDA backend.
    std::string name() const override {
        return "cuda";
    }
    /// 先比较长度，再按设备容量分批精确比较；空输入直接相等。 / Check lengths, then compare exact
    /// bytes in capacity-sized batches; empty inputs match.
    bool equal(std::span<const std::byte> a, std::span<const std::byte> b) override {
        if (a.size() != b.size())
            return false;
        auto& d = *device_;
        for (std::size_t offset = 0; offset < a.size();) {
            auto size = std::min(d.capacity, a.size() - offset);
            check(cudaMemcpyAsync(d.a, a.data() + offset, size, cudaMemcpyHostToDevice, d.stream));
            check(cudaMemcpyAsync(d.b, b.data() + offset, size, cudaMemcpyHostToDevice, d.stream));
            check(cudaMemsetAsync(d.different, 0, sizeof(int), d.stream));
            auto blocks = static_cast<unsigned>(std::min<std::size_t>((size + 255) / 256, 65535));
            equal_kernel<<<blocks, 256, 0, d.stream>>>(d.a, d.b, size, d.different);
            int different = 0;
            check(cudaMemcpyAsync(&different, d.different, sizeof(int), cudaMemcpyDeviceToHost,
                                  d.stream));
            d.sync();
            if (different)
                return false;
            offset += size;
        }
        return true;
    }
};
} // namespace
/// 按每块 2080 字节设备负载预算探测 CUDA；分配失败返回空以便 CPU 降级。 / Probe CUDA using 2080
/// device payload bytes per chunk; allocation failure returns null for CPU fallback.
std::unique_ptr<Compute> try_cuda_compute(std::size_t block_bytes, std::size_t device_budget) {
    constexpr auto overhead = sizeof(b3::Output) + sizeof(int);
    // 2 input bytes + 1/32 CV byte per byte; allocation accounting excludes driver context.
    // 每字节两份输入及 1/32 字节链值；预算不包含驱动上下文。
    if (block_bytes < 1024 || device_budget < overhead + 2080)
        return {};
    auto chunks = std::min(block_bytes / 1024, (device_budget - overhead) / 2080);
    chunks = std::min<std::size_t>(chunks, 65535 * 128);
    int count = 0;
    if (cudaGetDeviceCount(&count) != cudaSuccess || count == 0) {
        cudaGetLastError();
        return {};
    }
    try {
        auto device = std::make_shared<Device>(chunks * 1024);
        device->allocate();
        return std::make_unique<CudaCompute>(std::move(device));
    } catch (const ComputeError&) {
        cudaGetLastError();
        return {};
    }
}
} // namespace same
