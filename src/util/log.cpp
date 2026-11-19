#include "util/log.h"

#include <Windows.h>

#include <cstdio>
#include <filesystem>
#include <format>
#include <string>
#include <system_error>

#include "util/path.h"
#include "util/text.h"

namespace wintools::util
{

namespace
{

/// Rotate at 1 MB. The log is diagnostics for a tool that runs for weeks; anything bigger just
/// makes it harder to read the interesting part at the end.
constexpr unsigned long long kMaxLogBytes = 1024ull * 1024ull;

const wchar_t* TagOf(LogLevel level)
{
    switch (level)
    {
        case LogLevel::Info:
            return L"info";
        case LogLevel::Warn:
            return L"warn";
        case LogLevel::Error:
            return L"error";
    }

    return L"info";
}

void WriteToConsole(const std::wstring& line)
{
    const HANDLE handle = ::GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode = 0;

    if (handle != nullptr && handle != INVALID_HANDLE_VALUE && ::GetConsoleMode(handle, &mode) != 0)
    {
        DWORD written = 0;
        ::WriteConsoleW(handle, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
        ::WriteConsoleW(handle, L"\n", 1, &written, nullptr);
        return;
    }

    // No console: happens for a GUI-subsystem process started by Explorer, and for anything
    // launched with piped handles. The CRT stream may be unbound, in which case the write is a
    // silent no-op, which is exactly what is wanted - the file below is the real record.
    const std::string utf8 = ToUtf8(line);
    std::fwrite(utf8.data(), 1, utf8.size(), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

std::wstring Timestamp()
{
    SYSTEMTIME now{};
    ::GetLocalTime(&now);

    // Milliseconds, not seconds: the interesting sequences here (a display change settling, a
    // batch of windows being placed) all happen inside one second.
    return std::format(L"{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}", now.wYear, now.wMonth,
        now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
}

void AppendToFile(const std::wstring& line)
{
    const std::filesystem::path file = LogFile();

    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);

    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    const bool exists = ::GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &attributes) != 0;

    if (exists)
    {
        const unsigned long long size =
            (static_cast<unsigned long long>(attributes.nFileSizeHigh) << 32)
            | attributes.nFileSizeLow;

        if (size > kMaxLogBytes)
        {
            std::filesystem::path previous = file;
            previous += L".1";
            ::MoveFileExW(file.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    }

    // Viewers must be able to open the log while it is being written: most editors request
    // read+write access, and denying WRITE here makes "open the log" fail with a sharing violation
    // that reads to the user like a missing file. DELETE is shared for the rotation below.
    const HANDLE handle = ::CreateFileW(file.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);

    if (handle == INVALID_HANDLE_VALUE)
    {
        return;
    }

    std::string bytes;
    if (!exists)
    {
        // A byte order mark on a fresh file, so editors that still guess do not read the whole
        // log as ANSI and turn every Chinese log line into mojibake.
        bytes.append("\xEF\xBB\xBF");
    }

    bytes += ToUtf8(line);
    bytes += "\r\n";

    DWORD written = 0;
    ::WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    ::CloseHandle(handle);
}

} // namespace

void LogMessage(LogLevel level, std::wstring_view message)
{
    const std::wstring line = std::format(L"[{}] [{}] {}", Timestamp(), TagOf(level), message);

    WriteToConsole(line);
    AppendToFile(line);

#ifdef _DEBUG
    ::OutputDebugStringW(line.c_str());
    ::OutputDebugStringW(L"\n");
#endif
}

} // namespace wintools::util
