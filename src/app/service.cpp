#include "app/service.h"

#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

#include "app/actions.h"
#include "app/capture.h"
#include "app/features.h"
#include "app/startup.h"
#include "app/updater.h"
#include "core/desktop_wallpaper.h"
#include "core/layout_math.h"
#include "core/update_download.h"
#include "core/update_snapshot.h"
#include "core/note_theme.h"
#include "core/wallpaper_store.h"
#include "platform/win/hash.h"
#include "platform/win/http.h"
#include "util/i18n.h"
#include "util/log.h"
#include "util/path.h"
#include "util/text.h"
#include "util/version.h"

namespace wintools::app
{

using util::LogError;
using util::LogInfo;
using util::LogWarn;

namespace
{

constexpr wchar_t kHostClassName[] = L"WinTools.Service.2b8e5d10";

/// Tray menu commands.
enum MenuId : UINT
{
    kMenuOpen = 1001,
    kMenuCapture,
    kMenuApply,
    kMenuWallpaper,
    kMenuRestoreWallpaper,
    kMenuCanvas,
    kMenuNewNote,
    kMenuShowNotes,
    kMenuCheckUpdate,
    kMenuQuit,
};

/// "11-07 16:31" in local time, for the "last automatic restore" line.
std::wstring NowLabel()
{
    SYSTEMTIME time{};
    ::GetLocalTime(&time);
    return std::format(L"{:02}-{:02} {:02}:{:02}", time.wMonth, time.wDay, time.wHour, time.wMinute);
}

constexpr DWORD kUpdateStartDelayMs = 30'000;
constexpr DWORD kUpdateIntervalMs = 6u * 60u * 60u * 1000u;

/// "2026-11-25 14:03" in local time. Written as a sortable, human-readable label: the config
/// file stays hand-editable, and the same string is what the panel shows as "last checked".
std::wstring UpdateTimeLabel()
{
    SYSTEMTIME now{};
    ::GetLocalTime(&now);
    return std::format(L"{:04}-{:02}-{:02} {:02}:{:02}", now.wYear, now.wMonth, now.wDay,
        now.wHour, now.wMinute);
}

/// Milliseconds left before an opt-in check is due, or empty when it is due now. A damaged or
/// hand-edited label counts as due: the timer must never be able to stall forever on bad input.
std::optional<DWORD> UpdateDelayMs(std::wstring_view label)
{
    if (label.size() != 16 || label[4] != L'-' || label[7] != L'-' || label[10] != L' '
        || label[13] != L':')
    {
        return std::nullopt;
    }

    const auto digits = [&label](size_t at, int count, WORD& value) {
        int parsed = 0;
        for (int index = 0; index < count; ++index)
        {
            const wchar_t c = label[at + static_cast<size_t>(index)];
            if (c < L'0' || c > L'9')
            {
                return false;
            }
            parsed = parsed * 10 + (c - L'0');
        }
        value = static_cast<WORD>(parsed);
        return true;
    };

    SYSTEMTIME then{};
    if (!digits(0, 4, then.wYear) || !digits(5, 2, then.wMonth)
        || !digits(8, 2, then.wDay) || !digits(11, 2, then.wHour)
        || !digits(14, 2, then.wMinute))
    {
        return std::nullopt;
    }
    then.wSecond = 0;
    then.wMilliseconds = 0;

    SYSTEMTIME now{};
    ::GetLocalTime(&now);

    // Both sides are local SYSTEMTIMEs, so converting them with the same API gives a correct
    // difference even though SystemTimeToFileTime does not apply the time zone.
    FILETIME thenFile{}, nowFile{};
    if (::SystemTimeToFileTime(&then, &thenFile) == FALSE
        || ::SystemTimeToFileTime(&now, &nowFile) == FALSE)
    {
        return std::nullopt;
    }

    ULARGE_INTEGER start{};
    start.LowPart = thenFile.dwLowDateTime;
    start.HighPart = thenFile.dwHighDateTime;
    ULARGE_INTEGER end{};
    end.LowPart = nowFile.dwLowDateTime;
    end.HighPart = nowFile.dwHighDateTime;
    if (end.QuadPart <= start.QuadPart)
    {
        return kUpdateIntervalMs;
    }

    const ULONGLONG elapsedMs = (end.QuadPart - start.QuadPart) / 10'000ULL;
    if (elapsedMs >= kUpdateIntervalMs)
    {
        return std::nullopt;
    }
    return static_cast<DWORD>(kUpdateIntervalMs - elapsedMs);
}

/// Two member lists with the same displays, in any order.
bool SameMembers(const std::vector<std::wstring>& left, const std::vector<std::wstring>& right)
{
    if (left.size() != right.size())
    {
        return false;
    }

    for (const auto& member : left)
    {
        bool found = false;
        for (const auto& other : right)
        {
            if (core::Monitor::IEquals(member, other))
            {
                found = true;
                break;
            }
        }

        if (!found)
        {
            return false;
        }
    }

    return true;
}

bool ContainsMembers(
    const std::vector<std::vector<std::wstring>>& lists, const std::vector<std::wstring>& wanted)
{
    for (const auto& list : lists)
    {
        if (SameMembers(list, wanted))
        {
            return true;
        }
    }

    return false;
}

/// Where the calendar lands the first time it is switched on: top right of the display the cursor
/// is on, one margin in. Starting at (0, 0) would put it over the icons every time.
core::CanvasRect DefaultCanvasRect()
{
    POINT cursor{};
    ::GetCursorPos(&cursor);

    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (::GetMonitorInfoW(::MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &info) == FALSE)
    {
        return core::CanvasRect{};
    }

    constexpr int kMargin = 32;
    const int size = core::kCanvasDefaultSize;

    return core::CanvasRect{ info.rcWork.right - size - kMargin, info.rcWork.top + kMargin, size };
}

/// Cursor feedback for operations that block the message loop for a moment.
class WaitCursor
{
public:
    WaitCursor()
        : m_previous(::SetCursor(::LoadCursorW(nullptr, IDC_WAIT)))
    {
    }

    ~WaitCursor()
    {
        ::SetCursor(m_previous);
    }

