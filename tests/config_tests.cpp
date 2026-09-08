/** @file
 * @brief 配置类型与资源限制、忽略规则优先级和输入大小边界。 / Configuration types and limits,
 * ignore-rule precedence and bounded inputs.
 */
#include "same/config.hpp"
#include <chrono>
#include <fstream>
#include <stdexcept>
/// 运行本文件全部回归场景，断言失败即返回非零。 / Run all regressions; assertion failures produce a
/// nonzero exit.
int main() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
                      ("same-config-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root / ".same");
    /** 异常退出也清理测试目录。 / Clean the fixture even during unwinding. */
    struct Cleanup {
        /// 唯一临时目录，由本测试独占。 / Unique directory exclusively owned by this test.
        fs::path path;
        /// 不抛异常，以免掩盖断言。 / Do not mask assertion failures with cleanup errors.
        ~Cleanup() {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    } cleanup{root};
    same::Config::load(root).validate();
    if (!same::Config::load(root).pgo)
        throw std::runtime_error("runtime PGO must default to enabled");
    const auto defaults = same::Config::load(root);
    if (!defaults.telemetry || defaults.telemetry_queue_capacity != 4096 ||
        defaults.telemetry_retention_runs != 64 || defaults.telemetry_max_events != 16384)
        throw std::runtime_error("telemetry defaults");
    {
        std::ofstream file(root / ".same/config.toml");
        file << "workers = 2\nblock_bytes = 1024\nmemory_bytes = 16384\nbackend = 'cpu'\nrehash = "
                "true\npgo = false\n";
    }
    const auto config = same::Config::load(root);
    if (config.workers != 2 || config.metadata_workers != 2 || config.queue_capacity != 4 ||
        !config.rehash || config.backend != "cpu" || config.pgo)
        throw std::runtime_error("config override");
    {
        std::ofstream file(root / ".same/config.toml");
        file << "pgo = true\n";
    }
    if (!same::Config::load(root).pgo)
        throw std::runtime_error("explicit runtime PGO enable ignored");
    // 遥测开关不影响 PGO；数值上下界均可显式配置。
    // Telemetry is independent of PGO; both endpoints of numeric limits are supported.
    for (const auto* settings :
         {"telemetry = false\ntelemetry_queue_capacity = 1\ntelemetry_retention_runs = 1\n"
          "telemetry_max_events = 1\n",
          "telemetry = true\ntelemetry_queue_capacity = 65536\ntelemetry_retention_runs = 4096\n"
          "telemetry_max_events = 1000000\n"}) {
        {
            std::ofstream file(root / ".same/config.toml");
            file << settings;
        }
        const auto loaded = same::Config::load(root);
        const bool maximum = loaded.telemetry;
        if (!loaded.pgo || loaded.telemetry_queue_capacity != (maximum ? 65536 : 1) ||
            loaded.telemetry_retention_runs != (maximum ? 4096 : 1) ||
            loaded.telemetry_max_events != (maximum ? 1000000 : 1))
            throw std::runtime_error("telemetry override or PGO independence");
    }
    {
        std::ofstream file(root / ".same/config.toml");
        file << "workers = 0\n";
    }
    bool rejected = false;
    try {
        same::Config::load(root);
    } catch (...) {
        rejected = true;
    }
    if (!rejected)
        throw std::runtime_error("invalid config accepted");
    for (const auto* invalid : {"workers = 2.0\n",
                                "workers = true\n",
                                "rehash = 1\n",
                                "backend = 1\n",
                                "metadata_workers = 0\n",
                                "metadata_workers = 257\n",
                                "metadata_workers = 2.0\n",
                                "gpu_min_bytes = -1\n",
                                "gpu_min_bytes = 1.5\n",
                                "pgo = 1\n",
                                "pgo = 'false'\n",
                                "pgo = []\n",
                                "workers = {}\n",
                                "unknown = [\n",
                                "telemetry = 1\n",
                                "telemetry = 'false'\n",
                                "telemetry = []\n",
                                "telemetry_queue_capacity = 0\n",
                                "telemetry_queue_capacity = 65537\n",
                                "telemetry_queue_capacity = 1.0\n",
                                "telemetry_retention_runs = 0\n",
                                "telemetry_retention_runs = 4097\n",
                                "telemetry_retention_runs = true\n",
                                "telemetry_max_events = 0\n",
                                "telemetry_max_events = 1000001\n",
                                "telemetry_max_events = '10'\n"}) {
        {
            std::ofstream file(root / ".same/config.toml");
            file << invalid;
        }
        rejected = false;
        try {
            same::Config::load(root);
        } catch (...) {
            rejected = true;
        }
        if (!rejected)
            throw std::runtime_error("wrong config type accepted");
    }
    // 未知字段的值不受应用类型限制；已知字段仍须生效。
    // Unknown values have no application type contract; supported overrides still apply.
    for (const auto* unknown :
         {"future = 1\n", "future = [true, 'x']\n", "gpu_probe_bytes = -1\n",
          "gpu_probe_bytes = 'retired'\n", "[future]\nworkers = 0\npgo = 'ignored'\n"}) {
        {
            std::ofstream file(root / ".same/config.toml");
            file << "workers = 2\npgo = false\n" << unknown;
        }
        const auto loaded = same::Config::load(root);
        if (loaded.workers != 2 || loaded.pgo ||
            loaded.gpu_min_bytes != same::Config{}.gpu_min_bytes)
            throw std::runtime_error("unknown configuration changed supported defaults");
    }
    {
        std::ofstream file(root / ".same/config.toml");
        file << '#' << std::string(64 * 1024, 'x');
    }
    rejected = false;
    try {
        same::Config::load(root);
    } catch (...) {
        rejected = true;
    }
    if (!rejected)
        throw std::runtime_error("oversized config accepted");
    {
        std::ofstream file(root / ".same/ignore");
        file << "# test\n*.tmp\nbuild/\n!build/keep.txt\n/root.txt\na/**/b?.dat\n!.same/state.db\n";
    }
    same::Ignore ignore(root);
    if (!ignore.can_prune("build"))
        throw std::runtime_error("excluded parent must be pruned");
    auto check = [&](const char* path, bool directory, bool expected) {
        if (ignore.matches(path, directory) != expected)
            throw std::runtime_error(path);
    };
    check("x.tmp", false, true);
    check("sub/x.tmp", false, true);
    check("build", true, true);
    check("build", false, false);
    check("build/x.txt", false, true);
    check("build/keep.txt", false, true);
    check("root.txt", false, true);
    check("sub/root.txt", false, false);
    check("a/b1.dat", false, true);
    check("a/x/y/b2.dat", false, true);
    check("a/x/b22.dat", false, false);
    check(".same/state.db", false, true);
    check("normal", false, false);
    // Git-compatible escaping, bracket classes, anchoring and explicit parent reopening.
    // Git 兼容的转义、字符类别、锚定及显式重新纳入父目录。
    {
        std::ofstream file(root / ".same/ignore");
        file << "\xEF\xBB\xBF" << R"(\#literal
\!literal
[a-c].txt
[!a-c].dat
[[:digit:]].log
literal\*.txt
ab**cd
parent/
!parent/
parent/*
!parent/keep
/root-only/
a/**/target
)" << "escaped\\ \ntrailing   \n";
    }
    same::Ignore syntax(root);
    auto syntax_check = [&](const char* path, bool dir, bool expected) {
        if (syntax.matches(path, dir) != expected)
            throw std::runtime_error(std::string("gitignore syntax: ") + path);
    };
    syntax_check("#literal", false, true);
    syntax_check("!literal", false, true);
    syntax_check("trailing", false, true);
    syntax_check("trailing ", false, false);
    syntax_check("escaped ", false, true);
    syntax_check("escaped", false, false);
    syntax_check("b.txt", false, true);
    syntax_check("d.txt", false, false);
    syntax_check("z.dat", false, true);
    syntax_check("b.dat", false, false);
    syntax_check("5.log", false, true);
    syntax_check("x.log", false, false);
    syntax_check("literal*.txt", false, true);
    syntax_check("literalX.txt", false, false);
    syntax_check("abZZcd", false, true);
    syntax_check("ab/x/cd", false, false);
    syntax_check("parent/keep", false, false);
    syntax_check("parent/other", false, true);
    syntax_check("parent/other/keep", false, true);
    syntax_check("root-only/a", false, true);
    syntax_check("sub/root-only/a", false, false);
    syntax_check("a/target", false, true);
    syntax_check("a/b/c/target", false, true);
    syntax_check("nested/.same/state.db", false, true);
    {
        std::ofstream file(root / ".same/ignore");
        file << "build/\n";
    }
    if (!same::Ignore(root).can_prune("build") || same::Ignore(root).can_prune("source"))
        throw std::runtime_error("directory pruning");
    {
        std::ofstream file(root / ".same/ignore");
        for (int i = 0; i < 200; ++i)
            file << "*a";
        file << "z\n";
    }
    if (same::Ignore(root).matches(std::string(400, 'a'), false))
        throw std::runtime_error("adversarial glob mismatch");
    {
        std::ofstream file(root / ".same/ignore");
        for (int i = 0; i < 4097; ++i)
            file << "x\n";
    }
    rejected = false;
    try {
        same::Ignore invalid(root);
    } catch (...) {
        rejected = true;
    }
    if (!rejected)
        throw std::runtime_error("too many rules accepted");
    {
        std::ofstream file(root / ".same/ignore");
        file << std::string(1024 * 1024 + 1, 'x');
    }
    rejected = false;
    try {
        same::Ignore invalid(root);
    } catch (...) {
        rejected = true;
    }
    if (!rejected)
        throw std::runtime_error("oversized ignore accepted");
}
