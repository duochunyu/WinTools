#include "core/window_enum.h"

#include <Psapi.h>

#include <iterator>
#include <vector>

#include "core/monitor.h"
#include "util/text.h"

namespace wintools::core
{

namespace
{

bool HasStyle(LONG_PTR style, LONG_PTR flag)
{
    return (style & flag) != 0;
}

/// Popups that carry a caption or a min/max box are real windows (Calculator, Telegram); popups
/// without either are menus, tooltips and the shell's own flyouts.
bool IsMenuLikePopup(HWND window)
{
    const LONG_PTR style = ::GetWindowLongPtrW(window, GWL_STYLE);
    const bool isPopup = HasStyle(style, WS_POPUP);
    if (!isPopup)
    {
        return false;
    }

    const bool hasCaption = HasStyle(style, WS_CAPTION);
    const bool hasMinMax = HasStyle(style, WS_MINIMIZEBOX) || HasStyle(style, WS_MAXIMIZEBOX);
    return !(hasCaption || hasMinMax);
}

bool HasNoTitle(HWND window)
{
    const int length = ::GetWindowTextLengthW(window);
    return length <= 0;
}

/// Full path of this executable, read once.
///
/// Comparing process ids is not enough: the command-line build is a second process of the same
/// program, and from its point of view the resident instance's panel and tray windows belong to
/// "somebody else" and land in the layout. Comparing the image path catches every instance.
const std::wstring& OurExecutablePath()
{
    static const std::wstring path = [] {
        wchar_t buffer[MAX_PATH * 2]{};
        const DWORD length =
            ::GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
        return std::wstring{ buffer, length };
    }();

    return path;
}

/// The window's restored rectangle in screen coordinates - and "restored" is not the same question
/// for every window.
///
/// A maximized window reports the maximized frame from GetWindowRect, often one resize border past
/// the screen edge, so storing that as its restored geometry would leave the window full-screen
/// after it is un-maximized. A minimized window reports its icon position. WINDOWPLACEMENT carries
/// the restored rectangle for both in rcNormalPosition - but in work-area coordinates, so the
/// monitor's work-area offset has to be added back.
///
/// A snapped window is the opposite case, and it is the one that made every refresh store a
/// side-by-side pair as 铺满: Aero Snap never touches rcNormalPosition. A window snapped to the
/// right half keeps reporting the work area it filled before the snap, so reading it back says
/// "covers the whole display" - which is precisely what DockMode::Fill means, and the captured
/// layout then instructs the restore to fill the screen again. The live frame is the truth for a
/// snapped window, and it is also a valid rcNormalPosition for SetWindowPlacement, so the round
/// trip through ApplyPlacement stays exact.
RECT CapturedRectToScreen(HWND window, bool minimized, bool maximized)
{
    RECT rect{};

    if (!minimized && !maximized)
    {
        if (::GetWindowRect(window, &rect))
        {
            return rect;
        }
    }

    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);

    if (!::GetWindowPlacement(window, &placement))
    {
        ::GetWindowRect(window, &rect);
        return rect;
    }

    rect = placement.rcNormalPosition;

    const HMONITOR monitor = ::MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor == nullptr || !::GetMonitorInfoW(monitor, &monitorInfo))
    {
        return rect;
    }

    const long offsetX = monitorInfo.rcWork.left - monitorInfo.rcMonitor.left;
    const long offsetY = monitorInfo.rcWork.top - monitorInfo.rcMonitor.top;

    rect.left += offsetX;
    rect.right += offsetX;
    rect.top += offsetY;
    rect.bottom += offsetY;
    return rect;
}

} // namespace

std::wstring GetProcessPathForWindow(HWND window)
{
    DWORD pid = 0;
    ::GetWindowThreadProcessId(window, &pid);
    if (pid == 0)
    {
        return {};
    }

    // PROCESS_QUERY_LIMITED_INFORMATION is the least privilege that still allows the path query,
    // and unlike PROCESS_QUERY_INFORMATION it succeeds on many protected processes.
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr)
    {
        return {};
    }

    wchar_t buffer[MAX_PATH * 2]{};
    DWORD size = static_cast<DWORD>(std::size(buffer));

    std::wstring path;
    if (::QueryFullProcessImageNameW(process, 0, buffer, &size))
    {
        path.assign(buffer, size);
    }

    ::CloseHandle(process);
    return path;
}

