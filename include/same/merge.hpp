#pragma once
#include <filesystem>
#include <iosfwd>

namespace same {
/// 合并只修改当前工作区，来源在完整成功前不删除。
/// Merge modifies the destination; source cleanup is deferred until full success.
struct MergeOptions {
    /// 递归发现指定目录下的所有真实 .same 目录。 / Recursively discover real workspaces.
    bool recursive{};
    /// 成功后移除已导入的来源工作区。 / Remove imported source state after success.
    bool move{};
    /// 输出本次合并的完整计数与遥测健康状况。 / Report this merge's counts and telemetry.
    bool summary{};
    /// 用分区、可读单位展示报告。 / Use sectioned, human-readable reporting.
    bool pretty{};
    /// 仅显式或终端支持时写 ANSI。 / Emit ANSI only under the selected terminal policy.
    bool color{};
};
/** 将 source 下的工作区状态导入 destination；配置永远保留目的端。
 * Import source workspace state into destination, always preserving destination config.
 * @code
 * same::merge_workspaces(current, source, {.recursive=true, .summary=true}, std::cerr);
 * @endcode
 */
int merge_workspaces(const std::filesystem::path& destination,
                     const std::filesystem::path& source, MergeOptions options,
                     std::ostream& diagnostics);
} // namespace same
