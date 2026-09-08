/** @file
 * @brief 在线调度的确定性成本及观测契约。 / Deterministic online scheduling costs and observations.
 */
#include "same/resources.hpp"
#include <iostream>
#include <stdexcept>

namespace {
/// 保留断言上下文。 / Preserve assertion context.
void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
/// 真实摘要的假设备，不依赖真实硬件速度。 / Fake device with real digests, independent of hardware
/// speed.
class Device final : public same::Compute {
public:
    /// 复用真实摘要实现。 / Reuse real digest implementation.
    std::unique_ptr<same::Hasher> hasher() override {
        return cpu_->hasher();
    }
    /// 复用字节比较。 / Reuse byte comparison.
    bool equal(std::span<const std::byte> a, std::span<const std::byte> b) override {
        return cpu_->equal(a, b);
    }
    /// 可观察的测试身份。 / Observable test identity.
    std::string name() const override {
        return "cuda";
    }

private:
    /// 私有真实后端。 / Private real backend.
    std::unique_ptr<same::Compute> cpu_{same::make_cpu_compute()};
};
/// 最小预算使测试校准有界。 / Minimal budget keeps test calibration bounded.
same::Config config(bool enabled) {
    same::Config c;
    c.backend = "auto";
    c.workers = 1;
    c.block_bytes = 1024;
    c.gpu_min_bytes = 1024;
    c.memory_bytes = 32768;
    c.queue_capacity = 8;
    c.pgo = enabled;
    return c;
}
/// 注入受控观测而非依靠 sleep 推导服务时间。 / Inject controlled observations instead of timing
/// sleeps.
void observation(same::Resources& pool, bool gpu, double ms) {
    pool.submit([=](same::Worker& w) { w.sample = {1048576, ms, gpu, true}; }).get();
    pool.wait_idle();
}
/// 纯数学反例和误差余量。 / Pure mathematical counterexamples and residual margin.
void arithmetic() {
    using P = same::detail::OnlineModel::Prediction;
    require(!same::detail::gpu_finishes_first(P{3, 0, 1, true}, P{20, 0, 1, true}, 2, 0),
            "busy CPU completing in 5ms must beat idle 20ms GPU");
    require(same::detail::gpu_finishes_first(P{3, 0, 1, true}, P{20, 0, 1, true}, 30, 0),
            "GPU wins when CPU residual exceeds service difference");
    require(!same::detail::gpu_finishes_first(P{30, 0, 1, true}, P{3, 0, 1, true}, 0, 40),
            "GPU queue residual must matter");
    require(!same::detail::gpu_finishes_first(P{10, 2, 1, true}, P{8, 2, 1, true}, 0, 0),
            "uncertainty must suppress marginal routing changes");
}
/// 实测模型覆盖旧静态偏好；失败及关闭开关不学习。 / Model overrides static routes; failures/off do
/// not learn.
void scheduling(bool enabled) {
    auto cfg = config(enabled);
    same::Resources pool(cfg, [](std::size_t, std::size_t) { return std::make_unique<Device>(); });
    pool.prepare_auto(cfg, 1048576, 1);
    require(pool.gpu_workers() == 1, "validated device must remain available");
    require(pool.dispatch_evidence().cpu_block_ms == 0 &&
                pool.dispatch_evidence().gpu_block_ms == 0 &&
                !pool.dispatch_evidence().calibration_complete,
            "production startup must not run synthetic performance calibration");
    require(pool.profile_snapshot().samples == 0 && pool.profile_snapshot().predicted_samples == 0,
            "startup must not fabricate task observations");
    observation(pool, false, 30);
    observation(pool, true, 1);
    if (!enabled) {
        require(pool.profile_snapshot().samples == 0, "disabled profiler learned samples");
        require(pool.dispatch_evidence().cpu_block_ms == 0, "disabled profiler ran calibration");
        return;
    }
    auto route = [&](same::detail::HashRoute preference) {
        auto future = pool.submit_hash([](same::Worker& w) { return w.compute->name(); },
                                       preference, 1048576);
        auto result = future.get();
        pool.wait_idle();
        return result;
    };
    require(route(same::detail::HashRoute::cpu_preferred) == "cuda",
            "model must override CPU preference");
    for (int i = 0; i < 64; ++i) {
        observation(pool, false, 1);
        observation(pool, true, 30);
    }
    require(route(same::detail::HashRoute::gpu_preferred) != "cuda",
            "model must override GPU preference");
    const auto before = pool.profile_snapshot().samples;
    pool.submit([](same::Worker& w) { w.sample = {1048576, 2, false, false}; }).get();
    pool.wait_idle();
    require(pool.profile_snapshot().samples == before, "invalid sample was learned");
    pool.submit([](same::Worker& w) {
            w.sample = {1048576, 2, false, true};
            w.compute = same::make_cpu_compute();
        })
        .get();
    pool.wait_idle();
    require(pool.profile_snapshot().samples == before, "backend replacement sample was learned");
}
/// 只反馈实际被选后端，周期探索必须从过时先验中恢复。
/// Feed only the selected backend; periodic exploration must recover from stale priors.
void drift(bool gpu_recovers) {
    auto cfg = config(true);
    same::Resources pool(cfg, [](std::size_t, std::size_t) { return std::make_unique<Device>(); });
    pool.prepare_auto(cfg, 1048576, 1);
    observation(pool, false, gpu_recovers ? 3 : 20);
    observation(pool, true, gpu_recovers ? 20 : 3);
    auto run = [&] {
        auto task = pool.submit_hash(
            [=](same::Worker& w) {
                const bool gpu = w.compute->name() == "cuda";
                w.sample = {1048576, gpu == gpu_recovers ? 1.0 : 3.0, gpu, true};
                return gpu;
            },
            same::detail::HashRoute::cpu_preferred, 1048576);
        const bool gpu = task.get();
        pool.wait_idle();
        return gpu;
    };
    for (int i = 0; i < 4096; ++i)
        run();
    require(run() == gpu_recovers, "periodic selected-backend feedback failed drift recovery");
    require(pool.exploration_jobs() >= 32, "drift probes were not observed");
}
/// 过期预测必须唤醒另一后端；释放门闩前验证进度，无 sleep 决策。
/// Expired estimates wake the other backend; verify progress before release, no sleep-based
/// routing.
void overdue(bool blocked_gpu) {
    auto cfg = config(true);
    same::Resources pool(cfg, [](std::size_t, std::size_t) { return std::make_unique<Device>(); });
    pool.prepare_auto(cfg, 1048576, 1);
    observation(pool, false, blocked_gpu ? 20 : 1);
    observation(pool, true, blocked_gpu ? 1 : 20);
    std::promise<void> started, release;
    auto entered = started.get_future();
    auto gate = release.get_future().share();
    auto blocked = pool.submit_hash(
        [&](same::Worker&) {
            started.set_value();
            gate.wait();
        },
        same::detail::HashRoute::cpu_preferred, 1048576);
    const bool did_start = entered.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    if (!did_start) {
        release.set_value();
        require(false, "stalled-backend test never entered gate");
    }
    auto other = pool.submit_hash([](same::Worker& w) { return w.compute->name() == "cuda"; },
                                  same::detail::HashRoute::cpu_preferred, 1048576);
    const bool progressed = other.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    release.set_value();
    blocked.get();
    const bool used_gpu = other.get();
    pool.wait_idle();
    require(progressed, blocked_gpu ? "overdue GPU stalled CPU until gate release"
                                    : "overdue CPU stalled GPU until gate release");
    require(used_gpu != blocked_gpu, "overdue backend did not hand work to idle peer");
}
/// 关闭期间不能因暂时偏好 GPU 丢失 CPU 消费者，设备退役后仍排空两个队列。
/// Closure retains CPU consumers despite temporary GPU preference; retirement drains both queues.
void close_drain(bool fail_gpu) {
    auto cfg = config(true);
    auto pool = std::make_unique<same::Resources>(
        cfg, [](std::size_t, std::size_t) { return std::make_unique<Device>(); });
    pool->prepare_auto(cfg, 1048576, 1);
    observation(*pool, false, 10000);
    observation(*pool, true, 1000);
    std::promise<void> cpu_started, gpu_started, cpu_release, gpu_release;
    auto cpu_entered = cpu_started.get_future();
    auto gpu_entered = gpu_started.get_future();
    auto cpu_gate = cpu_release.get_future().share();
    auto gpu_gate = gpu_release.get_future().share();
    auto cpu_job = pool->submit([&](same::Worker&) {
        cpu_started.set_value();
        cpu_gate.wait();
    });
    cpu_entered.wait();
    auto gpu_job = pool->submit_hash(
        [&](same::Worker& worker) {
            gpu_started.set_value();
            gpu_gate.wait();
            bool first = true;
            worker.execute([&] {
                if (fail_gpu && std::exchange(first, false))
                    throw same::ComputeError("retire during close");
            });
        },
        same::detail::HashRoute::gpu_preferred, 1048576);
    gpu_entered.wait();
    auto first = pool->submit_hash([](same::Worker&) { return 1; },
                                   same::detail::HashRoute::cpu_preferred, 1048576);
    auto second = pool->submit_hash([](same::Worker&) { return 2; },
                                    same::detail::HashRoute::gpu_preferred, 1048576);
    std::promise<void> closing_started;
    auto closing_entered = closing_started.get_future();
    auto closing =
        std::async(std::launch::async, [owned = std::move(pool), &closing_started]() mutable {
            closing_started.set_value();
            owned.reset();
        });
    closing_entered.wait();
    cpu_release.set_value();
    const bool held =
        closing.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout;
    gpu_release.set_value();
    closing.get();
    cpu_job.get();
    gpu_job.get();
    require(held, "closure did not wait for admitted in-flight work");
    require(first.get() == 1 && second.get() == 2, "closure lost accepted sized work");
}
} // namespace
/// 非零退出提供可复现诊断。 / Nonzero exit preserves reproducible diagnostics.
int main() {
    try {
        arithmetic();
        scheduling(true);
        scheduling(false);
        drift(true);
        drift(false);
        overdue(true);
        overdue(false);
        close_drain(false);
        close_drain(true);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
