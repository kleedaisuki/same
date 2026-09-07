/** @file
 * @brief 进程排他锁、释放后复用与特殊文件拒绝。 / Process exclusion, lock reuse and rejection of
 * special files.
 */
#include "same/run_lock.hpp"
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
namespace {
/// 断言失败抛出可定位错误。 / Throw a diagnostic on assertion failure.
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
/// 尝试竞争锁，异常代表正确拒绝。 / Probe acquisition; an exception denotes expected rejection.
bool rejected(const std::filesystem::path& path) {
    try {
        same::RunLock lock(path);
    } catch (const std::exception&) {
        return true;
    }
    return false;
}
#ifndef _WIN32
/// 工作目录锁竞争立即失败。 / Workspace contention must fail immediately.
bool workspace_rejected(const std::filesystem::path& root) {
    try {
        same::WorkspaceLock lock(root);
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

/// 子进程重新打开工作目录验证跨进程互斥。 / Reopen the workspace in a child to verify process
/// exclusion.
void workspace_contender(const std::filesystem::path& root) {
    const auto child = fork();
    check(child >= 0, "fork workspace contender");
    if (child == 0)
        _exit(workspace_rejected(root) ? 0 : 1);
    int status{};
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    check(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "workspace must reject a second process");
}

/// 状态目录重建不能绕过稳定工作目录互斥。 / State recreation must not bypass stable workspace
/// exclusion.
void workspace_lifetime(const std::filesystem::path& root) {
    const auto state = root / ".same";
    const auto unrelated = root / "unrelated";
    std::filesystem::create_directory(state);
    std::filesystem::create_directory(unrelated);
    {
        same::WorkspaceLock lock(root);
        check(workspace_rejected(root), "workspace must reject a second owner");
        workspace_contender(root);
        check(!workspace_rejected(unrelated), "unrelated workspace must remain available");
        std::filesystem::remove_all(state);
        workspace_contender(root);
        std::filesystem::create_directory(state);
        workspace_contender(root);
    }
    check(!workspace_rejected(root), "released workspace must be reusable");
}

/// 拥有目录描述符，异常路径也关闭。 / Own a directory descriptor with exception-safe cleanup.
struct Directory {
    /// 锚定 openat 的目录句柄。 / Directory handle anchoring openat.
    int fd;
    /// 打开普通目录且不跟随链接。 / Open a directory without following links.
    explicit Directory(const std::filesystem::path& path)
        : fd(open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)) {
        check(fd >= 0, "open test directory descriptor");
    }
    /// 关闭测试拥有的描述符。 / Close the descriptor owned by this fixture.
    ~Directory() {
        close(fd);
    }
};

/// 相对句柄锁仍须拒绝竞争、路径逃逸与链接。 / Descriptor-relative locks reject contention, escapes
/// and links.
void relative_lock(const std::filesystem::path& root) {
    const auto directory = root / "relative";
    std::filesystem::create_directory(directory);
    Directory handle(directory);
    const auto rejects = [&](const char* name) {
        try {
            same::RunLock lock(handle.fd, name);
        } catch (const std::exception&) {
            return true;
        }
        return false;
    };
    {
        same::RunLock lock(handle.fd, "run.lock");
        check(rejects("run.lock"), "relative lock must reject second owner");
    }
    check(!rejects("run.lock"), "relative lock must be reusable");
    for (const auto* name : {"", ".", "..", "../escaped.lock", "nested/run.lock", "/escaped.lock"})
        check(rejects(name), "relative lock accepted a non-filename");
    check(!std::filesystem::exists(root / "escaped.lock"), "relative lock escaped directory");
    const auto target = directory / "target";
    {
        std::ofstream output(target);
        output << "preserve";
    }
    std::filesystem::create_symlink(target, directory / "link");
    check(rejects("link"), "relative lock must reject symlink");
    check(std::filesystem::file_size(target) == 8, "relative lock modified symlink target");
    const auto moved = root / "relative-moved";
    std::filesystem::rename(directory, moved);
    std::filesystem::create_directory(directory);
    check(!rejects("after-rename.lock"), "stable directory descriptor lost after rename");
    check(std::filesystem::exists(moved / "after-rename.lock") &&
              !std::filesystem::exists(directory / "after-rename.lock"),
          "relative lock followed replacement directory");
}
#endif
} // namespace
/// 运行本文件全部回归场景，断言失败即返回非零。 / Run all regressions; assertion failures produce a
/// nonzero exit.
int main() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("same-lock-test-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    try {
        const auto path = root / "run.lock";
        {
            same::RunLock lock(path);
            check(rejected(path), "second owner must be rejected");
#ifndef _WIN32
            const auto child = fork();
            check(child >= 0, "fork lock contender");
            if (child == 0)
                _exit(rejected(path) ? 0 : 1);
            int status{};
            check(waitpid(child, &status, 0) == child, "wait lock contender");
            check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "second process must be rejected");
#endif
        }
        check(std::filesystem::is_regular_file(path), "release must preserve lock inode");
        check(!rejected(path), "released lock can be acquired again");
        const auto target = root / "target";
        {
            std::ofstream file(target);
            file << "untouched";
        }
        const auto link = root / "link";
        std::error_code error;
        std::filesystem::create_symlink(target, link, error);
        if (!error)
            check(rejected(link), "symlink lock must be rejected");
        check(std::filesystem::file_size(target) == 9, "lock must not truncate link target");
        check(rejected(root), "directory lock must be rejected");
#ifndef _WIN32
        workspace_lifetime(root);
        relative_lock(root);
        const auto fifo = root / "fifo";
        check(mkfifo(fifo.c_str(), 0600) == 0, "create fifo");
        check(rejected(fifo), "FIFO must be rejected without blocking");
#endif
        std::filesystem::remove_all(root);
        std::cout << "run lock tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::filesystem::remove_all(root);
        return 1;
    }
}
