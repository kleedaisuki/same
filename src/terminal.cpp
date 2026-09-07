// Environment is read only during single-threaded CLI setup.
// 仅在单线程 CLI 初始化阶段读取环境变量。
#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "same/terminal.hpp"
#include <array>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cstdio>
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif
namespace same {
namespace {
/// Treat absent environment variables as empty. 将未设置环境变量视为空。
std::string_view environment(const char* name) {
    const auto* value = std::getenv(name);
    return value ? value : "";
}
/// Use stable decimal formatting without modifying caller streams.
/// 使用稳定小数格式，不修改调用方流。
std::string quantity(double value, std::string_view unit) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(value == 0 ? 0 : 2) << value << ' ' << unit;
    return out.str();
}
/// Reject invalid public formatting input rather than displaying misleading units.
/// 拒绝非法格式化输入，避免展示误导单位。
void validate_quantity(double value) {
    if (!std::isfinite(value) || value < 0)
        throw std::invalid_argument("quantity must be finite and nonnegative");
}
} // namespace
std::string human_bytes(double bytes) {
    validate_quantity(bytes);
    constexpr std::array units{"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
    std::size_t unit = 0;
    while (bytes >= 1024 && unit + 1 < units.size()) {
        bytes /= 1024;
        ++unit;
    }
    return quantity(bytes, units[unit]);
}
std::string human_duration(double ms) {
    validate_quantity(ms);
    if (ms == 0)
        return "0 ms";
    if (ms < 0.001)
        return quantity(ms * 1000000, "ns");
    if (ms < 1)
        return quantity(ms * 1000, "us");
    if (ms < 1000)
        return quantity(ms, "ms");
    if (ms < 60000)
        return quantity(ms / 1000, "s");
    if (ms < 3600000)
        return quantity(ms / 60000, "min");
    return quantity(ms / 3600000, "h");
}
ColorMode parse_color(std::string_view value) {
    if (value == "auto")
        return ColorMode::automatic;
    if (value == "always")
        return ColorMode::always;
    if (value == "never")
        return ColorMode::never;
    throw std::runtime_error("invalid color mode: " + std::string(value));
}
bool use_color(ColorMode mode, bool terminal, bool capable, std::string_view term,
               std::string_view no_color) {
    if (mode != ColorMode::automatic)
        return mode == ColorMode::always;
    return terminal && capable && term != "dumb" && no_color.empty();
}
bool is_terminal(TerminalStream stream) {
#ifdef _WIN32
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(stream == TerminalStream::output ? STD_OUTPUT_HANDLE
                                                                        : STD_ERROR_HANDLE),
                          &mode) != 0;
#else
    return isatty(stream == TerminalStream::output ? STDOUT_FILENO : STDERR_FILENO) == 1;
#endif
}
bool stdout_terminal() {
    return is_terminal(TerminalStream::output);
}
TerminalColor::TerminalColor(ColorMode mode, TerminalStream stream) {
    const bool terminal = is_terminal(stream);
    enabled_ = use_color(mode, terminal, true, environment("TERM"), environment("NO_COLOR"));
#ifdef _WIN32
    if (!enabled_ || !terminal)
        return;
    const auto handle =
        GetStdHandle(stream == TerminalStream::output ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
    DWORD original = 0;
    const bool capable = GetConsoleMode(handle, &original) &&
                         SetConsoleMode(handle, original | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    if (capable) {
        handle_ = handle;
        original_ = original;
    }
    enabled_ = use_color(mode, terminal, capable, environment("TERM"), environment("NO_COLOR"));
#endif
}
TerminalColor::~TerminalColor() {
#ifdef _WIN32
    if (handle_)
        SetConsoleMode(handle_, original_);
#endif
}
} // namespace same
