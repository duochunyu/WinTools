#include "core/window_layout.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <unordered_map>

#include "util/log.h"
#include "core/window_enum.h"

namespace wintools::core
{

namespace
{

/// WINDOWPLACEMENT.rcNormalPosition is expressed in work-area coordinates, while everything else
/// in this file speaks screen coordinates. Shifting by the work-area origin bridges the two.
void ScreenToWorkArea(HMONITOR monitor, RECT& rect)
{
    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!::GetMonitorInfoW(monitor, &monitorInfo))
    {
        return;
    }

    const long offsetX = monitorInfo.rcWork.left - monitorInfo.rcMonitor.left;
    const long offsetY = monitorInfo.rcWork.top - monitorInfo.rcMonitor.top;

    rect.left -= offsetX;
    rect.right -= offsetX;
    rect.top -= offsetY;
    rect.bottom -= offsetY;
}

bool ApplyPlacementOnce(HWND window, const RECT& targetScreenRect, bool minimized, bool maximized)
{
    const HMONITOR monitor = ::MonitorFromRect(&targetScreenRect, MONITOR_DEFAULTTOPRIMARY);

    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (!::GetWindowPlacement(window, &placement))
    {
        util::LogWarn(L"GetWindowPlacement failed for {:#x}", reinterpret_cast<uintptr_t>(window));
        return false;
    }

    if (minimized)
    {
        placement.showCmd = SW_SHOWMINIMIZED;
    }
    else
    {
        placement.showCmd = SW_RESTORE;
        RECT workArea = targetScreenRect;
        ScreenToWorkArea(monitor, workArea);
        placement.rcNormalPosition = workArea;
    }

    // WPF_ASYNCWINDOWPLACEMENT lets the call succeed across thread/process boundaries without
    // waiting for the target's message loop; without it every cross-process move would block.
    placement.flags |= WPF_ASYNCWINDOWPLACEMENT;

    if (!::SetWindowPlacement(window, &placement))
    {
        return false;
    }

    // Second pass: when the window has to be maximized, the first call puts it on the right
    // monitor at the right normal size, and only then may it be told to maximize. Doing both in
    // one go leaves some apps maximized on the monitor they were on before.
    if (maximized)
    {
        placement.showCmd = SW_SHOWMAXIMIZED;
        if (!::SetWindowPlacement(window, &placement))
        {
            return false;
        }
    }

    return true;
}

Rect FromRect(const RECT& rect)
{
    return Rect{ rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top };
}

int AbsDiff(long a, long b)
{
    return static_cast<int>(std::abs(a - b));
}

bool Intersects(const RECT& rect, const Rect& area)
{
    return rect.left < area.Right() && rect.right > area.left && rect.top < area.Bottom()
        && rect.bottom > area.top;
}

bool SameRect(const Rect& left, const Rect& right)
{
    return left.left == right.left && left.top == right.top && left.width == right.width
        && left.height == right.height;
}

/// True when the middle of the window is on that display.
///
/// The centre rather than "fully contained": a window that is slightly larger than its work area -
/// a snapped one that lost a pixel to rounding, an app with a minimum size - is still at home, and
/// demanding containment would send it to the strict path for a one-pixel overflow.
bool CentreIsInside(const RECT& rect, const Rect& area)
{
    const long centreX = (rect.left + rect.right) / 2;
    const long centreY = (rect.top + rect.bottom) / 2;

    return centreX >= area.left && centreX < area.Right() && centreY >= area.top
        && centreY < area.Bottom();
}

} // namespace

bool IsDockMode(const DockMode mode)
{
    return mode != DockMode::Position;
}

std::wstring DescribeDock(const DockMode mode, int splitPermille)
{
    const int permille = std::clamp(splitPermille, kMinSplitPermille, kMaxSplitPermille);

    // 50% is the common case by a wide margin, and "左半屏" reads better than "左侧 50%". The
    // label rounds to whole percent because that is the unit people think in; the stored value
    // keeps its third decimal so rounding back does not move the window.
    const bool exactHalf = (permille == kDefaultSplitPermille);
    const int percent = static_cast<int>(std::lround(permille / 10.0));

    switch (mode)
    {
        case DockMode::Position:
            return L"原位置";
        case DockMode::Left:
            return exactHalf ? L"左半屏" : std::format(L"左侧 {}%", percent);
        case DockMode::Right:
            return exactHalf ? L"右半屏" : std::format(L"右侧 {}%", percent);
        case DockMode::Top:
            return exactHalf ? L"上半屏" : std::format(L"上侧 {}%", percent);
        case DockMode::Bottom:
            return exactHalf ? L"下半屏" : std::format(L"下侧 {}%", percent);
        case DockMode::Maximize:
            return L"最大化";
        case DockMode::Fill:
            return L"铺满";
        case DockMode::Count:
            break;
    }

    return L"原位置";
}

