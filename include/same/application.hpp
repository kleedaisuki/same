#pragma once
#include "same/config.hpp"
#include <filesystem>
#include <iosfwd>
namespace same {
// Success means a completed scan, not the absence of duplicates.
// 成功代表扫描完整完成，而非没有重复文件。
int run(const std::filesystem::path& root, const Config& config, std::ostream& output,
        std::ostream& diagnostics);
} // namespace same
