#include "same/model_store.hpp"
#include <bit>
#include <cstdint>
#include <map>
#include <sqlite3.h>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace same {
namespace {
/// 格式与大小固定，不依赖本机字节序或结构填充。 / Fixed format independent of endianness/padding.
constexpr int schema_version = 1;
constexpr std::size_t payload_size = 3 * (30 * 8 + 8);
/// 拒绝链接和特殊文件，包括 SQLite 边车。 / Reject links and special files, including SQLite
/// sidecars.
void guard_path(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory ||
        status.type() == std::filesystem::file_type::not_found)
        return;
    if (error || !std::filesystem::is_regular_file(status))
        throw std::runtime_error("unsafe model database path");
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("unsafe model reparse point");
#endif
    if (std::filesystem::hard_link_count(path, error) != 1 || error)
        throw std::runtime_error("unsafe model hard links");
}
/// SQLite 语句作用域所有者。 / Scoped SQLite statement owner.
struct Statement {
    sqlite3_stmt* value{};
    ~Statement() {
        sqlite3_finalize(value);
    }
};
/// 错误转换为冷路径诊断异常。 / Convert errors into cold-path diagnostic exceptions.
void check(sqlite3* db, int result) {
    if (result != SQLITE_OK && result != SQLITE_DONE)
        throw std::runtime_error(sqlite3_errmsg(db));
}
/// 执行固定 SQL。 / Execute constant SQL.
void execute(sqlite3* db, const char* sql) {
    check(db, sqlite3_exec(db, sql, nullptr, nullptr, nullptr));
}
/// 编码一个小端整数。 / Encode one little-endian integer.
void append(std::vector<unsigned char>& bytes, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
        bytes.push_back(static_cast<unsigned char>(value >> (i * 8)));
}
/// 解码已通过长度检查的小端整数。 / Decode a length-checked little-endian integer.
std::uint64_t take(const unsigned char*& bytes) {
    std::uint64_t value{};
    for (unsigned i = 0; i < 8; ++i)
        value |= std::uint64_t(*bytes++) << (i * 8);
    return value;
}
/// 显式字段序列化，不保存 ABI。 / Explicit field serialization, never ABI serialization.
std::vector<unsigned char> encode(const detail::OnlineModel::State& state) {
    std::vector<unsigned char> bytes;
    bytes.reserve(payload_size);
    for (const auto& s : state) {
        for (auto x : s.xtx)
            append(bytes, std::bit_cast<std::uint64_t>(x));
        for (auto x : s.xty)
            append(bytes, std::bit_cast<std::uint64_t>(x));
        for (auto x : s.minimum)
            append(bytes, std::bit_cast<std::uint64_t>(x));
        for (auto x : s.maximum)
            append(bytes, std::bit_cast<std::uint64_t>(x));
        append(bytes, std::bit_cast<std::uint64_t>(s.yty));
        append(bytes, std::bit_cast<std::uint64_t>(s.weight));
        append(bytes, s.samples);
    }
    return bytes;
}
/// 对固定载荷逐字段解码。 / Decode a fixed payload field by field.
detail::OnlineModel::State decode(const unsigned char* bytes) {
    detail::OnlineModel::State state{};
    for (auto& s : state) {
        for (auto& x : s.xtx)
            x = std::bit_cast<double>(take(bytes));
        for (auto& x : s.xty)
            x = std::bit_cast<double>(take(bytes));
        for (auto& x : s.minimum)
            x = std::bit_cast<double>(take(bytes));
        for (auto& x : s.maximum)
            x = std::bit_cast<double>(take(bytes));
        s.yty = std::bit_cast<double>(take(bytes));
        s.weight = std::bit_cast<double>(take(bytes));
        s.samples = take(bytes);
    }
    return state;
}
} // namespace
/// 冷路径连接；关闭自动回滚未提交事务。 / Cold-path connection; closing rolls back transactions.
struct ModelStore::Impl {
    sqlite3* db{};
    std::string diagnostic;
    /// 启动快照支持并发只读查询。 / Startup snapshot supports concurrent read-only lookup.
    std::map<std::string, detail::OnlineModel::State, std::less<>> entries;
    ~Impl() {
        sqlite3_close(db);
    }
};
ModelStore::ModelStore(const std::filesystem::path& path) : impl_(std::make_unique<Impl>()) {
    try {
        guard_path(path);
        for (const auto* suffix : {"-journal", "-wal", "-shm"}) {
            auto sidecar = path;
            sidecar += suffix;
            guard_path(sidecar);
        }
        const auto utf8 = path.u8string();
        int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX;
#ifdef SQLITE_OPEN_NOFOLLOW
        flags |= SQLITE_OPEN_NOFOLLOW;
#endif
        check(impl_->db, sqlite3_open_v2(reinterpret_cast<const char*>(utf8.c_str()), &impl_->db,
                                         flags, nullptr));
        sqlite3_limit(impl_->db, SQLITE_LIMIT_LENGTH, 16384);
        sqlite3_busy_timeout(impl_->db, 100);
        execute(impl_->db, "PRAGMA trusted_schema=OFF; PRAGMA cache_size=-64;");
        Statement owner;
        check(impl_->db,
              sqlite3_prepare_v2(impl_->db, "PRAGMA application_id", -1, &owner.value, nullptr));
        if (sqlite3_step(owner.value) != SQLITE_ROW)
            throw std::runtime_error("missing model ownership");
        const int application = sqlite3_column_int(owner.value, 0);
        sqlite3_finalize(owner.value);
        owner.value = nullptr;
        if (application != 0 && application != 1396788556)
            throw std::runtime_error("foreign model database");
        Statement version;
        check(impl_->db,
              sqlite3_prepare_v2(impl_->db, "PRAGMA user_version", -1, &version.value, nullptr));
        if (sqlite3_step(version.value) != SQLITE_ROW)
            throw std::runtime_error("missing model schema version");
        const int v = sqlite3_column_int(version.value, 0);
        if (v != 0 && v != schema_version)
            throw std::runtime_error("unsupported model schema version");
        sqlite3_finalize(version.value);
        version.value = nullptr;
        if (v == 0) {
            Statement tables;
            check(impl_->db, sqlite3_prepare_v2(impl_->db, "SELECT 1 FROM sqlite_schema LIMIT 1",
                                                -1, &tables.value, nullptr));
            if (sqlite3_step(tables.value) != SQLITE_DONE || application != 0)
                throw std::runtime_error("nonempty unowned model database");
        } else if (application != 1396788556) {
            throw std::runtime_error("unowned model database");
        }
        execute(impl_->db, "PRAGMA max_page_count=256");
        if (v == 0)
            execute(impl_->db, "BEGIN IMMEDIATE; CREATE TABLE models(key BLOB PRIMARY KEY NOT "
                               "NULL, version INTEGER NOT NULL, "
                               "payload BLOB NOT NULL CHECK(length(payload)=744)) WITHOUT ROWID; "
                               "PRAGMA user_version=1; PRAGMA application_id=1396788556; COMMIT;");
        Statement query;
        check(impl_->db,
              sqlite3_prepare_v2(impl_->db, "SELECT key,version,payload FROM models LIMIT 33", -1,
                                 &query.value, nullptr));
        int count = 0;
        for (;;) {
            const int rc = sqlite3_step(query.value);
            if (rc == SQLITE_DONE)
                break;
            if (rc != SQLITE_ROW)
                check(impl_->db, rc);
            if (++count > 32)
                throw std::runtime_error("model row bound exceeded");
            const int length = sqlite3_column_bytes(query.value, 0);
            if (sqlite3_column_type(query.value, 0) != SQLITE_BLOB || length < 1 || length > 4096 ||
                sqlite3_column_type(query.value, 1) != SQLITE_INTEGER ||
                sqlite3_column_int64(query.value, 1) != detail::OnlineModel::feature_version ||
                sqlite3_column_type(query.value, 2) != SQLITE_BLOB ||
                sqlite3_column_bytes(query.value, 2) != payload_size)
                throw std::runtime_error("unsupported or malformed model payload");
            auto state =
                decode(static_cast<const unsigned char*>(sqlite3_column_blob(query.value, 2)));
            if (!detail::OnlineModel::valid_state(state))
                throw std::runtime_error("invalid model statistics");
            impl_->entries.emplace(
                std::string(static_cast<const char*>(sqlite3_column_blob(query.value, 0)), length),
                state);
        }

    } catch (const std::exception& e) {
        impl_->diagnostic = e.what();
        impl_->entries.clear();
        sqlite3_close(impl_->db);
        impl_->db = nullptr;
    }
}
ModelStore::~ModelStore() = default;
std::string_view ModelStore::diagnostic() const noexcept {
    return impl_->diagnostic;
}
std::optional<detail::OnlineModel::State> ModelStore::load(std::string_view key) const {
    const auto found = impl_->entries.find(key);
    if (found == impl_->entries.end())
        return std::nullopt;
    return found->second;
}

