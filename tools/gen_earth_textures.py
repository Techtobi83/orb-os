#!/usr/bin/env python3
"""Make the Earth app's two pictures from NASA's public-domain maps.

    python tools/gen_earth_textures.py <blue_marble.jpg> <black_marble.jpg> <out_dir>

Inputs (both NASA Earth Observatory, public domain):
  Blue Marble Next Generation, e.g. world.200407.3x5400x2700.jpg (image record 74092)
  Black Marble 2016, BlackMarble_2016_01deg.jpg (image record 144898)

Writes day.jpg (colour) and night.jpg (grey: the city lights, already thresholded so the dark
land is black) at 768x384, baseline JPEG, which is what the firmware decodes. They go on the
SD card in /earth/.
"""
import os
import sys
from PIL import Image, ImageOps

W, H = 768, 384


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    day_src, night_src, out = sys.argv[1:]
    os.makedirs(out, exist_ok=True)
    day = Image.open(day_src).convert("RGB").resize((W, H), Image.LANCZOS)
    day.save(os.path.join(out, "day.jpg"), "JPEG", quality=88, optimize=True, progressive=False)
    night = ImageOps.grayscale(Image.open(night_src).convert("RGB")).resize((W, H), Image.LANCZOS)
    # Keep the lights, drop the faint grey of unlit land: (v - 6 %) * 1.6, as the preview did.
    night = night.point(lambda v: max(0, min(255, int((v - 15) * 1.6))))
    night.save(os.path.join(out, "night.jpg"), "JPEG", quality=90, optimize=True, progressive=False)
    for f in ("day.jpg", "night.jpg"):
        print(f, os.path.getsize(os.path.join(out, f)) // 1024, "KB")


if __name__ == "__main__":
    main()
