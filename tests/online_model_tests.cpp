/** @file
 * @brief 在线模型的未知状态、漂移和有界统计契约。
 * Unknown-state, drift and bounded-statistics contracts for the online model.
 */
#include "same/detail/online_model.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
namespace {
/// 独立于 NDEBUG 的检查。 / Checks independent of NDEBUG.
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
/// 未知区间、独立后端与校准先验不污染真实计数。
/// Unknown bands, independent backends and priors do not pollute real sample counts.
void boundaries() {
    same::detail::OnlineModel model;
    require(!model.predict(false, 1024).known, "unknown prediction invented");
    require(model.seed(false, 1024, 1), "seed rejected");
    require(model.predict(false, 1024).known, "seed missing");
    require(model.predict(false, 1024).samples == 0, "seed counted as task");
    require(model.snapshot().samples == 0, "seed polluted totals");
    require(!model.predict(true, 1024).known, "backend evidence leaked");
    require(!model.predict(false, 1023).known, "lower band evidence leaked");
    require(model.predict(false, 4095).known, "same band missing");
    require(!model.predict(false, 4096).known, "upper band evidence leaked");
    require(!model.seed(false, 1024, 5), "existing evidence overwritten");
    require(model.observe(false, 2048, 2), "valid observation rejected");
    require(std::abs(model.predict(false, 1024).ms - 1) < 1e-12, "normalization wrong");
    require(model.observe(true, std::numeric_limits<std::uint64_t>::max(), 100),
            "maximum payload rejected");
    require(model.predict(true, std::numeric_limits<std::uint64_t>::max()).known,
            "maximum payload overflowed");
}
/// 失效测量不得改变估计；直方图包含所有合法观测。
/// Invalid samples do not alter estimates; histograms account for accepted observations.
void invalid_and_histograms() {
    same::detail::OnlineModel model;
    require(!model.observe(false, 0, 1), "zero size accepted");
    require(!model.observe(false, 1, 0), "zero duration accepted");
    require(!model.observe(false, 1, -1), "negative duration accepted");
    require(!model.observe(false, 1, std::numeric_limits<double>::infinity()), "infinity accepted");
    require(!model.observe(false, 1, std::numeric_limits<double>::quiet_NaN()), "NaN accepted");
    require(!model.observe(false, std::numeric_limits<std::uint64_t>::max(),
                           std::numeric_limits<double>::denorm_min()),
            "underflow accepted");
    require(model.observe(false, 1, 0.0001), "tiny valid duration rejected");
    require(model.observe(false, 1, 1), "duration rejected");
    require(model.observe(true, 1, std::numeric_limits<double>::max()), "finite maximum rejected");
    require(!model.predict(true, 3).known, "overflowing prediction treated as known");
    const auto stats = model.snapshot();
    require(stats.samples == 3 && stats.cpu_samples == 2 && stats.gpu_samples == 1,
            "backend totals wrong");
    require(stats.rejected_samples == 6, "rejection totals wrong");
    require(stats.predicted_samples == 1 && stats.mean_absolute_error_ms > 0,
            "prediction residual missing");
    require(std::accumulate(stats.latency_histogram.begin(), stats.latency_histogram.end(),
                            std::uint64_t{}) == 3,
            "latency count lost");
    require(stats.latency_histogram[0] == 1 && stats.latency_histogram[31] == 1,
            "histogram saturation wrong");
    require(std::accumulate(stats.residual_histogram.begin(), stats.residual_histogram.end(),
                            std::uint64_t{}) == 1,
            "residual count lost");
}
/// 稳态估计精确，有限新样本可响应速度变化。
/// Stationary estimates are exact and bounded new samples respond to speed drift.
void drift() {
    same::detail::OnlineModel model;
    for (int i = 0; i < 32; ++i)
        model.observe(false, 1048576, 1);
    require(model.predict(false, 1048576).ms == 1, "stationary estimate drifted");
    for (int i = 0; i < 24; ++i)
        model.observe(false, 1048576, 4);
    const auto prediction = model.predict(false, 1048576);
    require(prediction.ms > 3.8 && prediction.ms < 4, "drift response too slow or unstable");
    require(prediction.samples == 56 && prediction.error_ms > 0, "uncertainty missing");
}
} // namespace
int main() {
    try {
        boundaries();
        invalid_and_histograms();
        drift();
        std::cout << "online model tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
