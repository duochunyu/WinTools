#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/desktop_wallpaper.h"

namespace wintools::core
{

/// A snapshot of the desktop wallpaper configuration, taken before the module changes anything
/// so the previous state can be put back.
///
/// The snapshot stores paths, not pixels: the shell owns the original files and they are not ours
/// to duplicate. If the user deletes an image after it is backed up, the restore degrades to a
/// broken path rather than to a wrong image, which is the honest failure.
struct WallpaperState
{
    WallpaperPosition position{ WallpaperPosition::Fill };
    std::wstring globalWallpaper;
    std::vector<std::pair<std::wstring, std::wstring>> perMonitor;

    [[nodiscard]] bool IsEmpty() const
    {
        return globalWallpaper.empty() && perMonitor.empty();
    }

    [[nodiscard]] bool Save(const std::filesystem::path& file) const;
    [[nodiscard]] static std::optional<WallpaperState> Load(const std::filesystem::path& file);
};

/// Reads the current position mode plus every non-empty per-monitor assignment.
[[nodiscard]] WallpaperState CaptureState(const DesktopWallpaper& wallpaper);

/// Puts a previously captured state back.
///
/// Takes a non-const reference because restoring mutates the desktop; capture is the read-only
/// direction and keeps its const overload.
///
/// Returns false when the state was applied only partially. A state with no usable image is
/// reported as a success-with-warning rather than a failure: "there was nothing to restore"
/// is not the same thing as "restore broke".
bool RestoreState(DesktopWallpaper& wallpaper, const WallpaperState& state);

} // namespace wintools::core
