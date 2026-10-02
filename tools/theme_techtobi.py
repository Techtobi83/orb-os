#!/usr/bin/env python3
"""Draw the TechTobi theme's clock (dial plate, hands, shadows, balance wheel), its splash, and the
settings and menu plates.

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
def dial_base(R=202):
    """The dial both the clock and the splash stand on: warm black, sunburst, railway track.

    R is the amber rim's radius. The clock keeps 202, leaving room outside for the firmware's
    day ring; the screens without one (menus, Flight Tracker) take it out to 230, nearly the
    glass's edge. Everything else is placed relative to the rim."""
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
    circle(ImageDraw.Draw(m), C, C, R, fill=255)
    dial.putalpha(m)
    big.alpha_composite(dial)

    # Sunburst.
    segs = []
    for i in range(180):
        a = i * math.pi / 90
        segs.append(((C + 14 * math.sin(a), C - 14 * math.cos(a)), (C + (R - 4) * math.sin(a), C - (R - 4) * math.cos(a))))
    lines(big, segs, (0xFF, 0xF3, 0xD6), 0.8, 12)

    ring(big, C, C, R, 2, (0x6B, 0x50, 0x23))
    ring(big, C, C, R - 4, 1, (0x2A, 0x22, 0x17))
    ring(big, C, C, R - 16, 0.8, WHITE, 90)
    ring(big, C, C, R - 8, 0.8, WHITE, 90)
    rail = []
    for i in range(60):
        a = i * math.pi / 30
        rail.append(((C + (R - 16) * math.sin(a), C - (R - 16) * math.cos(a)), (C + (R - 8) * math.sin(a), C - (R - 8) * math.cos(a))))
    lines(big, rail, WHITE, 1, 140)
    return big


def plate(title="DAY RING"):
    big = dial_base()
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

    # Two lines, no maker's name (owner, 2026-10-02): the dial says what it is, not whose.
    # English in clock_plate.png, German in clock_plate_de.png (Settings > Language).
    text_spaced(big, title, C, 110, 14, 600, LUME, 4.5)
    text_spaced(big, "CHRONOMETER", C, 129, 12, 500, (0xA8, 0xA3, 0x99), 3)

    return big.resize((W, W), Image.LANCZOS).convert("RGBA")


# ---- the splash (splash.png) -----------------------------------------------------------------
def star4(d, cx, cy, long_r, short_r, fill):
    """A four-pointed compass star, long points up and down."""
    pts = []
    for k in range(8):
        a = k * math.pi / 4
        r = long_r if k in (0, 4) else (short_r if k in (2, 6) else short_r * 0.32)
        pts.append(P(cx + r * math.sin(a), cy - r * math.cos(a)))
    d.polygon(pts, fill=fill)


def steel_text(big, text, x_left, baseline, size, weight):
    """Blocky steel letters: a light-to-dark vertical fill, a dark edge, a soft drop shadow."""
    f = ImageFont.truetype(FONT, size * SS)
    f.set_variation_by_axes([weight])
    m = Image.new("L", big.size, 0)
    ImageDraw.Draw(m).text((x_left * SS, baseline * SS), text, font=f, fill=255, anchor="ls")
    sh = m.filter(ImageFilter.GaussianBlur(2.2 * SS))
    sl = Image.new("RGBA", big.size, (0, 0, 0, 255))
    sl.putalpha(Image.eval(sh, lambda v: int(v * 0.8)))
    sl = sl.transform(big.size, Image.AFFINE, (1, 0, -2 * SS, 0, 1, -3 * SS))
    big.alpha_composite(sl)
    edge = m.filter(ImageFilter.MaxFilter(5))
    el = Image.new("RGBA", big.size, (0x1A, 0x14, 0x0C, 255))
    el.putalpha(edge)
    big.alpha_composite(el)
    box = m.getbbox()
    g = Image.new("RGB", (4, 64))
    for y in range(64):
        for x in range(4):
            g.putpixel((x, y), lerp_stops([(0, (0xFF, 0xFC, 0xF4)), (0.45, (0xE4, 0xDF, 0xD4)),
                                           (0.55, (0xA3, 0x9E, 0x93)), (1, (0xD6, 0xD0, 0xC4))], y / 63))
    gi = Image.new("RGBA", big.size, (0, 0, 0, 0))
    gi.paste(g.resize((box[2] - box[0], box[3] - box[1]), Image.BILINEAR), (box[0], box[1]))
    gi.putalpha(Image.composite(m, Image.new("L", big.size, 0), gi.getchannel("A")))
    big.alpha_composite(gi)
    return box[2] / SS


