#pragma once
#include <lvgl.h>

// The sizes this binary actually contains.
//
// LVGL fonts are compiled glyph bitmaps, not scalable outlines, so a size not linked into
// the firmware cannot be drawn at any quality: there are no glyphs to draw. Every screen
// that offers a size control therefore offers exactly this ladder and nothing between the
// rungs, and Studio's slider walks the same list.
//
// Shared rather than copied. intel_view.cpp grew the original; the splash is the second
// screen to want it, which by this repo's own rule (see curved_text.cpp, extracted on the
// third copy) is one copy too early to start duplicating.
//
// Unknown sizes fall back to 16 rather than to the nearest rung. Nearest sounds friendlier
// and quietly redesigns the theme: a design asking for 30 would silently become 28 or 32
// and the person who wrote 30 would never be told.
inline const lv_font_t *font_ladder(int size) {
    switch (size) {
        case 12: return &font_de_12;
        case 14: return &font_de_14;
        case 16: return &font_de_16;
        case 18: return &font_de_18;
        case 20: return &font_de_20;
        case 22: return &font_de_22;
        case 24: return &font_de_24;
        case 26: return &font_de_26;
        case 28: return &font_de_28;
        case 32: return &font_de_32;
        case 36: return &font_de_36;
        case 40: return &font_de_40;
        case 44: return &font_de_44;
        case 48: return &font_de_48;
        default: return &font_de_16;
    }
}
