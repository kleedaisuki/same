/** @file
 * @brief 工作者私有模型、学习开关及同线程后端切换。 / Worker-local models, learning switch and
 * backend switching.
 */
#include "same/resources.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
using namespace std::chrono_literals;
namespace {
/// 保留失败上下文。 / Preserve failure context.
void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
/// 无硬件依赖的真实摘要后端。 / Real digest backend without a hardware dependency.
class Device final : public same::Compute {
public:
    /// 委托真实摘要。 / Delegate real hashing.
    std::unique_ptr<same::Hasher> hasher() override {
        return cpu_->hasher();
    }
    /// 委托真实比较。 / Delegate real comparison.
    bool equal(std::span<const std::byte> a, std::span<const std::byte> b) override {
        return cpu_->equal(a, b);
    }
    /// 模拟设备身份。 / Simulated device identity.
    std::string name() const override {
        return "cuda";
    }

private:
    /// 后端独占实现。 / Privately owned implementation.
    std::unique_ptr<same::Compute> cpu_{same::make_cpu_compute()};
};
/// 单工作者使路由决策可确定归因。 / One worker allows deterministic decision attribution.
same::Config config() {
    same::Config c;
    c.cuda_bootstrap_ms = 0;
    c.backend = "auto";
    c.workers = 1;
    c.block_bytes = 1024;
    c.gpu_min_bytes = 1024;
    c.memory_bytes = 128 * 1024 * 1024;
    c.device_memory_bytes = 128 * 1024 * 1024;
    c.queue_capacity = 4;
    return c;
}
/// 注入真实接口工厂。 / Inject a factory using the real interface.
same::detail::CudaFactory factory() {
    return [](std::size_t, std::size_t) { return std::make_unique<Device>(); };
}
/// 成功槽只由所属工作者消费；无效样本不学习。 / Only the owner consumes successful sample slots.
void samples(bool enabled) {
    auto c = config();
    c.pgo = enabled;
    same::Resources pool(c, factory());
    pool.submit_hash([](same::Worker& w) { w.sample = {1048576, 2, false, true}; }, 1048576).get();
    pool.wait_idle();
    require(pool.profile_snapshot().samples == (enabled ? 1 : 0),
            "sample learning switch violated");
    pool.submit_hash([](same::Worker& w) { w.sample = {1048576, 3, true, false}; }, 1048576).get();
    pool.wait_idle();
    require(pool.profile_snapshot().samples == (enabled ? 1 : 0), "invalid sample learned");
}
/// 模型可覆盖静态大小偏好，并在同一工作者上改变后端。 / Local evidence overrides size preference on
/// the same worker.
void switching() {
    auto c = config();
    same::Resources pool(c, factory());
    pool.submit([](same::Worker& w) {
            w.model.observe(same::BackendKind::cpu, {1048576, w.cpu_block_bytes, 0}, 30);
            w.model.observe(same::BackendKind::cuda, {1048576, w.gpu_block_bytes, 0}, 1);
        })
        .get();
    pool.wait_idle();
    require(pool.submit_hash([](same::Worker& w) { return w.compute->name(); }, 1048576).get() ==
                "cuda",
            "local model did not choose faster GPU");
    pool.wait_idle();
    pool.submit([](same::Worker& w) {
            for (int i = 0; i < 128; ++i) {
                w.model.observe(same::BackendKind::cpu, {1048576, w.cpu_block_bytes, 0}, 1);
                w.model.observe(same::BackendKind::cuda, {1048576, w.gpu_block_bytes, 0}, 30);
            }
        })
        .get();
    pool.wait_idle();
    require(pool.submit_hash([](same::Worker& w) { return w.compute->name(); }, 1048576).get() ==
                "cpu",
            "same worker did not adapt back to CPU");
}
/// 占住已训练工作者迫使第二个任务在新模型执行。 / Hold trained worker so second job observes a
/// distinct cold model.
void isolation() {
    auto c = config();
    c.workers = 2;
    same::Resources pool(c, factory());
    std::promise<void> started, release;
    auto gate = release.get_future().share();
    auto first = pool.submit([&](same::Worker& w) {
        w.model.observe(false, 1048576, 7);
        started.set_value();
        gate.wait();
        return std::pair{w.index, w.model.snapshot().samples};
    });
    const bool began = started.get_future().wait_for(5s) == std::future_status::ready;
    auto second =
        pool.submit([](same::Worker& w) { return std::pair{w.index, w.model.snapshot().samples}; });
    const bool independent = second.wait_for(5s) == std::future_status::ready;
    release.set_value();
    auto a = first.get();
    auto b = second.get();
    pool.wait_idle();
    require(began && independent, "one worker model operation blocked another worker");
    require(a.first != b.first && a.second == 1 && b.second == 0,
            "model history leaked between workers");
    require(pool.profile_snapshot().samples == 1, "aggregate profile lost local observation");
    const auto profiles = pool.worker_profiles();
    require(profiles.size() == 2, "snapshot omitted a worker model");
    require(profiles[a.first].snapshot.samples == 1 && profiles[b.first].snapshot.samples == 0,
            "snapshot merged histories between workers");
    constexpr auto cells =
        same::detail::OnlineModel::backend_count * same::detail::OnlineModel::band_count;
    require(profiles[a.first].parameters.size() == cells &&
                profiles[b.first].parameters.size() == cells,
            "worker snapshot omitted model bands");
}
} // namespace
/// 确定性断言失败返回非零。 / Deterministic failures return nonzero.
int main() {
    try {
        samples(true);
        samples(false);
        switching();
        isolation();
        std::cout << "worker-local model contracts passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
