/** @file
 * @brief iGPU 准入、归属和回退的无硬件测试。 / Hardware-free iGPU admission and fallback tests.
 */
#include "same/resources.hpp"
#include <atomic>
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
    explicit Device(same::BackendKind kind) : kind_(kind) {}
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
    std::unique_ptr<same::Compute> cpu_{same::make_cpu_compute()};
};
/// 有界共享预算。 / Bounded shared budget.
same::Config config() {
    same::Config c;
    c.workers = 2;
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
/// 三设备各自探索且模型不混合。 / All three devices explored without model aliasing.
void exploration() {
    auto c = config();
    c.workers = 1;
    c.backend = "auto";
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
    require(pool.profile_snapshot().igpu_samples > 0, "iGPU sample attributed incorrectly");
}
} // namespace
int main() {
    try {
        admission();
        unavailable();
        exploration();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
