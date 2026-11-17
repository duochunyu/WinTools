"""Clicks a spot inside the WinTools panel by posting mouse messages to its window.

The panel has no child controls - every pixel is drawn and hit-tested by the window itself - so
posting WM_LBUTTONDOWN/WM_LBUTTONUP at client coordinates is exactly what a real click does.

Usage:  python scripts/click_panel.py X Y
"""

import ctypes
import ctypes.wintypes as wt
import sys

WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
MK_LBUTTON = 0x0001

user32 = ctypes.windll.user32
user32.FindWindowW.restype = wt.HWND
user32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
user32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2

    x, y = int(sys.argv[1]), int(sys.argv[2])
    window = user32.FindWindowW("WinTools.Panel.6d21f0a4", None)
    if not window:
        print("panel window not found - is WinTools running with its panel open?")
        return 1

    packed = (y << 16) | (x & 0xFFFF)
    user32.PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, packed)
    user32.PostMessageW(window, WM_LBUTTONUP, 0, packed)
    print(f"clicked ({x},{y}) on hwnd {window}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
