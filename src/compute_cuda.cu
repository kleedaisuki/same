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
/// 完整叶按对齐小端字读取，避免通用逐字节组装的动态数组索引。 / Load aligned little-endian
/// words for full chunks, avoiding dynamic array indexing in the general byte assembly path.
__device__ void full_chunk(const unsigned char* input, std::uint64_t counter, std::uint32_t* cv) {
    b3::Output output{};
#pragma unroll
    for (int i = 0; i < 8; ++i)
        output.cv[i] = b3::initial(i);
    output.counter = counter;
    output.len = 64;
    const auto* words = reinterpret_cast<const std::uint32_t*>(input);
    for (unsigned block = 0; block < 16; ++block) {
#pragma unroll
        for (int i = 0; i < 16; ++i)
            output.block[i] = words[block * 16 + i];
        output.flags = (block == 0 ? 1 : 0) | (block == 15 ? 2 : 0);
        b3::compress(output, output.cv);
    }
#pragma unroll
    for (int i = 0; i < 8; ++i)
        cv[i] = output.cv[i];
}
/// 完整块归并 32 叶，末个非完整块回传独立叶；所有屏障分支在块内一致。 / Full blocks reduce
/// 32 leaves; the partial final block returns individual leaves. Barrier branches are
/// block-uniform.
__global__ void subtree_kernel(const unsigned char* input, std::uint32_t* cvs, std::uint64_t first,
                               std::size_t count) {
    // 九字步长使相邻叶映射到不同共享内存bank。 / Nine-word stride avoids shared bank conflicts.
    __shared__ std::uint32_t tree[32][9];
    const auto lane = threadIdx.x;
    const auto i = static_cast<std::size_t>(blockIdx.x) * 32 + lane;
    if (i < count)
        full_chunk(input + i * 1024, first + i, tree[lane]);
    __syncthreads();
    if (blockIdx.x < count / 32) {
        for (unsigned stride = 1; stride < 32; stride *= 2) {
            if (lane % (stride * 2) == 0)
                b3::compress(b3::parent(tree[lane], tree[lane + stride]), tree[lane]);
            __syncthreads();
        }
        if (lane < 8)
            cvs[static_cast<std::size_t>(blockIdx.x) * 8 + lane] = tree[0][lane];
    } else if (i < count) {
        for (unsigned word = 0; word < 8; ++word)
            cvs[(count / 32 + lane) * 8 + word] = tree[lane][word];
    }
}
/**
 * @brief 单个工作线程独占的 CUDA 暂存区。 / CUDA scratch storage exclusive to one worker thread.
 * Compute 与 Hasher 共享所有权但不允许并发访问；最后一个所有者析构时同步并释放。
 * Compute and Hasher share ownership, not concurrent access; the last owner synchronizes and
 * releases resources. allocate 与构造分离，让部分分配失败时也可由析构清理。 Allocation is separate
 * from construction so destruction cleans up partial allocation failures.
 */
