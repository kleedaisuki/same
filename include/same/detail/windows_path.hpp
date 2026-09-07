#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <filesystem>
#include <string>
#include <system_error>
#include <windows.h>

namespace same::detail {
/** 仅在原生 I/O 边界扩展长路径，不改变缓存键或展示路径。
 * Extend long paths only at native I/O boundaries, not in cache keys or output.
 * 先按 Win32 规则解析相对路径、斜杠与点段；不解析符号链接。
 * Resolve relative paths, slashes and dot segments using Win32 rules, without following symlinks.
 * 短路径及显式设备路径保留原语义；调用方不得并发修改进程工作目录。
 * Preserve short and explicit device paths; callers must not concurrently change the process CWD.
 */
inline std::wstring windows_path(const std::filesystem::path& path) {
    const auto& input = path.native();
    if (input.empty() || input.starts_with(L"\\\\?\\") || input.starts_with(L"\\\\.\\"))
        return input;
    std::wstring full(MAX_PATH, L'\0');
    for (;;) {
        const DWORD size =
            GetFullPathNameW(input.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
        if (!size)
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "resolve Windows path");
        if (size < full.size()) {
            full.resize(size);
            break;
        }
        full.resize(size);
    }
    if (full.size() < MAX_PATH)
        return input;
    if (full.starts_with(L"\\\\"))
        return L"\\\\?\\UNC\\" + full.substr(2);
    return L"\\\\?\\" + full;
}
} // namespace same::detail
#endif
