#!/usr/bin/env python3
"""Draw the TechTobi theme's clock: the dial plate, three hands and their shadows.

    python tools/theme_techtobi.py <out_dir>

The "Tagesring-Chronometer" from the design preview of 2026-09-30, in the forecast screen's
colours: a sunburst dial, a railway minute track, faceted steel indices with amber lume, an
open balance wheel at 9, a date window at 3, and two-tone sword hands. The 24-hour day ring
round the edge and the date inside the window are NOT in these pictures: the firmware draws
them (clock_style "dayRing" and "dateDE"), because both change with the day.

Everything is drawn at 4x and scaled down. Hands are cropped to their ink and the pivot (the
dial centre) is printed for clock_style.json.
"""
import json
import math
import os
import sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

SS = 4
W = 466
C = 233
FONT = os.path.join(os.path.dirname(__file__), "..", "assets", "fonts", "Sora-Variable.ttf")

AMBER = [(0.0, (0xFB, 0xD9, 0x8A)), (0.5, (0xD0, 0x92, 0x2E)), (1.0, (0x6E, 0x47, 0x15))]
COPPER = [(0.0, (0xE2, 0x9A, 0x68)), (1.0, (0x6B, 0x32, 0x19))]
STEEL_L = [(0.0, (0xFF, 0xFF, 0xFF)), (1.0, (0xE4, 0xDF, 0xD4))]
STEEL_D = [(0.0, (0xA3, 0x9E, 0x93)), (1.0, (0x6F, 0x6B, 0x63))]
LUME = (0xF5, 0xB3, 0x42)
WHITE = (0xF2, 0xF0, 0xEA)


def lerp_stops(stops, t):
    for (t0, c0), (t1, c1) in zip(stops, stops[1:]):
        if t <= t1:
            k = 0 if t1 == t0 else (t - t0) / (t1 - t0)
            return tuple(int(c0[i] + (c1[i] - c0[i]) * k) for i in range(3))
    return stops[-1][1]


def gradient(size, stops, diagonal=True):
    """A gradient image, diagonal (top-left to bottom-right) or horizontal."""
    w, h = size
    small = Image.new("RGB", (64, 64))
    px = small.load()
    for y in range(64):
        for x in range(64):
            t = (x + y) / 126 if diagonal else x / 63
            px[x, y] = lerp_stops(stops, t)
    return small.resize((w, h), Image.BILINEAR)


def fill(base, draw_mask, paint):
    """Composite `paint` (a colour, or an image the size of base) through a mask drawn by draw_mask."""
    mask = Image.new("L", base.size, 0)
    draw_mask(ImageDraw.Draw(mask))
    if isinstance(paint, tuple):
        layer = Image.new("RGBA", base.size, paint + (255,))
    else:
        layer = paint.convert("RGBA")
        mask = Image.composite(mask, Image.new("L", base.size, 0), layer.getchannel("A"))
    layer.putalpha(mask)
    base.alpha_composite(layer)


def P(x, y):
    return (x * SS, y * SS)


def rot(x, y, a, cx=C, cy=C):
    return (cx + (x - cx) * math.cos(a) - (y - cy) * math.sin(a), cy + (x - cx) * math.sin(a) + (y - cy) * math.cos(a))


def poly(pts, a=0.0):
    return [P(*rot(x, y, a)) for x, y in pts]


def circle(d, cx, cy, r, **kw):
    d.ellipse([(cx - r) * SS, (cy - r) * SS, (cx + r) * SS, (cy + r) * SS], **kw)


