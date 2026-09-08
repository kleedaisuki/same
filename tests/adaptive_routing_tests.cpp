/** @file
 * @brief 无硬件、无速度胜负依赖的双向调度回归。 / Hardware-independent bidirectional routing tests.
 */
#include "same/resources.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
using namespace std::chrono_literals;
using same::detail::HashRoute;

/// 失败时保留场景诊断。 / Preserve scenario diagnostics on failure.
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

/// 假设备使用真正 BLAKE3，只注入设备身份和可控失败。 / Real BLAKE3 with fake identity and failure.
class FakeCuda final : public same::Compute {
public:
    /// 失败开关跨测试线程安全共享。 / Share the failure switch safely across test threads.
    explicit FakeCuda(std::shared_ptr<std::atomic<bool>> fail,
                      std::chrono::milliseconds delay = 0ms)
        : fail_(std::move(fail)), delay_(delay) {}
    /// 校准返回真实摘要，故不绕开正确性校验。 / Real digests preserve calibration verification.
    std::unique_ptr<same::Hasher> hasher() override {
        if (fail_->load())
            throw same::ComputeError("injected device failure");
        std::this_thread::sleep_for(delay_);
        return cpu_->hasher();
    }
    /// 比较同样由真实 CPU 实现。 / Delegate comparison to the real CPU implementation.
    bool equal(std::span<const std::byte> a, std::span<const std::byte> b) override {
        return cpu_->equal(a, b);
    }
    /// 调度观察设备名，不依赖硬件。 / Routing observes the device name, not hardware.
    std::string name() const override {
        return "cuda";
    }

private:
    /// 独占摘要后端。 / Exclusively owned digest backend.
    std::unique_ptr<same::Compute> cpu_{same::make_cpu_compute()};
    /// 仅校准结束后打开失败开关。 / Enable failure only after calibration.
    std::shared_ptr<std::atomic<bool>> fail_;
    /// 可选固定费用验证不胜但可用，不改变摘要。 / Optional cost tests availability without wins.
    std::chrono::milliseconds delay_;
};

/// 门闩自身负责异常路径释放，任务仅捕获共享状态。 / Exception-safe gate with shared task state.
class Gate {
public:
    /// 创建可重复读取的启动通知。 / Create a reusable start notification.
    Gate() : started_(started_promise_->get_future().share()) {}
    /// 先释放再销毁池，避免断言异常造成挂起。 / Release before pool destruction on assertion
    /// failure.
    ~Gate() {
        open();
    }
    /// 返回不引用 Gate 本体的阻塞任务。 / Return a blocking job without referencing this object.
    auto job() const {
        return [state = state_, started = started_promise_](same::Worker&) {
            started->set_value();
            std::unique_lock lock(state->mutex);
            state->changed.wait(lock, [&] { return state->open; });
        };
    }
    /// 有界等待用于故障诊断，不用于决定路由。 / Bounded diagnostic wait, not a routing decision.
    void wait_started() {
        require(started_.wait_for(5s) == std::future_status::ready, "gate job did not start");
    }
    /// 幂等释放，允许正常和异常路径复用。 / Idempotent release for normal and exceptional paths.
    void open() {
        {
            std::lock_guard lock(state_->mutex);
            state_->open = true;
        }
        state_->changed.notify_all();
    }

private:
    /// 任务完成前保持门闩同步状态存活。 / Keep synchronization alive until task completion.
    struct State {
        /// 保护释放位。 / Protect the release bit.
        std::mutex mutex;
        /// 发布释放。 / Publish release.
        std::condition_variable changed;
        /// 单向释放位。 / One-way release bit.
        bool open{};
    };
    /// 任务与主线程共同拥有状态。 / State shared by job and main thread.
    std::shared_ptr<State> state_{std::make_shared<State>()};
    /// 任务可独立发布启动。 / Job independently publishes its start.
    std::shared_ptr<std::promise<void>> started_promise_{std::make_shared<std::promise<void>>()};
    /// 启动通知。 / Start notification.
    std::shared_future<void> started_;
};

