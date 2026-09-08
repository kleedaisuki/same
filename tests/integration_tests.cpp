/** @file
 * @brief 无 Python 的有界 CLI 回归测试。 / Bounded, Python-free CLI regressions.
 */
#include "same/run_lock.hpp"
#include <cerrno>
#include <chrono>
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

/// 编码期望路径，直接验证 CLI 的 JSON 字符转义契约。 / Encode expected paths to verify CLI JSON
/// escaping.
std::string json_path(std::string_view value) {
    std::ostringstream out;
    out << '"';
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char ch : value) {
        if (ch == '"' || ch == '\\')
            out << '\\' << char(ch);
        else if (ch < 32 || ch == 127)
            out << "\\u00" << hex[ch >> 4] << hex[ch & 15];
        else
            out << char(ch);
    }
    out << '"';
    return out.str();
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

/// 每个场景独立目录及配置；析构只删除自身目录。 / Own one isolated fixture and remove only its
/// directory.
struct Fixture {
    /// 被测可执行文件的绝对路径。 / Absolute executable under test.
    fs::path exe;
    /// 包含捕获文件与扫描根目录的临时目录。 / Temporary parent for captures and scan root.
    fs::path base;
    /// CLI 的工作目录。 / CLI working directory.
    fs::path root;
    /// 上次成功运行的计数器。 / Counters from the last successful run.
    std::map<std::string, int> stats;

    /// 创建唯一测试根并写入有界 CPU 默认配置。 / Create a unique root with bounded CPU defaults.
    explicit Fixture(const fs::path& executable) : exe(executable) {
        base = fs::temp_directory_path() /
               ("same-integration-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        check(fs::create_directory(base), "create unique fixture");
        root = base / "root";
        fs::create_directories(root / ".same");
        config();
    }
    /// 失败路径也回收目录，且不掩盖原始异常。 / Clean failure paths without masking the original
    /// error.
    ~Fixture() {
        std::error_code error;
        fs::remove_all(base, error);
    }

    /// TOML 覆盖项替换默认值，避免重复键。 / Merge TOML overrides without duplicate keys.
    void config(std::map<std::string, std::string> overrides = {}) {
        std::map<std::string, std::string> values{{"workers", "2"},
                                                  {"block_bytes", "1024"},
                                                  {"memory_bytes", "1048576"},
                                                  {"device_memory_bytes", "1048576"},
                                                  {"queue_capacity", "1"},
                                                  {"backend", "\"cpu\""}};
        for (const auto& [key, value] : overrides)
            values[key] = value;
        std::string text;
        for (const auto& [key, value] : values)
            text += key + " = " + value + "\n";
        file(".same/config.toml", text);
    }
    /// 创建父目录并逐字节写入测试文件。 / Create parents and write exact fixture bytes.
    fs::path file(std::string_view name, std::string_view content) {
        const auto path = root / native_path(name);
        fs::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary);
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        output.close();
        check(bool(output), "write fixture");
        return path;
    }
    /// 校验退出码、组编号、统计字段与输出一致性。 / Validate exit status, group IDs and
    /// output/counter consistency.
    Groups run(int code = 0,
               const std::vector<std::string>& arguments = {"scan", "-r", "--summary"}) {
        const auto actual = execute(exe, root, base / "stdout", base / "stderr", arguments);
        const auto output = read(base / "stdout");
        const auto errors = read(base / "stderr");
        check(actual == code, "exit code " + std::to_string(actual) + ": " + errors);
        if (code != 0) {
            check(output.empty(), "failure emitted groups");
            return {};
        }
        std::map<int, Group> parsed;
        std::istringstream lines(output);
        std::string line;
        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            const auto tab = line.find('\t');
            check(tab != std::string::npos && tab > 0 &&
                      line.find_first_not_of("0123456789") == tab,
                  "invalid group line");
            parsed[std::stoi(line.substr(0, tab))].insert(line.substr(tab + 1));
        }
        stats.clear();
        const std::regex counters("(\\w+)=(\\d+)");
        for (auto it = std::sregex_iterator(errors.begin(), errors.end(), counters);
             it != std::sregex_iterator(); ++it)
            stats[(*it)[1]] = std::stoi((*it)[2]);
        for (const auto* key :
             {"scanned", "hashed", "cached", "groups", "matches", "gpu_workers", "cpu_fallbacks"})
            check(stats.contains(key), "missing counter " + std::string(key));
        Groups result;
        int members = 0;
        for (const auto& [id, group] : parsed) {
            result.insert(group);
            members += static_cast<int>(group.size());
        }
        check(stats["groups"] == static_cast<int>(parsed.size()) && stats["matches"] == members,
              "inconsistent statistics");
        return result;
    }
    /// 精确比较完整组集合，不依赖扫描顺序。 / Compare complete groups independent of scan order.
    void expect(const Groups& groups) {
        check(run() == groups, "duplicate groups differ");
    }
    /// 用 SQLite C API 注入碰撞或检查缓存状态。 / Inject collisions or inspect cache through
    /// SQLite's C API.
    void sql(const char* statement, int (*callback)(void*, int, char**, char**) = nullptr,
             void* context = nullptr) {
        sqlite3* db{};
        const auto status = sqlite3_open(utf8(root / ".same/state.db").c_str(), &db);
        if (status != SQLITE_OK) {
            sqlite3_close(db);
            throw std::runtime_error("open SQLite fixture");
        }
        const auto result = sqlite3_exec(db, statement, callback, context, nullptr);
        sqlite3_close(db);
        check(result == SQLITE_OK, "SQLite fixture statement failed");
    }
    /// 无权限平台显式跳过链接场景。 / Explicitly skip links when platform privileges disallow them.
    bool symlink(const fs::path& source, const fs::path& destination, bool directory = false) {
        std::error_code error;
        if (directory)
            fs::create_directory_symlink(source, destination, error);
        else
            fs::create_symlink(source, destination, error);
        if (error)
            std::cout << "SKIP symlink: " << error.message() << '\n';
        return !error;
    }
};

