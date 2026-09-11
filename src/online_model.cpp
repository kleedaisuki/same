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
    return (std::bit_width(bytes) - 1) / OnlineModel::band_shift;
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
    return old * (1.0 - OnlineModel::smoothing_alpha) + value * OnlineModel::smoothing_alpha;
}
} // namespace
OnlineModel::Prediction OnlineModel::predict(BackendKind backend,
                                             std::uint64_t bytes) const noexcept {
    if (!bytes || static_cast<unsigned>(backend) >= backend_count)
        return {};
    const auto& band = bands_[static_cast<unsigned>(backend)][band_index(bytes)];
    const double ms = band.cost * static_cast<double>(bytes);
    const double error = band.error * static_cast<double>(bytes);
    if (!band.known || !std::isfinite(ms) || !std::isfinite(error) || ms <= 0)
        return {};
    return {ms, error, band.samples, true};
}
bool OnlineModel::seed(BackendKind backend, std::uint64_t bytes, double service_ms) noexcept {
    if (static_cast<unsigned>(backend) >= backend_count || !valid(bytes, service_ms))
        return false;
    auto& band = bands_[static_cast<unsigned>(backend)][band_index(bytes)];
    if (band.known)
        return false;
    band.cost = service_ms / static_cast<double>(bytes);
    band.known = true;
    return true;
}
bool OnlineModel::observe(BackendKind backend, std::uint64_t bytes, double service_ms) noexcept {
    if (static_cast<unsigned>(backend) >= backend_count || !valid(bytes, service_ms)) {
        increment(totals_.rejected_samples);
        return false;
    }
    auto& band = bands_[static_cast<unsigned>(backend)][band_index(bytes)];
    const auto before = predict(backend, bytes);
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
    increment(backend == BackendKind::igpu   ? totals_.igpu_samples
              : backend == BackendKind::cuda ? totals_.gpu_samples
                                             : totals_.cpu_samples);
    increment(totals_.latency_histogram[histogram_index(service_ms)]);
    return true;
}
OnlineModel::Snapshot OnlineModel::snapshot() const noexcept {
    auto result = totals_;
    for (const auto& band : bands_[0])
        result.cpu_known_bands += band.known;
    for (const auto& band : bands_[1])
        result.gpu_known_bands += band.known;
    for (const auto& band : bands_[2])
        result.igpu_known_bands += band.known;
    return result;
}
OnlineModel::Parameters OnlineModel::parameters() const noexcept {
    Parameters result{};
    for (unsigned backend = 0; backend < backend_count; ++backend) {
        for (unsigned index = 0; index < band_count; ++index) {
            const auto& band = bands_[backend][index];
            result[backend * band_count + index] = {band.cost,
                                                    band.error,
                                                    band.samples,
                                                    band.known,
                                                    index,
                                                    backend == 1,
                                                    static_cast<BackendKind>(backend)};
        }
    }
    return result;
}
} // namespace same::detail
#include <algorithm>

