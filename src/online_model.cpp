#include "same/detail/online_model.hpp"
#include <bit>
#include <cmath>
#include <limits>

namespace same::detail {
namespace {
/// 饱和递增避免长期运行计数回绕。 / Saturating increment for long-lived counters.
void increment(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max())
        ++value;
}
/// 调用方保证 bytes 非零。 / Caller guarantees nonzero bytes.
unsigned band_index(std::uint64_t bytes) noexcept {
    return (std::bit_width(bytes) - 1) / 2;
}
/// 避免无限值乘法及浮点到整数的越界转换。
/// Avoid overflowing multiplication and out-of-range floating-to-integer conversion.
unsigned histogram_index(double ms) noexcept {
    if (ms < 0.002)
        return 0;
    if (ms >= 2147483.648)
        return 31;
    return std::bit_width(static_cast<std::uint64_t>(ms * 1000.0)) - 1;
}
/// 合法观测必须具有可表示的正归一化成本。
/// Valid observations require a representable positive normalized cost.
bool valid(std::uint64_t bytes, double ms) noexcept {
    return bytes && std::isfinite(ms) && ms > 0 && ms / static_cast<double>(bytes) > 0;
}
/// 凸组合形式避免两个大数相减造成不必要的溢出。
/// Convex-combination form avoids unnecessary overflow from subtraction.
double smooth(double old, double value) noexcept {
    return old * 0.875 + value * 0.125;
}
} // namespace
OnlineModel::Prediction OnlineModel::predict(bool gpu, std::uint64_t bytes) const noexcept {
    if (!bytes)
        return {};
    const auto& band = bands_[gpu][band_index(bytes)];
    const double ms = band.cost * static_cast<double>(bytes);
    const double error = band.error * static_cast<double>(bytes);
    if (!band.known || !std::isfinite(ms) || !std::isfinite(error) || ms <= 0)
        return {};
    return {ms, error, band.samples, true};
}
bool OnlineModel::seed(bool gpu, std::uint64_t bytes, double service_ms) noexcept {
    if (!valid(bytes, service_ms))
        return false;
    auto& band = bands_[gpu][band_index(bytes)];
    if (band.known)
        return false;
    band.cost = service_ms / static_cast<double>(bytes);
    band.known = true;
    return true;
}
bool OnlineModel::observe(bool gpu, std::uint64_t bytes, double service_ms) noexcept {
    if (!valid(bytes, service_ms)) {
        increment(totals_.rejected_samples);
        return false;
    }
    auto& band = bands_[gpu][band_index(bytes)];
    const auto before = predict(gpu, bytes);
    const double cost = service_ms / static_cast<double>(bytes);
    if (before.known) {
        const double residual = std::abs(service_ms - before.ms);
        band.error = smooth(band.error, residual / static_cast<double>(bytes));
        totals_.mean_absolute_error_ms =
            totals_.predicted_samples ? smooth(totals_.mean_absolute_error_ms, residual) : residual;
        increment(totals_.predicted_samples);
        increment(totals_.residual_histogram[histogram_index(residual)]);
    }
    band.cost = band.known ? smooth(band.cost, cost) : cost;
    band.known = true;
    increment(band.samples);
    increment(totals_.samples);
    increment(gpu ? totals_.gpu_samples : totals_.cpu_samples);
    increment(totals_.latency_histogram[histogram_index(service_ms)]);
    return true;
}
OnlineModel::Snapshot OnlineModel::snapshot() const noexcept {
    auto result = totals_;
    for (const auto& band : bands_[0])
        result.cpu_known_bands += band.known;
    for (const auto& band : bands_[1])
        result.gpu_known_bands += band.known;
    return result;
}
} // namespace same::detail
