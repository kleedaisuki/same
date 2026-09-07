#pragma once
#include "same/config.hpp"
#include <filesystem>
#include <iosfwd>
namespace same {
/** Run a locked scan, verify candidate bytes, then emit deterministic groups.
 * 对根目录加锁扫描，逐字节验证候选后输出确定顺序的重复组。
 * @param root Existing scan directory; scan input files are never modified.
 * 已存在的扫描目录；不会修改待扫描文件。
 * @param config Validated resource limits and backend policy. 已验证的资源限制与后端策略。
 * @param output Group-number/tab/quoted-path records. 组号、制表符、转义路径记录。
 * @param diagnostics Counters and fallback notices. 统计信息与回退提示。
 * @return Zero means a completed scan, not absence of duplicates. 零表示完成而非无重复。
 * @throws std::exception On I/O, state or validation failure. I/O、状态或校验失败时抛出。
 * @code
 * auto root = std::filesystem::current_path();
 * same::run(root, same::Config::load(root), std::cout, std::cerr);
 * @endcode
 */
int run(const std::filesystem::path& root, const Config& config, std::ostream& output,
        std::ostream& diagnostics);
} // namespace same
