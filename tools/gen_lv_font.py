#!/usr/bin/env python3
"""Rasterise a TTF into an LVGL 8 font source file (.c), without Node.

lv_font_conv is the usual tool and needs Node, which not every machine that builds this
firmware has. This writes the same format the repo's other fonts use (lv_font_fmt_txt,
4 bpp, uncompressed, no kerning), from Pillow's FreeType, so a face can be added with the
PlatformIO Python alone. Variable fonts take a weight.

    python tools/gen_lv_font.py <font.ttf> <size_px> <weight|-> <name> <chars> <out.c|out.bin>

    out.bin  writes LVGL's binary font format instead, the one lv_font_load() reads at run
             time: a theme font (font_settings.bin, font_menu_current.bin, ...) for the SD
             card. Same glyphs, same packing; <name> is then unused.

    chars   the characters to include, literally (UTF-8). Keep it to what the screen
            draws: every glyph costs flash, and a glyph that is not here draws nothing.

Example (the forecast screen's temperature):
    python tools/gen_lv_font.py Sora.ttf 120 300 font_sora_120 "0123456789-°" src/font_sora_120.c
"""
import sys
from PIL import Image, ImageDraw, ImageFont


def glyph(font, ch):
    x0, y0, x1, y1 = font.getbbox(ch, anchor="ls")
    adv = font.getlength(ch)
    w, h = max(0, x1 - x0), max(0, y1 - y0)
    px = []
    if w and h:
        im = Image.new("L", (w, h), 0)
        ImageDraw.Draw(im).text((-x0, -y0), ch, font=font, fill=255, anchor="ls")
        px = list(im.getdata())
    return {"adv": adv, "w": w, "h": h, "ofs_x": x0, "ofs_y": -y1, "px": px}


def pack4(px):
    # LVGL's fmt_txt packs a glyph's pixels continuously (no row padding), first pixel in
    # the high nibble.
    out = []
    for i in range(0, len(px), 2):
        a = px[i] >> 4
        b = (px[i + 1] >> 4) if i + 1 < len(px) else 0
        out.append((a << 4) | b)
    return out


def runs(codes):
    out, start, prev = [], None, None
    for c in codes:
        if start is None:
            start = prev = c
        elif c == prev + 1:
            prev = c
        else:
            out.append((start, prev)); start = prev = c
    if start is not None:
        out.append((start, prev))
    return out


def write_bin(out, codes, glyphs, ascent, descent, size):
    """LVGL 8's binary font (lv_font_loader.c): head, cmap, loca, glyf; no kerning.

    Every glyph's header is byte-aligned on purpose (16-bit advance in 1/16 px, 8-bit
    offsets and box), so its bitmap starts on a byte and the loader takes the fast path."""
    import struct
    head = struct.pack("<IHHHhHhHhhHHBBBBBBBBBBhH",
                       1, 3, size, ascent, -descent, ascent, -descent, 0, -descent, ascent,
                       0, 0,           # default advance (unused: every glyph has its own), kerning scale
                       1, 0, 1, 4,     # u32 loca offsets, glyph ids (kerning only), advance in 1/16 px, 4 bpp
                       8, 8, 16, 0,    # xy bits, wh bits, advance bits, no compression
                       0, 0, -2, 1)
    head = struct.pack("<I4s", 8 + len(head), b"head") + head

    rs = runs(codes)
    entries, gid = b"", 1
    data_off = 8 + 4 + 16 * len(rs)
    for a, b in rs:
        entries += struct.pack("<IIHHHBB", data_off, a, b - a + 1, gid, 0, 2, 0)   # FORMAT0_TINY
        gid += b - a + 1
    cmap_body = struct.pack("<I", len(rs)) + entries
    cmap = struct.pack("<I4s", 8 + len(cmap_body), b"cmap") + cmap_body

    glyf_body, offsets = b"", []
    for g in [None] + glyphs:                     # glyph id 0 is reserved and empty
        offsets.append(8 + len(glyf_body))
        if g is None:
            glyf_body += struct.pack(">HbbBB", 0, 0, 0, 0, 0)
            continue
        glyf_body += struct.pack(">HbbBB", int(round(g["adv"] * 16)), g["ofs_x"], g["ofs_y"], g["w"], g["h"])
        glyf_body += bytes(pack4(g["px"]))
    glyf = struct.pack("<I4s", 8 + len(glyf_body), b"glyf") + glyf_body
    loca_body = struct.pack("<I", len(offsets)) + b"".join(struct.pack("<I", o) for o in offsets)
    loca = struct.pack("<I4s", 8 + len(loca_body), b"loca") + loca_body
    with open(out, "wb") as f:
        f.write(head + cmap + loca + glyf)
    print(f"{out}: {len(codes)} glyphs, binary, line height {ascent + descent}")


