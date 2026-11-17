"""Generates assets/wintools.ico.

The icon is drawn rather than hand-drawn so it stays consistent at every size Windows asks
for. Two rounded screens on a blue field is the whole identity of the app: one picture spread
across several monitors.

Run:  python scripts/make_icon.py
"""

from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "assets" / "wintools.ico"

# Same accent as the panel, so the tray icon and the window look like one product.
ACCENT = (43, 108, 246, 255)
ACCENT_DARK = (28, 82, 205, 255)
WHITE = (255, 255, 255, 255)

SIZES = [16, 20, 24, 32, 48, 64, 128, 256]


def draw(size: int) -> Image.Image:
    # Draw at 4x and downsample: Pillow has no antialiased rounded-rectangle primitive, and the
    # small sizes are exactly where hard edges would look worst.
    scale = 4
    canvas = size * scale
    image = Image.new("RGBA", (canvas, canvas), (0, 0, 0, 0))
    pen = ImageDraw.Draw(image)

    radius = int(canvas * 0.22)

    # Flat field with a darker surface along the bottom, then one rounded mask over the lot.
    # Building the shape first and masking once avoids the seams that per-shape clipping leaves
    # where two rounded outlines disagree by a fraction of a pixel.
    desk_top = int(canvas * 0.70)
    pen.rectangle([0, 0, canvas - 1, canvas - 1], fill=ACCENT)
    pen.rectangle([0, desk_top, canvas - 1, canvas - 1], fill=ACCENT_DARK)

    mask = Image.new("L", (canvas, canvas), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, canvas - 1, canvas - 1], radius=radius, fill=255)
    image.putalpha(mask)

    # Two screens. The gap stays proportional so it survives at 16px.
    margin = canvas * 0.17
    # Never let the gap fall below ~1.5 final pixels: at 16px the proportional gap rounds away
    # and the two screens merge into one blurry block.
    gap = max(canvas * 0.06, 1.5 * scale)
    top = canvas * 0.26
    screens_bottom = canvas * 0.74
    width = (canvas - 2 * margin - gap) / 2
    screen_radius = max(1, int(canvas * 0.035))

    for index in range(2):
        left = margin + index * (width + gap)
        pen.rounded_rectangle(
            [left, top, left + width, screens_bottom],
            radius=screen_radius,
            fill=WHITE,
        )

    return image.resize((size, size), Image.LANCZOS)


def main() -> None:
    OUT.parent.mkdir(parents=True, exist_ok=True)
    frames = [draw(size) for size in SIZES]
    # 256 first: Windows uses the largest entry for the shell preview and the smallest for the tray.
    frames[-1].save(OUT, format="ICO", sizes=[(s, s) for s in SIZES])
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes, {len(SIZES)} sizes)")


if __name__ == "__main__":
    main()