Rect ComputeDockRect(const DockMode mode, const Rect& workArea, int splitPermille)
{
    // One division per axis and the far side derived by subtraction, so the two sides always add
    // up to the whole: an odd width splits into 960 and 961, never 960 and 960 with a one-pixel
    // gap between them.
    const int permille = std::clamp(splitPermille, kMinSplitPermille, kMaxSplitPermille);

    // The cut position comes from one division; the far side is whatever is left over. Computing
    // both sides independently would round 1921 twice and lose a pixel between them.
    const int cutX = static_cast<int>(static_cast<long long>(workArea.width) * permille / 1000);
    const int farWidth = workArea.width - cutX;
    const int cutY = static_cast<int>(static_cast<long long>(workArea.height) * permille / 1000);
    const int farHeight = workArea.height - cutY;

    switch (mode)
    {
        case DockMode::Left:
            return Rect{ workArea.left, workArea.top, cutX, workArea.height };

        case DockMode::Right:
            return Rect{ workArea.left + cutX, workArea.top, farWidth, workArea.height };

        case DockMode::Top:
            return Rect{ workArea.left, workArea.top, workArea.width, cutY };

        case DockMode::Bottom:
            return Rect{ workArea.left, workArea.top + cutY, workArea.width, farHeight };

        case DockMode::Position:
        case DockMode::Maximize:
        case DockMode::Fill:
        case DockMode::Count:
            break;
    }

    return workArea;
}

DockMode DetectDock(const RECT& windowRect, const Rect& workArea, int& splitPermille)
{
    splitPermille = kDefaultSplitPermille;

    if (!workArea.IsValid())
    {
        return DockMode::Position;
    }

    constexpr int kFlushPx = 2;

    const int workRight = workArea.Right();
    const int workBottom = workArea.Bottom();
    const int width = static_cast<int>(windowRect.right - windowRect.left);
    const int height = static_cast<int>(windowRect.bottom - windowRect.top);

    if (width <= 0 || height <= 0)
    {
        return DockMode::Position;
    }

    const bool flushLeft = AbsDiff(windowRect.left, workArea.left) <= kFlushPx;
    const bool flushTop = AbsDiff(windowRect.top, workArea.top) <= kFlushPx;
    const bool flushRight = AbsDiff(windowRect.right, workRight) <= kFlushPx;
    const bool flushBottom = AbsDiff(windowRect.bottom, workBottom) <= kFlushPx;

    // The measured rectangle is a window frame, which carries an invisible resize border of a
    // pixel or two per side. A true half therefore measures as 501‰ on one side of the cut and
    // 500‰ on the other. Keeping 501 would restore a pair that overlaps by those few pixels, and
    // the next capture would measure the overlap back in; snapping to half makes the round trip
    // tile exactly and lets the stored value converge on a plain 500.
    const auto toPermille = [](int part, int whole)
    {
        if (whole <= 0)
        {
            return 0;
        }

        int permille = static_cast<int>(std::lround(part * 1000.0 / whole));
        const int tolerance = static_cast<int>(std::lround(kFlushPx * 1000.0 / whole));
        if (std::abs(permille - kDefaultSplitPermille) <= tolerance)
        {
            permille = kDefaultSplitPermille;
        }

        return permille;
    };

    const bool fitsWidth = width <= workArea.width + kFlushPx;
    const bool fitsHeight = height <= workArea.height + kFlushPx;

    if (flushLeft && flushTop && flushRight && flushBottom)
    {
        return DockMode::Fill;
    }

    // A ratio outside the accepted band means the window is so thin (or so wide) that re-applying
    // the split would not reproduce it, so it is treated as a free-floating window instead.
    const auto accept = [&splitPermille](const int permille)
    {
        if (permille < kMinSplitPermille || permille > kMaxSplitPermille)
        {
            return false;
        }

        splitPermille = permille;
        return true;
    };

    // Three flush edges plus one cut across the work area is the shape every Windows snap layout
    // produces; the cut edge is what carries the ratio. For a right or bottom dock the window
    // keeps the far side, so the ratio is whatever the near side gives up.
    if (flushTop && flushBottom && fitsWidth)
    {
        if (flushLeft && accept(toPermille(width, workArea.width)))
        {
            return DockMode::Left;
        }

        if (flushRight && accept(1000 - toPermille(width, workArea.width)))
        {
            return DockMode::Right;
        }
    }

    if (flushLeft && flushRight && fitsHeight)
    {
        if (flushTop && accept(toPermille(height, workArea.height)))
        {
            return DockMode::Top;
        }

        if (flushBottom && accept(1000 - toPermille(height, workArea.height)))
        {
            return DockMode::Bottom;
        }
    }

    return DockMode::Position;
}

