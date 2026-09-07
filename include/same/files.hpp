#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
namespace same {
/** 文件版本的保守缓存键；不是内容相同的证明。
 * Conservative cache key for a file version; not proof of equal contents.
 * 时间与标识是平台相关的不透明字符串，只能在同一语义下比较。
 * Timestamps and identities are platform-specific opaque strings, compared like-for-like.
 */
struct FileStamp {
    /// 文件长度，单位字节。 / File length in bytes.
    std::uint64_t size{};
    /// 卷与文件标识（POSIX 为设备号与 inode）。 / Volume and file ID (device and inode on POSIX).
    std::string identity;
    /// 最后写入时间，不是创建时间。 / Last-write timestamp, not creation time.
    std::string modified;
    /// 元数据变更时间，不是创建时间。 / Metadata-change timestamp, not creation time.
    std::string changed;
    /// 所有字段一致才允许复用缓存。 / Cache reuse requires equality of every field.
    bool operator==(const FileStamp&) const = default;
};
/** 独占一个顺序读取句柄，元数据始终取自该句柄而非重新解析路径。
 * Owns a sequential-read handle; metadata comes from that handle, not a new path lookup.
 * 不阻止并发写入；调用方需比较读取前后的 stamp，并自行处理变更。
 * Does not prevent concurrent writes; callers must compare stamps around reads.
 */
class FileReader {
public:
    /// 打开普通文件，拒绝末级符号链接/重解析点；失败抛出异常。
    /// Open a regular file, rejecting final-component symlinks/reparse points; throw on failure.
    explicit FileReader(const std::filesystem::path& path);
    /// 关闭持有的 OS 句柄。 / Close the owned OS handle.
    ~FileReader();
    /// 转移句柄所有权；源对象只可销毁或重新赋值。 / Transfer ownership; source may only be
    /// destroyed or assigned.
    FileReader(FileReader&&) noexcept;
    /// 关闭旧句柄并接管源句柄。 / Close the old handle and acquire the source handle.
    FileReader& operator=(FileReader&&) noexcept;
    FileReader(const FileReader&) = delete;
    FileReader& operator=(const FileReader&) = delete;
    /// 读取当前元数据；不移动读游标，也不提供原子内容快照。
    /// Read current metadata without moving the cursor; this is not an atomic content snapshot.
    FileStamp stamp() const;
    /// 从当前游标读取并前移；允许短读，空缓冲区或 EOF 返回零，错误抛异常。
    /// Read and advance the cursor; short reads are valid, empty buffers/EOF return zero, errors
    /// throw.
    std::size_t read(std::span<std::byte> destination);

private:
    /// 隐藏平台相关的句柄布局。 / Hide the platform-specific handle layout.
    struct Impl;
    /// 唯一拥有底层句柄。 / Sole owner of the native handle.
    std::unique_ptr<Impl> impl_;
};
/// 检查路径当前是否是重解析点（POSIX 上为符号链接）；不锁定路径。
/// Test for a reparse point (symlink on POSIX) without pinning the path.
bool is_reparse_point(const std::filesystem::path& path);
/// 临时打开文件取得 stamp 后关闭；后续再次打开可能得到不同文件。
/// Open temporarily to obtain a stamp; a later open may resolve to a different file.
FileStamp stamp_path(const std::filesystem::path& path);
} // namespace same
