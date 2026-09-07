/** @file
 * @brief 可复现的主机驻留 CPU/CUDA 调度基准。 / Reproducible host-resident CPU/CUDA dispatch
 * benchmark.
 */
#include "same/compute.hpp"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <future>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
/// 单调墙钟，不是设备事件时间。 / Monotonic wall clock, not device event time.
using Clock = std::chrono::steady_clock;
/// 显式字节单位和有界实验参数。 / Explicit byte units and bounded experiment parameters.
struct Options {
    /// 每个工作线程每次处理的完整文件长度。 / Full file bytes per worker per repeat.
    std::size_t size = 4 * 1024 * 1024;
    /// update 的最大输入大小。 / Maximum input size per update.
    std::size_t block = 1024 * 1024;
    /// 每试次任务数为 workers 倍此值。 / Total trial jobs equal workers times this value.
    std::size_t repeats = 3;
    /// 并发后端实例数。 / Concurrent backend instances.
    std::size_t workers = 1;
    /// 配对试次数量，奇数轮反序。 / Paired trials, reversed on odd trials.
    std::size_t trials = 5;
    /// 每线程非计时预热次数。 / Untimed warmup hashes per worker.
    std::size_t warmup = 1;
    /// cpu/cuda/both/mixed；请求 CUDA 不可用即失败。 / CUDA requests fail if unavailable.
    std::string backend = "both";
};
/// 严格解析非负整数，拒绝单位后缀及溢出。 / Parse unsigned integers, rejecting suffixes/overflow.
std::size_t number(std::string_view text) {
    std::size_t value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        throw std::runtime_error("invalid unsigned integer");
    return value;
}
/// 解析命令行，避免误启动无界跟踪。 / Parse CLI and prevent unbounded trace runs.
Options parse(int argc, char** argv) {
    Options result;
    for (int i = 1; i < argc; i += 2) {
        const std::string_view key = argv[i];
        if (i + 1 == argc)
            throw std::runtime_error(
                "expected --size/--block/--repeats/--workers/--trials/--warmup/--backend VALUE");
        const std::string_view value = argv[i + 1];
        if (key == "--backend")
            result.backend = value;
        else if (key == "--size")
            result.size = number(value);
        else if (key == "--block")
            result.block = number(value);
        else if (key == "--repeats")
            result.repeats = number(value);
        else if (key == "--workers")
            result.workers = number(value);
        else if (key == "--trials")
            result.trials = number(value);
        else if (key == "--warmup")
            result.warmup = number(value);
        else
            throw std::runtime_error("unknown option: " + std::string(key));
    }
    if (result.size > 1024ULL * 1024 * 1024 || !result.block || result.block > 64 * 1024 * 1024 ||
        result.block % 1024)
        throw std::runtime_error(
            "size must be <=1GiB; block must be a 1024-byte multiple in [1024,64MiB]");
    if (!result.workers || result.workers > 256 || !result.repeats || result.repeats > 10000 ||
        !result.trials || result.trials > 1000 || result.warmup > 10000)
        throw std::runtime_error("workers/repeats/trials/warmup outside bounded limits");
    if (result.backend != "cpu" && result.backend != "cuda" && result.backend != "both" &&
        result.backend != "mixed")
        throw std::runtime_error("backend must be cpu, cuda, both, or mixed (paired CPU/mixed)");
    return result;
}
/// 顺序流式输入完整文件并取得全部摘要。 / Stream a complete file and obtain the full digest.
same::Digest hash(same::Compute& backend, std::span<const std::byte> input, std::size_t block) {
    auto state = backend.hasher();
    for (std::size_t offset = 0; offset < input.size(); offset += block)
        state->update(input.subspan(offset, std::min(block, input.size() - offset)));
    return state->finish();
}
/// 毫秒墙钟差。 / Wall-clock difference in milliseconds.
double milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
/// 长生命周期计算实例，初始化与稳态分离。 / Long-lived compute instances separate setup from steady
/// state.
struct Cohort {
    /// 实际后端名，不允许静默降级。 / Actual backend name; no silent fallback.
    std::string name;
    /// 一个实例只由一个任务使用。 / Each instance is used by exactly one concurrent task.
    std::vector<std::unique_ptr<same::Compute>> computes;
    /// 后端顺序构造时间，包含首次 CUDA 上下文初始化。 / Sequential construction time including
    /// first CUDA context setup.
    double init_ms{};
    /// 每工作线程本轮完成数；任务结束后读取。 / Per-worker completions, read after joining.
    std::vector<std::size_t> completed;
};
/// 分配工作线程私有后端，不包含输入生成或 CPU oracle。 / Allocate private backends excluding
/// input/oracle preparation.
Cohort create(std::string name, const Options& options) {
    Cohort result{std::move(name), {}, 0, std::vector<std::size_t>(options.workers)};
    const auto start = Clock::now();
    for (std::size_t i = 0; i < options.workers; ++i) {
        const std::string actual = result.name == "mixed" ? (i == 0 ? "cuda" : "cpu") : result.name;
        auto backend = actual == "cpu"
                           ? same::make_cpu_compute()
                           : same::try_cuda_compute(options.block, options.block * 3 + 65536);
        if (!backend || backend->name() != actual)
            throw std::runtime_error("requested backend unavailable: " + result.name);
        result.computes.push_back(std::move(backend));
    }
    result.init_ms = milliseconds(start);
    return result;
}
/// 线程预启动后统一释放；每次检查完整 32 字节摘要。 / Prestart threads, release together, validate
/// all 32 digest bytes on every hash.
double measure(Cohort& cohort, const Options& options, std::span<const std::byte> input,
               const same::Digest& expected, std::size_t repeats, bool fixed_warmup = false) {
    std::atomic<std::size_t> next{};
    std::fill(cohort.completed.begin(), cohort.completed.end(), 0);
    std::promise<void> gate;
    auto start_signal = gate.get_future().share();
    std::vector<std::future<void>> tasks;
    std::vector<std::future<void>> ready;
    try {
        for (std::size_t worker = 0; worker < cohort.computes.size(); ++worker) {
            auto& compute = cohort.computes[worker];
            std::promise<void> signal;
            ready.push_back(signal.get_future());
            tasks.push_back(std::async(std::launch::async, [&, worker, backend = compute.get(),
                                                            signal = std::move(signal),
                                                            start_signal]() mutable {
                signal.set_value();
                start_signal.wait();
                std::size_t local = 0;
                while (fixed_warmup ? local++ < repeats
                                    : next.fetch_add(1, std::memory_order_relaxed) <
                                          options.workers * repeats) {
                    if (hash(*backend, input, options.block) != expected)
                        throw std::runtime_error("full digest mismatch: " + cohort.name);
                    ++cohort.completed[worker];
                }
            }));
        }
    } catch (...) {
        gate.set_value();
        throw;
    }
    for (auto& future : ready)
        future.get();
    const auto start = Clock::now();
    gate.set_value();
    for (auto& future : tasks)
        future.get();
    return milliseconds(start);
}
/// 逐试次 JSONL；聚合吞吐包括所有工作线程的字节。 / Per-trial JSONL; aggregate throughput counts
/// every worker's bytes.
void emit(const Cohort& cohort, const Options& options, std::size_t trial, std::size_t order,
          double elapsed) {
    const double bytes = static_cast<double>(options.size) * options.workers * options.repeats;
    std::cout << "{\"backend\":\"" << cohort.name << "\",\"trial\":" << trial
              << ",\"order\":" << order << ",\"size\":" << options.size
              << ",\"block\":" << options.block << ",\"workers\":" << options.workers
              << ",\"repeats\":" << options.repeats << ",\"init_ms\":" << cohort.init_ms
              << ",\"steady_ms\":" << elapsed
              << ",\"aggregate_mib_s\":" << bytes / 1048576.0 * 1000.0 / elapsed
              << ",\"verified_digests\":" << options.workers * options.repeats
              << ",\"worker_jobs\":[";
    std::size_t cpu = 0, gpu = 0;
    for (std::size_t worker = 0; worker < cohort.completed.size(); ++worker) {
        if (worker)
            std::cout << ",";
        std::cout << cohort.completed[worker];
        (cohort.computes[worker]->name() == "cpu" ? cpu : gpu) += cohort.completed[worker];
    }
    std::cout << "],\"cpu_jobs\":" << cpu << ",\"cuda_jobs\":" << gpu << "}\n" << std::flush;
}
} // namespace
/// 运行单一配置，非零退出表示不能使用该测量。 / Run one configuration; nonzero exit invalidates the
/// measurement.
int main(int argc, char** argv) {
    try {
        const auto options = parse(argc, argv);
        std::vector<std::byte> input(options.size);
        std::uint32_t random = 20260907;
        for (auto& byte : input) {
            random = random * 1664525U + 1013904223U;
            byte = static_cast<std::byte>(random >> 24);
        }
        auto reference = same::make_cpu_compute();
        const auto expected = hash(*reference, input, 65537);
        std::vector<Cohort> cohorts;
        if (options.backend != "cuda")
            cohorts.push_back(create("cpu", options));
        if (options.backend != "cpu")
            cohorts.push_back(create(options.backend == "mixed" ? "mixed" : "cuda", options));
        std::cout << std::fixed << std::setprecision(6);
        std::cerr << "oracle=" << same::hex_digest(expected)
                  << " shared_host_input=1 warmup=" << options.warmup << '\n';
        for (auto& cohort : cohorts)
            measure(cohort, options, input, expected, options.warmup, true);
        for (std::size_t trial = 0; trial < options.trials; ++trial) {
            for (std::size_t order = 0; order < cohorts.size(); ++order) {
                auto& cohort = cohorts[trial % 2 ? cohorts.size() - 1 - order : order];
                emit(cohort, options, trial, order,
                     measure(cohort, options, input, expected, options.repeats));
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "dispatch benchmark: " << error.what() << '\n';
        return 2;
    }
}
