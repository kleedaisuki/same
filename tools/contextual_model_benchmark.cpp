#include "same/detail/online_model.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string_view>

namespace {
using Model = same::detail::OnlineModel;
using Clock = std::chrono::steady_clock;
/// 可观察结果，避免消除测量循环。 / Observable result prevents dead-code elimination.
volatile double sink{};
/// 固定输入集覆盖大小、批量和竞争特征。 / Fixed inputs span sizes, batches and contention.
std::array<Model::Context, 256> contexts() {
    std::array<Model::Context, 256> result{};
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = {std::uint64_t{4096} << (i % 15), std::uint64_t{65536} << (i % 5),
                     double(i % 4)};
    return result;
}
/// 每例独立模型；计时不含初始化和输入构造。 / Independent model; setup is outside timing.
struct Trial {
    Model model;
    std::array<Model::Context, 256> input{contexts()};
    Trial() {
        for (unsigned pass = 0; pass < 4; ++pass)
            for (const auto& c : input) {
                for (auto b :
                     {same::BackendKind::cpu, same::BackendKind::cuda, same::BackendKind::igpu})
                    model.observe(b, c, 0.02 + double(c.bytes) / 1e7);
                model.observe(same::BackendKind::cpu, c.bytes, 0.02 + double(c.bytes) / 1e7);
            }
    }
    /// 分支在批次外；返回校验和。 / Dispatch outside loops; return checksum.
    template <unsigned Mode> double run(std::uint64_t count) {
        double sum = 0;
        for (std::uint64_t i = 0; i < count; ++i) {
            const auto& c = input[i & 255];
            if constexpr (Mode == 0)
                sum += double(c.bytes) / 1e7 + c.contention;
            if constexpr (Mode == 1)
                sum += model.predict(same::BackendKind::cpu, c.bytes).ms;
            if constexpr (Mode == 2)
                sum += model.predict(same::BackendKind::cpu, c).ms;
            if constexpr (Mode == 3)
                sum += model.observe(same::BackendKind::cpu, c.bytes, 0.02 + double(c.bytes) / 1e7);
            if constexpr (Mode == 4)
                sum += model.observe(same::BackendKind::cpu, c, 0.02 + double(c.bytes) / 1e7);
            if constexpr (Mode == 5) {
                for (auto b :
                     {same::BackendKind::cpu, same::BackendKind::cuda, same::BackendKind::igpu})
                    sum += model.predict(b, c).ms;
            }
        }
        return sum;
    }
};
/// 自适应批量、预热和九轮原始结果；报告绝对成本，不扣基线。
/// Adaptive batches, warmup and nine raw trials; absolute cost without baseline subtraction.
template <unsigned Mode> void measure(std::string_view name) {
    Trial calibration;
    std::uint64_t count = 4096;
    for (;;) {
        const auto start = Clock::now();
        sink = calibration.run<Mode>(count);
        if (Clock::now() - start >= std::chrono::milliseconds(100))
            break;
        count *= 2;
    }
    std::array<double, 9> times{};
    std::cout << name << ",iterations=" << count << ",ns/op=";
    for (auto& ns : times) {
        Trial trial;
        sink = trial.run<Mode>(4096);
        const auto start = Clock::now();
        sink = trial.run<Mode>(count);
        ns = std::chrono::duration<double, std::nano>(Clock::now() - start).count() / double(count);
        std::cout << ns << ';';
    }
    std::sort(times.begin(), times.end());
    std::cout << ",median=" << times[4] << '\n';
}
} // namespace
/// 独立微基准；不测文件 I/O 或调度收益。 / Standalone microbenchmark, not I/O or scheduling
/// benefit.
int main() {
    std::cout << std::fixed << std::setprecision(3) << "sizeof_model=" << sizeof(Model) << '\n';
    measure<0>("baseline");
    measure<1>("band_predict");
    measure<2>("context_predict");
    measure<3>("band_observe");
    measure<4>("context_observe_amortized");
    measure<5>("three_backend_predict");
}
