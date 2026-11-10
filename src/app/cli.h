#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace wintools::app
{

enum class Command
{
    Help,
    Monitors,
    Dump,
    Span,
    Restore,
    Windows,
    Capture,
    Apply,
    Show,
    Tray,
};

struct Options
{
    Command command{ Command::Help };
    std::filesystem::path image;
    bool dryRun{};

    /// Only meaningful for Tray: whether to put the panel on screen at startup. The autostart
    /// entry passes --tray so a login launch stays in the background.
    bool showPanel{ true };
};

struct ParseResult
{
    std::optional<Options> options;
    std::wstring error;
};

/// Parses argv into an Options struct.
///
/// Returns a ParseResult with an empty optional and a human-readable message on bad input; the
/// caller prints usage and exits. Kept separate from command execution so the parsing rules can
/// be reasoned about without touching COM.
[[nodiscard]] ParseResult ParseCommandLine(int argc, wchar_t** argv);

void PrintUsage();

} // namespace wintools::app
