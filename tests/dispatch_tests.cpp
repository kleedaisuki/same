/** @file
 * @brief 自动分派收益门槛、无效测量及摘要校准契约。
 * Automatic dispatch margin, invalid measurements and calibration digest contracts.
 */
#include "same/detail/dispatch.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>
namespace {
/// 不依赖 NDEBUG 的断言。 / Assertions independent of NDEBUG.
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
/// 按研究阈值验证独立短/长证据，避免单块落败否决整个 GPU。
/// Research-band boundaries and independent short/long evidence, not one global GPU veto.
void payload_bands() {
    using namespace same::detail;
    constexpr std::size_t mib = 1024 * 1024;
    DispatchEvidence evidence;
    evidence.calibration_complete = true;
    evidence.stream_gpu_preferred = true;
    const auto route = [&](std::uint64_t size) {
        return classify_hash(size, 16 * mib, 16 * mib, evidence);
    };
    require(route(0) == HashRoute::cpu_only, "empty payload spilled");
    require(route(16 * mib - 1) == HashRoute::cpu_only, "small floor violated");
    require(route(16 * mib) == HashRoute::cpu_preferred, "unknown short advantage invented");
    require(route(64 * mib - 1) == HashRoute::cpu_preferred, "long evidence leaked below band");
    require(route(64 * mib) == HashRoute::gpu_preferred, "stream win vetoed by short loss");
    require(route(std::numeric_limits<std::uint64_t>::max()) == HashRoute::gpu_preferred,
            "large size overflow");
    evidence.block_gpu_preferred = true;
    evidence.stream_gpu_preferred = false;
    require(route(16 * mib) == HashRoute::gpu_preferred, "short evidence discarded");
    require(route(64 * mib) == HashRoute::cpu_preferred, "short win leaked into losing stream");
    evidence.calibration_complete = false;
    require(route(16 * mib) == HashRoute::cpu_preferred, "partial calibration preferred GPU");
    require(classify_hash(0, 0, mib, evidence) == HashRoute::cpu_only,
            "zero floor offloaded empty file");
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
/// 在指定摘要创建时越过期限，摘要仍由真实 CPU 后端验证。
/// Cross the deadline on a selected creation while retaining real digest correctness.
class DelayedCompute final : public same::Compute {
    /// 委托真实摘要；计数仅用于确定性选择超时阶段。
    /// Real digest delegate; the counter selects which stage times out.
    std::unique_ptr<same::Compute> cpu_{same::make_cpu_compute()};
    unsigned calls_{}, delayed_call_;

public:
    /// 指定预热或样本阶段。 / Select the warmup or sampling stage.
    explicit DelayedCompute(unsigned call) : delayed_call_(call) {}
    /// 一次延迟超过测试预算。 / One delay exceeds the test budget.
    std::unique_ptr<same::Hasher> hasher() override {
        if (++calls_ == delayed_call_)
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        return cpu_->hasher();
    }
    /// 保持计算接口语义。 / Preserve the compute interface contract.
    bool equal(std::span<const std::byte> a, std::span<const std::byte> b) override {
        return cpu_->equal(a, b);
    }
    /// 测试后端名称。 / Test backend name.
    std::string name() const override {
        return "delayed";
    }
};
/// 已完成的块样本在长流超时后仍有效，部分样本不能建立偏好。
/// Completed block evidence survives stream timeout; partial samples cannot establish preference.
void partial_evidence() {
    using namespace same::detail;
    auto cpu = same::make_cpu_compute();
    std::vector<std::byte> bytes(1024);
    DelayedCompute stream_timeout(5);
    const auto partial =
        calibrate_dispatch(*cpu, stream_timeout, bytes, std::chrono::milliseconds(200));
    require(partial.device_validated && partial.block_complete && !partial.stream_complete &&
                !partial.calibration_complete && partial.cpu_block_ms > 0 &&
                partial.gpu_block_ms > 0 && partial.cpu_stream_ms == 0 &&
                partial.stop_reason == DispatchEvidence::StopReason::deadline,
            "stream timeout discarded valid block evidence");
    DelayedCompute sample_timeout(2);
    const auto incomplete =
        calibrate_dispatch(*cpu, sample_timeout, bytes, std::chrono::milliseconds(200));
    require(incomplete.device_validated && !incomplete.block_complete &&
                !incomplete.block_gpu_preferred && incomplete.cpu_block_ms == 0,
            "incomplete paired rounds invented a preference");
    DispatchEvidence evidence;
    evidence.block_complete = true;
    evidence.block_gpu_preferred = true;
    evidence.stop_reason = DispatchEvidence::StopReason::deadline;
    require(classify_hash(1024, 1024, 1024, evidence) == HashRoute::gpu_preferred,
            "valid independent shape vetoed by global incompleteness");
    require(classify_hash(64ULL * 1024 * 1024, 1024, 1024, evidence) == HashRoute::cpu_preferred,
            "unknown stream borrowed block preference");
    evidence.decision = DispatchEvidence::Decision::failed;
    require(classify_hash(1024, 1024, 1024, evidence) == HashRoute::cpu_preferred,
            "failed device retained preference");
}
} // namespace
/// 验证决策边界与完整摘要检查。 / Verify decision boundaries and full digest checks.
int main() {
    try {
        payload_bands();
        partial_evidence();
        using same::detail::stable_gpu_win;
        require(stable_gpu_win({10, 10, 10}, {7, 8, 6}), "stable margin rejected");
        require(!stable_gpu_win({10, 10, 10}, {7, 8.01, 6}), "unstable margin accepted");
        require(!stable_gpu_win({10, 10, 10}, {1, 1, 11}), "one slow trial hidden by median");
        require(!stable_gpu_win({10, 100, 100}, {7, 70, 70}),
                "paired drift masked unstable preference");
        require(!stable_gpu_win({0, 10, 10}, {0, 1, 1}), "zero timings accepted");
        require(!stable_gpu_win({10, 10, 10}, {-1, 1, 1}), "negative timings accepted");
        require(!stable_gpu_win({std::numeric_limits<double>::infinity(), 10, 10}, {1, 1, 1}),
                "infinite timing accepted");
        require(!stable_gpu_win({10, 10, 10}, {std::numeric_limits<double>::quiet_NaN(), 1, 1}),
                "NaN timing accepted");
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
        require(report.device_validated && report.block_complete && report.stream_complete &&
                    !expired.device_validated &&
                    expired.stop_reason == same::detail::DispatchEvidence::StopReason::deadline &&
                    failed.stop_reason ==
                        same::detail::DispatchEvidence::StopReason::device_error &&
                    report.calibration_complete && !expired.calibration_complete &&
                    !failed.calibration_complete,
                "availability confused with partial calibration");
        // 同一字节流按不同CPU/GPU更新粒度仍必须完整校验成功。
        // Different actual CPU/GPU update sizes must validate the same complete byte stream.
        const auto segmented = same::detail::calibrate_dispatch(
            *cpu, *cpu, bytes, std::chrono::milliseconds(2000), 257);
        require(segmented.calibration_complete, "different backend segment sizes changed digest");
        std::cout << "dispatch margin, samples and digest validation passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
