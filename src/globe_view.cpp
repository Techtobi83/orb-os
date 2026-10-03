#include "globe_view.h"
#include "config.h"
#include "lang.h"

#if GLOBE_ENABLED

#include <Arduino.h>
#include <TJpg_Decoder.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <time.h>
#include "theme_sd.h"
#include "diag_log.h"

// How it is drawn, and why it is fast enough.
//
// The globe is an orthographic projection seen from above the home latitude. Spinning the
// Earth about its axis does not change which latitude a screen pixel sees, only which
// longitude, and on an equirectangular picture a change of longitude is a change of column.
// So the whole projection is worked out once, on entry: for every pixel on the disc, the
// picture row it shows, its picture column at rotation zero and how far it is from the limb.
// A frame is then a table walk, one addition for the column, and the lighting from three
// small tables. No trigonometry runs per pixel at all.
//
// Memory, all PSRAM and all given back on exit: day picture 590 KB, night lights 295 KB,
// the projection table 4 bytes a pixel (~480 KB), and the 434 KB canvas.

namespace {

constexpr int TEX_W = 768, TEX_H = 384;           // the pictures' size (gen_earth_textures.py)
constexpr int SCR = SCREEN_W;
constexpr int R = GLOBE_RADIUS_PX;               // globe radius on the 466 px dial
constexpr int GLOW_PX = 9;                        // the atmosphere's width outside the limb
constexpr float VIEW_LAT_BELOW_HOME = 3.0f;       // look from a little south of home: more of the south shows
constexpr int DEG_PER_DETENT = 8;
constexpr uint32_t HOME_AFTER_MS = 5000;          // stillness before it glides back home
constexpr uint32_t TICK_MS = 40;

lv_obj_t   *s_scr = nullptr, *s_canvas = nullptr, *s_msg = nullptr;
lv_timer_t *s_timer = nullptr;
lv_color_t *s_pix = nullptr;                     // canvas, SCR x SCR
// The big tables are held in bands of rows, never in one block. After a few hours of uptime
// PSRAM had 2.9 MB free but no single 620 KB piece left, and the one-block version said
// "not enough memory" (owner, 2026-10-03). Bands of ~40 KB fit in whatever is there.
constexpr int BAND = 32;                          // rows per allocation
constexpr int NBAND_TEX = (TEX_H + BAND - 1) / BAND, NBAND_SCR = (SCR + BAND - 1) / BAND;
uint16_t   *s_dayBand[NBAND_TEX] = {};            // TEX_W x TEX_H RGB565
uint8_t    *s_nightBand[NBAND_TEX] = {};          // TEX_W x TEX_H lights, 0..255
uint32_t   *s_lutBand[NBAND_SCR] = {};            // per disc pixel: u0 (10) | v (9) << 10 | z (8) << 19
uint32_t   *s_lutRow[SCR] = {};                   // each row's first entry, inside its band
bool        s_lutReady = false;
inline uint16_t *dayRow(int v)   { return s_dayBand[v / BAND] + (v % BAND) * TEX_W; }
inline uint8_t  *nightRow(int v) { return s_nightBand[v / BAND] + (v % BAND) * TEX_W; }
int16_t     s_x0[SCR], s_x1[SCR];                // disc span per row; x0 > x1 = none
int16_t     s_sinLat[TEX_H], s_cosLat[TEX_H];    // Q14, by picture row
int16_t     s_cosLon[TEX_W];                     // Q14, by column difference
uint8_t     s_twi[513];                          // cos(zenith) >> 6 (+256) -> daylight 0..255

float    s_offsetDeg = 0;                         // the knob's spin, away from home
uint32_t s_lastTurnMs = 0;
int      s_drawnRot = INT32_MIN, s_drawnSun = INT32_MIN;
bool     s_active = false;

// ---- loading ---------------------------------------------------------------------------------
enum Target { T_DAY, T_NIGHT };
Target s_target = T_DAY;

bool jpg_out(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bmp) {
    for (int j = 0; j < h; ++j) {
        const int yy = y + j;
        if (yy < 0 || yy >= TEX_H) continue;
        for (int i = 0; i < w; ++i) {
            const int xx = x + i;
            if (xx < 0 || xx >= TEX_W) continue;
            const uint16_t p = bmp[j * w + i];
            if (s_target == T_DAY) dayRow(yy)[xx] = p;
            else {   // the night picture is grey: its green channel is its brightness
                nightRow(yy)[xx] = (uint8_t)(((p >> 5) & 0x3F) * 255 / 63);
            }
        }
    }
    return true;
}

bool load_jpg(const char *path, Target t) {
    size_t len = 0;
    uint8_t *buf = theme_sd::read_whole(path, len, 1024 * 1024);
    if (!buf) { Serial.printf("[globe] cannot read %s\n", path); return false; }
    uint16_t w = 0, h = 0;
    bool ok = TJpgDec.getJpgSize(&w, &h, buf, len) == JDR_OK && w == TEX_W && h == TEX_H;
    if (ok) {
        s_target = t;
        TJpgDec.setJpgScale(1);
        TJpgDec.setSwapBytes(false);
        TJpgDec.setCallback(jpg_out);
        ok = TJpgDec.drawJpg(0, 0, buf, len) == JDR_OK;
    } else {
        Serial.printf("[globe] %s is %ux%u, expected %dx%d baseline JPEG\n", path, w, h, TEX_W, TEX_H);
    }
    theme_sd::free(buf);
    return ok;
}

// ---- the projection, once per entry ---------------------------------------------------------
bool build(double homeLat, double homeLon) {
    for (int v = 0; v < TEX_H; ++v) {
        const double lat = (90.0 - (v + 0.5) * 180.0 / TEX_H) * M_PI / 180.0;
        s_sinLat[v] = (int16_t)lround(sin(lat) * 16384);
        s_cosLat[v] = (int16_t)lround(cos(lat) * 16384);
    }
    for (int d = 0; d < TEX_W; ++d) s_cosLon[d] = (int16_t)lround(cos(d * 2 * M_PI / TEX_W) * 16384);
    for (int i = 0; i < 513; ++i) {
        // A soft twilight band, +-6 degrees of solar elevation either side of the terminator.
        const float c = (i - 256) / 256.0f;
        float t = (c + 0.10f) / 0.20f;
        t = t < 0 ? 0 : (t > 1 ? 1 : t);
        s_twi[i] = (uint8_t)lroundf(t * 255);
    }

    int n = 0;
    for (int y = 0; y < SCR; ++y) {
        const float dy = (y + 0.5f) - SCR / 2.0f;
        if (fabsf(dy) >= R) { s_x0[y] = 1; s_x1[y] = 0; continue; }
        const float half = sqrtf((float)R * R - dy * dy);
        s_x0[y] = (int16_t)ceilf(SCR / 2.0f - half);
        s_x1[y] = (int16_t)floorf(SCR / 2.0f + half - 1);
        if (s_x1[y] >= s_x0[y]) n += s_x1[y] - s_x0[y] + 1;
    }
    for (int b = 0; b < NBAND_SCR; ++b) {
        int cnt = 0;
        for (int y = b * BAND; y < SCR && y < (b + 1) * BAND; ++y)
            if (s_x1[y] >= s_x0[y]) cnt += s_x1[y] - s_x0[y] + 1;
        if (!cnt) continue;
        s_lutBand[b] = (uint32_t *)heap_caps_malloc((size_t)cnt * 4, MALLOC_CAP_SPIRAM);
        if (!s_lutBand[b]) return false;
        uint32_t *q = s_lutBand[b];
        for (int y = b * BAND; y < SCR && y < (b + 1) * BAND; ++y) {
            s_lutRow[y] = q;
            if (s_x1[y] >= s_x0[y]) q += s_x1[y] - s_x0[y] + 1;
        }
    }
    (void)n;

    // Single precision throughout: the S3's FPU does float in hardware and double in
    // software, which made the first version of this loop take eight seconds.
    const float phi0 = (float)(homeLat - VIEW_LAT_BELOW_HOME) * (float)M_PI / 180.0f;
    const float sp = sinf(phi0), cp = cosf(phi0);
    const float lon0 = (float)homeLon + 180.0f;              // degrees, 0..360 at the centre
    const float toCol = TEX_W / 360.0f, toRow = TEX_H / 180.0f, deg = 180.0f / (float)M_PI;
    for (int y = 0; y < SCR; ++y) {
        const float py = -((y + 0.5f) - SCR / 2.0f) / R;
        uint32_t *row = s_lutRow[y];
        int k = 0;
        for (int x = s_x0[y]; x <= s_x1[y]; ++x) {
            const float px = ((x + 0.5f) - SCR / 2.0f) / R;
            const float z = sqrtf(fmaxf(0.0f, 1 - px * px - py * py));
            const float lat = asinf(fmaxf(-1.0f, fminf(1.0f, z * sp + py * cp)));
            float lonDeg = lon0 + atan2f(px, z * cp - py * sp) * deg;
            while (lonDeg >= 360.0f) lonDeg -= 360.0f;
            while (lonDeg < 0.0f) lonDeg += 360.0f;
            int u = (int)(lonDeg * toCol); if (u >= TEX_W) u -= TEX_W;
            int v = (int)((90.0f - lat * deg) * toRow); v = v < 0 ? 0 : (v >= TEX_H ? TEX_H - 1 : v);
            const int zq = (int)(z * 255.0f + 0.5f);
            row[k++] = (uint32_t)u | ((uint32_t)v << 10) | ((uint32_t)zq << 19);
        }
    }
    s_lutReady = true;
    return true;
}

// The atmosphere: a thin blue glow just outside the limb, drawn once; frames never touch it.
void draw_glow() {
    for (int y = 0; y < SCR; ++y) {
        for (int x = 0; x < SCR; ++x) {
            const float dx = (x + 0.5f) - SCR / 2.0f, dy = (y + 0.5f) - SCR / 2.0f;
            const float d = sqrtf(dx * dx + dy * dy) - R;
            uint16_t c = 0;
            if (d >= -1 && d < GLOW_PX) {
                const float g = 1.0f - (d + 1) / (GLOW_PX + 1);
                const float a = g * g * 0.55f;
                const int r = (int)(0.25f * 255 * a), gg = (int)(0.55f * 255 * a), b = (int)(1.0f * 255 * a);
                c = (uint16_t)(((r >> 3) << 11) | ((gg >> 2) << 5) | (b >> 3));
            }
            s_pix[y * SCR + x].full = c;
        }
    }
}

// ---- a frame -------------------------------------------------------------------------------
void sun_now(int &sunCol, int16_t &sinS, int16_t &cosS) {
    const time_t now = time(nullptr);
    struct tm g;
    gmtime_r(&now, &g);
    const double decl = -23.44 * cos(2 * M_PI / 365.0 * (g.tm_yday + 1 + 10));
    const double h = g.tm_hour + g.tm_min / 60.0;
    double slon = -(h - 12.0) * 15.0;                         // the subsolar longitude
    sinS = (int16_t)lround(sin(decl * M_PI / 180) * 16384);
    cosS = (int16_t)lround(cos(decl * M_PI / 180) * 16384);
    double c = fmod(slon + 180.0, 360.0); if (c < 0) c += 360;
    sunCol = (int)(c / 360.0 * TEX_W) % TEX_W;
}

void render(int rot, int sunCol, int16_t sinS, int16_t cosS) {
    for (int y = 0; y < SCR; ++y) {
        if (s_x1[y] < s_x0[y]) continue;
        lv_color_t *out = s_pix + y * SCR;
        const uint32_t *row = s_lutRow[y];
        int k = 0;
        for (int x = s_x0[y]; x <= s_x1[y]; ++x) {
            const uint32_t e = row[k++];
            int u = (int)(e & 0x3FF) + rot; if (u >= TEX_W) u -= TEX_W;
            const int v = (int)((e >> 10) & 0x1FF), z = (int)(e >> 19);
            int du = u - sunCol; if (du < 0) du += TEX_W;
            const int cosz = ((int)s_sinLat[v] * sinS + (((int)s_cosLat[v] * ((cosS * (int)s_cosLon[du]) >> 14)))) >> 14;
            const int t = s_twi[(cosz >> 6) + 256];                       // daylight 0..255
            const uint16_t p = dayRow(v)[u];
            const int r8 = (p >> 11) << 3, g8 = ((p >> 5) & 0x3F) << 2, b8 = (p & 0x1F) << 3;
            const int dg = (t * (90 + ((166 * t) >> 8))) >> 8;           // day: dimmer toward dusk
            const int nt = 255 - t, L = nightRow(v)[u];
            int r = ((r8 * dg) >> 8) + ((((r8 * 13) >> 8) + L) * nt >> 8);
            int g = ((g8 * dg) >> 8) + ((((g8 * 13) >> 8) + ((L * 209) >> 8)) * nt >> 8);
            int b = ((b8 * dg) >> 8) + ((((b8 * 13) >> 8) + ((L * 140) >> 8)) * nt >> 8);
            const int lz = 140 + ((115 * z) >> 8);                       // limb darkening
            r = (r * lz) >> 8; g = (g * lz) >> 8; b = (b * lz) >> 8;
            if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
            out[x].full = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
    lv_obj_invalidate(s_canvas);
}

void tick_cb(lv_timer_t *) {
    if (!s_active || !s_lutReady) return;
    // Glide home: after a few seconds of stillness the spin decays to zero, the short way.
    if (s_offsetDeg != 0 && millis() - s_lastTurnMs > HOME_AFTER_MS) {
        s_offsetDeg *= 0.90f;
        if (fabsf(s_offsetDeg) < 0.3f) s_offsetDeg = 0;
    }
    int rot = (int)lroundf(s_offsetDeg / 360.0f * TEX_W) % TEX_W; if (rot < 0) rot += TEX_W;
    int sunCol; int16_t sinS, cosS;
    sun_now(sunCol, sinS, cosS);
    if (rot == s_drawnRot && sunCol == s_drawnSun) return;
    s_drawnRot = rot; s_drawnSun = sunCol;
    const uint32_t t0 = micros();
    render(rot, sunCol, sinS, cosS);
    static uint32_t s_logged = 0;
    if (millis() - s_logged > 3000) { s_logged = millis(); Serial.printf("[globe] frame %lu us\n", (unsigned long)(micros() - t0)); }
}

void release_all() {
    if (s_canvas) { lv_obj_del(s_canvas); s_canvas = nullptr; }
    if (s_pix)   { heap_caps_free(s_pix);   s_pix = nullptr; }
    for (auto &b : s_dayBand)   if (b) { heap_caps_free(b); b = nullptr; }
    for (auto &b : s_nightBand) if (b) { heap_caps_free(b); b = nullptr; }
    for (auto &b : s_lutBand)   if (b) { heap_caps_free(b); b = nullptr; }
    s_lutReady = false;
}

void show_msg(const char *t) {
    lv_label_set_text(s_msg, t);
    lv_obj_clear_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
}

}  // namespace

namespace globeview {

void init() {
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    s_msg = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_msg, &font_de_16, 0);
    lv_obj_set_style_text_color(s_msg, lv_color_hex(0xB0B6BE), 0);
    lv_obj_set_style_text_align(s_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_msg);
    lv_obj_add_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
    s_timer = lv_timer_create(tick_cb, TICK_MS, nullptr);
}

lv_obj_t *screen() { return s_scr; }

void onEnter(double homeLat, double homeLon) {
    const uint32_t t0 = millis();
    lv_obj_add_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
    s_offsetDeg = 0; s_drawnRot = s_drawnSun = INT32_MIN;
    s_pix   = (lv_color_t *)heap_caps_malloc((size_t)SCR * SCR * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool ok = s_pix != nullptr;
    for (int b = 0; ok && b < NBAND_TEX; ++b) {
        s_dayBand[b]   = (uint16_t *)heap_caps_malloc((size_t)TEX_W * BAND * 2, MALLOC_CAP_SPIRAM);
        s_nightBand[b] = (uint8_t *)heap_caps_malloc((size_t)TEX_W * BAND, MALLOC_CAP_SPIRAM);
        ok = s_dayBand[b] && s_nightBand[b];
    }
    if (!ok) {
        Serial.printf("[globe] no PSRAM: free %u, largest block %u\n",
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        release_all();
        diag::log("globe: no PSRAM");
        show_msg(tr("Not enough memory", "Nicht genug Speicher"));
        return;
    }
    if (!load_jpg("/earth/day.jpg", T_DAY) || !load_jpg("/earth/night.jpg", T_NIGHT)) {
        release_all();
        show_msg(tr("Earth pictures missing\n/earth/day.jpg, night.jpg\non the SD card",
                    "Erdbilder fehlen\n/earth/day.jpg, night.jpg\nauf der SD-Karte"));
        return;
    }
    if (!build(homeLat, homeLon)) {
        Serial.printf("[globe] no PSRAM for the projection: free %u, largest block %u\n",
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        release_all();
        show_msg(tr("Not enough memory", "Nicht genug Speicher"));
        return;
    }
    s_canvas = lv_canvas_create(s_scr);
    lv_canvas_set_buffer(s_canvas, s_pix, SCR, SCR, LV_IMG_CF_TRUE_COLOR);
    lv_obj_center(s_canvas);
    lv_obj_move_background(s_canvas);
    draw_glow();
    s_active = true;
    tick_cb(nullptr);
    Serial.printf("[globe] ready in %lu ms, home %.2f %.2f\n", (unsigned long)(millis() - t0), homeLat, homeLon);
}

void onExit() {
    s_active = false;
    release_all();
}

void onTurn(int delta) {
    if (!s_active) return;
    s_offsetDeg += delta * DEG_PER_DETENT;
    while (s_offsetDeg > 180) s_offsetDeg -= 360;     // so the way home is always the short one
    while (s_offsetDeg <= -180) s_offsetDeg += 360;
    s_lastTurnMs = millis();
}

}  // namespace globeview

#else
namespace globeview {
void init() {}
lv_obj_t *screen() { return nullptr; }
void onEnter(double, double) {}
void onExit() {}
void onTurn(int) {}
}
#endif
