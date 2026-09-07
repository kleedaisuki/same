/** @file
 * @brief 进程排他锁、释放后复用与特殊文件拒绝。 / Process exclusion, lock reuse and rejection of
 * special files.
 */
#include "same/run_lock.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#ifndef _WIN32
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