/// 保留一条 CPU 和额外 GPU 的小预算测试配置。 / Small configuration with one CPU plus GPU.
same::Config config() {
    same::Config result;
    result.backend = "auto";
    result.workers = 1;
    result.queue_capacity = 8;
    result.block_bytes = 1024;
    result.gpu_min_bytes = 1024;
    result.memory_bytes = 32768;
    return result;
}

/// 注入工厂只伪装设备，不伪造计时优势。 / Fake only the device, never timing profitability.
same::detail::CudaFactory factory(std::shared_ptr<std::atomic<bool>> fail) {
    return [fail](std::size_t, std::size_t) { return std::make_unique<FakeCuda>(fail); };
}

/// 正确性验证后保留服务；启动不生成性能样本。 / Validation retains service without performance
/// samples.
void prepare(same::Resources& pool, const same::Config& cfg) {
    pool.prepare_auto(cfg, 64ULL * 1024 * 1024 * 1024, 8);
    require(pool.gpu_workers() == 1, "valid fake CUDA must remain available without a speed win");
    pool.wait_idle();
    const auto& evidence = pool.dispatch_evidence();
    const auto profile = pool.profile_snapshot();
    require(evidence.device_validated && !evidence.calibration_complete &&
                !evidence.block_complete && !evidence.stream_complete && evidence.elapsed_ms == 0 &&
                evidence.cpu_block_ms == 0 && evidence.gpu_block_ms == 0 &&
                evidence.cpu_stream_ms == 0 && evidence.gpu_stream_ms == 0,
            "startup must validate correctness without synthetic performance calibration");
    require(profile.samples == 0 && profile.cpu_known_bands == 0 && profile.gpu_known_bands == 0,
            "startup populated real-task statistics or synthetic model priors");
}

/// 简短路由观察任务。 / Small routing observation job.
std::string backend(same::Worker& worker) {
    return worker.compute->name();
}

/// 占满 CPU 后，只有可溢出的载荷可进入 GPU。 / Only spillable payloads overflow a busy CPU.
void cpu_busy() {
    auto cfg = config();
    same::Resources pool(cfg, factory(std::make_shared<std::atomic<bool>>(false)));
    prepare(pool, cfg);
    Gate cpu;
    auto held = pool.submit(cpu.job());
    cpu.wait_started();
    auto overflow = pool.submit_hash(backend, HashRoute::cpu_preferred);
    const bool ready = overflow.wait_for(5s) == std::future_status::ready;
    auto small = pool.submit_hash(backend, HashRoute::cpu_only);
    const bool small_waited = small.wait_for(50ms) == std::future_status::timeout;
    cpu.open();
    held.get();
    require(ready && overflow.get() == "cuda", "busy CPU must overflow eligible work to GPU");
    require(small_waited && small.get() == "cpu", "small CPU-only payload reached GPU");
    pool.wait_idle();
    require(pool.route_counts().first == 1, "GPU overflow count differs from actual dispatch");
}

/// GPU 被固定任务占满时，大载荷必须回流 CPU。 / Large payload spills when GPU is pinned busy.
void gpu_busy() {
    auto cfg = config();
    same::Resources pool(cfg, factory(std::make_shared<std::atomic<bool>>(false)));
    prepare(pool, cfg);
    Gate gpu;
    auto held = pool.submit(gpu.job(), true);
    gpu.wait_started();
    auto spill = pool.submit_hash(backend, HashRoute::gpu_preferred);
    const bool ready = spill.wait_for(5s) == std::future_status::ready;
    gpu.open();
    held.get();
    require(ready && spill.get() == "cpu", "busy GPU must spill large work to CPU");
    pool.wait_idle();
    require(pool.route_counts().second == 1, "CPU spill count differs from actual dispatch");
}

