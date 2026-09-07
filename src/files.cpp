#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#include "same/files.hpp"
#include <algorithm>
#include <cerrno>
#include <limits>
#include <stdexcept>
#include <system_error>
#ifdef _WIN32
#include "same/detail/windows_metadata.hpp"
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace same {
namespace {
/// 将当前平台 I/O 错误附上操作与路径后抛出。 / Throw the native I/O error with operation and path
/// context.
[[noreturn]] void io_error(const char* operation, const std::filesystem::path& path) {
#ifdef _WIN32
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                            std::string(operation) + ": " + path.string());
#else
    throw std::system_error(errno, std::generic_category(),
                            std::string(operation) + ": " + path.string());
#endif
}
} // namespace
/// 顺序读取资源；析构负责关闭，确保构造中途抛异常也不泄漏。
/// Sequential-read resource; destruction closes it even when construction throws midway.
struct FileReader::Impl {
    /// 仅用于诊断；读取和元数据查询均使用已打开句柄。 / Diagnostic path only; reads and metadata
    /// use the open handle.
    std::filesystem::path path;
#ifdef _WIN32
    /// 独占持有的 Windows 文件句柄。 / Exclusively owned Windows file handle.
    HANDLE handle{INVALID_HANDLE_VALUE};
    /// 无异常释放句柄。 / Release the handle without throwing.
    ~Impl() {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
#else
    /// 独占持有的 POSIX 文件描述符。 / Exclusively owned POSIX file descriptor.
    int fd{-1};
    /// 无异常释放描述符。 / Release the descriptor without throwing.
    ~Impl() {
        if (fd >= 0)
            close(fd);
    }
#endif
};
FileReader::FileReader(const std::filesystem::path& path) : impl_(std::make_unique<Impl>()) {
    impl_->path = path;
#ifdef _WIN32
    /// 允许其他程序写入/替换路径；持有句柄仍指向原文件，但不是内容快照。
    /// Sharing permits concurrent writes/path replacement; the handle pins the file, not a content
    /// snapshot.
    impl_->handle = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (impl_->handle == INVALID_HANDLE_VALUE)
        io_error("open file", impl_->path);
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(impl_->handle, &info))
        io_error("file attributes", impl_->path);
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
        GetFileType(impl_->handle) != FILE_TYPE_DISK)
        throw std::runtime_error("not a regular non-reparse file: " + path.string());
#else
    /// 不跟随末级链接；NONBLOCK 防止验证文件类型之前因 FIFO 而阻塞。
    /// Do not follow final symlinks; NONBLOCK avoids blocking on a FIFO before validating its type.
    impl_->fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (impl_->fd < 0)
        io_error("open file", impl_->path);
    struct stat info{};
    if (fstat(impl_->fd, &info) != 0)
        io_error("fstat", impl_->path);
    if (!S_ISREG(info.st_mode))
        throw std::runtime_error("not a regular file: " + path.string());
#endif
}
FileReader::~FileReader() = default;
FileReader::FileReader(FileReader&&) noexcept = default;
FileReader& FileReader::operator=(FileReader&&) noexcept = default;
FileStamp FileReader::stamp() const {
#ifdef _WIN32
    FILE_BASIC_INFO basic{};
    FILE_STANDARD_INFO standard{};
    FILE_ID_INFO id{};
    // ChangeTime has no equivalent in the legacy API: fail closed if unavailable.
    // 旧接口没有 ChangeTime 的等价物：不可用时拒绝扫描，不能伪造稳定元数据。
    if (!GetFileInformationByHandleEx(impl_->handle, FileBasicInfo, &basic, sizeof(basic)))
        io_error("file basic metadata (ChangeTime required)", impl_->path);
    if (!GetFileInformationByHandleEx(impl_->handle, FileStandardInfo, &standard, sizeof(standard)))
        io_error("file standard metadata", impl_->path);
    std::string identity;
    if (GetFileInformationByHandleEx(impl_->handle, FileIdInfo, &id, sizeof(id))) {
        identity = std::to_string(id.VolumeSerialNumber) + ":";
        constexpr char hex[] = "0123456789abcdef";
        for (auto byte : id.FileId.Identifier) {
            identity += hex[byte >> 4];
            identity += hex[byte & 15];
        }
    } else {
        // FAT/SMB may reject this information class; real I/O errors must surface.
        // FAT/SMB 可能不支持该信息类别；真实 I/O 错误必须向上传播。
        if (!detail::unsupported_metadata_class(GetLastError()))
            io_error("file identity", impl_->path);
        BY_HANDLE_FILE_INFORMATION legacy{};
        if (!GetFileInformationByHandle(impl_->handle, &legacy))
            io_error("legacy file identity", impl_->path);
        identity = detail::legacy_file_identity(legacy);
    }
    /// 保留原始 Windows 时间计数，不经日历格式化损失精度。
    /// Preserve raw Windows timestamp ticks without precision-losing calendar formatting.
    return {static_cast<std::uint64_t>(standard.EndOfFile.QuadPart), identity,
            std::to_string(basic.LastWriteTime.QuadPart),
            std::to_string(basic.ChangeTime.QuadPart)};
#else
    struct stat info{};
    if (fstat(impl_->fd, &info) != 0)
        io_error("fstat", impl_->path);
#ifdef __APPLE__
    const auto mt = info.st_mtimespec;
    const auto ct = info.st_ctimespec;
#else
    const auto mt = info.st_mtim;
    const auto ct = info.st_ctim;
#endif
    /// st_ctime 是状态变更时间；与写入时间分开保存以降低缓存误命中风险。
    /// st_ctime is status-change time; keep it separate from modification time to reduce stale
    /// cache hits.
    return {static_cast<std::uint64_t>(info.st_size),
            std::to_string(info.st_dev) + ":" + std::to_string(info.st_ino),
            std::to_string(mt.tv_sec) + ":" + std::to_string(mt.tv_nsec),
            std::to_string(ct.tv_sec) + ":" + std::to_string(ct.tv_nsec)};
#endif
}
std::size_t FileReader::read(std::span<std::byte> destination) {
#ifdef _WIN32
    DWORD count{};
    const auto size = static_cast<DWORD>(
        std::min<std::size_t>(destination.size(), std::numeric_limits<DWORD>::max()));
    if (!ReadFile(impl_->handle, destination.data(), size, &count, nullptr))
        io_error("read file", impl_->path);
    return count;
#else
    ssize_t count;
    do {
        count =
            ::read(impl_->fd, destination.data(),
                   std::min<std::size_t>(destination.size(), std::numeric_limits<ssize_t>::max()));
    } while (count < 0 && errno == EINTR);
    if (count < 0)
        io_error("read file", impl_->path);
    return static_cast<std::size_t>(count);
#endif
}
bool is_reparse_point(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        io_error("file attributes", path);
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return std::filesystem::is_symlink(std::filesystem::symlink_status(path));
#endif
}
FileStamp stamp_path(const std::filesystem::path& path) {
    return FileReader(path).stamp();
}
} // namespace same
