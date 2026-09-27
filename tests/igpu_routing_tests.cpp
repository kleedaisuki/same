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
/// 训练请求绕过大小偏好和冷启动信用，但只由指定后端生成标签。
/// Explicit training bypasses size preference and cold credit without relabeling fallbacks.
void training_override() {
    auto c = config();
    c.backend = "auto";
    c.workers = 1;
    c.gpu_min_bytes = 1024 * 1024;
    c.cuda_bootstrap_ms = 1000000;
    c.igpu_bootstrap_ms = 1000000;
    same::Resources pool(
        c, [](auto, auto) { return std::make_unique<Device>(same::BackendKind::cuda); },
        [](auto, auto) { return std::make_unique<Device>(same::BackendKind::igpu); });
    for (const auto kind :
         {same::BackendKind::cpu, same::BackendKind::cuda, same::BackendKind::igpu}) {
        const auto actual =
            pool.submit_training_hash(
                    [kind](same::Worker& worker) {
                        require(worker.compute->kind() == kind,
                                "training selected the wrong backend");
                        worker.sample = {2048, 1, kind == same::BackendKind::cuda, true, kind};
                        return worker.compute->kind();
                    },
                    2048, kind)
                .get();
        require(actual == kind, "training backend request was ignored");
    }
    pool.wait_idle();
    const auto snapshot = pool.profile_snapshot();
    require(snapshot.cpu_samples == 1 && snapshot.gpu_samples == 1 && snapshot.igpu_samples == 1,
            "training labels leaked across backends");
}
/// 每个线程都可测量三设备，并用各自的真实执行上下文更新模型。
/// Every worker can measure all three backends and update its own model from actual execution.
void three_backend_workers() {
    auto c = config();
    c.backend = "auto";
    std::atomic<unsigned> cuda_calls{}, igpu_calls{};
    same::Resources pool(
        c,
        [&](std::size_t, std::size_t budget) {
            require(budget <= c.device_memory_bytes / c.workers, "CUDA budget multiplied");
            ++cuda_calls;
            return std::make_unique<Device>(same::BackendKind::cuda);
        },
        [&](std::size_t block, std::size_t budget) {
            require(block <= c.block_bytes && budget <= c.device_memory_bytes / c.workers,
                    "iGPU budget multiplied");
            ++igpu_calls;
            return std::make_unique<Device>(same::BackendKind::igpu, block);
        });
    for (auto kind : {same::BackendKind::cpu, same::BackendKind::cuda, same::BackendKind::igpu}) {
        std::atomic<unsigned> entered{};
        std::promise<void> both, release;
        auto gate = release.get_future().share();
        auto submit = [&] {
            return pool.submit_training_hash(
                [&](same::Worker& w) {
                    require(w.compute->kind() == kind, "worker did not select requested backend");
                    w.sample = {2048, 2, kind == same::BackendKind::cuda, true, kind};
                    if (++entered == 2)
                        both.set_value();
                    gate.wait();
                    return w.index;
                },
                2048, kind);
        };
        auto first = submit();
        auto second = submit();
        const auto ready = both.get_future().wait_for(std::chrono::seconds(5));
        release.set_value();
        const auto first_index = first.get();
        const auto second_index = second.get();
        require(ready == std::future_status::ready && first_index != second_index,
                "training did not reach both workers");
    }
    pool.wait_idle();
    const auto profiles = pool.worker_profiles();
    if (cuda_calls != 2 || igpu_calls != 2 || pool.igpu_peak_concurrency() < 2)
        throw std::runtime_error(
            "not every worker owned both accelerators: cuda=" + std::to_string(cuda_calls.load()) +
            " igpu=" + std::to_string(igpu_calls.load()) +
            " peak=" + std::to_string(pool.igpu_peak_concurrency()) +
            " gpu_peak=" + std::to_string(pool.gpu_peak_concurrency()) +
            " selections=" + std::to_string(profiles[0].igpu_selections) + "," +
            std::to_string(profiles[1].igpu_selections) + " decisions=" +
            std::to_string(profiles[0].decisions[2][0] + profiles[0].decisions[2][1] +
                           profiles[0].decisions[2][2]) +
            "," +
            std::to_string(profiles[1].decisions[2][0] + profiles[1].decisions[2][1] +
                           profiles[1].decisions[2][2]) +
            " prior=" + std::to_string(profiles[0].prior[2].samples) + "," +
            std::to_string(profiles[1].prior[2].samples));
    for (const auto& profile : profiles)
        for (unsigned slot = 0; slot < 3; ++slot)
            require(profile.delta[slot].samples == 1 &&
                        profile.devices[slot].backend == static_cast<same::BackendKind>(slot),
                    "worker-local three-backend model lost a label or device identity");
}
/// 默认级预算不能只为一个核显实例预留却允许所有工作线程分配。
/// Pool budgets reserve all worker iGPU instances without losing normal batch size.
void shared_budget() {
    auto c = config();
    c.backend = "auto";
    c.workers = 12;
    c.block_bytes = 1ULL << 20;
    c.memory_bytes = c.device_memory_bytes = 64ULL << 20;
    same::Resources pool(
        c, [](auto, auto) { return std::make_unique<Device>(same::BackendKind::cuda); },
        [](auto, auto) { return std::make_unique<Device>(same::BackendKind::igpu); });
    pool.wait_idle();
    const auto profiles = pool.worker_profiles();
    std::uint64_t host = 0, device = 0;
    const auto buffers = [](std::size_t block) { return 2 * block + block / 32 + 4096; };
    for (const auto& p : profiles) {
        require(p.igpu_block_bytes == c.block_bytes && p.gpu_block_bytes > 0 &&
                    p.igpu_budget_bytes >= p.igpu_block_bytes,
                "per-worker iGPU reservation shrank a supported default workload");
        host += buffers(p.cpu_block_bytes) + buffers(p.gpu_block_bytes) + p.igpu_block_bytes / 32 +
                4096 + p.igpu_budget_bytes;
        device += p.igpu_budget_bytes + p.device_budget_bytes;
    }
    require(profiles.size() == c.workers && host <= c.memory_bytes &&
                device <= c.device_memory_bytes,
            "multi-worker CPU/CUDA/iGPU reservations exceed pool budgets");
}
/// 载入三设备先验后，每个线程在实际争用下都能选择核显而非被池级闸门挡回 CPU/CUDA。
/// With all priors loaded, each worker can route to iGPU under contention without a pool gate.
void auto_parallel_igpu() {
    auto c = config();
    c.backend = "auto";
    same::Resources pool(
        c, [](auto, auto) { return std::make_unique<Device>(same::BackendKind::cuda); },
        [](auto block, auto) { return std::make_unique<Device>(same::BackendKind::igpu, block); },
        [](same::BackendKind kind, const same::DeviceProfile& profile) {
            same::detail::OnlineModel prior;
            const double ms = kind == same::BackendKind::cpu    ? 100.0
                              : kind == same::BackendKind::cuda ? 50.0
                                                                : 1.0;
            for (auto bytes : {64ULL << 20, 128ULL << 20, 256ULL << 20})
                for (unsigned peers = 0; peers < 3; ++peers)
                    prior.observe(
                        kind, {bytes, profile.effective_batch_bytes, static_cast<double>(peers)},
                        ms);
            return prior.delta();
        });
    constexpr auto bytes = 128ULL << 20;
    for (auto kind : {same::BackendKind::cuda, same::BackendKind::igpu}) {
        std::atomic<unsigned> active{};
        std::promise<void> both, release;
        auto gate = release.get_future().share();
        const auto submit = [&] {
            return pool.submit_training_hash(
                [&](same::Worker& w) {
                    require(w.compute->kind() == kind, "cannot warm requested backend");
                    if (++active == 2)
                        both.set_value();
                    gate.wait();
                    return w.index;
                },
                bytes, kind);
        };
        auto first = submit(), second = submit();
        const auto ready = both.get_future().wait_for(std::chrono::seconds(5));
        release.set_value();
        require(ready == std::future_status::ready && first.get() != second.get(),
                "both workers did not load accelerator priors");
    }
    std::atomic<unsigned> active{};
    std::promise<void> both, release;
    auto gate = release.get_future().share();
    const auto submit = [&] {
        return pool.submit_hash(
            [&](same::Worker& w) {
                if (++active == 2)
                    both.set_value();
                gate.wait();
                w.sample = {bytes, 1, false, true, same::BackendKind::igpu};
                return std::pair{w.index, w.compute->kind()};
            },
            bytes);
    };
    auto first = submit(), second = submit();
    const auto ready = both.get_future().wait_for(std::chrono::seconds(5));
    release.set_value();
    const auto a = first.get(), b = second.get();
    pool.wait_idle();
    require(ready == std::future_status::ready && a.first != b.first &&
                a.second == same::BackendKind::igpu && b.second == same::BackendKind::igpu &&
                pool.igpu_peak_concurrency() >= 2,
            "online model did not route both workers to the preferred iGPU");
    const auto profiles = pool.worker_profiles();
    require(profiles[a.first].delta[2].samples == 1 && profiles[b.first].delta[2].samples == 1 &&
                profiles[a.first].learning_context.contention > 0 &&
                profiles[b.first].learning_context.contention > 0,
            "parallel auto iGPU samples lost actual contention or owner identity");
}
/// 两个工作线程在各自核显实例上同时处理文件，并各自学习真实争用上下文。
/// Two owners run iGPU work concurrently and learn their actual contention contexts.
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
            w.sample = {1024, 3, false, true, same::BackendKind::igpu};
            return w.index;
        },
        1024);
    require(entered.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "first iGPU worker never started");
    auto second = pool.submit_hash(
        [](same::Worker& w) {
            require(w.compute->kind() == same::BackendKind::igpu, "second worker lost iGPU");
            w.sample = {1024, 2, false, true, same::BackendKind::igpu};
            return w.index;
        },
        1024);
    const auto ready = second.wait_for(std::chrono::seconds(5));
    release.set_value();
    const auto first_index = first.get();
    const auto second_index = second.get();
    require(ready == std::future_status::ready && first_index != second_index,
            "one iGPU file task serialized another worker");
    pool.wait_idle();
    const auto profiles = pool.worker_profiles();
    require(calls == 2 && pool.gpu_peak_concurrency() == 0 && pool.igpu_peak_concurrency() >= 2 &&
                pool.profile_snapshot().igpu_samples == 2,
            "iGPU resources, peak, or samples still pooled as one instance");
    require(profiles[first_index].delta[2].samples == 1 &&
                profiles[second_index].delta[2].samples == 1 &&
                profiles[second_index].decision_context.contention >= 1,
            "parallel iGPU observations lost per-worker identity or contention");
}

