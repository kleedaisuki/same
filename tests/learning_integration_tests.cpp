/** @file
 * @brief 无 Python 的有界 CLI 回归测试。 / Bounded, Python-free CLI regressions.
 */
#include "same/model_store.hpp"
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sqlite3.h>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <shellapi.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
namespace fs = std::filesystem;
using Group = std::set<std::string>;
using Groups = std::set<Group>;

/// 失败时保留具体断言。 / Preserve the failing assertion in diagnostics.
void check(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

/// 以 UTF-8 保留路径字节，避免 Windows 本地代码页。 / Preserve UTF-8, not the Windows code page.
std::string utf8(const fs::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

/// 显式按 UTF-8 构造原生路径，不依赖本地代码页。 / Construct native paths from explicit UTF-8
/// bytes.
fs::path native_path(std::string_view value) {
    return fs::path(
        std::u8string_view(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}

/// Windows 直接读取 UTF-16 命令行，不受 CRT 窄字符参数的本地代码页限制。
/// Read Windows UTF-16 arguments directly, bypassing the CRT narrow-argument code page.
fs::path executable_path(const char* argument) {
#ifdef _WIN32
    (void)argument;
    int count = 0;
    auto release = [](wchar_t** values) { LocalFree(values); };
    std::unique_ptr<wchar_t*, decltype(release)> values(
        CommandLineToArgvW(GetCommandLineW(), &count), release);
    check(values != nullptr && count == 2, "cannot parse executable argument");
    return fs::absolute(fs::path(values.get()[1]));
#else
    return fs::absolute(fs::path(argument));
#endif
}

/// 二进制读取用于输出和数据库不变性比较。 / Read bytes for output and database invariance checks.
std::string read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    check(bool(stream), "cannot read " + utf8(path));
    return {std::istreambuf_iterator<char>(stream), {}};
}

/// 每次进程最多运行 30 秒；输出存入扫描目录之外。 / Limit each child to 30 seconds; capture outside
/// the scan root.
int execute(const fs::path& exe, const fs::path& root, const fs::path& out, const fs::path& err,
            const std::vector<std::string>& arguments = {}) {
#ifdef _WIN32
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE output = CreateFileW(out.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE errors = CreateFileW(err.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE || errors == INVALID_HANDLE_VALUE) {
        if (output != INVALID_HANDLE_VALUE)
            CloseHandle(output);
        if (errors != INVALID_HANDLE_VALUE)
            CloseHandle(errors);
        throw std::runtime_error("create capture files");
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = output;
    startup.hStdError = errors;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    auto command = L"\"" + exe.wstring() + L"\"";
    // Test arguments are fixed ASCII options without spaces or shell syntax.
    // 测试参数均为无空格或 shell 语法的固定 ASCII 选项。
    for (const auto& arg : arguments)
        command += L" " + std::wstring(arg.begin(), arg.end());
    const bool created =
        CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                       nullptr, root.c_str(), &startup, &process);
    CloseHandle(output);
    CloseHandle(errors);
    check(created, "CreateProcessW failed");
    const auto waited = WaitForSingleObject(process.hProcess, 30000);
    if (waited != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 124);
        WaitForSingleObject(process.hProcess, 5000);
    }
    DWORD code = 0;
    const bool retrieved = GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    check(waited == WAIT_OBJECT_0 && retrieved, "CLI timed out or wait failed");
    return static_cast<int>(code);
#else
    const auto child = fork();
    check(child >= 0, "fork failed");
    if (child == 0) {
        const int output = open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        const int errors = open(err.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (output < 0 || errors < 0 || dup2(output, STDOUT_FILENO) < 0 ||
            dup2(errors, STDERR_FILENO) < 0 || chdir(root.c_str()) != 0)
            _exit(126);
        close(output);
        close(errors);
        std::vector<char*> args{const_cast<char*>(exe.c_str())};
        for (const auto& arg : arguments)
            args.push_back(const_cast<char*>(arg.c_str()));
        args.push_back(nullptr);
        execv(exe.c_str(), args.data());
        _exit(127);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    int status{};
    for (;;) {
        const auto waited = waitpid(child, &status, WNOHANG);
        if (waited == child)
            break;
        if (waited < 0 && errno != EINTR) {
            const int error = errno;
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
            }
            throw std::runtime_error("waitpid failed: " + std::to_string(error));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
            }
            throw std::runtime_error("CLI timed out");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(WIFEXITED(status), "CLI terminated by signal");
    return WEXITSTATUS(status);
#endif
}

/// 取得数据库唯一学习身份。 / Read the sole persisted learning identity.
std::string stored_key(const fs::path& path) {
    sqlite3* db{};
    check(sqlite3_open(utf8(path).c_str(), &db) == SQLITE_OK, "open model database");
    sqlite3_stmt* statement{};
    check(sqlite3_prepare_v2(db, "SELECT key FROM models", -1, &statement, nullptr) == SQLITE_OK,
          "prepare model key");
    check(sqlite3_step(statement) == SQLITE_ROW, "missing learned model");
    const auto* bytes = static_cast<const char*>(sqlite3_column_blob(statement, 0));
    std::string key(bytes, sqlite3_column_bytes(statement, 0));
    check(sqlite3_step(statement) == SQLITE_DONE, "unexpected duplicate model keys");
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return key;
}
/// 汇总中的明确计数，不解析展示顺序。 / Extract an explicit summary count independent of ordering.
std::uint64_t metric(const std::string& text, const std::string& name) {
    std::smatch match;
    check(std::regex_search(text, match, std::regex("(?:^|[ \\n])" + name + "=([0-9]+)")),
          "missing " + name);
    return std::stoull(match[1]);
}
} // namespace
int main(int argc, char** argv) {
    fs::path base;
    try {
        check(argc == 2, "expected CLI executable");
        const auto exe = executable_path(argv[1]);
        base = fs::temp_directory_path() /
               ("same-learning-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const auto root = base / "root";
        fs::create_directories(root / ".same");
        std::ofstream(root / ".same" / "config.toml", std::ios::binary)
            << "workers=2\nmetadata_workers=1\nblock_bytes=65536\nmemory_bytes=8388608\ndevice_"
               "memory_bytes=1048576\nqueue_capacity=4\ngpu_min_bytes=0\n";
        const std::string payload(131073, 'x');
        for (unsigned i = 0; i < 16; ++i)
            std::ofstream(root / ("file" + std::to_string(i)), std::ios::binary) << payload;
        const auto out = base / "out", err = base / "err", database = root / ".same" / "model.db";
        auto run = [&](bool pgo = true) {
            std::vector<std::string> args{"--cpu", "--rehash", "--no-telemetry", "--summary"};
            if (!pgo)
                args.push_back("--no-pgo");
            const auto code = execute(exe, root, out, err, args);
            check(code == 0, "CLI failed: " + (fs::exists(err) ? read(err) : ""));
        };
        run();
        const auto expected = read(out);
        check(!expected.empty(), "missing duplicate output");
        check(metric(read(err), "model_saved_keys") == 1, "model not saved");
        const auto key = stored_key(database);
        same::detail::OnlineModel::State first{};
        {
            same::ModelStore store(database);
            first = *store.load(key);
        }
        // 队列容量不是设备身份，改变它仍应复用相同先验。
        // Queue capacity is not device identity; changing it must retain the same prior.
        auto config_text = read(root / ".same" / "config.toml");
        config_text.replace(config_text.find("queue_capacity=4"), 16, "queue_capacity=8");
        std::ofstream(root / ".same" / "config.toml", std::ios::binary) << config_text;
        run();
        check(read(out) == expected, "learning changed byte groups");
        check(metric(read(err), "model_prior_hits") == 2, "both workers did not load prior");
        {
            same::ModelStore store(database);
            const auto second = *store.load(key);
            const auto delta = second[0].samples - first[0].samples;
            check(delta == 16, "prior was multiplied by worker count or run samples lost");
            check(std::abs(second[0].weight - (0.9 * first[0].weight + 16)) < 1e-8,
                  "decayed prior not merged exactly once");
        }
        const auto before = read(database);
        run(false);
        check(read(database) == before, "no-pgo changed model database");
        check(read(out) == expected, "no-pgo changed groups");
        check(!fs::exists(root / ".same" / "telemetry.db"), "no-telemetry wrote telemetry");
        std::ofstream(database, std::ios::binary | std::ios::trunc) << "not a database";
        run();
        check(read(out) == expected, "corrupt model changed groups");
        check(read(err).find("Model learning warning:") != std::string::npos,
              "corrupt model did not warn");
        fs::remove_all(base);
        std::cout << "Cross-run learning CLI tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << " fixture=" << base << '\n';
        return 1;
    }
}
