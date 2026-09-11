/** @file
 * @brief 固定空间回归、先验合并与数值防护。 / Fixed-space regression, prior merge and numeric
 * guards.
 */
#include "same/detail/online_model.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
namespace {
/// 与发布构建无关的断言。 / Release-independent assertion.
void check(bool b) {
    if (!b)
        throw std::runtime_error("contextual model invariant failed");
}
/// 合成可辨识固定/字节/批量/竞争成本。 / Identifiable synthetic fixed/byte/batch/contention cost.
void regression() {
    using M = same::detail::OnlineModel;
    M model;
    for (unsigned n = 0; n < 256; ++n) {
        M::Context c{(1 + n % 23) * 1048576ull + 17, (1 + n % 7) * 65536ull, double(n % 5)};
        double batches =
            double(c.bytes / c.effective_batch_bytes + (c.bytes % c.effective_batch_bytes != 0)) /
            1024;
        double y = 2 + double(c.bytes) / 1048576 * .3 + batches * 7 +
                   double(c.bytes) / 1048576 * c.contention / 1024 * 40;
        check(model.observe(same::BackendKind::igpu, c, y));
    }
    M::Context c{8 * 1048576ull + 17, 3 * 65536ull, 2};
    auto p = model.predict(same::BackendKind::igpu, c);
    double y = 2 + double(c.bytes) / 1048576 * .3 +
               double(c.bytes / c.effective_batch_bytes + 1) / 1024 * 7 +
               double(c.bytes) / 1048576 * c.contention / 1024 * 40;
    check(p.known && std::abs(p.ms - y) < .001);
    M a, b;
    check(a.initialize(model.delta()) && b.initialize(model.delta()));
    check(a.delta()[2].samples == 0);
    check(a.observe(same::BackendKind::igpu, c, y));
    check(b.observe(same::BackendKind::igpu, c, y));
    auto total = a.prior();
    check(M::merge(total, a.delta()) && M::merge(total, b.delta()));
    check(total[2].samples == 258);
    M restored;
    check(restored.initialize(total, .9));
    check(std::abs(restored.predict(same::BackendKind::igpu, c).ms - y) < .001);
    check(!restored.observe(same::BackendKind::igpu, {1, 0, 0}, 1));
    check(!restored.observe(same::BackendKind::igpu, {1, 1, -1}, 1));
    auto corrupt = total;
    corrupt[2].xtx[1] += 1;
    check(!M::valid_state(corrupt));
    check(!restored.initialize(corrupt));
    check(restored.predict(same::BackendKind::igpu, c).known);
}
/// 共线输入仍稳定且域外可辨识。 / Collinear inputs remain stable with explicit extrapolation flag.
void collinear() {
    same::detail::OnlineModel m;
    for (unsigned i = 0; i < 4; ++i)
        check(m.observe(same::BackendKind::cpu, {1048576, 1048576, 0}, 2));
    check(m.observe(same::BackendKind::cpu, {2097152, 1048576, 0}, 4));
    check(m.predict(same::BackendKind::cpu, {2097152, 1048576, 0}).out_of_domain);
    check(m.predict(same::BackendKind::cpu, {1048576, 1048576, 0}).samples == 4);
    m = {};
    check(m.observe(same::BackendKind::cpu, {1048576, 1048576, 0}, 2));
    auto forged = m.delta();
    forged[0].maximum[1] = 100;
    check(!same::detail::OnlineModel::valid_state(forged));
    check(m.predict(same::BackendKind::cpu, {2097152, 1048576, 0}).out_of_domain);
    check(m.predict(same::BackendKind::cpu, {1048576, 1048576, 0}).error_ms < .001);
    m = {};
    for (unsigned i = 0; i < 32; ++i)
        check(m.observe(same::BackendKind::cpu, {1048576, 1048576, 0}, 2));
    auto p = m.predict(same::BackendKind::cpu, {1048576, 1048576, 0});
    check(p.known && std::abs(p.ms - 2) < 1e-5);
    check(m.predict(same::BackendKind::cpu, {2097152, 1048576, 0}).out_of_domain);
}
} // namespace
int main() {
    regression();
    collinear();
    std::cout << "contextual model tests passed\n";
}
