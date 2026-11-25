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
};

/// Sources are tried in order. An empty custom source means "use the built-in list": Gitee first
/// for mainland users, GitHub as the fallback. The list itself lives here rather than in the
/// feed so a compromised feed cannot redirect the client to an arbitrary mirror.
[[nodiscard]] std::vector<std::wstring> SourcesFor(std::wstring_view custom);

/// Pure orchestration: fetch each source, parse the feed, compare against the current version.
/// The caller injects the transport, so unit tests can feed hostile or offline responses without
/// opening a socket.
///
/// A source whose version is not newer does not end the search: mirrors lag, and the list is
/// ordered mirror-first, so a stale mirror must not be able to hide a release another source
/// already publishes. "Up to date" is only reported after every source has said so.
[[nodiscard]] UpdateCheck CheckForUpdate(
    const FetchText& fetch, std::wstring_view customSource, const AppVersion& current);

} // namespace wintools::core
