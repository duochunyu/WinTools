"""Verify generated wallpaper pieces against the source image, pixel by pixel.

Each piece is named m<hash>_<left>_<top>_<w>x<h>.png, where left/top are virtual-desktop
coordinates. This script crops the same rectangle out of the source image and compares the two
decoded pixel buffers. Comparing decoded pixels rather than file bytes is deliberate: the PNG
encoder parameters of the C# and C++ implementations differ, so byte equality would fail on
identical images.

Usage:
    python scripts/verify_pieces.py <source.png> <pieces-dir> <canvas-left> <canvas-top>
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

from PIL import Image, ImageChops

NAME_PATTERN = re.compile(r"^m[0-9a-f]{8}_(-?\d+)_(-?\d+)_(\d+)x(\d+)\.png$")


def compare(expected: Image.Image, actual: Image.Image) -> tuple[int, int, int]:
    """Returns (max channel difference, differing pixel count, total pixel count)."""
    diff = ImageChops.difference(expected, actual)
    max_channel_diff = max(channel[1] for channel in diff.getextrema())

    histogram = diff.convert("L").histogram()
    identical = histogram[0]
    total = expected.width * expected.height
    return max_channel_diff, total - identical, total


def main() -> int:
    if len(sys.argv) != 5:
        print(__doc__)
        return 1

    source_path = Path(sys.argv[1])
    pieces_dir = Path(sys.argv[2])
    origin_x = int(sys.argv[3])
    origin_y = int(sys.argv[4])

    source = Image.open(source_path).convert("RGB")
    print(f"source : {source_path.name}  {source.width}x{source.height}")
    print(f"canvas : origin ({origin_x},{origin_y})")
    print()

    checked = 0
    failures = 0
    for piece_path in sorted(pieces_dir.glob("*.png")):
        match = NAME_PATTERN.match(piece_path.name)
        if match is None:
            print(f"skip   {piece_path.name} (not a generated piece)")
            continue

        left, top, width, height = (int(group) for group in match.groups())
        checked += 1

        piece = Image.open(piece_path).convert("RGB")
        if piece.size != (width, height):
            print(f"FAIL   {piece_path.name}: size {piece.size} != declared {width}x{height}")
            failures += 1
            continue

        box = (left - origin_x, top - origin_y, left - origin_x + width, top - origin_y + height)
        if box[0] < 0 or box[1] < 0 or box[2] > source.width or box[3] > source.height:
            print(f"FAIL   {piece_path.name}: crop {box} falls outside the source image")
            failures += 1
            continue

        max_diff, differing, total = compare(source.crop(box), piece)
        status = "ok  " if max_diff == 0 else "FAIL"
        if max_diff != 0:
            failures += 1

        print(
            f"{status}   {piece_path.name}\n"
            f"       crop {box}  ->  {width}x{height}\n"
            f"       max channel diff {max_diff}, differing pixels {differing}/{total} "
            f"({differing / total * 100:.3f}%)"
        )

    print()
    if checked == 0:
        print("no pieces found")
        return 1

    print(f"{checked - failures}/{checked} piece(s) identical to the source")
    return 0 if failures == 0 else 2


if __name__ == "__main__":
    raise SystemExit(main())
