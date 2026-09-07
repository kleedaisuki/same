#pragma once
#include <string>
#include <string_view>
namespace same {
/// Auto follows terminal capability; explicit modes override environment hints.
/// 自动模式遵循终端能力；显式模式覆盖环境提示。
enum class ColorMode { automatic, always, never };
/// Parse auto/always/never or throw, before touching scan state.
/// 解析 auto/always/never，否则抛出；在修改扫描状态前执行。
ColorMode parse_color(std::string_view value);
/// Pure policy for deterministic tests; nonempty NO_COLOR suppresses auto color.
/// 可确定性测试的纯策略；非空 NO_COLOR 禁用自动颜色。
bool use_color(ColorMode mode, bool terminal, bool capable, std::string_view term,
               std::string_view no_color);
/// Standard stream to probe independently. 独立探测的标准流。
enum class TerminalStream { output, diagnostics };
/// Detect a standard stream without changing its mode. 检测标准流，不修改其模式。
bool is_terminal(TerminalStream stream);
/// Format nonnegative finite bytes or bytes/s with binary units, e.g. 1024 -> "1.00 KiB".
/// 用二进制单位格式化非负有限字节或字节每秒，例如 1024 -> "1.00 KiB"。
std::string human_bytes(double bytes);
/// Format nonnegative finite milliseconds as ns/us/ms/s/min/h, e.g. 1500 -> "1.50 s".
/// 将非负有限毫秒格式化为 ns/us/ms/s/min/h，例如 1500 -> "1.50 s"。
std::string human_duration(double milliseconds);
/// Detect stdout terminal without changing its mode. 检测标准输出终端，不修改其模式。
bool stdout_terminal();
/// Enable Windows VT when supported; restore original mode on scope exit.
/// 支持时启用 Windows VT，离开作用域时恢复原模式。
class TerminalColor {
public:
    /// Evaluate a standard stream color policy, independently of presentation format.
    /// 判断标准流颜色策略，与展示格式独立。
    explicit TerminalColor(ColorMode mode, TerminalStream stream = TerminalStream::output);
    /// Restore only a console mode changed by this instance. 仅恢复本实例改变的控制台模式。
    ~TerminalColor();
    /// Console mode ownership cannot be copied. 控制台模式所有权不可复制。
    TerminalColor(const TerminalColor&) = delete;
    /// Console mode ownership cannot be assigned. 控制台模式所有权不可赋值。
    TerminalColor& operator=(const TerminalColor&) = delete;
    /// Whether ANSI SGR is enabled. 是否启用 ANSI SGR。
    bool enabled() const {
        return enabled_;
    }

private:
    /// Resolved policy. 已解析的策略。
    bool enabled_{false};
    /// Borrowed console handle, null unless restoration is needed.
    /// 借用控制台句柄，仅需恢复时非空。
    void* handle_{nullptr};
    /// Original Windows console mode. 原始 Windows 控制台模式。
    unsigned long original_{0};
};
} // namespace same
