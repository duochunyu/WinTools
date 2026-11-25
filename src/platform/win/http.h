#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace wintools::platform::win
{

/// One synchronous HTTPS GET of a small text resource. WinHTTP is used directly rather than
/// WinINet or a third-party library: the shipping exe is a single static-CRT binary and WinHTTP
/// is already in the OS.
struct HttpResult
{
    bool ok{};
    unsigned long status{};  ///< HTTP status, 0 when the request never got a response.
    std::string body;        ///< UTF-8 bytes, regardless of the declared charset (feeds are ASCII/UTF-8).
    std::wstring error;      ///< Human-readable reason when ok is false.
};

/// Timeouts are short on purpose: this runs off the UI thread at startup, and a user with no
/// network must not feel that the app is hanging.
[[nodiscard]] HttpResult HttpGetText(std::wstring_view url, unsigned timeoutMs = 5000);

/// Streams one binary response to `target`, creating its parent folder. Still http/https only,
/// still 200-only, and capped at maxBytes so a hostile endpoint cannot fill the disk. The file
/// is written directly: the caller decides whether to keep it after checking size and hash.
struct DownloadOutcome
{
    bool ok{};
    unsigned long status{};
    std::uint64_t bytes{};
    std::wstring error;
};

[[nodiscard]] DownloadOutcome HttpDownloadToFile(std::wstring_view url,
    const std::filesystem::path& target, std::uint64_t maxBytes = 100ull * 1024ull * 1024ull,
    unsigned timeoutMs = 15'000);

} // namespace wintools::platform::win