Rect WorkAreaFor(const Rect& monitorRect)
{
    RECT rect{ monitorRect.left, monitorRect.top, monitorRect.Right(), monitorRect.Bottom() };

    const HMONITOR monitor = ::MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
    if (monitor == nullptr)
    {
        return monitorRect;
    }

    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!::GetMonitorInfoW(monitor, &info))
    {
        return monitorRect;
    }

    return Rect{ info.rcWork.left, info.rcWork.top, info.rcWork.right - info.rcWork.left,
        info.rcWork.bottom - info.rcWork.top };
}

PlacementPlan PlanPlacement(
    const WindowSnapshot& window,
    const MonitorSnapshot& savedMonitor,
    const Rect& currentMonitorRect,
    const Rect& currentWorkArea,
    const RECT& actualNormalRect,
    bool actualMaximized,
    RestoreScope scope)
{
    PlacementPlan plan;

    if (IsDockMode(window.dock))
    {
        // Docking ignores the saved rectangle entirely: the target is derived from the display as
        // it is now, which is the whole point - the window lands correctly even if the resolution
        // or the arrangement changed while it was away.
        const Rect docked =
            ComputeDockRect(window.dock, currentWorkArea, window.splitPermille);
        plan.target = RECT{ docked.left, docked.top, docked.Right(), docked.Bottom() };
        plan.minimized = false;
        plan.maximized = (window.dock == DockMode::Maximize);
    }
    else
    {
        const long dx = static_cast<long>(currentMonitorRect.left) - savedMonitor.rect.left;
        const long dy = static_cast<long>(currentMonitorRect.top) - savedMonitor.rect.top;

        plan.target.left = window.rect.left + dx;
        plan.target.top = window.rect.top + dy;
        plan.target.right = window.rect.Right() + dx;
        plan.target.bottom = window.rect.Bottom() + dy;
        plan.minimized = window.minimized;
        plan.maximized = window.maximized;
    }

    bool inPlace = false;

    // Homecoming stops here when the window is already at home: on its own display, on a display
    // that still has the geometry it had when the layout was taken. Whatever the user has arranged
    // since then is theirs - a docked window that was re-split by hand must survive a monitor going
    // to sleep and coming back.
    //
    // The geometry comparison is what keeps a resolution change from being ignored: a display that
    // came back at a different size has moved everything inside it, and then the captured dock is
    // applied as usual.
    if (scope == RestoreScope::Homecoming && SameRect(savedMonitor.rect, currentMonitorRect)
        && CentreIsInside(actualNormalRect, currentWorkArea))
    {
        plan.move = false;
        return plan;
    }

    if (plan.maximized)
    {
        // A maximized window reports its restore-down rectangle as the normal rect, so comparing
        // rectangles can never match. "Already maximized on the target display" is the honest
        // question, and without asking it the window would be re-maximized on every display
        // change, which flickers.
        inPlace = actualMaximized && Intersects(actualNormalRect, currentMonitorRect);
    }
    else
    {
        inPlace = AbsDiff(actualNormalRect.left, plan.target.left) <= kPlacementTolerancePx
               && AbsDiff(actualNormalRect.top, plan.target.top) <= kPlacementTolerancePx
               && AbsDiff(actualNormalRect.right, plan.target.right) <= kPlacementTolerancePx
               && AbsDiff(actualNormalRect.bottom, plan.target.bottom) <= kPlacementTolerancePx;
    }

    plan.move = !inPlace;
    return plan;
}

