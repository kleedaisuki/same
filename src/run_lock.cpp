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
struct RunLock::Impl {
#ifdef _WIN32
    HANDLE handle{INVALID_HANDLE_VALUE};
    ~Impl() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
#else
    int fd{-1};
    ~Impl() { if (fd >= 0) close(fd); }
#endif
};
RunLock::RunLock(const std::filesystem::path& path) : impl_(std::make_unique<Impl>()) {
#ifdef _WIN32
    impl_->handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (impl_->handle == INVALID_HANDLE_VALUE)
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "Cannot acquire run lock (another same process may be running)");
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!GetFileInformationByHandleEx(impl_->handle, FileAttributeTagInfo, &info, sizeof(info)))
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Run lock attributes");
    if ((info.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
        GetFileType(impl_->handle) != FILE_TYPE_DISK)
        throw std::runtime_error("Run lock must be a regular non-reparse file");
#else
    impl_->fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (impl_->fd < 0) throw std::system_error(errno, std::generic_category(), "Open run lock");
    struct stat info{};
    if (fstat(impl_->fd, &info) != 0)
        throw std::system_error(errno, std::generic_category(), "Run lock attributes");
    if (!S_ISREG(info.st_mode)) throw std::runtime_error("Run lock must be a regular file");
    int result;
    do { result = flock(impl_->fd, LOCK_EX | LOCK_NB); } while (result < 0 && errno == EINTR);
    if (result < 0)
        throw std::system_error(errno, std::generic_category(),
                                "Cannot acquire run lock (another same process may be running)");
#endif
}
RunLock::~RunLock() = default;
}
