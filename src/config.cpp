#include "same/config.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <toml++/toml.hpp>
namespace same {
namespace {
/// Missing settings use defaults; existing unreadable or oversized files are errors.
/// 缺少设置文件时使用默认值；已存在但无法读取或超限的文件属于错误。
std::optional<std::string> read_settings(const std::filesystem::path& path, std::size_t limit) {
    if (!std::filesystem::exists(path))
        return std::nullopt;
    const auto name = path.filename().string();
    if (std::filesystem::file_size(path) > limit)
        throw std::runtime_error(name + " exceeds byte limit");
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        throw std::runtime_error("cannot read " + name);
    // Cap the read itself, not just a racy pre-read size check.
    // 限制实际读取量，而不是仅依赖读取前可能失效的大小检查。
    std::string contents(limit + 1, '\0');
    stream.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    const auto count = static_cast<std::size_t>(stream.gcount());
    if (count > limit)
        throw std::runtime_error(name + " exceeds byte limit");
    if (stream.bad())
        throw std::runtime_error("error reading " + name);
    contents.resize(count);
    return contents;
}
} // namespace
Config::Config()
    : workers(std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, 8)),
      metadata_workers(std::min<std::size_t>(workers, 4)), queue_capacity(workers * 2) {}
Config Config::load(const std::filesystem::path& root) {
    Config result;
    const auto contents = read_settings(root / ".same" / "config.toml", 64 * 1024);
    if (!contents)
        return result;
    const auto table = toml::parse(*contents);
    // 仅读取明确支持的字段；未知字段（含旧版本字段）不参与校验。
    // Read supported fields only; unknown fields, including retired settings, are ignored.
    auto number = [&](const char* name, std::size_t& target) {
        if (!table.contains(name))
            return;
        auto value = table[name].value_exact<std::int64_t>();
        if (!value || *value <= 0 ||
            static_cast<std::uint64_t>(*value) > std::numeric_limits<std::size_t>::max())
            throw std::runtime_error(std::string("invalid positive integer: ") + name);
        target = static_cast<std::size_t>(*value);
    };
    number("workers", result.workers);
    result.metadata_workers = std::min<std::size_t>(result.workers, 4);
    number("metadata_workers", result.metadata_workers);
    if (table.contains("gpu_min_bytes")) {
        const auto value = table["gpu_min_bytes"].value_exact<std::int64_t>();
        if (!value || *value < 0 ||
            static_cast<std::uint64_t>(*value) > std::numeric_limits<std::size_t>::max())
            throw std::runtime_error("gpu_min_bytes must be a nonnegative integer");
        result.gpu_min_bytes = static_cast<std::size_t>(*value);
    }
    result.queue_capacity = result.workers <= std::numeric_limits<std::size_t>::max() / 2
                                ? result.workers * 2
                                : result.workers;
    number("block_bytes", result.block_bytes);
    number("memory_bytes", result.memory_bytes);
    number("device_memory_bytes", result.device_memory_bytes);
    number("queue_capacity", result.queue_capacity);
    number("telemetry_queue_capacity", result.telemetry_queue_capacity);
    number("telemetry_retention_runs", result.telemetry_retention_runs);
    number("telemetry_max_events", result.telemetry_max_events);
    if (table.contains("backend")) {
        auto value = table["backend"].value_exact<std::string>();
        if (!value)
            throw std::runtime_error("backend must be a string");
        result.backend = *value;
    }
    if (table.contains("rehash")) {
        auto value = table["rehash"].value_exact<bool>();
        if (!value)
            throw std::runtime_error("rehash must be boolean");
        result.rehash = *value;
    }
    if (table.contains("pgo")) {
        auto value = table["pgo"].value_exact<bool>();
        if (!value)
            throw std::runtime_error("pgo must be boolean");
        result.pgo = *value;
    }
    if (table.contains("telemetry")) {
        auto value = table["telemetry"].value_exact<bool>();
        if (!value)
            throw std::runtime_error("telemetry must be boolean");
        result.telemetry = *value;
    }
    auto real = [&](const char* name, double& target) {
        if (!table.contains(name))
            return;
        const auto value = table[name].value<double>();
        if (!value)
            throw std::runtime_error(std::string("invalid number: ") + name);
        target = *value;
    };
    real("igpu_bootstrap_ms", result.igpu_bootstrap_ms);
    real("cuda_bootstrap_ms", result.cuda_bootstrap_ms);
    real("cold_exploration_fraction", result.cold_exploration_fraction);
    result.validate();
    return result;
}
void Config::validate() const {
    if (!std::isfinite(cuda_bootstrap_ms) || cuda_bootstrap_ms < 0 || cuda_bootstrap_ms > 3600000)
        throw std::runtime_error("cuda_bootstrap_ms must be finite in [0, 3600000]");
    if (!std::isfinite(igpu_bootstrap_ms) || igpu_bootstrap_ms < 0 || igpu_bootstrap_ms > 3600000)
        throw std::runtime_error("igpu_bootstrap_ms must be finite in [0, 3600000]");
    if (!std::isfinite(cold_exploration_fraction) || cold_exploration_fraction < 0 ||
        cold_exploration_fraction > 1)
        throw std::runtime_error("cold_exploration_fraction must be finite in [0, 1]");
    if (!workers || workers > 256)
        throw std::runtime_error("workers must be in [1, 256]");
    if (!metadata_workers || metadata_workers > 256)
        throw std::runtime_error("metadata_workers must be in [1, 256]");
    if (!block_bytes || block_bytes > 64 * 1024 * 1024 || block_bytes % 1024)
        throw std::runtime_error("block_bytes must be a positive multiple of 1024, at most 64 MiB");
    if (!queue_capacity || queue_capacity > 65536)
        throw std::runtime_error("queue_capacity must be in [1, 65536]");
    if (!telemetry_queue_capacity || telemetry_queue_capacity > 65536)
        throw std::runtime_error("telemetry_queue_capacity must be in [1, 65536]");
    if (!telemetry_retention_runs || telemetry_retention_runs > 4096)
        throw std::runtime_error("telemetry_retention_runs must be in [1, 4096]");
    if (!telemetry_max_events || telemetry_max_events > 1000000)
        throw std::runtime_error("telemetry_max_events must be in [1, 1000000]");
    if (2 * block_bytes + block_bytes / 32 + 4096 > memory_bytes / workers)
        throw std::runtime_error(
            "memory_bytes must cover two blocks plus compute staging per worker");
    if (!device_memory_bytes)
        throw std::runtime_error("device_memory_bytes must be positive");
    if (backend != "auto" && backend != "cpu" && backend != "cuda" && backend != "igpu")
        throw std::runtime_error("backend must be auto, cpu, cuda or igpu");
}
namespace {
/// Match a bracket expression using ASCII/POSIX byte classes, independent of locale.
/// 使用不依赖区域设置的 ASCII/POSIX 字节类别匹配方括号表达式。
bool bracket(std::string_view pattern, std::size_t& end, unsigned char value) {
    auto i = end + 1;
    const bool negate = i < pattern.size() && (pattern[i] == '!' || pattern[i] == '^');
    i += negate;
    bool matched = false;
    bool first = true;
    for (; i < pattern.size(); ++i) {
        if (pattern[i] == ']' && !first) {
            end = i;
            return value != '/' && (matched != negate);
        }
        first = false;
        if (pattern[i] == '[' && i + 1 < pattern.size() && pattern[i + 1] == ':') {
            const auto close = pattern.find(":]", i + 2);
            if (close == std::string_view::npos)
                break;
            const auto name = pattern.substr(i + 2, close - i - 2);
            const bool digit = value >= '0' && value <= '9';
            const bool upper = value >= 'A' && value <= 'Z';
            const bool lower = value >= 'a' && value <= 'z';
            const bool alpha = upper || lower;
            const bool space = value == ' ' || (value >= 9 && value <= 13);
            matched |=
                (name == "alnum" && (alpha || digit)) || (name == "alpha" && alpha) ||
                (name == "blank" && (value == ' ' || value == 9)) ||
                (name == "cntrl" && (value < 32 || value == 127)) || (name == "digit" && digit) ||
                (name == "graph" && value > 32 && value < 127) || (name == "lower" && lower) ||
                (name == "print" && value >= 32 && value < 127) ||
                (name == "punct" && value > 32 && value < 127 && !alpha && !digit) ||
                (name == "space" && space) || (name == "upper" && upper) ||
                (name == "xdigit" &&
                 (digit || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F')));
            i = close + 1;
            continue;
        }
        if (pattern[i] == '\\' && i + 1 < pattern.size())
            ++i;
        const auto low = static_cast<unsigned char>(pattern[i]);
        if (i + 2 < pattern.size() && pattern[i + 1] == '-' && pattern[i + 2] != ']') {
            i += 2;
            if (pattern[i] == '\\' && i + 1 < pattern.size())
                ++i;
            matched |= value >= low && value <= static_cast<unsigned char>(pattern[i]);
        } else {
            matched |= value == low;
        }
    }
    end = pattern.size();
    return false;
}
/// Bounded glob dynamic programming; stars cross slashes only at ** component boundaries.
/// 有界 glob 动态规划；只有位于路径分量边界的 ** 可以跨越斜杠。
bool glob_match(std::string_view pattern, std::string_view path) {
    std::vector<unsigned char> previous(path.size() + 1), current(path.size() + 1);
    previous[0] = 1;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        std::fill(current.begin(), current.end(), 0);
        char token = pattern[i];
        bool escaped = false;
        if (token == '\\') {
            if (++i == pattern.size())
                return false;
            token = pattern[i];
            escaped = true;
        }
        bool cross = false;
        bool directories = false;
        if (token == '*' && !escaped) {
            const auto begin = i;
            while (i + 1 < pattern.size() && pattern[i + 1] == '*')
                ++i;
            cross = i > begin && (begin == 0 || pattern[begin - 1] == '/') &&
                    (i + 1 == pattern.size() || pattern[i + 1] == '/');
            directories = cross && i + 1 < pattern.size();
            if (directories)
                ++i;
        }
        bool reachable = false;
        auto bracket_end = i;
        for (std::size_t j = 0; j <= path.size(); ++j) {
            if (directories) {
                current[j] = previous[j] || (j && path[j - 1] == '/' && reachable);
                reachable |= previous[j] != 0;
            } else if (token == '*' && !escaped) {
                current[j] = previous[j] || (j && (cross || path[j - 1] != '/') && current[j - 1]);
            } else if (j) {
                bool match = token == path[j - 1];
                if (!escaped && token == '?')
                    match = path[j - 1] != '/';
                if (!escaped && token == '[') {
                    auto close = i;
                    match = bracket(pattern, close, static_cast<unsigned char>(path[j - 1]));
                    bracket_end = close;
                }
                current[j] = previous[j - 1] && match;
            }
        }
        if (!escaped && token == '[')
            i = bracket_end;
        previous.swap(current);
    }
    return previous.back() != 0;
}
} // namespace
Ignore::Ignore(const std::filesystem::path& root) {
    const auto contents = read_settings(root / ".same" / "ignore", 1024 * 1024);
    if (!contents)
        return;
    std::istringstream lines(*contents);
    std::string line;
    bool first_line = true;
    while (std::getline(lines, line)) {
        // Git accepts a UTF-8 BOM only at the start of an ignore file.
        // 与 Git 一致，仅在忽略文件起始处接受 UTF-8 BOM。
        if (first_line && line.starts_with("\xEF\xBB\xBF"))
            line.erase(0, 3);
        first_line = false;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        while (!line.empty() && line.back() == ' ') {
            std::size_t slashes = 0;
            for (auto i = line.size() - 1; i && line[i - 1] == '\\'; --i)
                ++slashes;
            if (slashes % 2)
                break;
            line.pop_back();
        }
        if (line.empty() || line.front() == '#')
            continue;
        bool negate = line.front() == '!';
        if (negate)
            line.erase(0, 1);
        bool anchored = !line.empty() && line.front() == '/';
        if (anchored)
            line.erase(0, 1);
        bool directory = !line.empty() && line.back() == '/';
        if (directory)
            line.pop_back();
        if (line.empty())
            continue;
        if (!anchored && line.find('/') == std::string::npos)
            line = "**/" + line;
        if (rules_.size() >= 4096)
            throw std::runtime_error("ignore file exceeds 4096 rules");
        rules_.push_back({std::move(line), negate, directory});
    }
}
bool Ignore::can_prune(std::string_view relative) const {
    return matches(relative, true);
}
bool Ignore::matches(std::string_view relative, bool directory) const {
    // Evaluate every ancestor first: Git cannot reinclude a child of an excluded parent.
    // 先评估每个祖先：Git 不允许重新纳入被排除父目录中的子项。
    for (std::size_t end = 0; end <= relative.size(); ++end) {
        if (end != relative.size() && relative[end] != '/')
            continue;
        const auto prefix = relative.substr(0, end);
        const bool is_directory = end != relative.size() || directory;
        const auto slash = prefix.rfind('/');
        if (prefix.substr(slash == std::string_view::npos ? 0 : slash + 1) == ".same")
            return true;
        bool ignored = false;
        for (const auto& rule : rules_) {
            if ((!rule.directory || is_directory) && glob_match(rule.pattern, prefix))
                ignored = !rule.negate;
        }
        if (ignored)
            return true;
    }
    return false;
}
} // namespace same
