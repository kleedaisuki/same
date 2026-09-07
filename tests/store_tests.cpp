#include "same/store.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <sqlite3.h>
#include <stdexcept>
#include <vector>

namespace {
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
same::FileRecord record(std::string path, std::uint64_t size, unsigned char hash = 42) {
    same::FileRecord result;
    result.path = std::move(path);
    result.stamp = {size, "identity", "mtime", "ctime"};
    result.digest.fill(hash);
    return result;
}
} // namespace
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
            store.begin_scan();
            store.save(a);
            store.end_scan();
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
