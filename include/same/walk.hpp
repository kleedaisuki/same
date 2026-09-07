#pragma once
#include "same/files.hpp"
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
namespace same {
/// 无序元数据结果；路径始终为根相对 UTF-8。 / Unordered metadata with root-relative UTF-8 path.
struct WalkEntry {
    /// 可直接作为缓存键。 / Suitable as a cache key.
    std::string path;
    /// 从文件句柄取得的保守版本。 / Conservative handle-derived version.
    FileStamp stamp;
    /// 元数据阶段打开的未读取句柄；消费者可移交给哈希，缓存命中时销毁。
    /// Unread handle opened by metadata work; transfer to hashing or destroy on a cache hit.
    std::unique_ptr<FileReader> reader;
};
/// 工作线程耗时相加，不是关键路径时间。 / Summed worker timings, not critical-path times.
struct WalkStats {
    /// 目录枚举、分类与过滤总毫秒。 / Directory enumeration, classification and filtering
    /// milliseconds.
    double enumerate_ms{};
    /// 文件打开和元数据读取总毫秒。 / File open and metadata milliseconds.
    double metadata_ms{};
    /// 待执行任务和待消费结果各自的峰值。 / Peaks of pending tasks and results respectively.
    std::size_t task_peak{}, result_peak{};
};
/** 有界动态树遍历，目录与文件元数据可并行；不跟随链接。
 * Bounded dynamic tree walk with parallel directory and file metadata work; no links followed.
 * 单消费者调用 next，析构取消并等待；错误不静默跳过。OS 阻塞调用不可强行中断。
 * Single consumer calls next; destruction cancels and joins; errors are never skipped.
 * In-flight blocking OS calls cannot be forcibly interrupted.
 * 用法 / Usage: ParallelWalk walk(root, 4, 64); while (auto e = walk.next()) consume(*e);
 */
class ParallelWalk {
public:
    /// workers 和 capacity 必须非零；recursive=false 仅枚举根目录文件。
    /// workers and capacity must be nonzero; recursive=false enumerates root files only.
    ParallelWalk(const std::filesystem::path& root, std::size_t workers, std::size_t capacity,
                 bool recursive = true);
    /// 唤醒阻塞生产者并等待所有线程退出。 / Wake blocked producers and join every thread.
    ~ParallelWalk();
    ParallelWalk(const ParallelWalk&) = delete;
    ParallelWalk& operator=(const ParallelWalk&) = delete;
    /// 等待一个结果；空表示结束；工作线程错误在此重新抛出。 / Wait for a result; null means EOF;
    /// rethrow worker failures.
    std::optional<WalkEntry> next();
    /// 返回同步统计快照；结束后为完整值。 / Synchronized snapshot; complete after EOF.
    WalkStats stats() const;

private:
    /// 隐藏同步状态，确保线程早于状态销毁。 / Hide synchronization; threads die before state.
    struct Impl;
    /// 独占运行状态。 / Exclusively owned run state.
    std::unique_ptr<Impl> impl_;
};
} // namespace same
