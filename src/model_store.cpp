#include "same/model_store.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <map>
#include <sqlite3.h>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace same {
namespace {
/// 格式与大小固定，不依赖本机字节序或结构填充。 / Fixed format independent of endianness/padding.
constexpr int schema_version = 4;
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
/// 严格验证初始化历史的数值域。 / Strictly validate setup history's numeric domain.
bool valid_setup(const SetupHistory& h) {
    return h.samples > 0 && std::isfinite(h.last_ms) && std::isfinite(h.mean_ms) &&
           std::isfinite(h.max_ms) && h.last_ms > 0 && h.mean_ms > 0 && h.max_ms <= 1e9 &&
           h.last_ms <= h.max_ms && h.mean_ms <= h.max_ms;
}
/// 初始化历史沿用固定 32 字节载荷。 / Setup history retains its fixed 32-byte payload.
std::vector<unsigned char> encode_setup(const SetupHistory& history) {
    std::vector<unsigned char> bytes;
    bytes.reserve(32);
    append(bytes, std::bit_cast<std::uint64_t>(history.last_ms));
    append(bytes, std::bit_cast<std::uint64_t>(history.mean_ms));
    append(bytes, std::bit_cast<std::uint64_t>(history.max_ms));
    append(bytes, history.samples);
    return bytes;
}
/// 解码来源账本及聚合表的同一表示。 / Decode the shared ledger/materialization format.
SetupHistory decode_setup(const unsigned char* bytes) {
    SetupHistory h;
    h.last_ms = std::bit_cast<double>(take(bytes));
    h.mean_ms = std::bit_cast<double>(take(bytes));
    h.max_ms = std::bit_cast<double>(take(bytes));
    h.samples = take(bytes);
    return h;
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
/// 存储种类决定表与版本列，避免调用方组合不一致的 SQL。
/// Record kind selects the table and version column, preventing mismatched SQL combinations.
enum class RecordKind { model, setup };
/// 聚合表写入处于调用方事务中；来源账本裁剪同步执行。
/// Materialized writes run inside the caller transaction and prune orphan ledger rows.
void write_payload(sqlite3* db, std::string_view key, const unsigned char* payload,
                   std::size_t payload_bytes, RecordKind kind) {
    const bool model = kind == RecordKind::model;
    const char* insert_sql =
        model ? "INSERT OR REPLACE INTO models(key,version,payload) VALUES(?,?,?)"
              : "INSERT OR REPLACE INTO setups(key,payload) VALUES(?,?)";
    const char* prune_sql = model ? "DELETE FROM models WHERE key IN (SELECT key FROM models "
                                    "WHERE key<>? ORDER BY key LIMIT -1 OFFSET 31)"
                                  : "DELETE FROM setups WHERE key IN (SELECT key FROM setups "
                                    "WHERE key<>? ORDER BY key LIMIT -1 OFFSET 31)";
    Statement insert;
    check(db, sqlite3_prepare_v2(db, insert_sql, -1, &insert.value, nullptr));
    check(db, sqlite3_bind_blob(insert.value, 1, key.data(), static_cast<int>(key.size()),
                                SQLITE_TRANSIENT));
    const int payload_column = model ? 3 : 2;
    if (model)
        check(db, sqlite3_bind_int(insert.value, 2, detail::OnlineModel::feature_version));
    check(db, sqlite3_bind_blob(insert.value, payload_column, payload,
                                static_cast<int>(payload_bytes), SQLITE_TRANSIENT));
    check(db, sqlite3_step(insert.value));

    // 保留当前键，其余按稳定次序裁剪。 / Keep the current key; trim others deterministically.
    Statement prune;
    check(db, sqlite3_prepare_v2(db, prune_sql, -1, &prune.value, nullptr));
    check(db, sqlite3_bind_blob(prune.value, 1, key.data(), static_cast<int>(key.size()),
                                SQLITE_TRANSIENT));
    check(db, sqlite3_step(prune.value));
    execute(db, model ? "DELETE FROM model_parts WHERE key NOT IN (SELECT key FROM models)"
                      : "DELETE FROM setup_parts WHERE key NOT IN (SELECT key FROM setups)");
}
/// 固定 BLOB 列复制为拥有值，供来源账本遍历。 / Copy a BLOB column into owned storage.
std::string blob(sqlite3_stmt* row, int column) {
    const auto size = sqlite3_column_bytes(row, column);
    const auto* data = static_cast<const char*>(sqlite3_column_blob(row, column));
    return size ? std::string(data, static_cast<std::size_t>(size)) : std::string{};
}
/// 路径标识后接 NUL 与随机实例号；同路径重建工作区不会复用旧来源。
/// Path plus NUL and a random instance token prevents identity reuse after --move.
std::string new_source_id(std::string_view root) {
    std::array<unsigned char, 16> random{};
    sqlite3_randomness(static_cast<int>(random.size()), random.data());
    std::string result(root);
    result.push_back('\0');
    result.append(reinterpret_cast<const char*>(random.data()), random.size());
    if (result.size() > 4096)
        throw std::runtime_error("model source identity exceeds 4096 bytes");
    return result;
}
/// 逐行验证来源状态，避免合并损坏的模型。 / Validate each contribution before merging.
std::vector<ModelContribution> model_parts(sqlite3* db, std::string_view key = {}) {
    const char* sql = key.empty()
                          ? "SELECT source,key,revision,attenuation,payload FROM model_parts "
                            "ORDER BY source,key"
                          : "SELECT source,key,revision,attenuation,payload FROM model_parts "
                            "WHERE key=?1 ORDER BY source";
    Statement query;
    check(db, sqlite3_prepare_v2(db, sql, -1, &query.value, nullptr));
    if (!key.empty())
        check(db, sqlite3_bind_blob(query.value, 1, key.data(), static_cast<int>(key.size()),
                                    SQLITE_TRANSIENT));
    std::vector<ModelContribution> result;
    for (;;) {
        const int rc = sqlite3_step(query.value);
        if (rc == SQLITE_DONE)
            break;
        if (rc != SQLITE_ROW)
            check(db, rc);
        ModelContribution part;
        part.source = blob(query.value, 0);
        part.key = blob(query.value, 1);
        const auto revision = sqlite3_column_int64(query.value, 2);
        part.attenuation = sqlite3_column_double(query.value, 3);
        if (part.source.empty() || part.key.empty() || part.key.size() > 4096 || revision <= 0 ||
            !std::isfinite(part.attenuation) || part.attenuation <= 0 || part.attenuation > 1 ||
            sqlite3_column_bytes(query.value, 4) != payload_size)
            throw std::runtime_error("invalid model contribution");
        part.revision = static_cast<std::uint64_t>(revision);
        part.state = decode(static_cast<const unsigned char*>(sqlite3_column_blob(query.value, 4)));
        if (!detail::OnlineModel::valid_state(part.state))
            throw std::runtime_error("invalid model contribution statistics");
        result.push_back(std::move(part));
    }
    return result;
}
/// 同一主键上的 replace 不增加来源数；外部 revision 必须已验证。
/// Replace a contribution at its stable source/key identity.
void put_model_part(sqlite3* db, const ModelContribution& part) {
    const auto bytes = encode(part.state);
    Statement insert;
    check(db, sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO model_parts VALUES(?1,?2,?3,?4,?5)",
                                 -1, &insert.value, nullptr));
    check(db, sqlite3_bind_blob(insert.value, 1, part.source.data(),
                                static_cast<int>(part.source.size()), SQLITE_TRANSIENT));
    check(db, sqlite3_bind_blob(insert.value, 2, part.key.data(), static_cast<int>(part.key.size()),
                                SQLITE_TRANSIENT));
    check(db, sqlite3_bind_int64(insert.value, 3, static_cast<sqlite3_int64>(part.revision)));
    check(db, sqlite3_bind_double(insert.value, 4, part.attenuation));
    check(db, sqlite3_bind_blob(insert.value, 5, bytes.data(), static_cast<int>(bytes.size()),
                                SQLITE_TRANSIENT));
    check(db, sqlite3_step(insert.value));
}
/// 聚合只从来源账本重建，杜绝旧来源残留和重复相加。
/// Rematerialize solely from provenance so replaced sources cannot leave stale evidence.
detail::OnlineModel::State materialize_model(sqlite3* db, std::string_view key) {
    detail::OnlineModel::State aggregate{};
    for (const auto& part : model_parts(db, key)) {
        detail::OnlineModel effective;
        if (!effective.initialize(part.state, part.attenuation) ||
            !detail::OnlineModel::merge(aggregate, effective.prior()))
            throw std::runtime_error("model contribution aggregate rejected");
    }
    const auto bytes = encode(aggregate);
    write_payload(db, key, bytes.data(), bytes.size(), RecordKind::model);
    return aggregate;
}
/// 初始化历史来源使用相同的 revision 协议。 / Setup histories use the same revision protocol.
std::vector<SetupContribution> setup_parts(sqlite3* db, std::string_view key = {}) {
    const char* sql = key.empty() ? "SELECT source,key,revision,payload FROM setup_parts "
                                    "ORDER BY source,key"
                                  : "SELECT source,key,revision,payload FROM setup_parts "
                                    "WHERE key=?1 ORDER BY source";
    Statement query;
    check(db, sqlite3_prepare_v2(db, sql, -1, &query.value, nullptr));
    if (!key.empty())
        check(db, sqlite3_bind_blob(query.value, 1, key.data(), static_cast<int>(key.size()),
                                    SQLITE_TRANSIENT));
    std::vector<SetupContribution> result;
    for (;;) {
        const int rc = sqlite3_step(query.value);
        if (rc == SQLITE_DONE)
            break;
        if (rc != SQLITE_ROW)
            check(db, rc);
        SetupContribution part;
        part.source = blob(query.value, 0);
        part.key = blob(query.value, 1);
        const auto revision = sqlite3_column_int64(query.value, 2);
        if (part.source.empty() || part.key.empty() || part.key.size() > 4096 || revision <= 0 ||
            sqlite3_column_bytes(query.value, 3) != 32)
            throw std::runtime_error("invalid setup contribution");
        part.revision = static_cast<std::uint64_t>(revision);
        part.history =
            decode_setup(static_cast<const unsigned char*>(sqlite3_column_blob(query.value, 3)));
        if (!valid_setup(part.history))
            throw std::runtime_error("invalid setup contribution history");
        result.push_back(std::move(part));
    }
    return result;
}
/// 保存完整来源初始化历史。 / Save one complete source setup history.
void put_setup_part(sqlite3* db, const SetupContribution& part) {
    const auto bytes = encode_setup(part.history);
    Statement insert;
    check(db, sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO setup_parts VALUES(?1,?2,?3,?4)", -1,
                                 &insert.value, nullptr));
    check(db, sqlite3_bind_blob(insert.value, 1, part.source.data(),
                                static_cast<int>(part.source.size()), SQLITE_TRANSIENT));
    check(db, sqlite3_bind_blob(insert.value, 2, part.key.data(), static_cast<int>(part.key.size()),
                                SQLITE_TRANSIENT));
    check(db, sqlite3_bind_int64(insert.value, 3, static_cast<sqlite3_int64>(part.revision)));
    check(db, sqlite3_bind_blob(insert.value, 4, bytes.data(), static_cast<int>(bytes.size()),
                                SQLITE_TRANSIENT));
    check(db, sqlite3_step(insert.value));
}
/// 样本数加权均值与保守最大值；last 取稳定来源顺序中的末项。
/// Weighted mean and conservative maximum; last follows stable source order.
SetupHistory materialize_setup(sqlite3* db, std::string_view key) {
    SetupHistory aggregate{};
    for (const auto& part : setup_parts(db, key)) {
        const auto& h = part.history;
        if (h.samples > UINT64_MAX - aggregate.samples)
            throw std::runtime_error("setup contribution count overflow");
        const auto total = aggregate.samples + h.samples;
        aggregate.mean_ms = (aggregate.mean_ms * static_cast<double>(aggregate.samples) +
                             h.mean_ms * static_cast<double>(h.samples)) /
                            static_cast<double>(total);
        aggregate.samples = total;
        aggregate.last_ms = h.last_ms;
        aggregate.max_ms = std::max(aggregate.max_ms, h.max_ms);
    }
    if (!valid_setup(aggregate))
        throw std::runtime_error("setup contribution aggregate rejected");
    const auto bytes = encode_setup(aggregate);
    write_payload(db, key, bytes.data(), bytes.size(), RecordKind::setup);
    return aggregate;
}
} // namespace
/// 冷路径连接；关闭自动回滚未提交事务。 / Cold-path connection; closing rolls back transactions.
struct ModelStore::Impl {
    sqlite3* db{};
    std::string diagnostic;
    /// 仅用于迁移旧路径身份与检测复制工作区。 / Root identity for legacy migration/copy detection.
    std::string root_source;
    /// 当前存储根的稳定绝对身份。 / Stable absolute identity of this store's workspace.
    std::string local_source;
    /// 启动快照支持并发只读查询。 / Startup snapshot supports concurrent read-only lookup.
    std::map<std::string, detail::OnlineModel::State, std::less<>> entries;
    /// 初始化历史冷路径快照。 / Cold-path setup history snapshot.
    std::map<std::string, SetupHistory, std::less<>> setups;
    ~Impl() {
        sqlite3_close(db);
    }
};
ModelStore::ModelStore(const std::filesystem::path& path) : impl_(std::make_unique<Impl>()) {
    try {
        const auto source_path = std::filesystem::canonical(path.parent_path()).generic_u8string();
        impl_->root_source.assign(reinterpret_cast<const char*>(source_path.data()),
                                  source_path.size());
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
        if (v < 0 || v > schema_version)
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
        execute(impl_->db, "PRAGMA max_page_count=16384");
        if (v == 0)
            execute(impl_->db, "BEGIN IMMEDIATE; CREATE TABLE models(key BLOB PRIMARY KEY NOT "
                               "NULL, version INTEGER NOT NULL, "
                               "payload BLOB NOT NULL CHECK(length(payload)=744)) WITHOUT ROWID; "
                               "PRAGMA user_version=1; PRAGMA application_id=1396788556; COMMIT;");
        if (v < 2)
            execute(impl_->db,
                    "BEGIN IMMEDIATE; CREATE TABLE setups(key BLOB PRIMARY KEY NOT NULL, "
                    "payload BLOB NOT NULL CHECK(length(payload)=32)) WITHOUT ROWID; PRAGMA "
                    "user_version=2; COMMIT;");
        if (v < 3) {
            execute(impl_->db,
                    "BEGIN IMMEDIATE;"
                    "CREATE TABLE model_parts(source BLOB NOT NULL,key BLOB NOT NULL,"
                    "revision INTEGER NOT NULL CHECK(revision>0),attenuation REAL NOT NULL "
                    "CHECK(attenuation>0 AND attenuation<=1),payload BLOB NOT NULL "
                    "CHECK(length(payload)=744),PRIMARY KEY(source,key)) WITHOUT ROWID;"
                    "CREATE TABLE setup_parts(source BLOB NOT NULL,key BLOB NOT NULL,"
                    "revision INTEGER NOT NULL CHECK(revision>0),payload BLOB NOT NULL "
                    "CHECK(length(payload)=32),PRIMARY KEY(source,key)) WITHOUT ROWID;");
            try {
                Statement models;
                check(impl_->db,
                      sqlite3_prepare_v2(impl_->db,
                                         "INSERT INTO model_parts SELECT ?1,key,1,1.0,payload "
                                         "FROM models",
                                         -1, &models.value, nullptr));
                check(impl_->db, sqlite3_bind_blob(models.value, 1, impl_->root_source.data(),
                                                   static_cast<int>(impl_->root_source.size()),
                                                   SQLITE_TRANSIENT));
                check(impl_->db, sqlite3_step(models.value));
                Statement setups;
                check(impl_->db,
                      sqlite3_prepare_v2(impl_->db,
                                         "INSERT INTO setup_parts SELECT ?1,key,1,payload "
                                         "FROM setups",
                                         -1, &setups.value, nullptr));
                check(impl_->db, sqlite3_bind_blob(setups.value, 1, impl_->root_source.data(),
                                                   static_cast<int>(impl_->root_source.size()),
                                                   SQLITE_TRANSIENT));
                check(impl_->db, sqlite3_step(setups.value));
                execute(impl_->db, "PRAGMA user_version=3; COMMIT;");
            } catch (...) {
                sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
                throw;
            }
        }
        if (v < 4) {
            const auto identity = new_source_id(impl_->root_source);
            execute(impl_->db,
                    "BEGIN IMMEDIATE; CREATE TABLE model_meta(id INTEGER PRIMARY KEY CHECK(id=1),"
                    "root BLOB NOT NULL,local_source BLOB NOT NULL);");
            try {
                Statement meta;
                check(impl_->db,
                      sqlite3_prepare_v2(impl_->db, "INSERT INTO model_meta VALUES(1,?1,?2)", -1,
                                         &meta.value, nullptr));
                check(impl_->db, sqlite3_bind_blob(meta.value, 1, impl_->root_source.data(),
                                                   static_cast<int>(impl_->root_source.size()),
                                                   SQLITE_TRANSIENT));
                check(impl_->db,
                      sqlite3_bind_blob(meta.value, 2, identity.data(),
                                        static_cast<int>(identity.size()), SQLITE_TRANSIENT));
                check(impl_->db, sqlite3_step(meta.value));
                for (const char* table : {"model_parts", "setup_parts"}) {
                    const auto sql =
                        std::string("UPDATE ") + table + " SET source=?1 WHERE source=?2";
                    Statement update;
                    check(impl_->db,
                          sqlite3_prepare_v2(impl_->db, sql.c_str(), -1, &update.value, nullptr));
                    check(impl_->db,
                          sqlite3_bind_blob(update.value, 1, identity.data(),
                                            static_cast<int>(identity.size()), SQLITE_TRANSIENT));
                    check(impl_->db, sqlite3_bind_blob(update.value, 2, impl_->root_source.data(),
                                                       static_cast<int>(impl_->root_source.size()),
                                                       SQLITE_TRANSIENT));
                    check(impl_->db, sqlite3_step(update.value));
                }
                execute(impl_->db, "PRAGMA user_version=4; COMMIT;");
                impl_->local_source = identity;
            } catch (...) {
                sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
                throw;
            }
        } else {
            Statement meta;
            check(impl_->db, sqlite3_prepare_v2(impl_->db,
                                                "SELECT root,local_source FROM model_meta WHERE "
                                                "id=1",
                                                -1, &meta.value, nullptr));
            if (sqlite3_step(meta.value) != SQLITE_ROW)
                throw std::runtime_error("missing model workspace identity");
            const auto owner_root = blob(meta.value, 0);
            impl_->local_source = blob(meta.value, 1);
            sqlite3_finalize(meta.value);
            meta.value = nullptr;
            if (impl_->local_source.empty())
                throw std::runtime_error("invalid model workspace identity");
            if (owner_root != impl_->root_source) {
                impl_->local_source = new_source_id(impl_->root_source);
                execute(impl_->db, "BEGIN IMMEDIATE");
                try {
                    Statement update;
                    check(impl_->db, sqlite3_prepare_v2(impl_->db,
                                                        "UPDATE model_meta SET root=?1,"
                                                        "local_source=?2 WHERE id=1",
                                                        -1, &update.value, nullptr));
                    check(impl_->db, sqlite3_bind_blob(update.value, 1, impl_->root_source.data(),
                                                       static_cast<int>(impl_->root_source.size()),
                                                       SQLITE_TRANSIENT));
                    check(impl_->db, sqlite3_bind_blob(update.value, 2, impl_->local_source.data(),
                                                       static_cast<int>(impl_->local_source.size()),
                                                       SQLITE_TRANSIENT));
                    check(impl_->db, sqlite3_step(update.value));
                    execute(impl_->db, "COMMIT");
                } catch (...) {
                    sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
                    throw;
                }
            }
        }
        // 仅在确认归属与版本后设置日志；学习状态允许断电丢失最近提交。
        // Change journaling only after ownership/version checks; recent learning may be lost on
        // power failure.
        execute(
            impl_->db,
            "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA wal_autocheckpoint=64;");
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

        Statement setups;
        check(impl_->db, sqlite3_prepare_v2(impl_->db, "SELECT key,payload FROM setups LIMIT 33",
                                            -1, &setups.value, nullptr));
        count = 0;
        for (;;) {
            const int rc = sqlite3_step(setups.value);
            if (rc == SQLITE_DONE)
                break;
            if (rc != SQLITE_ROW)
                check(impl_->db, rc);
            const int length = sqlite3_column_bytes(setups.value, 0);
            if (++count > 32 || sqlite3_column_type(setups.value, 0) != SQLITE_BLOB || length < 1 ||
                length > 4096 || sqlite3_column_type(setups.value, 1) != SQLITE_BLOB ||
                sqlite3_column_bytes(setups.value, 1) != 32)
                throw std::runtime_error("malformed setup history");
            const auto* bytes =
                static_cast<const unsigned char*>(sqlite3_column_blob(setups.value, 1));
            SetupHistory h;
            h.last_ms = std::bit_cast<double>(take(bytes));
            h.mean_ms = std::bit_cast<double>(take(bytes));
            h.max_ms = std::bit_cast<double>(take(bytes));
            h.samples = take(bytes);
            if (!valid_setup(h))
                throw std::runtime_error("invalid setup statistics");
            impl_->setups.emplace(
                std::string(static_cast<const char*>(sqlite3_column_blob(setups.value, 0)), length),
                h);
        }

    } catch (const std::exception& e) {
        impl_->diagnostic = e.what();
        impl_->entries.clear();
        impl_->setups.clear();
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
        execute(impl_->db, "BEGIN IMMEDIATE");
        std::uint64_t revision = 1;
        for (const auto& part : model_parts(impl_->db, key))
            if (part.source == impl_->local_source) {
                revision = part.revision + 1;
                break;
            }
        if (revision > static_cast<std::uint64_t>(INT64_MAX))
            throw std::runtime_error("model source revision overflow");
        put_model_part(impl_->db, {impl_->local_source, std::string(key), revision, 1, state});
        const auto aggregate = materialize_model(impl_->db, key);
        execute(impl_->db, "COMMIT");
        impl_->entries[std::string(key)] = aggregate;
        impl_->diagnostic.clear();
        return true;
    } catch (const std::exception& e) {
        sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
        impl_->diagnostic = e.what();
        return false;
    }
}
bool ModelStore::save_delta(std::string_view key, const detail::OnlineModel::State& delta,
                            double decay) {
    if (!impl_->db)
        return false;
    try {
        if (key.empty() || key.size() > 4096 || !detail::OnlineModel::valid_state(delta) ||
            !std::isfinite(decay) || decay <= 0 || decay > 1)
            throw std::runtime_error("invalid model delta/key/decay");
        execute(impl_->db, "BEGIN IMMEDIATE");
        bool local = false;
        for (auto part : model_parts(impl_->db, key)) {
            const auto next_attenuation = part.attenuation * decay;
            detail::OnlineModel effective;
            if (!effective.initialize(part.state, next_attenuation))
                throw std::runtime_error("model source decay rejected");
            if (part.source == impl_->local_source) {
                part.state = effective.prior();
                if (part.revision == static_cast<std::uint64_t>(INT64_MAX) ||
                    !detail::OnlineModel::merge(part.state, delta))
                    throw std::runtime_error("local model delta rejected");
                ++part.revision;
                part.attenuation = 1;
                local = true;
            } else
                part.attenuation = next_attenuation;
            put_model_part(impl_->db, part);
        }
        if (!local)
            put_model_part(impl_->db, {impl_->local_source, std::string(key), 1, 1, delta});
        const auto aggregate = materialize_model(impl_->db, key);
        execute(impl_->db, "COMMIT");
        impl_->entries[std::string(key)] = aggregate;
        impl_->diagnostic.clear();
        return true;
    } catch (const std::exception& e) {
        sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
        impl_->diagnostic = e.what();
        return false;
    }
}
std::vector<ModelContribution> ModelStore::contributions() const {
    if (!impl_->db)
        throw std::runtime_error("model store unavailable");
    return model_parts(impl_->db);
}
bool ModelStore::merge_contribution(const ModelContribution& contribution) {
    if (!impl_->db)
        return false;
    try {
        if (contribution.source.empty() || contribution.source.size() > 4096 ||
            contribution.key.empty() || contribution.key.size() > 4096 || !contribution.revision ||
            contribution.revision > static_cast<std::uint64_t>(INT64_MAX) ||
            !std::isfinite(contribution.attenuation) || contribution.attenuation <= 0 ||
            contribution.attenuation > 1 || !detail::OnlineModel::valid_state(contribution.state))
            throw std::runtime_error("invalid imported model contribution");
        execute(impl_->db, "BEGIN IMMEDIATE");
        bool newer = true;
        if (contribution.source == impl_->local_source)
            newer = false;
        for (const auto& existing : model_parts(impl_->db, contribution.key))
            if (existing.source == contribution.source) {
                newer = contribution.revision > existing.revision;
                break;
            }
        std::optional<detail::OnlineModel::State> aggregate;
        if (newer) {
            const auto separator = contribution.source.find('\0');
            if (separator != std::string::npos &&
                contribution.source.substr(0, separator) != impl_->root_source) {
                Statement legacy;
                check(impl_->db,
                      sqlite3_prepare_v2(impl_->db,
                                         "DELETE FROM model_parts WHERE source=?1 AND key=?2", -1,
                                         &legacy.value, nullptr));
                check(impl_->db, sqlite3_bind_blob(legacy.value, 1, contribution.source.data(),
                                                   static_cast<int>(separator), SQLITE_TRANSIENT));
                check(impl_->db, sqlite3_bind_blob(legacy.value, 2, contribution.key.data(),
                                                   static_cast<int>(contribution.key.size()),
                                                   SQLITE_TRANSIENT));
                check(impl_->db, sqlite3_step(legacy.value));
            }
            put_model_part(impl_->db, contribution);
            aggregate = materialize_model(impl_->db, contribution.key);
        }
        execute(impl_->db, "COMMIT");
        if (aggregate)
            impl_->entries[contribution.key] = *aggregate;
        impl_->diagnostic.clear();
        return true;
    } catch (const std::exception& e) {
        sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
        impl_->diagnostic = e.what();
        return false;
    }
}
std::optional<SetupHistory> ModelStore::load_setup(std::string_view key) const {
    const auto it = impl_->setups.find(key);
    if (it == impl_->setups.end())
        return std::nullopt;
    return it->second;
}
bool ModelStore::save_setup(std::string_view key, const SetupHistory& history) {
    if (!impl_->db)
        return false;
    try {
        if (key.empty() || key.size() > 4096 || !valid_setup(history))
            throw std::runtime_error("invalid setup history/key");
        execute(impl_->db, "BEGIN IMMEDIATE");
        std::uint64_t revision = 1;
        for (const auto& part : setup_parts(impl_->db, key))
            if (part.source == impl_->local_source) {
                revision = part.revision + 1;
                break;
            }
        if (revision > static_cast<std::uint64_t>(INT64_MAX))
            throw std::runtime_error("setup source revision overflow");
        put_setup_part(impl_->db, {impl_->local_source, std::string(key), revision, history});
        const auto aggregate = materialize_setup(impl_->db, key);
        execute(impl_->db, "COMMIT");
        impl_->setups[std::string(key)] = aggregate;
        impl_->diagnostic.clear();
        return true;
    } catch (const std::exception& e) {
        sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
        impl_->diagnostic = e.what();
        return false;
    }
}
bool ModelStore::save_setup_delta(std::string_view key, const SetupHistory& delta) {
    if (!impl_->db)
        return false;
    try {
        if (key.empty() || key.size() > 4096 || !valid_setup(delta))
            throw std::runtime_error("invalid setup delta/key");
        execute(impl_->db, "BEGIN IMMEDIATE");
        auto local = SetupContribution{impl_->local_source, std::string(key), 1, delta};
        for (const auto& part : setup_parts(impl_->db, key))
            if (part.source == impl_->local_source) {
                if (part.revision == static_cast<std::uint64_t>(INT64_MAX) ||
                    delta.samples > UINT64_MAX - part.history.samples)
                    throw std::runtime_error("setup source overflow");
                const auto total = part.history.samples + delta.samples;
                local.revision = part.revision + 1;
                local.history.mean_ms =
                    (part.history.mean_ms * static_cast<double>(part.history.samples) +
                     delta.mean_ms * static_cast<double>(delta.samples)) /
                    static_cast<double>(total);
                local.history.samples = total;
                local.history.max_ms = std::max(part.history.max_ms, delta.max_ms);
                break;
            }
        put_setup_part(impl_->db, local);
        const auto aggregate = materialize_setup(impl_->db, key);
        execute(impl_->db, "COMMIT");
        impl_->setups[std::string(key)] = aggregate;
        impl_->diagnostic.clear();
        return true;
    } catch (const std::exception& e) {
        sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
        impl_->diagnostic = e.what();
        return false;
    }
}
std::vector<SetupContribution> ModelStore::setup_contributions() const {
    if (!impl_->db)
        throw std::runtime_error("model store unavailable");
    return setup_parts(impl_->db);
}
bool ModelStore::merge_setup_contribution(const SetupContribution& contribution) {
    if (!impl_->db)
        return false;
    try {
        if (contribution.source.empty() || contribution.source.size() > 4096 ||
            contribution.key.empty() || contribution.key.size() > 4096 || !contribution.revision ||
            contribution.revision > static_cast<std::uint64_t>(INT64_MAX) ||
            !valid_setup(contribution.history))
            throw std::runtime_error("invalid imported setup contribution");
        execute(impl_->db, "BEGIN IMMEDIATE");
        bool newer = true;
        if (contribution.source == impl_->local_source)
            newer = false;
        for (const auto& existing : setup_parts(impl_->db, contribution.key))
            if (existing.source == contribution.source) {
                newer = contribution.revision > existing.revision;
                break;
            }
        std::optional<SetupHistory> aggregate;
        if (newer) {
            const auto separator = contribution.source.find('\0');
            if (separator != std::string::npos &&
                contribution.source.substr(0, separator) != impl_->root_source) {
                Statement legacy;
                check(impl_->db,
                      sqlite3_prepare_v2(impl_->db,
                                         "DELETE FROM setup_parts WHERE source=?1 AND key=?2", -1,
                                         &legacy.value, nullptr));
                check(impl_->db, sqlite3_bind_blob(legacy.value, 1, contribution.source.data(),
                                                   static_cast<int>(separator), SQLITE_TRANSIENT));
                check(impl_->db, sqlite3_bind_blob(legacy.value, 2, contribution.key.data(),
                                                   static_cast<int>(contribution.key.size()),
                                                   SQLITE_TRANSIENT));
                check(impl_->db, sqlite3_step(legacy.value));
            }
            put_setup_part(impl_->db, contribution);
            aggregate = materialize_setup(impl_->db, contribution.key);
        }
        execute(impl_->db, "COMMIT");
        if (aggregate)
            impl_->setups[contribution.key] = *aggregate;
        impl_->diagnostic.clear();
        return true;
    } catch (const std::exception& e) {
        sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
        impl_->diagnostic = e.what();
        return false;
    }
}

} // namespace same