/// 将未转义路径集合转换为 CLI 输出期望值。 / Convert raw path sets to expected CLI strings.
Group group(std::initializer_list<std::string> paths) {
    Group result;
    for (const auto& path : paths)
        result.insert(json_path(path));
    return result;
}

/// 空文件、BLAKE3 块边界、多块与热缓存。 / Cover empty input, BLAKE3 boundaries, multi-block input
/// and warm cache.
void boundaries(const fs::path& exe) {
    Fixture f(exe);
    Groups expected;
    for (int size : {0, 1, 63, 64, 65, 1023, 1024, 1025, 2048, 3073}) {
        std::string content(static_cast<std::size_t>(size), '\0');
        for (int i = 0; i < size; ++i)
            content[static_cast<std::size_t>(i)] = static_cast<char>((i * 17 + size) % 256);
        const auto a = "a/" + std::to_string(size), b = "b/" + std::to_string(size);
        f.file(a, content);
        f.file(b, content);
        expected.insert(group({a, b}));
        if (size) {
            content[0] ^= 1;
            f.file("unique/" + std::to_string(size), content);
        }
    }
    f.expect(expected);
    check(f.stats["scanned"] == 29 && f.stats["hashed"] == 29 && f.stats["cached"] == 0,
          "cold boundaries");
    f.expect(expected);
    check(f.stats["hashed"] == 0 && f.stats["cached"] == 29, "warm boundaries");
}

/// 恢复 mtime 不能掩盖内容更新，重命名和删除必须反映到数据库。 / Restored mtime must not hide
/// edits; rename/deletion must update the cache.
void cache_changes(const fs::path& exe) {
    Fixture f(exe);
    const auto a = f.file("a", "abcd");
    f.file("b", "abcd");
    f.run();
    const auto stamp = fs::last_write_time(a);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    f.file("a", "abce");
    fs::last_write_time(a, stamp);
    f.expect({});
    check(f.stats["hashed"] == 1 && f.stats["cached"] == 1, "restored mtime");
    f.file("a", "abcd");
    f.file("c", "different");
    f.run();
    fs::rename(f.root / "b", f.root / "renamed");
    fs::remove(f.root / "c");
    f.expect({group({"a", "renamed"})});
    check(f.stats["scanned"] == 2, "rename scan count");
    Group paths;
    f.sql(
        "SELECT CAST(path AS TEXT) FROM files",
        [](void* context, int, char** row, char**) {
            static_cast<Group*>(context)->insert(row[0]);
            return 0;
        },
        &paths);
    check(paths == Group{"a", "renamed"}, "stale database paths");
    f.config({{"rehash", "true"}});
    f.run();
    check(f.stats["hashed"] == 2 && f.stats["cached"] == 0, "forced rehash");
}

