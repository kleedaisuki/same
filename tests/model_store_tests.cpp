/** @file
 * 有界模型状态的跨实例与损坏测试。 / Cross-instance and corruption tests for bounded model state.
 */
#include "same/model_store.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <sqlite3.h>
#include <stdexcept>

namespace {
/// 检查发布构建契约。 / Assert contracts in release builds.
void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
/// 独立连接修改测试数据库。 / Modify the test database through an independent connection.
void sql(const std::filesystem::path& path, const char* text) {
    sqlite3* db{};
    const auto name = path.u8string();
    require(sqlite3_open(reinterpret_cast<const char*>(name.c_str()), &db) == SQLITE_OK,
            "open fixture");
    const int rc = sqlite3_exec(db, text, nullptr, nullptr, nullptr);
    sqlite3_close(db);
    require(rc == SQLITE_OK, "fixture SQL");
}
} // namespace
int main() {
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("same-model-test-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directories(directory);
        const auto path = std::filesystem::canonical(directory) / "model.db";
        same::detail::OnlineModel::State state{};
        state[0].xtx.fill(1);
        state[0].xty.fill(2);
        state[0].minimum.fill(1);
        state[0].maximum.fill(1);
        state[0].yty = 4;
        state[0].weight = 1;
        state[0].samples = 1;
        {
            same::ModelStore db(path);
            require(db.diagnostic().empty(), "initialization");
            require(!db.load("machine-a"), "invented state");
            require(db.save("machine-a", state), "save valid statistics");
            auto invalid = state;
            invalid[0].weight = std::numeric_limits<double>::quiet_NaN();
            require(!db.save("machine-a", invalid), "NaN accepted");
            require(!db.save(std::string(4097, 'x'), state), "oversized key accepted");
        }
        {
            same::ModelStore db(path);
            require(db.load("machine-a").has_value(), "reopen state missing");
            require(db.load("machine-a")->at(0).xty == state[0].xty &&
                        db.load("machine-a")->at(0).samples == 1,
                    "roundtrip statistics changed");
            require(!db.load("machine-b"), "key isolation");
            sql(path, "CREATE TRIGGER reject_write BEFORE INSERT ON models BEGIN SELECT "
                      "RAISE(ABORT,'test'); END");
            require(!db.save("machine-a", state), "failed transaction accepted");
        }
        {
            same::ModelStore db(path);
            require(db.load("machine-a").has_value(), "failed write destroyed previous state");
        }
        sql(path, "DROP TRIGGER reject_write; UPDATE models SET version=999");
        {
            same::ModelStore db(path);
            require(!db.load("machine-a") && !db.diagnostic().empty(),
                    "unknown feature version accepted");
        }
        sql(path, "UPDATE models SET version=1; PRAGMA user_version=999");
        {
            same::ModelStore db(path);
            require(!db.load("machine-a") && !db.diagnostic().empty(), "unknown schema accepted");
        }
        sql(path, "PRAGMA user_version=1; PRAGMA ignore_check_constraints=ON; UPDATE models SET "
                  "payload=x'00'");
        {
            same::ModelStore db(path);
            require(!db.load("machine-a") && !db.diagnostic().empty(),
                    "truncated payload accepted");
        }
        const auto foreign = std::filesystem::canonical(directory) / "foreign.db";
        sql(foreign, "CREATE TABLE precious(value TEXT); INSERT INTO precious VALUES('keep')");
        const auto read_bytes = [](const std::filesystem::path& file) {
            std::ifstream in(file, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in), {});
        };
        const auto before = read_bytes(foreign);
        {
            same::ModelStore db(foreign);
            require(!db.diagnostic().empty() && !db.save("x", state), "foreign database accepted");
        }
        require(read_bytes(foreign) == before, "foreign database modified");
        const auto bounded = std::filesystem::canonical(directory) / "bounded.db";
        {
            same::ModelStore db(bounded);
            for (int i = 0; i < 40; ++i)
                require(db.save("key-" + std::to_string(i), state), "bounded save");
        }
        {
            same::ModelStore db(bounded);
            require(db.diagnostic().empty() && db.load("key-39").has_value(), "bounded reopen");
        }
        std::filesystem::remove_all(directory);
        std::cout << "model store tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        std::filesystem::remove_all(directory);
        return 1;
    }
}
