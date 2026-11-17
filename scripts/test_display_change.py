"""End-to-end check of the display-change path.

Captures a fresh layout, then picks one window that the layout actually mentions and asserts the
two halves of the automatic restore rule, in order:

1. A window that ended up on the *wrong* display is brought home. This is the case a display
   being unplugged and replugged produces, and the one worth automating.
2. A window that is still on its own display, moved by hand, is left alone. Display changes are
   often the user pressing a button on the monitor itself, and the arrangement they built after
   that must not be undone two seconds later.

The debounce timer has to run out before either answer means anything, so each phase waits longer
than the configured two seconds.

The window is located by title *and* captured position rather than by title alone: this desktop
has more than one window titled "设置", and moving one the layout never saw would make the test
pass or fail for the wrong reason.

Usage:  python scripts/test_display_change.py
"""

import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

WM_DISPLAYCHANGE = 0x007E

user32 = ctypes.windll.user32
user32.FindWindowW.restype = wt.HWND
user32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
user32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
EnumProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

EXE = os.path.join(os.path.dirname(__file__), "..", "build", "debug", "bin", "WinTools.exe")
LAYOUT = os.path.join(os.environ["LOCALAPPDATA"], "WinTools", "layout.txt")


def visible_windows():
    result = []

    def callback(hwnd, _):
        if not user32.IsWindowVisible(hwnd):
            return True
        length = user32.GetWindowTextLengthW(hwnd)
        if length:
            buffer = ctypes.create_unicode_buffer(length + 1)
            user32.GetWindowTextW(hwnd, buffer, length + 1)
            rect = wt.RECT()
            user32.GetWindowRect(hwnd, ctypes.byref(rect))
            result.append((buffer.value, hwnd, rect))
        return True

    user32.EnumWindows(EnumProc(callback), 0)
    return result


def captured_non_maximized():
    """Title and rect of the first plain (not maximized) window in the saved layout."""
    with open(LAYOUT, encoding="utf-8-sig") as handle:
        for line in handle:
            parts = line.rstrip("\r\n").split("\t")
            if len(parts) >= 9 and parts[0] == "window" and "x" not in parts[5]:
                return parts[-1], tuple(int(v) for v in parts[1:5])
    return None, None


class MONITORINFO(ctypes.Structure):
    _fields_ = [
        ("cbSize", wt.DWORD),
        ("rcMonitor", wt.RECT),
        ("rcWork", wt.RECT),
        ("dwFlags", wt.DWORD),
    ]


# Without argtypes ctypes marshals HMONITOR as a 32-bit int and some handles overflow it.
user32.GetMonitorInfoW.argtypes = [wt.HMONITOR, ctypes.POINTER(MONITORINFO)]
user32.GetMonitorInfoW.restype = wt.BOOL


def work_areas():
    """Every display's work area, in the same screen coordinates the layout uses."""
    rects = []
    proc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HMONITOR, wt.HDC, ctypes.POINTER(wt.RECT), wt.LPARAM)

    def callback(hmonitor, _hdc, _rect, _data):
        info = MONITORINFO()
        info.cbSize = ctypes.sizeof(info)
        if user32.GetMonitorInfoW(hmonitor, ctypes.byref(info)):
            rects.append(info.rcWork)
        return True

    user32.EnumDisplayMonitors(None, None, proc(callback), 0)
    return rects


def elsewhere(rect):
    """A point on a display that does not contain the middle of the given rect."""
    centre_x = (rect.left + rect.right) // 2
    centre_y = (rect.top + rect.bottom) // 2
    for area in work_areas():
        inside = area.left <= centre_x < area.right and area.top <= centre_y < area.bottom
        if not inside:
            return area.left + 80, area.top + 60
    return None


def broadcast(watcher):
    user32.PostMessageW(watcher, WM_DISPLAYCHANGE, 8, 0)
    print("posted WM_DISPLAYCHANGE, waiting for the debounce...")
    time.sleep(3.5)


def main() -> int:
    subprocess.run([EXE, "capture"], check=False)
    title, expected = captured_non_maximized()
    if title is None:
        print("no plain window in the layout to test with")
        return 1

    left, top, width, height = expected
    print(f"layout says {title!r} is at ({left},{top}) {width}x{height}")

    match = None
    for window_title, hwnd, rect in visible_windows():
        if window_title != title:
            continue
        if (rect.left, rect.top) == (left, top):
            match = hwnd
            break

    if match is None:
        print(f"could not find a live window titled {title!r} at ({left},{top})")
        for window_title, hwnd, rect in visible_windows():
            if window_title == title:
                print(f"  candidate: hwnd {hwnd} at ({rect.left},{rect.top})")
        return 1

    watcher = user32.FindWindowW("WinTools.DisplayWatcher.7c4a1b93", None)
    if not watcher:
        print("display watcher window not found - is WinTools running?")
        return 1

    live = wt.RECT()
    user32.GetWindowRect(match, ctypes.byref(live))
    destination = elsewhere(live)
    if destination is None:
        print("this desktop has a single display; the come-home case cannot be tested")
        return 1

    # Phase 1: park it on another display and expect it back.
    user32.SetWindowPos(match, 0, destination[0], destination[1], width, height, 0x0004 | 0x0010)
    time.sleep(0.4)
    moved = wt.RECT()
    user32.GetWindowRect(match, ctypes.byref(moved))
    print(f"parked on another display at ({moved.left},{moved.top})")
    broadcast(watcher)

    final = wt.RECT()
    user32.GetWindowRect(match, ctypes.byref(final))
    came_home = abs(final.left - left) <= 2 and abs(final.top - top) <= 2
    print(f"  after the broadcast: ({final.left},{final.top})", "-> home" if came_home else "-> LEFT OUT")

    # Phase 2: nudge it 150 px on its own display and expect it to stay put.
    user32.SetWindowPos(match, 0, left + 150, top + 120, width, height, 0x0004 | 0x0010)
    time.sleep(0.4)
    nudged = wt.RECT()
    user32.GetWindowRect(match, ctypes.byref(nudged))
    print(f"nudged by hand to ({nudged.left},{nudged.top})")
    broadcast(watcher)

    stayed = wt.RECT()
    user32.GetWindowRect(match, ctypes.byref(stayed))
    untouched = abs(stayed.left - nudged.left) <= 2 and abs(stayed.top - nudged.top) <= 2
    print(f"  after the broadcast: ({stayed.left},{stayed.top})",
          "-> left alone" if untouched else "-> OVERRIDDEN")

    # Put the window back where the layout has it, whatever the two phases concluded.
    user32.SetWindowPos(match, 0, left, top, width, height, 0x0004 | 0x0010)

    ok = came_home and untouched
    print("RESULT:", "ok" if ok else "failed")
    return 0 if ok else 2


if __name__ == "__main__":
    raise SystemExit(main())
