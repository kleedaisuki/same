#include "same/run_lock.hpp"
#include <cerrno>
#include <stdexcept>
#include <system_error>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace same {
/// 通过资源生命周期保持进程锁；关闭资源即可释放，无需删除锁文件。
/// Hold the process lock through resource lifetime; closing releases it without deleting the lock
/// file.
struct RunLock::Impl {
#ifdef _WIN32
    /// 禁止共享打开的文件句柄即是 Windows 锁。 / A file handle opened without sharing is the
    /// Windows lock.
    HANDLE handle{INVALID_HANDLE_VALUE};
    /// 关闭句柄释放独占打开限制。 / Close the handle to release exclusive-open protection.
    ~Impl() {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
#else
    /// flock 附着在此描述符引用的打开文件描述上。 / flock is attached to the open file description
    /// referenced here.
    int fd{-1};
    /// 关闭本对象唯一拥有的描述符以释放 flock。 / Close this object's owned descriptor to release
    /// flock.
    ~Impl() {
        if (fd >= 0)
            close(fd);
    }
#endif
};
RunLock::RunLock(const std::filesystem::path& path) : impl_(std::make_unique<Impl>()) {
#ifdef _WIN32
    /// 共享模式为零，因此已有不兼容打开会立刻失败；OPEN_ALWAYS 保留锁文件身份。
    /// Zero sharing fails immediately on incompatible opens; OPEN_ALWAYS preserves lock-file
    /// identity.
    impl_->handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (impl_->handle == INVALID_HANDLE_VALUE)
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "Cannot acquire run lock (another same process may be running)");
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!GetFileInformationByHandleEx(impl_->handle, FileAttributeTagInfo, &info, sizeof(info)))
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "Run lock attributes");
    if ((info.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
        GetFileType(impl_->handle) != FILE_TYPE_DISK)
        throw std::runtime_error("Run lock must be a regular non-reparse file");
#else
    /// NOFOLLOW 拒绝末级链接；先验证普通文件，再取得非阻塞协作式锁。
    /// NOFOLLOW rejects final symlinks; validate a regular file before taking a nonblocking
    /// advisory lock.
    impl_->fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (impl_->fd < 0)
        throw std::system_error(errno, std::generic_category(), "Open run lock");
    struct stat info{};
    if (fstat(impl_->fd, &info) != 0)
        throw std::system_error(errno, std::generic_category(), "Run lock attributes");
    if (!S_ISREG(info.st_mode))
        throw std::runtime_error("Run lock must be a regular file");
    int result;
    do {
        result = flock(impl_->fd, LOCK_EX | LOCK_NB);
    } while (result < 0 && errno == EINTR);
    if (result < 0)
        throw std::system_error(errno, std::generic_category(),
                                "Cannot acquire run lock (another same process may be running)");
#endif
}
RunLock::~RunLock() = default;
} // namespace same
