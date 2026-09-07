#include "same/application.hpp"
#include "same/terminal.hpp"
#include <iostream>
#include <string_view>

/// Apply CLI overrides after loading local configuration; preserve machine-readable redirected
/// stdout. 加载本地配置后应用命令行覆盖；重定向标准输出保留机器可读结果。 Return 2 on exceptions,
/// including partial scans; duplicates are a successful result. 异常（含未完成扫描）返回
/// 2；找到重复文件仍属于成功。
int main(int argc, char** argv) {
    try {
        bool rehash = false, cpu = false, unique_files = false;
        auto color_mode = same::ColorMode::automatic;
        std::string_view format = "auto";
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i];
            if (arg == "--help" || arg == "-h") {
                std::cout << "same 0.1.0 - exact duplicate files in the working directory\n"
                             "Usage: same [--rehash] [--cpu] [--color=auto|always|never] "
                             "[--format=auto|pretty|tsv] [--unique-files]\n"
                             "  --rehash  ignore cached hashes for this scan\n"
                             "  --cpu     force the CPU backend\n"
                             "  --unique-files show unmatched paths (TSV: group 0)\n"
                             "  --color=MODE  auto honors NO_COLOR, TERM=dumb and redirection\n"
                             "  --format=MODE auto uses pretty on terminals, legacy TSV otherwise\n"
                             "Configuration: .same/config.toml; ignore rules: .same/ignore\n"
                             "TSV: group-number<TAB>quoted-relative-path, duplicates only. Pretty: "
                             "SAME; UNIQUE is opt-in. Statistics: stderr.\n";
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
            else if (arg == "--unique-files")
                unique_files = true;
            else if (arg.starts_with("--color="))
                color_mode = same::parse_color(arg.substr(8));
            else if (arg.starts_with("--format=")) {
                format = arg.substr(9);
                if (format != "auto" && format != "pretty" && format != "tsv")
                    throw std::runtime_error("invalid output format: " + std::string(format));
            } else
                throw std::runtime_error("unknown argument: " + std::string(arg));
        }
        const auto root = std::filesystem::current_path();
        auto config = same::Config::load(root);
        if (rehash)
            config.rehash = true;
        if (cpu)
            config.backend = "cpu";
        same::TerminalColor color(color_mode);
        const bool pretty = format == "pretty" || (format == "auto" && same::stdout_terminal());
        same::TerminalColor diagnostic_color(color_mode, same::TerminalStream::diagnostics);
        const bool pretty_profile =
            format == "pretty" ||
            (format == "auto" && same::is_terminal(same::TerminalStream::diagnostics));
        return same::run(
            root, config, std::cout, std::cerr,
            {pretty, color.enabled(), unique_files, diagnostic_color.enabled(), pretty_profile});
    } catch (const std::exception& error) {
        std::cerr << "same: " << error.what() << '\n';
        return 2;
    }
}
