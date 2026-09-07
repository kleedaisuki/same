#include "same/detail/windows_metadata.hpp"
#include "same/files.hpp"
#include <array>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <utility>
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
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    } cleanup{root};
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