/// 两边空闲时，每项必须兑现偏好，不能受唤醒竞争影响。 / Idle routing must not depend on wake races.
void idle_preferences() {
    auto cfg = config();
    same::Resources pool(cfg, factory(std::make_shared<std::atomic<bool>>(false)));
    prepare(pool, cfg);
    for (int round = 0; round < 100; ++round) {
        require(pool.submit_hash(backend, HashRoute::cpu_preferred).get() == "cpu",
                "idle CPU-preferred work ran on GPU");
        pool.wait_idle();
        require(pool.submit_hash(backend, HashRoute::gpu_preferred).get() == "cuda",
                "idle GPU-preferred work ran on CPU");
        pool.wait_idle();
    }
}

/// 设备错误后固定与偏好队列均能回流，并由析构排空。 / Failure reroutes and destructor drains
/// queues.
void failure_drain() {
    auto cfg = config();
    auto fail = std::make_shared<std::atomic<bool>>(false);
    std::future<std::string> recovered, queued, pinned;
    {
        same::Resources pool(cfg, factory(fail));
        prepare(pool, cfg);
        Gate cpu, gpu;
        auto held_cpu = pool.submit(cpu.job());
        cpu.wait_started();
        auto held_gpu = pool.submit(gpu.job(), true);
        gpu.wait_started();
        fail->store(true);
        recovered = pool.submit(
            [&](same::Worker& worker) {
                return worker.execute([&] {
                    auto hash = worker.compute->hasher();
                    (void)hash->finish();
                    return worker.compute->name();
                });
            },
            true);
        pinned = pool.submit(backend, true);
        queued = pool.submit_hash(backend, HashRoute::gpu_preferred);
        gpu.open();
        held_gpu.get();
        const bool retried = recovered.wait_for(5s) == std::future_status::ready;
        cpu.open();
        held_cpu.get();
        require(retried && recovered.get() == "cpu", "device operation failed to retry on CPU");
        require(pool.fallbacks() == 1, "device failure must count one whole-operation retry");
    }
    require(queued.get() == "cpu" && pinned.get() == "cpu", "failed GPU left queued work stranded");
}

/// 合法的最小 CPU 预算不应因可选 GPU 而失效，也不应初始化设备。 / Optional GPU respects CPU budget.
void tight_budget() {
    auto cfg = config();
    cfg.memory_bytes = 2 * cfg.block_bytes + cfg.block_bytes / 32 + 4096;
    std::atomic<unsigned> calls{};
    same::Resources pool(cfg, [&](std::size_t, std::size_t) {
        ++calls;
        return std::make_unique<FakeCuda>(std::make_shared<std::atomic<bool>>(false));
    });
    pool.prepare_auto(cfg, 64ULL * 1024 * 1024 * 1024, 8);
    require(calls == 0 && pool.gpu_workers() == 0, "insufficient extra budget still probed CUDA");
    require(pool.submit_hash(backend, HashRoute::gpu_preferred).get() == "cpu",
            "minimum legal budget must retain CPU service");
}

