#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "core/update_feed.h"

namespace wintools::core
{

/// Writes one URL's bytes to `target`. Returns false with a human-readable reason on any failure;
/// a partial file is left behind for DownloadUpdate to remove before trying the next mirror.
using FetchFile = std::function<bool(
    std::wstring_view url, const std::filesystem::path& target, std::string& error)>;

/// Lower-case hex SHA-256 of the file now at `target`.
using HashFile = std::function<bool(const std::filesystem::path& target, std::string& hex)>;

/// Mirrors in the order they are tried. Duplicates and empty strings are dropped; the list is
/// built from the feed rather than from a user-controlled redirect.
[[nodiscard]] std::vector<std::wstring> DownloadUrls(const UpdateFeed& feed);

/// `.../update/WinTools-<version>.exe.new`. Version is in the name so a retry after a failed
/// install never mistakes an older package for the one the feed described.
[[nodiscard]] std::filesystem::path UpdateFilePath(const UpdateFeed& feed);

struct DownloadResult
{
    bool ok{};
    std::wstring message;
    std::filesystem::path path;
};

/// Downloads one mirror at a time and verifies size and SHA-256 before accepting it. A rejected
/// file is deleted; on total failure no `.new` file is left behind. Both IO primitives are
/// injected so hostile mirrors and bad hashes can be tested without opening a socket.
[[nodiscard]] DownloadResult DownloadUpdate(const FetchFile& fetch, const HashFile& hash,
    const UpdateFeed& feed, const std::filesystem::path& target);

} // namespace wintools::core
