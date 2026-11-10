#pragma once

#include <format>
#include <string>
#include <string_view>
#include <utility>

#include "util/i18n.h"

namespace wintools::util
{

enum class LogLevel
{
    Info,
    Warn,
    Error,
};

/// Writes one line to stderr and, in debug builds, to the debugger.
///
/// Lives in util rather than app on purpose: every layer below app logs, and putting it any
/// higher would make core depend on app. Output goes through WriteConsoleW when stderr is a real
/// console so CJK text survives; when stderr is redirected to a file or pipe the line is written
/// as UTF-8 instead.
void LogMessage(LogLevel level, std::wstring_view message);

// The Chinese format string is the lookup key. Resolving it here instead of at each call
// site means both the console and WinTools.log follow the selected language, and the
// compile-time format check still applies to the source string.
template <typename... Args>
void LogInfo(std::wformat_string<Args...> format, Args&&... args)
{
    LogMessage(LogLevel::Info,
        std::vformat(i18n::Text(format.get()), std::make_wformat_args(args...)));
}

template <typename... Args>
void LogWarn(std::wformat_string<Args...> format, Args&&... args)
{
    LogMessage(LogLevel::Warn,
        std::vformat(i18n::Text(format.get()), std::make_wformat_args(args...)));
}

template <typename... Args>
void LogError(std::wformat_string<Args...> format, Args&&... args)
{
    LogMessage(LogLevel::Error,
        std::vformat(i18n::Text(format.get()), std::make_wformat_args(args...)));
}

} // namespace wintools::util