def main():
    if len(sys.argv) != 7:
        raise SystemExit(__doc__)
    path, size, weight, name, chars, out = sys.argv[1:]
    size = int(size)
    font = ImageFont.truetype(path, size)
    if weight != "-":
        font.set_variation_by_axes([int(weight)])
    codes = sorted({ord(c) for c in chars} | {0x20})
    glyphs = [glyph(font, chr(c)) for c in codes]
    ascent, descent = font.getmetrics()
    if out.lower().endswith(".bin"):
        for g in glyphs:
            if not (-128 <= g["ofs_x"] <= 127 and -128 <= g["ofs_y"] <= 127 and g["w"] <= 255 and g["h"] <= 255):
                raise SystemExit(f"a glyph does not fit 8-bit metrics at {size}px")
        write_bin(out, codes, glyphs, ascent, descent, size)
        return

    bitmap, dsc, idx = [], [], 0
    for g in glyphs:
        data = pack4(g["px"])
        adv = int(round(g["adv"] * 16))
        for v, lo, hi, what in ((adv, 0, 4095, "adv_w"), (g["w"], 0, 255, "box_w"), (g["h"], 0, 255, "box_h"),
                                (g["ofs_x"], -128, 127, "ofs_x"), (g["ofs_y"], -128, 127, "ofs_y")):
            if not lo <= v <= hi:
                raise SystemExit(f"{what}={v} does not fit LVGL 8's glyph descriptor at {size}px")
        dsc.append(f"    {{.bitmap_index = {idx}, .adv_w = {adv}, .box_w = {g['w']}, .box_h = {g['h']}, "
                   f".ofs_x = {g['ofs_x']}, .ofs_y = {g['ofs_y']}}}")
        bitmap.append(data)
        idx += len(data)

    lines = [
        "/*******************************************************************************",
        f" * {name}: {path.replace(chr(92), '/').split('/')[-1]}, {size} px, weight {weight}, 4 bpp",
        " * Generated by tools/gen_lv_font.py. Do not edit; regenerate.",
        " * Sora is (c) The Sora Project Authors, SIL Open Font License 1.1 (see NOTICE).",
        " ******************************************************************************/",
        '#include "lvgl.h"',
        "",
        "static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {",
    ]
    for c, data in zip(codes, bitmap):
        lines.append(f"    /* U+{c:04X} */")
        for i in range(0, len(data), 16):
            lines.append("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 16]) + ",")
    if idx == 0:
        lines.append("    0x00,")
    lines += ["};", "", "static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {",
              "    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0},"]
    lines.append(",\n".join(dsc))
    lines += ["};", "", "static const lv_font_fmt_txt_cmap_t cmaps[] = {"]
    gid = 1
    rs = runs(codes)
    for a, b in rs:
        lines.append(f"    {{.range_start = {a}, .range_length = {b - a + 1}, .glyph_id_start = {gid}, "
                     ".unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, "
                     ".type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY},")
        gid += b - a + 1
    lines += [
        "};", "",
        "static lv_font_fmt_txt_glyph_cache_t cache;",
        "static const lv_font_fmt_txt_dsc_t font_dsc = {",
        "    .glyph_bitmap = glyph_bitmap, .glyph_dsc = glyph_dsc, .cmaps = cmaps,",
        f"    .kern_dsc = NULL, .kern_scale = 0, .cmap_num = {len(rs)}, .bpp = 4,",
        "    .kern_classes = 0, .bitmap_format = 0, .cache = &cache",
        "};", "",
        f"const lv_font_t {name} = {{",
        "    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,",
        "    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,",
        f"    .line_height = {ascent + descent}, .base_line = {descent},",
        "    .subpx = LV_FONT_SUBPX_NONE, .underline_position = -2, .underline_thickness = 1,",
        "    .dsc = &font_dsc, .fallback = NULL, .user_data = NULL,",
        "};", "",
    ]
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    print(f"{out}: {len(codes)} glyphs, {idx} bitmap bytes, line height {ascent + descent}")


if __name__ == "__main__":
    main()
