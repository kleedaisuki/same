#pragma once
#include <filesystem>
#include <memory>

namespace same {
// Held across scan, comparison and output. The persistent inode must never be unlinked.
// 锁覆盖扫描、比较与输出；禁止删除持久锁文件，避免不同进程锁住不同 inode。
class RunLock {
public:
    explicit RunLock(const std::filesystem::path& path);
    ~RunLock();
    RunLock(const RunLock&) = delete;
    RunLock& operator=(const RunLock&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace same
