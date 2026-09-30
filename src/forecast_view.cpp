#include "forecast_view.h"
#include "weather.h"
#include "wx_icons.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

LV_FONT_DECLARE(font_sora_120);
LV_FONT_DECLARE(font_sora_21);
LV_FONT_DECLARE(font_sora_20);
LV_FONT_DECLARE(font_sora_19b);
LV_FONT_DECLARE(font_sora_15);

extern bool host_location_name(char *out, size_t n);   // main.cpp / sim_main.cpp
extern bool host_wifi_connected();                     // main.cpp / sim_main.cpp

namespace forecastview {
namespace {

// ---- the look (the preview "Tagesring (jetzt)", 2026-09-30) --------------------------------
constexpr int  SCR = 466, C = SCR / 2;
constexpr int  RING_R = 214, RING_W = 10;            // centre line and width of the day ring
constexpr int  LABEL_R = 183;                        // sunrise/sunset times, inside the ring
constexpr int  MARK_HALO = 68, MARK_DISC = 54;       // time marker: faint halo, black disc under the symbol
constexpr int  COND_MAX_W = 372;                     // widest condition line inside the ring at its height
// Tomorrow's capsule. It must stay clear of the time marker's halo wherever the marker is on
// the ring: its far corner lies within RING_R - MARK_HALO / 2 = 180 px of the centre. At 270
// wide and 64 high that caps CAP_Y at 307; the text above is stacked up to make room.
constexpr int  CAP_W = 270, CAP_H = 64, CAP_Y = 302;
constexpr int  LOC_Y = 64, TEMP_Y = 118, COND_Y = 258; // tops of the location, temperature, condition
constexpr int  SYMBOL_Y = -4;                          // the large symbol's frame, centred on the temperature
// The clearance above, checked rather than trusted: the centre of the capsule's rounded end,
// plus its radius, must stay inside the circle the marker's halo never enters.
constexpr int CAP_END_DX = CAP_W / 2 - CAP_H / 2, CAP_END_DY = CAP_Y + CAP_H / 2 - C;
constexpr int CAP_CLEAR  = RING_R - MARK_HALO / 2 - CAP_H / 2;
static_assert(CAP_END_DX * CAP_END_DX + CAP_END_DY * CAP_END_DY <= CAP_CLEAR * CAP_CLEAR,
              "tomorrow's capsule reaches the ring: the sun/moon marker would cover it");
constexpr uint32_t COL_TEXT   = 0xF2F0EA, COL_TEXT2 = 0xE4E1D9, COL_DIM = 0x8F8B83;
constexpr uint32_t COL_RING   = 0x161B24, COL_SUN = 0xF5B342;
constexpr uint32_t COL_MOON   = 0xE9E6DD, COL_CLOUD = 0xDDE3EC, COL_RAIN = 0x5AB4F0;
constexpr uint32_t COL_SNOW   = 0xE9EEF5, COL_FOG   = 0xC9CDD3, COL_CAPSULE = 0x10151C;
constexpr uint32_t STALE_MS   = 2UL * 60 * 60 * 1000; // older than this and the temperature greys

lv_obj_t *s_scr, *s_ring, *s_rise, *s_set, *s_halo, *s_disc, *s_mark;
lv_obj_t *s_bgBase, *s_bgSun, *s_bgOver;               // large symbol: base, sun behind a cloud, layer
lv_obj_t *s_loc, *s_temp, *s_cond;
lv_obj_t *s_cap, *s_capDay, *s_capBase, *s_capOver, *s_capTemp, *s_capRain;
lv_timer_t *s_tick;
uint32_t s_gotAt = 0;                                  // lv_tick of the last snapshot, 0 = none

// ---- WMO weather codes -----------------------------------------------------------------------
enum Layer { NONE, DROPS, SNOW, BOLT, FOG };
struct Look { bool cloud; bool sky; Layer layer; };    // sky = sun (day) or moon (night) shows

Look look_for(int code) {
    if (code == 0)                         return { false, true,  NONE  };
    if (code == 1 || code == 2)            return { true,  true,  NONE  };
    if (code == 45 || code == 48)          return { true,  false, FOG   };
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return { true, false, DROPS };
    if ((code >= 71 && code <= 77) || code == 85 || code == 86)   return { true, false, SNOW  };
    if (code >= 95)                        return { true,  false, BOLT  };
    return { true, false, NONE };          // 3 and anything unknown: overcast
}

const char *condition_de(int code, bool day) {
    switch (code) {
        case 0:  return day ? "Sonnig" : "Klar";
        case 1:  return day ? "Heiter" : "Überwiegend klar";
        case 2:  return "Teilweise bewölkt";
        case 3:  return "Bedeckt";
        case 45: return "Nebel";
        case 48: return "Reifnebel";
        case 51: return "Leichter Niesel";
        case 53: return "Nieselregen";
        case 55: return "Starker Niesel";
        case 56: case 57: return "Gefrierender Niesel";
        case 61: return "Leichter Regen";
        case 63: return "Regen";
        case 65: return "Starker Regen";
        case 66: case 67: return "Gefrierender Regen";
        case 71: return "Leichter Schneefall";
        case 73: return "Schneefall";
        case 75: return "Starker Schneefall";
        case 77: return "Schneegriesel";
        case 80: return "Leichte Schauer";
        case 81: return "Schauer";
        case 82: return "Starke Schauer";
        case 85: return "Schneeschauer";
        case 86: return "Starke Schneeschauer";
        case 95: return "Gewitter";
        case 96: case 99: return "Gewitter mit Hagel";
        default: return "Wetter unbekannt";
    }
}

// "2026-10-01" -> "DO". Zeller-free: mktime does the calendar.
const char *day_abbrev(const char *iso) {
    static const char *DE[] = { "SO", "MO", "DI", "MI", "DO", "FR", "SA" };
    int y, m, d;
    if (!iso || sscanf(iso, "%d-%d-%d", &y, &m, &d) != 3) return "";
    struct tm t = {};
    t.tm_year = y - 1900; t.tm_mon = m - 1; t.tm_mday = d; t.tm_hour = 12;
    mktime(&t);
    return DE[t.tm_wday % 7];
}

// ---- geometry ------------------------------------------------------------------------------
// The ring is a 24-hour clock with noon at the top: minutes after midnight -> degrees
// clockwise from 12 o'clock.
float ring_deg(int minutes) { return (minutes / 60.0f - 12.0f) * 15.0f; }
void  ring_point(int minutes, int r, int &x, int &y) {
    const float a = ring_deg(minutes) * (float)M_PI / 180.0f;
    x = C + (int)lroundf(r * sinf(a));
    y = C - (int)lroundf(r * cosf(a));
}
// LVGL arcs count from 3 o'clock.
int lv_deg(int minutes) { int a = (int)lroundf(ring_deg(minutes)) - 90; return ((a % 360) + 360) % 360; }

bool local_now(int &minutes) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_year < 120) return false;                  // clock not set yet
    minutes = t.tm_hour * 60 + t.tm_min;
    return true;
}

