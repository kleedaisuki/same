#pragma once
#include "same/compute.hpp"
#include <array>
#include <cstdint>

namespace same::detail {
/// 固定空间的在线服务时间模型；外部同步，不读取时钟。
/// Fixed-space online service-time model; caller synchronizes and supplies durations.
/// Example: model.observe(false, 1048576, 0.5); model.predict(false, 1048576);
class OnlineModel {
public:
    /// 固定维度线性成本特征。 / Fixed-dimensional linear cost features.
    static constexpr unsigned feature_count = 4;
    /// 持久化特征编码版本。 / Persisted feature encoding version.
    static constexpr unsigned feature_version = 1;
    /// payload、有效批量与额外竞争者数。 / Payload, effective batch and extra concurrent peers.
    struct Context {
        std::uint64_t bytes{}, effective_batch_bytes{};
        double contention{};
    };
    /// 可加充分统计量；范围仅用于外推诊断。 / Additive sufficient statistics; ranges diagnose
    /// extrapolation.
    struct Statistics {
        std::array<double, 16> xtx{};
        std::array<double, 4> xty{}, minimum{}, maximum{};
        double yty{}, weight{};
        std::uint64_t samples{};
    };
    /// 顺序为 CPU/CUDA/iGPU。 / Ordered CPU/CUDA/iGPU.
    using State = std::array<Statistics, 3>;
    /// 新观测的 EWMA 权重；旧观测权重为 1-alpha。
    /// EWMA weight of a new observation; the previous estimate weighs 1-alpha.
    static constexpr double smoothing_alpha = 0.125;
    /// 每区间覆盖两个大小指数位，即四倍范围。
    /// Each band spans two size-exponent bits, a factor-four range.
    static constexpr unsigned band_shift = 2;
    /// 完整 uint64_t 大小空间的区间数。 / Band count for the complete uint64_t size space.
    static constexpr unsigned band_count = 64 / band_shift;
    /// CPU、CUDA 与 iGPU 独立后端。 / Independent CPU, CUDA and iGPU backends.
    static constexpr unsigned backend_count = 3;
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
        /// 实际后端，gpu 仅表示 CUDA。 / Actual backend; legacy gpu means CUDA only.
        BackendKind backend{BackendKind::cpu};
    };
    /// 固定大小导出，依次 CPU、CUDA、iGPU，各自按区间索引递增。
    /// Fixed-size export: CPU, CUDA, then iGPU, with ascending band indices within each backend.
    using Parameters = std::array<BandParameters, backend_count * band_count>;
    /// 相同四倍大小区间内的估计；未知不是零成本。
    /// Estimate within the same factor-four size band; unknown is not zero cost.
    struct Prediction {
        /// 超出训练特征包围盒；不代表统计置信度。 / Outside training feature bounds, not
        /// confidence. 预计服务时间与绝对残差 EWMA（毫秒），不是置信区间。 Predicted service time
        /// and absolute-residual EWMA in ms, not a confidence interval.
        double ms{}, error_ms{};
        /// 真实任务样本数，不包含校准先验。 / Task samples, excluding calibration priors.
        std::uint64_t samples{};
        /// 是否有有效先验或观测。 / Whether a valid prior or observation exists.
        bool known{};
        bool out_of_domain{};
    };
    /// 累计观测概要；直方图单位微秒，桶为 floor(log2(us))，两端饱和。
    /// Observation totals; histogram buckets use floor(log2(us)), saturated at both ends.
    struct Snapshot {
        /// 成功与拒绝的样本数；计数器饱和而不回绕。
        /// Accepted and rejected samples; counters saturate rather than wrap.
        std::uint64_t samples{}, cpu_samples{}, gpu_samples{}, igpu_samples{}, rejected_samples{};
        /// 已有预测的观测数及绝对预测误差 EWMA。
        /// Count of observations with a prior prediction and absolute prediction-error EWMA.
        std::uint64_t predicted_samples{};
        /// 含校准先验的有效区间数。 / Known bands including calibration priors.
        std::uint64_t cpu_known_bands{}, gpu_known_bands{}, igpu_known_bands{};
        double mean_absolute_error_ms{};
        /// 更新前残差总量及域外计数。 / Pre-update residual totals and out-of-domain count.
        double absolute_error_sum_ms{}, squared_error_sum_ms2{};
        std::uint64_t out_of_domain_samples{}, numerical_rejections{};
        /// 服务时间与预测残差的有界分布。 / Bounded service-time and residual distributions.
        std::array<std::uint64_t, 32> latency_histogram{}, residual_histogram{};
    };
    /// 只查询对应后端与大小区间，不向未测量区间外推。
    /// Query only this backend/size band; never extrapolate into unmeasured bands.
    Prediction predict(BackendKind backend, std::uint64_t bytes) const noexcept;
    /// 记录成功任务；拒绝零大小及非正或非有限时间。
    /// Record successful work; reject zero bytes and nonpositive/nonfinite durations.
    bool observe(BackendKind backend, std::uint64_t bytes, double service_ms) noexcept;
    /// 独立播种某一校准形状，不覆盖已有观测，不增加任务统计。
    /// Seed one calibration shape independently; preserve observations and task statistics.
    bool seed(BackendKind backend, std::uint64_t bytes, double service_ms) noexcept;
    /// 在外部锁内取得固定大小快照。 / Obtain a fixed-size snapshot under caller's lock.
    Snapshot snapshot() const noexcept;
    /// 在外部锁内或空闲后复制参数；不分配、不采样、不改变预测。
    /// Copy parameters under caller's lock or after idle; no allocation, sampling or mutation.
    /// Example: const auto bands = model.parameters(); // bands[0] is CPU band zero.
    Parameters parameters() const noexcept;

