#pragma once
#include "same/compute.hpp"
#include "same/files.hpp"
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace same {
/// 持久化的单文件缓存项；摘要仅筛选候选，不能代替逐字节验证。
/// Persisted per-file cache entry; a digest filters candidates, not bytewise verification.
struct FileRecord {
    /// 根目录相对路径，UTF-8 编码。 / Root-relative UTF-8 path.
    std::string path;
    /// 计算摘要所对应的文件版本。 / File version associated with the digest.
    FileStamp stamp;
    /// 内容摘要，允许碰撞。 / Content digest; collisions remain possible.
    Digest digest;
};
/** 单线程独占 SQLite 仓储；调用方负责持有整个运行周期的 RunLock。
 * Single-owner SQLite repository; the caller holds RunLock for the whole run.
 * 回调逐行执行，引用仅在本次回调内有效；不得在回调中重入同一遍历或修改其源表。
 * Callbacks stream rows whose references expire on return; do not reenter a traversal or mutate its
 * source table. 典型流程：begin_scan → cached/save（每个已见文件）→ end_scan → 候选验证。 Typical
 * use: begin_scan → cached/save (every observed file) → end_scan → candidate verification.
 */
class Store {
public:
    /// 打开/初始化 schema；缓存预算按 KiB 换算并限制为 16 KiB 至 1 GiB。
    /// Open/initialize the schema; convert the cache budget to KiB and clamp to 16 KiB–1 GiB.
    explicit Store(const std::filesystem::path& path, std::size_t cache_bytes = 2 * 1024 * 1024);
    /// 回滚尚未完成的扫描，然后关闭连接。 / Roll back an unfinished scan, then close the
    /// connection.
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    /// 开启 IMMEDIATE 事务并推进代次；禁止嵌套扫描。
    /// Begin an IMMEDIATE transaction and advance the generation; nested scans are rejected.
    void begin_scan();
    /// 返回路径对应的缓存副本；不验证 stamp，也不将该文件标为本轮已见。
    /// Return an owned cache entry; neither validate its stamp nor mark it seen in this scan.
    std::optional<FileRecord> cached(std::string_view path);
    /// 在活动扫描中插入/更新记录并标记本轮已见；缓存命中可改用 mark_seen。
    /// Upsert and mark seen in the active scan; cache hits may use mark_seen instead.
    void save(const FileRecord& record);
    /// 已验证缓存命中后仅更新扫描代次；路径必须存在且扫描必须活动。
    /// After validating a cache hit, update only its generation; requires an existing path
    /// and active scan. Example: if (cached(path)->stamp == stamp) mark_seen(path).
    void mark_seen(std::string_view path);
    /// 完整文件戳匹配时标记已见，单条语句完成缓存验证和更新；必须有活动扫描。
    /// Validate the complete stamp and mark seen in one statement; requires an active scan.
    /// 返回 false 表示缺失或版本变化，不修改记录。False means missing/changed, without mutation.
    /// Example: if (store.mark_if_unchanged(path, stamp)) skip_hash();
    bool mark_if_unchanged(std::string_view path, const FileStamp& stamp);
    /// 删除本轮未见记录，与所有更新一并提交；必须有活动扫描。
    /// Delete unseen entries and commit them atomically with updates; requires an active scan.
    void end_scan();
    /// 尽力回滚活动扫描；无扫描时无操作，清理期间不抛异常。
    /// Best-effort rollback of an active scan; no-op when inactive and never throws during cleanup.
    void rollback_scan() noexcept;
    /// 扫描提交后访问至少两个文件的大小/摘要桶，按大小、摘要、路径排序。
    /// After scan commit, visit size/digest buckets of at least two files, ordered by
    /// size/digest/path.
    void visit_candidates(const std::function<void(const FileRecord&)>& visitor);
    /// 清空当前候选桶的临时代表表；调用方负责桶边界。
    /// Clear temporary representatives for the current candidate bucket; caller manages bucket
    /// boundaries.
    void clear_representatives();
    /// 加入已验证等价类的代表；重复路径会报错。
    /// Add a representative of a verified equivalence class; duplicate paths fail.
    void add_representative(const FileRecord& record);
    /// 按路径遍历代表，false 提前停止；回调中禁止修改代表表。
    /// Visit representatives in path order; false stops early. Do not mutate that table in
    /// callbacks.
    void visit_representatives(const std::function<bool(const FileRecord&)>& visitor);
    /// 清空连接本地的精确匹配结果，不修改持久缓存。
    /// Clear connection-local exact matches without changing the persistent cache.
    void reset_matches();
    /// 幂等加入已验证成员；代表自身也需作为成员显式加入，仓储不执行内容验证。
    /// Idempotently add a verified member; explicitly add the representative itself too. Store does
    /// not verify content.
    void add_match(std::string_view representative, std::string_view member);
    /// 按代表/成员路径输出至少两个成员的组；UTF-8 视图仅在回调内有效。
    /// Emit groups with at least two members in representative/member order; UTF-8 views live only
    /// within the callback.
    void visit_matches(const std::function<void(std::string_view, std::string_view)>& visitor);

    /// Stream paths outside duplicate classes, ordered by path; bounded memory.
    /// 按路径流式遍历不属于重复组的文件，内存有界。
    void visit_unique(const std::function<void(std::string_view)>& visitor);

private:
    /// 隐藏 SQLite 连接与事务状态。 / Hide SQLite connection and transaction state.
    struct Impl;
    /// 独占连接，禁止跨线程并发使用。 / Own the connection exclusively; no concurrent cross-thread
    /// use.
    std::unique_ptr<Impl> impl_;
};
} // namespace same
