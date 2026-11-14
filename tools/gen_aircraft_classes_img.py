"""Draws docs/img/fork-aircraft-classes.png: the flight radar's size-class symbols for the README.

The shapes are the firmware's own (draw_class_shape in src/radar_view.cpp): the same convex
pieces in the same unit box, nose up at y = -0.5, so the picture cannot drift from the device.
Run with the PlatformIO Python, which has Pillow:
    python tools/gen_aircraft_classes_img.py
"""
import math, os
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.join(os.path.dirname(__file__), "..")
FONT = os.path.join(ROOT, "assets", "fonts", "Sora-Variable.ttf")
OUT = os.path.join(ROOT, "docs", "img", "fork-aircraft-classes.png")

SS = 3                                   # supersampling
W, H = 1640, 700
BG = (5, 5, 6); PANEL = (17, 18, 20); LINE = (38, 40, 44)
INK = (236, 234, 228); MUTED = (143, 139, 131); AMBER = (245, 179, 66)

# (points, mirror) per piece, copied from radar_view.cpp
P_LIGHT = [([(0,-.47),(.05,-.42),(.055,-.06),(.025,.44),(-.025,.44),(-.055,-.06)], False),
           ([(-.56,-.19),(.56,-.19),(.56,-.06),(-.56,-.06)], False),
           ([(-.2,.35),(.2,.35),(.2,.44),(-.2,.44)], False),
           ([(-.17,-.53),(.17,-.53),(.17,-.49),(-.17,-.49)], False)]
P_SMALL = [([(0,-.5),(.045,-.43),(.06,.06),(.04,.47),(-.04,.47),(-.06,.06)], False),
           ([(.055,-.08),(.44,.1),(.44,.15),(.06,.06)], True),
           ([(.055,.2),(.13,.2),(.14,.33),(.055,.33)], True),
           ([(.03,.38),(.22,.45),(.22,.49),(.03,.47)], True)]
P_LARGE = [([(0,-.5),(.05,-.43),(.055,.06),(.03,.45),(-.03,.45),(-.055,.06)], False),
           ([(.055,-.12),(.5,.12),(.5,.18),(.055,.06)], True),
           ([(.04,.33),(.2,.42),(.2,.47),(.03,.45)], True),
           ([(.1775,-.02),(.2225,-.02),(.2225,.1),(.1775,.1)], True)]
P_HEAVY = [([(0,-.5),(.075,-.41),(.08,.08),(.04,.46),(-.04,.46),(-.08,.08)], False),
           ([(.08,-.13),(.53,.15),(.53,.22),(.08,.08)], True),
           ([(.06,.34),(.23,.43),(.23,.48),(.04,.46)], True),
           ([(.17,-.04),(.23,-.04),(.23,.1),(.17,.1)], True),
           ([(.325,.05),(.375,.05),(.375,.17),(.325,.17)], True)]
P_HELI = [([(0,-.26),(.1,-.18),(.12,-.02),(.06,.13),(-.06,.13),(-.12,-.02)], False),
          ([(-.025,.1),(.025,.1),(.025,.48),(-.025,.48)], False)]

CLASSES = [
    ("light", P_LIGHT, 13, "Light aircraft", "A1", "Cessna 172, Piper, gliders, ultralights"),
    ("small", P_SMALL, 16, "Small aircraft", "A2", "Learjet, Citation, ATR, Dash 8"),
    ("large", P_LARGE, 21, "Airliner", "A3", "A319, A320, A321, Boeing 737"),
    ("heavy", P_HEAVY, 28, "Wide-body", "A4 / A5", "Boeing 777, 787, A350, A380"),
    ("heli",  P_HELI,  20, "Helicopter", "A7", "rescue, police, ADAC"),
]


def font(px, weight):
    f = ImageFont.truetype(FONT, px * SS)
    try: f.set_variation_by_axes([weight])
    except Exception: pass
    return f


def shape(d, pieces, key, cx, cy, s, hdg, col):
    th = math.radians(hdg); c, sn = math.cos(th), math.sin(th)
    m = lambda x, y: (cx + (x * c - y * sn) * s, cy + (x * sn + y * c) * s)
    for pts, mirror in pieces:
        d.polygon([m(x, y) for x, y in pts], fill=col)
        if mirror: d.polygon([m(-x, y) for x, y in reversed(pts)], fill=col)
    if key == "heli":
        w = max(1, round(s * .05))
        for a, b in (((-.4, -.32), (.4, .24)), ((.4, -.32), (-.4, .24)), ((-.1, .45), (.1, .45))):
            d.line([m(*a), m(*b)], fill=col, width=w)
        hx, hy = m(0, -.04); r = .48 * s
        dim = tuple(int(v * .55 + b * .45) for v, b in zip(col, (0, 0, 0)))
        d.ellipse([hx - r, hy - r, hx + r, hy + r], outline=dim, width=max(1, round(s * .035)))