// ---- building blocks -----------------------------------------------------------------------
lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    return l;
}

lv_obj_t *alpha_img(lv_obj_t *parent) {
    lv_obj_t *i = lv_img_create(parent);
    lv_obj_set_style_img_recolor_opa(i, LV_OPA_COVER, 0);
    lv_obj_add_flag(i, LV_OBJ_FLAG_HIDDEN);
    return i;
}

// Show `dsc` at its offset inside the symbol's frame, in `color`; nullptr hides it.
void show_img(lv_obj_t *img, const lv_img_dsc_t *dsc, int x, int y, uint32_t color) {
    if (!dsc) { lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN); return; }
    lv_img_set_src(img, dsc);
    lv_obj_set_pos(img, x, y);
    lv_obj_set_style_img_recolor(img, lv_color_hex(color), 0);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *circle(lv_obj_t *parent, int d, uint32_t color, lv_opa_t opa) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, d, d);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

void place_center(lv_obj_t *o, int x, int y) { lv_obj_align(o, LV_ALIGN_CENTER, x - C, y - C); }

// ---- drawing -------------------------------------------------------------------------------
void draw_symbol(const Look &k, bool day) {
    // Large, faint, behind everything. The fade is baked into the images' alpha.
    const int fx = (SCR - WXI_FRAME_L) / 2, fy = SYMBOL_Y;
    const lv_img_dsc_t *sky  = day ? &wxi_sun_l : &wxi_moon_l;
    const int skyX = day ? WXI_SUN_L_X : WXI_MOON_L_X, skyY = day ? WXI_SUN_L_Y : WXI_MOON_L_Y;
    const uint32_t skyCol = day ? COL_SUN : COL_MOON;
    if (k.cloud) {
        show_img(s_bgSun, k.sky ? sky : nullptr, fx + skyX, fy + skyY, skyCol);
        show_img(s_bgBase, &wxi_cloud_l, fx + WXI_CLOUD_L_X, fy + WXI_CLOUD_L_Y, COL_CLOUD);
    } else {
        show_img(s_bgSun, nullptr, 0, 0, 0);
        show_img(s_bgBase, sky, fx + skyX, fy + skyY, skyCol);
    }
    switch (k.layer) {
        case DROPS: show_img(s_bgOver, &wxi_drops_l, fx + WXI_DROPS_L_X, fy + WXI_DROPS_L_Y, COL_RAIN); break;
        case SNOW:  show_img(s_bgOver, &wxi_snow_l,  fx + WXI_SNOW_L_X,  fy + WXI_SNOW_L_Y,  COL_SNOW); break;
        case BOLT:  show_img(s_bgOver, &wxi_bolt_l,  fx + WXI_BOLT_L_X,  fy + WXI_BOLT_L_Y,  COL_SUN);  break;
        case FOG:   show_img(s_bgOver, &wxi_fog_l,   fx + WXI_FOG_L_X,   fy + WXI_FOG_L_Y,   COL_FOG);  break;
        default:    show_img(s_bgOver, nullptr, 0, 0, 0); break;
    }
}