/// 忽略规则的否定不能重新纳入内部状态目录。 / Negations cannot reinclude internal state.
void ignores(const fs::path& exe) {
    Fixture f(exe);
    for (auto name : {"a", "drop.tmp", "dir/drop", "dir/keep", ".same/hidden"})
        f.file(name, "same");
    f.file(".same/ignore", "*.tmp\ndir/*\n!dir/keep\n!.same/hidden\n");
    f.expect({group({"a", "dir/keep"})});
    check(f.stats["scanned"] == 2, "ignore count");
}

/// 保留硬链接、UTF-8 与 POSIX 控制字符路径的逐字节身份。 / Preserve hard links, UTF-8 and POSIX
/// control-character paths.
void paths(const fs::path& exe) {
    Fixture f(exe);
    auto original = f.file("original", "linked content");
    std::error_code error;
    fs::create_hard_link(original, f.root / "alias", error);
    if (!error) {
        f.file("copy", "linked content");
        f.expect({group({"original", "alias", "copy"})});
    } else
        std::cout << "SKIP hardlink: " << error.message() << '\n';
    Fixture unicode(exe);
    const std::string chinese = "中文/可莉.txt", japanese = "日本語/クレー.txt";
    unicode.file(chinese, "unicode");
    unicode.file(japanese, "unicode");
    unicode.expect({group({chinese, japanese})});
#ifdef _WIN32
    // 长目录与中文文件名必须贯穿扫描、哈希、输出及缓存复用。
    // Long directories and Unicode names must survive scanning, hashing, output and cache reuse.
    Fixture long_paths(exe);
    const std::string deep = std::string(100, 'a') + "/" + std::string(100, 'b') + "/论文资料/" +
                             std::string(60, 'c') + ".pdf";
    const fs::path extended(L"\\\\?\\" +
                            (long_paths.root / native_path(deep)).make_preferred().native());
    fs::create_directories(extended.parent_path());
    {
        std::ofstream file(extended, std::ios::binary);
        file << "long path content";
        check(bool(file), "create long path fixture");
    }
    long_paths.file("copy.pdf", "long path content");
    long_paths.expect({group({deep, "copy.pdf"})});
    check(long_paths.stats["hashed"] == 2, "long path cold scan");
    long_paths.expect({group({deep, "copy.pdf"})});
    check(long_paths.stats["cached"] == 2, "long path warm scan");
#endif
#ifndef _WIN32
    Fixture controls(exe);
    for (auto name : {"line\nbreak", "tab\tand\"quote", "back\\slash"})
        controls.file(name, "controls");
    controls.expect({group({"line\nbreak", "tab\tand\"quote", "back\\slash"})});
#endif
}

/// 忽略普通符号链接并拒绝状态位置的链接，防止越界写入。 / Ignore ordinary symlinks and reject
/// linked state paths to prevent writes outside state.
void symlinks(const fs::path& exe) {
    {
        Fixture f(exe);
        f.file("a", "same");
        f.file("b", "same");
        if (f.symlink(f.root / "a", f.root / "link") && f.symlink(f.root, f.root / "loop", true)) {
            f.expect({group({"a", "b"})});
            check(f.stats["scanned"] == 2, "symlink count");
        }
    }
    {
        Fixture f(exe);
        auto target = f.file("untouched", "do not modify");
        if (f.symlink(target, f.root / ".same/state.db")) {
            f.run(2);
            check(read(target) == "do not modify", "linked database modified");
        }
    }
    {
        Fixture f(exe);
        fs::remove(f.root / ".same/config.toml");
        fs::remove(f.root / ".same");
        auto target = f.root / "state-target";
        fs::create_directory(target);
        if (f.symlink(target, f.root / ".same", true)) {
            f.run(2);
            check(fs::is_empty(target), "linked state directory modified");
        }
    }
}

