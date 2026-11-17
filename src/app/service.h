#pragma once

#include <Windows.h>

#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "app/actions.h"
#include "app/desktop_canvas.h"
#include "app/note_manager.h"
#include "app/panel.h"
#include "app/settings.h"
#include "app/tray.h"
#include "core/canvas.h"
#include "core/display_watcher.h"
#include "core/group.h"

namespace wintools::app
{

/// The resident agent: tray icon, settings panel, display watcher.
///
/// Owns every piece of mutable state and is the only place that decides what an action means.
/// The panel, the tray menu and the CLI all describe intent; this class executes it.
class Service
{
public:
    Service(HINSTANCE instance, HICON icon);
    ~Service();

    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;
    Service(Service&&) = delete;
    Service& operator=(Service&&) = delete;

    /// Creates the host window, the tray icon, the panel and the watcher.
    ///
    /// showPanel is false for the autostart path: a login-time launch must not steal focus.
    [[nodiscard]] bool Start(bool showPanel);

    /// Runs the message loop until the user quits. Returns the process exit code.
    int Run();

private:
    static constexpr UINT kTrayMessage = WM_APP + 1;
    static constexpr UINT kQuitMessage = WM_APP + 2;

    /// Posted by the worker thread when the long action it was given has finished.
    static constexpr UINT kWorkDone = WM_APP + 4;

    /// One-shot timer that gives a display stack which is still coming up a second chance.
    static constexpr UINT_PTR kRetryTimerId = 3;
    static constexpr UINT kRetryDelayMs = 1500;

    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);

    [[nodiscard]] bool CreateHostWindow();
    void HandleAction(const PanelRequest& request);
    /// isRetry marks the second attempt, which must not schedule a third.
    void OnDisplaysSettled(bool isRetry = false);
    void ShowTrayMenu();

    /// Reads groups from disk, or migrates the pre-group format into them on first run.
    void LoadOrMigrateGroups();

    /// Makes the model agree with the displays that are attached right now, and writes the file
    /// when that changed anything.
    void SyncGroups();

    void SaveGroupsNow();

    /// Brings the desktop calendar up, or takes it down. Idempotent both ways.
    [[nodiscard]] bool StartCanvas();
    void StopCanvas();

    /// Registers every configured shortcut on the host window. Unregisters first, so a settings
    /// change can rebind without restarting. A single collision is logged and skipped.
    void RegisterHotkeys();
    void UnregisterHotkeys();

    /// Re-clamps the calendar after the display layout changed.
    void SyncCanvas();

    /// Renders and applies every group; the piece cache is swept once, at the end.
    ActionResult ApplyAllGroups() const;

    /// Runs work off the UI thread so the busy state can actually be seen. The panel is disabled
    /// for the duration, and the result comes back through kWorkDone.
    void BeginWork(std::wstring label, std::function<ActionResult()> work, bool clearDirty);
    void FinishWork();

    void SetStatus(std::wstring text, bool isError = false);
    void SyncTrayTooltip();

    [[nodiscard]] bool DarkTheme() const;

    /// The slot of the group the wallpaper buttons act on, clamped to what exists.
    [[nodiscard]] int ClampActiveSlot(int slot) const;

    [[nodiscard]] PanelState ReadState();

    HINSTANCE m_instance{};
    HICON m_icon{};
    HWND m_window{};
    UINT m_taskbarCreated{};

    Settings m_settings;
    std::wstring m_status;

    /// Red status line. Cleared by the next action that succeeds, so a fixed problem stops
    /// shouting without the user having to do anything about it.
    bool m_statusIsError{};

    /// Newest first, three at most. A failure stays here after a later success overwrote the
    /// status line - which is the only reason it exists.
    std::deque<StatusEntry> m_history;

    std::vector<core::Group> m_groups;
    int m_activeSlot{};

    bool m_busy{};
    std::wstring m_busyLabel;
    bool m_clearDirtyOnSuccess{};

    struct WorkResult
    {
        bool ok{};
        std::wstring message;
    };

    WorkResult m_workResult{};
    std::thread m_worker;

    Panel m_panel;
    TrayIcon m_tray;
    DesktopCanvas m_canvas;
    NoteManager m_notes;
    std::unique_ptr<core::DisplayWatcher> m_watcher;
};

} // namespace wintools::app
