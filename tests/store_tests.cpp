/** @file
 * @brief 缓存事务、碰撞候选排序、精确组去重和未来版本拒绝。 / Cache transactions, collision
 * ordering, exact-group deduplication and future-schema rejection.
 */
#include "same/store.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <sqlite3.h>
#include <stdexcept>
#include <vector>

namespace {
/// 断言失败抛出可定位错误。 / Throw a diagnostic on assertion failure.
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
/// 构造可控大小与摘要的记录，隔离存储行为。 / Construct controlled metadata and hashes to isolate
/// storage behavior.
same::FileRecord record(std::string path, std::uint64_t size, unsigned char hash = 42) {
    same::FileRecord result;
    result.path = std::move(path);
    result.stamp = {size, "identity", "mtime", "ctime"};
    result.digest.fill(hash);
    return result;
}
} // namespace
/// 运行本文件全部回归场景，断言失败即返回非零。 / Run all regressions; assertion failures produce a
/// nonzero exit.
int main() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("same-store-test-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    try {
        const auto db = root / "state.db";
        const auto a = record("a", 123);
        const auto b = record("b", 123);
        const auto c = record("c", 456);
        const auto binary = record(std::string("nul\0name", 8), UINT64_MAX);
        {
            same::Store store(db);
            store.begin_scan();
            store.save(b);
            store.save(c);
            store.save(a);
            store.save(binary);
            store.end_scan();
            check(store.cached(binary.path)->stamp == binary.stamp,
                  "64-bit size / binary path roundtrip");
            std::vector<std::string> paths;
            store.visit_candidates([&](const auto& item) { paths.push_back(item.path); });
            check(paths == std::vector<std::string>{"a", "b"}, "candidate order and filtering");
            store.visit_candidates([&](const auto& item) {
                store.clear_representatives();
                store.add_representative(item);
                store.visit_representatives([&](const auto& rep) {
                    store.add_match(rep.path, item.path);
                    return true;
                });
            });
            store.begin_scan();
            store.save(record("a", 999));
            store.rollback_scan();
            check(store.cached("a")->stamp == a.stamp, "rollback restores metadata");
            store.clear_representatives();
            store.add_representative(b);
            store.add_representative(a);
            // 重复键失败后，复用语句必须可重新绑定。 / A failed reused statement must
            // accept fresh bindings after a constraint error.
            bool duplicate_rejected = false;
            try {
                store.add_representative(a);
            } catch (const std::runtime_error&) {
                duplicate_rejected = true;
            }
            check(duplicate_rejected, "duplicate representative rejected");
            store.add_representative(c);
            check(!store.cached("missing"), "reused lookup miss");
            check(store.cached(binary.path)->stamp == binary.stamp, "lookup bindings refreshed");
            int visited = 0;
            store.visit_representatives([&](const auto& item) {
                ++visited;
                check(item.path == "a", "representative ordering");
                return false;
            });
            check(visited == 1, "representative early stop");
            store.reset_matches();
            store.add_match("a", "b");
            store.add_match("a", "a");
            store.add_match("a", "a");
            store.add_match("c", "c");
            paths.clear();
            store.visit_matches([&](auto rep, auto member) {
                check(rep == "a", "singleton exact group filtered");
                paths.emplace_back(member);
            });
            check(paths == std::vector<std::string>{"a", "b"},
                  "exact matches deduplicated ordered");
            paths.clear();
            store.visit_unique([&](auto path) { paths.emplace_back(path); });
            check(paths == std::vector<std::string>{"c", binary.path}, "unique singleton handling");
            bool inactive_rejected = false;
            try {
                store.mark_if_unchanged(a.path, a.stamp);
            } catch (const std::logic_error&) {
                inactive_rejected = true;
            }
            check(inactive_rejected, "conditional mark requires scan");
            store.begin_scan();
            check(store.mark_if_unchanged(binary.path, binary.stamp),
                  "conditional mark binary path and uint64 size");
            check(store.mark_if_unchanged(binary.path, binary.stamp),
                  "conditional mark repeats in same generation");
            check(!store.mark_if_unchanged("missing", a.stamp), "conditional mark absent record");
            // 四个戳字段必须分别参与比较，任一变化都不能保留旧摘要。
            // Every stamp field participates independently; changes cannot retain a stale digest.
            auto changed = a.stamp;
            ++changed.size;
            check(!store.mark_if_unchanged(a.path, changed), "conditional mark checks size");
            changed = a.stamp;
            changed.identity += "changed";
            check(!store.mark_if_unchanged(a.path, changed), "conditional mark checks identity");
            changed = a.stamp;
            changed.modified += "changed";
            check(!store.mark_if_unchanged(a.path, changed), "conditional mark checks modified");
            changed = a.stamp;
            changed.changed += "changed";
            check(!store.mark_if_unchanged(a.path, changed), "conditional mark checks changed");
            check(store.cached(a.path)->stamp == a.stamp, "conditional misses preserve metadata");
            store.rollback_scan();
            store.begin_scan();
            check(store.mark_if_unchanged(a.path, a.stamp),
                  "conditional mark reuse after rollback");
            store.mark_seen(b.path);
            store.mark_seen(c.path);
            store.mark_seen(binary.path);
            store.end_scan();
            store.begin_scan();
            bool missing_rejected = false;
            try {
                store.mark_seen("missing");
            } catch (const std::logic_error&) {
                missing_rejected = true;
            }
            check(missing_rejected, "mark_seen rejects absent records");
            store.mark_seen(a.path);
            store.end_scan();
            check(store.cached(a.path)->stamp == a.stamp, "mark_seen preserves metadata");
            check(store.cached(a.path)->digest == a.digest, "mark_seen preserves digest");
            check(!store.cached("b") && !store.cached("c"), "unseen files removed");
            store.begin_scan();
            store.save(record("a", 777));
            // Destruction must roll back the incomplete scan.
            // 析构必须回滚未完成扫描。
        }
        {
            same::Store store(db);
            check(store.cached("a")->stamp == a.stamp, "reopen / destruction rollback");
            store.begin_scan();
            store.end_scan();
            check(!store.cached("a"), "empty scan removes stale files");
        }
        {
            same::Store store(root / "buckets.db", 16 * 1024);
            store.begin_scan();
            // 交错插入多个桶，验证两层游标的排序、过滤和独立回调操作。
            // Interleave buckets to verify two-cursor ordering, filtering and independent
            // callbacks.
            for (const auto& item :
                 {record("z", 9, 2), record("b", 1, 2), record("x", 9, 2), record("a", 1, 2),
                  record("solo", 1, 3), record("d", 1, 1), record("c", 1, 1)})
                store.save(item);
            store.end_scan();
            bool interrupted = false;
            try {
                store.visit_candidates([](const auto&) { throw std::runtime_error("stop"); });
            } catch (const std::runtime_error&) {
                interrupted = true;
            }
            check(interrupted, "candidate callback exception propagates");
            std::vector<std::string> paths;
            store.visit_candidates([&](const auto& item) {
                check(store.cached(item.path)->stamp == item.stamp, "nested independent lookup");
                paths.push_back(item.path);
            });
            check(paths == std::vector<std::string>{"c", "d", "a", "b", "x", "z"},
                  "multiple buckets ordered after callback exception");
            store.add_match("x", "z");
            store.add_match("a", "b");
            store.add_match("solo", "solo");
            store.add_match("x", "x");
            store.add_match("a", "a");
            interrupted = false;
            try {
                store.visit_matches([](auto, auto) { throw std::runtime_error("stop"); });
            } catch (const std::runtime_error&) {
                interrupted = true;
            }
            check(interrupted, "match callback exception propagates");
            paths.clear();
            store.visit_matches([&](auto, auto member) { paths.emplace_back(member); });
            check(paths == std::vector<std::string>{"a", "b", "x", "z"},
                  "multiple exact groups ordered after callback exception");
            store.begin_scan();
            check(!store.mark_if_unchanged("a", record("a", 2, 2).stamp),
                  "changed entry is not marked seen");
            check(store.mark_if_unchanged("b", record("b", 1, 2).stamp), "unchanged retained");
            store.end_scan();
            check(!store.cached("a") && store.cached("b"), "conditional misses removed at commit");
        }
        sqlite3* raw{};
        check(sqlite3_open(db.string().c_str(), &raw) == SQLITE_OK, "raw database open");
        check(sqlite3_exec(raw, "PRAGMA journal_mode=WAL; PRAGMA user_version=999", nullptr,
                           nullptr, nullptr) == SQLITE_OK,
              "set future version");
        sqlite3_close(raw);
        bool rejected = false;
        try {
            same::Store store(db);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        check(rejected, "future schema rejected");
        check(sqlite3_open(db.string().c_str(), &raw) == SQLITE_OK, "reopen future database");
        std::string mode;
        auto capture = [](void* context, int, char** values, char**) {
            *static_cast<std::string*>(context) = values[0];
            return 0;
        };
        check(sqlite3_exec(raw, "PRAGMA journal_mode", capture, &mode, nullptr) == SQLITE_OK,
              "query preserved journal mode");
        sqlite3_close(raw);
        check(mode == "wal", "future database journal mode modified");
        std::filesystem::remove_all(root);
        std::cout << "store tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::filesystem::remove_all(root);
        return 1;
    }
}
