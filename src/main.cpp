#include "same/application.hpp"
#include "same/merge.hpp"
#include "same/terminal.hpp"
#include "same/workspace.hpp"
#include <charconv>
#include <iostream>
#include <limits>
#include <string_view>

namespace {
/// 使用已有终端颜色策略展示层级；纯文本保留相同标签和内容。
/// Render hierarchy with the existing terminal policy; plain text retains labels and content.
void help(same::ColorMode mode) {
    same::TerminalColor terminal(mode);
    const bool color = terminal.enabled();
    auto styled = [&](std::string_view value, std::string_view sgr) {
        if (color)
            std::cout << sgr;
        std::cout << value;
        if (color)
            std::cout << "\033[0m";
    };
    auto heading = [&](std::string_view title) {
        std::cout << '\n';
        styled(title, "\033[1;36m");
        std::cout << '\n';
    };
    auto row = [&](std::string_view name, std::string_view description) {
        std::cout << "  ";
        styled(name, "\033[1;32m");
        if (name.size() < 28)
            std::cout << std::string(28 - name.size(), ' ');
        else
            std::cout << ' ';
        std::cout << description << '\n';
    };
    styled("same " SAME_VERSION, "\033[1;35m");
    std::cout << " - exact duplicate files in the working directory\n";
    heading("Usage");
    std::cout << "  same [scan] [-r] [--summary] [scan options]\n"
                 "  same train [dirs...] [--max-files=N] [--max-bytes=N] [--summary]\n"
                 "  same merge [dir] [-r] [--move] [--summary]\n"
                 "  same new\n"
                 "  same clean [-r]\n";
    heading("Commands");
    row("scan (default)", "scan direct files; -r includes subdirectories");
    row("train", "calibrate the model used by auto scans in this workspace");
    row("merge", "import workspace state, preserving local configuration");
    row("new", "create defaults and .same/ignore; preserve existing files");
    row("clean", "remove current .same; -r also removes descendant .same directories");
    heading("Scan options");
    row("--rehash", "ignore cached hashes for this scan");
    row("--cpu", "force the CPU backend");
    row("--cuda", "select CUDA (CPU fallback when unavailable)");
    row("--igpu", "select integrated OpenCL GPU (CPU fallback when unavailable/busy)");
    row("--no-pgo", "disable runtime profile-guided routing");
    row("--unique-files", "show unmatched paths (TSV: group 0)");
    heading("Train options");
    row("dirs...", "read-only training directories (default: working directory)");
    row("--corpus=DIR", "compatible alias for a training directory");
    row("--max-files=N", "selected-file ceiling (default: 128; maximum: 4096)");
    row("--max-bytes=N", "logical-byte ceiling (default: 2 GiB)");
    heading("Merge options");
    row("dir", "source directory (default: working directory)");
    row("-r, --recursive", "discover descendant .same workspaces");
    row("--move", "remove source workspaces only after successful import");
    heading("Shared options");
    row("--summary", "show the full current-run diagnostic report");
    row("--no-telemetry", "disable local diagnostics, not model learning");
    row("--color=auto|always|never", "auto honors NO_COLOR and redirection");
    row("--format=auto|pretty|tsv", "auto uses pretty on terminals, TSV otherwise");
    row("-h, --help", "show help");
    row("--version", "show version");
    heading("Notes");
    std::cout << "  Configuration: .same/config.toml; ignore rules: .same/ignore\n"
                 "  Commands use the working directory; train saves its model there.\n"
                 "  Scan results: stdout; summaries and warnings/errors: stderr.\n"
                 "  Successful train is quiet unless --summary is requested.\n";
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
            if (command != "scan" && command != "new" && command != "clean" && command != "train" &&
                command != "merge")
                throw std::runtime_error("unknown command: " + std::string(command));
        }
        bool rehash = false, unique_files = false;
        std::string_view backend;
        bool recursive = false, summary = false, no_pgo = false;
        bool no_telemetry = false;
        std::vector<std::filesystem::path> corpora;
        std::filesystem::path merge_source;
        bool merge_source_set = false, move = false;
        bool positional_only = false;
        std::size_t train_max_files = 128;
        std::uint64_t train_max_bytes = 2ULL * 1024 * 1024 * 1024;
        const auto positive = [](std::string_view value) -> std::uint64_t {
            std::uint64_t number = 0;
            const auto* first = value.data();
            const auto* last = first + value.size();
            const auto result = std::from_chars(first, last, number);
            if (value.empty() || result.ec != std::errc{} || result.ptr != last || !number)
                throw std::runtime_error("training limit must be a positive decimal integer");
            return number;
        };
        auto color_mode = same::ColorMode::automatic;
        std::string_view format = "auto";
        const auto path_argument = [](std::string_view value) {
            return std::filesystem::path(
                std::u8string_view(reinterpret_cast<const char8_t*>(value.data()), value.size()));
        };
        for (int i = first; i < argc; ++i) {
            const std::string_view arg = argv[i];
            if ((command == "train" || command == "merge") && arg == "--") {
                positional_only = true;
                continue;
            }
            if (command == "train" && positional_only) {
                corpora.push_back(path_argument(arg));
                continue;
            }
            if (command == "merge" && positional_only) {
                if (merge_source_set)
                    throw std::runtime_error("merge accepts one source directory");
                merge_source = path_argument(arg);
                merge_source_set = true;
                continue;
            }
            if (arg == "--help" || arg == "-h") {
                auto help_mode = color_mode;
                for (int later = i + 1; later < argc; ++later) {
                    const std::string_view option = argv[later];
                    if (option.starts_with("--color="))
                        help_mode = same::parse_color(option.substr(8));
                }
                help(help_mode);
                return 0;
            }
            if (arg == "--version") {
                std::cout << "same " SAME_VERSION "\n";
                return 0;
            }
            if ((arg == "-r" || arg == "--recursive") && command != "new") {
                if (command == "train")
                    throw std::runtime_error("train already reads the corpus recursively");
                recursive = true;
                continue;
            }
            if (command == "train") {
                if (arg == "--no-telemetry")
                    no_telemetry = true;
                else if (arg == "--summary")
                    summary = true;
                else if (arg.starts_with("--color="))
                    color_mode = same::parse_color(arg.substr(8));
                else if (arg.starts_with("--format=")) {
                    format = arg.substr(9);
                    if (format != "auto" && format != "pretty" && format != "tsv")
                        throw std::runtime_error("invalid output format: " + std::string(format));
                } else if (arg.starts_with("--corpus=")) {
                    if (arg.size() == 9)
                        throw std::runtime_error("train requires nonempty --corpus=DIR");
                    corpora.push_back(path_argument(arg.substr(9)));
                } else if (arg.starts_with("--max-files=")) {
                    const auto value = positive(arg.substr(12));
                    if (value > 4096 || value > std::numeric_limits<std::size_t>::max())
                        throw std::runtime_error("--max-files must be in [1, 4096]");
                    train_max_files = static_cast<std::size_t>(value);
                } else if (arg.starts_with("--max-bytes="))
                    train_max_bytes = positive(arg.substr(12));
                else if (!arg.starts_with('-'))
                    corpora.push_back(path_argument(arg));
                else
                    throw std::runtime_error("unsupported argument for train: " + std::string(arg));
                continue;
            }
            if (command == "merge") {
                if (arg == "--move")
                    move = true;
                else if (arg == "--summary")
                    summary = true;
                else if (arg.starts_with("--color="))
                    color_mode = same::parse_color(arg.substr(8));
                else if (arg.starts_with("--format=")) {
                    format = arg.substr(9);
                    if (format != "auto" && format != "pretty" && format != "tsv")
                        throw std::runtime_error("invalid output format: " + std::string(format));
                } else if (!arg.starts_with('-') && !merge_source_set) {
                    merge_source = path_argument(arg);
                    merge_source_set = true;
                } else
                    throw std::runtime_error("unsupported argument for merge: " + std::string(arg));
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
        if (command == "merge") {
            same::TerminalColor diagnostic_color(color_mode, same::TerminalStream::diagnostics);
            const bool pretty_profile =
                format == "pretty" ||
                (format == "auto" && same::is_terminal(same::TerminalStream::diagnostics));
            return same::merge_workspaces(
                root, merge_source_set ? merge_source : root,
                {recursive, move, summary, pretty_profile, diagnostic_color.enabled()}, std::cerr);
        }
        auto config = same::Config::load(root);
        if (command == "train") {
            if (no_telemetry)
                config.telemetry = false;
            same::TerminalColor diagnostic_color(color_mode, same::TerminalStream::diagnostics);
            const bool pretty_profile =
                format == "pretty" ||
                (format == "auto" && same::is_terminal(same::TerminalStream::diagnostics));
            return same::train(root, corpora, config, std::cout, std::cerr, train_max_files,
                               train_max_bytes,
                               {summary, pretty_profile, diagnostic_color.enabled()});
        }
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
