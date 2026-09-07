#pragma once
#include <cstddef>
#include <filesystem>

#include <string>
#include <string_view>
#include <vector>
namespace same {
struct Config {
    std::size_t workers;
    std::size_t block_bytes{1024 * 1024};
    std::size_t memory_bytes{64 * 1024 * 1024};
    std::size_t device_memory_bytes{64 * 1024 * 1024};
    std::size_t queue_capacity;
    std::string backend{"auto"};
    bool rehash{false};
    Config();
    static Config load(const std::filesystem::path& root);
    void validate() const;
};
class Ignore {
public:
    explicit Ignore(const std::filesystem::path& root);
    bool matches(std::string_view relative, bool directory) const;
    // Negated children may require access to an otherwise ignored directory.
    // 否定规则可能需要访问原本被忽略的目录。
    bool can_prune(std::string_view relative) const;

private:
    struct Rule {
        std::string pattern;
        bool negate;
        bool directory;
    };
    std::vector<Rule> rules_;
    bool has_negations_{false};
};
} // namespace same