struct Device {
    /// 输入设备缓冲，含 capacity 字节。 / Device input buffer containing capacity bytes.
    unsigned char* a = nullptr;
    /// 完整子树及至多 31 个尾叶链值。 / Full-subtree CVs plus up to 31 trailing leaf CVs.
    std::uint32_t* cvs = nullptr;
    /// 显式非阻塞流；所有传输和内核按流顺序提交。 / Explicit nonblocking stream ordering all
    /// transfers and kernels.
    cudaStream_t stream = nullptr;
    /// 调用方拥有的已注册范围，存活至最后一个 Device 所有者析构。 / Caller-owned registered
    /// range, alive until the final Device owner is destroyed; null means no registration.
    void* registered_input = nullptr;
    /// 注册字节数，用于同范围幂等检查。 / Registered bytes for identical-range idempotence.
    std::size_t registered_bytes = 0;
    /// 输入容量，至少 1024 且为 1024 的倍数。 / Input capacity, at least 1024 and a multiple of
    /// 1024.
    std::size_t capacity;
    /// 同步后按输入顺序供主机归并的链值副本。 / Host copies reduced in input order after
    /// synchronization.
    std::vector<std::array<std::uint32_t, 8>> host_cvs;
    /// 仅分配主机容器；设备申请由 allocate 完成。 / Allocate only the host container; allocate
    /// handles device storage.
    explicit Device(std::size_t cap)
        : capacity(cap), host_cvs(std::min(cap / 1024, cap / 32768 + 31)) {}
    /// 清理前等待在途操作；析构不传播 CUDA 错误。 / Wait for in-flight work before cleanup;
    /// destruction does not propagate CUDA errors.
    ~Device() {
        if (stream)
            cudaStreamSynchronize(stream);
        if (registered_input)
            cudaHostUnregister(registered_input);
        cudaFree(a);
        cudaFree(cvs);
        if (stream)
            cudaStreamDestroy(stream);
    }
    /// 在零初始化句柄上逐项申请，供析构处理部分失败。 / Allocate into zero-initialized handles so
    /// destruction handles partial failure.
    void allocate() {
        check(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
        check(cudaMalloc(reinterpret_cast<void**>(&a), capacity));
        check(cudaMalloc(reinterpret_cast<void**>(&cvs), host_cvs.size() * 32));
    }
    /// 只锁定现有缓冲，不增设暂存区；成功后才记录所有权以安全处理失败。 / Pin existing
    /// storage without staging; record registration only after success for safe failure cleanup.
    void prepare(std::span<std::byte> input) {
        if (input.empty())
            return;
        if (registered_input) {
            if (registered_input != input.data() || registered_bytes != input.size())
                throw ComputeError("CUDA: input buffer already prepared with a different range");
            return;
        }
        check(cudaHostRegister(input.data(), input.size(), cudaHostRegisterDefault));
        registered_input = input.data();
        registered_bytes = input.size();
    }
    /// 检查启动错误并等待流完成，使主机结果和输入复用安全。 / Check launch errors and await
    /// completion for safe host results and input reuse.
    void sync() {
        check(cudaGetLastError());
        check(cudaStreamSynchronize(stream));
    }
};
/// GPU 融合计算子树、CPU 维护有界二叉树的增量摘要。 / Incremental digest with GPU subtrees and a
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
    /// carries, preserving old-left/new-right order. count is an aligned power-of-two subtree.
    /// count 为对齐的二次幂子树叶数。
    void push(std::array<std::uint32_t, 8> cv, std::uint64_t count = 1) {
        chunks_ += count;
        auto total = chunks_ / count;
        while ((total & 1) == 0) {
            b3::compress(b3::parent(stack_[--depth_].data(), cv.data()), cv.data());
            total >>= 1;
        }
        stack_[depth_++] = cv;
    }
    /// 主机处理小批量及子树边界，避免微小传输。 / Handle small batches and subtree edges on
    /// the host, avoiding tiny transfers; index always matches the committed chunk count.
    void host_leaf(const unsigned char* bytes) {
        std::array<std::uint32_t, 8> cv{};
        b3::compress(b3::chunk(bytes, 1024, chunks_), cv.data());
        push(cv);
    }
    /// 前缀对齐后，完整子树和尾叶在同次 GPU 调用计算；微小批量留在主机。 / After prefix
    /// alignment, fuse full subtrees and trailing leaves in one launch; tiny batches stay on host.
    void leaves(const unsigned char* bytes, std::size_t count) {
        while (count && chunks_ % 32) {
            host_leaf(bytes);
            bytes += 1024;
            --count;
        }
        auto& d = *device_;
        const auto trees = count / 32;
        if (trees) {
            check(cudaMemcpyAsync(d.a, bytes, count * 1024, cudaMemcpyHostToDevice, d.stream));
            subtree_kernel<<<static_cast<unsigned>((count + 31) / 32), 32, 0, d.stream>>>(
                d.a, d.cvs, chunks_, count);
            check(cudaMemcpyAsync(d.host_cvs.data(), d.cvs, (trees + count % 32) * 32,
                                  cudaMemcpyDeviceToHost, d.stream));
            d.sync();
            for (std::size_t i = 0; i < trees; ++i)
                push(d.host_cvs[i], 32);
            for (std::size_t i = 0; i < count % 32; ++i)
                push(d.host_cvs[trees + i]);
            return;
        }
        for (std::size_t i = 0; i < count; ++i)
            host_leaf(bytes + i * 1024);
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
        // 最终叶已经在主机，无需单线程 GPU 往返。 / Final leaf is already host-resident.
        auto output = b3::chunk(pending_.data(), static_cast<unsigned>(length_), chunks_);
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
/// 单工作线程同步接口；摘要使用设备，比较保留在主机。 / Single-worker synchronous interface;
/// hashing uses device scratch and comparison stays on the host.
class CudaCompute final : public Compute {
    /// 共享生命周期的工作线程私有暂存区，不提供互斥。 / Worker-private scratch with shared
    /// lifetime, without mutual exclusion.
    std::shared_ptr<Device> device_;

public:
    /// 接管已成功申请的设备资源共享所有权。 / Retain shared ownership of successfully allocated
    /// device resources.
    explicit CudaCompute(std::shared_ptr<Device> device) : device_(std::move(device)) {}
    /// 注册稳定的调用方输入；Device 与存活的 Hasher 共同持有注册生命周期。 / Register stable
    /// caller input; Device and surviving Hashers jointly retain registration lifetime.
    void prepare_input(std::span<std::byte> input) override {
        device_->prepare(input);
    }
    /// 创建独立树状态，但共享设备暂存区，不能并行调用。 / Create independent tree state but shared
    /// scratch; calls must not overlap.
    std::unique_ptr<Hasher> hasher() override {
        return std::make_unique<CudaHasher>(device_);
    }
    /// 报告实际 CUDA 后端。 / Report the actual CUDA backend.
    std::string name() const override {
        return "cuda";
    }
    /// 主机已有两份输入，直接比较以避免两次 H2D 和同步。 / Compare host-resident inputs
    /// directly, avoiding two H2D transfers and synchronization; empty spans need no pointers.
    bool equal(std::span<const std::byte> a, std::span<const std::byte> b) override {
        return a.size() == b.size() &&
               (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
    }
};
} // namespace
/// 按每块 2080 字节设备负载预算探测 CUDA；分配失败返回空以便 CPU 降级。 / Probe CUDA using 2080
/// device payload bytes per chunk; allocation failure returns null for CPU fallback.
std::unique_ptr<Compute> try_cuda_compute(std::size_t block_bytes, std::size_t device_budget) {
    constexpr auto overhead = sizeof(b3::Output) + sizeof(int);
    // Preserve historical capacity selection for configured budgets; actual allocation is now
    // one input buffer plus subtree/tail CVs, below this conservative bound.
    // 保留旧预算容量语义；实际分配输入及子树/尾叶链值，低于该保守上界。
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
        // 在所属工作线程绑定当前设备的共享主上下文，不创建独立上下文或重置其他线程。
        // Bind this worker to the current device's shared primary context; never reset peers.
        int ordinal = 0;
        check(cudaGetDevice(&ordinal));
        check(cudaSetDevice(ordinal));
        auto device = std::make_shared<Device>(chunks * 1024);
        device->allocate();
        return std::make_unique<CudaCompute>(std::move(device));
    } catch (const ComputeError&) {
        cudaGetLastError();
        return {};
    }
}
} // namespace same