/// 无效配置必须在任何数据库创建或修改之前失败。 / Invalid configuration must fail before database
/// creation or mutation.
void invalid_config(const fs::path& exe) {
    Fixture f(exe);
    for (auto text : {"workers = 0", "backend = \"typo\"", "unknown = 1", "rehash = \"yes\"",
                      "block_bytes = 1", "workers = 256\nmemory_bytes = 1"}) {
        f.file(".same/config.toml", text);
        f.run(2);
        check(!fs::exists(f.root / ".same/state.db"), "invalid config created database");
    }
    f.config();
    f.file("a", "a");
    f.run();
    auto before = read(f.root / ".same/state.db");
    f.file(".same/config.toml", "workers = 0");
    f.run(2);
    check(read(f.root / ".same/state.db") == before, "invalid config modified database");
}

/// 伪造相同摘要仍必须按内容分组；并行桶和单槽队列不得丢任务。 / Forged hashes require exact
/// partitioning; parallel buckets and single-slot queues must not lose work.
void collisions_and_queue(const fs::path& exe) {
    Fixture f(exe);
    f.config({{"queue_capacity", "4"}});
    for (auto name : {"a1", "a2"})
        f.file(name, "AAAA");
    for (auto name : {"b1", "b2"})
        f.file(name, "BBBB");
    f.file("c", "CCCC");
    f.run();
    f.sql("UPDATE files SET digest=zeroblob(32)");
    f.expect({group({"a1", "a2"}), group({"b1", "b2"})});
    check(f.stats["cached"] == 5 && f.stats["hashed"] == 0, "collision cache");
    check(execute(exe, f.root, f.base / "stdout", f.base / "stderr",
                  {"--format=pretty", "--color=never", "--unique-files"}) == 0,
          "collision report failed");
    const auto report = read(f.base / "stdout");
    const auto unique = report.find("[UNIQUE]");
    check(unique != std::string::npos && report.find("\"c\"", unique) != std::string::npos,
          "collision singleton lost from unique report");
    check(report.find("[SAME] Group 2") != std::string::npos, "collision groups merged");
    Fixture many(exe);
    Group names;
    for (int i = 0; i < 50; ++i) {
        auto name = "file-" + std::to_string(i);
        many.file(name, std::string(3600, 'b'));
        names.insert(json_path(name));
    }
    many.expect({names});
    check(many.stats["hashed"] == 50, "single-slot hash count");
    many.expect({names});
    check(many.stats["cached"] == 50, "single-slot cache count");
    Fixture parallel(exe);
    parallel.config({{"workers", "4"}, {"queue_capacity", "5"}});
    Groups expected;
    for (int bucket = 0; bucket < 6; ++bucket) {
        Group members;
        for (int member = 0; member < 7; ++member) {
            auto name = "g" + std::to_string(bucket) + "-" + std::to_string(member);
            parallel.file(name, std::string(static_cast<std::size_t>(1024 + bucket % 3),
                                            static_cast<char>(bucket)));
            members.insert(json_path(name));
        }
        expected.insert(members);
    }
    parallel.expect(expected);
    parallel.sql("UPDATE files SET digest=zeroblob(32)");
    parallel.expect(expected);
    check(parallel.stats["cached"] == 42, "parallel collision cache");
}

