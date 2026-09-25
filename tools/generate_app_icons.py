#!/usr/bin/env python3
"""Generates the StudyBoard application icon assets from the master artwork.

The build does not run this script; its outputs are committed under
resources/icons/app/. Re-run it only when the artwork changes. Requires Pillow.

Usage:
    # 1. Import new source artwork (any raster format) as the master image. Non-square
    #    artwork is padded (never cropped or scaled) to a square using the colour of its
    #    corner pixel, so the artwork itself is not altered.
    python tools/generate_app_icons.py --import-source <path-to-image>

    # 2. (Re)generate all sizes and the Windows .ico from the committed master.
    python tools/generate_app_icons.py

Outputs (resources/icons/app/):
    studyboard-master.png       lossless square master (source of truth)
    studyboard-{16..256}.png    raster sizes compiled into the app via Qt resources
    studyboard.ico              Windows executable icon (16, 24, 32, 48, 64, 256)
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    sys.exit("Pillow is required: pip install Pillow")

ROOT = Path(__file__).resolve().parent.parent
OUT_DIR = ROOT / "resources" / "icons" / "app"
MASTER = OUT_DIR / "studyboard-master.png"

PNG_SIZES = (16, 32, 48, 64, 128, 256)
ICO_SIZES = (16, 24, 32, 48, 64, 256)


def import_source(source: Path) -> None:
    image = Image.open(source)
    image.load()
    rgba = image.convert("RGBA")
    width, height = rgba.size
    side = max(width, height)
    if (width, height) != (side, side):
        fill = rgba.getpixel((0, 0))
        square = Image.new("RGBA", (side, side), fill)
        square.paste(rgba, ((side - width) // 2, (side - height) // 2))
        rgba = square
        print(f"padded {width}x{height} -> {side}x{side} with corner colour {fill}")
    # Keep RGB when the artwork has no transparency (smaller files, identical pixels).
    if rgba.getextrema()[3] == (255, 255):
        rgba = rgba.convert("RGB")
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    rgba.save(MASTER, optimize=True)
    print(f"wrote {MASTER.relative_to(ROOT)} ({rgba.size[0]}x{rgba.size[1]}, {rgba.mode})")


def resized(master: Image.Image, size: int) -> Image.Image:
    if size > master.size[0]:
        sys.exit(f"refusing to upscale the {master.size[0]}px master to {size}px")
    return master.resize((size, size), Image.Resampling.LANCZOS)


def generate() -> None:
    if not MASTER.exists():
        sys.exit(f"missing {MASTER.relative_to(ROOT)}; run with --import-source first")
    master = Image.open(MASTER)
    master.load()

    for size in PNG_SIZES:
        path = OUT_DIR / f"studyboard-{size}.png"
        resized(master, size).save(path, optimize=True)
        print(f"wrote {path.relative_to(ROOT)}")

    images = [resized(master, size).convert("RGBA") for size in ICO_SIZES]
    ico = OUT_DIR / "studyboard.ico"
    largest = images[-1]
    largest.save(ico, format="ICO", sizes=[(s, s) for s in ICO_SIZES],
                 append_images=images[:-1])
    print(f"wrote {ico.relative_to(ROOT)} (sizes {', '.join(map(str, ICO_SIZES))})")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--import-source", type=Path, metavar="IMAGE",
                        help="import new artwork as the master image before generating")
    args = parser.parse_args()
    if args.import_source:
        import_source(args.import_source)
    generate()


if __name__ == "__main__":
    main()