/// 强制核显不把另一个线程正在探测误当成设备不可用。
/// Forced iGPU waits for a peer's one-time probe rather than silently using CPU.
void forced_probe_waits() {
    auto c = config();
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    std::promise<void> first_active, finish_first;
    auto finish_gate = finish_first.get_future().share();
    std::atomic<unsigned> probes{}, factories{};
    same::Resources pool(
        c, {},
        [&](std::size_t block, std::size_t) {
            ++factories;
            return std::make_unique<Device>(same::BackendKind::igpu, block);
        },
        {}, {},
        [&](std::size_t block, std::size_t) -> std::optional<same::DeviceProfile> {
            ++probes;
            entered.set_value();
            gate.wait();
            return Device(same::BackendKind::igpu, block).profile();
        });
    auto first = pool.submit_hash(
        [&](same::Worker& w) {
            first_active.set_value();
            finish_gate.wait();
            return w.compute->kind();
        },
        1024);
    require(entered.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "first forced iGPU probe did not start");
    auto second = pool.submit_hash([](same::Worker& w) { return w.compute->kind(); }, 1024);
    release.set_value();
    const auto active = first_active.get_future().wait_for(std::chrono::seconds(5));
    const auto ready = second.wait_for(std::chrono::seconds(5));
    finish_first.set_value();
    require(active == std::future_status::ready && ready == std::future_status::ready &&
                first.get() == same::BackendKind::igpu && second.get() == same::BackendKind::igpu,
            "forced iGPU fell back solely because discovery was busy");
    pool.wait_idle();
    require(probes == 1 && factories == 2, "probe was repeated or owner backend omitted");
}

