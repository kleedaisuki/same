/** @file
 * 工作区合并的来源替换、缓存范围与安全删除回归。
 * Workspace merge regressions for source replacement, cache scope and safe cleanup.
 */
#include "same/application.hpp"
#include "same/config.hpp"
#include "same/merge.hpp"
#include "same/model_store.hpp"
#include "same/run_lock.hpp"
#include "same/workspace.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sqlite3.h>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
namespace fs = std::filesystem;

/// Release 构建也保留断言。 / Assertions remain active in Release.
void require(bool okay, const char* message) {
    if (!okay)
        throw std::runtime_error(message);
}

/// 测试文件永远位于仓库 .temp。 / Keep all fixtures beneath repository .temp.
fs::path fixture() {
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::path(SAME_TEST_ROOT) / ".temp" / ("merge-test-" + std::to_string(tick));
}

/// 创建内容不同而长度相同的样本。 / Create same-size samples with distinct content.
void file(const fs::path& path, char value) {
    std::ofstream out(path, std::ios::binary);
    out << std::string(8192, value);
    require(bool(out), "cannot create merge fixture file");
}

/// 本轮扫描至少形成一个 CPU 模型观测和一个遥测运行。
/// One scan creates a CPU model observation and a telemetry run.
void scan(const fs::path& root, bool rehash = false) {
    same::Config config;
    config.workers = 1;
    config.metadata_workers = 1;
    config.queue_capacity = 4;
    config.gpu_min_bytes = 1;
    config.backend = "cpu";
    config.rehash = rehash;
    std::ostringstream out, err;
    same::OutputOptions options;
    options.summary = false;
    options.recursive = false;
    require(same::run(root, config, out, err, options) == 0, "fixture scan failed");
    require(out.str().empty(), "single-file scan invented duplicates");
}

/// 精确查询来源账本与聚合样本数。 / Inspect provenance and aggregate sample counts.
std::uint64_t number(const fs::path& path, const char* sql) {
    sqlite3* db{};
    const auto name = path.u8string();
    require(sqlite3_open_v2(reinterpret_cast<const char*>(name.c_str()), &db, SQLITE_OPEN_READONLY,
                            nullptr) == SQLITE_OK,
            "cannot inspect merge database");
    sqlite3_stmt* row{};
    require(sqlite3_prepare_v2(db, sql, -1, &row, nullptr) == SQLITE_OK,
            "cannot prepare merge query");
    require(sqlite3_step(row) == SQLITE_ROW, "merge query returned no row");
    const auto value = static_cast<std::uint64_t>(sqlite3_column_int64(row, 0));
    sqlite3_finalize(row);
    sqlite3_close(db);
    return value;
}

/// 固定模型载荷中 CPU 样本数位于首块末尾，按小端读取。
/// CPU sample count occupies the end of the first fixed-format backend block.
std::uint64_t model_samples(const fs::path& path) {
    sqlite3* db{};
    const auto name = path.u8string();
    require(sqlite3_open_v2(reinterpret_cast<const char*>(name.c_str()), &db, SQLITE_OPEN_READONLY,
                            nullptr) == SQLITE_OK,
            "cannot inspect aggregate model");
    sqlite3_stmt* row{};
    require(sqlite3_prepare_v2(db, "SELECT payload FROM models LIMIT 1", -1, &row, nullptr) ==
                SQLITE_OK,
            "cannot query aggregate model");
    require(sqlite3_step(row) == SQLITE_ROW && sqlite3_column_bytes(row, 0) == 744,
            "invalid aggregate model payload");
    const auto* bytes = static_cast<const unsigned char*>(sqlite3_column_blob(row, 0));
    std::uint64_t samples = 0;
    for (unsigned i = 0; i < 8; ++i)
        samples |= std::uint64_t(bytes[240 + i]) << (8 * i);
    sqlite3_finalize(row);
    sqlite3_close(db);
    return samples;
}

