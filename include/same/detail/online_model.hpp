#pragma once
#include <array>
#include <cstdint>

namespace same::detail {
/// 固定空间的在线服务时间模型；外部同步，不读取时钟。
/// Fixed-space online service-time model; caller synchronizes and supplies durations.
/// Example: model.observe(false, 1048576, 0.5); model.predict(false, 1048576);
class OnlineModel {
public:
    /// 相同四倍大小区间内的估计；未知不是零成本。
    /// Estimate within the same factor-four size band; unknown is not zero cost.
    struct Prediction {
        /// 预计服务时间与绝对残差 EWMA（毫秒），不是置信区间。
        /// Predicted service time and absolute-residual EWMA in ms, not a confidence interval.
        double ms{}, error_ms{};
        /// 真实任务样本数，不包含校准先验。 / Task samples, excluding calibration priors.
        std::uint64_t samples{};
        /// 是否有有效先验或观测。 / Whether a valid prior or observation exists.
        bool known{};
    };
    /// 累计观测概要；直方图单位微秒，桶为 floor(log2(us))，两端饱和。
    /// Observation totals; histogram buckets use floor(log2(us)), saturated at both ends.
    struct Snapshot {
        /// 成功与拒绝的样本数；计数器饱和而不回绕。
        /// Accepted and rejected samples; counters saturate rather than wrap.
        std::uint64_t samples{}, cpu_samples{}, gpu_samples{}, rejected_samples{};
        /// 已有预测的观测数及绝对预测误差 EWMA。
        /// Count of observations with a prior prediction and absolute prediction-error EWMA.
        std::uint64_t predicted_samples{};
        /// 含校准先验的有效区间数。 / Known bands including calibration priors.
        std::uint64_t cpu_known_bands{}, gpu_known_bands{};
        double mean_absolute_error_ms{};
        /// 服务时间与预测残差的有界分布。 / Bounded service-time and residual distributions.
        std::array<std::uint64_t, 32> latency_histogram{}, residual_histogram{};
    };
    /// 只查询对应后端与大小区间，不向未测量区间外推。
    /// Query only this backend/size band; never extrapolate into unmeasured bands.
    Prediction predict(bool gpu, std::uint64_t bytes) const noexcept;
    /// 记录成功任务；拒绝零大小及非正或非有限时间。
    /// Record successful work; reject zero bytes and nonpositive/nonfinite durations.
    bool observe(bool gpu, std::uint64_t bytes, double service_ms) noexcept;
    /// 独立播种某一校准形状，不覆盖已有观测，不增加任务统计。
    /// Seed one calibration shape independently; preserve observations and task statistics.
    bool seed(bool gpu, std::uint64_t bytes, double service_ms) noexcept;
    /// 在外部锁内取得固定大小快照。 / Obtain a fixed-size snapshot under caller's lock.
    Snapshot snapshot() const noexcept;

private:
    /// 每字节成本及残差，alpha=1/8，约五个样本的半衰期。
    /// Per-byte cost/residual, alpha=1/8, approximately five-sample half-life.
    struct Band {
        double cost{}, error{};
        std::uint64_t samples{};
        bool known{};
    };
    /// 两后端各32个四倍区间，覆盖完整 uint64_t 范围。
    /// Thirty-two factor-four bands per backend cover the complete uint64_t range.
    std::array<std::array<Band, 32>, 2> bands_{};
    /// 固定空间的累计诊断状态。 / Fixed-space cumulative diagnostic state.
    Snapshot totals_{};
};
} // namespace same::detail