/// CLI overrides preserve redirected TSV and reject invalid values before touching state.
/// 命令行覆盖保留重定向 TSV；在修改状态前拒绝非法参数。
void presentation(const fs::path& exe) {
    Fixture f(exe);
    f.file("a", "same");
    f.file("b", "same");
    f.file("unique", "different");
    for (auto arg : {"--color=bad", "--format=bad"}) {
        check(execute(exe, f.root, f.base / "stdout", f.base / "stderr", {arg}) == 2,
              "invalid presentation accepted");
        check(!fs::exists(f.root / ".same/state.db"), "invalid option created database");
    }
    f.run();
    const auto legacy = read(f.base / "stdout");
    check(legacy.find('\033') == std::string::npos, "redirected auto color");
    check(execute(exe, f.root, f.base / "stdout", f.base / "stderr",
                  {"--color=never", "--format=tsv"}) == 0,
          "explicit TSV failed");
    check(read(f.base / "stdout") == legacy, "explicit TSV changed bytes");
    check(execute(exe, f.root, f.base / "stdout", f.base / "stderr",
                  {"scan", "-r", "--summary", "--color=always", "--format=pretty",
                   "--unique-files"}) == 0,
          "forced pretty failed");
    const auto colored = read(f.base / "stdout");
    check(colored.find("\033[32m") != std::string::npos &&
              colored.find("\033[33m") != std::string::npos,
          "forced color missing");
    check(colored.find("[UNIQUE]") != std::string::npos &&
              colored.find("\"unique\"") != std::string::npos,
          "unique file missing");
    check(read(f.base / "stderr").find("\033[1;36mSummary") != std::string::npos,
          "profile color missing");
    check(execute(exe, f.root, f.base / "stdout", f.base / "stderr",
                  {"--color=never", "--format=pretty"}) == 0,
          "plain pretty failed");
    check(read(f.base / "stdout").find('\033') == std::string::npos, "never emitted escapes");
    const auto folded = read(f.base / "stdout");
    check(folded.find("[UNIQUE]") == std::string::npos &&
              folded.find("\"unique\"") == std::string::npos,
          "unique not folded by default");
    check(read(f.base / "stderr").find('\033') == std::string::npos, "never colored profile");
    check(execute(exe, f.root, f.base / "stdout", f.base / "stderr",
                  {"scan", "-r", "--summary", "--unique-files", "--format=tsv", "--color=never"}) ==
              0,
          "unique TSV failed");
    check(read(f.base / "stdout").find("0\t\"unique\"") != std::string::npos, "unique TSV missing");
    check(read(f.base / "stderr").find("elapsed_ms=") != std::string::npos, "raw profile changed");
}

/// 默认浅扫描、显式递归和诊断开关必须保持独立。 / Keep shallow defaults, recursion and diagnostics
/// independent.
void scan_commands(const fs::path& exe) {
    Fixture f(exe);
    f.file("a", "same");
    f.file("b", "same");
    f.file("nested/c", "same");
    f.file("nested/deeper/d", "same");
    check(execute(exe, f.root, f.base / "stdout", f.base / "stderr") == 0, "default scan failed");
    const auto shallow = read(f.base / "stdout");
    check(shallow == "1\t\"a\"\n1\t\"b\"\n" || shallow == "1\t\"a\"\r\n1\t\"b\"\r\n",
          "default scan must omit nested files");
    check(read(f.base / "stderr").empty(), "default emitted diagnostics");
    check(execute(exe, f.root, f.base / "stdout", f.base / "stderr", {"scan"}) == 0,
          "explicit shallow scan failed");
    check(read(f.base / "stdout") == shallow && read(f.base / "stderr").empty(),
          "same differs from same scan");
    check(f.run(0, {"scan", "-r", "--summary"}) ==
              Groups{group({"a", "b", "nested/c", "nested/deeper/d"})},
          "recursive scan omitted descendants");
    check(f.stats["scanned"] == 4, "recursive scan count");
    check(f.run(0, {"scan", "--summary"}) == Groups{group({"a", "b"})},
          "recursive cache leaked into shallow result");
    check(f.stats["scanned"] == 2, "shallow cache scan count");
    check(f.run(0, {"scan", "-r", "--summary"}) ==
              Groups{group({"a", "b", "nested/c", "nested/deeper/d"})},
          "recursive rescan failed");
    for (const auto& arguments : std::vector<std::vector<std::string>>{
             {"scan", "-r"}, {"scan", "--format=pretty", "--color=never"}}) {
        check(execute(exe, f.root, f.base / "stdout", f.base / "stderr", arguments) == 0,
              "quiet scan failed");
        check(read(f.base / "stderr").empty(), "scan without summary emitted diagnostic panels");
    }
}

