#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
namespace same {
struct FileStamp {
    std::uint64_t size{};
    std::string identity;
    std::string modified;
    std::string changed;
    bool operator==(const FileStamp&) const = default;
};
// Handle-based metadata prevents path replacement from mixing file versions.
// 基于句柄获取元数据，避免路径替换混合不同文件版本。
class FileReader {
public:
    explicit FileReader(const std::filesystem::path& path);
    ~FileReader();
    FileReader(FileReader&&) noexcept;
    FileReader& operator=(FileReader&&) noexcept;
    FileReader(const FileReader&) = delete;
    FileReader& operator=(const FileReader&) = delete;
    FileStamp stamp() const;
    std::size_t read(std::span<std::byte> destination);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
bool is_reparse_point(const std::filesystem::path& path);
FileStamp stamp_path(const std::filesystem::path& path);
}
