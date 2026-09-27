#include "same/merge.hpp"
#include "same/files.hpp"
#include "same/model_store.hpp"
#include "same/run_lock.hpp"
#include "same/store.hpp"
#include "same/terminal.hpp"
#include "same/workspace.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <sqlite3.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace same {
namespace {
namespace fs = std::filesystem;

/// 仅供合并展示的计数；每个库各自事务化，重试必须幂等。
/// Merge counters; each database commits separately, so retries must be idempotent.
struct Counts {
    std::uint64_t workspaces{}, model_parts{}, setup_parts{}, cache_imported{};
    std::uint64_t telemetry_runs{}, telemetry_events{}, ignore_rules_added{}, removed{};
};

/// UTF-8 路径的持久形式与已有 state/model 身份一致。 / Persist paths as generic UTF-8.
std::string key(const fs::path& path) {
    const auto bytes = path.generic_u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

/// 从数据库 BLOB 解码路径，禁止隐式本地代码页转换。
/// Decode a persisted UTF-8 path without an implicit locale conversion.
fs::path path_from_key(std::string_view bytes) {
    return fs::path(
        std::u8string_view(reinterpret_cast<const char8_t*>(bytes.data()), bytes.size()));
}

/// 只接受不逃离来源根的相对缓存路径。 / Reject absolute and parent-traversing source keys.
fs::path source_file(const fs::path& root, std::string_view relative) {
    const auto path = path_from_key(relative);
    if (path.empty() || path.is_absolute())
        throw std::runtime_error("invalid relative state cache path");
    for (const auto& component : path)
        if (component == "." || component == "..")
            throw std::runtime_error("traversing state cache path");
    return (root / path).lexically_normal();
}

/// 合并不跟随 .same 链接，也不把陌生 SQLite 文件误认为 same 状态。
/// Never follow linked state or accept special/multiply linked database files.
bool regular_state(const fs::path& path) {
    const auto status = fs::symlink_status(path);
    if (!fs::exists(status))
        return false;
    if (!fs::is_regular_file(status) || is_reparse_point(path) || fs::hard_link_count(path) != 1)
        throw std::runtime_error("unsafe merge state file: " + key(path));
    return true;
}

/// SQLite 主库与边车都必须是普通非链接文件，避免重定向读写。
/// Validate the SQLite main file and sidecars before opening either connection.
bool safe_database(const fs::path& path) {
    const bool exists = regular_state(path);
    for (const auto* suffix : {"-wal", "-shm", "-journal"}) {
        auto sidecar = path;
        sidecar += suffix;
        if (regular_state(sidecar) && !exists)
            throw std::runtime_error("orphan SQLite sidecar in merge workspace");
    }
    return exists;
}

/// 发现真实工作区，按路径稳定排序；.same 内部不继续遍历。
/// Discover real workspaces deterministically without traversing state or links.
std::vector<fs::path> sources(const fs::path& destination, const fs::path& requested,
                              bool recursive) {
    if (!fs::is_directory(requested) || is_reparse_point(requested))
        throw std::runtime_error("merge source must be a real directory");
    const auto root = fs::canonical(requested);
    std::vector<fs::path> result;
    const auto consider = [&](const fs::path& directory) {
        if (directory == destination)
            return;
        const auto state = directory / ".same";
        const auto status = fs::symlink_status(state);
        if (!fs::exists(status))
            return;
        if (!fs::is_directory(status) || is_reparse_point(state))
            throw std::runtime_error("merge refuses linked or non-directory .same");
        result.push_back(directory);
    };
    consider(root);
    if (recursive) {
        for (fs::recursive_directory_iterator it(root), end; it != end; ++it) {
            const auto& entry = *it;
            if (entry.path().filename() == ".same") {
                it.disable_recursion_pending();
                continue;
            }
            if (is_reparse_point(entry.path())) {
                it.disable_recursion_pending();
                continue;
            }
            if (entry.is_directory())
                consider(fs::canonical(entry.path()));
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    if (result.empty())
        throw std::runtime_error("merge found no other .same workspaces");
    return result;
}

/// 外部来源没有可推断的目标子路径；子工作区则相对目标根锚定。
/// External roots have no target subpath; descendant roots anchor at their relative location.
std::string scoped_rule(std::string line, const fs::path& destination, const fs::path& source,
                        bool& external) {
    const auto relative = source.lexically_relative(destination);
    if (relative.empty() || *relative.begin() == "..") {
        external = true;
        return line;
    }
    std::string prefix;
    for (unsigned char byte : key(relative)) {
        if (byte < 32 || byte == 127)
            throw std::runtime_error("cannot scope ignore through control-character path");
        if (byte == '*' || byte == '?' || byte == '[' || byte == ']' || byte == '\\')
            prefix += '\\';
        prefix += static_cast<char>(byte);
    }
    const bool negate = line.starts_with('!');
    if (negate)
        line.erase(0, 1);
    const bool anchored = line.starts_with('/');
    if (anchored)
        line.erase(0, 1);
    auto pattern = line;
    while (!pattern.empty() && pattern.back() == ' ')
        pattern.pop_back();
    if (pattern.ends_with('/'))
        pattern.pop_back();
    const bool anywhere = !anchored && pattern.find('/') == std::string::npos;
    return (negate ? "!" : "") + std::string("/") + prefix + "/" + (anywhere ? "**/" : "") + line;
}

/// 按字节读取至现有 Ignore 的 1 MiB 限制。 / Read within Ignore's existing 1 MiB ceiling.
std::string ignore_text(const fs::path& path) {
    if (!regular_state(path))
        return {};
    if (fs::file_size(path) > 1024 * 1024)
        throw std::runtime_error("ignore file exceeds 1 MiB");
    std::ifstream in(path, std::ios::binary);
    const std::string content(std::istreambuf_iterator<char>{in}, {});
    if (!in.eof() && !in)
        throw std::runtime_error("cannot read merge ignore file");
    return content;
}

/// 仅比较实际规则行，不把注释当规则。 / Compare active rule lines, not comments.
std::vector<std::string> ignore_rules(const std::string& content) {
    std::vector<std::string> rules;
    std::istringstream lines(content);
    std::string line;
    bool first = true;
    while (std::getline(lines, line)) {
        if (first && line.starts_with("\xEF\xBB\xBF"))
            line.erase(0, 3);
        first = false;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line.front() == '#')
            continue;
        rules.push_back(line);
    }
    return rules;
}

/// 先写同目录临时文件再替换；失败时保持旧规则。 / Replace from a sibling temp file.
void replace_ignore(const fs::path& target, const std::string& content) {
    auto temporary = target;
    temporary +=
        ".merge-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    if (fs::exists(fs::symlink_status(temporary)))
        throw std::runtime_error("ignore merge temporary path collision");
    try {
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            out.write(content.data(), static_cast<std::streamsize>(content.size()));
            out.close();
            if (!out)
                throw std::runtime_error("cannot write merged ignore rules");
        }
        fs::rename(temporary, target);
    } catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}

/// 现有配置不动；规则逐条去重，子目录规则加作用域。
/// Preserve configuration and append only missing rules, scoping descendant patterns.
void merge_ignore(const fs::path& destination, const fs::path& source, Counts& counts,
                  std::ostream& diagnostics) {
    const auto target = destination / ".same" / "ignore";
    const auto origin = source / ".same" / "ignore";
    if (!regular_state(origin))
        return;
    auto merged = ignore_text(target);
    auto existing = ignore_rules(merged);
    std::unordered_set<std::string> known(existing.begin(), existing.end());
    bool external = false;
    std::uint64_t added = 0;
    for (auto& rule : ignore_rules(ignore_text(origin))) {
        auto scoped = scoped_rule(std::move(rule), destination, source, external);
        if (!known.insert(scoped).second)
            continue;
        if (!merged.empty() && merged.back() != '\n')
            merged += '\n';
        merged += scoped + '\n';
        ++added;
    }
    if (!added)
        return;
    if (merged.size() > 1024 * 1024 || existing.size() + added > 4096)
        throw std::runtime_error("merged ignore exceeds supported limits");
    replace_ignore(target, merged);
    counts.ignore_rules_added += added;
    if (external)
        diagnostics << "Merge warning: external ignore rules now apply at the destination root; "
                       "review .same/ignore.\n";
}

/// 归档可包含暂时离线的文件；真正复用时仍须完整文件戳匹配。
/// Archive even temporarily absent files; reuse still requires an exact current stamp.
void merge_state(const fs::path& destination, const fs::path& source, Counts& counts) {
    const auto origin = source / ".same" / "state.db";
    if (!safe_database(origin))
        return;
    safe_database(destination / ".same" / "state.db");
    Store input(origin);
    Store output(destination / ".same" / "state.db");
    output.begin_import();
    try {
        const auto import = [&](FileRecord record, const fs::path& path) {
            record.path = key(path.lexically_normal());
            output.save_absolute(record);
            ++counts.cache_imported;
        };
        input.visit_files(
            [&](const FileRecord& record) { import(record, source_file(source, record.path)); });
        input.visit_absolute([&](const FileRecord& record) {
            const auto path = path_from_key(record.path);
            if (!path.is_absolute())
                throw std::runtime_error("non-absolute archive cache path");
            import(record, path);
        });
        output.end_import();
    } catch (...) {
        output.rollback_import();
        throw;
    }
}

/// 来源账本替换而非整库相加；旧来源增量因此不会重复计数。
/// Replace provenance contributions, never add opaque cumulative aggregates twice.
void merge_model(const fs::path& destination, const fs::path& source, Counts& counts) {
    const auto origin = source / ".same" / "model.db";
    if (!safe_database(origin))
        return;
    ModelStore input(origin);
    ModelStore output(destination / ".same" / "model.db");
    if (!input.diagnostic().empty() || !output.diagnostic().empty())
        throw std::runtime_error("merge model store unavailable");
    for (const auto& part : input.contributions()) {
        if (!output.merge_contribution(part))
            throw std::runtime_error("merge model contribution failed: " +
                                     std::string(output.diagnostic()));
        ++counts.model_parts;
    }
    for (const auto& part : input.setup_contributions()) {
        if (!output.merge_setup_contribution(part))
            throw std::runtime_error("merge setup contribution failed: " +
                                     std::string(output.diagnostic()));
        ++counts.setup_parts;
    }
}

/// 独占一个短期 SQLite 连接。 / Own a short-lived SQLite connection.
struct Database {
    sqlite3* value{};
    Database(const fs::path& path, int flags) {
        const auto bytes = path.u8string();
        if (sqlite3_open_v2(reinterpret_cast<const char*>(bytes.c_str()), &value, flags, nullptr) !=
            SQLITE_OK) {
            sqlite3_close(value);
            value = nullptr;
            throw std::runtime_error("cannot open merge telemetry database");
        }
        sqlite3_busy_timeout(value, 5000);
        if (sqlite3_exec(value, "PRAGMA trusted_schema=OFF", nullptr, nullptr, nullptr) !=
            SQLITE_OK) {
            sqlite3_close(value);
            value = nullptr;
            throw std::runtime_error("cannot configure merge telemetry database");
        }
    }
    ~Database() {
        sqlite3_close(value);
    }
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
};

/// 每次 SQL 错误携带当前连接原因。 / Preserve the failing SQLite connection's reason.
void sql(sqlite3* db, const char* statement) {
    if (sqlite3_exec(db, statement, nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db));
}

/// 语句 RAII；查询与复制期间游标始终有界。 / Bound cursors with RAII.
struct Query {
    sqlite3_stmt* value{};
    Query(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &value, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Query() {
        sqlite3_finalize(value);
    }
    Query(const Query&) = delete;
    Query& operator=(const Query&) = delete;
};

/// schema 与所有权在任何写入之前检查。 / Check schema and ownership before writes.
void verify_telemetry(sqlite3* db, bool empty_allowed) {
    Query version(db, "PRAGMA user_version");
    Query owner(db, "PRAGMA application_id");
    if (sqlite3_step(version.value) != SQLITE_ROW || sqlite3_step(owner.value) != SQLITE_ROW)
        throw std::runtime_error("cannot read telemetry ownership");
    const auto v = sqlite3_column_int(version.value, 0);
    const auto app = sqlite3_column_int(owner.value, 0);
    if (empty_allowed && v == 0 && app == 0) {
        Query existing(db, "SELECT count(*) FROM sqlite_schema WHERE name NOT LIKE 'sqlite_%'");
        if (sqlite3_step(existing.value) == SQLITE_ROW &&
            sqlite3_column_int(existing.value, 0) == 0)
            return;
    }
    if (!empty_allowed && v == 1 && app == 1396788564)
        return;
    throw std::runtime_error("unsupported or foreign telemetry database");
}

/// 通用定列复制，列值类型与 BLOB 精度原样保留。 / Copy typed SQLite values exactly.
std::uint64_t copy_rows(sqlite3* source, sqlite3* target, const char* select_sql,
                        const char* insert_sql, std::string_view run_id) {
    Query rows(source, select_sql);
    Query insert(target, insert_sql);
    if (sqlite3_bind_text(rows.value, 1, run_id.data(), static_cast<int>(run_id.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK)
        throw std::runtime_error("cannot bind telemetry run ID");
    std::uint64_t copied = 0;
    for (;;) {
        const int rc = sqlite3_step(rows.value);
        if (rc == SQLITE_DONE)
            break;
        if (rc != SQLITE_ROW)
            throw std::runtime_error(sqlite3_errmsg(source));
        sqlite3_reset(insert.value);
        sqlite3_clear_bindings(insert.value);
        const auto columns = sqlite3_column_count(rows.value);
        for (int i = 0; i < columns; ++i)
            if (sqlite3_bind_value(insert.value, i + 1, sqlite3_column_value(rows.value, i)) !=
                SQLITE_OK)
                throw std::runtime_error(sqlite3_errmsg(target));
        if (sqlite3_step(insert.value) != SQLITE_DONE)
            throw std::runtime_error(sqlite3_errmsg(target));
        ++copied;
    }
    return copied;
}

/// 来源运行不可变，按 run_id 去重；复制四表并保留原事件时序。
/// Deduplicate immutable runs by ID and copy all four related tables transactionally.
void merge_telemetry(const fs::path& destination, const fs::path& source, Counts& counts) {
    const auto origin = source / ".same" / "telemetry.db";
    if (!safe_database(origin))
        return;
    const auto target = destination / ".same" / "telemetry.db";
    const bool existed = safe_database(target);
    Database input(origin, SQLITE_OPEN_READONLY);
    verify_telemetry(input.value, false);
    if (!existed) {
        bool owned_empty_target = false;
        try {
            // 新目标复制失败时删除不完整库，重试不能被残留 schema 卡住。
            // Remove a failed first copy so a retry is not blocked by a partial schema.
            Database output(target, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
            verify_telemetry(output.value, true);
            owned_empty_target = true;
            auto* backup = sqlite3_backup_init(output.value, "main", input.value, "main");
            if (!backup)
                throw std::runtime_error("cannot start telemetry copy");
            const int copied = sqlite3_backup_step(backup, -1);
            const int finished = sqlite3_backup_finish(backup);
            if (copied != SQLITE_DONE || finished != SQLITE_OK)
                throw std::runtime_error("cannot copy telemetry database");
            Query runs(output.value, "SELECT count(*) FROM runs");
            Query events(output.value, "SELECT count(*) FROM events");
            if (sqlite3_step(runs.value) != SQLITE_ROW || sqlite3_step(events.value) != SQLITE_ROW)
                throw std::runtime_error("cannot count copied telemetry");
            counts.telemetry_runs +=
                static_cast<std::uint64_t>(sqlite3_column_int64(runs.value, 0));
            counts.telemetry_events +=
                static_cast<std::uint64_t>(sqlite3_column_int64(events.value, 0));
        } catch (...) {
            if (owned_empty_target) {
                for (const auto* suffix : {"-wal", "-shm", "-journal", ""}) {
                    auto artifact = target;
                    artifact += suffix;
                    if (regular_state(artifact))
                        fs::remove(artifact);
                }
            }
            throw;
        }
        return;
    }
    Database output(target, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    verify_telemetry(output.value, false);
    sql(output.value, "PRAGMA foreign_keys=ON; BEGIN IMMEDIATE");
    try {
        Query runs(input.value, "SELECT run_id FROM runs ORDER BY rowid");
        Query exists(output.value, "SELECT 1 FROM runs WHERE run_id=?1");
        for (;;) {
            const int rc = sqlite3_step(runs.value);
            if (rc == SQLITE_DONE)
                break;
            if (rc != SQLITE_ROW)
                throw std::runtime_error(sqlite3_errmsg(input.value));
            const auto* id = reinterpret_cast<const char*>(sqlite3_column_text(runs.value, 0));
            const auto length = sqlite3_column_bytes(runs.value, 0);
            if (!id || !length)
                throw std::runtime_error("invalid source telemetry run ID");
            const std::string_view run_id(id, static_cast<std::size_t>(length));
            sqlite3_reset(exists.value);
            sqlite3_clear_bindings(exists.value);
            sqlite3_bind_text(exists.value, 1, id, length, SQLITE_TRANSIENT);
            if (sqlite3_step(exists.value) == SQLITE_ROW)
                continue;
            counts.telemetry_runs +=
                copy_rows(input.value, output.value, "SELECT * FROM runs WHERE run_id=?1",
                          "INSERT INTO runs VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", run_id);
            counts.telemetry_events += copy_rows(
                input.value, output.value,
                "SELECT run_id,type,name,severity,message,backend,worker,span_id,"
                "parent_span_id,time_ns,duration_ns,bytes,value,unit,truncated "
                "FROM events WHERE run_id=?1 ORDER BY event_id",
                "INSERT INTO events(run_id,type,name,severity,message,backend,worker,span_id,"
                "parent_span_id,time_ns,duration_ns,bytes,value,unit,truncated) "
                "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                run_id);
            copy_rows(input.value, output.value, "SELECT * FROM metrics WHERE run_id=?1",
                      "INSERT INTO metrics VALUES(?,?,?,?)", run_id);
            copy_rows(input.value, output.value, "SELECT * FROM parameters WHERE run_id=?1",
                      "INSERT INTO parameters VALUES(?,?,?,?)", run_id);
        }
        sql(output.value, "COMMIT");
    } catch (...) {
        sqlite3_exec(output.value, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}

/// 和 scan summary 一样保留结构与纯文本语义，颜色仅加强层级。
/// Mirror scan summary's plain-text hierarchy; color only reinforces it.
void report(const Counts& counts, MergeOptions options, std::ostream& out) {
    if (!options.summary)
        return;
    if (!options.pretty) {
        out << "merge_workspaces=" << counts.workspaces
            << " merge_model_contributions=" << counts.model_parts
            << " merge_setup_contributions=" << counts.setup_parts
            << " merge_cache_imported=" << counts.cache_imported
            << " merge_telemetry_runs=" << counts.telemetry_runs
            << " merge_telemetry_events=" << counts.telemetry_events
            << " merge_ignore_rules_added=" << counts.ignore_rules_added
            << " merge_sources_removed=" << counts.removed << '\n';
        return;
    }
    const auto cyan = options.color ? "\033[1;36m" : "";
    const auto green = options.color ? "\033[1;32m" : "";
    const auto reset = options.color ? "\033[0m" : "";
    out << '\n'
        << cyan << "Merge summary" << reset << "\n"
        << "========================================================================\n\n"
        << cyan << "[ Sources ]" << reset << "\n"
        << "  Workspaces          " << green << counts.workspaces << reset << " imported | "
        << counts.removed << " removed\n\n"
        << cyan << "[ State cache ]" << reset << "\n"
        << "  Absolute entries    " << counts.cache_imported
        << " imported (checked against file stamps only when reused)\n\n"
        << cyan << "[ Online routing model ]" << reset << "\n"
        << "  Contributions       " << counts.model_parts << " model | " << counts.setup_parts
        << " setup considered (revision-deduplicated)\n\n"
        << cyan << "[ Workspace settings ]" << reset << "\n"
        << "  Configuration       preserved\n"
        << "  Ignore              " << counts.ignore_rules_added << " missing rule(s) appended\n\n"
        << cyan << "[ Telemetry ]" << reset << "\n"
        << "  Imported            " << counts.telemetry_runs << " runs | "
        << counts.telemetry_events << " events\n";
}
} // namespace

int merge_workspaces(const fs::path& destination, const fs::path& source, MergeOptions options,
                     std::ostream& diagnostics) {
    const auto target = fs::canonical(destination);
    const auto inputs = sources(target, source, options.recursive);
    const auto state = target / ".same";
    if (fs::exists(fs::symlink_status(state))) {
        if (!fs::is_directory(fs::symlink_status(state)) || is_reparse_point(state))
            throw std::runtime_error("destination .same must be a real directory");
    } else
        fs::create_directory(state);
    Counts counts;
    std::vector<std::pair<fs::path, WorkspaceStateSnapshot>> cleanup;
    {
        WorkspaceLock target_workspace(target);
        RunLock target_lock(state / "run.lock");
        for (const auto& input : inputs) {
            WorkspaceLock source_workspace(input);
            RunLock source_lock(input / ".same" / "run.lock");
            merge_model(target, input, counts);
            merge_state(target, input, counts);
            merge_telemetry(target, input, counts);
            merge_ignore(target, input, counts, diagnostics);
            if (options.move)
                cleanup.emplace_back(input, snapshot_workspace_state(input));
            ++counts.workspaces;
        }
    }
    for (const auto& [input, expected] : cleanup) {
        clean_workspace_if_unchanged(input, expected);
        ++counts.removed;
    }
    report(counts, options, diagnostics);
    return 0;
}
} // namespace same