/// 初始化及清理命令实际分发至生命周期实现。 / Dispatch initialization and cleanup to lifecycle
/// operations.
void lifecycle_commands(const fs::path& exe) {
    Fixture f(exe);
    fs::remove_all(f.root / ".same");
    check(execute(exe, f.root, f.base / "stdout", f.base / "stderr", {"new"}) == 0,
          "new command failed");
    const auto config = read(f.root / ".same/config.toml");
    for (const auto* key :
         {"workers", "metadata_workers", "gpu_min_bytes", "gpu_probe_bytes", "block_bytes",
          "memory_bytes", "device_memory_bytes", "queue_capacity", "backend", "rehash"})
        check(config.find(std::string(key) + " =") != std::string::npos,
              "new omitted default " + std::string(key));
    check(!read(f.root / ".same/ignore").empty(), "new omitted recommended ignore");
    f.file(".same/config.toml", "# custom configuration\n");
    f.file(".same/ignore", "custom-ignore\n");
    const auto repeated = execute(exe, f.root, f.base / "stdout", f.base / "stderr", {"new"});
    check(repeated == 0, "repeated new must succeed without replacing files");
    check(read(f.root / ".same/config.toml") == "# custom configuration\n" &&
              read(f.root / ".same/ignore") == "custom-ignore\n",
          "new overwrote existing files");
    f.file("child/.same/marker", "nested state");
    f.file("child/data", "keep user data");
    check(execute(exe, f.root, f.base / "stdout", f.base / "stderr", {"clean"}) == 0,
          "clean command failed");
    check(!fs::exists(f.root / ".same") && fs::exists(f.root / "child/.same/marker"),
          "shallow clean removed nested state or retained root state");
    check(execute(exe, f.root, f.base / "stdout", f.base / "stderr", {"clean", "-r"}) == 0,
          "recursive clean command failed");
    check(!fs::exists(f.root / "child/.same") && read(f.root / "child/data") == "keep user data",
          "recursive clean omitted state or removed user data");
}

/// 子命令拒绝无效组合且不隐式创建状态。 / Reject invalid command combinations without creating
/// state.
void command_validation(const fs::path& exe) {
    Fixture f(exe);
    fs::remove_all(f.root / ".same");
    for (const auto& arguments : std::vector<std::vector<std::string>>{{"unknown"},
                                                                       {"scan", "new"},
                                                                       {"new", "-r"},
                                                                       {"clean", "--summary"},
                                                                       {"scan", "--unknown"},
                                                                       {"scan", "--sumary"},
                                                                       {"clean", "--cpu"},
                                                                       {"clean", "--no-pgo"},
                                                                       {"new", "--no-pgo"}}) {
        check(execute(exe, f.root, f.base / "stdout", f.base / "stderr", arguments) == 2,
              "invalid command accepted");
        check(!fs::exists(f.root / ".same"), "invalid command created state");
    }
    for (const auto& arguments : std::vector<std::vector<std::string>>{{"--help"},
                                                                       {"scan", "--help"},
                                                                       {"new", "--help"},
                                                                       {"clean", "--help"},
                                                                       {"--version"}}) {
        check(execute(exe, f.root, f.base / "stdout", f.base / "stderr", arguments) == 0,
              "help/version failed");
        check(!read(f.base / "stdout").empty() && !fs::exists(f.root / ".same"),
              "help/version mutated state or omitted output");
    }
}

