#pragma once
#include "same/detail/online_model.hpp"
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace same {
/// 设备初始化历史独立于稳定服务时间。 / Device setup history is separate from steady service time.
struct SetupHistory {
    /// 最近、平均、最大毫秒；必须有限且为正。 / Last, mean, maximum milliseconds; finite and
    /// positive.
    double last_ms{}, mean_ms{}, max_ms{};
    /// 有效初始化观测次数。 / Number of valid setup observations.
    std::uint64_t samples{};
};
/// 独立的冷路径学习状态库；调用方持有工作区运行锁。
/// Independent cold-path learning store; caller holds the workspace run lock.
/// WAL/NORMAL 可在断电时丢失最近学习，但不涉及内容缓存或正确性。
/// WAL/NORMAL may lose recent learning on power failure; content cache/correctness is unaffected.
/// Example: ModelStore db(path); auto state = db.load(key); db.save(key, model.delta());
class ModelStore {
public:
    /// 打开有界数据库；存储失败只形成诊断，不影响内容正确性。
    /// Open a bounded database; storage failures become diagnostics, not content failures.
    explicit ModelStore(const std::filesystem::path& path);
    /// 关闭连接。 / Close the connection.
    ~ModelStore();
    ModelStore(const ModelStore&) = delete;
    ModelStore& operator=(const ModelStore&) = delete;
    /// 精确键匹配启动快照，缺失返回空；允许多个工作线程并发读取。
    /// Exact key lookup; absent/corrupt entries return empty. Immutable startup snapshot;
    /// concurrent reads are safe.
    std::optional<detail::OnlineModel::State> load(std::string_view key) const;
    /// 原子替换，失败保留旧状态；仅在所有工作线程停止后使用。
    /// Atomic replacement preserves old state on failure. Use after workers stop.
    bool save(std::string_view key, const detail::OnlineModel::State& state);
    /// 不可变初始化历史快照，允许并发读取。 / Immutable setup snapshot supports concurrent reads.
    std::optional<SetupHistory> load_setup(std::string_view key) const;
    /// 空闲阶段原子写入初始化历史。 / Atomically save setup history while idle.
    bool save_setup(std::string_view key, const SetupHistory& history);
    /// 构造或保存操作的结果；成功为空，load 不改变诊断。
    /// Constructor/save status; empty on success, never mutated by load.
    std::string_view diagnostic() const noexcept;

private:
    /// 独占连接及诊断，绝不从工作线程访问。 / Sole connection and diagnostic, never
    /// worker-accessed.
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace same