def ring(base, cx, cy, r, width, color, alpha=255):
    layer = Image.new("RGBA", base.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    d.ellipse([(cx - r) * SS, (cy - r) * SS, (cx + r) * SS, (cy + r) * SS], outline=color + (alpha,),
              width=max(1, int(width * SS)))
    base.alpha_composite(layer)


def lines(base, segs, color, width, alpha):
    layer = Image.new("RGBA", base.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    for a, b in segs:
        d.line([P(*a), P(*b)], fill=color + (alpha,), width=max(1, int(width * SS)))
    base.alpha_composite(layer)


def text_spaced(base, text, cx, baseline, size, weight, color, spacing, alpha=255):
    f = ImageFont.truetype(FONT, size * SS)
    f.set_variation_by_axes([weight])
    widths = [f.getlength(ch) for ch in text]
    total = sum(widths) + spacing * SS * (len(text) - 1)
    x = cx * SS - total / 2
    layer = Image.new("RGBA", base.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    for ch, w in zip(text, widths):
        d.text((x, baseline * SS), ch, font=f, fill=color + (alpha,), anchor="ls")
        x += w + spacing * SS
    base.alpha_composite(layer)


def gear_pts(cx, cy, r, n, depth):
    step = 2 * math.pi / n
    pts = []
    for i in range(n):
        t = i * step
        for rr, tt in ((r - depth, t - 0.3 * step), (r, t - 0.15 * step), (r, t + 0.15 * step), (r - depth, t + 0.3 * step)):
            pts.append(P(cx + rr * math.sin(tt), cy - rr * math.cos(tt)))
    return pts


# ---- the plate -------------------------------------------------------------------------------
def plate():
    big = Image.new("RGBA", (W * SS, W * SS), (0, 0, 0, 255))

    # Dial: a radial fall-off from warm near-black to black, centred a little above the middle.
    dial = Image.new("RGBA", big.size, (0, 0, 0, 0))
    g = Image.new("RGB", (128, 128))
    gp = g.load()
    for y in range(128):
        for x in range(128):
            dx, dy = (x - 64) / 64, (y - 58) / 64
            t = min(1.0, math.hypot(dx, dy) / 1.05)
            gp[x, y] = lerp_stops([(0, (0x1C, 0x18, 0x13)), (0.7, (0x0C, 0x0B, 0x09)), (1, (0x04, 0x04, 0x04))], t)
    dial = g.resize(big.size, Image.BILINEAR).convert("RGBA")
    m = Image.new("L", big.size, 0)
    circle(ImageDraw.Draw(m), C, C, 202, fill=255)
    dial.putalpha(m)
    big.alpha_composite(dial)

    # Sunburst.
    segs = []
    for i in range(180):
        a = i * math.pi / 90
        segs.append(((C + 14 * math.sin(a), C - 14 * math.cos(a)), (C + 198 * math.sin(a), C - 198 * math.cos(a))))
    lines(big, segs, (0xFF, 0xF3, 0xD6), 0.8, 12)

    ring(big, C, C, 202, 2, (0x6B, 0x50, 0x23))
    ring(big, C, C, 198, 1, (0x2A, 0x22, 0x17))
    ring(big, C, C, 186, 0.8, WHITE, 90)
    ring(big, C, C, 194, 0.8, WHITE, 90)
    rail = []
    for i in range(60):
        a = i * math.pi / 30
        rail.append(((C + 186 * math.sin(a), C - 186 * math.cos(a)), (C + 194 * math.sin(a), C - 194 * math.cos(a))))
    lines(big, rail, WHITE, 1, 140)
    ring(big, C, C, 118, 0.8, LUME, 90)

    # Indices: shadow, light facet, dark facet, lume. 3 is the date and 9 the balance wheel.
    shadow = Image.new("L", big.size, 0)
    sd = ImageDraw.Draw(shadow)
    for h in (1, 2, 4, 5, 6, 7, 8, 10, 11):
        a = h * math.pi / 6
        sd.polygon(poly([(C - 6 + 2, 42 + 3), (C + 2, 36 + 3), (C + 6 + 2, 42 + 3), (C + 6 + 2, 76 + 3), (C + 2, 80 + 3), (C - 6 + 2, 76 + 3)], a), fill=180)
    shadow = shadow.filter(ImageFilter.GaussianBlur(2.4 * SS))
    sl = Image.new("RGBA", big.size, (0, 0, 0, 255))
    sl.putalpha(shadow)
    big.alpha_composite(sl)
    steelL = gradient(big.size, STEEL_L, diagonal=False)
    steelD = gradient(big.size, STEEL_D, diagonal=False)
    for h in (1, 2, 4, 5, 6, 7, 8, 10, 11):
        a = h * math.pi / 6
        fill(big, lambda d, a=a: d.polygon(poly([(C, 36), (C - 6, 42), (C - 6, 76), (C, 80)], a), fill=255), steelL)
        fill(big, lambda d, a=a: d.polygon(poly([(C, 36), (C + 6, 42), (C + 6, 76), (C, 80)], a), fill=255), steelD)
        fill(big, lambda d, a=a: d.polygon(poly([(C - 1.6, 46), (C + 1.6, 46), (C + 1.6, 70), (C - 1.6, 70)], a), fill=255), LUME)
    amber = gradient(big.size, AMBER)
    for x0 in (221, 235):
        fill(big, lambda d, x0=x0: d.rectangle([P(x0, 34), P(x0 + 10, 74)], fill=255), (0x3A, 0x2A, 0x10))
        fill(big, lambda d, x0=x0: d.rectangle([P(x0 + 0.8, 34.8), P(x0 + 9.2, 73.2)], fill=255), amber)

    # The open balance wheel at 9.
    hx, hy = 128, 233
    win = Image.new("RGBA", big.size, (0, 0, 0, 0))
    circle(ImageDraw.Draw(win), hx, hy, 46, fill=(6, 5, 4, 255))
    copper = gradient(big.size, COPPER)
    gm = Image.new("L", big.size, 0)
    ImageDraw.Draw(gm).polygon(gear_pts(150, 256, 30, 20, 4), fill=255)
    cl = copper.convert("RGBA"); cl.putalpha(gm); win.alpha_composite(cl)
    # The wheel itself is NOT in the plate: it is clock_static1.png, swung by the firmware
    # ("swing1"). The window shows its dark ground and the copper wheel of the train.
    clip = Image.new("L", big.size, 0)
    circle(ImageDraw.Draw(clip), hx, hy, 46, fill=255)
    win.putalpha(Image.composite(win.getchannel("A"), Image.new("L", big.size, 0), clip))
    big.alpha_composite(win)
    bez = Image.new("L", big.size, 0)
    ImageDraw.Draw(bez).ellipse([(hx - 47) * SS, (hy - 47) * SS, (hx + 47) * SS, (hy + 47) * SS], outline=255, width=4 * SS)
    bl = amber.convert("RGBA"); bl.putalpha(bez); big.alpha_composite(bl)
    ring(big, hx, hy, 51, 1, (0x3A, 0x2A, 0x10))
    for sx, sy in ((128, 185), (169.6, 257), (86.4, 257)):
        fill(big, lambda d, sx=sx, sy=sy: circle(d, sx, sy, 3, fill=255), amber)
        ring(big, sx, sy, 3, 0.6, (0x2A, 0x1C, 0x0C))

    # The date window's frame. The date itself is the firmware's (dateDE at 339,233).
    fill(big, lambda d: d.rounded_rectangle([P(284, 205), P(394, 261)], radius=12 * SS, fill=255), (0x3A, 0x2A, 0x10))
    fill(big, lambda d: d.rounded_rectangle([P(287, 208), P(391, 258)], radius=10 * SS, fill=255), amber)
    dg = Image.new("RGB", (4, 64))
    for y in range(64):
        for x in range(4):
            dg.putpixel((x, y), lerp_stops([(0, (0x05, 0x07, 0x0A)), (1, (0x14, 0x1B, 0x24))], y / 63))
    dimg = Image.new("RGBA", big.size, (0, 0, 0, 0))
    dimg.paste(dg.resize((96 * SS, 42 * SS), Image.BILINEAR), (291 * SS, 212 * SS))
    fill(big, lambda d: d.rounded_rectangle([P(291, 212), P(387, 254)], radius=8 * SS, fill=255), dimg)

    text_spaced(big, "TECHTOBI", C, 108, 15, 600, LUME, 5)
    text_spaced(big, "TAGESRING · CHRONOMETER", C, 130, 12, 500, (0xA8, 0xA3, 0x99), 2.5)

    return big.resize((W, W), Image.LANCZOS).convert("RGBA")


# ---- the balance wheel (clock_static1.png) ----------------------------------------------------
def balance_wheel():
    """The rim, three arms, hairspring and jewelled hub, alone, centred on the window at 4x."""
    hx, hy = 128, 233
    big = Image.new("RGBA", (W * SS, W * SS), (0, 0, 0, 0))
    amber = gradient(big.size, AMBER)
    wm = Image.new("L", big.size, 0)
    wd = ImageDraw.Draw(wm)
    wd.ellipse([(hx - 30) * SS, (hy - 30) * SS, (hx + 30) * SS, (hy + 30) * SS], outline=255, width=5 * SS)
    for a in (0, math.pi / 3, 2 * math.pi / 3):
        wd.line([P(hx + 30 * math.sin(a), hy - 30 * math.cos(a)), P(hx - 30 * math.sin(a), hy + 30 * math.cos(a))], fill=255, width=3 * SS)
    al = amber.convert("RGBA"); al.putalpha(wm); big.alpha_composite(al)
    sp = []
    for i in range(141):
        t = i / 140 * math.pi * 8
        rr = 3 + i / 140 * 20
        sp.append(P(hx + rr * math.cos(t), hy + rr * math.sin(t)))
    spl = Image.new("RGBA", big.size, (0, 0, 0, 0))
    ImageDraw.Draw(spl).line(sp, fill=LUME + (200,), width=max(1, int(0.9 * SS)))
    big.alpha_composite(spl)
    d = ImageDraw.Draw(big)
    circle(d, hx, hy, 5, fill=(0x2A, 0x1C, 0x0C, 255))
    circle(d, hx, hy, 2.5, fill=(0xB2, 0x33, 0x2E, 255))
    im = big.resize((W, W), Image.LANCZOS)
    r = 34                                   # rim 30 + half its width, and a margin
    return im.crop((hx - r, hy - r, hx + r + 1, hy + r + 1)), r, r


# ---- hands -----------------------------------------------------------------------------------
def hand_layers(kind):
    """The hand drawn upright about the dial centre, at 4x, full canvas."""
    big = Image.new("RGBA", (W * SS, W * SS), (0, 0, 0, 0))
    steelL = gradient(big.size, STEEL_L, diagonal=False)
    steelD = gradient(big.size, STEEL_D, diagonal=False)
    if kind == "hour":
        fill(big, lambda d: d.polygon(poly([(C, 124), (C - 13, 152), (C - 7, 238), (C, 248)]), fill=255), steelL)
        fill(big, lambda d: d.polygon(poly([(C, 124), (C + 13, 152), (C + 7, 238), (C, 248)]), fill=255), steelD)
        fill(big, lambda d: d.polygon(poly([(C, 142), (C + 5.5, 155), (C + 3.5, 218), (C - 3.5, 218), (C - 5.5, 155)]), fill=255), LUME)
        lines(big, [((C, 142), (C, 218))], (0xFF, 0xE3, 0xA3), 0.8, 200)
    elif kind == "minute":
        fill(big, lambda d: d.polygon(poly([(C, 42), (C - 9, 70), (C - 5, 240), (C, 250)]), fill=255), steelL)
        fill(big, lambda d: d.polygon(poly([(C, 42), (C + 9, 70), (C + 5, 240), (C, 250)]), fill=255), steelD)
        fill(big, lambda d: d.polygon(poly([(C, 58), (C + 4, 72), (C + 2.5, 214), (C - 2.5, 214), (C - 4, 72)]), fill=255), LUME)
        lines(big, [((C, 58), (C, 214))], (0xFF, 0xE3, 0xA3), 0.8, 200)
    else:   # second hand, with the centre cap, which has to sit over all three
        amber = gradient(big.size, AMBER)
        lines(big, [((C, 286), (C, 40))], LUME, 1.8, 255)
        ring(big, C, 268, 9, 3, LUME)
        fill(big, lambda d: circle(d, C, 268, 3, fill=255), LUME)
        fill(big, lambda d: circle(d, C, 66, 4.5, fill=255), (0, 0, 0))
        ring(big, C, 66, 4.5, 2, LUME)
        fill(big, lambda d: circle(d, C, C, 12, fill=255), amber)
        ring(big, C, C, 12, 1, (0x2A, 0x1C, 0x0C))
        fill(big, lambda d: circle(d, C, C, 6, fill=255), (0x1A, 0x12, 0x08))
        fill(big, lambda d: circle(d, C, C, 2.6, fill=255), (0xB2, 0x33, 0x2E))
    return big.resize((W, W), Image.LANCZOS)


def crop_box(im, pad):
    box = im.getchannel("A").getbbox()
    return (max(0, box[0] - pad), max(0, box[1] - pad), min(W, box[2] + pad), min(W, box[3] + pad))


def shadow_of(im):
    a = im.getchannel("A").filter(ImageFilter.GaussianBlur(2.6))
    s = Image.new("RGBA", im.size, (0, 0, 0, 0))
    s.putalpha(Image.eval(a, lambda v: int(v * 0.65)))
    return s


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    plate().save(os.path.join(out, "clock_plate.png"), optimize=True)
    # The firmware turns a hand's shadow about the HAND's pivot, so the two pictures share one
    # crop: the blurred shadow's, which is the larger.
    pivots = {}
    for kind in ("hour", "minute", "second"):
        full = hand_layers(kind)
        shadow = shadow_of(full)
        box = crop_box(shadow, 2)
        full.crop(box).save(os.path.join(out, f"clock_hand_{kind}.png"), optimize=True)
        shadow.crop(box).save(os.path.join(out, f"clock_shadow_{kind}.png"), optimize=True)
        pivots[kind] = {"pivotX": C - box[0], "pivotY": C - box[1]}
    wheel, wx, wy = balance_wheel()
    wheel.save(os.path.join(out, "clock_static1.png"), optimize=True)
    pivots["static1"] = {"pivotX": wx, "pivotY": wy, "centerX": 128, "centerY": 233}
    print(json.dumps(pivots))


if __name__ == "__main__":
    main()