/// 自动模式在设备预算不足时回退；显式环境变量开启真实 GPU 回归。 / Auto mode falls back on
/// insufficient device budget; an environment opt-in exercises a real GPU.
void backends(const fs::path& exe) {
    Fixture f(exe);
    f.config({{"backend", "\"auto\""},
              {"device_memory_bytes", "1"},
              {"gpu_min_bytes", "0"},
              {"gpu_probe_bytes", "0"}});
    f.file("a", std::string(8192, 'f'));
    f.file("b", std::string(8192, 'f'));
    f.expect({group({"a", "b"})});
    check(f.stats["gpu_workers"] == 0, "tiny device budget");
    check(f.stats["gpu_hashes"] == 0 && f.stats["cpu_hashes"] == 2,
          "unavailable auto must use CPU");
    const char* required = std::getenv("SAME_REQUIRE_CUDA");
    if (!required || std::string_view(required) != "1") {
        std::cout << "SKIP real CUDA (set SAME_REQUIRE_CUDA=1)\n";
        return;
    }
    Fixture gpu(exe);
    gpu.config({{"backend", "\"cuda\""},
                {"gpu_min_bytes", "0"},
                {"workers", "3"},
                {"block_bytes", "65536"},
                {"queue_capacity", "2"}});
    std::string content(1048576 + 113, '\0');
    for (std::size_t i = 0; i < content.size(); ++i)
        content[i] = static_cast<char>(i % 256);
    for (auto name : {"a", "nested/b", "c", "d"})
        gpu.file(name, content);
    content.back() ^= 1;
    gpu.file("distinct", content);
    gpu.expect({group({"a", "nested/b", "c", "d"})});
    check(gpu.stats["gpu_workers"] == 3 && gpu.stats["cpu_fallbacks"] == 0 &&
              gpu.stats["hashed"] == 5 && gpu.stats["gpu_hashes"] == 5 &&
              gpu.stats["cpu_hashes"] == 0,
          "CUDA cold scan");
    gpu.expect({group({"a", "nested/b", "c", "d"})});
    check(gpu.stats["gpu_workers"] == 3 && gpu.stats["cpu_fallbacks"] == 0 &&
              gpu.stats["hashed"] == 0 && gpu.stats["cached"] == 5,
          "CUDA warm scan");
    // 自动选择结果依赖硬件；无论选择如何，摘要/输出必须保持不变。
    // Auto decisions depend on hardware; digest/output correctness must not depend on the decision.
    gpu.config({{"backend", "\"auto\""},
                {"gpu_probe_bytes", "0"},
                {"gpu_min_bytes", "0"},
                {"workers", "3"},
                {"block_bytes", "65536"},
                {"rehash", "true"}});
    gpu.expect({group({"a", "nested/b", "c", "d"})});
    check(gpu.stats["gpu_workers"] <= 1 && gpu.stats["cpu_hashes"] + gpu.stats["gpu_hashes"] == 5,
          "auto must keep at most one calibrated GPU lane and account for real work");
}

/// 大小路由必须保留完整摘要/缓存语义，并能显式禁用。 / Size routing preserves hashes/cache and can
/// be disabled.
void size_routing(const fs::path& exe) {
    Fixture f(exe);
    f.config({{"gpu_min_bytes", "4096"}, {"metadata_workers", "4"}, {"queue_capacity", "3"}});
    f.file("small-a", std::string(4095, 'a'));
    f.file("small-b", std::string(4095, 'a'));
    f.file("large-a", std::string(4096, 'b'));
    f.file("large-b", std::string(4096, 'b'));
    const Groups expected{group({"small-a", "small-b"}), group({"large-a", "large-b"})};
    f.expect(expected);
    check(f.stats["cpu_routed_hashes"] == 2, "size route threshold boundary");
    f.expect(expected);
    check(f.stats["cached"] == 4 && f.stats["cpu_routed_hashes"] == 0, "size route warm cache");
    f.config({{"gpu_min_bytes", "0"}, {"rehash", "true"}});
    f.expect(expected);
    check(f.stats["cpu_routed_hashes"] == 0 && f.stats["hashed"] == 4, "size route disabled");
    // 小载荷资格下界，而不是退役的批次字节门槛，阻止设备初始化。
    // The payload floor, not the retired volume gate, prevents GPU setup for small files.
    f.config({{"backend", "\"auto\""},
              {"gpu_min_bytes", "4097"},
              {"gpu_probe_bytes", "0"},
              {"rehash", "true"}});
    f.expect(expected);
    check(f.stats["gpu_workers"] == 0 && f.stats["calibration_ms"] == 0 &&
              f.stats["gpu_setup_ms"] == 0 && f.stats["cpu_hashes"] == 4,
          "below-floor payloads must not pay CUDA setup or calibration");
    check(read(f.base / "stderr").find("using CPU fallback") == std::string::npos,
          "intentional auto CPU policy must not report unavailable CUDA");
    f.config({{"backend", "\"auto\""},
              {"gpu_min_bytes", "0"},
              {"gpu_probe_bytes", "4294967296"},
              {"rehash", "true"}});
    f.expect(expected);
    check(f.stats["cpu_hashes"] + f.stats["gpu_hashes"] == 4 && f.stats["cpu_fallbacks"] == 0,
          "adaptive attempts or retry accounting changed");
    if (std::getenv("SAME_REQUIRE_CUDA"))
        check(f.stats["gpu_workers"] == 1 && f.stats["gpu_setup_ms"] > 0,
              "retired volume gate still blocks eligible work");
    f.config({{"backend", "\"auto\""}, {"gpu_min_bytes", "0"}});
    f.expect(expected);
    check(f.stats["cached"] == 4 && f.stats["gpu_workers"] == 0 && f.stats["gpu_setup_ms"] == 0,
          "cached eligible files initialized CUDA");
}

