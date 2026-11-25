#pragma once

#include <filesystem>
#include <string>

#include "core/canvas.h"
#include "core/hotkeys.h"
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

    /// Re-create the sticky notes that were on screen before the last exit. Off keeps their index
    /// and text on disk but opens no note windows at startup; the tray / panel "show all" still
    /// brings them back, so this is a startup convenience, not a delete switch.
    bool restoreNotes{ true };

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

    /// 0 = follow the system UI language, 1 = Simplified Chinese, 2 = English. Mirrors
    /// util::i18n::Language and is applied once at startup, before the first log line.
    int language{ 0 };

    /// The sticky-note papers, as "RRGGBB,RRGGBB,..." for 2-5 colours. Empty keeps the built-in
    /// six. Stored raw rather than parsed: the palette is applied once at startup, and a
    /// hand-edited value that parses to nothing simply falls back to the default.
    std::wstring noteThemes;

    /// The desktop calendar: whether it is up, and where its square sits in virtual-desktop
    /// pixels. Stored here rather than in a file of its own because it is one widget, and a
    /// second file for three numbers would be a second thing to keep in sync.
    bool canvasEnabled{};
    core::CanvasRect canvas{};

    /// Whether the widget's second hand sweeps or steps. Sweeping costs about twenty-five tiny
    /// frames a second; stepping costs one, and is what a quartz watch does. Defaults to the
    /// former because a desktop clock that jerks looks broken to most people.
    bool canvasSweep{ true };

    /// Update checking is off by default: the product promises "no network" and the user opted
    /// into auto-update explicitly. A zero keeps that promise for existing installations.
    bool autoCheckUpdate{};

    /// Extra feed URL tried before the built-in Gitee/GitHub sources. Empty uses the defaults.
    std::wstring updateSource;

    /// When the last check ran, as display text. Purely informational, like lastAutoRestore.
    std::wstring lastUpdateCheck;

    /// Global shortcuts. Defaults are Ctrl+Alt+N (new note) and Ctrl+Alt+S (area capture).
    core::Hotkeys hotkeys{ core::DefaultHotkeys() };

    [[nodiscard]] static Settings Load();
    [[nodiscard]] bool Save() const;

    [[nodiscard]] static std::filesystem::path FilePath();

    /// Points Load/Save at a scratch file for tests. Empty (the default) means the real
    /// %LOCALAPPDATA%\WinTools\settings.txt. Without this, testing the v1/v2 parse would mean
    /// overwriting the user's own settings - the same reason NoteStore has SetFolder.
    static void SetFileForTests(std::filesystem::path file);
};

} // namespace wintools::app
