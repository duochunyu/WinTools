"""Generate a gridded test wallpaper covering the virtual desktop bounding box.

The grid is the point: a slice placed at a one-pixel-wrong offset shows up as a visibly
broken line or a duplicate coordinate label, which is far easier to spot than a diff of two
photographs.

Usage:  python scripts/make_test_image.py [output.png]
"""

from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

# Must match the bounding box of the monitors this is being tested against. Printed by
# `WinTools.exe monitors`; kept in sync by hand because this is a one-off test fixture.
CANVAS_W = 3840
CANVAS_H = 2640
CANVAS_ORIGIN = (0, -1440)

# (left, top, width, height) in virtual-desktop coordinates.
MONITORS = [
    (0, 0, 1920, 1200),
    (1920, 0, 1920, 1200),
    (181, -1440, 3440, 1440),
]

BACKGROUND = (24, 26, 33)
MINOR = (45, 48, 60)
MAJOR = (80, 90, 110)
LABEL = (150, 200, 255)


def load_font(size: int) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    for name in ("consola.ttf", "arial.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default()


def main() -> int:
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).parent.parent / ".tmp" / "grid.png"

    image = Image.new("RGB", (CANVAS_W, CANVAS_H), BACKGROUND)
    draw = ImageDraw.Draw(image)

    for x in range(0, CANVAS_W, 200):
        draw.line([(x, 0), (x, CANVAS_H)], fill=MAJOR if x % 400 == 0 else MINOR, width=1)
    for y in range(0, CANVAS_H, 200):
        draw.line([(0, y), (CANVAS_W, y)], fill=MAJOR if y % 400 == 0 else MINOR, width=1)

    font = load_font(18)
    origin_x, origin_y = CANVAS_ORIGIN
    for x in range(0, CANVAS_W, 400):
        for y in range(0, CANVAS_H, 400):
            # Label with virtual-desktop coordinates, not canvas ones: if a slice lands at the
            # wrong offset the numbers prove it without consulting any diff tool.
            draw.text((x + 6, y + 4), f"{x + origin_x},{y + origin_y}", fill=LABEL, font=font)

    colours = [(255, 90, 90), (255, 170, 60), (90, 220, 140)]
    for index, (left, top, width, height) in enumerate(MONITORS):
        box = [
            left - origin_x,
            top - origin_y,
            left - origin_x + width - 1,
            top - origin_y + height - 1,
        ]
        colour = colours[index % len(colours)]
        draw.rectangle(box, outline=colour, width=4)
        draw.text((box[0] + 12, box[1] + 34), f"DISPLAY{index + 1}  {width}x{height}", fill=colour, font=load_font(34))

    out.parent.mkdir(parents=True, exist_ok=True)
    image.save(out, "PNG")
    print(f"{out}  {CANVAS_W}x{CANVAS_H}  {out.stat().st_size / 1024:.0f} KB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