bool ModelStore::save(std::string_view key, const detail::OnlineModel::State& state) {
    if (!impl_->db)
        return false;
    try {
        if (key.empty() || key.size() > 4096)
            throw std::runtime_error("invalid model key length");
        if (!detail::OnlineModel::valid_state(state))
            throw std::runtime_error("invalid model statistics");
        const auto bytes = encode(state);
        execute(impl_->db, "BEGIN IMMEDIATE");
        Statement insert;
        check(impl_->db,
              sqlite3_prepare_v2(impl_->db,
                                 "INSERT OR REPLACE INTO models(key,version,payload) VALUES(?,?,?)",
                                 -1, &insert.value, nullptr));
        check(impl_->db, sqlite3_bind_blob(insert.value, 1, key.data(),
                                           static_cast<int>(key.size()), SQLITE_TRANSIENT));
        check(impl_->db, sqlite3_bind_int(insert.value, 2, detail::OnlineModel::feature_version));
        check(impl_->db, sqlite3_bind_blob(insert.value, 3, bytes.data(),
                                           static_cast<int>(bytes.size()), SQLITE_TRANSIENT));
        check(impl_->db, sqlite3_step(insert.value));

        // 当前键必保留，其他键按稳定次序裁剪。 / Keep the current key and trim others
        // deterministically.
        Statement prune;
        check(impl_->db, sqlite3_prepare_v2(impl_->db,
                                            "DELETE FROM models WHERE key IN (SELECT key FROM "
                                            "models WHERE key<>? ORDER BY key LIMIT -1 OFFSET 31)",
                                            -1, &prune.value, nullptr));
        check(impl_->db, sqlite3_bind_blob(prune.value, 1, key.data(), static_cast<int>(key.size()),
                                           SQLITE_TRANSIENT));
        check(impl_->db, sqlite3_step(prune.value));
        execute(impl_->db, "COMMIT");
        impl_->diagnostic.clear();
        return true;
    } catch (const std::exception& e) {
        sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
        impl_->diagnostic = e.what();
        return false;
    }
}
} // namespace same
