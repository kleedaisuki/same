#include "same/config.hpp"
#include <chrono>
#include <fstream>
#include <stdexcept>
int main() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("same-config-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root / ".same");
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; fs::remove_all(path, ec); } } cleanup{root};
    same::Config::load(root).validate();
    { std::ofstream file(root / ".same/config.toml"); file << "workers = 2\nblock_bytes = 1024\nmemory_bytes = 16384\nbackend = 'cpu'\nrehash = true\n"; }
    const auto config = same::Config::load(root);
    if (config.workers != 2 || config.queue_capacity != 4 || !config.rehash || config.backend != "cpu") throw std::runtime_error("config override");
    { std::ofstream file(root / ".same/config.toml"); file << "workers = 0\n"; }
    bool rejected = false;
    try { same::Config::load(root); } catch (...) { rejected = true; }
    if (!rejected) throw std::runtime_error("invalid config accepted");
    { std::ofstream file(root / ".same/ignore"); file << "# test\n*.tmp\nbuild/\n!build/keep.txt\n/root.txt\na/**/b?.dat\n!.same/state.db\n"; }
    same::Ignore ignore(root);
    if (ignore.can_prune("build")) throw std::runtime_error("negated children pruned");
    auto check = [&](const char* path, bool directory, bool expected) { if (ignore.matches(path, directory) != expected) throw std::runtime_error(path); };
    check("x.tmp", false, true); check("sub/x.tmp", false, true);
    check("build", true, true); check("build", false, false); check("build/x.txt", false, true);
    check("build/keep.txt", false, false); check("root.txt", false, true); check("sub/root.txt", false, false);
    check("a/b1.dat", false, true); check("a/x/y/b2.dat", false, true); check("a/x/b22.dat", false, false);
    check(".same/state.db", false, true); check("normal", false, false);    { std::ofstream file(root / ".same/ignore"); file << "build/\n"; }
    if (!same::Ignore(root).can_prune("build") || same::Ignore(root).can_prune("source")) throw std::runtime_error("directory pruning");
    { std::ofstream file(root / ".same/ignore"); for (int i = 0; i < 200; ++i) file << "*a"; file << "z\n"; }
    if (same::Ignore(root).matches(std::string(400, 'a'), false)) throw std::runtime_error("adversarial glob mismatch");
    { std::ofstream file(root / ".same/ignore"); for (int i = 0; i < 4097; ++i) file << "x\n"; }
    rejected = false;
    try { same::Ignore invalid(root); } catch (...) { rejected = true; }
    if (!rejected) throw std::runtime_error("too many rules accepted");
    { std::ofstream file(root / ".same/ignore"); file << std::string(1024 * 1024 + 1, 'x'); }
    rejected = false;
    try { same::Ignore invalid(root); } catch (...) { rejected = true; }
    if (!rejected) throw std::runtime_error("oversized ignore accepted");
}
