/** @file
 * @brief iGPU 准入、归属和回退的无硬件测试。 / Hardware-free iGPU admission and fallback tests.
 */
#include "same/resources.hpp"
#include <atomic>
#include <cmath>
#include <iostream>
#include <stdexcept>
namespace {
/// 独立于 NDEBUG 的检查。 / Checks independent of NDEBUG.
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
/// CPU 摘要加模拟设备身份。 / CPU digest with simulated device identity.
class Device final : public same::Compute {
public:
    /// 可配置身份。 / Configurable identity.
    explicit Device(same::BackendKind kind, std::uint64_t batch = 0) : kind_(kind) {
        profile_.backend = kind;
        profile_.effective_batch_bytes = batch;
        profile_.device_name = "routing-test";
    }
    /// 模拟可验证批容量。 / Simulated inspectable batch capacity.
    const same::DeviceProfile& profile() const override {
        return profile_;
    }
    /// 复用可靠摘要。 / Reuse trusted digest.
    std::unique_ptr<same::Hasher> hasher() override {
        return cpu_->hasher();
    }
    /// 精确比较。 / Exact comparison.
    bool equal(std::span<const std::byte> a, std::span<const std::byte> b) override {
        return cpu_->equal(a, b);
    }
    /// 强类型身份。 / Typed identity.
    same::BackendKind kind() const noexcept override {
        return kind_;
    }
    /// 稳定显示身份。 / Stable display identity.
    std::string name() const override {
        return kind_ == same::BackendKind::igpu ? "igpu" : "cuda";
    }

private:
    /// 设备身份与参考实现。 / Device identity and reference implementation.
    same::BackendKind kind_;
    same::DeviceProfile profile_;
    std::unique_ptr<same::Compute> cpu_{same::make_cpu_compute()};
};
/// 有界共享预算。 / Bounded shared budget.
same::Config config() {
    same::Config c;
    c.cuda_bootstrap_ms = 0;
    c.workers = 2;
    c.igpu_bootstrap_ms = 0; // 明确允许快速路由实验。 / Explicit fast-routing experiment.
    c.backend = "igpu";
    c.block_bytes = 1024;
    c.gpu_min_bytes = 0;
    c.memory_bytes = 8 * 1024 * 1024;
    c.queue_capacity = 8;
    return c;
}
/// 忙设备不等待，工厂仅调用一次。 / Busy device never waits; initialize exactly once.
void admission() {
    auto c = config();
    std::atomic<unsigned> calls{};
    same::Resources pool(c, {}, [&](auto, auto) {
        ++calls;
        return std::make_unique<Device>(same::BackendKind::igpu);
    });
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    auto first = pool.submit_hash(
        [&](same::Worker& w) {
            require(w.compute->kind() == same::BackendKind::igpu, "iGPU missing");
            entered.set_value();
            gate.wait();
        },
        1024);
    entered.get_future().wait();
    auto second = pool.submit_hash([](same::Worker& w) { return w.compute->kind(); }, 1024);
    const auto ready = second.wait_for(std::chrono::seconds(5));
    release.set_value();
    first.get();
    require(ready == std::future_status::ready, "busy iGPU blocked other worker");
    require(second.get() == same::BackendKind::cpu, "busy iGPU was reused");
    pool.wait_idle();
    require(calls == 1, "compiler storm");
    auto retry = pool.submit_hash(
        [](same::Worker& w) {
            unsigned attempts = 0;
            return w.execute([&] {
                if (++attempts == 1)
                    throw same::ComputeError("injected device failure");
                require(w.compute->kind() == same::BackendKind::cpu, "retry not CPU");
                return attempts;
            });
        },
        1024);
    require(retry.get() == 2, "whole operation not retried");
    pool.wait_idle();
    require(pool.submit_hash([](same::Worker& w) { return w.compute->kind(); }, 1024).get() ==
                same::BackendKind::cpu,
            "failed iGPU not retired");
    require(calls == 1, "failed device reinitialized");
}
/// 初始化失败不重探，低预算不分配。 / Failed initialization is not repeated; low budget allocates
/// nothing.
void unavailable() {
    auto c = config();
    c.workers = 1;
    unsigned calls = 0;
    same::Resources pool(
        c, {}, [&](std::size_t block, std::size_t budget) -> std::unique_ptr<same::Compute> {
            ++calls;
            require(budget <= block + block / 32, "device reservation inflated");
            throw same::ComputeError("unavailable");
        });
    for (unsigned i = 0; i < 4; ++i)
        require(pool.submit_hash([](same::Worker& w) { return w.compute->kind(); }, 1024).get() ==
                    same::BackendKind::cpu,
                "unavailable fallback missing");
    pool.wait_idle();
    require(calls == 1, "failed factory repeated");
    c.memory_bytes = 2 * c.block_bytes + c.block_bytes / 32 + 4096;
    same::Resources small(c, {}, [&](auto, auto) -> std::unique_ptr<same::Compute> {
        throw std::runtime_error("over-budget factory invoked");
    });
    require(small.submit_hash([](same::Worker& w) { return w.compute->kind(); }, 1024).get() ==
                same::BackendKind::cpu,
            "tight budget did not use CPU");
}
/// 先验只加载一次，实际批容量及冻结上下文用于本轮增量。
/// Load prior once; actual capacity and frozen context feed run-only deltas.
void contextual_prior() {
    auto c = config();
    c.workers = 1;
    unsigned loads = 0;
    same::Resources pool(
        c, {}, [](auto, auto) { return std::make_unique<Device>(same::BackendKind::igpu, 512); },
        [&](same::BackendKind kind, const same::DeviceProfile& profile) {
            ++loads;
            same::detail::OnlineModel prior;
            require(profile.effective_batch_bytes == (kind == same::BackendKind::igpu ? 512 : 1024),
                    "prior key received wrong actual capacity");
            prior.observe(kind, {2048, profile.effective_batch_bytes, 0}, 1);
            return prior.delta();
        });
    for (unsigned i = 0; i < 2; ++i)
        pool.submit_hash(
                [](same::Worker& w) {
                    require(w.decision_context.bytes == 2048 &&
                                w.decision_context.effective_batch_bytes == 512 &&
                                w.decision_context.contention == 0,
                            "context not frozen to actual batch");
                    w.sample = {2048, 2, false, true, same::BackendKind::igpu};
                },
                2048)
            .get();
    pool.wait_idle();
    const auto profile = pool.worker_profiles().front();
    require(loads == 2, "prior loaded more than once per device");
    require(profile.prior[2].samples == 1 && profile.delta[2].samples == 2 &&
                profile.delta[0].samples == 0,
            "prior multiplied into worker delta");
    require(profile.devices[2].effective_batch_bytes == 512,
            "export identity differs from load identity");
    // 错误输入大小不是冻结决策的数据，必须拒绝训练。
    // A mismatched payload is not the frozen decision and must not train.
    pool.submit_hash(
            [](same::Worker& w) { w.sample = {4096, 1, false, true, same::BackendKind::igpu}; },
            2048)
        .get();
    pool.wait_idle();
    require(pool.worker_profiles().front().delta[2].samples == 2,
            "mismatched context trained model");
}
/// 懒初始化发现慢设备先验后，执行前重新选择 CPU。
/// A slow prior discovered lazily redirects to CPU before actual work.
void lazy_prior_reselection() {
    auto c = config();
    c.workers = 1;
    c.backend = "auto";
    c.memory_bytes = c.device_memory_bytes = 128ULL * 1024 * 1024;
    unsigned factories = 0;
    same::Resources pool(
        c,
        [&](auto, auto) {
            ++factories;
            return std::make_unique<Device>(same::BackendKind::cuda);
        },
        {},
        [](same::BackendKind kind, const same::DeviceProfile& profile) {
            same::detail::OnlineModel prior;
            prior.observe(kind, {128ULL * 1024 * 1024, profile.effective_batch_bytes, 0},
                          kind == same::BackendKind::cpu ? 1 : 100);
            return prior.delta();
        });
    require(pool.submit_hash([](same::Worker& worker) { return worker.compute->kind(); },
                             128ULL * 1024 * 1024)
                    .get() == same::BackendKind::cpu,
            "lazy loaded prior did not influence first execution");
    pool.wait_idle();
    require(factories == 1 && pool.gpu_peak_concurrency() == 0,
            "discovery counted as executed CUDA work or repeated initialization");
}
/// 轻量发现与实际设备身份一致。 / Lightweight discovery matches activation identity.
same::detail::IgpuProbe probe(unsigned& calls) {
    return [&calls](std::size_t block, std::size_t) -> std::optional<same::DeviceProfile> {
        ++calls;
        Device device(same::BackendKind::igpu, block);
        return device.profile();
    };
}
/// 未知短任务不支付初始化，累计完成工作后只初始化一次。
/// Unknown short tasks defer setup; accumulated completed work eventually activates once.
void startup_credit() {
    auto c = config();
    c.backend = "auto";
    c.workers = 1;
    c.igpu_bootstrap_ms = 0;
    c.cold_exploration_fraction = 1;
    unsigned probes = 0, factories = 0;
    same::Resources pool(
        c, {},
        [&](std::size_t block, auto) {
            ++factories;
            return std::make_unique<Device>(same::BackendKind::igpu, block);
        },
        {}, [](auto, const auto&) { return 10.0; }, probe(probes));
    auto submit = [&] {
        return pool
            .submit_hash(
                [](same::Worker& worker) {
                    const auto kind = worker.compute->kind();
                    worker.sample = {1024, 5, false, true, kind};
                    return kind;
                },
                1024)
            .get();
    };
    require(submit() == same::BackendKind::cpu && submit() == same::BackendKind::cpu,
            "short unknown tasks paid cold activation");
    pool.wait_idle();
    require(probes == 1 && factories == 0 && pool.cold_credit_ms() == 10 &&
                pool.igpu_setup_estimate_ms() == 10 && pool.igpu_deferred() > 0,
            "discovery/history/credit accounting wrong");
    bool admitted = false;
    for (unsigned i = 0; i < 8 && !admitted; ++i)
        admitted = submit() == same::BackendKind::igpu;
    require(admitted, "earned credit never admitted cold exploration");
    pool.wait_idle();
    require(factories == 1 && pool.igpu_attempted() &&
                std::abs(pool.cold_spent_ms() - pool.igpu_setup_ms() - pool.igpu_discovery_ms()) <
                    1e-9,
            "setup not charged exactly once");
}
/// 先验劣势避免工厂；单任务净收益足够时无需已完成工作额度。
/// Slow priors avoid activation; profitable current work needs no earned credit.
void startup_prior(bool profitable) {
    auto c = config();
    c.backend = "auto";
    c.workers = 1;
    c.igpu_bootstrap_ms = profitable ? 100 : 0;
    unsigned probes = 0, factories = 0;
    same::Resources pool(
        c, {},
        [&](std::size_t block, auto) {
            ++factories;
            return std::make_unique<Device>(same::BackendKind::igpu, block);
        },
        [profitable](same::BackendKind kind, const same::DeviceProfile& profile) {
            same::detail::OnlineModel prior;
            const auto ms = kind == same::BackendKind::cpu ? (profitable ? 1000.0 : 1.0) : 10.0;
            prior.observe(kind, {1024, profile.effective_batch_bytes, 0}, ms);
            return prior.delta();
        },
        {}, probe(probes));
    const auto actual =
        pool.submit_hash([](same::Worker& worker) { return worker.compute->kind(); }, 1024).get();
    pool.wait_idle();
    require(probes == 1 && factories == (profitable ? 1U : 0U) &&
                actual == (profitable ? same::BackendKind::igpu : same::BackendKind::cpu) &&
                pool.cold_credit_ms() == 0,
            "startup prior did not control activation");
}
/// 默认短扫描与静态 CPU 选择均不触发发现；CUDA 冷探索也必须有依据。
/// Default short scans and static CPU routing never discover; cold CUDA needs evidence too.
void short_scan_discovery(bool pgo) {
    auto c = config();
    c.backend = "auto";
    c.workers = 1;
    c.pgo = pgo;
    c.cuda_bootstrap_ms = c.igpu_bootstrap_ms = 100;
    unsigned probes = 0, factories = 0;
    same::Resources pool(
        c,
        [&](auto, auto) -> std::unique_ptr<same::Compute> {
            ++factories;
            return std::make_unique<Device>(same::BackendKind::cuda);
        },
        [&](auto, auto) -> std::unique_ptr<same::Compute> {
            ++factories;
            return std::make_unique<Device>(same::BackendKind::igpu);
        },
        {}, {}, probe(probes));
    for (unsigned i = 0; i < 6; ++i)
        require(pool.submit_hash(
                        [](same::Worker& worker) {
                            worker.sample = {1024, 1, false, true};
                            return worker.compute->kind();
                        },
                        1024)
                        .get() == same::BackendKind::cpu,
                "short scan left CPU");
    pool.wait_idle();
    require(probes == 0 && factories == 0 && pool.cold_spent_ms() == 0,
            "short scan paid discarded discovery or activation");
}
/// 三设备各自探索且模型不混合。 / All three devices explored without model aliasing.
void exploration() {
    auto c = config();
    c.workers = 1;
    c.backend = "auto";
    c.cold_exploration_fraction =
        0; // 零估计显式允许无预算校准。 / Zero estimate opts into unfunded calibration.
    same::Resources pool(
        c, [](auto, auto) { return std::make_unique<Device>(same::BackendKind::cuda); },
        [](auto, auto) { return std::make_unique<Device>(same::BackendKind::igpu); });
    bool seen[3]{};
    for (unsigned i = 0; i < 12; ++i) {
        const auto kind =
            pool.submit_hash(
                    [](same::Worker& w) {
                        const auto kind = w.compute->kind();
                        w.sample = {1024, 1, kind == same::BackendKind::cuda, true, kind};
                        return kind;
                    },
                    1024)
                .get();
        seen[static_cast<unsigned>(kind)] = true;
    }
    pool.wait_idle();
    require(seen[0] && seen[1] && seen[2], "auto never evaluated all devices");
    require(pool.cold_credit_ms() == 0 && pool.cold_spent_ms() > 0,
            "zero-estimate experiment did not tolerate measured startup debt");
    require(pool.profile_snapshot().igpu_samples > 0, "iGPU sample attributed incorrectly");
}
} // namespace
int main() {
    try {
        admission();
        unavailable();
        contextual_prior();
        lazy_prior_reselection();
        startup_credit();
        startup_prior(false);
        startup_prior(true);
        short_scan_discovery(true);
        short_scan_discovery(false);
        exploration();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