std::vector<MonitorSnapshot> CaptureMonitors()
{
    std::vector<MonitorSnapshot> monitors;

    // Deliberately not IDesktopWallpaper: that object belongs to the shell, and the moment this
    // runs is exactly the moment the shell is least available. Waking a monitor up restarts
    // explorer, and creating the wallpaper object then fails with REGDB_E_CLASSNOTAVAILABLE
    // (0x80040154) - which used to empty the list, mark every saved display as "not attached",
    // and quietly restore nothing. The window layout needs the geometry and the device path,
    // never the wallpaper service, and EnumDisplayMonitors answers both from USER32 alone.
    //
    // rcMonitor, not rcWork: the layout records the display rectangle, and WorkAreaFor re-reads
    // the taskbar-free area at restore time (the taskbar can move between capture and restore).
    ::EnumDisplayMonitors(
        nullptr,
        nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
            auto* out = reinterpret_cast<std::vector<MonitorSnapshot>*>(data);

            MONITORINFO info{};
            info.cbSize = sizeof(info);
            if (!::GetMonitorInfoW(monitor, &info))
            {
                return TRUE;
            }

            // The same device path form GetMonitorPathForWindow reports, so a window and its
            // display can be matched by string equality later. A display without one could never
            // be found again on restore, so leaving it out is the honest outcome.
            std::wstring path = GetMonitorPath(monitor);
            if (path.empty())
            {
                return TRUE;
            }

            MonitorSnapshot snapshot;
            snapshot.devicePath = std::move(path);
            snapshot.rect = Rect{ info.rcMonitor.left, info.rcMonitor.top,
                info.rcMonitor.right - info.rcMonitor.left,
                info.rcMonitor.bottom - info.rcMonitor.top };
            out->push_back(std::move(snapshot));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&monitors));

    return monitors;
}

Layout CaptureLayout()
{
    Layout layout;
    layout.monitors = CaptureMonitors();

    // Work areas are resolved once per display rather than once per window: they need a Win32
    // call each and every window on the same display would ask for the same answer.
    std::unordered_map<std::wstring, Rect> workAreas;
    for (const auto& monitor : layout.monitors)
    {
        workAreas.emplace(monitor.devicePath, WorkAreaFor(monitor.rect));
    }

    for (const auto& entry : EnumerateWindows())
    {
        WindowSnapshot snapshot;
        snapshot.exePath = entry.exePath;
        snapshot.appName = entry.appName;
        snapshot.title = entry.title;
        snapshot.monitorPath = GetMonitorPathForWindow(entry.handle);
        snapshot.rect = FromRect(entry.rect);
        snapshot.minimized = entry.minimized;
        snapshot.maximized = entry.maximized;

        // What the window *is* on its display, read the same way the restore path will reason
        // about it later. Maximized has to be asked first: a maximized window reports its
        // restore-down rectangle as the normal rect, which is not the area it covers, so the
        // edge test below would classify it as an ordinary floating window.
        if (entry.maximized)
        {
            snapshot.dock = DockMode::Maximize;
        }
        else
        {
            const auto workArea = workAreas.find(snapshot.monitorPath);
            if (workArea != workAreas.end())
            {
                snapshot.dock = DetectDock(entry.rect, workArea->second, snapshot.splitPermille);
            }
        }

        layout.windows.push_back(std::move(snapshot));
    }

    return layout;
}

bool ApplyPlacement(HWND window, const RECT& targetScreenRect, bool minimized, bool maximized)
{
    if (ApplyPlacementOnce(window, targetScreenRect, minimized, maximized))
    {
        return true;
    }

    // Windows that were just re-parented or are mid-animation can reject the first call. One
    // retry covers that without turning a transient failure into a permanent one.
    return ApplyPlacementOnce(window, targetScreenRect, minimized, maximized);
}

