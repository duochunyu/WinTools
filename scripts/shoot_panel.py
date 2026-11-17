"""Saves the WinTools panel as a PNG.

Asks the window to render itself through PrintWindow / WM_PRINTCLIENT instead of grabbing the
screen. That works even when the panel is behind another window or on a display where a video
overlay would be captured instead - which is exactly the situation on a working desktop.

PrintWindow rather than sending WM_PRINTCLIENT directly: GDI handles are per-process, so a DC
created here is meaningless inside the panel's process. The window manager marshals the message
and builds a DC the target can actually use.

The image covers the client area only: the caption belongs to the window manager, not to us, and
leaving it out makes the result usable as a documentation figure at any DPI.

Usage:  python scripts/shoot_panel.py [output.png]
"""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import struct
import sys
import zlib

PANEL_CLASS = "WinTools.Panel.6d21f0a4"
PW_CLIENTONLY = 1

HANDLE = ctypes.c_void_p

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32


def bind(library, name, result, arguments):
    fn = getattr(library, name)
    fn.restype = result
    fn.argtypes = arguments
    return fn


find_window = bind(user32, "FindWindowW", HANDLE, [wt.LPCWSTR, wt.LPCWSTR])
get_client_rect = bind(user32, "GetClientRect", wt.BOOL, [HANDLE, ctypes.POINTER(wt.RECT)])
get_dc = bind(user32, "GetDC", HANDLE, [HANDLE])
release_dc = bind(user32, "ReleaseDC", ctypes.c_int, [HANDLE, HANDLE])
print_window = bind(user32, "PrintWindow", wt.BOOL, [HANDLE, HANDLE, wt.UINT])

create_compatible_dc = bind(gdi32, "CreateCompatibleDC", HANDLE, [HANDLE])
create_compatible_bitmap = bind(gdi32, "CreateCompatibleBitmap", HANDLE,
                               [HANDLE, ctypes.c_int, ctypes.c_int])
select_object = bind(gdi32, "SelectObject", HANDLE, [HANDLE, HANDLE])
delete_object = bind(gdi32, "DeleteObject", wt.BOOL, [HANDLE])
delete_dc = bind(gdi32, "DeleteDC", wt.BOOL, [HANDLE])
get_dibits = bind(gdi32, "GetDIBits", ctypes.c_int,
                  [HANDLE, HANDLE, wt.UINT, wt.UINT, ctypes.c_void_p, ctypes.c_void_p, wt.UINT])


def write_png(path: str, width: int, height: int, bgra: bytes) -> None:
    """Writes a 24-bit PNG. Hand-rolled so the script needs no image library."""
    rows = bytearray()
    for y in range(height):
        rows.append(0)  # filter: none
        start = y * width * 4
        for x in range(start, start + width * 4, 4):
            rows += bytes((bgra[x + 2], bgra[x + 1], bgra[x]))

    def chunk(tag: bytes, payload: bytes) -> bytes:
        return (struct.pack('>I', len(payload)) + tag + payload
                + struct.pack('>I', zlib.crc32(tag + payload) & 0xFFFFFFFF))

    header = struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)
    with open(path, 'wb') as out:
        out.write(b'\x89PNG\r\n\x1a\n')
        out.write(chunk(b'IHDR', header))
        out.write(chunk(b'IDAT', zlib.compress(bytes(rows), 9)))
        out.write(chunk(b'IEND', b''))


def main() -> int:
    output = sys.argv[1] if len(sys.argv) > 1 else "panel.png"

    # The panel is per-monitor DPI aware, so it only reports its real client size to a DPI-aware
    # caller: an unaware one is handed a virtualised rectangle that is smaller than the pixels the
    # panel actually draws, and the capture would come out cropped. -4 is
    # DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2. Ignored when the host already has a context.
    try:
        user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
    except AttributeError:
        pass

    panel = find_window(PANEL_CLASS, None)
    if not panel:
        print("panel window not found - is WinTools running with its panel open?")
        return 1

    rect = wt.RECT()
    get_client_rect(panel, ctypes.byref(rect))
    width, height = rect.right - rect.left, rect.bottom - rect.top

    screen = get_dc(None)
    memory = create_compatible_dc(screen)
    bitmap = create_compatible_bitmap(screen, width, height)
    previous = select_object(memory, bitmap)

    if not print_window(panel, memory, PW_CLIENTONLY):
        print("PrintWindow failed")
        select_object(memory, previous)
        delete_object(bitmap)
        delete_dc(memory)
        release_dc(None, screen)
        return 1

    # GetDIBits refuses a bitmap that is still selected into a DC, so it is deselected first.
    select_object(memory, previous)

    buffer = ctypes.create_string_buffer(width * height * 4)
    info = struct.pack('<IiiHHIIiiII', 40, width, -height, 1, 32, 0, width * height * 4, 0, 0, 0, 0)
    copied = get_dibits(memory, bitmap, 0, height, buffer, info, 0)
    if copied != height:
        print(f"GetDIBits copied {copied} of {height} scanlines")

    delete_object(bitmap)
    delete_dc(memory)
    release_dc(None, screen)

    write_png(output, width, height, buffer.raw)
    print(f"{output}  {width}x{height}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