img = Image.new("RGB", (W * SS, H * SS), BG)
d = ImageDraw.Draw(img)
S = lambda v: v * SS

# ---- left: the scope, device pixels scaled 1.3x
SC = 1.3; CX, CY, R = 330, 350, 214 * SC
d.ellipse([S(CX - R - 18), S(CY - R - 18), S(CX + R + 18), S(CY + R + 18)], fill=(30, 31, 34))
d.ellipse([S(CX - R), S(CY - R), S(CX + R), S(CY + R)], fill=(10, 9, 8))
import random
random.seed(7)
for _ in range(9):          # road lines, like the map behind the scope
    x, y, a = CX + random.uniform(-260, 260), CY + random.uniform(-260, 260), random.uniform(0, 6.3)
    pts = [(x, y)]
    for _ in range(30):
        a += random.uniform(-.25, .25); x += math.cos(a) * 18; y += math.sin(a) * 18
        if math.hypot(x - CX, y - CY) < R - 4: pts.append((x, y))
        else: break
    if len(pts) > 2: d.line([(S(px), S(py)) for px, py in pts], fill=(43, 39, 34), width=round(S(1.5)))
for km in (10, 20, 30):
    rr = km * R / 40
    d.ellipse([S(CX - rr), S(CY - rr), S(CX + rr), S(CY + rr)], outline=(70, 55, 30), width=S(1))
f_small = font(13, 400)
for km in (10, 20, 30):
    d.text((S(CX + 5), S(CY - km * R / 40 + 3)), f"{km} km", font=f_small, fill=MUTED)
d.ellipse([S(CX - 4.5), S(CY - 4.5), S(CX + 4.5), S(CY + 4.5)], fill=AMBER)
d.ellipse([S(CX - R), S(CY - R), S(CX + R), S(CY + R)], outline=(120, 90, 40), width=S(2))
traffic = [("large", -18, -12, 105, "DLH4CM", "A320"), ("large", 14, 20, 300, "EWG7LT", "A319"),
           ("heavy", -24, 15, 62, "UAE55", "B77W"), ("heavy", 21, -22, 228, "QTR8EV", "A359"),
           ("light", -9, 5, 20, "D-EKLW", "C172"), ("small", -9, 25, 340, "D-IBAD", "C25A"),
           ("heli", 11, 4, 250, "CHX8", "EC35"), ("large", 28, 7, 270, "RYR2KP", "B38M")]
f_cs, f_ty = font(13, 600), font(12, 400)
by = {c[0]: c for c in CLASSES}
for key, kx, ky, hdg, cs, ty in traffic:
    _, pieces, px, *_ = by[key]
    x, y = CX + kx * R / 40, CY + ky * R / 40
    shape(d, pieces, key, S(x), S(y), S(px * SC), hdg, AMBER)
    d.text((S(x + px * SC * .55 + 6), S(y - 15)), cs, font=f_cs, fill=INK)
    d.text((S(x + px * SC * .55 + 6), S(y + 1)), ty, font=f_ty, fill=MUTED)

# ---- right: the legend
LX, LY, LW = 700, 40, 900
d.rounded_rectangle([S(LX), S(LY), S(LX + LW), S(H - 40)], radius=S(18), fill=PANEL, outline=LINE, width=S(1))
f_head, f_name, f_ex, f_code = font(15, 500), font(24, 600), font(17, 300), font(17, 500)
d.text((S(LX + 32), S(LY + 26)), "AIRCRAFT BY SIZE CLASS", font=f_head, fill=MUTED)
row_h = (H - 40 - LY - 80) / 5
for i, (key, pieces, px, name, cat, ex) in enumerate(CLASSES):
    y0 = LY + 70 + i * row_h
    if i: d.line([S(LX + 24), S(y0), S(LX + LW - 24), S(y0)], fill=LINE, width=S(1))
    tile = 86; tx, ty = LX + 32, y0 + (row_h - tile) / 2
    d.rounded_rectangle([S(tx), S(ty), S(tx + tile), S(ty + tile)], radius=S(12), fill=(0, 0, 0))
    shape(d, pieces, key, S(tx + tile / 2), S(ty + tile / 2), S(60), 45, AMBER)
    d.text((S(tx + tile + 28), S(y0 + row_h / 2 - 30)), name, font=f_name, fill=INK)
    d.text((S(tx + tile + 28), S(y0 + row_h / 2 + 4)), ex, font=f_ex, fill=MUTED)
    d.text((S(LX + LW - 40), S(y0 + row_h / 2 - 28)), cat, font=f_code, fill=AMBER, anchor="ra")
    d.text((S(LX + LW - 40), S(y0 + row_h / 2 + 4)), f"{px} px", font=f_ex, fill=MUTED, anchor="ra")

img = img.resize((W, H), Image.LANCZOS)
img.save(OUT, optimize=True)
print(OUT, img.size, os.path.getsize(OUT), "bytes")
