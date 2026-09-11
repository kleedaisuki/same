#include "blake3_opencl_kernel.hpp"
#include "blake3_scalar.hpp"
#include "same/compute.hpp"
#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>
#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif
namespace same {
namespace {
/// 所有错误均可触发整文件重试。 / Translate API errors into whole-file retry failures.
void check(cl_int e) {
    if (e != CL_SUCCESS)
        throw ComputeError("OpenCL error " + std::to_string(e));
}
/// 动态导入，不要求系统安装 OpenCL。 / Dynamic imports keep OpenCL optional at runtime.
struct Api {
#ifdef _WIN32
    /// 保留运行库句柄，后台驱动线程可能继续使用。 / Retain module for driver background threads.
    HMODULE module = LoadLibraryExW(L"OpenCL.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    /// 查找可选运行库入口。 / Resolve a runtime entry point.
    void* symbol(const char* n) {
        return reinterpret_cast<void*>(GetProcAddress(module, n));
    }
#else
#ifdef __APPLE__
    void* module =
        dlopen("/System/Library/Frameworks/OpenCL.framework/OpenCL", RTLD_NOW | RTLD_LOCAL);
#else
    void* module = dlopen("libOpenCL.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
    /// 查找可选运行库入口。 / Resolve a runtime entry point.
    void* symbol(const char* n) {
        return dlsym(module, n);
    }
#endif
#define ENTRY(n) decltype(&::n) n = nullptr
    ENTRY(clGetPlatformIDs);
    ENTRY(clGetDeviceIDs);
    ENTRY(clGetDeviceInfo);
    ENTRY(clCreateContext);
    ENTRY(clReleaseContext);
    ENTRY(clCreateProgramWithSource);
    ENTRY(clBuildProgram);
    ENTRY(clGetProgramBuildInfo);
    ENTRY(clReleaseProgram);
    ENTRY(clCreateCommandQueue);
    ENTRY(clReleaseCommandQueue);
    ENTRY(clCreateKernel);
    ENTRY(clReleaseKernel);
    ENTRY(clCreateBuffer);
    ENTRY(clReleaseMemObject);
    ENTRY(clSetKernelArg);
    ENTRY(clEnqueueWriteBuffer);
    ENTRY(clEnqueueNDRangeKernel);
    ENTRY(clEnqueueReadBuffer);
    ENTRY(clFinish);
#undef ENTRY
    /// 加载完整 1.2 接口；不完整运行库视为不可用。 / Require the complete used 1.2 API.
    Api() {
        if (!module)
            throw ComputeError("OpenCL runtime unavailable");
#define LOAD(n)                                                                                    \
    n = reinterpret_cast<decltype(n)>(symbol(#n));                                                 \
    if (!n)                                                                                        \
    throw ComputeError("Missing " #n)
        LOAD(clGetPlatformIDs);
        LOAD(clGetDeviceIDs);
        LOAD(clGetDeviceInfo);
        LOAD(clCreateContext);
        LOAD(clReleaseContext);
        LOAD(clCreateProgramWithSource);
        LOAD(clBuildProgram);
        LOAD(clGetProgramBuildInfo);
        LOAD(clReleaseProgram);
        LOAD(clCreateCommandQueue);
        LOAD(clReleaseCommandQueue);
        LOAD(clCreateKernel);
        LOAD(clReleaseKernel);
        LOAD(clCreateBuffer);
        LOAD(clReleaseMemObject);
        LOAD(clSetKernelArg);
        LOAD(clEnqueueWriteBuffer);
        LOAD(clEnqueueNDRangeKernel);
        LOAD(clEnqueueReadBuffer);
        LOAD(clFinish);
#undef LOAD
    }
    // 模块保留至进程退出，避免驱动后台线程卸载竞态。 / Retain module until process exit for driver
    // threads.
};
/// 每种选择策略共享编译程序；队列及内核不共享。 / Share compiled program, never queues/kernels.
struct Program {
    /// 共享动态入口生命周期。 / Shared dynamic entry point lifetime.
    std::shared_ptr<Api> api;
    /// 所选设备及引用计数的上下文和程序。 / Selected device and refcounted context/program.
    cl_device_id id = nullptr;
    cl_context context = nullptr;
    cl_program program = nullptr;
    /// 部分初始化也安全释放。 / Partial initialization is safely released.
    ~Program() {
        if (program)
            api->clReleaseProgram(program);
        if (context)
            api->clReleaseContext(context);
    }
    /// 按设备布尔能力查询。 / Read a device boolean capability.
    bool flag(cl_device_info key) {
        cl_bool v = CL_FALSE;
        check(api->clGetDeviceInfo(id, key, sizeof(v), &v, nullptr));
        return v == CL_TRUE;
    }
    /// 生产只接受统一内存 GPU；测试允许 CPU ICD。 / Production accepts unified-memory GPU only.
    void init(bool testing) {
        static auto shared_api = std::make_shared<Api>();
        api = shared_api;
        cl_uint n = 0;
        check(api->clGetPlatformIDs(0, nullptr, &n));
        std::vector<cl_platform_id> platforms(n);
        check(api->clGetPlatformIDs(n, platforms.data(), nullptr));
        for (auto p : platforms) {
            cl_uint count = 0;
            auto type = testing ? CL_DEVICE_TYPE_ALL : CL_DEVICE_TYPE_GPU;
            if (api->clGetDeviceIDs(p, type, 0, nullptr, &count) != CL_SUCCESS)
                continue;
            std::vector<cl_device_id> devices(count);
            check(api->clGetDeviceIDs(p, type, count, devices.data(), nullptr));
            for (auto d : devices) {
                id = d;
                if (flag(CL_DEVICE_AVAILABLE) && flag(CL_DEVICE_COMPILER_AVAILABLE) &&
                    flag(CL_DEVICE_ENDIAN_LITTLE) &&
                    (testing || flag(CL_DEVICE_HOST_UNIFIED_MEMORY)))
                    break;
                id = nullptr;
            }
            if (id)
                break;
        }
        if (!id)
            throw ComputeError("No suitable OpenCL device");
        cl_int e = 0;
        context = api->clCreateContext(nullptr, 1, &id, nullptr, nullptr, &e);
        check(e);
        const char* source = opencl_source;
        program = api->clCreateProgramWithSource(context, 1, &source, nullptr, &e);
        check(e);
        const auto status = api->clBuildProgram(program, 1, &id, "-cl-std=CL1.2", nullptr, nullptr);
        if (status != CL_SUCCESS) {
            std::size_t size = 0;
            api->clGetProgramBuildInfo(program, id, CL_PROGRAM_BUILD_LOG, 0, nullptr, &size);
            std::string log(std::min(size, std::size_t{65536}), '\0');
            if (size <= log.size() && size)
                api->clGetProgramBuildInfo(program, id, CL_PROGRAM_BUILD_LOG, size, log.data(),
                                           nullptr);
            throw ComputeError("OpenCL program build failed: " + log);
        }
    }
};
/// 初始化串行化，实际工作线程不持锁。 / Serialize initialization, not worker execution.
std::shared_ptr<Program> program_for(bool testing) {
    static std::mutex mutex;
    static std::shared_ptr<Program> production, test;
    std::lock_guard lock(mutex);
    auto& p = testing ? test : production;
    if (!p) {
        auto next = std::make_shared<Program>();
        next->init(testing);
        p = std::move(next);
    }
    return p;
}
/// 每工作线程拥有独立有界设备缓冲及队列。 / Per-worker bounded buffers and queue.
struct Device {
    /// 程序先于所有子资源存在。 / Program outlives child resources.
    std::shared_ptr<Program> p;
    /// 按顺序执行的私有队列、内核和缓冲。 / Private in-order queue, kernel, and buffers.
    cl_command_queue queue = nullptr;
    cl_kernel kernel = nullptr;
    cl_mem input = nullptr, output = nullptr;
    /// 已成功完成的内核批次。 / Successfully completed kernel batches.
    std::uint64_t submissions = 0;
    /// 输入容量和匹配的主机链值数组。 / Input capacity and matching host CV array.
    std::size_t capacity;
    std::vector<std::array<std::uint32_t, 8>> host_cvs;
    /// 仅分配主机数组，设备分配单独执行。 / Allocate host array before device resources.
    Device(std::shared_ptr<Program> program, std::size_t cap)
        : p(std::move(program)), capacity(cap), host_cvs(cap / 1024) {}
    /// 无借用主机内存的异步传输；退出仍收敛设备工作。 / No borrowed async host storage; drain
    /// device work.
    ~Device() {
        auto& a = *p->api;
        if (queue)
            a.clFinish(queue);
        if (input)
            a.clReleaseMemObject(input);
        if (output)
            a.clReleaseMemObject(output);
        if (kernel)
            a.clReleaseKernel(kernel);
        if (queue)
            a.clReleaseCommandQueue(queue);
    }
    /// 逐项分配便于 RAII 清理。 / Allocate incrementally for RAII cleanup.
    void allocate() {
        auto& a = *p->api;
        cl_int e = 0;
        queue = a.clCreateCommandQueue(p->context, p->id, 0, &e);
        check(e);
        kernel = a.clCreateKernel(p->program, "leaves", &e);
        check(e);
        input = a.clCreateBuffer(p->context, CL_MEM_READ_ONLY, capacity, nullptr, &e);
        check(e);
        output = a.clCreateBuffer(p->context, CL_MEM_WRITE_ONLY, host_cvs.size() * 32, nullptr, &e);
        check(e);
    }
    /// 阻塞复制保障调用方输入生命周期；不声称零拷贝或驱动挂起可取消。 / Blocking copies ensure
    /// host lifetime; neither zero-copy nor cancellation of a hung driver is promised.
    void run(const unsigned char* bytes, std::size_t count, std::uint64_t first) {
        auto& a = *p->api;
        cl_ulong counter = first;
        check(a.clEnqueueWriteBuffer(queue, input, CL_TRUE, 0, count * 1024, bytes, 0, nullptr,
                                     nullptr));
        check(a.clSetKernelArg(kernel, 0, sizeof(input), &input));
        check(a.clSetKernelArg(kernel, 1, sizeof(output), &output));
        check(a.clSetKernelArg(kernel, 2, sizeof(counter), &counter));
        check(a.clEnqueueNDRangeKernel(queue, kernel, 1, nullptr, &count, nullptr, 0, nullptr,
                                       nullptr));
        check(a.clEnqueueReadBuffer(queue, output, CL_TRUE, 0, count * 32, host_cvs.data(), 0,
                                    nullptr, nullptr));
        ++submissions;
    }
};
/// 设备叶计算及主机有界树归并。 / Device leaves and bounded host tree reduction.
class OpenclHasher final : public Hasher {
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
    /// 所有已确定非最终完整叶均由设备执行；主机按序归并。 / Execute every non-final full
    /// leaf on device, then merge in input order on the host.
    void leaves(const unsigned char* bytes, std::size_t count) {
        device_->run(bytes, count, chunks_);
        for (std::size_t i = 0; i < count; ++i)
            push(device_->host_cvs[i]);
    }

public:
    /// 新建空摘要并延长暂存区生命周期。 / Start an empty digest and retain scratch lifetime.
    explicit OpenclHasher(std::shared_ptr<Device> device) : device_(std::move(device)) {}
    /// 分批吸收任意长度输入，仅提交确定不是最终块的完整叶块。 / Absorb input in batches, committing
    /// only full chunks known not to be final.
    void update(std::span<const std::byte> bytes) override {
        if (bytes.size() > std::numeric_limits<std::uint64_t>::max() - (chunks_ * 1024 + length_))
            throw ComputeError("OpenCL: BLAKE3 input exceeds 64-bit byte length");
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
class OpenclCompute final : public Compute {
    /// 共享生命周期的工作线程私有暂存区，不提供互斥。 / Worker-private scratch with shared
    /// lifetime, without mutual exclusion.
    std::shared_ptr<Device> device_;

public:
    /// 接管已成功申请的设备资源共享所有权。 / Retain shared ownership of successfully allocated
    /// device resources.
    explicit OpenclCompute(std::shared_ptr<Device> device) : device_(std::move(device)) {}
    /// 创建独立树状态，但共享设备暂存区，不能并行调用。 / Create independent tree state but shared
    /// scratch; calls must not overlap.
    std::unique_ptr<Hasher> hasher() override {
        return std::make_unique<OpenclHasher>(device_);
    }
    /// 驱动报告的设备名，仅用于诊断。 / Driver-reported device name for diagnostics.
    std::string device_name() const {
        std::size_t size = 0;
        auto& p = *device_->p;
        check(p.api->clGetDeviceInfo(p.id, CL_DEVICE_NAME, 0, nullptr, &size));
        std::string name(size, '\0');
        check(p.api->clGetDeviceInfo(p.id, CL_DEVICE_NAME, size, name.data(), nullptr));
        if (!name.empty() && name.back() == '\0')
            name.pop_back();
        return name;
    }
    /// 完成内核计数，不含初始化自检。 / Completed kernels excluding startup validation.
    std::uint64_t submissions() const {
        return device_->submissions;
    }
    /// 强类型后端身份。 / Typed backend identity.
    BackendKind kind() const override {
        return BackendKind::igpu;
    }
    /// 稳定诊断标签。 / Stable diagnostic label.
    std::string name() const override {
        return "igpu";
    }
    /// 主机已有两份输入，直接比较以避免两次 H2D 和同步。 / Compare host-resident inputs
    /// directly, avoiding two H2D transfers and synchronization; empty spans need no pointers.
    bool equal(std::span<const std::byte> a, std::span<const std::byte> b) override {
        return a.size() == b.size() &&
               (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
    }
};
/// 尝试有界分配；运行库缺失或预算不足返回空。 / Probe bounded allocations, returning null if
/// unavailable.
std::unique_ptr<Compute> create(std::size_t block, std::size_t budget, bool testing) {
    if (std::endian::native != std::endian::little || block < 1024 || budget < 1056)
        return {};
    auto count = std::min({block / 1024, budget / 1056, std::size_t{65536}});
    try {
        auto device = std::make_shared<Device>(program_for(testing), count * 1024);
        device->allocate();
        // 非零高计数器自检覆盖 64 位内核 ABI，不能仅检查空摘要。 / Check the 64-bit kernel
        // counter ABI with nonzero high bits, not merely the empty scalar digest.
        std::array<unsigned char, 1024> probe{};
        for (std::size_t i = 0; i < probe.size(); ++i)
            probe[i] = static_cast<unsigned char>(i % 251);
        constexpr std::uint64_t counter = (std::uint64_t{1} << 32) + 17;
        device->run(probe.data(), 1, counter);
        std::array<std::uint32_t, 8> expected{};
        b3::compress(b3::chunk(probe.data(), 1024, counter), expected.data());
        if (device->host_cvs[0] != expected)
            throw std::runtime_error("OpenCL device self-test mismatch");
        device->submissions = 0;
        return std::make_unique<OpenclCompute>(std::move(device));
    } catch (const ComputeError&) {
        return {};
    }
}
} // namespace
std::unique_ptr<Compute> try_igpu_compute(std::size_t block, std::size_t budget) {
    return create(block, budget, false);
}
namespace detail {
std::string opencl_device_name(const Compute& compute) {
    auto* p = dynamic_cast<const OpenclCompute*>(&compute);
    return p ? p->device_name() : std::string{};
}
std::uint64_t opencl_kernel_submissions(const Compute& compute) {
    auto* p = dynamic_cast<const OpenclCompute*>(&compute);
    return p ? p->submissions() : 0;
}
std::unique_ptr<Compute> try_opencl_test_compute(std::size_t block, std::size_t budget) {
    return create(block, budget, true);
}
} // namespace detail
} // namespace same
