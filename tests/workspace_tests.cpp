/** @file 工作区初始化与安全清理回归。 / Workspace initialization and safe cleanup regressions. */
#include "same/config.hpp"
#include "same/run_lock.hpp"
#include "same/workspace.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace {
/// 失败即报告契约。 / Report violated contracts.
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
/// 捕获预期拒绝。 / Capture expected rejection.
template <class F> bool rejects(F operation) {
    try {
        operation();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}
/// 读取原始内容以检查幂等性。 / Read bytes to verify idempotence.
std::string read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}
} // namespace
/// 独立临时目录内验证全部生命周期。 / Verify lifecycle inside an isolated temporary directory.
int main() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
                      ("same-workspace-test-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root / "child");
    try {
        same::initialize_workspace(root);
        const auto config = read(root / ".same/config.toml");
        const auto loaded = same::Config::load(root);
        const same::Config defaults;
        check(loaded.workers == defaults.workers &&
                  loaded.metadata_workers == defaults.metadata_workers &&
                  loaded.block_bytes == defaults.block_bytes &&
                  loaded.memory_bytes == defaults.memory_bytes &&
                  loaded.device_memory_bytes == defaults.device_memory_bytes &&
                  loaded.queue_capacity == defaults.queue_capacity &&
                  loaded.gpu_min_bytes == defaults.gpu_min_bytes &&
                  loaded.backend == defaults.backend && loaded.rehash == defaults.rehash,
              "all defaults round trip");
        check(config.find("gpu_probe_bytes") == std::string::npos, "retired field generated");
        check(loaded.pgo == defaults.pgo, "PGO default round trip");
        std::ofstream(root / ".same/ignore", std::ios::binary) << "custom\n";
        same::initialize_workspace(root);
        check(read(root / ".same/config.toml") == config &&
                  read(root / ".same/ignore") == "custom\n",
              "new preserves files");
        fs::remove(root / ".same/config.toml");
        same::initialize_workspace(root);
        check(read(root / ".same/config.toml") == config, "new fills missing configuration");
        {
            same::RunLock lock(root / ".same/run.lock");
            check(rejects([&] { same::clean_workspace(root, false); }), "active scan blocks clean");
            check(fs::exists(root / ".same/config.toml"), "failed clean preserves payload");
        }
        same::initialize_workspace(root / "child");
        std::ofstream(root / "keep.txt") << "keep";
        check(same::clean_workspace(root, false) == 1 && fs::exists(root / "child/.same"),
              "nonrecursive clean scope");
        check(same::clean_workspace(root, false) == 0, "absent clean idempotence");
        same::initialize_workspace(root);
        check(same::clean_workspace(root, true) == 2 && fs::exists(root / "keep.txt"),
              "recursive clean preserves user files");
#ifdef _WIN32
        // 无关的独占打开文件不能阻止递归清理。 / Unrelated exclusively open files do not block
        // clean.
        const auto busy = root / "busy.txt";
        HANDLE handle = CreateFileW(busy.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(handle != INVALID_HANDLE_VALUE, "create exclusively open unrelated file");
        try {
            check(same::clean_workspace(root, true) == 0, "ignore unrelated open files");
        } catch (...) {
            CloseHandle(handle);
            throw;
        }
        CloseHandle(handle);
#endif
        fs::create_directories(root / "outside/.same");
        std::ofstream(root / "outside/.same/keep") << "safe";
        std::error_code error;
        fs::create_directory_symlink(root / "outside/.same", root / ".same", error);
        if (!error) {
            std::cout << "symlink safety and replacement race tests enabled\n";
            check(rejects([&] { same::clean_workspace(root, false); }), "linked state rejected");
            check(rejects([&] { same::initialize_workspace(root); }), "linked state init rejected");
            fs::remove(root / ".same");
            same::initialize_workspace(root);
            fs::create_directory_symlink(root / "outside", root / ".same/link");
            same::clean_workspace(root, false);
            check(fs::exists(root / "outside/.same/keep"), "payload links never followed");
            fs::create_directory_symlink(root / "outside", root / "child/link");
            check(same::clean_workspace(root / "child", true) == 0,
                  "recursive discovery skips linked directories");
            check(fs::exists(root / "outside/.same/keep"), "external linked state preserved");
            // 持续交换目录与外部链接，失败可接受但外部数据必须保留。
            // Race directory/link replacement: failure is safe, external deletion is not.
            for (int trial = 0; trial < 12; ++trial) {
                const auto race = root / ("race-" + std::to_string(trial));
                fs::create_directories(race / ".same/payload");
                for (int item = 0; item < 32; ++item)
                    std::ofstream(race / ".same/payload" / std::to_string(item)) << "x";
                std::atomic<bool> stop{false};
                std::thread attacker([&] {
                    while (!stop.load()) {
                        std::error_code ignored;
                        fs::rename(race / ".same/payload", race / "held", ignored);
                        if (ignored)
                            continue;
                        fs::create_directory_symlink(root / "outside", race / ".same/payload",
                                                     ignored);
                        if (!ignored)
                            fs::remove(race / ".same/payload", ignored);
                        fs::rename(race / "held", race / ".same/payload", ignored);
                    }
                });
                try {
                    same::clean_workspace(race, false);
                } catch (const std::exception&) {
                }
                stop.store(true);
                attacker.join();
                check(fs::exists(root / "outside/.same/keep"),
                      "replacement race preserves external data");
            }
        }
        fs::remove_all(root);
        std::cout << "workspace tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        fs::remove_all(root);
        return 1;
    }
}
