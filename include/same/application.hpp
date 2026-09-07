#pragma once
#include "same/config.hpp"
#include <filesystem>
#include <iosfwd>
namespace same {
/// Explicit presentation policy; library callers retain legacy TSV by default.
/// 显式展示策略；库调用方默认保留旧 TSV 格式。
struct OutputOptions {
    /// Group headings instead of TSV. 使用分组标题，而非 TSV。
    bool pretty{false};
    /// Emit ANSI SGR only when requested by the caller. 仅在调用方请求时输出 ANSI SGR。
    bool color{false};
    /// Opt-in unmatched paths; TSV uses reserved group 0. 显式展开无副本路径；TSV 使用保留组号 0。
    bool unique_files{false};
    /// Independent stderr color policy. 独立的标准错误颜色策略。
    bool diagnostics_color{false};
    /// Human-readable units on stderr; false preserves raw key=value metrics.
    /// 标准错误使用可读单位；false 保留原始 key=value 统计。
    bool diagnostics_pretty{false};
    /// Descend into subdirectories; CLI explicitly opts out unless -r is supplied.
    /// 下降进入子目录；CLI 仅在提供 -r 时显式开启。
    bool recursive{true};
    /// Emit summary, database and profiling panels; warnings remain independent.
    /// 输出汇总、数据库与性能面板；警告不受此选项影响。
    bool summary{true};
};
/// Run with explicit presentation; e.g. run(root, config, out, err, {true, false}).
/// 使用显式展示策略运行；示例为无色可读输出。
int run(const std::filesystem::path& root, const Config& config, std::ostream& output,
        std::ostream& diagnostics, OutputOptions options);
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
