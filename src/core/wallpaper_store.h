#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wintools::core
{

/// A display's own picture. An empty image means "follow the spanning wallpaper".
struct MonitorWallpaper
{
    std::wstring devicePath;
    std::wstring image;
};

using MonitorWallpapers = std::vector<MonitorWallpaper>;

/// Default location: %LOCALAPPDATA%\WinTools\wallpapers.txt
///
/// Separate from settings.txt because this is a list, not a set of scalars, and separate from
/// layout.txt because the two change at completely different times: the layout is captured on
/// demand, the picture assignment survives every capture.
[[nodiscard]] std::filesystem::path DefaultWallpaperFile();

[[nodiscard]] bool SaveMonitorWallpapers(
    const MonitorWallpapers& list, const std::filesystem::path& file);

/// Reads the assignments back. Returns nullopt when the file is missing or has no usable rows;
/// rows for displays that are no longer attached are kept, so plugging a screen back in restores
/// its picture without the user having to pick it again.
[[nodiscard]] std::optional<MonitorWallpapers> LoadMonitorWallpapers(
    const std::filesystem::path& file);

/// The picture assigned to one display, empty when it follows the spanning one.
[[nodiscard]] std::wstring_view FindMonitorImage(
    const MonitorWallpapers& list, std::wstring_view devicePath);

/// Assigns or clears one display's picture (empty image clears it).
///
/// Matching is case-insensitive: Windows reports the same display with different casing in
/// different sessions, and an exact comparison would silently create a second entry every time.
void SetMonitorImage(MonitorWallpapers& list, const std::wstring& devicePath,
    const std::wstring& image);

} // namespace wintools::core