/// 验证重复合并不会放大同一来源，但较新的来源贡献会替换旧值。
/// Repeated import is idempotent, while a newer source revision replaces old evidence.
void merge_lifecycle() {
    const auto base = fixture();
    const auto source = base / "source";
    const auto target = base / "target";
    fs::create_directories(source / ".same");
    fs::create_directories(target / ".same");
    try {
        file(source / "a.bin", 'a');
        file(target / "b.bin", 'b');
        {
            std::ofstream(source / ".same" / "ignore") << "# source rule\n*.tmp\n";
            std::ofstream(target / ".same" / "config.toml") << "# keep target config\n";
        }
        scan(source);
        scan(target);
        std::ostringstream report;
        same::MergeOptions options;
        options.summary = true;
        require(same::merge_workspaces(target, source, options, report) == 0, "first merge failed");
        require(report.str().find("merge_telemetry_runs=1") != std::string::npos,
                "first merge omitted telemetry");
        const auto model = target / ".same" / "model.db";
        const auto state = target / ".same" / "state.db";
        const auto telemetry = target / ".same" / "telemetry.db";
        require(number(model, "SELECT count(*) FROM model_parts") == 2,
                "model source ledger omitted a workspace");
        require(model_samples(model) == 2, "first merge lost independent model evidence");
        require(number(state, "SELECT count(*) FROM absolute_cache") == 2,
                "absolute cache did not retain both directories");
        require(number(telemetry, "SELECT count(*) FROM runs") == 2,
                "telemetry runs were not merged");
        require(fs::exists(target / ".same" / "ignore"), "missing ignore was not copied");
        std::ifstream config(target / ".same" / "config.toml");
        std::string config_line;
        std::getline(config, config_line);
        require(config_line == "# keep target config", "target configuration was overwritten");
        report.str({});
        report.clear();
        require(same::merge_workspaces(target, source, options, report) == 0,
                "repeat merge failed");
        require(number(model, "SELECT count(*) FROM model_parts") == 2 &&
                    number(telemetry, "SELECT count(*) FROM runs") == 2 &&
                    model_samples(model) == 2,
                "repeat merge duplicated provenance or telemetry");
        file(source / "a.bin", 'c');
        scan(source);
        require(same::merge_workspaces(target, source, options, report) == 0,
                "new source revision merge failed");
        require(number(model, "SELECT max(revision) FROM model_parts") == 2 &&
                    number(telemetry, "SELECT count(*) FROM runs") == 3 &&
                    model_samples(model) == 3,
                "new source contribution or run not imported");
        scan(target, true);
        require(model_samples(model) == 4,
                "target-local update discarded an imported model contribution");
        require(number(state, "SELECT count(*) FROM files") == 1,
                "external cache entered current scan result scope");
        {
            same::RunLock held(source / ".same" / "run.lock");
            options.move = true;
            bool locked = false;
            try {
                same::merge_workspaces(target, source, options, report);
            } catch (const std::exception&) {
                locked = true;
            }
            require(locked && fs::exists(source / ".same"),
                    "active source lock did not protect --move");
        }
        require(same::merge_workspaces(target, source, options, report) == 0,
                "guarded move failed");
        require(model_samples(model) == 4,
                "same source revision was recounted after target-local learning");
        require(!fs::exists(source / ".same") && fs::exists(source / "a.bin"),
                "move removed input content or retained source state");
        scan(source);
        options.move = false;
        require(same::merge_workspaces(target, source, options, report) == 0,
                "recreated source workspace merge failed");
        require(model_samples(model) == 5 && number(model, "SELECT count(*) FROM model_parts") == 3,
                "recreated path reused an old model-source identity");
    } catch (const std::exception& error) {
        std::cerr << "merge fixture failed: " << error.what() << " at " << base << '\n';
        throw;
    }
    fs::remove_all(base);
}

/// 递归移动两个来源，且快照变化时不删除源工作区。
/// Recursive move handles two workspaces, and changed snapshots prevent cleanup.
void recursive_and_changed_source() {
    const auto base = fixture();
    const auto parent = base / "parent";
    const auto child = parent / "child";
    const auto target = base / "target";
    fs::create_directories(parent / ".same");
    fs::create_directories(child / ".same");
    fs::create_directories(target);
    try {
        file(parent / "parent.bin", 'p');
        file(child / "child.bin", 'q');
        scan(parent);
        scan(child);
        std::ostringstream report;
        same::MergeOptions options;
        options.recursive = true;
        options.move = true;
        options.summary = true;
        require(same::merge_workspaces(target, parent, options, report) == 0,
                "recursive move failed");
        require(report.str().find("merge_workspaces=2") != std::string::npos &&
                    report.str().find("merge_sources_removed=2") != std::string::npos,
                "recursive merge omitted a source");
        require(!fs::exists(parent / ".same") && !fs::exists(child / ".same") &&
                    fs::exists(parent / "parent.bin") && fs::exists(child / "child.bin"),
                "recursive move removed user content or retained imported state");
        fs::create_directory(parent / ".same");
        std::ofstream(parent / ".same" / "ignore") << "initial\n";
        same::WorkspaceStateSnapshot before;
        {
            same::RunLock lock(parent / ".same" / "run.lock");
            before = same::snapshot_workspace_state(parent);
        }
        std::ofstream(parent / ".same" / "ignore") << "changed\n";
        bool refused = false;
        try {
            same::clean_workspace_if_unchanged(parent, before);
        } catch (const std::exception&) {
            refused = true;
        }
        require(refused && fs::exists(parent / ".same"),
                "changed source state was removed after snapshot mismatch");
    } catch (const std::exception& error) {
        std::cerr << "recursive fixture failed: " << error.what() << " at " << base << '\n';
        throw;
    }
    fs::remove_all(base);
}