    WaitCursor(const WaitCursor&) = delete;
    WaitCursor& operator=(const WaitCursor&) = delete;

private:
    HCURSOR m_previous;
};

} // namespace

Service::Service(HINSTANCE instance, HICON icon)
    : m_instance(instance)
    , m_icon(icon)
    , m_panel([this](const PanelRequest& request) { HandleAction(request); },
          [this] { return ReadState(); })
{
    // A note can be closed straight from its own title bar, with the panel nowhere in the loop.
    // This is how the panel's closed-note block learns about it without the manager knowing about
    // the panel.
    m_notes.archiveChanged = [this] { m_panel.Refresh(); };
}

Service::~Service()
{
    // Both workers hold `this` and post to the host window, so neither may outlive the state it
    // writes. Join them before tearing anything else down.
    if (m_updateWorker.joinable())
    {
        m_updateWorker.join();
    }

    // The watcher can fire into a half-destroyed object, so it goes first - and the worker has to
    // be joined before any of the state it reads disappears.
    m_watcher.reset();

    if (m_worker.joinable())
    {
        m_worker.join();
    }

    m_tray.Remove();

    if (m_window != nullptr)
    {
        UnregisterHotkeys();
        ::DestroyWindow(m_window);
        m_window = nullptr;
    }
}

bool Service::CreateHostWindow()
{
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &Service::WndProc;
    windowClass.hInstance = m_instance;
    windowClass.hIcon = m_icon;
    windowClass.lpszClassName = kHostClassName;

    if (::RegisterClassExW(&windowClass) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        LogError(L"RegisterClassEx failed for the service window");
        return false;
    }

    m_window = ::CreateWindowExW(0, kHostClassName, L"WinTools", WS_POPUP, 0, 0, 0, 0, nullptr,
        nullptr, m_instance, this);

    if (m_window == nullptr)
    {
        LogError(L"CreateWindowEx failed for the service window");
        return false;
    }

    // Explorer broadcasts this when the taskbar is rebuilt; every tray icon has to be re-added.
    m_taskbarCreated = ::RegisterWindowMessageW(L"TaskbarCreated");
    RegisterHotkeys();
    return true;
}

void Service::UnregisterHotkeys()
{
    if (m_window == nullptr)
    {
        return;
    }

    // Unregistering a hotkey that was never registered is harmless, so no need to track which
    // ones took: the ids are fixed and small.
    for (const core::HotkeyAction action : core::kHotkeyActions)
    {
        ::UnregisterHotKey(m_window, static_cast<int>(action));
    }
}

void Service::RegisterHotkeys()
{
    UnregisterHotkeys();

    for (const core::HotkeyAction action : core::kHotkeyActions)
    {
        const core::Hotkey& hotkey = m_settings.hotkeys.At(action);
        if (!hotkey.Bound())
        {
            continue;
        }

        if (!::RegisterHotKey(m_window, static_cast<int>(action), hotkey.modifiers, hotkey.vk))
        {
            LogWarn(L"hotkey {} could not be registered: {}", core::HotkeyActionLabel(action),
                core::DescribeHotkey(hotkey));
        }
    }
}

bool Service::Start(bool showPanel)
{
    m_settings = Settings::Load();
    m_status.clear();

    // Custom note papers, applied once before any note window or panel exists. An unparsable or
    // out-of-range value keeps the built-in six; see core::SetNotePalette.
    core::SetNotePalette(core::ParseNotePalette(m_settings.noteThemes));

    if (!CreateHostWindow())
    {
        return false;
    }

    LoadOrMigrateGroups();
    SyncGroups();

    if (!m_panel.Create(m_instance, m_icon))
    {
        return false;
    }

    m_tray.Add(m_window, m_icon, L"WinTools", kTrayMessage);

    // Notes are restored before anything else can cover the screen - when the user asked for that.
    // With restart restore off the index is still read, but no window is created until the tray
    // or the notes page asks for them.
    m_notes.Start(m_instance, m_settings.restoreNotes);

    if (kDesktopWidgetsEnabled && m_settings.canvasEnabled)
    {
        (void)StartCanvas();
    }

    m_watcher = std::make_unique<core::DisplayWatcher>(
        [this] { OnDisplaysSettled(); }, m_settings.debounceSeconds * 1000);

    if (!m_watcher->Start())
    {
        // Not fatal: without the watcher the app still works, it just needs to be told when
        // displays change. A message box here would be worse than a log line.
        LogWarn(L"display watcher unavailable, automatic restore is disabled");
    }

    SyncTrayTooltip();

    if (showPanel)
    {
        m_panel.ShowPanel();
    }

    // The opt-in check starts 30 seconds in, so startup never waits on the network. The timer
    // also carries the six-hour repeat; turning the setting off cancels it.
    ScheduleUpdateCheck();
    return true;
}

int Service::Run()
{
    MSG message{};

    while (true)
    {
        const BOOL result = ::GetMessageW(&message, nullptr, 0, 0);
        if (result == 0)
        {
            break;
        }

        if (result == -1)
        {
            LogError(L"GetMessage failed, stopping the message loop");
            break;
        }

        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }

    return static_cast<int>(message.wParam);
}

LRESULT CALLBACK Service::WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCCREATE)
    {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        auto* self = static_cast<Service*>(create->lpCreateParams);
        ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_window = window;
        return ::DefWindowProcW(window, message, wparam, lparam);
    }

    auto* self = reinterpret_cast<Service*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self != nullptr)
    {
        return self->HandleMessage(message, wparam, lparam);
    }

    return ::DefWindowProcW(window, message, wparam, lparam);
}

