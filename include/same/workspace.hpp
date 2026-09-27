#pragma once
#include "same/compute.hpp"
#include "same/files.hpp"
#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>
namespace same {
/// 删除前逐文件验证版本与内容，不依赖文件系统时间戳分辨率。
/// Verify both version and content before deletion, independent of timestamp resolution.
struct WorkspaceStateFile {
    std::string name;
    FileStamp stamp;
    Digest digest;
    bool operator==(const WorkspaceStateFile&) const = default;
};
/// 不含独占打开的 run.lock。 / Excludes the exclusively opened run.lock.
using WorkspaceStateSnapshot = std::vector<WorkspaceStateFile>;
/// 创建缺失的标准配置与忽略文件，永不覆盖已有文件。
/// Create missing standard configuration and ignore files without overwriting existing files.
void initialize_workspace(const std::filesystem::path& root);
/// 删除当前或所有后代工作区状态，拒绝活动扫描和链接状态目录。
/// Remove current or descendant workspace state, rejecting active scans and linked state dirs.
/// 原生目录能力防止链接替换将清理重定向到外部；并发变动可导致安全失败。
/// Native directory capabilities prevent link-redirection; concurrent changes may fail safely.
/// 不支持与不遵守生命周期锁协议的旧版本并发。
/// Concurrent older versions lacking the lifecycle lock protocol are unsupported.
std::size_t clean_workspace(const std::filesystem::path& root, bool recursive);
/// 在调用方持有源运行锁时采集完整直系子项快照。 / Snapshot direct state children under lock.
WorkspaceStateSnapshot snapshot_workspace_state(const std::filesystem::path& root);
/// 仅在状态与合并后的快照完全相同时删除，防止移动期间丢掉新写入。
/// Remove state only when unchanged since merge, preventing deletion of new source work.
void clean_workspace_if_unchanged(const std::filesystem::path& root,
                                  const WorkspaceStateSnapshot& expected);
} // namespace same