/// 两个路由队列共享唯一容量；释放 GPU 不会扩充 CPU-only 排队容量。 / Queue capacity is shared.
void shared_capacity() {
    auto cfg = config();
    cfg.queue_capacity = 1;
    same::Resources pool(cfg, factory(std::make_shared<std::atomic<bool>>(false)));
    prepare(pool, cfg);
    Gate cpu, gpu;
    auto held_cpu = pool.submit(cpu.job());
    cpu.wait_started();
    auto held_gpu = pool.submit(gpu.job(), true);
    gpu.wait_started();
    auto queued = pool.submit_hash(backend, HashRoute::cpu_only);
    std::promise<void> entered;
    auto producer = std::async(std::launch::async, [&] {
        entered.set_value();
        return pool.submit_hash(backend, HashRoute::gpu_preferred).get();
    });
    /// 在异步 future 等待析构前释放门闩。 / Release gates before an async future joins on unwind.
    struct Release {
        /// 引用的门闩比本保护对象存活更久。 / Referenced gates outlive this guard.
        Gate &cpu, &gpu;
        /// 异常和正常退出都解除阻塞。 / Unblock on both exceptional and normal exits.
        ~Release() {
            cpu.open();
            gpu.open();
        }
    } release{cpu, gpu};
    entered.get_future().wait();
    const bool blocked = producer.wait_for(50ms) == std::future_status::timeout;
    gpu.open();
    held_gpu.get();
    const bool still_blocked = producer.wait_for(50ms) == std::future_status::timeout;
    cpu.open();
    held_cpu.get();
    require(blocked && still_blocked, "routing queues exceeded shared capacity");
    require(queued.get() == "cpu", "CPU-only queued job was stolen");
    require(producer.get() == "cuda", "unblocked GPU-preferred producer lost its preferred lane");
}
/// 慢设备仍可通过正确性验证，不凭初始化延迟否决服务。 / Slow validation must not veto service.
void slow_validation_overflow() {
    auto cfg = config();
    same::Resources pool(cfg, [](std::size_t, std::size_t) {
        return std::make_unique<FakeCuda>(std::make_shared<std::atomic<bool>>(false), 10ms);
    });
    prepare(pool, cfg);
    const auto& evidence = pool.dispatch_evidence();
    require(evidence.device_validated && !evidence.block_gpu_preferred &&
                !evidence.stream_gpu_preferred,
            "slow validated GPU must remain available without synthetic performance preferences");
    Gate cpu;
    auto held = pool.submit(cpu.job());
    cpu.wait_started();
    auto overflow = pool.submit_hash(backend, HashRoute::cpu_preferred);
    const bool ready = overflow.wait_for(5s) == std::future_status::ready;
    cpu.open();
    held.get();
    require(ready && overflow.get() == "cuda", "nonwinning available GPU rejected CPU overflow");
}

/// 慢于旧两秒校准预算的验证不应被误认为设备失败。
/// Validation slower than the retired two-second probe budget is not a device failure.
void slow_validation_service() {
    const auto cfg = config();
    same::Resources pool(cfg, [](std::size_t, std::size_t) {
        return std::make_unique<FakeCuda>(std::make_shared<std::atomic<bool>>(false), 2100ms);
    });
    prepare(pool, cfg);
    const auto& evidence = pool.dispatch_evidence();
    require(evidence.device_validated && !evidence.calibration_complete &&
                !evidence.block_complete && !evidence.stream_complete,
            "successful slow validation must retain service without performance measurements");
    Gate cpu;
    auto held = pool.submit(cpu.job());
    cpu.wait_started();
    auto overflow = pool.submit_hash(backend, HashRoute::cpu_preferred);
    const bool ready = overflow.wait_for(5s) == std::future_status::ready;
    cpu.open();
    held.get();
    require(ready && overflow.get() == "cuda", "slow validation stranded usable GPU service");
}

/// 探测参数不可改变构造时的预算或资格契约。 / Probing cannot change construction contracts.
void immutable_budgets() {
    const auto cfg = config();
    std::atomic<unsigned> calls{};
    same::Resources pool(cfg, [&](std::size_t, std::size_t) {
        ++calls;
        return std::make_unique<FakeCuda>(std::make_shared<std::atomic<bool>>(false));
    });
    for (unsigned field = 0; field < 3; ++field) {
        auto changed = cfg;
        if (field == 0)
            changed.memory_bytes *= 2;
        else if (field == 1)
            changed.device_memory_bytes *= 2;
        else
            changed.gpu_min_bytes *= 2;
        bool rejected = false;
        try {
            pool.prepare_auto(changed, 1024 * 1024, 1);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected && calls == 0, "modified probe contract initialized optional resources");
    }
    prepare(pool, cfg);
    require(calls == 1, "rejected probe must not consume the valid preparation attempt");
}

