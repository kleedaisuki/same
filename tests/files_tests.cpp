/** @file
 * @brief 文件身份、移动所有权、读取边界与符号链接防护。 / File identity, moved ownership, read
 * boundaries and symlink protection.
 */
#include "same/detail/windows_metadata.hpp"
#include "same/detail/windows_path.hpp"
#include "same/files.hpp"
#include "same/run_lock.hpp"
#include <array>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <utility>
/// 运行本文件全部回归场景，断言失败即返回非零。 / Run all regressions; assertion failures produce a
/// nonzero exit.
int main() {
#ifdef _WIN32
    for (DWORD error : {ERROR_INVALID_PARAMETER, ERROR_INVALID_LEVEL, ERROR_NOT_SUPPORTED,
                        ERROR_INVALID_FUNCTION}) {
        if (!same::detail::unsupported_metadata_class(error))
            throw std::runtime_error("missing metadata fallback");
    }
    for (DWORD error :
         {ERROR_ACCESS_DENIED, ERROR_INVALID_HANDLE, ERROR_READ_FAULT, ERROR_SUCCESS}) {
        if (same::detail::unsupported_metadata_class(error))
            throw std::runtime_error("I/O error masked");
    }
    BY_HANDLE_FILE_INFORMATION legacy{};
    legacy.dwVolumeSerialNumber = 42;
    legacy.nFileIndexHigh = 1;
    legacy.nFileIndexLow = 7;
    if (same::detail::legacy_file_identity(legacy) != "win32-id64:42:4294967303")
        throw std::runtime_error("legacy ID truncation");
#endif
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
                      ("same-files-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    /** 异常退出也清理测试目录。 / Clean the fixture even during unwinding. */
    struct Cleanup {
        /// 唯一临时目录，由本测试独占。 / Unique directory exclusively owned by this test.
        fs::path path;
        /// 不抛异常，以免掩盖断言。 / Do not mask assertion failures with cleanup errors.
        ~Cleanup() {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    } cleanup{root};
#ifdef _WIN32
    // 显式扩展路径创建夹具，避免依赖注册表或测试进程清单。
    // Create fixtures with explicit extended paths, independent of registry/manifest opt-in.
    const auto deep = root / std::wstring(100, L'a') / std::wstring(100, L'b') / L"论文资料";
    const fs::path extended(L"\\\\?\\" + deep.native());
    fs::create_directories(extended);
    {
        std::ofstream file(extended / L"论文.pdf", std::ios::binary);
        file << "abc";
        if (!file)
            throw std::runtime_error("long path fixture");
    }
    const auto long_file = deep / L"论文.pdf";
    if (long_file.native().size() < MAX_PATH || same::is_reparse_point(long_file))
        throw std::runtime_error("long path attributes");
    for (const auto& path :
         {long_file, deep / L".." / L"论文资料" / L"论文.pdf", extended / L"论文.pdf"}) {
        same::FileReader long_reader(path);
        std::array<std::byte, 4> content{};
        if (long_reader.stamp().size != 3 || long_reader.read(content) != 3 ||
            content[0] != std::byte{'a'} || long_reader.read(content) != 0)
            throw std::runtime_error("long path read");
    }
    {
        same::RunLock lock(deep / L"run.lock");
    }
    // 构造盘符相对路径，覆盖不同工作盘符而不修改进程工作目录。
    // Exercise drive-relative input without changing the process working directory.
    const auto relative =
        long_file.root_name() / long_file.lexically_relative(fs::absolute(long_file.root_name()));
    if (same::stamp_path(relative) != same::stamp_path(long_file))
        throw std::runtime_error("drive-relative long path");
    const std::wstring unc = L"\\\\server\\share\\" + std::wstring(250, L'x');
    if (same::detail::windows_path(unc) != L"\\\\?\\UNC\\" + unc.substr(2) ||
        same::detail::windows_path(L"file") != L"file" ||
        same::detail::windows_path(L"\\\\.\\NUL") != L"\\\\.\\NUL")
        throw std::runtime_error("Windows path conversion");
#endif
    {
        std::ofstream file(root / "file", std::ios::binary);
        file << "abc";
    }
    same::FileReader reader(root / "file");
    const auto stamp = reader.stamp();
    if (stamp.size != 3 || stamp.identity.empty() || stamp != same::stamp_path(root / "file"))
        throw std::runtime_error("stamp");
    std::array<std::byte, 8> bytes{};
    if (reader.read(bytes) != 3 || bytes[0] != std::byte{'a'} || reader.read(bytes) != 0)
        throw std::runtime_error("read");
    auto moved = std::move(reader);
    if (moved.stamp() != stamp)
        throw std::runtime_error("move");
    bool rejected = false;
    try {
        same::FileReader directory(root);
    } catch (...) {
        rejected = true;
    }
    if (!rejected)
        throw std::runtime_error("directory accepted");
    std::error_code ec;
    fs::create_symlink(root / "file", root / "link", ec);
    if (!ec) {
        rejected = false;
        try {
            same::FileReader symlink(root / "link");
        } catch (...) {
            rejected = true;
        }
        if (!rejected)
            throw std::runtime_error("symlink accepted");
    }
    {
        std::ofstream file(root / "file", std::ios::binary | std::ios::app);
        file << 'd';
    }
    if (moved.stamp() == stamp || moved.stamp().size != 4)
        throw std::runtime_error("mutation not detected");
}
