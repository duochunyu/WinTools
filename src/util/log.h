#pragma once

#include <format>
#include <string>
#include <string_view>
#include <utility>

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

template <typename... Args>
void LogInfo(std::wformat_string<Args...> format, Args&&... args)
{
    LogMessage(LogLevel::Info, std::format(format, std::forward<Args>(args)...));
}

template <typename... Args>
void LogWarn(std::wformat_string<Args...> format, Args&&... args)
{
    LogMessage(LogLevel::Warn, std::format(format, std::forward<Args>(args)...));
}

template <typename... Args>
void LogError(std::wformat_string<Args...> format, Args&&... args)
{
    LogMessage(LogLevel::Error, std::format(format, std::forward<Args>(args)...));
}

} // namespace wintools::util