/// 分析器开关不改变摘要与缓存；禁用后不得产生在线样本。
/// Analyzer control preserves digests/cache and suppresses online samples when disabled.
void pgo_control(const fs::path& exe) {
    Fixture f(exe);
    f.config({{"pgo", "true"}, {"gpu_min_bytes", "0"}});
    f.file("a", std::string(4096, 'p'));
    f.file("b", std::string(4096, 'p'));
    const Groups expected{group({"a", "b"})};
    f.expect(expected);
    check(f.stats["pgo_enabled"] == 1 && f.stats["pgo_samples"] == 2,
          "enabled analyzer did not observe successful eligible CPU hashes");
    check(execute(exe, f.root, f.base / "stdout", f.base / "stderr",
                  {"scan", "--summary", "--rehash", "--format=pretty"}) == 0,
          "pretty online summary failed");
    const auto pretty = read(f.base / "stderr");
    check(pretty.find("Online PGO") != std::string::npos &&
              pretty.find("2 samples (2 CPU | 0 GPU)") != std::string::npos &&
              pretty.find("Model error") != std::string::npos &&
              pretty.find("Sample latency") != std::string::npos,
          "pretty summary omitted online profiling evidence");
    check(f.run(0, {"scan", "-r", "--summary", "--rehash", "--no-pgo"}) == expected,
          "no-pgo changed duplicate groups");
    check(f.stats["pgo_enabled"] == 0 && f.stats["pgo_samples"] == 0 &&
              f.stats["pgo_predicted_samples"] == 0 && f.stats["calibration_ms"] == 0,
          "no-pgo left performance analysis enabled");
    f.expect(expected);
    check(f.stats["cached"] == 2 && f.stats["pgo_samples"] == 0,
          "cache-only scan produced model samples");
    f.config({{"pgo", "false"}, {"rehash", "true"}});
    f.expect(expected);
    check(f.stats["pgo_enabled"] == 0 && f.stats["pgo_samples"] == 0,
          "configuration did not disable analyzer");
}

/// 跨进程锁必须拒绝第二个扫描器，Windows 状态目录大小写不敏感。 / Reject a competing scanner;
/// Windows state-directory exclusion is case insensitive.
void locking_and_state(const fs::path& exe) {
    Fixture f(exe);
    {
        same::RunLock lock(f.root / ".same/run.lock");
        f.run(2);
    }
    f.run();
#ifdef _WIN32
    Fixture upper(exe);
    fs::rename(upper.root / ".same", upper.root / ".state-renaming");
    fs::rename(upper.root / ".state-renaming", upper.root / ".SAME");
    upper.file("a", "state exclusion");
    upper.file("b", "state exclusion");
    upper.file(".SAME/hidden", "state exclusion");
    upper.expect({group({"a", "b"})});
    check(upper.stats["scanned"] == 2 && upper.stats["cached"] == 0, "uppercase state excluded");
    upper.run();
    check(upper.stats["cached"] == 2, "uppercase state cache");
#endif
}
} // namespace

/// 独立运行所有场景并标识失败阶段。 / Run all isolated scenarios and identify failures by stage.
int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: integration_tests <same executable>\n";
        return 1;
    }
    fs::path exe;
    try {
        exe = executable_path(argv[1]);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    const std::pair<const char*, void (*)(const fs::path&)> cases[] = {
        {"boundaries", boundaries},
        {"cache changes", cache_changes},
        {"ignores", ignores},
        {"paths", paths},
        {"symlinks", symlinks},
        {"invalid config", invalid_config},
        {"collisions and queue", collisions_and_queue},
        {"backends", backends},
        {"size routing", size_routing},
        {"presentation", presentation},
        {"scan commands", scan_commands},
        {"pgo control", pgo_control},
        {"command validation", command_validation},
        {"lifecycle commands", lifecycle_commands},
        {"locking and state", locking_and_state}};
    for (const auto& [name, test] : cases) {
        try {
            test(exe);
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
            return 1;
        }
    }
    return 0;
}