/// 所属线程的设备错误不能退休其他线程的核显。 / One owner's failure cannot retire a peer iGPU.
void failure_isolation() {
    auto c = config();
    std::atomic<unsigned> calls{};
    same::Resources pool(c, {}, [&](auto, auto) {
        ++calls;
        return std::make_unique<Device>(same::BackendKind::igpu);
    });
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    auto failed = pool.submit_hash(
        [&](same::Worker& w) {
            require(w.compute->kind() == same::BackendKind::igpu, "first worker lacks iGPU");
            entered.set_value();
            gate.wait();
            unsigned attempts = 0;
            return std::pair{w.index, w.execute([&] {
                                 if (++attempts == 1)
                                     throw same::ComputeError("injected device failure");
                                 require(w.compute->kind() == same::BackendKind::cpu,
                                         "failed file was not retried on CPU");
                                 return attempts;
                             })};
        },
        1024);
    require(entered.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "failure fixture did not start");
    auto healthy = pool.submit_hash(
        [](same::Worker& w) {
            require(w.compute->kind() == same::BackendKind::igpu,
                    "peer iGPU was excluded before failure");
            return w.index;
        },
        1024);
    const auto ready = healthy.wait_for(std::chrono::seconds(5));
    release.set_value();
    const auto [failed_index, attempts] = failed.get();
    const auto healthy_index = healthy.get();
    pool.wait_idle();
    const auto profiles = pool.worker_profiles();
    require(ready == std::future_status::ready && attempts == 2 && failed_index != healthy_index &&
                calls == 2 && pool.fallbacks() == 1 && pool.igpu_service_enabled() &&
                profiles[healthy_index].igpu_enabled && !profiles[failed_index].igpu_enabled,
            "worker-local iGPU failure poisoned the peer or skipped complete retry");
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
/// 四种配置意图保持独立；非哈希任务仅在显式设备模式下使用设备。
/// Four configured intents stay distinct; generic work uses a device only when forced.
void backend_policy() {
    for (const auto* name : {"cpu", "auto", "cuda", "igpu"}) {
        auto c = config();
        c.workers = 1;
        c.backend = name;
        c.pgo = false;
        unsigned cuda_calls = 0, igpu_calls = 0;
        same::Resources pool(
            c,
            [&](auto, auto) -> std::unique_ptr<same::Compute> {
                ++cuda_calls;
                return std::make_unique<Device>(same::BackendKind::cuda);
            },
            [&](auto, auto) -> std::unique_ptr<same::Compute> {
                ++igpu_calls;
                return std::make_unique<Device>(same::BackendKind::igpu);
            });
        const auto generic = pool.submit([](same::Worker& w) { return w.compute->kind(); }).get();
        const auto small =
            pool.submit_hash([](same::Worker& w) { return w.compute->kind(); }, 1024).get();
        const auto large = pool.submit_hash([](same::Worker& w) { return w.compute->kind(); },
                                            same::detail::RoutingParameters::static_gpu_floor_bytes)
                               .get();
        pool.wait_idle();
        const std::string_view policy{name};
        const auto forced = policy == "cuda"   ? same::BackendKind::cuda
                            : policy == "igpu" ? same::BackendKind::igpu
                                               : same::BackendKind::cpu;
        require(generic == forced, "generic work violated configured backend policy");
        require(small == forced, "small hash violated configured backend policy");
        require(large == (policy == "auto" ? same::BackendKind::cuda : forced),
                "large hash violated configured backend policy");
        require((cuda_calls > 0) == (policy == "auto" || policy == "cuda") &&
                    (igpu_calls > 0) == (policy == "igpu"),
                "backend policy initialized an ineligible device");
    }
}
} // namespace
int main() {
    try {
        admission();
        forced_probe_waits();
        failure_isolation();
        unavailable();
        contextual_prior();
        lazy_prior_reselection();
        startup_credit();
        startup_prior(false);
        startup_prior(true);
        short_scan_discovery(true);
        short_scan_discovery(false);
        exploration();
        training_override();
        three_backend_workers();
        shared_budget();
        auto_parallel_igpu();
        backend_policy();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
