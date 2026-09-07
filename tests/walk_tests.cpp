/** @file 并行遍历的终止、背压与过滤回归。 / Parallel walk termination, backpressure and filtering
 * regressions. */
#include "same/walk.hpp"
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>
namespace fs = std::filesystem;
/// 检查不依赖 NDEBUG。 / Checks survive NDEBUG.
void require(bool value) {
    if (!value)
        throw std::runtime_error("walk assertion failed");
}
/// 固定规模夹具覆盖宽树、深树与文件级并发。 / Fixed fixture covers wide/deep trees and file tasks.
int run_tests() {
    const auto root = fs::temp_directory_path() /
                      ("same-walk-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root / ".same");
    /// 独占测试路径；失败时清理。 / Exclusive fixture; cleanup on failure.
    struct Cleanup {
        /// 已验证临时测试根。 / Validated temporary test root.
        fs::path root;
        /// 析构不掩盖测试异常。 / Destructor never masks test errors.
        ~Cleanup() {
            std::error_code ec;
            fs::remove_all(root, ec);
        }
    } cleanup{root};
    std::ofstream(root / ".same" / "ignore") << "skip/*\n!skip/keep.txt\n*.tmp\n";
    std::set<std::string> expected;
    for (int i = 0; i < 24; ++i) {
        const auto directory = "d" + std::to_string(i);
        fs::create_directory(root / directory);
        for (int j = 0; j < 12; ++j) {
            const auto key = directory + "/f" + std::to_string(j);
            std::ofstream(root / fs::path(key)) << "abc";
            expected.insert(key);
        }
    }
    fs::create_directory(root / "skip");
    std::ofstream(root / "skip" / "keep.txt") << "abc";
    std::ofstream(root / "skip" / "drop.txt") << "abc";
    std::ofstream(root / "drop.tmp") << "abc";
    expected.insert("skip/keep.txt");
    std::ofstream(root / "root.txt") << "abc";
    expected.insert("root.txt");
    auto deep = root;
    std::string deep_key;
    for (int i = 0; i < 60; ++i) {
        deep /= "deep";
        deep_key += "deep/";
    }
    fs::create_directories(deep);
    std::ofstream(deep / fs::path(u8"论文.pdf")) << "abc";
    expected.insert(deep_key + "论文.pdf");
    std::error_code ec;
    fs::create_directory_symlink(root, root / "cycle", ec);
    for (const auto workers : {1u, 4u, 16u}) {
        for (const auto capacity : {1u, 3u, 64u}) {
            same::ParallelWalk walk(root, workers, capacity);
            std::set<std::string> actual;
            while (auto entry = walk.next()) {
                require(entry->stamp.size == 3);
                require(entry->reader && entry->reader->stamp() == entry->stamp);
                std::array<std::byte, 3> bytes{};
                require(entry->reader->read(bytes) == 3);
                require(bytes[0] == std::byte{97});
                require(actual.insert(entry->path).second);
            }
            require(actual == expected);
            require(!walk.next());
            require(walk.stats().task_peak <= capacity);
            require(walk.stats().result_peak <= capacity);
        }
    }
    // 浅扫描只产生根目录普通文件，不进入宽树、深树或链接目录。
    // Shallow scans return root regular files only, without entering wide/deep/link directories.
    for (const auto workers : {1u, 4u}) {
        same::ParallelWalk walk(root, workers, 1, false);
        const auto entry = walk.next();
        require(entry && entry->path == "root.txt" && entry->stamp.size == 3);
        require(!walk.next());
    }
    // 满输出队列取消必须可退出。 / Cancellation must unblock a full output queue.
    {
        same::ParallelWalk walk(root, 8, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    bool failed = false;
    try {
        same::ParallelWalk walk(root / "missing", 4, 1);
        (void)walk.next();
    } catch (const std::system_error&) {
        failed = true;
    }
    require(failed);
    failed = false;
    try {
        same::ParallelWalk walk(root, 0, 1);
    } catch (const std::invalid_argument&) {
        failed = true;
    }
    require(failed);
    return 0;
}

/// 输出失败原因而非平台 fast-fail。 / Print failure rather than platform fast-fail.
int main() {
    try {
        return run_tests();
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
