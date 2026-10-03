#include "forecast_view.h"
#include "lang.h"
#include "day_ring.h"
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
// The ring and its sun/moon marker are day_ring's, shared with the clock.
using day_ring::SCR; using day_ring::C; using day_ring::RING_R; using day_ring::MARK_HALO;
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
constexpr uint32_t COL_SUN    = 0xF5B342;
constexpr uint32_t COL_MOON   = 0xE9E6DD, COL_CLOUD = 0xDDE3EC, COL_RAIN = 0x5AB4F0;
constexpr uint32_t COL_SNOW   = 0xE9EEF5, COL_FOG   = 0xC9CDD3, COL_CAPSULE = 0x10151C;
constexpr uint32_t STALE_MS   = 2UL * 60 * 60 * 1000; // older than this and the temperature greys

lv_obj_t *s_scr;
day_ring::Ring s_ring;
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
        case 0:  return day ? tr("Sunny", "Sonnig") : tr("Clear", "Klar");
        case 1:  return day ? tr("Mostly sunny", "Heiter") : tr("Mostly clear", "Überwiegend klar");
        case 2:  return tr("Partly cloudy", "Teilweise bewölkt");
        case 3:  return tr("Overcast", "Bedeckt");
        case 45: return tr("Fog", "Nebel");
        case 48: return tr("Freezing fog", "Reifnebel");
        case 51: return tr("Light drizzle", "Leichter Niesel");
        case 53: return tr("Drizzle", "Nieselregen");
        case 55: return tr("Heavy drizzle", "Starker Niesel");
        case 56: case 57: return tr("Freezing drizzle", "Gefrierender Niesel");
        case 61: return tr("Light rain", "Leichter Regen");
        case 63: return tr("Rain", "Regen");
        case 65: return tr("Heavy rain", "Starker Regen");
        case 66: case 67: return tr("Freezing rain", "Gefrierender Regen");
        case 71: return tr("Light snow", "Leichter Schneefall");
        case 73: return tr("Snow", "Schneefall");
        case 75: return tr("Heavy snow", "Starker Schneefall");
        case 77: return tr("Snow grains", "Schneegriesel");
        case 80: return tr("Light showers", "Leichte Schauer");
        case 81: return tr("Showers", "Schauer");
        case 82: return tr("Heavy showers", "Starke Schauer");
        case 85: return tr("Snow showers", "Schneeschauer");
        case 86: return tr("Heavy snow showers", "Starke Schneeschauer");
        case 95: return tr("Thunderstorm", "Gewitter");
        case 96: case 99: return tr("Thunderstorm, hail", "Gewitter mit Hagel");
        default: return tr("Unknown weather", "Wetter unbekannt");
    }
}

// "2026-10-01" -> "DO". Zeller-free: mktime does the calendar.
const char *day_abbrev(const char *iso) {
    static const char *DE[] = { "SO", "MO", "DI", "MI", "DO", "FR", "SA" };
    static const char *EN[] = { "SU", "MO", "TU", "WE", "TH", "FR", "SA" };
    int y, m, d;
    if (!iso || sscanf(iso, "%d-%d-%d", &y, &m, &d) != 3) return "";
    struct tm t = {};
    t.tm_year = y - 1900; t.tm_mon = m - 1; t.tm_mday = d; t.tm_hour = 12;
    mktime(&t);
    return (lang::de() ? DE : EN)[t.tm_wday % 7];
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

// ---- drawing -------------------------------------------------------------------------------
void draw_symbol(const Look &k, bool day) {
    // Large, faint, behind everything. The fade is baked into the images' alpha.
    const int fx = (SCR - WXI_FRAME_L) / 2, fy = SYMBOL_Y;
    const lv_img_dsc_t *sky  = day ? &wxi_sun_l : &wxi_moon_l;
    const int skyX = day ? WXI_SUN_L_X : WXI_MOON_L_X, skyY = day ? WXI_SUN_L_Y : WXI_MOON_L_Y;
    const uint32_t skyCol = day ? COL_SUN : COL_MOON;
    if (k.layer == FOG) {
        // Fog is its own picture on the large symbol: banks of mist, no cloud. Two thin lines
        // under a cloud were what this showed before, and they sat behind tomorrow's capsule.
        show_img(s_bgSun, nullptr, 0, 0, 0);
        show_img(s_bgBase, &wxi_fogbank_l, fx + WXI_FOGBANK_L_X, fy + WXI_FOGBANK_L_Y, COL_FOG);
        show_img(s_bgOver, nullptr, 0, 0, 0);
        return;
    }
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

    const bool day = day_ring::update(s_ring);   // the ring, its times and the sun/moon

    if (!have) {
        // Honest about which thing is missing: the network, or the answer.
        draw_symbol(look_for(3), day);
        lv_label_set_text(s_temp, "--°");
        lv_obj_set_style_text_color(s_temp, lv_color_hex(COL_DIM), 0);
        lv_label_set_text(s_cond, host_wifi_connected() ? tr("Loading weather data", "Wetterdaten werden geladen") : tr("No WiFi", "Kein WLAN"));
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
    snprintf(b, sizeof(b), tr("%s · feels %ld°", "%s · gefühlt %ld°"), cond, lroundf(w.feelsC));
    lv_point_t sz;
    lv_txt_get_size(&sz, b, &font_sora_21, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (sz.x <= COND_MAX_W)
        snprintf(b, sizeof(b), tr("%s #8F8B83 ·# feels %ld°", "%s #8F8B83 ·# gefühlt %ld°"), cond, lroundf(w.feelsC));
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

    day_ring::create_track(s_ring, s_scr);

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

    day_ring::create_marker(s_ring, s_scr);   // over the ring and the text

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