LRESULT Service::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == m_taskbarCreated && m_taskbarCreated != 0)
    {
        LogInfo(L"taskbar rebuilt, re-adding the tray icon");
        m_tray.ReAdd();
        return 0;
    }

    switch (message)
    {
        case kTrayMessage:
            if (lparam == WM_LBUTTONUP)
            {
                m_panel.Toggle();
            }
            else if (lparam == WM_RBUTTONUP)
            {
                ShowTrayMenu();
            }
            return 0;

        case kWorkDone:
            FinishWork();
            return 0;

        case kUpdateDone:
            FinishUpdateCheck();
            return 0;

        case WM_HOTKEY:
        {
            const int id = static_cast<int>(wparam);
            if (id == static_cast<int>(core::HotkeyAction::NewNote))
            {
                m_notes.NewNote();
            }
            else if (id == static_cast<int>(core::HotkeyAction::Capture))
            {
                // Into the note the user is in, when there is one. The hotkey exists so that a
                // screenshot can be pasted where the thought is; sending it to a folder and
                // changing nothing on screen reads as "the hotkey did nothing", which is what was
                // reported after a day of screenshots quietly landing in Pictures.
                if (Note* note = m_notes.NoteInForeground(); note != nullptr)
                {
                    util::LogInfo(L"hotkey capture goes into note {}", note->Id());
                    note->CaptureScreenshot();
                    return 0;
                }

                // Nothing to insert into, so the picture goes to the clipboard instead of a file:
                // the user asked for a screenshot they can paste wherever they are, and a file in
                // Pictures was both invisible and an unnecessary copy of something that is about to
                // be pasted anyway. The balloon is the only feedback this path has - the panel's
                // status line is not on screen when the panel is closed.
                switch (CopyRegionToClipboard(m_instance))
                {
                    case CaptureResult::Cancelled:
                        SetStatus(util::i18n::Cstr(L"截图已取消"), false);
                        break;

                    case CaptureResult::Copied:
                        SetStatus(util::i18n::Cstr(L"截图完成，可粘贴"), false);
                        m_tray.ShowInfo(util::i18n::Cstr(L"截图完成，可粘贴"),
                            util::i18n::Cstr(L"按 Ctrl+V 粘贴到任意位置"));
                        break;

                    case CaptureResult::Failed:
                        SetStatus(util::i18n::Cstr(L"截图失败，详见日志"), true);
                        break;
                }
            }
            else if (id == static_cast<int>(core::HotkeyAction::CaptureLayout)
                || id == static_cast<int>(core::HotkeyAction::ApplyLayout))
            {
                // Routed through the same request the layout page's buttons send, so the shortcut and
                // the click can never drift apart. The tray tooltip carries the result, exactly as it
                // does when the same action is picked from the tray menu.
                const PanelAction action = id == static_cast<int>(core::HotkeyAction::CaptureLayout)
                    ? PanelAction::RefreshLayout
                    : PanelAction::ApplyLayout;
                HandleAction(PanelRequest{ action });
            }
            return 0;
        }

        case WM_COMMAND:
            switch (LOWORD(wparam))
            {
                case kMenuOpen:
                    m_panel.ShowPanel();
                    return 0;

                case kMenuCapture:
                    HandleAction(PanelRequest{ PanelAction::RefreshLayout });
                    return 0;

                case kMenuApply:
                    HandleAction(PanelRequest{ PanelAction::ApplyLayout });
                    return 0;

                case kMenuWallpaper:
                    HandleAction(PanelRequest{ PanelAction::PickWallpaper });
                    return 0;

                case kMenuRestoreWallpaper:
                    HandleAction(PanelRequest{ PanelAction::RestoreWallpaper });
                    return 0;

                case kMenuCanvas:
                    HandleAction(PanelRequest{ PanelAction::ToggleCanvas });
                    return 0;

                case kMenuNewNote:
                    m_notes.NewNote();
                    return 0;

                case kMenuShowNotes:
                    m_notes.ShowAll();
                    return 0;

                case kMenuCheckUpdate:
                    HandleAction(PanelRequest{ m_updateFeed.has_value()
                            ? PanelAction::ShowUpdateNotes
                            : PanelAction::CheckUpdate });
                    return 0;

                case kMenuQuit:
                    HandleAction(PanelRequest{ PanelAction::Quit });
                    return 0;

                default:
                    break;
            }
            break;

        case kQuitMessage:
            m_notes.SaveAll();
            ::PostQuitMessage(0);
            return 0;

        case WM_TIMER:
            if (wparam == kRetryTimerId)
            {
                ::KillTimer(m_window, kRetryTimerId);
                OnDisplaysSettled(true);
                return 0;
            }
            if (wparam == kUpdateCheckTimerId)
            {
                ::KillTimer(m_window, kUpdateCheckTimerId);
                // Re-arming happens in FinishUpdateCheck, after lastUpdateCheck is written.
                BeginUpdateCheck(false);
                return 0;
            }
            break;

        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;

        default:
            break;
    }

    return ::DefWindowProcW(m_window, message, wparam, lparam);
}

void Service::ShowTrayMenu()
{
    const HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr)
    {
        return;
    }

    ::AppendMenuW(menu, MF_STRING, kMenuOpen, util::i18n::Cstr(L"面板"));
    ::SetMenuDefaultItem(menu, kMenuOpen, FALSE);

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kMenuCapture, util::i18n::Cstr(L"刷新布局"));
    ::AppendMenuW(menu, MF_STRING, kMenuApply, util::i18n::Cstr(L"恢复布局"));

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kMenuWallpaper, util::i18n::Cstr(L"更换壁纸"));
    ::AppendMenuW(menu, MF_STRING, kMenuRestoreWallpaper, util::i18n::Cstr(L"还原壁纸"));
    if constexpr (kDesktopWidgetsEnabled)
    {
        ::AppendMenuW(menu, MF_STRING | (m_settings.canvasEnabled ? MF_CHECKED : 0), kMenuCanvas,
            util::i18n::Cstr(L"桌面日历"));
    }
    ::AppendMenuW(menu, MF_STRING, kMenuNewNote, util::i18n::Cstr(L"新建便签"));
    ::AppendMenuW(menu, MF_STRING, kMenuShowNotes, util::i18n::Cstr(L"显示所有便签"));

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kMenuCheckUpdate,
        util::i18n::Cstr(m_updateFeed.has_value() ? L"有新版本 · 查看说明" : L"检查更新"));

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kMenuQuit, util::i18n::Cstr(L"退出"));

    POINT cursor{};
    ::GetCursorPos(&cursor);

    // Two documented workarounds, both still required: without the foreground window the menu
    // refuses to close when the user clicks elsewhere, and without the trailing WM_NULL the
    // first click after dismissing it is swallowed.
    ::SetForegroundWindow(m_window);

    ::TrackPopupMenu(
        menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, cursor.x, cursor.y, 0, m_window, nullptr);

    ::PostMessageW(m_window, WM_NULL, 0, 0);
    ::DestroyMenu(menu);
}

// --- Groups ---------------------------------------------------------------------------------

void Service::LoadOrMigrateGroups()
{
    if (auto loaded = core::LoadGroups(core::DefaultGroupFile()); loaded.has_value())
    {
        m_groups = std::move(*loaded);
        m_activeSlot = ClampActiveSlot(m_settings.activeGroup);
        LogInfo(L"loaded {} group(s)", m_groups.size());
        return;
    }

    // First run, or the first run after groups were introduced: the single spanning picture plus
    // the per-display overrides map exactly onto one group of the survivors and one group per
    // override. Nothing the user did before this version is lost.
    m_groups = core::GroupsFromLegacy(core::CaptureMonitors(), m_settings.wallpaperSource,
        m_settings.wallpaperView, ListDisplayPictures());

    m_activeSlot = 0;
    LogInfo(L"migrated the per-display wallpaper file into {} group(s)", m_groups.size());
    SaveGroupsNow();
}

int Service::ClampActiveSlot(int slot) const
{
    if (m_groups.empty())
    {
        return 0;
    }

    return std::clamp(slot, 0, static_cast<int>(m_groups.size()) - 1);
}

void Service::SyncGroups()
{
    const std::vector<core::MonitorSnapshot> monitors = core::CaptureMonitors();

    const std::vector<core::Group> before = m_groups;
    core::NormaliseGroups(m_groups, monitors);

    const int active = ClampActiveSlot(m_activeSlot);
    const bool changed = active != m_activeSlot || m_groups.size() != before.size();
    m_activeSlot = active;

    bool membershipsDiffer = changed;
    if (!membershipsDiffer)
    {
        for (size_t index = 0; index < m_groups.size(); ++index)
        {
            if (!SameMembers(m_groups[index].members, before[index].members))
            {
                membershipsDiffer = true;
                break;
            }
        }
    }

    if (membershipsDiffer)
    {
        SaveGroupsNow();
    }
}

