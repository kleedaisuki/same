/** @file
 * @brief 自动分派收益门槛、无效测量及摘要校准契约。
 * Automatic dispatch margin, invalid measurements and calibration digest contracts.
 */
#include "same/detail/dispatch.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
namespace {
/// 不依赖 NDEBUG 的断言。 / Assertions independent of NDEBUG.
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
/// 错误摘要不可通过校准后悄悄参与文件处理。 / Incorrect digests must never pass calibration.
class WrongHasher final : public same::Hasher {
public:
    /// 故意忽略输入用于故障注入。 / Deliberately ignore input for fault injection.
    void update(std::span<const std::byte>) override {}
    /// 故意返回错误的零摘要。 / Deliberately return an incorrect zero digest.
    same::Digest finish() override {
        return {};
    }
};
/// 隔离摘要验证测试，不依赖实际 GPU。 / Isolate digest verification without a real GPU.
class WrongCompute final : public same::Compute {
    /// 区分设备异常和错误摘要两条拒绝路径。 / Distinguish device exceptions from wrong digests.
    bool throws_;

public:
    /// 默认注入错误摘要；可选择设备异常。 / Inject wrong digests by default, optionally device
    /// exceptions.
    explicit WrongCompute(bool throws = false) : throws_(throws) {}
    /// 创建故障摘要器。 / Create a faulty hasher.
    std::unique_ptr<same::Hasher> hasher() override {
        if (throws_)
            throw same::ComputeError("injected probe failure");
        return std::make_unique<WrongHasher>();
    }
    /// 校准不使用比较方法。 / Calibration never uses comparison.
    bool equal(std::span<const std::byte>, std::span<const std::byte>) override {
        return false;
    }
    /// 稳定故障名称。 / Stable fault name.
    std::string name() const override {
        return "fault";
    }
};
} // namespace
/// 验证决策边界与完整摘要检查。 / Verify decision boundaries and full digest checks.
int main() {
    try {
        using same::detail::WorkQueue;
        // 穷尽队列快照，包括关闭排空时仅剩固定任务的 CPU 空闲情形。
        // Exhaust every snapshot, including CPU idleness with only pinned work during draining.
        const std::array<WorkQueue, 16> expected{
            WorkQueue::none,   WorkQueue::normal, WorkQueue::eligible, WorkQueue::normal,
            WorkQueue::none,   WorkQueue::normal, WorkQueue::eligible, WorkQueue::normal,
            WorkQueue::none,   WorkQueue::normal, WorkQueue::eligible, WorkQueue::eligible,
            WorkQueue::pinned, WorkQueue::pinned, WorkQueue::pinned,   WorkQueue::pinned};
        for (unsigned mask = 0; mask < expected.size(); ++mask)
            require(same::detail::select_work_queue(mask & 8, mask & 4, mask & 2, mask & 1) ==
                        expected[mask],
                    "incorrect queue priority or stealing decision");
        using same::detail::stable_gpu_win;
        require(stable_gpu_win({10, 10, 10}, {7, 8, 6}), "stable margin rejected");
        require(!stable_gpu_win({10, 10, 10}, {7, 8.01, 6}), "unstable margin accepted");
        require(!stable_gpu_win({10, 10, 10}, {1, 1, 11}), "one slow trial hidden by median");
        require(!stable_gpu_win({0, 10, 10}, {0, 1, 1}), "zero timings accepted");
        require(!stable_gpu_win({10, 10, 10}, {-1, 1, 1}), "negative timings accepted");
        require(!stable_gpu_win({std::numeric_limits<double>::infinity(), 10, 10}, {1, 1, 1}),
                "infinite timing accepted");
        require(!stable_gpu_win({10, 10, 10}, {std::numeric_limits<double>::quiet_NaN(), 1, 1}),
                "NaN timing accepted");
        using same::detail::conservative_gpu_saving;
        using same::detail::gpu_setup_amortized;
        require(conservative_gpu_saving(10, 8, 1024, 4096) == 8, "wrong batch normalization");
        require(conservative_gpu_saving(10, 8.01, 1024, 4096) == 0, "weak mixed win accepted");
        require(conservative_gpu_saving(10, -1, 1024, 4096) == 0, "negative GPU accepted");
        require(conservative_gpu_saving(10, 8, 0, 4096) == 0, "zero sample accepted");
        require(conservative_gpu_saving(10, 8, 1024, 0) == 0, "empty pending accepted");
        require(conservative_gpu_saving(10, 8, std::numeric_limits<double>::infinity(), 4096) == 0,
                "infinite sample accepted");
        require(conservative_gpu_saving(std::numeric_limits<double>::quiet_NaN(), 8, 1024, 4096) ==
                    0,
                "NaN CPU accepted");
        require(conservative_gpu_saving(1e308, 1, 1, 4096) == 0, "overflow saving accepted");
        require(gpu_setup_amortized(8, 4), "amortization boundary rejected");
        require(!gpu_setup_amortized(7.99, 4), "unamortized setup accepted");
        require(!gpu_setup_amortized(0, 0), "zero saving accepted");
        require(!gpu_setup_amortized(8, -1), "negative setup accepted");
        require(!gpu_setup_amortized(std::numeric_limits<double>::infinity(), 4),
                "infinite savings accepted");
        require(!gpu_setup_amortized(8, std::numeric_limits<double>::quiet_NaN()),
                "NaN setup accepted");
        auto cpu = same::make_cpu_compute();
        WrongCompute wrong;
        WrongCompute failing(true);
        std::vector<std::byte> bytes(1024);
        const auto failed = same::detail::calibrate_dispatch(*cpu, failing, bytes);
        require(failed.decision == same::detail::DispatchEvidence::Decision::failed,
                "device probe failure must select the CPU failure path");
        const auto expired =
            same::detail::calibrate_dispatch(*cpu, wrong, bytes, std::chrono::milliseconds(0));
        require(expired.decision == same::detail::DispatchEvidence::Decision::cpu,
                "expired budget must not start the faulty backend");
        bool rejected = false;
        try {
            same::detail::calibrate_dispatch(*cpu, wrong, bytes);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected, "incorrect digest accepted");
        rejected = false;
        try {
            same::detail::calibrate_dispatch(*cpu, wrong, {});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "empty probe accepted");
        const auto report = same::detail::calibrate_dispatch(*cpu, *cpu, bytes);
        require(report.cpu_block_ms > 0 && report.gpu_block_ms > 0 && report.cpu_stream_ms > 0 &&
                    report.gpu_stream_ms > 0,
                "missing calibration evidence");
        require(report.cpu_stream_fastest_ms > 0 &&
                    report.cpu_stream_fastest_ms <= report.cpu_stream_ms &&
                    report.gpu_stream_slowest_ms >= report.gpu_stream_ms,
                "missing conservative serial extremes");
        require(report.elapsed_ms > 0, "missing calibration cost");
        std::cout << "dispatch margin, samples and digest validation passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
