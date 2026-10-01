#include "day_ring.h"
#include "weather.h"
#include "wx_icons.h"
#include <math.h>
#include <stdio.h>
#include <time.h>

LV_FONT_DECLARE(font_sora_15);

namespace day_ring {
namespace {

constexpr uint32_t COL_TRACK = 0x161B24, COL_SUN = 0xF5B342, COL_MOON = 0xE9E6DD;

// Minutes after midnight -> degrees clockwise from 12 o'clock (noon at the top).
float ring_deg(int minutes) { return (minutes / 60.0f - 12.0f) * 15.0f; }
void  ring_point(int minutes, int r, int &x, int &y) {
    const float a = ring_deg(minutes) * (float)M_PI / 180.0f;
    x = C + (int)lroundf(r * sinf(a));
    y = C - (int)lroundf(r * cosf(a));
}
// LVGL arcs count from 3 o'clock.
int lv_deg(int minutes) { int a = (int)lroundf(ring_deg(minutes)) - 90; return ((a % 360) + 360) % 360; }

void place_center(lv_obj_t *o, int x, int y) { lv_obj_align(o, LV_ALIGN_CENTER, x - C, y - C); }

lv_obj_t *time_label(lv_obj_t *parent) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, &font_sora_15, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_SUN), 0);
    lv_label_set_text(l, "");
    lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    return l;
}

lv_obj_t *circle(lv_obj_t *parent, int d, uint32_t color, lv_opa_t opa) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, d, d);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}

}   // namespace

bool local_now(int &minutes) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_year < 120) return false;                  // clock not set yet
    minutes = t.tm_hour * 60 + t.tm_min;
    return true;
}

void create_track(Ring &r, lv_obj_t *parent) {
    r.arc = lv_arc_create(parent);
    lv_obj_set_size(r.arc, 2 * RING_R + RING_W, 2 * RING_R + RING_W);
    lv_obj_center(r.arc);
    lv_obj_remove_style(r.arc, nullptr, LV_PART_KNOB);
    lv_obj_clear_flag(r.arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(r.arc, 0, 0);
    lv_obj_set_style_bg_opa(r.arc, LV_OPA_TRANSP, 0);
    lv_arc_set_bg_angles(r.arc, 0, 360);
    lv_obj_set_style_arc_width(r.arc, RING_W, LV_PART_MAIN);
    lv_obj_set_style_arc_color(r.arc, lv_color_hex(COL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(r.arc, false, LV_PART_MAIN);
    lv_obj_set_style_arc_width(r.arc, RING_W, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(r.arc, lv_color_hex(COL_SUN), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(r.arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(r.arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
    r.rise = time_label(parent);
    r.set  = time_label(parent);
}

void create_marker(Ring &r, lv_obj_t *parent) {
    r.halo = circle(parent, MARK_HALO, COL_MOON, 26);
    r.disc = circle(parent, MARK_DISC, 0x000000, LV_OPA_COVER);
    r.mark = lv_img_create(parent);
    lv_obj_set_style_img_recolor_opa(r.mark, LV_OPA_COVER, 0);
    lv_obj_add_flag(r.mark, LV_OBJ_FLAG_HIDDEN);
}

bool update(Ring &r) {
    WeatherSnapshot w;
    const bool have = weather_get(w) && w.valid && w.dayCount > 0;
    const WeatherDay *today = have ? &w.days[0] : nullptr;
    int now = 0;
    const bool clock = local_now(now);

    const bool sun = today && today->sunriseMin >= 0 && today->sunsetMin > today->sunriseMin;
    if (sun) {
        lv_arc_set_angles(r.arc, lv_deg(today->sunriseMin), lv_deg(today->sunsetMin));
        lv_obj_set_style_arc_opa(r.arc, LV_OPA_COVER, LV_PART_INDICATOR);
        char b[8];
        int x, y;
        snprintf(b, sizeof(b), "%02d:%02d", today->sunriseMin / 60, today->sunriseMin % 60);
        lv_label_set_text(r.rise, b);
        ring_point(today->sunriseMin, LABEL_R, x, y); place_center(r.rise, x, y);
        snprintf(b, sizeof(b), "%02d:%02d", today->sunsetMin / 60, today->sunsetMin % 60);
        lv_label_set_text(r.set, b);
        ring_point(today->sunsetMin, LABEL_R, x, y); place_center(r.set, x, y);
        lv_obj_clear_flag(r.rise, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(r.set, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_style_arc_opa(r.arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
        lv_obj_add_flag(r.rise, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(r.set, LV_OBJ_FLAG_HIDDEN);
    }
    // Without sun times, day is 06:00-18:00: a guess, but only the symbols depend on it.
    const bool day = sun ? (now >= today->sunriseMin && now < today->sunsetMin)
                         : (now >= 6 * 60 && now < 18 * 60);

    if (clock && r.mark) {
        int x, y;
        ring_point(now, RING_R, x, y);
        place_center(r.halo, x, y);
        place_center(r.disc, x, y);
        lv_obj_set_style_bg_color(r.halo, lv_color_hex(day ? COL_SUN : COL_MOON), 0);
        lv_obj_set_style_bg_opa(r.halo, day ? 36 : 26, 0);
        lv_img_set_src(r.mark, day ? &wxi_sun_m : &wxi_moon_m);
        lv_obj_set_pos(r.mark, x - WXI_FRAME_M / 2 + (day ? WXI_SUN_M_X : WXI_MOON_M_X),
                               y - WXI_FRAME_M / 2 + (day ? WXI_SUN_M_Y : WXI_MOON_M_Y));
        lv_obj_set_style_img_recolor(r.mark, lv_color_hex(day ? COL_SUN : COL_MOON), 0);
        lv_obj_clear_flag(r.halo, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(r.disc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(r.mark, LV_OBJ_FLAG_HIDDEN);
    } else if (r.mark) {
        lv_obj_add_flag(r.halo, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(r.disc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(r.mark, LV_OBJ_FLAG_HIDDEN);
    }
    r.complete = sun && clock;
    return day;
}

}   // namespace day_ring