def text_width(text, size, weight):
    f = ImageFont.truetype(FONT, size)
    f.set_variation_by_axes([weight])
    return f.getlength(text)


LOGO_LETTERS = os.path.join(os.path.dirname(__file__), "..", "assets", "iron_orbit_letters.png")


def splash():
    """The start screen: the chronometer's dial with the owner's IRON ORBIT logo on it.

    The logo's contours are measured off the owner's artwork (assets/iron_orbit_logo.png,
    1672x941) and rebuilt clean, without its rust: the long plaque with pointed ends and a
    rim, the big circle through its middle with a compass star top and bottom, a rivet at
    each end, the planet with its tilted ring as the O. The letters are the artwork's own,
    cut out once into assets/iron_orbit_letters.png. All coordinates below are the
    artwork's; LS and the map() lambda scale them onto the dial. Clock colours: amber metal
    for the frame, warm steel for the letters and the planet."""
    big = dial_base()
    ring(big, C, C, 214, 10, (0x16, 0x1B, 0x24))         # the day ring's track, as on the clock
    amber = gradient(big.size, AMBER)

    X0, X1, YC = 55, 1620, 450                          # the artwork's tips and axis
    LS = 404.0 / (X1 - X0)                               # artwork px -> dial px
    XC = (X0 + X1) / 2
    def M(x, y):                                         # artwork -> 4x canvas
        return ((C + (x - XC) * LS) * SS, (C + (y - YC) * LS) * SS)
    def ell(d, cx, cy, r, **kw):
        a, b = M(cx - r, cy - r), M(cx + r, cy + r)
        d.ellipse([a[0], a[1], b[0], b[1]], **kw)

    CX, CR = 845, 252                                    # the big circle

    def plaque(d, inset):
        """Plate + circle + the shoulders joining them, shrunk by `inset` artwork px."""
        i = inset
        d.polygon([M(X0 + i * 1.6, YC), M(118 + i * 0.7, 315 + i), M(650, 281 + i), M(1040, 281 + i),
                   M(1557 - i * 0.7, 315 + i), M(X1 - i * 1.6, YC), M(1557 - i * 0.7, 585 - i),
                   M(1040, 619 - i), M(650, 619 - i), M(118 + i * 0.7, 585 - i)], fill=255)
        ell(d, CX, YC, CR - i, fill=255)
        for sx in (-1, 1):                               # the shoulders, top and bottom
            xa, xb = CX + sx * 225, CX + sx * 155
            for sy in (-1, 1):
                ya = YC + sy * (169 - i)
                yb = YC + sy * (199 - i)
                d.polygon([M(xa, ya), M(xb, yb), M(xb, ya)], fill=255)

    # A double frame, as in the artwork: an outer rim, a dark gap, a thinner inner line.
    outer = Image.new("L", big.size, 0); plaque(ImageDraw.Draw(outer), 0)
    inner = Image.new("L", big.size, 0); plaque(ImageDraw.Draw(inner), 10)
    line_o = Image.new("L", big.size, 0); plaque(ImageDraw.Draw(line_o), 16)
    line_i = Image.new("L", big.size, 0); plaque(ImageDraw.Draw(line_i), 21)

    # The compass stars, top and bottom of the circle.
    stars = Image.new("L", big.size, 0)
    sd = ImageDraw.Draw(stars)
    for sy in (205, 695):
        pts = []
        for k in range(8):
            a = k * math.pi / 4
            r = 84 if k in (0, 4) else (42 if k in (2, 6) else 16)
            pts.append(M(845 + r * math.sin(a), sy - r * math.cos(a)))
        sd.polygon(pts, fill=255)

    # Drop shadow of the whole badge.
    from PIL import ImageChops
    sh = ImageChops.lighter(outer, stars).filter(ImageFilter.GaussianBlur(4 * SS))
    sl = Image.new("RGBA", big.size, (0, 0, 0, 255)); sl.putalpha(Image.eval(sh, lambda v: int(v * 0.75)))
    big.alpha_composite(sl.transform(big.size, Image.AFFINE, (1, 0, -2 * SS, 0, 1, -4 * SS)))

    al = amber.convert("RGBA"); al.putalpha(outer); big.alpha_composite(al)
    face = Image.new("RGB", (64, 64))
    for y in range(64):
        for x in range(64):
            face.putpixel((x, y), lerp_stops([(0, (0x1F, 0x1B, 0x16)), (1, (0x0A, 0x09, 0x07))], y / 63))
    fi = face.resize(big.size, Image.BILINEAR).convert("RGBA"); fi.putalpha(inner); big.alpha_composite(fi)
    # A dark groove just inside the rim gives it depth.
    groove = ImageChops.subtract(inner, inner.filter(ImageFilter.MinFilter(int(1.5 * SS) | 1)))
    gl = Image.new("RGBA", big.size, (0x2A, 0x1E, 0x0E, 255)); gl.putalpha(groove); big.alpha_composite(gl)
    al = amber.convert("RGBA"); al.putalpha(ImageChops.subtract(line_o, line_i)); big.alpha_composite(al)
    # The stars sit ON the circle's rim, over the frame, as in the artwork.
    al = amber.convert("RGBA"); al.putalpha(stars); big.alpha_composite(al)

    # Rivets at the ends.
    for rx in (110, 1565):
        rr = 26
        g2 = Image.new("RGB", (32, 32))
        for y in range(32):
            for x in range(32):
                t = min(1.0, math.hypot(x - 11, y - 10) / 22)
                g2.putpixel((x, y), lerp_stops([(0, (0xFF, 0xFC, 0xF2)), (0.5, (0xA8, 0xA2, 0x96)), (1, (0x3A, 0x36, 0x30))], t))
        a, b = M(rx - rr, YC - rr), M(rx + rr, YC + rr)
        sp = Image.new("RGBA", big.size, (0, 0, 0, 0))
        sp.paste(g2.resize((int(b[0] - a[0]), int(b[1] - a[1])), Image.BILINEAR), (int(a[0]), int(a[1])))
        mk = Image.new("L", big.size, 0); ell(ImageDraw.Draw(mk), rx, YC, rr, fill=255)
        rim = Image.new("L", big.size, 0); ell(ImageDraw.Draw(rim), rx, YC, rr + 8, fill=255)
        al = amber.convert("RGBA"); al.putalpha(rim); big.alpha_composite(al)
        sp.putalpha(mk); big.alpha_composite(sp)

    # The planet and its ring: centre (900,465) r 102; ring an ellipse 476 x 116 about
    # (890,440), tilted 39 degrees up to the right, behind the planet at the top.
    PX, PY, PR = 900, 465, 102
    RX, RY, RA, RB, TILT = 890, 440, 238, 58, -39
    ring_parts = []
    for part in ("back", "front"):
        lay = Image.new("L", big.size, 0)
        a, b = M(RX - RA, RY - RB), M(RX + RA, RY + RB)
        ImageDraw.Draw(lay).ellipse([a[0], a[1], b[0], b[1]], outline=255, width=int(18 * LS * SS))
        cut = Image.new("L", big.size, 0)
        cy4 = M(RX, RY)[1]
        if part == "back":
            ImageDraw.Draw(cut).rectangle([0, 0, W * SS, cy4], fill=255)
        else:
            ImageDraw.Draw(cut).rectangle([0, cy4, W * SS, W * SS], fill=255)
        lay = Image.composite(lay, Image.new("L", big.size, 0), cut)
        c4 = M(RX, RY)
        ring_parts.append(lay.rotate(-TILT, center=c4, resample=Image.BICUBIC))
    al = amber.convert("RGBA"); al.putalpha(ring_parts[0]); big.alpha_composite(al)
    g3 = Image.new("RGB", (64, 64))
    for y in range(64):
        for x in range(64):
            t = min(1.0, math.hypot(x - 22, y - 20) / 50)
            g3.putpixel((x, y), lerp_stops([(0, (0xFA, 0xF6, 0xEC)), (0.35, (0xC4, 0xBE, 0xB2)),
                                            (0.75, (0x6F, 0x6B, 0x63)), (1, (0x2E, 0x2B, 0x27))], t))
    a, b = M(PX - PR, PY - PR), M(PX + PR, PY + PR)
    sp = Image.new("RGBA", big.size, (0, 0, 0, 0))
    sp.paste(g3.resize((int(b[0] - a[0]), int(b[1] - a[1])), Image.BILINEAR), (int(a[0]), int(a[1])))
    mk = Image.new("L", big.size, 0); ell(ImageDraw.Draw(mk), PX, PY, PR, fill=255)
    sp.putalpha(mk); big.alpha_composite(sp)
    al = amber.convert("RGBA"); al.putalpha(ring_parts[1]); big.alpha_composite(al)

    # The letters: the artwork's own, as a mask, in warm steel with a dark edge and a shadow.
    src = Image.open(LOGO_LETTERS).convert("L")
    tl = M(0, 0)
    sz = (int(src.width * LS * SS), int(src.height * LS * SS))
    lm = Image.new("L", big.size, 0)
    lm.paste(src.resize(sz, Image.LANCZOS), (int(tl[0]), int(tl[1])))
    lsh = lm.filter(ImageFilter.GaussianBlur(2.2 * SS))
    sl = Image.new("RGBA", big.size, (0, 0, 0, 255)); sl.putalpha(Image.eval(lsh, lambda v: int(v * 0.85)))
    big.alpha_composite(sl.transform(big.size, Image.AFFINE, (1, 0, -2 * SS, 0, 1, -3 * SS)))
    el = Image.new("RGBA", big.size, (0x1A, 0x14, 0x0C, 255)); el.putalpha(lm.filter(ImageFilter.MaxFilter(5)))
    big.alpha_composite(el)
    box = lm.getbbox()
    g = Image.new("RGB", (4, 64))
    for y in range(64):
        for x in range(4):
            g.putpixel((x, y), lerp_stops([(0, (0xFF, 0xFC, 0xF4)), (0.45, (0xE8, 0xE3, 0xD8)),
                                           (0.55, (0xA8, 0xA3, 0x98)), (1, (0xD8, 0xD2, 0xC6))], y / 63))
    gi = Image.new("RGBA", big.size, (0, 0, 0, 0))
    gi.paste(g.resize((box[2] - box[0], box[3] - box[1]), Image.BILINEAR), (box[0], box[1]))
    gi.putalpha(Image.composite(lm, Image.new("L", big.size, 0), gi.getchannel("A")))
    big.alpha_composite(gi)

    text_spaced(big, "TECHTOBI", C, 92, 15, 600, LUME, 5)
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
    plate("TAGESRING").save(os.path.join(out, "clock_plate_de.png"), optimize=True)
    splash().save(os.path.join(out, "splash.png"), optimize=True)
    # Settings and the app menu stand on the same dial, without the clock's furniture, so the
    # wheel of words reads over it and the screens belong to the clock.
    RIM = 230                                           # out to the glass: no day ring here
    menu = dial_base(RIM).resize((W, W), Image.LANCZOS).convert("RGBA")
    menu.save(os.path.join(out, "settings_plate.png"), optimize=True)
    menu.save(os.path.join(out, "menu_plate.png"), optimize=True)
    # The Flight Tracker stands on it too ("A · Bernstein", 2026-10-01): the dial as its plate,
    # and its range rings and crosshair in faint amber over it.
    menu.save(os.path.join(out, "radar_plate.png"), optimize=True)
    # The weather map stands on the same plate. Its rim is kept over the roads and the rain
    # by an inverted keep-out circle in weather_style.json (r = RIM - 19), which puts this
    # plate back everywhere outside it: the weather screen has no rings layer to carry it.
    menu.save(os.path.join(out, "weather_plate.png"), optimize=True)
    # And the Livestream: livecam_style.json's frameR (the same 211) lays it over the camera.
    menu.save(os.path.join(out, "livecam_plate.png"), optimize=True)
    rings = Image.new("RGBA", (W * SS, W * SS), (0, 0, 0, 0))
    for r in (68, 136, 204):
        ring(rings, C, C, r, 1.1, LUME, 72)
    lines(rings, [((C, C - 206), (C, C + 206)), ((C - 206, C), (C + 206, C))], LUME, 1, 36)
    rings = rings.resize((W, W), Image.LANCZOS)
    # The rim rides in this layer too, so it lies OVER the roads: the firmware lays the rings
    # over the map (radar_view's flatten), and the plate alone sits under it. Everything from
    # just inside the railway track outward is copied in opaque, so a road ends at the frame.
    frame_mask = Image.new("L", (W * SS, W * SS), 0)
    fd = ImageDraw.Draw(frame_mask)
    fd.rectangle([0, 0, W * SS, W * SS], fill=255)
    circle(fd, C, C, RIM - 19, fill=0)
    frame_mask = frame_mask.resize((W, W), Image.LANCZOS)
    frame = menu.copy(); frame.putalpha(frame_mask)
    rings.alpha_composite(frame)
    rings.save(os.path.join(out, "radar_rings.png"), optimize=True)
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
