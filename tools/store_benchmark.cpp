/** @file
 * @brief 固定合成数据的 SQLite 仓储基准。 / SQLite store benchmark with deterministic synthetic
 * data. Usage: store_benchmark --rows 100000 --trials 3 --cache-bytes 2097152 --lookup legacy
 * Define SAME_STORE_FUSED_LOOKUP to enable --lookup fused on implementations with
 * mark_if_unchanged. 每试次新库，提交计入扫描耗时；不清除 OS 缓存，不测真实文件读取或哈希。 Fresh
 * database per trial; scans include commit. No OS cache flush, file reads, or hashing.
 * 缓存扫描按生产合同比较：legacy 检查 stamp，fused 检查命中；全部记录审核不计时。
 * Cache scans compare production contracts: legacy checks stamps, fused checks hits; full audits
 * are untimed.
 */
#include "same/store.hpp"
#include <charconv>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
/// 单调墙钟。 / Monotonic wall clock.
using Clock = std::chrono::steady_clock;
/// 有界工作负载配置。 / Bounded workload configuration.
struct Options {
    /// 总记录数，具体分布由 shape 指定。 / Total records; shape defines the distribution.
    std::size_t rows = 100000;
    /// 独立新库试次数。 / Independent fresh-database trials.
    std::size_t trials = 3;
    /// SQLite 页面缓存预算。 / SQLite page-cache budget.
    std::size_t cache_bytes = 2 * 1024 * 1024;
    /// 查找模式，fused 需要显式编译宏。 / Lookup mode; fused requires an explicit build macro.
    std::string lookup = "legacy";
    /// pairs 为半数重复对，single 为全体一桶，unique 为全体唯一。 / Pair mix, one bucket, or all
    /// unique.
    std::string shape = "pairs";
};
/// 不依赖 NDEBUG 的验证。 / Validation independent of NDEBUG.
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
/// 严格无符号十进制参数。 / Strict unsigned decimal argument.
std::size_t number(std::string_view text) {
    std::size_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    check(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size(), "invalid number");
    return value;
}
/// 限制数据量，保证路径格式和测试可控。 / Bound volume to preserve path format and tractability.
Options parse(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; i += 2) {
        check(i + 1 < argc, "expected --rows/--trials/--cache-bytes/--lookup/--shape VALUE");
        const std::string_view key = argv[i];
        if (key == "--lookup") {
            options.lookup = argv[i + 1];
            continue;
        }
        if (key == "--shape") {
            options.shape = argv[i + 1];
            continue;
        }
        const auto value = number(argv[i + 1]);
        if (key == "--rows")
            options.rows = value;
        else if (key == "--trials")
            options.trials = value;
        else if (key == "--cache-bytes")
            options.cache_bytes = value;
        else
            throw std::runtime_error("unknown option: " + std::string(key));
    }
    check(options.rows >= 4 && options.rows <= 10000000 && options.rows % 4 == 0,
          "rows must be a multiple of 4 in [4,10000000]");
    check(options.trials > 0 && options.trials <= 100, "trials must be in [1,100]");
    check(options.cache_bytes >= 16384 && options.cache_bytes <= 1024ULL * 1024 * 1024,
          "cache-bytes must be in [16384,1073741824]");
    check(options.lookup == "legacy" || options.lookup == "fused",
          "lookup must be legacy or fused");
#ifndef SAME_STORE_FUSED_LOOKUP
    check(options.lookup == "legacy", "fused requires SAME_STORE_FUSED_LOOKUP at build time");
