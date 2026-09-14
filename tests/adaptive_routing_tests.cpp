/** @file
 * @brief 无真实设备的统一工作者并发与预算验证。 / Hardware-free unified concurrency and budgets.
 */
#include "same/resources.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <tuple>
using namespace std::chrono_literals;
namespace {
/// 保留断言上下文。 / Preserve assertion context.
void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
/// 使用真实摘要但模拟 CUDA 身份。 / Real digest implementation with simulated CUDA identity.
class Device final : public same::Compute {
public:
    /// 真实哈希用于设备校验。 / Real hash for device validation.
    std::unique_ptr<same::Hasher> hasher() override {
        return cpu_->hasher();
    }
    /// 保留精确比较。 / Preserve exact comparison.
    bool equal(std::span<const std::byte> a, std::span<const std::byte> b) override {
        return cpu_->equal(a, b);
    }
    /// 可观察后端。 / Observable backend identity.
    std::string name() const override {
        return "cuda";
    }

private:
    /// 每设备独占 CPU 实现。 / CPU implementation privately owned by each device.
    std::unique_ptr<same::Compute> cpu_{same::make_cpu_compute()};
};
/// 高预算、两个同质工作者。 / Generous budget and two homogeneous workers.
same::Config config() {
    same::Config c;
    c.cuda_bootstrap_ms = 0;
    c.backend = "auto";
    c.workers = 2;
    c.queue_capacity = 1;
    c.block_bytes = 1024;
    c.gpu_min_bytes = 1024;
    c.memory_bytes = 128 * 1024 * 1024;
    c.device_memory_bytes = 128 * 1024 * 1024;
    c.pgo = false;
    return c;
}
/// 返回当前后端。 / Return selected backend.
std::string backend(same::Worker& w) {
    return w.compute->name();
}
/// 两个任务必须同时持有独立 GPU 上下文，不依赖速度猜测。 / Two jobs must hold GPU contexts
/// concurrently.
void concurrent(bool fail_first) {
    auto c = config();
    std::atomic<unsigned> calls{};
    same::Resources pool(
        c, [&](std::size_t block, std::size_t device) -> std::unique_ptr<same::Compute> {
            require(block > 0 && device <= c.device_memory_bytes / c.workers,
                    "per-worker device budget exceeded");
            if (calls.fetch_add(1) == 0 && fail_first)
                throw same::ComputeError("one factory failed");
            return std::make_unique<Device>();
        });
    const auto warm = pool.submit_hash(backend, 128ULL * 1024 * 1024).get();
    pool.wait_idle();
    require(warm == (fail_first ? "cpu" : "cuda"),
            "bootstrap outcome differs from injected factory");
    std::promise<void> release;
    auto gate = release.get_future().share();
    std::promise<void> a_started, b_started;
    auto a = pool.submit_hash(
        [&](same::Worker& w) {
            a_started.set_value();
            gate.wait();
            return std::pair{w.index, w.compute->name()};
        },
        128ULL * 1024 * 1024);
    const bool a_ready = a_started.get_future().wait_for(5s) == std::future_status::ready;
    auto b = pool.submit_hash(
        [&](same::Worker& w) {
            b_started.set_value();
            gate.wait();
            return std::pair{w.index, w.compute->name()};
        },
        128ULL * 1024 * 1024);
    const bool b_ready = b_started.get_future().wait_for(5s) == std::future_status::ready;
    release.set_value();
    auto av = a.get();
    auto bv = b.get();
    pool.wait_idle();
    require(a_ready && b_ready, "workers serialized behind one GPU gate");
    require(av.first != bv.first, "two in-flight jobs shared worker identity");
    require(calls == 2, "each worker must lazily create its own context");
    require(pool.worker_count() == 2, "GPU initialization added extra worker threads");
    require(pool.gpu_peak_concurrency() == (fail_first ? 1 : 2),
            "GPU in-flight peak disagrees with gated execution");
    require(fail_first ? ((av.second == "cpu" && bv.second == "cuda") ||
                          (av.second == "cuda" && bv.second == "cpu"))
                       : (av.second == "cuda" && bv.second == "cuda"),
            "backend initialization failure escaped its worker");
    require(pool.profile_snapshot().samples == 0, "no-pgo learned samples");
}
/// 紧预算降级不会触发设备创建。 / Tight budget falls back without initializing a device.
void budgets() {
    auto c = config();
    c.memory_bytes = c.workers * (2 * c.block_bytes + c.block_bytes / 32 + 4096);
    std::atomic<unsigned> calls{};
    same::Resources pool(c, [&](std::size_t, std::size_t) {
        ++calls;
        return std::make_unique<Device>();
    });
    require(pool.submit_hash(backend, 128ULL * 1024 * 1024).get() == "cpu",
            "tight budget failed CPU fallback");
    pool.wait_idle();
    require(calls == 0, "GPU initialized beyond host budget");
}
/// 小文件不启动设备，单工作者随后仍可选择 GPU。 / Small files defer device creation, then same
/// worker can use GPU.
void lazy() {
    auto c = config();
    c.workers = 1;
    std::atomic<unsigned> calls{};
    same::Resources pool(c, [&](std::size_t, std::size_t) {
        ++calls;
        return std::make_unique<Device>();
    });
    require(calls == 0, "constructor eagerly created CUDA context");
    require(pool.submit_hash(backend, c.gpu_min_bytes - 1).get() == "cpu" && calls == 0,
            "small hash initialized CUDA");
    require(pool.submit_hash(backend, 128ULL * 1024 * 1024).get() == "cuda",
            "eligible work could not use GPU");
    require(pool.submit_hash(backend, 1).get() == "cpu", "same worker could not return to CPU");
}
/// 运行时错误完整重试并仅停用所属工作者设备。 / Runtime errors retry fully and retire the local
/// device.
void retry() {
    auto c = config();
    c.workers = 1;
    same::Resources pool(c, [](std::size_t, std::size_t) { return std::make_unique<Device>(); });
    auto result = pool.submit_hash(
                          [](same::Worker& w) {
                              int calls = 0;
                              auto selected = w.execute([&] {
                                  if (++calls == 1)
                                      throw same::ComputeError("injected runtime error");
                                  return w.compute->name();
                              });
                              return std::pair{calls, selected};
                          },
                          128ULL * 1024 * 1024)
                      .get();
    pool.wait_idle();
    require(result.first == 2 && result.second == "cpu" && pool.fallbacks() == 1,
            "runtime failure did not retry completely on CPU");
    require(pool.submit_hash(backend, 128ULL * 1024 * 1024).get() == "cpu",
            "retired device unexpectedly reused");
}
/// 首次设备初始化不得阻塞其他工作者；延迟的工作者之后仍可启用私有设备。
/// Cold device bootstrap never blocks peers, and deferred peers can initialize private devices
/// later.
void cold_bootstrap() {
    auto c = config();
    c.pgo = true;
    std::promise<void> factory_started, release_factory;
    auto factory_gate = release_factory.get_future().share();
    std::atomic<unsigned> calls{};
    same::Resources pool(
        c,
        [&](std::size_t, std::size_t) {
            if (calls.fetch_add(1) == 0) {
                factory_started.set_value();
                factory_gate.wait();
            }
            return std::make_unique<Device>();
        },
        {},
        [](same::BackendKind kind, const same::DeviceProfile& profile) {
            // 并发契约独立于探索顺序；使用覆盖零/一个竞争者的确定性先验。
            // Concurrency contract is independent of exploration order; cover zero/one peer.
            same::detail::OnlineModel prior;
            for (double peers : {0.0, 1.0})
                prior.observe(kind, {128ULL * 1024 * 1024, profile.effective_batch_bytes, peers},
                              kind == same::BackendKind::cpu ? 100 : 1);
            return prior.delta();
        });
    auto first = pool.submit_hash([](same::Worker& w) { return w.index; }, 128ULL * 1024 * 1024);
    const bool initializing =
        factory_started.get_future().wait_for(5s) == std::future_status::ready;
    auto peer = pool.submit_hash(
        [](same::Worker& w) {
            w.sample = {128ULL * 1024 * 1024, 10, false, true};
            return std::tuple{w.index, w.compute->name(), w.gpu_attempted};
        },
        128ULL * 1024 * 1024);
    const bool progressed = peer.wait_for(5s) == std::future_status::ready;
    release_factory.set_value();
    const auto first_index = first.get();
    const auto [peer_index, selected, attempted] = peer.get();
    pool.wait_idle();
    require(initializing && progressed, "cold bootstrap blocked CPU peer progress");
    require(first_index != peer_index && selected == "cpu" && !attempted,
            "deferred worker was marked attempted or did not select CPU");
    const auto profiles = pool.worker_profiles();
    require(profiles[peer_index].snapshot.cpu_samples == 1 && !profiles[peer_index].gpu_attempted &&
                profiles[peer_index].unavailable_cpu == 0 &&
                profiles[peer_index].cold_start_cpu == 1,
            "deferred CPU sample lost or optional device permanently consumed");
    std::promise<void> release_jobs, a_started, b_started;
    auto jobs_gate = release_jobs.get_future().share();
    auto a = pool.submit_hash(
        [&](same::Worker& w) {
            a_started.set_value();
            jobs_gate.wait();
            return w.compute->name();
        },
        128ULL * 1024 * 1024);
    const bool a_ready = a_started.get_future().wait_for(5s) == std::future_status::ready;
    auto b = pool.submit_hash(
        [&](same::Worker& w) {
            b_started.set_value();
            jobs_gate.wait();
            return w.compute->name();
        },
        128ULL * 1024 * 1024);
    const bool b_ready = b_started.get_future().wait_for(5s) == std::future_status::ready;
    release_jobs.set_value();
    const auto av = a.get();
    const auto bv = b.get();
    pool.wait_idle();
    require(a_ready && b_ready && av == "cuda" && bv == "cuda",
            "deferred worker could not later join concurrent GPU work");
    require(calls == 2 && pool.gpu_peak_concurrency() == 2,
            "bootstrap became a permanent single-GPU gate");
}
} // namespace
/// 每个场景都执行，无硬件跳过。 / Every case executes without hardware skips.
int main() {
    try {
        concurrent(false);
        concurrent(true);
        budgets();
        lazy();
        retry();
        cold_bootstrap();
        std::cout << "unified adaptive concurrency passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
