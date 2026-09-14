/** @file
 * 独立 SQLite 遥测生命周期、压力丢弃和失败隔离。 / Independent SQLite telemetry lifecycle, pressure
 * loss and isolation.
 */
// 白盒验证 SQLite 错误契约；与 hash_retry/async_scan 测试采用同一链接方式。
// White-box SQLite error-contract tests use the same linking pattern as hash_retry/async_scan.
#include "../src/telemetry.cpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sqlite3.h>
#include <stdexcept>
#include <thread>

namespace {
/// 可定位测试断言。 / Diagnosable test assertion.
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
/// 测试查询连接，只在测试线程使用。 / Test-only query connection, owned by the test thread.
struct Database {
    sqlite3* db = nullptr;
    /// 打开 UTF-8 路径。 / Open a UTF-8 path.
    explicit Database(const std::filesystem::path& path) {
        const auto text = path.u8string();
        check(sqlite3_open(reinterpret_cast<const char*>(text.c_str()), &db) == SQLITE_OK,
              "open test db");
    }
    ~Database() {
        sqlite3_close(db);
    }
    /// 执行测试控制 SQL。 / Execute test control SQL.
    void exec(const char* sql) {
        check(sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK, "test SQL");
    }
    /// 读取单一整数。 / Read one integer.
    sqlite3_int64 scalar(const char* sql) {
        sqlite3_stmt* stmt = nullptr;
        check(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK, "prepare test query");
        const int rc = sqlite3_step(stmt);
        const auto result = rc == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : -1;
        sqlite3_finalize(stmt);
        check(rc == SQLITE_ROW, "read test query");
        return result;
    }
};
/// 精确检查被拒绝数据库未被修改。 / Check rejected databases are byte-for-byte unchanged.
std::string contents(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
/// 构造最终快照，验证专用终态槽。 / Construct final snapshot to test the dedicated terminal slot.
same::telemetry::FinalRecord final_record() {
    same::telemetry::FinalRecord final;
    final.metrics.push_back({"files", 42, "count"});
    final.parameters.push_back({"model", "cpu.0.mean", "1.234"});
    same::telemetry::Event span;
    span.type = "span";
    span.name = "run";
    span.duration_ns = 1234;
    final.events.push_back(span);
    return final;
}
} // namespace
/// 独立测试不依赖 CUDA 或产品查询接口。 / Standalone tests require neither CUDA nor product query
/// APIs.
int main() {
    using namespace same::telemetry;
    // macOS 的 /var 临时目录祖先可能是链接；不削弱产品的 NOFOLLOW 策略。
    // macOS temporary ancestors may be symlinks; preserve production NOFOLLOW protection.
    const auto root = std::filesystem::canonical(std::filesystem::temp_directory_path()) /
                      ("same-telemetry-test-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    try {
        {
            Database memory(":memory:");
            memory.exec("CREATE TABLE binding(value)");
            Statement statement(memory.db, "INSERT INTO binding VALUES(?)");
            sqlite3_limit(memory.db, SQLITE_LIMIT_LENGTH, 64);
            bool text_failed = false, integer_failed = false, real_failed = false;
            try {
                statement.text(1, std::string(128, 'x').c_str());
            } catch (const std::runtime_error&) {
                text_failed = true;
            }
            try {
                statement.integer(2, -1);
            } catch (const std::runtime_error&) {
                integer_failed = true;
            }
            try {
                statement.real(2, 1.0);
            } catch (const std::runtime_error&) {
                real_failed = true;
            }
            check(text_failed && integer_failed && real_failed, "every binding error propagated");
            check(memory.scalar("SELECT count(*) FROM binding") == 0,
                  "failed bindings never silently write null");
            statement.reset();
            statement.number(1, 42);
            statement.write();
            check(memory.scalar("SELECT value FROM binding") == 42,
                  "successful binding/reset preserved");
        }
        const auto path = root / "telemetry.db";
        Options options;
        options.queue_capacity = 8;
        options.batch_size = 2;
        options.event_cap = 4;
        Event event;
        event.type = "log";
        event.name = "test";
        event.message = std::string(500, 'x');
        std::string first_id;
        {
            Telemetry t(path, {"test", "scan", "root", "{}"}, options);
            first_id = t.run_id();
            check(!first_id.empty(), "run ID");
            for (int i = 0; i < 100; ++i)
                t.emit(event);
            auto result = t.finish(final_record());
            if (std::string_view(result.status) != "completed")
                throw std::runtime_error(std::string("final status: ") + result.error.c_str());
            check(result.accepted <= 4 && result.persisted == result.accepted,
                  "cap and persistence");
            check(result.accepted + result.dropped == 100, "loss accounted");
            check(result.truncated == result.accepted, "truncation accounted");
            check(result.drain_ms >= 0, "drain timing");
            check(t.finish().persisted == result.persisted, "idempotent finish");
        }
        {
            Database db(path);
            check(db.scalar("PRAGMA user_version") == 1, "schema version");
            check(db.scalar("SELECT count(*) FROM spans") == 1, "reserved terminal span");
            check(db.scalar("SELECT count(*) FROM metrics WHERE name='files' AND value=42") == 1,
                  "final metric");
            check(db.scalar("SELECT count(*) FROM parameters WHERE value='1.234'") == 1,
                  "model snapshot");
            db.exec("UPDATE runs SET status='running'");
        }
        {
            Telemetry t(path, {}, options);
            check(t.run_id() != first_id, "cross-run ID");
            check(std::string_view(t.finish().status) == "completed", "second run");
            Database db(path);
            check(db.scalar("SELECT count(*) FROM runs") == 2, "cross-run persisted");
            check(db.scalar("SELECT count(*) FROM runs WHERE status='interrupted'") == 1,
                  "abandoned recovered");
        }
        options.retain_runs = 2;
        for (int i = 0; i < 3; ++i) {
            Telemetry t(path, {}, options);
            t.finish(final_record());
        }
        {
            Database db(path);
            check(db.scalar("SELECT count(*) FROM runs") == 2, "retention runs");
            check(db.scalar("SELECT count(*) FROM parameters WHERE category='model'") == 2,
                  "retention cascades");
        }
        {
            Database locked(path);
            locked.exec("BEGIN IMMEDIATE");
            options.queue_capacity = 2;
            options.batch_size = 1;
            options.event_cap = 1000;
            options.busy_timeout_ms = 500;
            Telemetry t(path, {}, options);
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < 1000; ++i)
                t.emit(event);
            const auto elapsed = std::chrono::steady_clock::now() - start;
            check(elapsed < std::chrono::milliseconds(100), "SQL lock cannot block producers");
            check(t.snapshot().dropped >= 998, "queue pressure drops");
            locked.exec("ROLLBACK");
            check(std::string_view(t.finish(final_record()).status) == "completed",
                  "terminal survives pressure");
        }
        {
            Database locked(path);
            locked.exec("BEGIN IMMEDIATE");
            options.busy_timeout_ms = 1;
            Telemetry t(path, {}, options);
            t.emit(event);
            const auto stats = t.finish();
            check(stats.errors == 1 && std::string_view(stats.status) == "disabled",
                  "external lock isolated");
            check(stats.persisted == 0, "failed write not counted");
            locked.exec("ROLLBACK");
        }
        {
            const auto corrupt = root / "corrupt.db";
            std::ofstream(corrupt) << "not a SQLite database";
            Telemetry t(corrupt, {});
            t.emit(event);
            check(t.finish().errors == 1, "corrupt DB isolated");
            check(std::filesystem::file_size(corrupt) == 21, "corrupt DB not reset");
        }
        {
            const auto future = root / "future.db";
            {
                Database db(future);
                db.exec("PRAGMA user_version=99; CREATE TABLE precious(x)");
            }
            const auto before = contents(future);
            Telemetry t(future, {});
            check(t.finish().errors == 1, "future schema rejected");
            Database db(future);
            check(db.scalar("PRAGMA user_version") == 99, "future schema preserved");
            check(contents(future) == before, "future database byte unchanged");
        }
        {
            const auto disabled = root / "disabled.db";
            Options off;
            off.enabled = false;
            Telemetry t(disabled, {}, off);
            check(!t.emit(event), "disabled emit");
            check(t.finish().errors == 0 && !std::filesystem::exists(disabled), "disabled no IO");
        }
        {
            const auto interrupted = root / "interrupted.db";
            {
                Telemetry t(interrupted, {});
            }
            Database db(interrupted);
            check(db.scalar("SELECT count(*) FROM runs WHERE status='interrupted'") == 1,
                  "destructor joins");
        }
        {
            const auto failed = root / "failed.db";
            Telemetry t(failed, {});
            auto final = final_record();
            final.status = "failed";
            final.error = "controlled application failure";
            check(std::string_view(t.finish(std::move(final)).status) == "failed",
                  "application failure is terminal, not writer failure");
            Database db(failed);
            check(db.scalar("SELECT count(*) FROM runs WHERE status='failed' AND errors=0") == 1,
                  "failed application persisted");
        }
        {
            const auto unknown = root / "unknown.db";
            {
                Database db(unknown);
                db.exec("CREATE TABLE precious(x); INSERT INTO precious VALUES(7)");
            }
            const auto before = contents(unknown);
            Telemetry t(unknown, {});
            check(t.finish().errors == 1, "unknown schema rejected");
            Database db(unknown);
            check(db.scalar("SELECT x FROM precious") == 7, "unknown schema preserved");
            check(contents(unknown) == before, "unknown database byte unchanged");
        }
        {
            const auto rollback = root / "clock-rollback.db";
            Options one;
            one.queue_capacity = 1;
            one.retain_runs = 1;
            RunInfo later;
            later.started_unix_ns = 2000;
            {
                Telemetry t(rollback, later, one);
                check(t.finish().errors == 0, "queue one clamps default batch");
            }
            RunInfo earlier;
            earlier.started_unix_ns = 1000;
            {
                Telemetry t(rollback, earlier, one);
                t.emit(event);
                check(t.finish(final_record()).errors == 0, "clock rollback retains current run");
            }
            Database db(rollback);
            check(db.scalar("SELECT count(*) FROM runs") == 1, "rollback retention bounded");
            check(db.scalar("SELECT started_unix_ns FROM latest_run") == 1000,
                  "latest is insertion order");
            check(db.scalar("SELECT count(*) FROM spans") == 1, "rollback terminal retained");
            check(db.scalar("SELECT CAST(value AS INTEGER) FROM parameters WHERE "
                            "category='telemetry' AND name='batch_size'") == 1,
                  "effective clamped writer batch dumped");
            check(db.scalar("SELECT CAST(value AS INTEGER) FROM parameters WHERE "
                            "category='telemetry' AND name='flush_interval_ms'") == 250,
                  "writer flush deadline dumped");
        }
        {
            const auto special = root / "directory.db";
            std::filesystem::create_directory(special);
            Telemetry t(special, {});
            check(t.finish().errors == 1, "directory isolated");
        }
        {
            const auto sidecar = root / "sidecar.db";
            std::filesystem::create_directory(root / "sidecar.db-wal");
            Telemetry t(sidecar, {});
            check(t.finish().errors == 1, "special sidecar isolated");
            check(!std::filesystem::exists(sidecar), "sidecar rejected before database creation");
        }
        {
            const auto target = root / "target.db";
            std::ofstream(target) << "do not touch";
            const auto linked = root / "hardlink.db";
            std::error_code error;
            std::filesystem::create_hard_link(target, linked, error);
            if (!error) {
                Telemetry t(linked, {});
                check(t.finish().errors == 1, "hardlink isolated");
                check(contents(target) == "do not touch", "hardlink target unchanged");
            }
            const auto symbolic = root / "symlink.db";
            std::filesystem::create_symlink(target, symbolic, error);
            if (!error) {
                Telemetry t(symbolic, {});
                check(t.finish().errors == 1, "symlink isolated");
                check(contents(target) == "do not touch", "symlink target unchanged");
            }
        }
        Text<5> text;
        text = "abc\xc3\xa9";
        check(std::string_view(text) == "abc" && text.truncated, "UTF8 truncation boundary");
        std::filesystem::remove_all(root);
        std::cout << "telemetry tests passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        std::filesystem::remove_all(root);
        return 1;
    }
}