void Service::SaveGroupsNow()
{
    m_settings.activeGroup = m_activeSlot;
    (void)m_settings.Save();

    if (!core::SaveGroups(m_groups, core::DefaultGroupFile()))
    {
        LogError(L"cannot write the group file");
    }
}

ActionResult Service::ApplyAllGroups() const
{
    // The monitors are captured on the worker thread: the display stack is the one input that can
    // change while a render is running, and the group can only be rendered against the geometry
    // it will be applied to.
    return ApplyGroups(m_groups, core::CaptureMonitors());
}

void Service::BeginWork(std::wstring label, std::function<ActionResult()> work, bool clearDirty)
{
    if (m_busy)
    {
        return;
    }

    m_busy = true;
    m_busyLabel = std::move(label);
    m_clearDirtyOnSuccess = clearDirty;

    // Paint the busy state before the work starts. Rendering is a synchronous decode plus a write
    // per screen; without this the panel shows the old frame for the whole duration and the
    // spinner never appears at all.
    m_panel.Refresh();
    if (m_panel.Handle() != nullptr)
    {
        ::UpdateWindow(m_panel.Handle());
    }

    if (m_worker.joinable())
    {
        m_worker.join();
    }

    m_workResult = WorkResult{};
    m_worker = std::thread([this, work = std::move(work)]() {
        // The wallpaper API is COM, and the worker is a fresh apartment that has to join one or
        // every call inside it fails with CO_E_NOTINITIALIZED.
        const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        ActionResult result = work();

        if (SUCCEEDED(com))
        {
            ::CoUninitialize();
        }

        m_workResult = WorkResult{ result.ok, std::move(result.message) };
        ::PostMessageW(m_window, kWorkDone, 0, 0);
    });
}

void Service::FinishWork()
{
    if (m_worker.joinable())
    {
        m_worker.join();
    }

    m_busy = false;
    m_busyLabel.clear();

    if (m_workResult.ok && m_clearDirtyOnSuccess)
    {
        for (auto& group : m_groups)
        {
            group.dirty = false;
        }
        SaveGroupsNow();
    }

    m_clearDirtyOnSuccess = false;
    SetStatus(m_workResult.message, !m_workResult.ok);

    if (m_pendingInstall)
    {
        m_pendingInstall = false;
        if (m_workResult.ok && !m_downloadedUpdate.empty())
        {
            BeginSelfUpdate();
        }
    }
}

void Service::ScheduleUpdateCheck()
{
    if (m_window == nullptr)
    {
        return;
    }

    ::KillTimer(m_window, kUpdateCheckTimerId);
    if (!m_settings.autoCheckUpdate)
    {
        return;
    }

    DWORD delay = kUpdateStartDelayMs;
    if (const auto remaining = UpdateDelayMs(m_settings.lastUpdateCheck); remaining.has_value())
    {
        delay = std::clamp(*remaining, static_cast<DWORD>(1000), kUpdateIntervalMs);
    }
    ::SetTimer(m_window, kUpdateCheckTimerId, delay, nullptr);
}

void Service::BeginUpdateCheck(bool manual)
{
    if (m_updateChecking || m_window == nullptr)
    {
        return;
    }

    const auto current = core::AppVersion::Parse(std::wstring{ wintools::kVersion });
    if (!current.has_value())
    {
        util::LogError(L"cannot compare against the compiled version {}",
            std::wstring_view{ wintools::kVersion });
        if (manual)
        {
            SetStatus(util::i18n::Format(L"检查更新失败：{}", wintools::kVersion), true);
        }
        return;
    }

    if (m_updateWorker.joinable())
    {
        m_updateWorker.join();
    }

    m_updateChecking = true;
    m_updateCheckManual = manual;
    m_updateResult = UpdateWorkerResult{};

    if (manual)
    {
        // Pin the click to a line in the log, and repaint at once: the answer takes a network
        // round trip, and up to a source timeout when one of them is down.
        util::LogInfo(L"update check started (manual)");
        m_panel.Refresh();
    }

    // Everything the worker touches is copied in: m_settings is owned by the UI thread and may
    // be saved while a fetch is in flight.
    const std::wstring source = m_settings.updateSource;
    const std::wstring preferred = m_settings.updateLastGoodSource;
    m_updateWorker = std::thread([this, source, preferred, current]() {
        UpdateWorkerResult result;
        const core::UpdateCheck check = core::CheckForUpdate(
            [](std::wstring_view url, std::string& body) {
                const auto response = platform::win::HttpGetText(url);
                if (!response.ok)
                {
                    util::LogWarn(L"update source {} unavailable: {}", url, response.error);
                    return false;
                }
                body = response.body;
                return true;
            },
            source, *current, preferred);

        result.ok = !check.error;
        result.message = check.message;
        result.feed = check.feed;
        result.source = check.source;
        m_updateResult = std::move(result);
        ::PostMessageW(m_window, kUpdateDone, 0, 0);
    });
}

void Service::FinishUpdateCheck()
{
    if (m_updateWorker.joinable())
    {
        m_updateWorker.join();
    }
    m_updateChecking = false;

    // Record the attempt, not just a success: a source that is down must not turn into a
    // request on every timer tick.
    m_settings.lastUpdateCheck = UpdateTimeLabel();
    if (!m_updateResult.source.empty() && m_updateResult.source != m_settings.updateLastGoodSource)
    {
        // Learn where this network can actually reach a feed. Without this a client that has to
        // fall back to GitHub pays the Gitee timeout on every check, forever.
        util::LogInfo(L"update source remembered: {}", m_updateResult.source);
        m_settings.updateLastGoodSource = m_updateResult.source;
    }
    (void)m_settings.Save();

    m_updateFeed.reset();
    std::wstring answer; // what the user is told, and what the tray repeats when the panel is away

    if (m_updateResult.ok && m_updateResult.feed.has_value())
    {
        m_updateFeed = m_updateResult.feed;
        m_updateDismissed = false;

        answer = util::i18n::Format(L"有新版本 {}", m_updateFeed->version.Text());
        SetStatus(answer);
    }
    else if (m_updateCheckManual)
    {
        answer = m_updateResult.ok
            ? std::wstring{ util::i18n::Cstr(L"当前已是最新版本") }
            : util::i18n::Format(L"检查更新失败：{}", m_updateResult.message);
        SetStatus(answer, !m_updateResult.ok);
    }
    else if (!m_updateResult.ok)
    {
        // An opt-in background check must stay quiet. The worker already logged each source.
        util::LogWarn(L"background update check failed: {}", m_updateResult.message);
    }

    // A background check that finds nothing stays silent; one the user asked for does not. With
    // the panel off screen the footer cannot answer, so the balloon has to - otherwise the tray
    // menu's "检查更新" looks just as dead as the panel link did.
    if (!answer.empty() && m_panel.Handle() != nullptr && !::IsWindowVisible(m_panel.Handle()))
    {
        m_tray.ShowInfo(L"WinTools", util::i18n::Text(answer));
    }

    if (m_updateCheckManual)
    {
        // Pairs with "started (manual)": between the two lines the log answers the only question a
        // silent-looking click raises - did it reach the network, or nowhere at all?
        if (!m_updateResult.ok)
        {
            util::LogInfo(L"update check finished (manual): failed - {}", m_updateResult.message);
        }
        else if (m_updateFeed.has_value())
        {
            util::LogInfo(L"update check finished (manual): new version {}",
                m_updateFeed->version.Text());
        }
        else
        {
            util::LogInfo(L"update check finished (manual): already up to date");
        }
    }

    m_panel.Refresh();
    ScheduleUpdateCheck();
}