std::wstring GetMonitorPath(HMONITOR monitor)
{
    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!::GetMonitorInfoW(monitor, &monitorInfo))
    {
        return {};
    }

    // QueryDisplayConfig is the only API that reports the same string IDesktopWallpaper uses.
    // The obvious alternative, EnumDisplayDevices, returns "MONITOR\\SAM0F72\\{class}\\0005" -
    // a completely different namespace that never compares equal, which silently turned every
    // restore into "that display is not attached".
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (::GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
    {
        return {};
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);

    const LONG queried = ::QueryDisplayConfig(
        QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
    if (queried != ERROR_SUCCESS)
    {
        return {};
    }

    paths.resize(pathCount);

    for (const auto& path : paths)
    {
        // Two lookups per path: the GDI name ("\\.\DISPLAY1") lives on the source half of the
        // path and the device interface path lives on the target half. Neither structure carries
        // both, so the pairing is what makes the mapping work.
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;

        if (::DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS)
        {
            continue;
        }

        if (::lstrcmpiW(source.viewGdiDeviceName, monitorInfo.szDevice) != 0)
        {
            continue;
        }

        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = path.targetInfo.adapterId;
        target.header.id = path.targetInfo.id;

        if (::DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS)
        {
            continue;
        }

        if (target.monitorDevicePath[0] == L'\0')
        {
            continue;
        }

        return target.monitorDevicePath;
    }

    return {};
}

std::wstring GetMonitorPathForWindow(HWND window)
{
    const HMONITOR monitor = ::MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    if (monitor == nullptr)
    {
        return {};
    }

    return GetMonitorPath(monitor);
}

bool IsWindowMinimized(HWND window)
{
    return ::IsIconic(window) != 0;
}

bool IsWindowMaximized(HWND window)
{
    return ::IsZoomed(window) != 0;
}

bool ShouldManageWindow(HWND window)
{
    if (window == nullptr || !::IsWindow(window))
    {
        return false;
    }

    DWORD pid = 0;
    ::GetWindowThreadProcessId(window, &pid);
    if (pid == ::GetCurrentProcessId())
    {
        // Our tray window and settings panel are tool windows anyway, but being explicit keeps
        // a future UI change from quietly entering the snapshot.
        return false;
    }

    const LONG_PTR style = ::GetWindowLongPtrW(window, GWL_STYLE);
    const LONG_PTR exStyle = ::GetWindowLongPtrW(window, GWL_EXSTYLE);

    if (!HasStyle(style, WS_VISIBLE) || !::IsWindowVisible(window))
    {
        return false;
    }

    if (HasStyle(exStyle, WS_EX_TOOLWINDOW))
    {
        return false;
    }

    // Only root windows: child windows, buttons and combo boxes are not ours to move.
    if (::GetAncestor(window, GA_ROOT) != window)
    {
        return false;
    }

    // Owned windows are dialogs and palettes that follow their owner; moving them independently
    // would fight the owner's own placement logic.
    if (::GetWindow(window, GW_OWNER) != nullptr)
    {
        return false;
    }

    if (IsMenuLikePopup(window))
    {
        return false;
    }

    if (HasNoTitle(window))
    {
        return false;
    }

    RECT rect{};
    if (!::GetWindowRect(window, &rect))
    {
        return false;
    }

    if (rect.right - rect.left <= 0 || rect.bottom - rect.top <= 0)
    {
        return false;
    }

    // A window with no thick frame is normally a splash screen or an overlay. Steam games are the
    // documented exception in the original implementation, but this build has no Steam awareness,
    // so a full-screen borderless window is left out rather than guessed at.
    const LONG_PTR frameStyle = style & (WS_CAPTION | WS_THICKFRAME);
    if (frameStyle == 0)
    {
        return false;
    }

    return true;
}

std::vector<WindowEntry> EnumerateWindows()
{
    std::vector<WindowEntry> entries;

    ::EnumWindows(
        [](HWND window, LPARAM data) -> BOOL {
            auto* out = reinterpret_cast<std::vector<WindowEntry>*>(data);

            if (!ShouldManageWindow(window))
            {
                return TRUE;
            }

            WindowEntry entry;
            entry.handle = window;
            entry.exePath = GetProcessPathForWindow(window);
            if (entry.exePath.empty())
            {
                // Elevated process we cannot inspect. Leaving it out is the honest outcome: we
                // could capture a rect but never identify the window again on restore.
                return TRUE;
            }

            if (!OurExecutablePath().empty() && Monitor::IEquals(entry.exePath, OurExecutablePath()))
            {
                // Another instance of ourselves - the resident tray process, usually. Never
                // capture our own windows; the restore could not place them anyway.
                return TRUE;
            }

            const size_t slash = entry.exePath.find_last_of(L"\\/");
            entry.appName = (slash == std::wstring::npos) ? entry.exePath : entry.exePath.substr(slash + 1);

            wchar_t title[512]{};
            const int titleLength = ::GetWindowTextW(window, title, static_cast<int>(std::size(title)));
            entry.title.assign(title, titleLength > 0 ? static_cast<size_t>(titleLength) : 0);

            entry.minimized = IsWindowMinimized(window);
            entry.maximized = IsWindowMaximized(window);
            entry.rect = CapturedRectToScreen(window, entry.minimized, entry.maximized);

            out->push_back(std::move(entry));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&entries));

    return entries;
}

} // namespace wintools::core
