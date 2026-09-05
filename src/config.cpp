#include "same/config.hpp"
#include <toml++/toml.hpp>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <limits>
#include <stdexcept>
#include <thread>
namespace same {
Config::Config() : workers(std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, 8)), queue_capacity(workers * 2) {}
Config Config::load(const std::filesystem::path& root) {
    Config result;
    const auto path = root / ".same" / "config.toml";
    if (!std::filesystem::exists(path)) return result;
    std::ifstream stream(path);
    if (!stream) throw std::runtime_error("cannot read .same/config.toml");
    const auto table = toml::parse(stream);
    for (const auto& [key, node] : table) {
        const auto name = key.str();
        if (name != "workers" && name != "block_bytes" && name != "memory_bytes" && name != "device_memory_bytes" &&
            name != "queue_capacity" && name != "backend" && name != "rehash") throw std::runtime_error("unknown configuration key: " + std::string(name));
    }
    auto number = [&](const char* name, std::size_t& target) {
        if (!table.contains(name)) return;
        auto value = table[name].value<std::int64_t>();
        if (!value || *value <= 0 || static_cast<std::uint64_t>(*value) > std::numeric_limits<std::size_t>::max())
            throw std::runtime_error(std::string("invalid positive integer: ") + name);
        target = static_cast<std::size_t>(*value);
    };
    number("workers", result.workers);
    result.queue_capacity = result.workers <= std::numeric_limits<std::size_t>::max() / 2 ? result.workers * 2 : result.workers;
    number("block_bytes", result.block_bytes); number("memory_bytes", result.memory_bytes);
    number("device_memory_bytes", result.device_memory_bytes); number("queue_capacity", result.queue_capacity);
    if (table.contains("backend")) {
        auto value = table["backend"].value<std::string>();
        if (!value) throw std::runtime_error("backend must be a string");
        result.backend = *value;
    }
    if (table.contains("rehash")) {
        auto value = table["rehash"].value<bool>();
        if (!value) throw std::runtime_error("rehash must be boolean");
        result.rehash = *value;
    }
    result.validate();
    return result;
}
void Config::validate() const {
    if (!workers || workers > 256) throw std::runtime_error("workers must be in [1, 256]");
    if (!block_bytes || block_bytes > 64 * 1024 * 1024 || block_bytes % 1024) throw std::runtime_error("block_bytes must be a positive multiple of 1024, at most 64 MiB");
    if (!queue_capacity || queue_capacity > 65536) throw std::runtime_error("queue_capacity must be in [1, 65536]");
    if (2 * block_bytes + block_bytes / 32 + 4096 > memory_bytes / workers) throw std::runtime_error("memory_bytes must cover two blocks plus compute staging per worker");
    if (!device_memory_bytes) throw std::runtime_error("device_memory_bytes must be positive");
    if (backend != "auto" && backend != "cpu" && backend != "cuda") throw std::runtime_error("backend must be auto, cpu or cuda");
}
namespace {
bool glob_match(std::string_view pattern, std::string_view path, bool directory_rule, bool directory) {
    // Dynamic programming bounds matching work; no recursive regex backtracking.
    // 动态规划限制匹配复杂度，避免正则表达式递归回溯。
    std::vector<unsigned char> previous(path.size() + 1), current(path.size() + 1);
    previous[0] = 1;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        std::fill(current.begin(), current.end(), 0);
        const char token = pattern[i];
        bool double_star = token == '*' && i + 1 < pattern.size() && pattern[i + 1] == '*';
        if (double_star) ++i;
        bool directory_star = double_star && i + 1 < pattern.size() && pattern[i + 1] == '/';
        if (directory_star) ++i;
        bool reachable = false;
        for (std::size_t j = 0; j <= path.size(); ++j) {
            if (directory_star) {
                current[j] = previous[j] || (j && path[j - 1] == '/' && reachable);
                reachable = reachable || previous[j];
            } else if (token == '*') {
                current[j] = previous[j] || (j && (double_star || path[j - 1] != '/') && current[j - 1]);
            } else if (j) {
                current[j] = previous[j - 1] && (token == '?' ? path[j - 1] != '/' : token == path[j - 1]);
            }
        }
        previous.swap(current);
    }
        for (std::size_t j = 0; j < path.size(); ++j) {
        if (path[j] == '/' && previous[j]) return true;
    }
    return (!directory_rule || directory) && previous.back() != 0;
}
}
Ignore::Ignore(const std::filesystem::path& root) {
    const auto path = root / ".same" / "ignore";
    if (!std::filesystem::exists(path)) return;
    if (std::filesystem::file_size(path) > 1024 * 1024) throw std::runtime_error("ignore file exceeds 1 MiB");
    std::ifstream stream(path);
    if (!stream) throw std::runtime_error("cannot read .same/ignore");
    // Read a capped snapshot, including concurrent growth after the size check.
    // 有界读取快照，同时限制大小检查后的并发增长。
    std::string contents(1024 * 1024 + 1, '\0');
    stream.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    const auto count = static_cast<std::size_t>(stream.gcount());
    if (count > 1024 * 1024) throw std::runtime_error("ignore file exceeds 1 MiB");
    if (stream.bad()) throw std::runtime_error("error reading .same/ignore");
    contents.resize(count);
    std::istringstream lines(contents);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.front() == '#') continue;
        bool negate = line.front() == '!';
        if (negate) line.erase(0, 1);
        bool anchored = !line.empty() && line.front() == '/';
        if (anchored) line.erase(0, 1);
        bool directory = !line.empty() && line.back() == '/';
        if (directory) line.pop_back();
        if (line.empty()) continue;
        if (!anchored && line.find('/') == std::string::npos) line = "**/" + line;
        if (rules_.size() >= 4096) throw std::runtime_error("ignore file exceeds 4096 rules");
        has_negations_ = has_negations_ || negate;
        rules_.push_back({std::move(line), negate, directory});
    }
    if (stream.bad()) throw std::runtime_error("error reading .same/ignore");
}
bool Ignore::can_prune(std::string_view relative) const {
    return !has_negations_ && matches(relative, true);
}
bool Ignore::matches(std::string_view relative, bool directory) const {
    if (relative == ".same" || relative.starts_with(".same/")) return true;
    bool ignored = false;
    // Match ancestors as well; callers must still descend to allow negated children.
    // 同时匹配父目录；调用方仍须递归，以允许子项否定规则生效。
    for (const auto& rule : rules_) {
        if (glob_match(rule.pattern, relative, rule.directory, directory)) ignored = !rule.negate;
    }
    return ignored;
}
}