    /// 装入启动先验并衰减；清空本次增量。 / Load decayed startup prior and clear this run's delta.
    bool initialize(const State& prior, double decay = 1.0) noexcept;
    /// 延迟装入单设备先验，不重置其他设备或本次增量。 / Lazily load one device prior, preserving
    /// other devices and run deltas.
    bool initialize_backend(BackendKind backend, const Statistics& prior,
                            double decay = 1.0) noexcept;
    /// 只导出本次观测，合并时先验只能计入一次。 / Export run-only observations; merge the prior
    /// once.
    const State& delta() const noexcept {
        return delta_;
    }
    /// 已衰减的启动先验。 / Decayed startup prior.
    const State& prior() const noexcept {
        return prior_;
    }
    /// 事务性合并；非法或溢出输入不改变目标。 / Transactional merge; invalid/overflow input
    /// preserves target.
    static bool merge(State& target, const State& addition) noexcept;
    /// 验证外部充分统计量的数值域和结构。 / Validate external sufficient-statistic domain and
    /// structure.
    static bool valid_state(const State& state) noexcept;
    /// 固定维度岭回归预测；error_ms 是残差 RMS 而非置信区间。
    /// Fixed-dimensional ridge prediction; error_ms is residual RMS, not a confidence interval.
    Prediction predict(BackendKind backend, const Context& context) const noexcept;
    /// 在更新前记录预测残差，然后累加充分统计量。 / Record pre-update residual then sufficient
    /// statistics.
    bool observe(BackendKind backend, const Context& context, double service_ms) noexcept;

    /// 兼容 CUDA 布尔调用。 / Compatibility for CUDA boolean callers.
    Prediction predict(bool gpu, std::uint64_t bytes) const noexcept {
        return predict(gpu ? BackendKind::cuda : BackendKind::cpu, bytes);
    }

    /// 兼容 CUDA 布尔调用。 / Compatibility for CUDA boolean callers.
    bool observe(bool gpu, std::uint64_t bytes, double service_ms) noexcept {
        return observe(gpu ? BackendKind::cuda : BackendKind::cpu, bytes, service_ms);
    }

    /// 兼容 CUDA 布尔调用。 / Compatibility for CUDA boolean callers.
    bool seed(bool gpu, std::uint64_t bytes, double service_ms) noexcept {
        return seed(gpu ? BackendKind::cuda : BackendKind::cpu, bytes, service_ms);
    }

private:
    /// 启动先验与线程局部增量分离，避免多工作线程重复先验。
    /// Separate startup prior and thread-local delta prevent multiplying prior evidence.
    State prior_{}, delta_{};
    /// 缓存的回归系数与残差尺度。 / Cached regression coefficients and residual scale.
    struct Fit {
        std::array<double, 4> beta{};
        /// 域边界必须与缓存系数来自同一次拟合。 / Domain bounds must belong to the cached fit.
        std::array<double, 4> minimum{}, maximum{};
        std::uint64_t samples{};
        double residual{};
        bool valid{};
    };
    std::array<Fit, 3> fits_{};
    /// 小型 Cholesky 求解；失败不发布新系数。 / Small Cholesky solve; failures do not publish
    /// coefficients.
    bool solve(unsigned backend) noexcept;
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
