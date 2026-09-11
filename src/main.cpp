#include "same/application.hpp"
#include "same/terminal.hpp"
#include "same/workspace.hpp"
#include <iostream>
#include <string_view>

namespace {
/// Print the command contract without touching workspace state.
/// 输出命令契约，不访问或修改工作区状态。
void help() {
    std::cout
        << "same " SAME_VERSION " - exact duplicate files in the working directory\n"
           "Usage: same [scan] [-r] [--summary] [scan options]\n"
           "       same new\n"
           "       same clean [-r]\n"
           "  scan (default) scan direct files; -r, --recursive includes subdirectories\n"
           "  new            create complete defaults and recommended .same/ignore; preserve "
           "existing files\n"
           "  clean          remove current .same; -r also removes descendant .same directories\n"
           "Scan options:\n"
           "  --summary  show Summary, Database and profiling (off by default)\n"
           "  --rehash       ignore cached hashes for this scan\n"
           "  --cpu          force the CPU backend\n"
           "  --cuda         select CUDA (CPU fallback when unavailable)\n"
           "  --igpu         select integrated OpenCL GPU (CPU fallback when unavailable/busy)\n"
           "  --no-pgo       disable runtime profile-guided routing (not compiler PGO)\n"
           "  --no-telemetry disable persistent local diagnostics (not PGO or summary)\n"
           "  --unique-files show unmatched paths (TSV: group 0)\n"
           "  --color=auto|always|never  auto honors NO_COLOR and redirection\n"
           "  --format=auto|pretty|tsv  auto uses pretty on terminals, TSV otherwise\n"
           "  -h, --help     show help\n"
           "  --version      show version\n"
           "Configuration: .same/config.toml; Git-compatible patterns: .same/ignore\n"
           "No directory argument: all commands operate on the working directory.\n"
           "Results: stdout; requested profiling and warnings/errors: stderr.\n";
}
} // namespace

/// Validate all arguments before side effects; omitted subcommand is scan.
/// 所有参数在副作用发生前校验；省略子命令时执行 scan。
/// Errors (including partial scans) return 2; duplicate results return 0.
/// 错误（包括未完成扫描）返回 2；发现重复文件返回 0。
int main(int argc, char** argv) {
    try {
        std::string_view command = "scan";
        int first = 1;
        if (argc > 1 && !std::string_view(argv[1]).starts_with("-")) {
            command = argv[1];
            first = 2;
            if (command != "scan" && command != "new" && command != "clean")
                throw std::runtime_error("unknown command: " + std::string(command));
        }
        bool rehash = false, unique_files = false;
        std::string_view backend;
        bool recursive = false, summary = false, no_pgo = false;
        bool no_telemetry = false;
        auto color_mode = same::ColorMode::automatic;
        std::string_view format = "auto";
        for (int i = first; i < argc; ++i) {
            const std::string_view arg = argv[i];
            if (arg == "--help" || arg == "-h") {
                help();
                return 0;
            }
            if (arg == "--version") {
                std::cout << "same " SAME_VERSION "\n";
                return 0;
            }
            if ((arg == "-r" || arg == "--recursive") && command != "new") {
                recursive = true;
                continue;
            }
            if (command != "scan")
                throw std::runtime_error("unsupported argument for " + std::string(command) + ": " +
                                         std::string(arg));
            if (arg == "--rehash")
                rehash = true;
            else if (arg == "--cpu" || arg == "--cuda" || arg == "--igpu") {
                const auto selected = arg.substr(2);
                if (!backend.empty() && backend != selected)
                    throw std::runtime_error("conflicting compute backend options");
                backend = selected;
            } else if (arg == "--no-pgo")
                no_pgo = true;
            else if (arg == "--no-telemetry")
                no_telemetry = true;
            else if (arg == "--unique-files")
                unique_files = true;
            else if (arg == "--summary")
                summary = true;
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
        if (command == "new") {
            same::initialize_workspace(root);
            std::cout << "Workspace ready: .same/config.toml and .same/ignore (existing files "
                         "preserved)\n";
            return 0;
        }
        if (command == "clean") {
            const auto removed = same::clean_workspace(root, recursive);
            std::cout << "Removed " << removed << " .same directories\n";
            return 0;
        }
        auto config = same::Config::load(root);
        if (rehash)
            config.rehash = true;
        if (!backend.empty())
            config.backend = backend;
        if (no_pgo)
            config.pgo = false;
        if (no_telemetry)
            config.telemetry = false;
        same::TerminalColor color(color_mode);
        const bool pretty = format == "pretty" || (format == "auto" && same::stdout_terminal());
        same::TerminalColor diagnostic_color(color_mode, same::TerminalStream::diagnostics);
        const bool pretty_profile =
            format == "pretty" ||
            (format == "auto" && same::is_terminal(same::TerminalStream::diagnostics));
        return same::run(root, config, std::cout, std::cerr,
                         {pretty, color.enabled(), unique_files, diagnostic_color.enabled(),
                          pretty_profile, recursive, summary});
    } catch (const std::exception& error) {
        std::cerr << "same: " << error.what() << '\n';
        return 2;
    }
}
