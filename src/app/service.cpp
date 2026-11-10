#include "app/service.h"

#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <format>
#include <utility>

#include "app/actions.h"
#include "app/startup.h"
#include "core/desktop_wallpaper.h"
#include "core/layout_math.h"
#include "core/wallpaper_store.h"
#include "util/log.h"
#include "util/path.h"

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
    kMenuQuit,
};

/// "11-07 16:31" in local time, for the "last automatic restore" line.
std::wstring NowLabel()
{
    SYSTEMTIME time{};
    ::GetLocalTime(&time);
    return std::format(L"{:02}-{:02} {:02}:{:02}", time.wMonth, time.wDay, time.wHour, time.wMinute);
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
}

Service::~Service()
{
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
    return true;
}

bool Service::Start(bool showPanel)
{
    m_settings = Settings::Load();
    m_status.clear();

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

                case kMenuQuit:
                    HandleAction(PanelRequest{ PanelAction::Quit });
                    return 0;

                default:
                    break;
            }
            break;

        case kQuitMessage:
            ::PostQuitMessage(0);
            return 0;

        case WM_TIMER:
            if (wparam == kRetryTimerId)
            {
                ::KillTimer(m_window, kRetryTimerId);
                OnDisplaysSettled(true);
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

    ::AppendMenuW(menu, MF_STRING, kMenuOpen, L"面板");
    ::SetMenuDefaultItem(menu, kMenuOpen, FALSE);

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kMenuCapture, L"刷新布局");
    ::AppendMenuW(menu, MF_STRING, kMenuApply, L"恢复布局");

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kMenuWallpaper, L"更换壁纸");
    ::AppendMenuW(menu, MF_STRING, kMenuRestoreWallpaper, L"还原壁纸");

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kMenuQuit, L"退出");

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
                PickImageFile(m_panel.Handle(), L"选择「" + label + L"」的壁纸");
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
            SetStatus(std::format(L"已选中 {}：更换壁纸 / 应用壁纸都以它为准", label));
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
                SetStatus(std::format(L"已单独成组 {}（{} 台）", label, screens));
            }
            else
            {
                SetStatus(std::format(L"已加入 {}（{} 台）", label, screens));
            }

            return;
        }

        case PanelAction::CycleTheme:
        {
            m_settings.theme = (std::clamp(m_settings.theme, 0, 2) + 1) % 3;
            (void)m_settings.Save();
            SetStatus(m_settings.theme == 0 ? L"主题：浅色"
                    : m_settings.theme == 1 ? L"主题：深色"
                                            : L"主题：跟随系统");
            return;
        }

        case PanelAction::OpenLog:
        {
            const std::filesystem::path file = util::LogFile();
            const HINSTANCE opened =
                ::ShellExecuteW(m_panel.Handle(), L"open", file.c_str(), nullptr, nullptr,
                    SW_SHOWNORMAL);

            if (reinterpret_cast<INT_PTR>(opened) <= 32)
            {
                SetStatus(L"打不开日志文件", true);
            }
            return;
        }

        case PanelAction::Quit:
            ::PostQuitMessage(0);
            return;
    }
}

void Service::OnDisplaysSettled(bool isRetry)
{
    std::wstring report;

    // The arrangement has to follow the hardware: a display that came back is a member of the
    // group it left, and a display that is gone must not keep its group's canvas stretched.
    SyncGroups();

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
            result.message, core::CaptureMonitors().size());

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

    SetStatus(L"显示器变化：" + report);

    // Only when nobody is looking at the panel: a balloon over an open window is noise.
    if (m_panel.Handle() != nullptr && !::IsWindowVisible(m_panel.Handle()))
    {
        m_tray.ShowInfo(L"WinTools", report);
    }
}

void Service::SetStatus(std::wstring text, bool isError)
{
    m_status = std::move(text);
    m_statusIsError = isError;

    // Every record is kept, including the successes: three lines of context is what turns "贴图
    // 失败" into something a user can act on. A failure is never dropped early - it is only
    // pushed out by three later records.
    if (!m_status.empty())
    {
        m_history.push_front(StatusEntry{ m_status,
            isError ? StatusKind::Error : StatusKind::Ok });
        while (m_history.size() > 3)
        {
            m_history.pop_back();
        }
    }

    SyncTrayTooltip();
    m_panel.Refresh();
}

void Service::SyncTrayTooltip()
{
    // No dangling separator when there is nothing to report.
    m_tray.SetTooltip(m_status.empty() ? std::wstring{ L"WinTools" } : L"WinTools · " + m_status);
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
    state.darkTheme = DarkTheme();

    state.busy = m_busy;
    state.busyLabel = m_busyLabel;

    state.status = m_status;
    state.statusIsError = m_statusIsError;
    state.history.assign(m_history.begin(), m_history.end());

    state.snapshotAt = SavedLayoutTimestamp();

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
