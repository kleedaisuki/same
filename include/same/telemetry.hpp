#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace same::telemetry {
/// 有界文本，过长时在 UTF-8 边界截断；生产者不分配内存。 / Bounded UTF-8 text; no producer
/// allocation.
template <std::size_t N> struct Text {
    /// N 包括结尾零。 / N includes the terminator.
    std::array<char, N> data{};
    /// 标识原文被截断。 / Whether source text was truncated.
    bool truncated = false;
    Text() = default;
    /// 复制拥有内容，不保存借用引用。 / Copy owned contents, never retain borrowed references.
    Text(std::string_view text) noexcept {
        *this = text;
    }
    /// 截断不抛异常。 / Truncating assignment never throws.
    Text& operator=(std::string_view text) noexcept {
        auto n = std::min(text.size(), N - 1);
        truncated = text.size() > n;
        if (truncated)
            while (n && (static_cast<unsigned char>(text[n]) & 0xc0) == 0x80)
                --n;
        if (n)
            std::memcpy(data.data(), text.data(), n);
        data[n] = '\0';
        return *this;
    }
    /// 借用零结尾内容。 / Borrow zero-terminated contents.
    const char* c_str() const noexcept {
        return data.data();
    }
    /// 返回内容视图。 / Return a contents view.
    operator std::string_view() const noexcept {
        return data.data();
    }
};
/// 固定尺寸的日志、跨度或指标；时间为运行内单调纳秒。 / Fixed log/span/metric record; monotonic
/// run-relative ns.
struct Event {
    /// 事件分类与检索名。 / Event kind and searchable name.
    Text<24> type;
    Text<96> name;
    /// 严重性、后端及度量单位。 / Severity, backend and measurement unit.
    Text<16> severity, backend;
    Text<24> unit;
    /// 有界诊断信息；不要放入敏感文件内容。 / Bounded diagnostic; never put sensitive file contents
    /// here.
    Text<384> message;
    /// 显式跨度关联、时间和负载。 / Explicit span correlation, time and payload.
    std::uint64_t span_id = 0, parent_span_id = 0, time_ns = 0, duration_ns = 0, bytes = 0;
    /// -1 表示协调线程或未关联线程。 / -1 means coordinator or unspecified worker.
    std::int64_t worker = -1;
    /// 数值型指标。 / Numeric metric value.
    double value = 0;
};
/// 运行身份元数据，构造时移交所有权。 / Run identity metadata, ownership transferred at
/// construction.
struct RunInfo {
    /// 产品版本、命令、扫描根及配置快照。 / Version, command, root and configuration snapshot.
    std::string version, command, root, config_json;
    /// UTC 启动纳秒；零表示由构造器采集。 / UTC start ns; zero captures at construction.
    std::uint64_t started_unix_ns = 0;
};
/// 结束时的数值指标。 / Final numeric metric.
struct Metric {
    /// 可检索名字、数值和单位。 / Searchable name, value and unit.
    std::string name;
    double value = 0;
    std::string unit;
};
/// 模型及配置的字符串快照。 / String snapshot of model and configuration parameters.
struct Parameter {
    /// 分类、键和完整值；只在结束路径构造。 / Category, key and full value; constructed only at
    /// finish.
    std::string category, name, value;
};
/// 独立结束槽，不受事件队列满影响。 / Dedicated final slot unaffected by event queue pressure.
struct FinalRecord {
    /// completed 或 failed；析构未显式结束记 interrupted。 / completed or failed; implicit
    /// destruction is interrupted.
    std::string status = "completed", error;
    /// 完整终态快照，不采样。 / Complete final snapshot, not sampled.
    std::vector<Metric> metrics;
    /// 保证结束阶段跨度不受队列压力影响。 / Terminal spans bypass queue pressure.
    std::vector<Event> events;
    std::vector<Parameter> parameters;
};
/// 有界资源和保留策略。 / Bounded resources and retention policy.
struct Options {
    /// 禁用时不分配队列、不启动线程也不访问数据库。 / Disabled means no queue, thread or database
    /// access.
    bool enabled = true;
    /// 队列、批次、每轮事件上限及保留运行数；批次限制到队列大小，250 ms 超时刷新。
    /// Queue, batch, per-run cap and retained runs; batch clamps to queue, with a 250 ms flush
    /// timeout.
    std::size_t queue_capacity = 2048, batch_size = 64, event_cap = 16384, retain_runs = 64;
    /// SQLite 锁等待仅发生于后台线程。 / SQLite lock waiting occurs only on the background thread.
    int busy_timeout_ms = 50;
};
/// 可观测损失及写入状态，不假装遥测绝不丢失。 / Visible loss and writer status; telemetry is not
/// lossless.
struct Stats {
    /// 接收、持久化、丢失、错误及截断计数。 / Accepted, persisted, dropped, error and truncation
    /// counts.
    std::uint64_t accepted = 0, persisted = 0, dropped = 0, errors = 0, truncated = 0;
    /// 峰值排队数量及最终等待耗时。 / Queue high-water and final drain wait.
    std::uint64_t queue_high_water = 0;
    double drain_ms = 0;
    /// starting/running/completed/failed/interrupted/disabled。 / Writer lifecycle state.
    Text<24> status;
    /// 最后一个后台错误。 / Last background error.
    Text<384> error;
};
/** 独立 SQLite 单写者；调用方持有工作区锁直到 finish/析构结束。
 * Independent SQLite single writer; caller holds workspace lock through finish/destruction.
 * emit 不等待、不分配、不执行 SQL；finish 明确允许等待。 / emit never waits, allocates or runs SQL;
 * finish explicitly permits waiting. Example: Telemetry t(path, info); t.emit(event); t.finish({});
 */
class Telemetry {
public:
    /// 不在调用线程打开数据库；初始化失败仅禁用遥测。 / No caller-thread DB open; failure only
    /// disables telemetry.
    Telemetry(const std::filesystem::path& path, RunInfo info, Options options = {}) noexcept;
    /// 未结束的运行按 interrupted 收尾并 join。 / Finish unfinished run as interrupted and join.
    ~Telemetry();
    Telemetry(const Telemetry&) = delete;
    Telemetry& operator=(const Telemetry&) = delete;
    /// 尝试入队；争用、容量及事件上限导致可计数丢弃。 / Try enqueue; contention/capacity/cap cause
    /// counted loss.
    bool emit(const Event& event) noexcept;
    /// 传递终态并等待后台关闭数据库；可重复调用。 / Transfer final state and wait for DB closure;
    /// idempotent.
    Stats finish(FinalRecord final = {}) noexcept;
    /// 无等待快照，活跃阶段非事务一致。 / Nonblocking snapshot, not transactionally consistent
    /// while active.
    Stats snapshot() const noexcept;
    /// 唯一关联 ID；禁用时可能为空。 / Unique correlation ID; may be empty when disabled.
    const std::string& run_id() const noexcept;

private:
    /// 实现拥有线程和预分配队列。 / Implementation owns thread and preallocated queue.
    struct Impl;
    std::unique_ptr<Impl> impl_;
    /// 区分用户禁用与启动失败。 / Distinguish requested disablement from startup failure.
    bool startup_failed_ = false;
};
} // namespace same::telemetry