void draw_capsule_icon(int code) {
    const Look k = look_for(code);             // tomorrow: always the daytime symbol
    if (k.cloud) show_img(s_capBase, &wxi_cloud_s, WXI_CLOUD_S_X, WXI_CLOUD_S_Y, 0xC9CDD3);
    else         show_img(s_capBase, &wxi_sun_s, WXI_SUN_S_X, WXI_SUN_S_Y, COL_SUN);
    switch (k.layer) {
        case DROPS: show_img(s_capOver, &wxi_drops_s, WXI_DROPS_S_X, WXI_DROPS_S_Y, COL_RAIN); break;
        case SNOW:  show_img(s_capOver, &wxi_snow_s,  WXI_SNOW_S_X,  WXI_SNOW_S_Y,  COL_SNOW); break;
        case BOLT:  show_img(s_capOver, &wxi_bolt_s,  WXI_BOLT_S_X,  WXI_BOLT_S_Y,  COL_SUN);  break;
        case FOG:   show_img(s_capOver, &wxi_fog_s,   WXI_FOG_S_X,   WXI_FOG_S_Y,   COL_FOG);  break;
        default:    show_img(s_capOver, nullptr, 0, 0, 0); break;
    }
}

void refresh() {
    WeatherSnapshot w;
    const bool have = weather_get(w) && w.valid;
    int now = 0;
    const bool clock = local_now(now);

    // Location: whatever Settings > Location named (the town only; host_location_name cuts
    // at the first comma for every screen). Nothing named, nothing drawn.
    char loc[64];
    if (host_location_name(loc, sizeof(loc))) {
        for (char *p = loc; *p; ++p) if (*p >= 'a' && *p <= 'z') *p -= 32;   // ASCII upper; umlauts stay
        lv_label_set_text(s_loc, loc);
        lv_obj_clear_flag(s_loc, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_loc, LV_OBJ_FLAG_HIDDEN);
    }

    // Daylight on the ring, from today's sunrise and sunset.
    const WeatherDay *today = (have && w.dayCount > 0) ? &w.days[0] : nullptr;
    const bool sun = today && today->sunriseMin >= 0 && today->sunsetMin > today->sunriseMin;
    if (sun) {
        lv_arc_set_angles(s_ring, lv_deg(today->sunriseMin), lv_deg(today->sunsetMin));
        lv_obj_set_style_arc_opa(s_ring, LV_OPA_COVER, LV_PART_INDICATOR);
        char b[8];
        int x, y;
        snprintf(b, sizeof(b), "%02d:%02d", today->sunriseMin / 60, today->sunriseMin % 60);
        lv_label_set_text(s_rise, b);
        ring_point(today->sunriseMin, LABEL_R, x, y); place_center(s_rise, x, y);
        snprintf(b, sizeof(b), "%02d:%02d", today->sunsetMin / 60, today->sunsetMin % 60);
        lv_label_set_text(s_set, b);
        ring_point(today->sunsetMin, LABEL_R, x, y); place_center(s_set, x, y);
        lv_obj_clear_flag(s_rise, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_set, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_style_arc_opa(s_ring, LV_OPA_TRANSP, LV_PART_INDICATOR);
        lv_obj_add_flag(s_rise, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_set, LV_OBJ_FLAG_HIDDEN);
    }
    // Without sun times, day is 06:00-18:00: a guess, but only the symbols depend on it.
    const bool day = sun ? (now >= today->sunriseMin && now < today->sunsetMin)
                         : (now >= 6 * 60 && now < 18 * 60);

    // The time marker: a sun by day, a moon by night, on the ring at the current time.
    if (clock) {
        int x, y;
        ring_point(now, RING_R, x, y);
        place_center(s_halo, x, y);
        place_center(s_disc, x, y);
        lv_obj_set_style_bg_color(s_halo, lv_color_hex(day ? COL_SUN : COL_MOON), 0);
        lv_obj_set_style_bg_opa(s_halo, day ? 36 : 26, 0);
        show_img(s_mark, day ? &wxi_sun_m : &wxi_moon_m,
                 x - WXI_FRAME_M / 2 + (day ? WXI_SUN_M_X : WXI_MOON_M_X),
                 y - WXI_FRAME_M / 2 + (day ? WXI_SUN_M_Y : WXI_MOON_M_Y),
                 day ? COL_SUN : COL_MOON);
        lv_obj_clear_flag(s_halo, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_disc, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_halo, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_disc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_mark, LV_OBJ_FLAG_HIDDEN);
    }

    if (!have) {
        // Honest about which thing is missing: the network, or the answer.
        draw_symbol(look_for(3), day);
        lv_label_set_text(s_temp, "--°");
        lv_obj_set_style_text_color(s_temp, lv_color_hex(COL_DIM), 0);
        lv_label_set_text(s_cond, host_wifi_connected() ? "Wetterdaten werden geladen" : "Kein WLAN");
        lv_obj_add_flag(s_cap, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    draw_symbol(look_for(w.code), day);
    char b[64];
    snprintf(b, sizeof(b), "%ld°", lroundf(w.tempC));
    lv_label_set_text(s_temp, b);
    const bool stale = s_gotAt && lv_tick_elaps(s_gotAt) > STALE_MS;
    lv_obj_set_style_text_color(s_temp, lv_color_hex(stale ? COL_DIM : COL_TEXT), 0);
    // "Bedeckt · gefühlt 21°", unless that is wider than the ring allows at this height; then
    // the condition alone, rather than a second line running into the capsule. Measured on
    // the plain text: the recolour marks draw nothing.
    const char *cond = condition_de(w.code, day);
    snprintf(b, sizeof(b), "%s · gefühlt %ld°", cond, lroundf(w.feelsC));
    lv_point_t sz;
    lv_txt_get_size(&sz, b, &font_sora_21, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (sz.x <= COND_MAX_W)
        snprintf(b, sizeof(b), "%s #8F8B83 ·# gefühlt %ld°", cond, lroundf(w.feelsC));
    else
        snprintf(b, sizeof(b), "%s", cond);
    lv_label_set_text(s_cond, b);

    if (w.dayCount > 1) {
        const WeatherDay &t = w.days[1];
        lv_label_set_text(s_capDay, day_abbrev(t.date));
        draw_capsule_icon(t.code);
        snprintf(b, sizeof(b), "%ld°#8F8B83 /%ld°#", lroundf(t.tempMaxC), lroundf(t.tempMinC));
        lv_label_set_text(s_capTemp, b);
        snprintf(b, sizeof(b), "%d %%", t.rainChance);
        lv_label_set_text(s_capRain, b);
        lv_obj_clear_flag(s_cap, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_cap, LV_OBJ_FLAG_HIDDEN);
    }
}

void tick_cb(lv_timer_t *) {
    if (lv_scr_act() == s_scr) refresh();   // the marker moves a pixel or two a minute
}

}   // namespace

void init() {
    s_scr = lv_obj_create(nullptr);
    lv_obj_remove_style_all(s_scr);
    lv_obj_set_size(s_scr, SCR, SCR);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    // Back to front: the large symbol, the ring, the text, the capsule, the marker.
    s_bgSun = alpha_img(s_scr);
    s_bgBase = alpha_img(s_scr);
    s_bgOver = alpha_img(s_scr);

    s_ring = lv_arc_create(s_scr);
    lv_obj_set_size(s_ring, 2 * RING_R + RING_W, 2 * RING_R + RING_W);
    lv_obj_center(s_ring);
    lv_obj_remove_style(s_ring, nullptr, LV_PART_KNOB);
    lv_obj_clear_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(s_ring, 0, 0);
    lv_arc_set_bg_angles(s_ring, 0, 360);
    lv_obj_set_style_arc_width(s_ring, RING_W, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(COL_RING), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_ring, false, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ring, RING_W, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(COL_SUN), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_ring, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_ring, LV_OPA_TRANSP, LV_PART_INDICATOR);

    s_rise = label(s_scr, &font_sora_15, COL_SUN);
    s_set  = label(s_scr, &font_sora_15, COL_SUN);

    s_loc = label(s_scr, &font_sora_19b, COL_TEXT);
    lv_obj_set_style_text_letter_space(s_loc, 5, 0);
    lv_obj_align(s_loc, LV_ALIGN_TOP_MID, 0, LOC_Y);

    s_temp = label(s_scr, &font_sora_120, COL_TEXT);
    lv_obj_set_style_text_letter_space(s_temp, -3, 0);
    lv_obj_align(s_temp, LV_ALIGN_TOP_MID, 0, TEMP_Y);   // label top; the 120 px face puts the baseline 117 below

    s_cond = label(s_scr, &font_sora_21, COL_TEXT2);
    lv_label_set_recolor(s_cond, true);
    lv_obj_align(s_cond, LV_ALIGN_TOP_MID, 0, COND_Y);

    s_cap = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_cap);
    lv_obj_set_size(s_cap, CAP_W, CAP_H);
    lv_obj_align(s_cap, LV_ALIGN_TOP_MID, 0, CAP_Y);
    lv_obj_set_style_radius(s_cap, CAP_H / 2, 0);
    lv_obj_set_style_bg_color(s_cap, lv_color_hex(COL_CAPSULE), 0);
    lv_obj_set_style_bg_opa(s_cap, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(s_cap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_cap, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_cap, 10, 0);
    lv_obj_clear_flag(s_cap, LV_OBJ_FLAG_SCROLLABLE);
    s_capDay = label(s_cap, &font_sora_15, COL_DIM);
    lv_obj_set_style_text_letter_space(s_capDay, 2, 0);
    lv_obj_t *icon = lv_obj_create(s_cap);
    lv_obj_remove_style_all(icon);
    lv_obj_set_size(icon, WXI_FRAME_S, WXI_FRAME_S);
    s_capBase = alpha_img(icon);
    s_capOver = alpha_img(icon);
    s_capTemp = label(s_cap, &font_sora_20, COL_TEXT);
    lv_label_set_recolor(s_capTemp, true);
    s_capRain = label(s_cap, &font_sora_20, COL_RAIN);

    // The marker sits over the ring: a faint halo, a black disc that cuts the ring, the symbol.
    s_halo = circle(s_scr, MARK_HALO, COL_MOON, 26);
    s_disc = circle(s_scr, MARK_DISC, 0x000000, LV_OPA_COVER);
    s_mark = alpha_img(s_scr);

    s_tick = lv_timer_create(tick_cb, 30000, nullptr);
    refresh();
}

lv_obj_t *screen() { return s_scr; }
void onEnter() { refresh(); }
void onExit() {}
void onWeather() {
    s_gotAt = lv_tick_get();
    if (s_scr) refresh();
}

}   // namespace forecastview
