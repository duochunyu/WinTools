#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/update_feed.h"
#include "core/update_version.h"

namespace wintools::core
{

using FetchText = std::function<bool(std::wstring_view url, std::string& body)>;

/// The result of one update check. `available` is false both when there is no newer version and
/// when the check failed; `message` and `error` tell them apart.
///
/// `feed` is only set when `available` is true: it means "the update to apply", not "the feed we
/// managed to parse". Callers treat a present feed as "there is something to install", so an
/// up-to-date check must not leave one behind.
struct UpdateCheck
{
    bool available{};
    bool error{};
    std::wstring message;
    std::optional<UpdateFeed> feed;
    /// The first source that produced a usable feed, empty when none answered. Callers persist it
    /// as the preferred source so the next check starts where this one succeeded.
    std::wstring source;
};

/// Sources are tried in order. `custom` is an explicit user override and always goes first;
/// `preferred` — the source that worked last time on this machine — goes next, so a client that
/// has to fall back to GitHub stops paying the Gitee timeout on every single check. Both are
/// deduplicated against the built-in list: Gitee first for mainland users, GitHub as the
/// fallback. The list itself lives here rather than in the feed so a compromised feed cannot
/// redirect the client to an arbitrary mirror.
[[nodiscard]] std::vector<std::wstring> SourcesFor(
    std::wstring_view custom, std::wstring_view preferred = {});

/// Pure orchestration: fetch each source, parse the feed, compare against the current version.
/// The caller injects the transport, so unit tests can feed hostile or offline responses without
/// opening a socket.
///
/// A source whose version is not newer does not end the search: mirrors lag, and the list is
/// ordered mirror-first, so a stale mirror must not be able to hide a release another source
/// already publishes. "Up to date" is only reported after every source has said so.
[[nodiscard]] UpdateCheck CheckForUpdate(
    const FetchText& fetch, std::wstring_view customSource, const AppVersion& current,
    std::wstring_view preferred = {});

} // namespace wintools::core
