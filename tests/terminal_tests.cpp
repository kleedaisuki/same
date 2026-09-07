#include "same/application.hpp"
#include "same/terminal.hpp"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
namespace {
/// Fail with a named invariant. 以具名不变量报告失败。
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
/// Isolated fixtures are removed even after a failed assertion.
/// 即使断言失败也清理隔离测试目录。
struct Fixture {
    /// Unique temporary root. 唯一临时根目录。
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("same-terminal-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    /// Create an empty fixture directory. 创建空测试目录。
    Fixture() {
        std::filesystem::create_directory(root);
    }
    /// Best-effort cleanup of this fixture only. 仅尽力清理本测试目录。
    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};
/// Extract integral byte counters without accepting missing fields.
/// 提取整数字节计数，拒绝字段缺失。
std::uint64_t counter(const std::string& text, const std::string& key) {
    auto pos = text.find(" " + key + "=");
    if (pos != std::string::npos)
        ++pos;
    else if (text.starts_with(key + "="))
        pos = 0;
    else {
        pos = text.find("\n" + key + "=");
        if (pos != std::string::npos)
            ++pos;
    }
    check(pos != std::string::npos, "missing profile field");
    return std::stoull(text.substr(pos + key.size() + 1));
}
/// Parse fractional timings by complete key and check the pipeline accounting invariant.
/// 按完整键解析小数耗时，并验证流水线计时恒等关系。
void check_scan_timing(const std::string& text, bool warm) {
    const auto metric = [&](const std::string& key) {
        auto pos = text.find(key + "=");
        check(pos != std::string::npos, "missing scan timing");
        const auto value = std::stod(text.substr(pos + key.size() + 1));
        check(std::isfinite(value) && value >= 0, "invalid scan timing");
        return value;
    };
    check(std::abs(metric("scan_ms") - metric("scan_work_ms") - metric("hash_wait_ms")) < 0.002,
          "pipeline timing accounting");
    if (warm)
        check(metric("hash_work_ms") == 0 && metric("hash_wait_ms") == 0, "warm hash timing");
    else
        check(metric("hash_work_ms") > 0, "cold hash timing");
}
/// Exercise plain/color parity, legacy TSV and cold/warm byte accounting.
/// 验证有色/无色一致性、旧 TSV 以及冷/热缓存字节统计。
void reports() {
    Fixture f;
    std::ofstream(f.root / "a") << "abcd";
    std::ofstream(f.root / "b") << "abcd";
    std::ofstream(f.root / "unique") << "xyz";
    same::Config config;
    config.backend = "cpu";
    config.workers = 1;
    std::ostringstream out, err;
    same::run(f.root, config, out, err);
    check(out.str() == "1\t\"a\"\n1\t\"b\"\n", "legacy TSV changed");
    check(counter(err.str(), "scanned_bytes") == 11, "logical bytes");
    check(counter(err.str(), "hash_read_bytes") == 11, "cold hash bytes");
    check(counter(err.str(), "compare_read_bytes") == 8, "comparison counts both files");
    check(counter(err.str(), "read_bytes") == 19, "total read bytes");
    check(counter(err.str(), "unique") == 1, "unique count");
    check_scan_timing(err.str(), false);
    check(counter(err.str(), "database_bytes") ==
              std::filesystem::file_size(f.root / ".same/state.db"),
          "database file size");
    check(counter(err.str(), "database_records") == 3, "database records");
    for (auto key : {"init_ms", "scan_ms", "compare_ms", "validate_ms", "output_ms", "elapsed_ms",
                     "read_mib_s"})
        check(err.str().find(std::string(key) + "=") != std::string::npos, "missing timing");
    out.str("");
    err.str("");
    same::run(f.root, config, out, err, {true, false});
    check(out.str().find("[UNIQUE]") == std::string::npos &&
              out.str().find("\"unique\"") == std::string::npos,
          "unique files not folded");
    check(out.str().find("Summary") == std::string::npos &&
              out.str().find("exact duplicate report") == std::string::npos,
          "removed banners");
    out.str("");
    err.str("");
    same::run(f.root, config, out, err, {true, false, true});
    const auto plain = out.str();
    check(plain.find("[SAME] Group 1") != std::string::npos, "same heading");
    check(plain.find("[UNIQUE]") != std::string::npos &&
              plain.find("\"unique\"") != std::string::npos,
          "unique path");
    check(plain.find('\033') == std::string::npos, "plain output escape");
    check(counter(err.str(), "hash_read_bytes") == 0 && counter(err.str(), "cached_bytes") == 11,
          "warm hash bytes");
    check(counter(err.str(), "compare_read_bytes") == 8, "warm comparison bytes");
    check_scan_timing(err.str(), true);
    out.str("");
    err.str("");
    same::run(f.root, config, out, err, {true, true, true, true, true});
    auto colored = out.str();
    check(colored.find("Summary") == std::string::npos, "summary leaked to results");
    check(err.str().find("\033[1;36mSummary\033[0m\n") != std::string::npos, "summary heading");
    check(err.str().find("Database") != std::string::npos &&
              err.str().find("3 records | committed") != std::string::npos,
          "database table row");
    check(err.str().find("Matching files") != std::string::npos &&
              err.str().find("Unique files") != std::string::npos &&
              err.str().find("Groups") != std::string::npos,
          "result table rows");
    check(err.str().find("wall time / logical reads") == std::string::npos &&
              err.str().find("Profile") == std::string::npos,
          "removed profile title");
    check(err.str().find(" B") != std::string::npos && err.str().find("/s") != std::string::npos,
          "human profile units");
    check(err.str().find("Hash work") != std::string::npos &&
              err.str().find("Hash wait") != std::string::npos &&
              err.str().find("Scan + hash") == std::string::npos,
          "split scan/hash display");
    check(err.str().find("elapsed_ms=") == std::string::npos, "raw metrics in pretty profile");
    check(colored.find("\033[32m") != std::string::npos &&
              colored.find("\033[33m") != std::string::npos,
          "color labels");
    for (auto code : {"\033[32m", "\033[33m", "\033[0m", "\033[1;36m"}) {
        std::size_t pos;
        while ((pos = colored.find(code)) != std::string::npos)
            colored.erase(pos, std::string_view(code).size());
    }
    check(colored == plain, "color changed report semantics");
    out.str("");
    err.str("");
    same::run(f.root, config, out, err, {false, false, true});
    check(out.str() == "1\t\"a\"\n1\t\"b\"\n0\t\"unique\"\n", "unique TSV group 0");
    out.str("");
    err.str("");
    same::run(f.root, config, out, err, {true, true, false, false, true});
    check(err.str().find('\033') == std::string::npos &&
              err.str().find("Summary") != std::string::npos,
          "independent plain diagnostic stream");
    out.str("");
    err.str("");
    same::run(f.root, config, out, err, {false, false, false, true, true});
    check(out.str() == "1\t\"a\"\n1\t\"b\"\n" && err.str().find('\033') != std::string::npos,
          "independent colored diagnostic stream");
    out.str("");
    err.str("");
    same::run(f.root, config, out, err, {true, false, false, false, true});
    check(err.str().find("1 (hidden; --unique-files to show)") != std::string::npos,
          "hidden unique hint in summary table");
    check(err.str().find(".same/state.db | " +
                         same::human_bytes(static_cast<double>(std::filesystem::file_size(
                             f.root / ".same/state.db")))) != std::string::npos,
          "actual database size in summary table");
    Fixture empty;
    out.str("");
    err.str("");
    same::run(empty.root, config, out, err, {true, false});
    check(counter(err.str(), "read_bytes") == 0, "empty read bytes");
    check(counter(err.str(), "database_records") == 0 && counter(err.str(), "database_bytes") > 0,
          "empty database metadata");
    check(out.str().empty(), "empty pretty output has no banner");
    std::filesystem::remove(f.root / "unique");
    out.str("");
    err.str("");
    same::run(f.root, config, out, err, {true, false});
    check(counter(err.str(), "database_records") == 2 &&
              out.str().find("Database") == std::string::npos,
          "deleted database record");
    check(err.str().find("nan") == std::string::npos && err.str().find("inf") == std::string::npos,
          "nonfinite rate");
}
/// Verify unit thresholds and zero/invalid input without timing-dependent assertions.
/// 不依赖真实耗时验证单位边界及零值、非法输入。
void units() {
    check(same::human_bytes(0) == "0 B", "zero bytes");
    check(same::human_bytes(1023) == "1023.00 B", "bytes boundary");
    check(same::human_bytes(1024) == "1.00 KiB", "KiB boundary");
    check(same::human_bytes(1048576) == "1.00 MiB", "MiB boundary");
    check(same::human_bytes(1073741824) == "1.00 GiB", "GiB boundary");
    check(same::human_bytes(1099511627776) == "1.00 TiB", "TiB boundary");
    check(same::human_duration(0) == "0 ms", "zero duration");
    check(same::human_duration(0.000001) == "1.00 ns", "ns duration");
    check(same::human_duration(0.001) == "1.00 us", "us boundary");
    check(same::human_duration(1) == "1.00 ms", "ms boundary");
    check(same::human_duration(1000) == "1.00 s", "seconds boundary");
    check(same::human_duration(60000) == "1.00 min", "minutes boundary");
    check(same::human_duration(3600000) == "1.00 h", "hours boundary");
    for (double invalid : {-1.0, std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()}) {
        bool bytes_rejected = false, time_rejected = false;
        try {
            same::human_bytes(invalid);
        } catch (const std::invalid_argument&) {
            bytes_rejected = true;
        }
        try {
            same::human_duration(invalid);
        } catch (const std::invalid_argument&) {
            time_rejected = true;
        }
        check(bytes_rejected && time_rejected, "invalid quantity accepted");
    }
}
} // namespace
/// Validate all color-policy combinations without requiring an interactive console.
/// 不依赖交互控制台验证所有颜色策略组合。
int main() {
    try {
        using same::ColorMode;
        for (bool terminal : {false, true})
            for (bool capable : {false, true})
                for (auto term : {"", "xterm", "dumb"})
                    for (auto no_color : {"", "1", "0"}) {
                        check(same::use_color(ColorMode::automatic, terminal, capable, term,
                                              no_color) ==
                                  (terminal && capable && std::string_view(term) != "dumb" &&
                                   std::string_view(no_color).empty()),
                              "auto policy");
                        check(same::use_color(ColorMode::always, terminal, capable, term, no_color),
                              "always policy");
                        check(!same::use_color(ColorMode::never, terminal, capable, term, no_color),
                              "never policy");
                    }
        check(same::parse_color("auto") == ColorMode::automatic, "parse auto");
        check(same::parse_color("always") == ColorMode::always, "parse always");
        check(same::parse_color("never") == ColorMode::never, "parse never");
        bool rejected = false;
        try {
            same::parse_color("bad");
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        check(rejected, "invalid color accepted");
        units();
        reports();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
