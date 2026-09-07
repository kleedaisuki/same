#pragma once
#include "same/compute.hpp"
#include "same/files.hpp"
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace same {
struct FileRecord {
    std::string path;
    FileStamp stamp;
    Digest digest;
};
// Single-owner repository. Callbacks are streamed; references expire on return.
// 单线程独占仓储。回调逐行执行，引用仅在本次回调内有效。
class Store {
public:
    explicit Store(const std::filesystem::path& path, std::size_t cache_bytes = 2 * 1024 * 1024);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    void begin_scan();
    std::optional<FileRecord> cached(std::string_view path);
    void save(const FileRecord& record);
    void end_scan();
    void rollback_scan() noexcept;
    // Includes only size/hash buckets with at least two files; ordered by size/hash/path.
    // 仅访问包含至少两个文件的大小/哈希桶，按大小、哈希、路径排序。
    void visit_candidates(const std::function<void(const FileRecord&)>& visitor);
    void clear_representatives();
    void add_representative(const FileRecord& record);
    // Return false to stop. Do not mutate representatives during this callback.
    // 回调返回 false 停止；回调期间不可修改代表文件表。
    void visit_representatives(const std::function<bool(const FileRecord&)>& visitor);
    void reset_matches();
    void add_match(std::string_view representative, std::string_view member);
    // Only exact groups with >=2 members. Paths refer to root-relative UTF-8 strings.
    // 仅输出至少两个成员的精确相同组；路径为相对根目录的 UTF-8 字符串。
    void visit_matches(const std::function<void(std::string_view, std::string_view)>& visitor);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace same
