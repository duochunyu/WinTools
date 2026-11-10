#pragma once

#include <shobjidl_core.h>

#include <filesystem>
#include <string>
#include <vector>

#include "core/monitor.h"
#include "platform/win/com.h"

namespace wintools::core
{

/// DESKTOP_WALLPAPER_POSITION. The value is global: it applies to every monitor at once.
///
/// Spanning mode uses Center, because each piece is already cut to the exact monitor resolution
/// and any scaling mode would resample it a second time. Per-monitor mode uses Fill.
enum class WallpaperPosition : int
{
    Center = 0,
    Tile = 1,
    Stretch = 2,
    Fit = 3,
    Fill = 4,
    Span = 5,
};

/// Thin wrapper over the shell's IDesktopWallpaper.
///
/// Every entry point is best-effort: a failure is logged and reported as false or an empty list,
/// never thrown at the caller. Wallpaper is cosmetic and must not take the process down.
class DesktopWallpaper
{
public:
    DesktopWallpaper();
    ~DesktopWallpaper() = default;

    DesktopWallpaper(const DesktopWallpaper&) = delete;
    DesktopWallpaper& operator=(const DesktopWallpaper&) = delete;
    DesktopWallpaper(DesktopWallpaper&&) = delete;
    DesktopWallpaper& operator=(DesktopWallpaper&&) = delete;

    [[nodiscard]] bool IsValid() const
    {
        return m_wallpaper != nullptr;
    }

    /// Every monitor the shell knows about, each with its virtual-desktop rectangle.
    [[nodiscard]] std::vector<Monitor> Enumerate() const;

    [[nodiscard]] bool GetPosition(WallpaperPosition& position) const;
    [[nodiscard]] bool SetPosition(WallpaperPosition position);

    /// Assigns an image to one monitor.
    [[nodiscard]] bool SetWallpaper(const std::wstring& devicePath, const std::filesystem::path& image);

    /// Assigns one image to every monitor and clears any per-monitor assignment.
    [[nodiscard]] bool SetWallpaperForAll(const std::filesystem::path& image);

    /// Reads the wallpaper of one monitor, or the global one when devicePath is null.
    [[nodiscard]] std::wstring GetWallpaper(const std::wstring* devicePath) const;

private:
    platform::ComPtr<IDesktopWallpaper> m_wallpaper;
};

/// The shell's own copy of the current wallpaper, used when COM refuses to report a path.
///
/// While the position mode is Span, and for wallpapers applied through a theme or a slideshow,
/// both the global and the per-monitor queries report nothing. The shell still keeps the actual
/// pixels in %APPDATA%\Microsoft\Windows\Themes, and pointing a restore at that file reproduces
/// the original appearance even though the original source path is lost.
[[nodiscard]] std::wstring GetShellWallpaperFile();

} // namespace wintools::core
