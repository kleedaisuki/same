#include "same/application.hpp"
#include <iostream>
#include <string_view>

/// Apply CLI overrides after loading local configuration; keep stdout machine-readable.
/// 加载本地配置后应用命令行覆盖；标准输出仅包含机器可读结果。
/// Return 2 on exceptions, including partial scans; duplicates are a successful result.
/// 异常（含未完成扫描）返回 2；找到重复文件仍属于成功。
int main(int argc, char** argv) {
    try {
        bool rehash = false, cpu = false;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i];
            if (arg == "--help" || arg == "-h") {
                std::cout
                    << "same 0.1.0 - exact duplicate files in the working directory\n"
                       "Usage: same [--rehash] [--cpu]\n"
                       "  --rehash  ignore cached hashes for this scan\n"
                       "  --cpu     force the CPU backend\n"
                       "Configuration: .same/config.toml; ignore rules: .same/ignore\n"
                       "Output: group-number<TAB>quoted-relative-path, one member per line.\n";
                return 0;
            }
            if (arg == "--version") {
                std::cout << "same 0.1.0\n";
                return 0;
            }
            if (arg == "--rehash")
                rehash = true;
            else if (arg == "--cpu")
                cpu = true;
            else
                throw std::runtime_error("unknown argument: " + std::string(arg));
        }
        const auto root = std::filesystem::current_path();
        auto config = same::Config::load(root);
        if (rehash)
            config.rehash = true;
        if (cpu)
            config.backend = "cpu";
        return same::run(root, config, std::cout, std::cerr);
    } catch (const std::exception& error) {
        std::cerr << "same: " << error.what() << '\n';
        return 2;
    }
}
