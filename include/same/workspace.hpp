#pragma once
#include <cstddef>
#include <filesystem>
namespace same {
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
} // namespace same
