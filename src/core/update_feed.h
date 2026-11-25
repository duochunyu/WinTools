#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/update_version.h"

namespace wintools::core
{

/// The parsed contents of update.txt, the small key=value file published beside each release.
///
/// This is untrusted input from the network: parsing is a pure function with no Win32 calls,
/// no file IO and no side effects, so it can be tested against hostile text in isolation.
struct UpdateFeed
{
    AppVersion version;
    std::uint64_t size{};
    std::wstring sha256;
    std::wstring url;
    std::wstring urlGithub;
    std::wstring notesZh;
    std::wstring notesEn;
};

/// Returns nullopt when a required field is missing or malformed. Unknown keys are ignored so
/// a future version of the publisher can add metadata without breaking older clients.
///
/// Length caps matter here: notes are rendered by a custom-drawn GDI panel, so an unbounded
/// string from the feed could make it lay out a huge amount of text on the UI thread.
[[nodiscard]] std::optional<UpdateFeed> ParseUpdateFeed(std::wstring_view text);

} // namespace wintools::core