/// 大缓冲不得抬高自定义载荷下界；分类器与工作者必须一致。 / Large buffers do not raise eligibility.
void independent_gpu_buffer() {
    auto cfg = config();
    cfg.memory_bytes = 128 * 1024 * 1024;
    cfg.block_bytes = 32 * 1024 * 1024;
    cfg.gpu_min_bytes = 1024;
    std::size_t requested{};
    same::Resources pool(cfg, [&](std::size_t block, std::size_t) {
        requested = block;
        return std::make_unique<FakeCuda>(std::make_shared<std::atomic<bool>>(false));
    });
    prepare(pool, cfg);
    const auto [buffer, floor] =
        pool.submit(
                [](same::Worker& worker) {
                    return std::pair{worker.first.size(), worker.gpu_floor};
                },
                true)
            .get();
    require(requested == 16 * 1024 * 1024 && buffer == requested && floor == cfg.gpu_min_bytes,
            "GPU service capacity incorrectly changed the configured eligibility floor");
    const auto route = same::detail::classify_hash(floor, floor, buffer, pool.dispatch_evidence());
    require(route != HashRoute::cpu_only &&
                same::detail::classify_hash(floor - 1, floor, buffer, pool.dispatch_evidence()) ==
                    HashRoute::cpu_only,
            "classifier disagrees with the GPU worker eligibility boundary");
    pool.wait_idle();
    Gate cpu;
    auto held = pool.submit(cpu.job());
    cpu.wait_started();
    auto eligible = pool.submit_hash(backend, route);
    const bool ready = eligible.wait_for(5s) == std::future_status::ready;
    cpu.open();
    held.get();
    require(ready && eligible.get() == "cuda", "eligible sub-buffer payload did not reach GPU");
}

/// 部分 CPU 忙不构成饱和；只有全部工作者忙才溢出。 / Overflow requires every CPU worker busy.
void full_cpu_saturation() {
    auto cfg = config();
    cfg.workers = 2;
    same::Resources pool(cfg, factory(std::make_shared<std::atomic<bool>>(false)));
    prepare(pool, cfg);
    Gate first, second;
    auto held_first = pool.submit(first.job());
    first.wait_started();
    auto partial = pool.submit_hash(backend, HashRoute::cpu_preferred);
    require(partial.wait_for(5s) == std::future_status::ready && partial.get() == "cpu",
            "one busy CPU incorrectly counted as full CPU saturation");
    auto held_second = pool.submit(second.job());
    second.wait_started();
    auto saturated = pool.submit_hash(backend, HashRoute::cpu_preferred);
    const bool ready = saturated.wait_for(5s) == std::future_status::ready;
    first.open();
    second.open();
    held_first.get();
    held_second.get();
    require(ready && saturated.get() == "cuda", "all busy CPUs failed to trigger GPU overflow");
}

/// 独立校准缓冲允许与已有 CPU 任务重叠。 / Independent calibration overlaps active CPU jobs.
void prepare_while_cpu_busy() {
    const auto cfg = config();
    same::Resources pool(cfg, factory(std::make_shared<std::atomic<bool>>(false)));
    Gate cpu;
    auto held = pool.submit(cpu.job());
    cpu.wait_started();
    auto preparing = std::async(std::launch::async,
                                [&] { pool.prepare_auto(cfg, 64ULL * 1024 * 1024 * 1024, 8); });
    /// 必须在异步 future 的等待式析构前释放 CPU。 / Release CPU before the async future joins.
    struct Release {
        /// 被测任务的共享门闩。 / Gate of the active test job.
        Gate& gate;
        /// 异常路径也解除任务阻塞。 / Unblock the job on exceptional paths too.
        ~Release() {
            gate.open();
        }
    } release{cpu};
    const bool overlapped = preparing.wait_for(5s) == std::future_status::ready;
    cpu.open();
    held.get();
    preparing.get();
    require(overlapped && pool.gpu_workers() == 1,
            "automatic preparation unnecessarily waited for existing CPU work to drain");
}
} // namespace

/// 全部场景都执行，不允许因无真实设备或测量较慢跳过。 / Execute every case without hardware skips.
int main() {
    try {
        cpu_busy();
        gpu_busy();
        idle_preferences();
        failure_drain();
        tight_budget();
        shared_capacity();
        slow_validation_overflow();
        slow_validation_service();
        immutable_budgets();
        independent_gpu_buffer();
        full_cpu_saturation();
        prepare_while_cpu_busy();
        std::cout << "adaptive routing: all deterministic scenarios passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
