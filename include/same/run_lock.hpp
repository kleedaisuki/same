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
} // namespace same
