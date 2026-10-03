#!/usr/bin/env python3
"""Draw the forecast screen's weather symbols and write them as LVGL alpha-only images.

    python tools/gen_wx_icons.py src/wx_icons.c src/wx_icons.h

Every symbol is a shape on a 24-unit grid, drawn at 4x and scaled down so its edges are
smooth. Each is stored as LV_IMG_CF_ALPHA_8BIT, one byte a pixel, and the screen colours it
with img_recolor. That is what lets one cloud be grey and the rain under it blue, and what
keeps a 300-pixel symbol at a third of the flash an RGB image would take.

Two sets:
  _l  the large background symbol. Its alpha already carries the fade from top to bottom
      (see L_TOP / L_BOTTOM) and a slight blur, so the screen draws it at full opacity.
  _s  small, crisp symbols at full alpha, for tomorrow's capsule.
  _m  the ring's time marker, sun and moon only, a size up from _s.

Base shapes are cloud, sun and moon; rain, snow, bolt and fog are separate layers drawn
over the cloud. Every image is cropped to its ink, and its offset inside the square frame
is written to the header, so layers placed at one origin line up.
"""
import math
import sys
from PIL import Image, ImageDraw, ImageFilter

SS = 4                      # supersampling
L_UNIT, S_UNIT, M_UNIT = 16, 36 / 24, 2  # px per grid unit: frames of 384, 36 and 48 px
L_TOP, L_BOTTOM = 0.34, 0.10  # large symbols: opacity at the top of the frame, at the bottom


def cloud(d, u):
    for cx, cy, r in ((8.0, 13.9, 4.6), (12.3, 10.9, 5.6), (17.3, 14.3, 4.2)):
        d.ellipse([(cx - r) * u, (cy - r) * u, (cx + r) * u, (cy + r) * u], fill=255)
    d.rectangle([8.0 * u, 12.0 * u, 17.3 * u, 18.5 * u], fill=255)


def sun(d, u):
    r = 4.6
    d.ellipse([(12 - r) * u, (12 - r) * u, (12 + r) * u, (12 + r) * u], fill=255)
    for k in range(8):
        a = k * math.pi / 4
        line(d, u, 12 + 7.0 * math.cos(a), 12 + 7.0 * math.sin(a),
             12 + 10.2 * math.cos(a), 12 + 10.2 * math.sin(a), 1.7)


def moon(d, u):
    r = 7.5
    d.ellipse([(12 - r) * u, (12 - r) * u, (12 + r) * u, (12 + r) * u], fill=255)
    r2 = 6.6
    d.ellipse([(16.0 - r2) * u, (8.4 - r2) * u, (16.0 + r2) * u, (8.4 + r2) * u], fill=0)


def line(d, u, x0, y0, x1, y1, w):
    d.line([x0 * u, y0 * u, x1 * u, y1 * u], fill=255, width=max(1, int(w * u)))
    for x, y in ((x0, y0), (x1, y1)):   # round caps
        d.ellipse([(x - w / 2) * u, (y - w / 2) * u, (x + w / 2) * u, (y + w / 2) * u], fill=255)


def drops(d, u):
    for x in (8.5, 12.5, 16.5):
        line(d, u, x, 19.8, x - 1.0, 22.8, 1.4)


def snow(d, u):
    for x, y in ((8.2, 20.6), (12.2, 22.4), (16.2, 20.6), (10.2, 23.4), (14.2, 23.4)):
        r = 0.85
        d.ellipse([(x - r) * u, (y - r) * u, (x + r) * u, (y + r) * u], fill=255)


def bolt(d, u):
    pts = [(13.2, 17.4), (10.0, 21.2), (12.1, 21.2), (10.9, 23.9), (14.9, 19.6), (12.8, 19.6), (14.3, 17.4)]
    d.polygon([(x * u, y * u) for x, y in pts], fill=255)


def fog(d, u):
    line(d, u, 5.0, 20.6, 19.0, 20.6, 1.3)
    line(d, u, 7.0, 23.0, 17.0, 23.0, 1.3)


