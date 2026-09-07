#pragma once
#include <filesystem>
#include <memory>

namespace same {
/** 覆盖扫描、比较与输出的进程间独占锁，生命周期结束时自动释放。
 * Interprocess exclusive lock spanning scan, comparison and output; released on destruction.
 * 永不删除持久锁文件，否则不同进程可能锁住不同 inode。
 * Never unlink the persistent lock file: processes could otherwise lock different inodes.
 * POSIX 使用协作式 flock；所有参与者必须遵守同一锁协议。
 * POSIX uses advisory flock; all participants must follow the same locking protocol.
 */
class RunLock {
public:
    /// 打开或创建普通锁文件并立即尝试独占；竞争或 I/O 错误抛异常，不等待。
    /// Open/create a regular lock file and try exclusive ownership; contention/I/O errors throw
    /// without waiting.
    explicit RunLock(const std::filesystem::path& path);
#ifndef _WIN32
    /// 相对稳定目录句柄打开锁文件；name 必须是单个文件名。
    /// Open relative to a stable directory descriptor; name must be one filename.
    RunLock(int directory_fd, const char* name);
#endif
    /// 关闭句柄释放锁，但保留锁文件。 / Close the handle to release the lock, retaining the file.
    ~RunLock();
    RunLock(const RunLock&) = delete;
    RunLock& operator=(const RunLock&) = delete;

private:
    /// 平台相关的锁资源。 / Platform-specific lock resource.
    struct Impl;
    /// 唯一所有权使构造失败和正常销毁都能释放资源。 / Sole ownership cleans up on construction
    /// failure and destruction.
    std::unique_ptr<Impl> impl_;
};
/** 工作区生命周期锁；POSIX 锁住不会被 clean 删除的工作目录 inode。
 * Workspace lifecycle guard; POSIX locks the working-directory inode, which clean preserves.
 * Windows 使用原有独占文件句柄及清理目录句柄，此守卫不额外加锁。
 * Windows relies on exclusive file and cleanup directory handles; this guard is a no-op.
 */
class WorkspaceLock {
public:
    /// 立即尝试工作区互斥，不等待；须先于创建状态目录取得。
    /// Try workspace exclusion without waiting, before state directory creation.
    explicit WorkspaceLock(const std::filesystem::path& root);
    /// 释放稳定目录锁。 / Release the stable directory lock.
    ~WorkspaceLock();
    WorkspaceLock(const WorkspaceLock&) = delete;
    WorkspaceLock& operator=(const WorkspaceLock&) = delete;

private:
    /// POSIX 描述符；Windows 保持 -1。 / POSIX descriptor; stays -1 on Windows.
    int fd_{-1};
};
} // namespace same
