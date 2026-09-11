#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>

#include <string>
#include <string_view>
#include <vector>
namespace same {
/// Validated per-run limits; byte budgets cover workers, not the entire process.
/// 每轮运行的资源限制；字节预算约束工作线程，并非整个进程的硬上限。
struct Config {
    /// Worker count, constrained to [1, 256]. 工作线程数，限定为 [1, 256]。
    std::size_t workers;
    /// 并行目录/元数据工作线程，独立于内容计算，范围 [1, 256]。
    /// Directory/metadata workers independent of content compute, constrained to [1, 256].
    std::size_t metadata_workers;
    /// 小于阈值的文件使用 CPU SIMD；0 禁用大小路由以便对照实验。
    /// Files below this threshold use CPU SIMD; zero disables size routing for ablations.
    std::size_t gpu_min_bytes{16 * 1024 * 1024};
    /// I/O block size, a multiple of one BLAKE3 chunk (1024 bytes).
    /// I/O 块大小，必须是 BLAKE3 分块大小（1024 字节）的整数倍。
    std::size_t block_bytes{1024 * 1024};
    /// Aggregate host buffer/staging allowance. 主机缓冲区和暂存区总预算。
    std::size_t memory_bytes{64 * 1024 * 1024};
    /// Aggregate device allocation allowance, shared across workers. 工作线程共享的显存总预算。
    std::size_t device_memory_bytes{64 * 1024 * 1024};
    /// Bound both queued work and retained futures. 同时限制排队任务与保留的 future 数量。
    std::size_t queue_capacity;
    /// auto lets each worker choose CPU/CUDA/iGPU using its private online model.
    /// auto 使用线程私有三设备模型；cuda/igpu 显式卸载，cpu 不探测。
    std::string backend{"auto"};
    /// Ignore stored hashes even when file stamps match. 即使文件戳匹配也重新计算哈希。
    bool rehash{false};
    /// Enable runtime profile-guided routing; unrelated to compiler PGO or summary output.
    /// 启用运行时剖析引导路由；与编译器 PGO 和汇总输出开关无关。
    bool pgo{true};
    /// 未知核显冷启动估计（毫秒）；零允许显式实验，不是测量值。
    /// Unknown iGPU cold-start estimate in ms; zero enables experiments, not a measurement.
    double igpu_bootstrap_ms{100.0};
    /// 自动 CUDA 冷启动潜在成本估计（毫秒）。 / Auto CUDA cold-start potential-cost estimate in ms.
    double cuda_bootstrap_ms{100.0};
    /// 已完成合格 CPU 工作/线程数中可用于冷探索的比例；不是墙钟保证。
    /// Fraction of completed eligible CPU work/workers for cold exploration, not a wall-time bound.
    double cold_exploration_fraction{0.05};
    /// Persist local diagnostics independently of runtime PGO and summary output.
    /// 本地持久化诊断信息，与运行时 PGO 及汇总输出独立；可能包含敏感元数据。
    bool telemetry{true};
    /// Bounded asynchronous event queue, [1, 65536]; overflow drops telemetry, not work.
    /// 异步事件队列上限 [1, 65536]；溢出丢弃遥测，而非阻塞工作任务。
    std::size_t telemetry_queue_capacity{4096};
    /// Retained run count, [1, 4096]. 跨运行保留数量，范围 [1, 4096]。
    std::size_t telemetry_retention_runs{64};
    /// Per-run event limit, [1, 1000000]. 每轮事件记录上限，范围 [1, 1000000]。
    std::size_t telemetry_max_events{16384};
    /// Choose bounded defaults from hardware concurrency. 根据硬件并发数选择有界默认值。
    Config();
    /// Load optional .same/config.toml; ignore unknown keys, reject invalid supported values.
    /// 加载可选配置文件；忽略未知键；仍拒绝已支持字段的错误类型及不合法限制。
    static Config load(const std::filesystem::path& root);
    /// Throw on inconsistent budgets before allocating worker buffers.
    /// 在分配工作缓冲区前拒绝不一致的资源预算。
    void validate() const;
};
/// Ordered, bounded glob rules; later matches override earlier ones.
/// 有序、有界的 glob 规则；后匹配的规则覆盖先前结果。
class Ignore {
public:
    /// Read optional .same/ignore, at most 1 MiB and 4096 rules.
    /// 读取可选忽略文件，最多 1 MiB、4096 条规则。
    explicit Ignore(const std::filesystem::path& root);
    /// Match a root-relative forward-slash path; .same is always excluded.
    /// 匹配使用正斜杠的根目录相对路径；始终排除 .same。
    bool matches(std::string_view relative, bool directory) const;
    /// Excluded parents are pruned, as in Git; child negations cannot reopen them.
    /// 与 Git 一致地剪枝被排除的父目录；子项否定规则不能重新纳入它们。
    bool can_prune(std::string_view relative) const;

private:
    /// Parsed rule with anchoring normalized into its pattern. 锚定已归一化到模式中的规则。
    struct Rule {
        /// Normalized UTF-8 byte pattern. 归一化后的 UTF-8 字节模式。
        std::string pattern;
        /// Reinclude matching paths. 重新纳入匹配路径。
        bool negate;
        /// Restrict terminal matches to directories. 末端匹配仅作用于目录。
        bool directory;
    };
    /// File order is semantically significant. 文件中的规则顺序具有语义。
    std::vector<Rule> rules_;
};
} // namespace same
