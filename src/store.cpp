#include "same/store.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <sqlite3.h>
#include <stdexcept>

namespace same {
namespace {
[[noreturn]] void fail(sqlite3* db, const char* operation) {
    throw std::runtime_error(std::string(operation) + ": " + sqlite3_errmsg(db));
}
void exec(sqlite3* db, const char* sql) {
    if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
        fail(db, "SQLite execution");
}
class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK)
            fail(db, "SQLite prepare");
    }
    ~Statement() {
        sqlite3_finalize(stmt_);
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    void bytes(int index, const void* data, std::size_t length) {
        if (sqlite3_bind_blob64(stmt_, index, data, length, SQLITE_TRANSIENT) != SQLITE_OK)
            fail(db_, "SQLite bind bytes");
    }
    void string(int index, std::string_view value) {
        bytes(index, value.data(), value.size());
    }
    void integer(int index, sqlite3_int64 value) {
        if (sqlite3_bind_int64(stmt_, index, value) != SQLITE_OK)
            fail(db_, "SQLite bind integer");
    }
    bool step() {
        const int result = sqlite3_step(stmt_);
        if (result == SQLITE_ROW)
            return true;
        if (result == SQLITE_DONE)
            return false;
        fail(db_, "SQLite step");
    }
    sqlite3_int64 integer(int index) const {
        return sqlite3_column_int64(stmt_, index);
    }
    std::string string(int index) const {
        const auto* data = static_cast<const char*>(sqlite3_column_blob(stmt_, index));
        const auto length = sqlite3_column_bytes(stmt_, index);
        return length == 0 ? std::string{} : std::string(data, static_cast<std::size_t>(length));
    }
    FileRecord record() const {
        FileRecord result;
        result.path = string(0);
        const auto size = string(1);
        const auto digest = string(5);
        if (size.size() != 8 || digest.size() != result.digest.size())
            throw std::runtime_error("Invalid file metadata in state database");
        for (const unsigned char byte : size)
            result.stamp.size = (result.stamp.size << 8) | byte;
        result.stamp.identity = string(2);
        result.stamp.modified = string(3);
        result.stamp.changed = string(4);
        std::memcpy(result.digest.data(), digest.data(), digest.size());
        return result;
    }
    void bind_record(const FileRecord& record) {
        std::array<unsigned char, 8> size{};
        auto value = record.stamp.size;
        for (std::size_t i = size.size(); i != 0; --i) {
            size[i - 1] = static_cast<unsigned char>(value & 255);
            value >>= 8;
        }
        string(1, record.path);
        bytes(2, size.data(), size.size());
        string(3, record.stamp.identity);
        string(4, record.stamp.modified);
        string(5, record.stamp.changed);
        bytes(6, record.digest.data(), record.digest.size());
    }

private:
    sqlite3* db_{};
    sqlite3_stmt* stmt_{};
};
} // namespace
struct Store::Impl {
    sqlite3* db{};
    bool scanning{};
    sqlite3_int64 generation{};
    ~Impl() {
        if (db)
            sqlite3_close_v2(db);
    }
};
Store::Store(const std::filesystem::path& path, std::size_t cache_bytes)
    : impl_(std::make_unique<Impl>()) {
    const auto utf8 = path.u8string();
    if (sqlite3_open_v2(reinterpret_cast<const char*>(utf8.c_str()), &impl_->db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                        nullptr) != SQLITE_OK)
        fail(impl_->db, "Open state database");
    auto* db = impl_->db;
    sqlite3_busy_timeout(db, 5000);
    // Reject future schemas before even changing their journal mode.
    // 拒绝未来版本时，连日志模式也不能修改。
    {
        Statement version(db, "PRAGMA user_version");
        if (!version.step())
            throw std::runtime_error("Missing SQLite schema version");
        const auto schema = version.integer(0);
        if (schema != 0 && schema != 1)
            throw std::runtime_error("Unsupported state database schema version");
    }
    exec(db, "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL; PRAGMA temp_store=FILE; PRAGMA "
             "mmap_size=0;");
    const auto cache_kib = std::clamp<std::size_t>(cache_bytes / 1024, 16, 1024 * 1024);
    const auto pragmas = "PRAGMA cache_size=-" + std::to_string(cache_kib) +
                         "; PRAGMA temp.cache_size=-" + std::to_string(cache_kib) + ";";
    exec(db, pragmas.c_str());
    exec(db, "BEGIN IMMEDIATE;");
    try {
        exec(db, "CREATE TABLE IF NOT EXISTS files("
                 "path BLOB PRIMARY KEY NOT NULL,size BLOB NOT NULL CHECK(length(size)=8),"
                 "identity BLOB NOT NULL,modified BLOB NOT NULL,changed BLOB NOT NULL,"
                 "digest BLOB NOT NULL CHECK(length(digest)=32),generation INTEGER NOT NULL) "
                 "WITHOUT ROWID;"
                 "CREATE INDEX IF NOT EXISTS files_content ON files(size,digest,path);"
                 "CREATE TABLE IF NOT EXISTS scan_state(id INTEGER PRIMARY KEY "
                 "CHECK(id=1),generation INTEGER NOT NULL);"
                 "INSERT OR IGNORE INTO scan_state VALUES(1,0); PRAGMA user_version=1; COMMIT;");
    } catch (...) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
    exec(db, "CREATE TEMP TABLE representatives("
             "path BLOB PRIMARY KEY NOT NULL,size BLOB NOT NULL,identity BLOB NOT NULL,"
             "modified BLOB NOT NULL,changed BLOB NOT NULL,digest BLOB NOT NULL) WITHOUT ROWID;"
             "CREATE TEMP TABLE matches(representative BLOB NOT NULL,member BLOB NOT NULL,"
             "PRIMARY KEY(representative,member)) WITHOUT ROWID;");
}
Store::~Store() {
    rollback_scan();
}
void Store::begin_scan() {
    if (impl_->scanning)
        throw std::logic_error("Scan already active");
    exec(impl_->db, "BEGIN IMMEDIATE");
    impl_->scanning = true;
    try {
        Statement query(impl_->db, "SELECT generation FROM scan_state WHERE id=1");
        if (!query.step())
            throw std::runtime_error("Missing state generation");
        const auto old = query.integer(0);
        if (old == std::numeric_limits<sqlite3_int64>::max()) {
            exec(impl_->db, "UPDATE files SET generation=0");
            impl_->generation = 1;
        } else {
            impl_->generation = old + 1;
        }
        Statement update(impl_->db, "UPDATE scan_state SET generation=?1 WHERE id=1");
        update.integer(1, impl_->generation);
        update.step();
    } catch (...) {
        rollback_scan();
        throw;
    }
}
std::optional<FileRecord> Store::cached(std::string_view path) {
    Statement query(impl_->db,
                    "SELECT path,size,identity,modified,changed,digest FROM files WHERE path=?1");
    query.string(1, path);
    if (!query.step())
        return std::nullopt;
    return query.record();
}
void Store::save(const FileRecord& record) {
    if (!impl_->scanning)
        throw std::logic_error("No active scan");
    Statement insert(
        impl_->db,
        "INSERT INTO files(path,size,identity,modified,changed,digest,generation)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7) ON CONFLICT(path) DO UPDATE SET "
        "size=excluded.size,identity=excluded.identity,modified=excluded.modified,"
        "changed=excluded.changed,digest=excluded.digest,generation=excluded.generation");
    insert.bind_record(record);
    insert.integer(7, impl_->generation);
    insert.step();
}
void Store::end_scan() {
    if (!impl_->scanning)
        throw std::logic_error("No active scan");
    Statement remove(impl_->db, "DELETE FROM files WHERE generation<>?1");
    remove.integer(1, impl_->generation);
    remove.step();
    exec(impl_->db, "COMMIT");
    impl_->scanning = false;
}
void Store::rollback_scan() noexcept {
    if (!impl_->scanning)
        return;
    sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
    impl_->scanning = false;
}
void Store::visit_candidates(const std::function<void(const FileRecord&)>& visitor) {
    if (impl_->scanning)
        throw std::logic_error("Commit scan before comparing");
    Statement query(impl_->db,
                    "SELECT f.path,f.size,f.identity,f.modified,f.changed,f.digest FROM files f "
                    "JOIN (SELECT size,digest FROM files GROUP BY size,digest HAVING count(*)>1) d "
                    "ON f.size=d.size AND f.digest=d.digest ORDER BY f.size,f.digest,f.path");
    while (query.step())
        visitor(query.record());
}
void Store::clear_representatives() {
    exec(impl_->db, "DELETE FROM representatives");
}
void Store::add_representative(const FileRecord& record) {
    Statement insert(impl_->db, "INSERT INTO representatives VALUES(?1,?2,?3,?4,?5,?6)");
    insert.bind_record(record);
    insert.step();
}
void Store::visit_representatives(const std::function<bool(const FileRecord&)>& visitor) {
    Statement query(
        impl_->db,
        "SELECT path,size,identity,modified,changed,digest FROM representatives ORDER BY path");
    while (query.step())
        if (!visitor(query.record()))
            break;
}
void Store::reset_matches() {
    exec(impl_->db, "DELETE FROM matches");
}
void Store::add_match(std::string_view representative, std::string_view member) {
    Statement insert(impl_->db, "INSERT OR IGNORE INTO matches VALUES(?1,?2)");
    insert.string(1, representative);
    insert.string(2, member);
    insert.step();
}
void Store::visit_matches(const std::function<void(std::string_view, std::string_view)>& visitor) {
    Statement query(
        impl_->db,
        "SELECT m.representative,m.member FROM matches m JOIN "
        "(SELECT representative FROM matches GROUP BY representative HAVING count(*)>1) g "
        "ON m.representative=g.representative ORDER BY m.representative,m.member");
    while (query.step()) {
        const auto representative = query.string(0);
        const auto member = query.string(1);
        visitor(representative, member);
    }
}
} // namespace same
