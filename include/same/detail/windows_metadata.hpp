#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cstdint>
#include <string>
#include <windows.h>
namespace same::detail {
inline bool unsupported_metadata_class(DWORD error) noexcept {
    return error == ERROR_INVALID_PARAMETER || error == ERROR_INVALID_LEVEL ||
           error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_FUNCTION;
}
inline std::string legacy_file_identity(const BY_HANDLE_FILE_INFORMATION& info) {
    const auto index = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    // Separate ID namespaces avoid confusing 64-bit fallback with 128-bit IDs.
    // 隔离标识命名空间，避免将回退的 64 位标识与 128 位标识混淆。
    return "win32-id64:" + std::to_string(info.dwVolumeSerialNumber) + ":" + std::to_string(index);
}
} // namespace same::detail
#endif