void Service::BeginSelfUpdate()
{
    // Flush open notes first: the snapshot below is the rollback point, and it has to contain
    // what the user typed, not what happened to be on disk when the download started.
    m_notes.SaveAll();

    std::wstring error;
    if (!core::SnapshotBeforeUpdate(util::AppDataFolder(), wintools::kVersion, error))
    {
        LogError(L"update snapshot failed: {}", error);
        SetStatus(util::i18n::Format(L"更新准备失败：{}", error), true);
        return;
    }

    std::array<wchar_t, 32768> buffer{};
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(),
        static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
    {
        SetStatus(util::i18n::Format(L"更新准备失败：{}", L"cannot locate the running exe"), true);
        return;
    }
    const std::filesystem::path target{ std::wstring{ buffer.data(), length } };

    SetStatus(util::i18n::Cstr(L"正在更新…"));
    if (!StartUpdaterProcess(target, m_downloadedUpdate, ::GetCurrentProcessId(), error))
    {
        LogError(L"cannot start the updater: {}", error);
        SetStatus(util::i18n::Format(L"无法启动更新程序：{}", error), true);
        return;
    }

    // The updater waits on this process before touching the exe. A normal quit is what releases
    // the file lock and also saves every remaining piece of state.
    ::PostQuitMessage(0);
}

bool Service::DarkTheme() const
{
    const int mode = std::clamp(m_settings.theme, 0, 2);
    if (mode != 2)
    {
        return mode == 1;
    }

    // Follow the system: the same value Settings > Personalisation writes.
    DWORD light = 1;
    DWORD size = sizeof(light);
    if (::RegGetValueW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size)
        != ERROR_SUCCESS)
    {
        return false;
    }

    return light == 0;
}

// --- Actions --------------------------------------------------------------------------------

