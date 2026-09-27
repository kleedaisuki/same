/** @file
 * @brief 独立训练的失败原子性与文件边界。 / Independent training failure and file boundaries.
 */
#include "same/application.hpp"
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sqlite3.h>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
namespace fs = std::filesystem;

/// 独立于 Release 断言设置，保留具体失败原因。 / Assertions retain failures in Release.
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

/// 只在仓库根目录 .temp 下创建有界语料。 / Keep fixtures beneath the repository .temp.
fs::path fixture_root() {
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::path(SAME_TEST_ROOT) / ".temp" / ("training-test-" + std::to_string(tick));
}

/// 零设备配额使三后端训练确定失败，不依赖测试机器 GPU。
/// A one-byte device budget makes three-backend training fail independently of hardware.
void unavailable_backend() {
    const auto base = fixture_root();
    const auto workspace = base / "workspace";
    const auto corpus = base / "corpus";
    fs::create_directories(workspace);
    fs::create_directories(corpus);
    try {
        std::array<char, 8192> bytes{};
        for (unsigned i = 0; i < 4; ++i) {
            bytes[i] = static_cast<char>(i + 1);
            std::ofstream file(corpus / ("sample-" + std::to_string(i)), std::ios::binary);
            file.write(bytes.data(), static_cast<std::streamsize>(1024 * (i + 1)));
            require(bool(file), "cannot create training fixture");
        }
        same::Config config;
        config.workers = 1;
        config.metadata_workers = 1;
        config.queue_capacity = 4;
        config.gpu_min_bytes = 1;
        config.device_memory_bytes = 1;
        std::ostringstream output, diagnostics;
        bool failed = false;
        try {
            same::train(workspace, corpus, config, output, diagnostics, 4, 32768);
        } catch (const std::runtime_error& error) {
            failed =
                std::string(error.what()).find("training backend unavailable") != std::string::npos;
        }
        require(failed, "unavailable accelerator was accepted as a training label");
        require(output.str().empty(), "failed training announced success");
        require(!fs::exists(workspace / ".same" / "state.db"),
                "training modified the scan digest cache");
        sqlite3* db = nullptr;
        const auto database = workspace / ".same" / "model.db";
        require(sqlite3_open_v2(database.string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) ==
                    SQLITE_OK,
                "cannot inspect model database");
        sqlite3_stmt* query = nullptr;
        require(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM models", -1, &query, nullptr) ==
                    SQLITE_OK,
                "cannot inspect model rows");
        require(sqlite3_step(query) == SQLITE_ROW && sqlite3_column_int(query, 0) == 0,
                "failed training persisted partial model data");
        sqlite3_finalize(query);
        sqlite3_close(db);
        const auto telemetry = workspace / ".same" / "telemetry.db";
        require(sqlite3_open_v2(telemetry.string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) ==
                    SQLITE_OK,
                "training failure did not write telemetry");
        require(sqlite3_prepare_v2(db,
                                   "SELECT COUNT(*) FROM runs WHERE command='train' AND "
                                   "status='failed'",
                                   -1, &query, nullptr) == SQLITE_OK,
                "cannot inspect training run status");
        require(sqlite3_step(query) == SQLITE_ROW && sqlite3_column_int(query, 0) == 1,
                "failed training run was not finalized");
        sqlite3_finalize(query);
        require(sqlite3_prepare_v2(db,
                                   "SELECT COUNT(*) FROM metrics WHERE "
                                   "name='train.model_saved_keys' AND value=0",
                                   -1, &query, nullptr) == SQLITE_OK,
                "cannot inspect training final metrics");
        require(sqlite3_step(query) == SQLITE_ROW && sqlite3_column_int(query, 0) == 1,
                "failure lost its complete final metrics");
        sqlite3_finalize(query);
        require(sqlite3_prepare_v2(db,
                                   "SELECT COUNT(*) FROM events WHERE name='train.measure' AND "
                                   "severity='error'",
                                   -1, &query, nullptr) == SQLITE_OK,
                "cannot inspect failed training stage");
        require(sqlite3_step(query) == SQLITE_ROW && sqlite3_column_int(query, 0) == 1,
                "failure lost its measure-stage error span");
        sqlite3_finalize(query);
        require(sqlite3_prepare_v2(db,
                                   "SELECT COUNT(*) FROM logs WHERE name='train.failed' AND "
                                   "severity='error'",
                                   -1, &query, nullptr) == SQLITE_OK,
                "cannot inspect training failure log");
        require(sqlite3_step(query) == SQLITE_ROW && sqlite3_column_int(query, 0) == 1,
                "failure lost its terminal error log");
        sqlite3_finalize(query);
        sqlite3_close(db);
        const auto quiet_workspace = base / "quiet-workspace";
        fs::create_directories(quiet_workspace);
        config.telemetry = false;
        bool quiet_failed = false;
        try {
            same::train(quiet_workspace, corpus, config, output, diagnostics, 4, 32768);
        } catch (const std::runtime_error&) {
            quiet_failed = true;
        }
        require(quiet_failed, "no-telemetry changed training failure semantics");
        require(!fs::exists(quiet_workspace / ".same" / "telemetry.db"),
                "disabled training telemetry touched its database");
        for (unsigned i = 0; i < 4; ++i)
            require(fs::file_size(corpus / ("sample-" + std::to_string(i))) == 1024 * (i + 1),
                    "training altered corpus files");
    } catch (...) {
        fs::remove_all(base);
        throw;
    }
    fs::remove_all(base);
}

