#pragma once

#include <Windows.h>

#include <string>
#include <vector>

namespace wintools::core
{

/// One top-level window that is worth remembering the position of.
///
/// Everything here is captured in the process's own coordinate space, which is DPI-unaware by
/// manifest (no dpiAware entry). That choice is deliberate: it makes every monitor share one
/// logical coordinate system, so a window can be moved between displays with different scaling
/// factors using plain pixel arithmetic. Declaring Per-Monitor V2 would return physical pixels
/// and force a DPI conversion at every boundary, which is where the PowerToys original needed
/// DPIAware::Convert and still had bugs around mixed-DPI arrangements.
struct WindowEntry
{
    HWND handle{};
    std::wstring exePath;
    std::wstring appName;
    std::wstring title;

    /// Restored geometry in screen coordinates.
    ///
    /// For a maximized window this is the rectangle it would shrink to, and for a minimized one
    /// the rectangle it was minimized from. Capturing GetWindowRect for those would store the
    /// maximized frame as the window's "normal" size and leave it stuck full-screen later.
    ///
    /// For a window that is neither, this is the live frame. Aero Snap leaves WINDOWPLACEMENT
    /// untouched, so rcNormalPosition still describes the shape the window had before the snap -
    /// reading it would store a snapped pair as two full-screen windows.
    RECT rect{};
    bool minimized{};
    bool maximized{};
};

/// Every top-level, user-facing window of every other process, in z-order.
///
/// EnumWindows already walks in z-order, so the index within one exe is stable enough to serve
/// as a fallback pairing key.
[[nodiscard]] std::vector<WindowEntry> EnumerateWindows();

/// True when the window looks like something a user would expect to be put back in place.
///
/// Exposed separately from EnumerateWindows so the rules can be reasoned about (and partially
/// tested) without a desktop full of real windows.
[[nodiscard]] bool ShouldManageWindow(HWND window);

/// Full exe path of the process that owns the window, or empty when it cannot be read.
///
/// Fails for elevated processes when we run unelevated; that is a permission reality, not a bug,
/// and such windows are simply left alone.
[[nodiscard]] std::wstring GetProcessPathForWindow(HWND window);

/// The shell's monitor device path ("\\\\?\\DISPLAY#...") that contains the window.
[[nodiscard]] std::wstring GetMonitorPathForWindow(HWND window);

/// Converts an HMONITOR to the same device path form IDesktopWallpaper reports.
[[nodiscard]] std::wstring GetMonitorPath(HMONITOR monitor);

/// True when the window is minimized (IsIconic) / maximized (IsZoomed).
[[nodiscard]] bool IsWindowMinimized(HWND window);
[[nodiscard]] bool IsWindowMaximized(HWND window);

} // namespace wintools::core