/// 子来源规则转换成目标根下的限定规则，不误伤其他目录。
/// Descendant rules are scoped beneath their source relative to the destination.
void scoped_ignore() {
    const auto base = fixture();
    const auto source = base / "nested";
    fs::create_directories(source / ".same");
    fs::create_directories(base / ".same");
    try {
        std::ofstream(base / ".same" / "ignore") << "# destination\n";
        std::ofstream(source / ".same" / "ignore") << "*.tmp\n!keep.tmp\n/build/\nfoo/bar\n";
        std::ostringstream report;
        require(same::merge_workspaces(base, source, {.summary = true}, report) == 0,
                "scoped ignore merge failed");
        same::Ignore rules(base);
        require(rules.matches("nested/a.tmp", false) && rules.matches("nested/deep/a.tmp", false) &&
                    !rules.matches("other/a.tmp", false) &&
                    !rules.matches("nested/keep.tmp", false) &&
                    rules.matches("nested/build/child", false) &&
                    !rules.matches("nested/deep/build/child", false) &&
                    !rules.matches("other/build/child", false) &&
                    rules.matches("nested/foo/bar", false),
                "merged ignore rules lost their descendant scope");
        std::ifstream in(base / ".same" / "ignore");
        const std::string before(std::istreambuf_iterator<char>{in}, {});
        require(same::merge_workspaces(base, source, {.summary = true}, report) == 0,
                "repeat ignore merge failed");
        std::ifstream again(base / ".same" / "ignore");
        const std::string after(std::istreambuf_iterator<char>{again}, {});
        require(before == after, "repeat merge duplicated ignore rules");
    } catch (const std::exception& error) {
        std::cerr << "ignore fixture failed: " << error.what() << " at " << base << '\n';
        throw;
    }
    fs::remove_all(base);
}

/// 父工作区扫描子目录时，导入的绝对缓存免重哈希但结果仍在当前范围。
/// A parent scan reuses an imported absolute digest without widening its result scope.
void absolute_cache_reuse() {
    const auto base = fixture();
    const auto child = base / "child";
    fs::create_directories(child / ".same");
    fs::create_directories(base / ".same");
    try {
        file(child / "payload.bin", 'p');
        scan(child);
        std::ostringstream report;
        require(same::merge_workspaces(base, child, {.summary = true}, report) == 0,
                "cannot import child digest archive");
        same::Config config;
        config.workers = 1;
        config.metadata_workers = 1;
        config.backend = "cpu";
        config.pgo = false;
        config.telemetry = false;
        std::ostringstream out, err;
        same::OutputOptions options;
        options.recursive = true;
        require(same::run(base, config, out, err, options) == 0,
                "parent scan after cache import failed");
        require(err.str().find("scanned=1 hashed=0 cached=1") != std::string::npos,
                "matching imported absolute digest was not reused");
        require(number(base / ".same" / "state.db", "SELECT count(*) FROM files") == 1,
                "cache import widened active scan records");
    } catch (const std::exception& error) {
        std::cerr << "cache fixture failed: " << error.what() << " at " << base << '\n';
        throw;
    }
    fs::remove_all(base);
}

/// 复制工作区保留旧来源证据，但新的本地训练必须获得新实例身份。
/// A copied workspace keeps old evidence but forks a fresh local source identity.
void copied_model_identity() {
    const auto base = fixture();
    const auto first = base / "first" / ".same";
    const auto copy = base / "copy" / ".same";
    const auto target = base / "target" / ".same";
    fs::create_directories(first);
    fs::create_directories(copy);
    fs::create_directories(target);
    try {
        same::detail::OnlineModel model;
        require(model.observe(same::BackendKind::cpu, {8192, 4096, 0}, 1.0),
                "cannot create model contribution fixture");
        const auto delta = model.delta();
        {
            same::ModelStore db(first / "model.db");
            require(db.diagnostic().empty() && db.save("key", delta),
                    "cannot save first model contribution");
        }
        fs::copy_file(first / "model.db", copy / "model.db");
        {
            same::ModelStore db(copy / "model.db");
            require(db.diagnostic().empty() && db.save_delta("key", delta, 0.9),
                    "copied workspace did not fork local model identity");
            require(db.contributions().size() == 2,
                    "copied workspace reused original contribution identity");
        }
        same::ModelStore result(target / "model.db");
        same::ModelStore original(first / "model.db");
        same::ModelStore cloned(copy / "model.db");
        for (const auto& part : original.contributions())
            require(result.merge_contribution(part), "cannot import original contribution");
        for (const auto& part : cloned.contributions())
            require(result.merge_contribution(part), "cannot import copied contribution");
        require(result.contributions().size() == 2 && result.load("key") &&
                    result.load("key")->at(0).samples == 2,
                "copy import double-counted original provenance");
    } catch (const std::exception& error) {
        std::cerr << "copy fixture failed: " << error.what() << " at " << base << '\n';
        throw;
    }
    fs::remove_all(base);
}
} // namespace

int main() {
    try {
        merge_lifecycle();
        recursive_and_changed_source();
        scoped_ignore();
        absolute_cache_reuse();
        copied_model_identity();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