/// 两目录共用一个选样上限，且重叠目录在创建工作区前拒绝。
/// Multiple corpora share one selection ceiling; overlap fails before workspace side effects.
void multiple_corpora() {
    const auto base = fixture_root();
    const auto workspace = base / "workspace";
    const auto first = base / "first";
    const auto second = base / "second";
    fs::create_directories(workspace);
    fs::create_directories(first / "child");
    fs::create_directories(second);
    try {
        for (const auto& root : {first, second})
            for (int i = 0; i < 2; ++i) {
                std::ofstream out(root / ("sample-" + std::to_string(i)), std::ios::binary);
                out << std::string(4096 + i * 1024, static_cast<char>('a' + i));
            }
        same::Config config;
        config.workers = 1;
        config.metadata_workers = 1;
        config.queue_capacity = 4;
        config.gpu_min_bytes = 1;
        config.device_memory_bytes = 1;
        std::ostringstream output, diagnostics;
        bool overlap = false;
        try {
            same::train(workspace, std::vector<fs::path>{first, first / "child"}, config, output,
                        diagnostics, 4, 32768, {});
        } catch (const std::runtime_error& error) {
            overlap = std::string(error.what()).find("overlap") != std::string::npos;
        }
        require(overlap && !fs::exists(workspace / ".same"),
                "overlapping corpora mutated workspace");
        bool failed_device = false;
        try {
            same::train(workspace, std::vector<fs::path>{first, second}, config, output,
                        diagnostics, 4, 32768, {});
        } catch (const std::runtime_error& error) {
            failed_device =
                std::string(error.what()).find("training backend unavailable") != std::string::npos;
        }
        require(failed_device, "multi-corpus selection did not reach backend validation");
        sqlite3* db{};
        const auto telemetry = workspace / ".same" / "telemetry.db";
        require(sqlite3_open_v2(telemetry.string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) ==
                    SQLITE_OK,
                "cannot inspect multi-corpus telemetry");
        sqlite3_stmt* row{};
        require(sqlite3_prepare_v2(db,
                                   "SELECT value FROM metrics WHERE name='train.selected_files'",
                                   -1, &row, nullptr) == SQLITE_OK,
                "cannot inspect global selection count");
        require(sqlite3_step(row) == SQLITE_ROW && sqlite3_column_int(row, 0) == 4,
                "multi-corpus training did not select from both directories");
        sqlite3_finalize(row);
        sqlite3_close(db);
    } catch (...) {
        fs::remove_all(base);
        throw;
    }
    fs::remove_all(base);
}
} // namespace

int main() {
    unavailable_backend();
    multiple_corpora();
}
