#include "same/telemetry.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <random>
#include <sqlite3.h>
#include <stdexcept>
#include <thread>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace same::telemetry {
namespace {
/// 批次刷新时限的唯一来源。 / Single source for the batch flush deadline.
constexpr int flush_interval_ms = 250;
/// SQL 失败只退出遥测线程。 / SQL failure exits only the telemetry writer.
void sql(sqlite3* db, const char* query) {
    if (sqlite3_exec(db, query, nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db));
}
/// 拒绝链接和特殊文件；检查只发生在写者线程。 / Reject links and special files on the writer thread
/// only.
void guard_path(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory)
        return;
    if (error)
        throw std::runtime_error("cannot inspect telemetry path");
    if (status.type() == std::filesystem::file_type::not_found)
        return;
    if (!std::filesystem::is_regular_file(status))
        throw std::runtime_error("telemetry path is not a regular non-link file");
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("telemetry path is a reparse point or inaccessible");
#endif
    const auto links = std::filesystem::hard_link_count(path, error);
    if (error || links != 1)
        throw std::runtime_error("telemetry path has hard links or cannot be inspected");
}
/// 自动释放语句。 / Automatically release a prepared statement.
struct Statement {
    sqlite3_stmt* value = nullptr;
    /// 编译一次并重复绑定。 / Compile once and bind repeatedly.
    Statement(sqlite3* db, const char* query) {
        if (sqlite3_prepare_v2(db, query, -1, &value, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Statement() {
        sqlite3_finalize(value);
    }
    /// 所有绑定和重置错误都终止写者，不能把失败当作 SQL NULL。 / Any bind/reset failure aborts the
    /// writer, never silently binds SQL NULL.
    void require(int result) {
        if (result != SQLITE_OK)
            throw std::runtime_error(sqlite3_errstr(result));
    }
    /// 复制字符串绑定并立即检查分配错误。 / Copy text binding and immediately check allocation
    /// failure.
    void text(int i, const char* v) {
        require(sqlite3_bind_text(value, i, v, -1, SQLITE_TRANSIENT));
    }
    /// 无符号遥测计数映射到 SQLite 整数。 / Map unsigned telemetry counts to SQLite integers.
    void number(int i, std::uint64_t v) {
        integer(i, static_cast<sqlite3_int64>(v));
    }
    /// 保留线程编号的 -1 哨兵。 / Preserve the -1 worker sentinel.
    void integer(int i, sqlite3_int64 v) {
        require(sqlite3_bind_int64(value, i, v));
    }
    /// 检查浮点参数索引和 SQLite 错误。 / Check floating parameter indices and SQLite errors.
    void real(int i, double v) {
        require(sqlite3_bind_double(value, i, v));
    }
    /// 重置本次执行并清除绑定，错误不可隐藏。 / Reset execution and clear bindings without hiding
    /// errors.
    void reset() {
        require(sqlite3_reset(value));
        require(sqlite3_clear_bindings(value));
    }
    /// 完成一次写入并重置。 / Complete one write and reset.
    void write() {
        if (sqlite3_step(value) != SQLITE_DONE)
            throw std::runtime_error(sqlite3_errmsg(sqlite3_db_handle(value)));
        reset();
    }
};
/// UTC 时间仅在非热路径读取。 / UTC clock is read only off the hot path.
std::uint64_t utc_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}
/// 128 位随机运行 ID，跨运行关联不依赖路径。 / Random 128-bit run ID, independent of paths.
std::string unique_id() {
    std::random_device random;
    constexpr char hex[] = "0123456789abcdef";
    std::string id(32, '0');
    for (std::size_t i = 0; i < 32; i += 8) {
        const auto bits = random();
        for (std::size_t j = 0; j < 8; ++j)
            id[i + j] = hex[(bits >> (4 * j)) & 15];
    }
    return id;
}
/// 任何字段截断均可见。 / Truncation of any field remains visible.
bool truncated(const Event& e) {
    return e.type.truncated || e.name.truncated || e.severity.truncated || e.backend.truncated ||
           e.unit.truncated || e.message.truncated;
}
} // namespace

/// 只有 writer 访问 SQLite；生产者只访问有界环和原子计数器。 / Only writer accesses SQLite;
/// producers use bounded ring and atomics.
struct Telemetry::Impl {
    std::filesystem::path path;
    RunInfo info;
    Options options;
    std::string id = unique_id();
    std::vector<Event> queue, batch;
    std::mutex mutex;
    std::condition_variable ready;
    std::thread thread;
    std::size_t head = 0, size = 0;
    bool stopping = false;
    std::optional<FinalRecord> final;
    std::atomic<bool> enabled{true};
    std::atomic<std::uint64_t> accepted{0}, persisted{0}, dropped{0}, errors{0}, truncations{0},
        high{0};
    std::atomic<int> state{0};
    std::atomic<double> drain_ms{0};
    Text<384> error;
    /// 分配在启动路径完成，限制非法选项避免无限队列。 / Allocate at startup; reject invalid
    /// unbounded options.
    Impl(const std::filesystem::path& p, RunInfo r, Options o)
        : path(p), info(std::move(r)), options(o) {
        if (!o.queue_capacity || !o.batch_size || !o.retain_runs || o.queue_capacity > 1048576 ||
            o.busy_timeout_ms < 0 || o.busy_timeout_ms > 1000)
            throw std::runtime_error("invalid telemetry options");
        queue.resize(o.queue_capacity);
        options.batch_size = std::min(o.batch_size, o.queue_capacity);
        batch.resize(options.batch_size);
        if (!info.started_unix_ns)
            info.started_unix_ns = utc_ns();
        thread = std::thread([this] { run(); });
    }
    /// 最多复制一个有界批次，SQLite 工作不持锁。 / Copy at most one bounded batch; SQL never holds
    /// producer lock.
    std::size_t take() {
        std::unique_lock lock(mutex);
        ready.wait(lock, [this] { return size || stopping; });
        // 第一条事件开始批次时限，结束立即刷新。 / First event starts a batch deadline; finish
        // flushes immediately.
        ready.wait_for(lock, std::chrono::milliseconds(flush_interval_ms),
                       [this] { return size >= batch.size() || stopping; });
        const auto count = std::min(size, batch.size());
        for (std::size_t i = 0; i < count; ++i)
            batch[i] = queue[(head + i) % queue.size()];
        head = (head + count) % queue.size();
        size -= count;
        return count;
    }
    /// 初始化和保留清理也属于后台事务。 / Initialization and retention also belong to background
    /// transactions.
    void initialize(sqlite3* db) {
        // 修改任何持久 PRAGMA 前确认版本和所有者。 / Validate version and ownership before
        // persistent PRAGMA changes.
        Statement version(db, "PRAGMA user_version");
        if (sqlite3_step(version.value) != SQLITE_ROW)
            throw std::runtime_error("missing telemetry schema version");
        const auto v = sqlite3_column_int(version.value, 0);
        version.reset();
        Statement owner(db, "PRAGMA application_id");
        if (sqlite3_step(owner.value) != SQLITE_ROW)
            throw std::runtime_error("missing telemetry database owner");
        const auto application = sqlite3_column_int(owner.value, 0);
        owner.reset();
        if (v != 0 && v != 1)
            throw std::runtime_error("unsupported telemetry schema version");
        if ((v == 1 && application != 1396788564) || (v == 0 && application != 0))
            throw std::runtime_error("telemetry database belongs to another application");
        if (!v) {
            Statement existing(db,
                               "SELECT count(*) FROM sqlite_master WHERE name NOT LIKE 'sqlite_%'");
            if (sqlite3_step(existing.value) != SQLITE_ROW ||
                sqlite3_column_int(existing.value, 0) != 0)
                throw std::runtime_error("unrecognized telemetry schema; refusing reset");
        }
        sql(db, "PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; "
                "PRAGMA wal_autocheckpoint=256;");
        sql(db, R"SQL(BEGIN IMMEDIATE;
CREATE TABLE IF NOT EXISTS runs(run_id TEXT PRIMARY KEY, started_unix_ns INTEGER NOT NULL, ended_unix_ns INTEGER,
 status TEXT NOT NULL, version TEXT, command TEXT, root TEXT, config_json TEXT, error TEXT,
 accepted INTEGER DEFAULT 0, persisted INTEGER DEFAULT 0, dropped INTEGER DEFAULT 0,
 errors INTEGER DEFAULT 0, truncated INTEGER DEFAULT 0, queue_high_water INTEGER DEFAULT 0);
CREATE TABLE IF NOT EXISTS events(event_id INTEGER PRIMARY KEY, run_id TEXT NOT NULL REFERENCES runs ON DELETE CASCADE,
 type TEXT, name TEXT, severity TEXT, message TEXT, backend TEXT, worker INTEGER, span_id INTEGER,
 parent_span_id INTEGER, time_ns INTEGER, duration_ns INTEGER, bytes INTEGER, value REAL, unit TEXT, truncated INTEGER);
CREATE TABLE IF NOT EXISTS metrics(run_id TEXT NOT NULL REFERENCES runs ON DELETE CASCADE, name TEXT NOT NULL,
 value REAL, unit TEXT, PRIMARY KEY(run_id,name));
CREATE TABLE IF NOT EXISTS parameters(run_id TEXT NOT NULL REFERENCES runs ON DELETE CASCADE,
 category TEXT NOT NULL, name TEXT NOT NULL, value TEXT, PRIMARY KEY(run_id,category,name));
CREATE INDEX IF NOT EXISTS events_run_time ON events(run_id,time_ns);
CREATE INDEX IF NOT EXISTS events_name ON events(name,run_id);
CREATE INDEX IF NOT EXISTS events_span ON events(run_id,span_id);
CREATE INDEX IF NOT EXISTS runs_start ON runs(started_unix_ns);
CREATE VIEW IF NOT EXISTS logs AS SELECT * FROM events WHERE type='log';
CREATE VIEW IF NOT EXISTS spans AS SELECT * FROM events WHERE type='span';
CREATE VIEW IF NOT EXISTS latest_run AS SELECT * FROM runs ORDER BY rowid DESC LIMIT 1;
PRAGMA user_version=1;
PRAGMA application_id=1396788564;
UPDATE runs SET status='interrupted', error='previous process did not finalize' WHERE status='running';
COMMIT;)SQL");
        Statement insert(
            db, "INSERT INTO runs(run_id,started_unix_ns,status,version,command,root,config_json) "
                "VALUES(?,?,'running',?,?,?,?)");
        insert.text(1, id.c_str());
        insert.number(2, info.started_unix_ns);
        insert.text(3, info.version.c_str());
        insert.text(4, info.command.c_str());
        insert.text(5, info.root.c_str());
        insert.text(6, info.config_json.c_str());
        insert.write();
        Statement retain(
            db,
            "DELETE FROM runs WHERE run_id IN (SELECT run_id FROM runs WHERE run_id<>? ORDER BY "
            "started_unix_ns DESC,rowid DESC LIMIT -1 OFFSET ?)");
        retain.text(1, id.c_str());
        retain.number(2, options.retain_runs - 1);
        retain.write();
        state.store(1);
    }
    /// 事件全字段绑定，准备语句在批次间复用。 / Bind every event field; reuse statement across
    /// batches.
    void event(Statement& s, const Event& e) {
        s.text(1, id.c_str());
        s.text(2, e.type.c_str());
        s.text(3, e.name.c_str());
        s.text(4, e.severity.c_str());
        s.text(5, e.message.c_str());
        s.text(6, e.backend.c_str());
        s.integer(7, e.worker);
        s.number(8, e.span_id);
        s.number(9, e.parent_span_id);
        s.number(10, e.time_ns);
        s.number(11, e.duration_ns);
        s.number(12, e.bytes);
        s.real(13, e.value);
        s.text(14, e.unit.c_str());
        s.number(15, truncated(e));
        s.write();
    }
    /// 独立结束快照和状态在一个事务中提交。 / Commit dedicated final snapshot and status in one
    /// transaction.
    void finalize(sqlite3* db, Statement& events) {
        sql(db, "BEGIN IMMEDIATE");
        for (const auto& e : final->events)
            event(events, e);
        Statement metric(db, "INSERT OR REPLACE INTO metrics VALUES(?,?,?,?)");
        for (const auto& m : final->metrics) {
            metric.text(1, id.c_str());
            metric.text(2, m.name.c_str());
            metric.real(3, m.value);
            metric.text(4, m.unit.c_str());
            metric.write();
        }
        Statement parameter(db, "INSERT OR REPLACE INTO parameters VALUES(?,?,?,?)");
        for (const auto& p : final->parameters) {
            parameter.text(1, id.c_str());
            parameter.text(2, p.category.c_str());
            parameter.text(3, p.name.c_str());
            parameter.text(4, p.value.c_str());
            parameter.write();
        }
        // 写入实际生效的写者参数，覆盖调用方可能提供的未钳制值。 / Persist effective writer
        // settings, overriding any unclamped caller values.
        const std::pair<const char*, std::uint64_t> settings[] = {
            {"queue_capacity", queue.size()},
            {"batch_size", batch.size()},
            {"event_cap", options.event_cap},
            {"retain_runs", options.retain_runs},
            {"busy_timeout_ms", static_cast<std::uint64_t>(options.busy_timeout_ms)},
            {"flush_interval_ms", flush_interval_ms},
            {"schema_version", 1},
            {"application_id", 1396788564}};
        for (const auto& [name, value] : settings) {
            parameter.text(1, id.c_str());
            parameter.text(2, "telemetry");
            parameter.text(3, name);
            parameter.text(4, std::to_string(value).c_str());
            parameter.write();
        }
        Statement done(db, "UPDATE runs SET "
                           "ended_unix_ns=?,status=?,error=?,accepted=?,persisted=?,dropped=?,"
                           "errors=?,truncated=?,queue_high_water=? WHERE run_id=?");
        done.number(1, utc_ns());
        done.text(2, final->status.c_str());
        done.text(3, final->error.c_str());
        done.number(4, accepted);
        done.number(5, persisted);
        done.number(6, dropped);
        done.number(7, errors);
        done.number(8, truncations);
        done.number(9, high);
        done.text(10, id.c_str());
        done.write();
        sql(db, "COMMIT");
        state.store(final->status == "completed" ? 2 : final->status == "failed" ? 3 : 4);
    }
    /// 所有 SQLite 生命周期包括 checkpoint 均在此线程。 / Entire SQLite lifecycle including
    /// checkpoint runs here.
    void run() noexcept {
        sqlite3* db = nullptr;
        try {
            guard_path(path);
            for (const auto* suffix : {"-wal", "-shm", "-journal"}) {
                auto sidecar = path;
                sidecar += suffix;
                guard_path(sidecar);
            }
            const auto utf8 = path.u8string();
            int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX;
#ifdef SQLITE_OPEN_NOFOLLOW
            flags |= SQLITE_OPEN_NOFOLLOW;
#endif
            if (sqlite3_open_v2(reinterpret_cast<const char*>(utf8.c_str()), &db, flags, nullptr) !=
                SQLITE_OK)
                throw std::runtime_error(db ? sqlite3_errmsg(db)
                                            : "cannot allocate telemetry connection");
            sqlite3_busy_timeout(db, options.busy_timeout_ms);
            initialize(db);
            {
                Statement insert(db, "INSERT INTO "
                                     "events(run_id,type,name,severity,message,backend,worker,span_"
                                     "id,parent_span_id,time_ns,duration_ns,bytes,value,unit,"
                                     "truncated) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
                for (;;) {
                    const auto count = take();
                    if (!count)
                        break;
                    sql(db, "BEGIN IMMEDIATE");
                    for (std::size_t i = 0; i < count; ++i)
                        event(insert, batch[i]);
                    sql(db, "COMMIT");
                    persisted.fetch_add(count);
                }
                finalize(db, insert);
            }
            sqlite3_wal_checkpoint_v2(db, nullptr, SQLITE_CHECKPOINT_PASSIVE, nullptr, nullptr);
        } catch (const std::exception& e) {
            std::lock_guard lock(mutex);
            enabled.store(false);
            error = e.what();
            errors.fetch_add(1);
            dropped.fetch_add(accepted.load() - persisted.load());
            state.store(5);
        } catch (...) {
            std::lock_guard lock(mutex);
            enabled.store(false);
            error = "unknown telemetry writer failure";
            errors.fetch_add(1);
            dropped.fetch_add(accepted.load() - persisted.load());
            state.store(5);
        }
        if (db)
            sqlite3_close_v2(db);
    }
};

Telemetry::Telemetry(const std::filesystem::path& path, RunInfo info, Options options) noexcept {
    if (!options.enabled)
        return;
    try {
        impl_ = std::make_unique<Impl>(path, std::move(info), options);
    } catch (...) {
        startup_failed_ = true;
    }
}
Telemetry::~Telemetry() {
    if (impl_ && impl_->thread.joinable()) {
        FinalRecord record;
        record.status = "interrupted";
        finish(std::move(record));
    }
}
bool Telemetry::emit(const Event& event) noexcept {
    if (!impl_)
        return false;
    auto& p = *impl_;
    std::unique_lock lock(p.mutex, std::try_to_lock);
    if (!lock || !p.enabled.load() || p.stopping || p.size == p.queue.size() ||
        p.accepted >= p.options.event_cap) {
        p.dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    p.queue[(p.head + p.size) % p.queue.size()] = event;
    ++p.size;
    p.accepted.fetch_add(1, std::memory_order_relaxed);
    if (truncated(event))
        p.truncations.fetch_add(1, std::memory_order_relaxed);
    if (p.size > p.high.load(std::memory_order_relaxed))
        p.high.store(p.size, std::memory_order_relaxed);
    lock.unlock();
    p.ready.notify_one();
    return true;
}
Stats Telemetry::finish(FinalRecord final) noexcept {
    if (!impl_)
        return snapshot();
    auto& p = *impl_;
    if (!p.thread.joinable())
        return snapshot();
    const auto start = std::chrono::steady_clock::now();
    {
        std::lock_guard lock(p.mutex);
        p.final.emplace(std::move(final));
        p.stopping = true;
    }
    p.ready.notify_one();
    p.thread.join();
    p.drain_ms.store(
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count());
    return snapshot();
}
Stats Telemetry::snapshot() const noexcept {
    Stats result;
    if (!impl_) {
        result.status = "disabled";
        result.errors = startup_failed_ ? 1 : 0;
        if (startup_failed_)
            result.error = "telemetry startup failed";
        return result;
    }
    const auto& p = *impl_;
    result.accepted = p.accepted;
    result.persisted = p.persisted;
    result.dropped = p.dropped;
    result.errors = p.errors;
    result.truncated = p.truncations;
    result.queue_high_water = p.high;
    result.drain_ms = p.drain_ms;
    constexpr const char* states[] = {"starting", "running",     "completed",
                                      "failed",   "interrupted", "disabled"};
    result.status = states[p.state.load()];
    // state 的发布先于读取不可变错误文本。 / State publication precedes reading immutable error
    // text.
    if (p.state.load() == 5)
        result.error = p.error;
    return result;
}
const std::string& Telemetry::run_id() const noexcept {
    static const std::string empty;
    return impl_ ? impl_->id : empty;
}
} // namespace same::telemetry