#endif
    check(options.shape == "pairs" || options.shape == "single" || options.shape == "unique",
          "shape must be pairs, single, or unique");
    return options;
}
/// 仅清理自己原子创建的临时目录，不接受用户目标路径。 / Clean only our atomically created temp
/// directory.
class Scratch {
public:
    /// 重试名字冲突，不接管现有目录。 / Retry name collisions without adopting existing
    /// directories.
    Scratch() {
        const auto base = std::filesystem::temp_directory_path();
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            const auto name = "same-store-benchmark-" +
                              std::to_string(Clock::now().time_since_epoch().count()) + "-" +
                              std::to_string(attempt);
            const auto candidate = base / name;
            if (std::filesystem::create_directory(candidate)) {
                path = candidate;
                return;
            }
        }
        throw std::runtime_error("could not allocate benchmark directory");
    }
    /// Store 必须先销毁；清理失败仅警告。 / Destroy Store first; cleanup failure only warns.
    ~Scratch() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
        if (error)
            std::cerr << "scratch cleanup failed: " << error.message() << '\n';
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    /// 独占目录的绝对路径。 / Absolute path of the exclusively owned directory.
    std::filesystem::path path;
};
/// 摘要按大端组编号编码，候选顺序与记录顺序相同。 / Big-endian group digests preserve record order.
std::vector<same::FileRecord> dataset(const Options& options) {
    const auto rows = options.rows;
    std::vector<same::FileRecord> records;
    records.reserve(rows);
    for (std::size_t i = 0; i < rows; ++i) {
        same::FileRecord record;
        record.path = "synthetic/" + std::to_string(1000000000ULL + i).substr(1) + ".bin";
        record.stamp = {4096, "identity-" + std::to_string(i), "mtime-1", "ctime-1"};
        record.digest.fill(0);
        const auto group =
            static_cast<std::uint64_t>(options.shape == "single"                  ? 0
                                       : options.shape == "pairs" && i < rows / 2 ? i / 2
                                                                                  : i);
        for (unsigned byte = 0; byte < 8; ++byte)
            record.digest[byte] = static_cast<unsigned char>(group >> (56 - 8 * byte));
        records.push_back(std::move(record));
    }
    return records;
}
/// 全记录对照，防止仅计数掩盖损坏。 / Compare complete records, not merely row counts.
void verify(const same::FileRecord& actual, const same::FileRecord& expected) {
    check(actual.path == expected.path && actual.stamp == expected.stamp &&
              actual.digest == expected.digest,
          "record mismatch");
}
/// JSONL 逐阶段输出；验证和回调开销计时且所有版本一致。 / Per-phase JSONL; validation is timed
/// equally.
template <class Work>
void measure(const Options& options, std::size_t trial, std::string_view phase,
             std::size_t operations, Work work) {
    const auto start = Clock::now();
    work();
    const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    std::cout << "{\"benchmark\":\"store\",\"dataset_version\":1,\"trial\":" << trial
              << ",\"phase\":\"" << phase << "\",\"rows\":" << options.rows << ",\"lookup\":\""
              << options.lookup << "\""
              << ",\"shape\":\"" << options.shape << "\""
              << ",\"cache_bytes\":" << options.cache_bytes << ",\"operations\":" << operations
              << ",\"elapsed_ms\":" << elapsed << ",\"verified\":true}\n"
              << std::flush;
}
/// 各试次工作量相同；缓存扫描同时测量读取和 mark_seen。 / Identical trials; cache scan reads and
/// marks seen.
void trial(const Options& options, std::size_t index,
           const std::vector<same::FileRecord>& records) {
    Scratch scratch;
    same::Store store(scratch.path / "state.db", options.cache_bytes);
    const auto duplicates = options.shape == "single"  ? options.rows
                            : options.shape == "pairs" ? options.rows / 2
                                                       : 0;
    const auto group_size = options.shape == "single" ? options.rows : 2;
    measure(options, index, "initial_scan", options.rows, [&] {
        store.begin_scan();
        for (const auto& record : records)
            store.save(record);
        store.end_scan();
    });
    measure(options, index, "cache_hit_scan", options.rows, [&] {
        store.begin_scan();
        for (const auto& record : records) {
#ifdef SAME_STORE_FUSED_LOOKUP
            if (options.lookup == "fused") {
                check(store.mark_if_unchanged(record.path, record.stamp), "fused cache miss");
                continue;
            }
#endif
            const auto cached = store.cached(record.path);
            // 与生产缓存命中合同一致，仅比较 stamp；全记录审核位于计时外。
            // Match the production cache-hit contract: compare stamps; audit full records untimed.
            check(cached.has_value() && cached->stamp == record.stamp, "legacy cache miss");
            store.mark_seen(record.path);
        }
        store.end_scan();
    });
    measure(options, index, "visit_candidates", duplicates, [&] {
        std::size_t count = 0;
        store.visit_candidates([&](const auto& record) {
            check(count < duplicates, "excess candidate");
            verify(record, records[count++]);
        });
        check(count == duplicates, "candidate count mismatch");
    });
    measure(options, index, "add_matches", duplicates, [&] {
        store.reset_matches();
        for (std::size_t i = 0; i < duplicates; ++i)
            store.add_match(records[i - i % group_size].path, records[i].path);
    });
    measure(options, index, "visit_matches", duplicates, [&] {
        std::size_t count = 0;
        store.visit_matches([&](std::string_view representative, std::string_view member) {
            check(count < duplicates, "excess match");
            check(representative == records[count - count % group_size].path &&
                      member == records[count].path,
                  "match mismatch or ordering error");
            ++count;
        });
        check(count == duplicates, "match count mismatch");
    });
    measure(options, index, "visit_unique", options.rows - duplicates, [&] {
        std::size_t count = duplicates;
        store.visit_unique([&](std::string_view path) {
            check(count < records.size(), "excess unique row");
            check(path == records[count++].path, "unique mismatch or ordering error");
        });
        check(count == records.size(), "unique count mismatch");
    });
    // 计时外验证全部持久数据，覆盖 fused 未返回的摘要。 / Untimed full audit includes fused
    // digests.
    for (const auto& record : records) {
        const auto cached = store.cached(record.path);
        check(cached.has_value(), "missing row after benchmark");
        verify(*cached, record);
    }
}
} // namespace
/// 失败则全部结果无效；新库不等于冷 OS 缓存。 / Any failure invalidates results; fresh DB is not
/// cold OS cache.
int main(int argc, char** argv) {
    try {
        const auto options = parse(argc, argv);
        const auto records = dataset(options);
        std::cout << std::fixed << std::setprecision(6);
        std::cerr << "synthetic=1 shape=" << options.shape
                  << " validation_timed=1 "
                     "schema_init_timed=0 os_cache_flushed=0 temp_parent="
                  << std::filesystem::temp_directory_path() << '\n';
        for (std::size_t i = 0; i < options.trials; ++i)
            trial(options, i, records);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "store benchmark: " << error.what() << '\n';
        return 2;
    }
}