namespace same::detail {
namespace {
/// 固定尺度与硬界限限制数值条件数。 / Fixed scales and hard bounds limit numerical conditioning.
bool features(const OnlineModel::Context& c, std::array<double, 4>& x) noexcept {
    if (!c.bytes || !c.effective_batch_bytes || !std::isfinite(c.contention) || c.contention < 0 ||
        c.contention > 1024)
        return false;
    const auto batches =
        c.bytes / c.effective_batch_bytes + (c.bytes % c.effective_batch_bytes != 0);
    x = {1, static_cast<double>(c.bytes) / 1048576.0, static_cast<double>(batches) / 1024.0,
         static_cast<double>(c.bytes) / 1048576.0 * c.contention / 1024.0};
    for (double v : x)
        if (!std::isfinite(v) || v > 1e9)
            return false;
    return true;
}
/// 将一个非空统计块相加；调用方最后验证。 / Add a nonempty block; caller validates afterward.
void add_stats(OnlineModel::Statistics& a, const OnlineModel::Statistics& b) noexcept {
    if (!b.weight)
        return;
    for (unsigned i = 0; i < 4; ++i) {
        a.minimum[i] = a.weight ? std::min(a.minimum[i], b.minimum[i]) : b.minimum[i];
        a.maximum[i] = a.weight ? std::max(a.maximum[i], b.maximum[i]) : b.maximum[i];
        a.xty[i] += b.xty[i];
    }
    for (unsigned i = 0; i < 16; ++i)
        a.xtx[i] += b.xtx[i];
    a.yty += b.yty;
    a.weight += b.weight;
    a.samples = b.samples > UINT64_MAX - a.samples ? UINT64_MAX : a.samples + b.samples;
}
} // namespace
bool OnlineModel::valid_state(const State& state) noexcept {
    for (const auto& s : state) {
        auto bounded = [](double v) { return std::isfinite(v) && v >= 0 && v <= 1e32; };
        if (!bounded(s.weight) || s.weight > 1e12 || !bounded(s.yty))
            return false;
        if ((s.weight == 0) != (s.samples == 0))
            return false;
        for (unsigned i = 0; i < 4; ++i) {
            if (!bounded(s.xty[i]) || !bounded(s.minimum[i]) || !bounded(s.maximum[i]) ||
                s.minimum[i] > s.maximum[i] || s.maximum[i] > 1e9)
                return false;
            for (unsigned j = 0; j < 4; ++j)
                if (!bounded(s.xtx[i * 4 + j]) || s.xtx[i * 4 + j] != s.xtx[j * 4 + i])
                    return false;
        }
        if (s.xtx[0] != s.weight)
            return false;
        if (!s.weight) {
            for (double v : s.xtx)
                if (v)
                    return false;
            for (double v : s.xty)
                if (v)
                    return false;
            for (double v : s.minimum)
                if (v)
                    return false;
            for (double v : s.maximum)
                if (v)
                    return false;
            if (s.yty)
                return false;
        } else if (s.minimum[0] != 1 || s.maximum[0] != 1)
            return false;
        // 联合 Gram 矩阵必须半正定，包括响应列；允许舍入级误差。
        // The joint Gram matrix, including response, must be PSD up to rounding tolerance.
        if (s.weight) {
            // 单样本不能声明宽训练域；加权衰减不改变该样本的特征。
            // One sample cannot claim a broad training domain; decay preserves its features.
            for (unsigned i = 0; i < 4; ++i) {
                const double mean = s.xtx[i] / s.weight;
                const double tolerance = 1e-10 * std::max(1.0, std::abs(mean));
                if (mean < s.minimum[i] - tolerance || mean > s.maximum[i] + tolerance)
                    return false;
                if (s.samples == 1 && (std::abs(s.minimum[i] - mean) > tolerance ||
                                       std::abs(s.maximum[i] - mean) > tolerance))
                    return false;
            }
            std::array<double, 25> l{};
            std::array<double, 5> scale{};
            for (unsigned i = 0; i < 5; ++i)
                scale[i] = std::sqrt(std::max(i == 4 ? s.yty : s.xtx[i * 4 + i], 1e-24));
            for (unsigned i = 0; i < 5; ++i) {
                for (unsigned j = 0; j <= i; ++j) {
                    double v = i == 4 ? (j == 4 ? s.yty : s.xty[j]) : s.xtx[i * 4 + j];
                    v = v / scale[i] / scale[j] + (i == j ? 1e-8 : 0);
                    for (unsigned k = 0; k < j; ++k)
                        v -= l[i * 5 + k] * l[j * 5 + k];
                    if (!std::isfinite(v) || (i == j && v <= 0))
                        return false;
                    l[i * 5 + j] = i == j ? std::sqrt(v) : v / l[j * 5 + j];
                }
            }
        }
    }
    return true;
}
bool OnlineModel::merge(State& target, const State& addition) noexcept {
    if (!valid_state(target) || !valid_state(addition))
        return false;
    State next = target;
    for (unsigned b = 0; b < 3; ++b)
        add_stats(next[b], addition[b]);
    if (!valid_state(next))
        return false;
    target = next;
    return true;
}
bool OnlineModel::initialize(const State& prior, double decay) noexcept {
    if (!valid_state(prior) || !std::isfinite(decay) || decay <= 0 || decay > 1)
        return false;
    OnlineModel next;
    next.prior_ = prior;
    for (auto& s : next.prior_) {
        s.weight *= decay;
        s.yty *= decay;
        for (auto& v : s.xtx)
            v *= decay;
        for (auto& v : s.xty)
            v *= decay;
    }
    if (!valid_state(next.prior_))
        return false;
    for (unsigned b = 0; b < 3; ++b)
        if (next.prior_[b].weight && !next.solve(b))
            return false;
    *this = next;
    return true;
}
bool OnlineModel::solve(unsigned b) noexcept {
    auto s = prior_[b];
    add_stats(s, delta_[b]);
    if (!s.weight)
        return false;
    std::array<double, 16> l{};
    std::array<double, 4> scale{}, rhs{}, beta{};
    // 对角预条件和固定岭惩罚；不向原始统计量加入正则项。
    // Diagonal preconditioning and fixed ridge; raw statistics remain unregularized.
    for (unsigned i = 0; i < 4; ++i)
        scale[i] = std::sqrt(std::max(s.xtx[i * 4 + i], 1e-24));
    for (unsigned i = 0; i < 4; ++i) {
        rhs[i] = s.xty[i] / scale[i];
        for (unsigned j = 0; j <= i; ++j) {
            double v = s.xtx[i * 4 + j] / scale[i] / scale[j] + (i == j ? 1e-8 : 0);
            for (unsigned k = 0; k < j; ++k)
                v -= l[i * 4 + k] * l[j * 4 + k];
            if (i == j) {
                if (!std::isfinite(v) || v <= 0)
                    return false;
                l[i * 4 + j] = std::sqrt(v);
            } else
                l[i * 4 + j] = v / l[j * 4 + j];
        }
    }
    for (unsigned i = 0; i < 4; ++i) {
        for (unsigned j = 0; j < i; ++j)
            rhs[i] -= l[i * 4 + j] * rhs[j];
        rhs[i] /= l[i * 4 + i];
    }
    for (int i = 3; i >= 0; --i) {
        double v = rhs[i];
        for (unsigned j = i + 1; j < 4; ++j)
            v -= l[j * 4 + i] * beta[j];
        beta[i] = v / l[i * 4 + i];
    }
    for (unsigned i = 0; i < 4; ++i)
        beta[i] /= scale[i];
    double sse = s.yty;
    for (unsigned i = 0; i < 4; ++i) {
        if (!std::isfinite(beta[i]))
            return false;
        sse -= 2 * beta[i] * s.xty[i];
        for (unsigned j = 0; j < 4; ++j)
            sse += beta[i] * s.xtx[i * 4 + j] * beta[j];
    }
    if (!std::isfinite(sse) || sse < -1e-6 * std::max(1.0, s.yty))
        return false;
    fits_[b] = {beta, s.minimum, s.maximum, s.samples, std::sqrt(std::max(0.0, sse) / s.weight),
                true};
    return true;
}
OnlineModel::Prediction OnlineModel::predict(BackendKind backend,
                                             const Context& context) const noexcept {
    const auto b = static_cast<unsigned>(backend);
    std::array<double, 4> x{};
    if (b >= 3 || !features(context, x) || !fits_[b].valid)
        return {};
    const auto& fit = fits_[b];
    double ms = 0;
    bool outside = false;
    for (unsigned i = 0; i < 4; ++i) {
        ms += fits_[b].beta[i] * x[i];
        outside |= x[i] < fit.minimum[i] || x[i] > fit.maximum[i];
    }
    if (!std::isfinite(ms) || ms <= 0 || ms > 1e9)
        return {};
    return {ms, fit.residual, fit.samples, true, outside};
}
bool OnlineModel::observe(BackendKind backend, const Context& context, double ms) noexcept {
    const auto b = static_cast<unsigned>(backend);
    std::array<double, 4> x{};
    if (b >= 3 || !features(context, x) || !std::isfinite(ms) || ms <= 0 || ms > 1e9) {
        increment(totals_.rejected_samples);
        return false;
    }
    const auto before = predict(backend, context);
    Statistics s{};
    s.weight = 1;
    s.samples = 1;
    s.yty = ms * ms;
    s.minimum = x;
    s.maximum = x;
    for (unsigned i = 0; i < 4; ++i) {
        s.xty[i] = x[i] * ms;
        for (unsigned j = 0; j < 4; ++j)
            s.xtx[i * 4 + j] = x[i] * x[j];
    }
    auto next = delta_[b];
    add_stats(next, s);
    // 内部增量由外积构造，天然半正定；热路径只检查累加界限。
    // Internal deltas are outer products and hence PSD; hot path checks accumulation bounds only.
    bool bounded = next.weight <= 1e12 && next.yty <= 1e32;
    for (double value : next.xtx)
        bounded &= std::isfinite(value) && value <= 1e32;
    for (double value : next.xty)
        bounded &= std::isfinite(value) && value <= 1e32;
    if (!bounded) {
        increment(totals_.rejected_samples);
        increment(totals_.numerical_rejections);
        return false;
    }
    delta_[b] = next;
    if (before.known) {
        const double error = std::abs(ms - before.ms);
        totals_.absolute_error_sum_ms += error;
        totals_.squared_error_sum_ms2 += error * error;
        increment(totals_.predicted_samples);
        totals_.mean_absolute_error_ms = totals_.absolute_error_sum_ms / totals_.predicted_samples;
        increment(totals_.residual_histogram[histogram_index(error)]);
        if (before.out_of_domain)
            increment(totals_.out_of_domain_samples);
    }
    increment(totals_.samples);
    increment(b == 0 ? totals_.cpu_samples : b == 1 ? totals_.gpu_samples : totals_.igpu_samples);
    increment(totals_.latency_histogram[histogram_index(ms)]);
    if (delta_[b].samples <= 4 || delta_[b].samples % 16 == 0)
        if (!solve(b))
            increment(totals_.numerical_rejections);
    return true;
}
} // namespace same::detail
namespace same::detail {
bool OnlineModel::initialize_backend(BackendKind backend, const Statistics& prior,
                                     double decay) noexcept {
    const auto b = static_cast<unsigned>(backend);
    if (b >= 3)
        return false;
    State state{};
    state[b] = prior;
    OnlineModel checked;
    if (!checked.initialize(state, decay))
        return false;
    auto old = prior_[b];
    auto fit = fits_[b];
    prior_[b] = checked.prior_[b];
    if (!prior_[b].weight && !delta_[b].weight)
        fits_[b] = {};
    if ((prior_[b].weight || delta_[b].weight) && !solve(b)) {
        prior_[b] = old;
        fits_[b] = fit;
        return false;
    }
    return true;
}
} // namespace same::detail
