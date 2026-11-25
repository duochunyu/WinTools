#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace wintools::core
{

/// A semantic version from a release tag or an update feed: major.minor.patch.
///
/// Deliberately no prerelease/build suffix. The project publishes only stable channels, and a
/// suffix would need precedence rules this app does not use. Rejecting it outright is safer than
/// silently comparing "1.2.0-beta" as if it were a release.
struct AppVersion
{
    std::uint32_t major{};
    std::uint32_t minor{};
    std::uint32_t patch{};

    /// Parses exactly three numeric components. Leading zeros are accepted; anything else
    /// (signs, suffixes, empty fields, overflow) is rejected rather than guessed at.
    [[nodiscard]] static std::optional<AppVersion> Parse(std::wstring_view text);

    /// Renders back to "major.minor.patch" with no leading zeros.
    [[nodiscard]] std::wstring Text() const;
};

/// Three-way comparison: negative when left < right, zero when equal, positive when greater.
/// Numeric per component - "1.10.0" is newer than "1.9.0".
[[nodiscard]] int Compare(const AppVersion& left, const AppVersion& right);

} // namespace wintools::core