void Service::HandleAction(const PanelRequest& request)
{
    switch (request.action)
    {
        case PanelAction::PickWallpaper:
        {
            if (m_groups.empty())
            {
                SetStatus(L"还没有组合，先接一台显示器", true);
                return;
            }

            const int slot = ClampActiveSlot(m_activeSlot);
            core::Group& group = m_groups[static_cast<size_t>(slot)];
            const std::wstring label = core::GroupLabel(group, core::CaptureMonitors());

            const auto chosen =
                PickImageFile(m_panel.Handle(),
                    util::i18n::Format(L"选择「{}」的壁纸", label));
            if (!chosen.has_value())
            {
                return;
            }

            // A different picture means the crop framed for the previous one is meaningless, so
            // the framing starts over. Re-picking the same file keeps it.
            if (!core::Monitor::IEquals(group.image, chosen->wstring()))
            {
                group.view = core::WallpaperView{};
            }

            group.image = chosen->wstring();
            group.dirty = false;
            SaveGroupsNow();

            BeginWork(L"生成中…", [this] { return ApplyAllGroups(); }, true);
            return;
        }

        case PanelAction::RecropWallpaper:
        {
            const int slot = ClampActiveSlot(m_activeSlot);
            if (m_groups.empty() || slot >= static_cast<int>(m_groups.size())
                || m_groups[static_cast<size_t>(slot)].image.empty())
            {
                SetStatus(L"这个组合还没选图片", true);
                return;
            }

            BeginWork(L"生成中…", [this] { return ApplyAllGroups(); }, true);
            return;
        }

        case PanelAction::RotateWallpaper:
        {
            if (m_groups.empty())
            {
                SetStatus(L"还没有组合，先接一台显示器", true);
                return;
            }

            const int slot = ClampActiveSlot(m_activeSlot);
            core::Group& group = m_groups[static_cast<size_t>(slot)];
            if (group.image.empty())
            {
                SetStatus(L"这个组合还没选图片", true);
                return;
            }

            // A quarter-turn clockwise on the picture itself, not a change of picture: a short
            // click on the arc handle steps 90 degrees, while a drag uses CommitWallpaperRotation
            // to set an arbitrary angle. The turn is part of the view, so it persists with the
            // group, feeds the map preview, and re-renders the slices through ApplyAllGroups.
            group.view.rotation = core::ClampView(
                core::WallpaperView{ group.view.zoom, group.view.focusX, group.view.focusY,
                    group.view.rotation + 90.0 }).rotation;
            group.dirty = false;
            SaveGroupsNow();
            util::LogInfo(L"组合 {}: 壁纸旋转到 {:.1f} 度", core::GroupLabel(group, core::CaptureMonitors()),
                group.view.rotation);

            BeginWork(L"生成中…", [this] { return ApplyAllGroups(); }, true);
            return;
        }

        case PanelAction::CommitWallpaperRotation:
        {
            if (m_groups.empty() || request.index < 0
                || request.index >= static_cast<int>(m_groups.size()))
            {
                return;
            }

            core::Group& group = m_groups[static_cast<size_t>(request.index)];
            if (group.image.empty())
            {
                return;
            }

            // One write and one re-render for the whole gesture, not one per mouse move.
            group.view = core::ClampView(request.view);
            group.dirty = false;
            SaveGroupsNow();
            util::LogInfo(L"组合 {}: 壁纸旋转到 {:.1f} 度", core::GroupLabel(group, core::CaptureMonitors()),
                group.view.rotation);

            BeginWork(L"生成中…", [this] { return ApplyAllGroups(); }, true);
            return;
        }

        case PanelAction::RestoreWallpaper:
        {
            const WaitCursor busy;
            const ActionResult result = RestoreSavedWallpaper();
            SetStatus(result.message, !result.ok);
            return;
        }

        case PanelAction::RefreshLayout:
        {
            BeginWork(L"获取中…",
                [] {
                    int windows = 0;
                    int monitors = 0;
                    return CaptureCurrentLayout(windows, monitors);
                },
                false);
            return;
        }

        case PanelAction::ApplyLayout:
        {
            const WaitCursor busy;
            core::RestoreStats stats{};
            const ActionResult result = RestoreSavedLayout(stats);
            SetStatus(result.message, !result.ok);
            return;
        }

        case PanelAction::ToggleAutoLayout:
            m_settings.autoRestoreLayout = !m_settings.autoRestoreLayout;
            (void)m_settings.Save();
            SetStatus(m_settings.autoRestoreLayout ? L"自动恢复已开" : L"自动恢复已关");
            return;

        case PanelAction::ToggleAutostart:
        {
            const bool target = !IsAutostartEnabled();
            if (!SetAutostart(target))
            {
                SetStatus(L"开机自启修改失败", true);
                return;
            }

            m_settings.startWithWindows = target;
            (void)m_settings.Save();
            SetStatus(target ? L"开机自启已开" : L"开机自启已关");
            return;
        }

        case PanelAction::ToggleNoteRestore:
            m_settings.restoreNotes = !m_settings.restoreNotes;
            (void)m_settings.Save();
            SetStatus(m_settings.restoreNotes ? L"便签重启恢复已开，下次重启生效"
                                              : L"便签重启恢复已关，下次重启生效");
            return;

        case PanelAction::ToggleWindow:
        {
            const ActionResult result = SetWindowRemembered(request.index, request.value != 0);
            SetStatus(result.message, !result.ok);
            return;
        }

        case PanelAction::SetWallpaperView:
        {
            // Deliberately silent: the status line is for outcomes, and "moved the picture" is
            // not one. Nothing is re-rendered here either - the user decides when to spend a
            // second re-encoding the slices.
            if (request.index < 0 || request.index >= static_cast<int>(m_groups.size()))
            {
                return;
            }

            core::Group& group = m_groups[static_cast<size_t>(request.index)];
            group.view = core::ClampView(request.view);

            // The desktop still shows the old crop, so the group is genuinely not applied.
            group.dirty = !group.image.empty();
            SaveGroupsNow();
            m_panel.Refresh();
            return;
        }

        case PanelAction::SetActiveGroup:
        {
            m_activeSlot = ClampActiveSlot(request.index);
            SaveGroupsNow();

            const core::Group& group = m_groups[static_cast<size_t>(m_activeSlot)];
            const std::wstring label =
                core::GroupLabel(group, core::CaptureMonitors());
            SetStatus(util::i18n::Format(L"已选中 {}：更换壁纸 / 应用壁纸都以它为准", label));
            return;
        }

        case PanelAction::MoveMonitorToGroup:
        {
            std::vector<std::vector<std::wstring>> before;
            before.reserve(m_groups.size());
            for (const auto& group : m_groups)
            {
                before.push_back(group.members);
            }

            const int moved = core::MoveMonitorToGroup(m_groups, request.target, request.index);
            if (moved < 0)
            {
                SetStatus(L"没有可以移动的屏", true);
                return;
            }

            // A group whose members changed has a different canvas, so the framing that was set
            // for the old one no longer describes the same crop. Reset it - and say so, because
            // the desktop still shows the old one until 应用壁纸 runs.
            for (auto& group : m_groups)
            {
                if (ContainsMembers(before, group.members))
                {
                    continue;
                }

                group.view = core::WallpaperView{};
                if (!group.image.empty())
                {
                    group.dirty = true;
                }
            }

            core::NormaliseGroups(m_groups, core::CaptureMonitors());
            m_activeSlot = ClampActiveSlot(m_activeSlot);
            SaveGroupsNow();

            const std::wstring label =
                core::GroupLabel(m_groups[static_cast<size_t>(moved)], core::CaptureMonitors());
            const int screens = static_cast<int>(m_groups[static_cast<size_t>(moved)].members.size());

            if (request.index < 0)
            {
                SetStatus(util::i18n::Format(L"已单独成组 {}（{} 台）", label, screens));
            }
            else
            {
                SetStatus(util::i18n::Format(L"已加入 {}（{} 台）", label, screens));
            }

            return;
        }

        case PanelAction::SetTheme:
        {
            // Picked, not cycled: the config page shows all three choices at once, so it says which
            // one it wants instead of asking for "the next one".
            m_settings.theme = std::clamp(request.value, 0, 2);
            (void)m_settings.Save();
            SetStatus(m_settings.theme == 0 ? L"主题：浅色"
                    : m_settings.theme == 1 ? L"主题：深色"
                                            : L"主题：跟随系统");
            return;
        }

        case PanelAction::SetLanguage:
        {
            m_settings.language = std::clamp(request.value, 0, 2);
            (void)m_settings.Save();
            util::i18n::SetLanguage(static_cast<util::i18n::Language>(m_settings.language));
            // SetStatus refreshes the panel, which re-measures every label in the new language.
            SetStatus(std::wstring{ util::i18n::LanguageLabel() });
            return;
        }

        case PanelAction::OpenLog:
        {
            const std::filesystem::path file = util::LogFile();
            std::error_code missing;
            if (!std::filesystem::exists(file, missing))
            {
                // The first write happens at startup, so this is unusual; say which file is
                // missing instead of letting ShellExecute turn it into a generic Windows dialog.
                util::LogWarn(L"log file is missing: {}", file.wstring());
                SetStatus(L"日志文件还没生成", true);
                return;
            }

            const auto launch = [this](const wchar_t* verb, const wchar_t* target,
                                    const std::wstring& arguments) {
                return reinterpret_cast<INT_PTR>(::ShellExecuteW(m_panel.Handle(), verb, target,
                    arguments.empty() ? nullptr : arguments.c_str(), nullptr, SW_SHOWNORMAL));
            };

            if (launch(L"open", file.c_str(), {}) <= 32
                && launch(L"open", L"notepad.exe", std::format(L"\"{}\"", file.wstring())) <= 32
                && launch(L"open", L"explorer.exe",
                       std::format(L"/select,\"{}\"", file.wstring()))
                       <= 32)
            {
                util::LogWarn(L"no opener worked for the log: {}", file.wstring());
                SetStatus(L"无法打开日志文件", true);
            }
            return;
        }

        case PanelAction::ToggleCanvas:
        {
            // The widget is shelved; a stale panel or an old settings file must not bring it back.
            // The message has no sender any more, but keeping the case costs nothing and the body
            // stays ready for the day the feature returns.
            if constexpr (kDesktopWidgetsEnabled)
            {
                if (m_settings.canvasEnabled)
                {
                    m_settings.canvasEnabled = false;
                    m_canvas.Stop();
                    (void)m_settings.Save();
                    SetStatus(L"桌面日历已隐藏");
                    return;
                }

                // A settings file written before the widget existed carries no position; the
                // default lands it on the display the cursor is on rather than in the top-left
                // corner.
                if (m_settings.canvas.x == 0 && m_settings.canvas.y == 0)
                {
                    m_settings.canvas = DefaultCanvasRect();
                }

                m_settings.canvasEnabled = true;
                (void)m_settings.Save();

                SetStatus(StartCanvas() ? L"桌面日历已显示" : L"桌面日历启动失败，详见日志",
                    !m_canvas.Running());
            }
            return;
        }

        case PanelAction::MoveCanvas:
        {
            if constexpr (kDesktopWidgetsEnabled)
            {
                const core::CanvasRect moved = core::ClampCanvas(
                    core::CanvasRect{ request.x, request.y, m_settings.canvas.size },
                    core::CaptureMonitors());

                if (moved == m_settings.canvas)
                {
                    return;
                }

                m_settings.canvas = moved;
                (void)m_settings.Save();
                m_canvas.SetRect(moved);
                m_panel.Refresh();
            }
            return;
        }

        case PanelAction::NewNote:
            m_notes.NewNote();
            return;

        case PanelAction::ShowNotes:
            m_notes.ShowAll();
            return;

        case PanelAction::ReopenNote:
        {
            const bool ok = m_notes.Reopen(request.target);
            SetStatus(ok ? util::i18n::Cstr(L"便签已恢复") : util::i18n::Cstr(L"便签已不存在"),
                !ok);
            if (ok)
            {
                m_panel.Refresh();
            }
            return;
        }

        case PanelAction::ForgetNote:
        {
            const bool ok = m_notes.Forget(request.target);
            SetStatus(ok ? util::i18n::Cstr(L"已从历史中删除") : util::i18n::Cstr(L"便签已不存在"),
                !ok);
            m_panel.Refresh();
            return;
        }

        case PanelAction::SetNotePalette:
        {
            core::SetNotePalette(request.colors);

            // What is written is what was accepted, not what was asked for: SetNotePalette falls
            // back to the built-in papers when the count is out of range, and a settings file that
            // disagreed with the notes on screen would come back different on the next start.
            std::wstring value;
            if (core::NotePaletteIsCustom())
            {
                for (int index = 0; index < core::NoteThemeCount(); ++index)
                {
                    if (index > 0)
                    {
                        value += L",";
                    }
                    value += std::format(L"{:06X}", core::NoteThemeAt(index).paper);
                }
            }

            if (value != m_settings.noteThemes)
            {
                m_settings.noteThemes = value;
                (void)m_settings.Save();
            }

            m_notes.ApplyPalette(request.index);
            SetStatus(util::i18n::Format(L"便签配色：{} 种", core::NoteThemeCount()));
            return;
        }

        case PanelAction::SetHotkey:
        {
            const int action = request.value;
            if (action < 1 || action > static_cast<int>(core::kHotkeyActions.size()))
            {
                return;
            }

            core::Hotkey hotkey;
            hotkey.modifiers = static_cast<unsigned int>(request.index & 0xFFFF);
            hotkey.vk = static_cast<unsigned int>((request.index >> 16) & 0xFFFF);
            const core::HotkeyAction changed = static_cast<core::HotkeyAction>(action);
            m_settings.hotkeys.At(changed) = hotkey;

            // RegisterHotKey keys on the combination, not on our action id: two rows holding the
            // same combination would leave the second one unregistered. Move the binding rather
            // than silently accepting a duplicate.
            if (hotkey.Bound())
            {
                for (const core::HotkeyAction other : core::kHotkeyActions)
                {
                    if (other == changed)
                    {
                        continue;
                    }

                    const core::Hotkey& taken = m_settings.hotkeys.At(other);
                    if (taken.Bound() && taken.modifiers == hotkey.modifiers && taken.vk == hotkey.vk)
                    {
                        m_settings.hotkeys.At(other) = core::Hotkey{};
                    }
                }
            }

            (void)m_settings.Save();
            RegisterHotkeys();
            SetStatus(hotkey.Bound()
                    ? util::i18n::Format(L"快捷键已设为 {}", core::DescribeHotkey(hotkey))
                    : util::i18n::Cstr(L"快捷键已清除"));
            return;
        }

        case PanelAction::ResetHotkeys:
            m_settings.hotkeys = core::DefaultHotkeys();
            (void)m_settings.Save();
            RegisterHotkeys();
            SetStatus(util::i18n::Cstr(L"快捷键已恢复默认"));
            return;

        case PanelAction::SuspendHotkeys:
            UnregisterHotkeys();
            return;

        case PanelAction::ResumeHotkeys:
            RegisterHotkeys();
            return;

        case PanelAction::ToggleAutoCheck:
            m_settings.autoCheckUpdate = !m_settings.autoCheckUpdate;
            (void)m_settings.Save();
            ScheduleUpdateCheck();
            SetStatus(util::i18n::Cstr(m_settings.autoCheckUpdate
                    ? L"自动检查更新已开"
                    : L"自动检查更新已关"));
            return;

        case PanelAction::CheckUpdate:
            BeginUpdateCheck(true);
            return;

        case PanelAction::ShowUpdateNotes:
            m_panel.ShowUpdateNotes();
            return;

        case PanelAction::InstallUpdate:
        {
            if (!m_updateFeed.has_value())
            {
                SetStatus(util::i18n::Cstr(L"没有可用的更新包"), true);
                return;
            }

            const core::UpdateFeed feed = *m_updateFeed;
            const std::filesystem::path target = core::UpdateFilePath(feed);
            m_downloadedUpdate.clear();
            m_pendingInstall = true;

            BeginWork(util::i18n::Cstr(L"下载中…"), [this, feed, target]() {
                const auto fetch = [](std::wstring_view url, const std::filesystem::path& path,
                                       std::string& error) {
                    const auto outcome = platform::win::HttpDownloadToFile(url, path);
                    if (!outcome.ok)
                    {
                        error = util::ToUtf8(outcome.error);
                        return false;
                    }
                    return true;
                };
                const auto hash = [](const std::filesystem::path& path, std::string& hex) {
                    return platform::win::Sha256File(path, hex);
                };

                const auto result = core::DownloadUpdate(fetch, hash, feed, target);
                if (result.ok)
                {
                    m_downloadedUpdate = target;
                }
                return ActionResult{ result.ok,
                    result.ok ? util::i18n::Format(L"更新包已下载并校验")
                              : util::i18n::Format(L"下载更新失败：{}", result.message) };
            }, false);
            return;
        }

        case PanelAction::Quit:
            m_notes.SaveAll();
            ::PostQuitMessage(0);
            return;
    }
}

