#pragma once
#include <array>
#include <cstdint>

namespace same::detail {
/// 固定空间的在线服务时间模型；外部同步，不读取时钟。
/// Fixed-space online service-time model; caller synchronizes and supplies durations.
/// Example: model.observe(false, 1048576, 0.5); model.predict(false, 1048576);
class OnlineModel {
public:
    /// 新观测的 EWMA 权重；旧观测权重为 1-alpha。
    /// EWMA weight of a new observation; the previous estimate weighs 1-alpha.
    static constexpr double smoothing_alpha = 0.125;
    /// 每区间覆盖两个大小指数位，即四倍范围。
    /// Each band spans two size-exponent bits, a factor-four range.
    static constexpr unsigned band_shift = 2;
    /// 完整 uint64_t 大小空间的区间数。 / Band count for the complete uint64_t size space.
    static constexpr unsigned band_count = 64 / band_shift;
    /// CPU 与 GPU 两个独立后端。 / Two independent backends: CPU and GPU.
    static constexpr unsigned backend_count = 2;
    /// 单个区间的原始学习参数；未知区间保留零值但不代表零成本。
    /// Raw learned band parameters; unknown zero values do not imply zero cost.
    struct BandParameters {
        /// 每字节服务时间与绝对残差 EWMA（毫秒）。
        /// Per-byte service cost and absolute-residual EWMA, in milliseconds.
        double cost_ms_per_byte{}, error_ms_per_byte{};
        /// 不含校准先验的成功任务数。 / Successful task count excluding calibration priors.
        std::uint64_t samples{};
        /// 是否存在有效先验或观测。 / Whether a valid prior or observation exists.
        bool known{};
        /// 区间指数索引；不转换为可能溢出的有符号大小边界。
        /// Band exponent index; avoids conversion into overflowing signed size boundaries.
        unsigned band_index{};
        /// false 为 CPU，true 为 GPU。 / False identifies CPU, true identifies GPU.
        bool gpu{};
    };
    /// 固定大小导出，先 CPU 后 GPU，各自按区间索引递增。
    /// Fixed-size export: CPU then GPU, with ascending band indices within each backend.
    using Parameters = std::array<BandParameters, backend_count * band_count>;
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
    /// 在外部锁内或空闲后复制参数；不分配、不采样、不改变预测。
    /// Copy parameters under caller's lock or after idle; no allocation, sampling or mutation.
    /// Example: const auto bands = model.parameters(); // bands[0] is CPU band zero.
    Parameters parameters() const noexcept;

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
    std::array<std::array<Band, band_count>, backend_count> bands_{};
    /// 固定空间的累计诊断状态。 / Fixed-space cumulative diagnostic state.
    Snapshot totals_{};
};
} // namespace same::detail