def fogbank(d, u):
    """The large fog symbol: no cloud at all, banks of mist across the dial."""
    # About three quarters the size of the first version (owner, 2026-10-03), same centre.
    # Kept in the upper two thirds, inside the day ring: the lowest bank ends well above
    # tomorrow's capsule (screen y 302).
    for y, x0, x1 in ((7.4, 7.5, 16.5), (9.7, 5.6, 18.4), (12.0, 6.4, 17.6), (14.3, 5.2, 18.8), (16.6, 8.0, 16.0)):
        line(d, u, x0, y, x1, y, 1.2)


SHAPES = [("cloud", cloud), ("sun", sun), ("moon", moon),
          ("drops", drops), ("snow", snow), ("bolt", bolt), ("fog", fog)]

# The large symbol's weather layers. Drawn where the small ones are, under the cloud, they
# landed behind tomorrow's capsule and at the bottom of the fade, about a tenth opaque: on the
# Orb every rainy, snowy or foggy day looked like the same plain cloud (owner, 2026-10-03).
# So the large set draws them up inside the cloud's lower half instead, at one even opacity
# that reads over it, and fog gets a symbol of its own rather than two lines under a cloud.
L_LAYER_SHIFT = {"drops": -6.0, "snow": -6.2, "bolt": -5.0}
L_LAYER_K = 0.42


def shifted(fn, dy):
    def f(d, u):
        layer = Image.new("L", d.im.size, 0)
        fn(ImageDraw.Draw(layer), u)
        d._image.paste(layer.transform(layer.size, Image.AFFINE, (1, 0, 0, 0, 1, -dy * u)), (0, 0), None)
    return f


def render(fn, unit, large, flat=None):
    frame = round(24 * unit)
    big = Image.new("L", (frame * SS, frame * SS), 0)
    fn(ImageDraw.Draw(big), unit * SS)
    im = big.resize((frame, frame), Image.LANCZOS)
    if large:
        im = im.filter(ImageFilter.GaussianBlur(1.2))
        px = im.load()
        for y in range(frame):
            k = flat if flat is not None else L_TOP + (L_BOTTOM - L_TOP) * (y / (frame - 1))
            for x in range(frame):
                px[x, y] = int(px[x, y] * k + 0.5)
    box = im.getbbox()
    return im.crop(box), box[0], box[1], frame


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    out_c, out_h = sys.argv[1:]
    c = ['/* Generated by tools/gen_wx_icons.py. Do not edit; regenerate. */',
         '#include "wx_icons.h"', '']
    h = ['#pragma once', '/* Generated by tools/gen_wx_icons.py. Do not edit; regenerate. */',
         '#include "lvgl.h"', '']
    total = 0
    large_shapes = [(n, shifted(f, L_LAYER_SHIFT[n]) if n in L_LAYER_SHIFT else f) for n, f in SHAPES]
    large_shapes.append(("fogbank", fogbank))
    sets = ((True, L_UNIT, "l", large_shapes), (False, S_UNIT, "s", SHAPES),
            (False, M_UNIT, "m", [x for x in SHAPES if x[0] in ("sun", "moon")]))
    for large, unit, suffix, shapes in sets:
        for name, fn in shapes:
            flat = L_LAYER_K if large and name in ("drops", "snow", "bolt") else (0.30 if large and name == "fogbank" else None)
            im, ox, oy, frame = render(fn, unit, large, flat)
            sym = f"wxi_{name}_{suffix}"
            w, hgt = im.size
            data = list(im.tobytes())
            total += len(data)
            c.append(f"static const uint8_t {sym}_map[] = {{")
            for i in range(0, len(data), 24):
                c.append("    " + ",".join(str(v) for v in data[i:i + 24]) + ",")
            c += ["};",
                  f"const lv_img_dsc_t {sym} = {{",
                  f"    .header = {{.cf = LV_IMG_CF_ALPHA_8BIT, .always_zero = 0, .reserved = 0, .w = {w}, .h = {hgt}}},",
                  f"    .data_size = {len(data)}, .data = {sym}_map,",
                  "};", ""]
            h += [f"extern const lv_img_dsc_t {sym};",
                  f"#define {sym.upper()}_X {ox}   /* offset inside the {frame} px frame */",
                  f"#define {sym.upper()}_Y {oy}"]
        h.append(f"#define WXI_FRAME_{suffix.upper()} {round(24 * unit)}")
        h.append("")
    with open(out_c, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(c))
    with open(out_h, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(h))
    print(f"{out_c}: {total} bytes of alpha")


if __name__ == "__main__":
    main()