bool Service::StartCanvas()
{
    const std::vector<core::MonitorSnapshot> monitors = core::CaptureMonitors();
    if (monitors.empty())
    {
        SetStatus(L"没有检测到显示器", true);
        return false;
    }

    m_settings.canvas = core::ClampCanvas(m_settings.canvas, monitors);

    m_canvas.SetSweep(m_settings.canvasSweep);

    if (!m_canvas.Start(m_instance, m_settings.canvas))
    {
        LogError(L"the desktop canvas could not be started");
        return false;
    }

    return true;
}

void Service::StopCanvas()
{
    m_canvas.Stop();
}

void Service::SyncCanvas()
{
    if (!m_canvas.Running())
    {
        return;
    }

    const std::vector<core::MonitorSnapshot> monitors = core::CaptureMonitors();
    if (monitors.empty())
    {
        return;
    }

    const core::CanvasRect clamped = core::ClampCanvas(m_settings.canvas, monitors);
    if (clamped == m_settings.canvas)
    {
        return;
    }

    m_settings.canvas = clamped;
    (void)m_settings.Save();
    m_canvas.SetRect(clamped);
}

void Service::OnDisplaysSettled(bool isRetry)
{
    std::wstring report;

    // The arrangement has to follow the hardware: a display that came back is a member of the
    // group it left, and a display that is gone must not keep its group's canvas stretched.
    SyncGroups();

    // Same rule for the calendar: a widget left half off a display that just disappeared is one
    // the user cannot see and cannot grab again.
    SyncCanvas();

    // And for the notes, which are worse off: the text below the edge of the screen is text the
    // user cannot scroll to.
    m_notes.SyncMonitors(core::CaptureMonitors());

    // The wallpaper is deliberately not re-applied here. Re-rendering and re-setting several
    // pieces takes long enough to be felt, it flashes the desktop, and the slice files are already
    // on disk for the geometry they were made for - so the honest thing is to leave it alone and
    // let the user decide with 应用壁纸. Only the layout, which is instant, is put back.
    if (m_settings.autoRestoreLayout)
    {
        core::RestoreStats stats{};
        // Homecoming, not the exact layout: this runs on every display change, including the ones
        // the user caused by leaning on a monitor's input button, and it must not undo whatever
        // they arranged since the last capture. See core::RestoreScope.
        const ActionResult result =
            RestoreSavedLayout(stats, core::RestoreScope::Homecoming);

        report = result.message;
        LogInfo(L"automatic restore after display change: {} ({} displays attached)",
            util::i18n::Text(result.message), core::CaptureMonitors().size());

        // A resume does not hand over a finished desktop: the displays come back one at a time and
        // the shell is still starting, so the first attempt can find a topology that is not the
        // one the user will end up with - every window skipped as "display not attached". Coming
        // back once, 1.5 seconds later, covers that window without turning into a polling loop.
        if (!isRetry && stats.moved == 0 && stats.alreadyInPlace == 0
            && stats.skippedMissingMonitor > 0)
        {
            LogInfo(L"no display matched the layout yet, asking again in {} ms", kRetryDelayMs);
            ::SetTimer(m_window, kRetryTimerId, kRetryDelayMs, nullptr);
        }

        // Only when something actually moved. "Ran and found nothing to do" is not a restore.
        if (result.ok && stats.moved > 0)
        {
            m_settings.lastAutoRestore = NowLabel();
            (void)m_settings.Save();
        }
    }

    m_panel.Refresh();

    if (report.empty())
    {
        return;
    }

    SetStatus(util::i18n::Format(L"显示器变化：{}", report));

    // Only when nobody is looking at the panel: a balloon over an open window is noise.
    if (m_panel.Handle() != nullptr && !::IsWindowVisible(m_panel.Handle()))
    {
        m_tray.ShowInfo(L"WinTools", util::i18n::Text(report));
    }
}