RestoreStats RestoreLayout(const Layout& layout, RestoreScope scope)
{
    RestoreStats stats;

    if (layout.windows.empty())
    {
        util::LogWarn(L"layout is empty, nothing to restore");
        return stats;
    }

    struct LiveMonitor
    {
        Rect rect{};
        Rect workArea{};
    };

    std::unordered_map<std::wstring, LiveMonitor> byPath;
    for (const auto& monitor : CaptureMonitors())
    {
        // The work area is read now, not from the layout file: docking must respect the taskbar
        // as it is at restore time, which is what the user sees.
        byPath.emplace(monitor.devicePath, LiveMonitor{ monitor.rect, WorkAreaFor(monitor.rect) });
    }

    // Index the saved monitors so a window can look up its own display's original geometry.
    std::unordered_map<std::wstring, MonitorSnapshot> savedByPath;
    for (const auto& monitor : layout.monitors)
    {
        savedByPath.emplace(monitor.devicePath, monitor);
    }

    const auto windows = EnumerateWindows();

    // Pairing: exact (exe + title) first, then whatever is left within the same exe in order.
    // Title alone is unreliable when documents are renamed; exe alone is ambiguous when one app
    // has several windows. Doing both in two passes gets the common cases right without any
    // learned heuristics.
    std::vector<bool> windowUsed(windows.size(), false);
    std::vector<const WindowSnapshot*> pending;
    pending.reserve(layout.windows.size());

    std::vector<std::pair<const WindowSnapshot*, size_t>> pairs;

    for (const auto& snapshot : layout.windows)
    {
        if (!snapshot.enabled)
        {
            ++stats.skippedDisabled;
            continue;
        }

        size_t match = windows.size();
        for (size_t index = 0; index < windows.size(); ++index)
        {
            if (windowUsed[index])
            {
                continue;
            }

            const auto& candidate = windows[index];
            if (!Monitor::IEquals(candidate.exePath, snapshot.exePath) || candidate.title != snapshot.title)
            {
                continue;
            }

            match = index;
            break;
        }

        if (match < windows.size())
        {
            windowUsed[match] = true;
            pairs.emplace_back(&snapshot, match);
        }
        else
        {
            pending.push_back(&snapshot);
        }
    }

    for (const WindowSnapshot* snapshot : pending)
    {
        for (size_t index = 0; index < windows.size(); ++index)
        {
            if (windowUsed[index] || !Monitor::IEquals(windows[index].exePath, snapshot->exePath))
            {
                continue;
            }

            windowUsed[index] = true;
            pairs.emplace_back(snapshot, index);
            break;
        }
    }

    stats.missingWindow = static_cast<int>(layout.windows.size() - stats.skippedDisabled - pairs.size());

    for (const auto& [snapshot, index] : pairs)
    {
        const auto savedMonitor = savedByPath.find(snapshot->monitorPath);
        if (savedMonitor == savedByPath.end())
        {
            // The snapshot predates any display bookkeeping, or the path format changed.
            ++stats.skippedMissingMonitor;
            continue;
        }

        const auto currentMonitor = byPath.find(snapshot->monitorPath);
        if (currentMonitor == byPath.end())
        {
            util::LogInfo(
                L"display of {} is not attached, leaving the window where it is", windows[index].appName);
            ++stats.skippedMissingMonitor;
            continue;
        }

        const PlacementPlan plan = PlanPlacement(*snapshot, savedMonitor->second,
            currentMonitor->second.rect, currentMonitor->second.workArea, windows[index].rect,
            windows[index].maximized, scope);

        if (!plan.move)
        {
            ++stats.alreadyInPlace;
            continue;
        }

        if (ApplyPlacement(windows[index].handle, plan.target, plan.minimized, plan.maximized))
        {
            util::LogInfo(
                L"placed {} at ({},{}) {}x{}",
                windows[index].appName,
                plan.target.left,
                plan.target.top,
                plan.target.right - plan.target.left,
                plan.target.bottom - plan.target.top);
            ++stats.moved;
        }
        else
        {
            util::LogWarn(L"failed placing {}", windows[index].appName);
            ++stats.failed;
        }
    }

    util::LogInfo(
        L"restore finished: {} moved, {} already in place, {} skipped (display missing), "
        L"{} skipped (unticked), {} not running, {} failed",
        stats.moved,
        stats.alreadyInPlace,
        stats.skippedMissingMonitor,
        stats.skippedDisabled,
        stats.missingWindow,
        stats.failed);

    return stats;
}

} // namespace wintools::core
