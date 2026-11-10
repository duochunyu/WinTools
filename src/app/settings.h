#pragma once

#include <filesystem>
#include <string>

#include "core/layout_math.h"

namespace wintools::app
{

/// Everything the app remembers between runs.
///
/// Stored as a plain key=value text file rather than JSON: there are six fields, the file is
/// meant to be readable and hand-editable, and a JSON dependency would be 300 lines of escaping
/// code to buy nothing here. The nested structures (layout snapshots) live in their own
/// tab-separated file next to this one.
struct Settings
{
    /// Last image the user applied, so the tray can offer "re-apply" without a file dialog.
    std::wstring wallpaperSource;

    /// How that image is framed on the virtual desktop. Persisted because a crop the user spent
    /// time on has to survive a restart - and because the wallpaper is re-rendered from it
    /// whenever the picture is applied again.
    core::WallpaperView wallpaperView{};

    /// Re-place windows after displays come back.
    bool autoRestoreLayout{ true };

    bool startWithWindows{};

    /// When the layout was last put back automatically, as "11-07 16:31"; empty when it never was.
    ///
    /// Stored as display text rather than a timestamp: nothing computes with it - the panel shows
    /// it and the user reads it - and a readable field in a hand-editable file is worth more than
    /// a number that needs decoding.
    std::wstring lastAutoRestore;

    /// How long to wait after WM_DISPLAYCHANGE before acting. Panels come back one at a time and
    /// each arrival fires the message; acting on the first one would place windows for a topology
    /// that is still changing.
    int debounceSeconds{ 2 };

    /// 0 = light, 1 = dark, 2 = follow the system. Stored as a number rather than a word because
    /// the panel cycles through the three in order, and an unknown value falls back to 0.
    int theme{ 0 };

    /// Slot of the group the wallpaper buttons act on, so reopening the panel comes back to the
    /// group the user was working on.
    int activeGroup{ 0 };

    [[nodiscard]] static Settings Load();
    [[nodiscard]] bool Save() const;

    [[nodiscard]] static std::filesystem::path FilePath();
};

} // namespace wintools::app