void Service::SetStatus(std::wstring text, bool isError)
{
    m_status = std::move(text);
    m_statusIsError = isError;

    SyncTrayTooltip();
    m_panel.Refresh();
}

void Service::SyncTrayTooltip()
{
    // No dangling separator when there is nothing to report.
    m_tray.SetTooltip(m_status.empty()
            ? std::wstring{ L"WinTools" }
            : L"WinTools · " + std::wstring{ util::i18n::Text(m_status) });
}

PanelState Service::ReadState()
{
    SyncGroups();

    PanelState state;

    state.monitors = core::CaptureMonitors();
    state.groups = m_groups;
    state.activeSlot = ClampActiveSlot(m_activeSlot);

    state.autoLayout = m_settings.autoRestoreLayout;
    state.autostart = IsAutostartEnabled();
    state.noteRestore = m_settings.restoreNotes;
    state.darkTheme = DarkTheme();
    // The settings as written, alongside the theme the window actually resolved to: the config
    // page offers "follow the system" as a choice, so it needs the choice, not its outcome.
    state.theme = std::clamp(m_settings.theme, 0, 2);
    state.language = std::clamp(m_settings.language, 0, 2);

    // The switch reflects the window, not the setting: a widget that failed to start must not
    // look switched on.
    state.canvasOn = m_canvas.Running();
    state.canvas = m_settings.canvas;

    state.busy = m_busy;
    state.busyLabel = m_busyLabel;
    state.updateChecking = m_updateChecking;

    state.status = m_status;
    state.statusIsError = m_statusIsError;

    // Starred notes ride at the front, newest still winning inside each group: the archive is
    // newest-first, and a stable partition keeps that order where the star does not care.
    state.archivedNotes = m_notes.Archived();
    std::stable_sort(state.archivedNotes.begin(), state.archivedNotes.end(),
        [](const core::ArchiveRecord& left, const core::ArchiveRecord& right)
        { return left.starred && !right.starred; });
    state.hotkeys = m_settings.hotkeys;
    state.snapshotAt = SavedLayoutTimestamp();

    state.autoCheckUpdate = m_settings.autoCheckUpdate;
    state.lastUpdateCheck = m_settings.lastUpdateCheck;
    if (m_updateFeed.has_value())
    {
        state.updateAvailableVersion = m_updateFeed->version.Text();
        state.updateNotesZh = m_updateFeed->notesZh;
        state.updateNotesEn = m_updateFeed->notesEn;
        state.updateSize = m_updateFeed->size;
        state.updateSha256 = m_updateFeed->sha256;
    }

    if (const auto saved = LoadSavedLayout(); saved.has_value())
    {
        state.windows.reserve(saved->windows.size());

        for (const auto& window : saved->windows)
        {
            WindowRow row;
            row.title = window.title;
            row.app = window.appName;
            row.monitorPath = window.monitorPath;
            row.enabled = window.enabled;

            // The position label is read straight from the capture rather than configured here:
            // a dock mode carries its own ratio, and a floating window shows its size.
            row.where = core::IsDockMode(window.dock)
                ? core::DescribeDock(window.dock, window.splitPermille)
                : std::format(L"{}×{}", window.rect.width, window.rect.height);

            state.windows.push_back(std::move(row));
        }
    }

    return state;
}

} // namespace wintools::app
