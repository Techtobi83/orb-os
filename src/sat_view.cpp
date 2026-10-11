#include "sat_view.h"
#include "config.h"
#include "lang.h"

#if SAT_ENABLED

#include <Arduino.h>
#include <Preferences.h>
#include <TJpg_Decoder.h>
#include <Sgp4.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <new>
#include <time.h>
#include "theme_sd.h"
#include "diag_log.h"
#include "net_fetch.h"
#include "settings_sprite.h"
#include "jwst_ephem.h"

LV_FONT_DECLARE(font_sat_24b);
LV_FONT_DECLARE(font_sat_18);
LV_FONT_DECLARE(font_sat_16b);
LV_FONT_DECLARE(font_sat_14);
LV_FONT_DECLARE(font_sat_12);

// How the work is split, and why.
//
// SGP4 is double-precision arithmetic, which the S3 does in software, and its setup keeps a
// few KB of locals on the stack. The LVGL loop has about 2.4 KB of stack to spare, so none
// of it runs there. A worker task does all of it: the satellite's position once a second,
// the 130-minute ground track, the next pass over home, and the globe's projection table.
// Its stack is in PSRAM, which is allowed because it never touches a socket (lwIP refuses
// socket calls from a PSRAM stack; that is why the Livestream's task could not do this) and
// never writes flash. It costs internal RAM only for its control block.
//
// The LVGL side only draws. Once a second, or when the knob moves, it copies what the worker
// published and paints a frame: the globe from the projection table (an integer walk, the
// same technique as the Earth app, about 10 ms), then the lines and the reticle on top.
//
// The projection table depends on the latitude the globe is seen from, which here is the
// satellite's and keeps changing. The worker rebuilds it into a second buffer whenever the
// satellite has moved a degree in latitude, and the LVGL side switches over on its next frame.
//
// Memory while open, all PSRAM and all given back on exit: the pictures 885 KB, the canvas
// 434 KB, two projection tables 2 x 136 KB, the saved background behind the globe 115 KB.

namespace {

// ---- what is shown -------------------------------------------------------------------------
struct SatDef { const char *name, *en, *de; uint32_t catnr; };
const SatDef SATS[] = {
    { "ISS",        "INTERNATIONAL SPACE STATION", "INTERNATIONALE RAUMSTATION", 25544 },
    { "TIANGONG",   "CHINESE SPACE STATION",       "CHINESISCHE RAUMSTATION",    48274 },
    { "HUBBLE",     "SPACE TELESCOPE",             "WELTRAUMTELESKOP",           20580 },
    { "JAMES WEBB", "SPACE TELESCOPE AT L2",       "WELTRAUMTELESKOP BEI L2",        0 },
};
constexpr int NSAT = 4, NTLE = 3, JWST = 3;

// ---- geometry on the 466 px dial -------------------------------------------------------------
constexpr int SCR = SCREEN_W;
// Every text line was measured against the Tagesring dial with the real Sora glyphs: the
// minute ring starts at radius 214, and nothing written here comes within 10 px of it in
// either language (the widest, "Mo 06:33 · 14° · SW → NO · im Schatten", by 11 px). The top
// of a round screen is narrow, which is why the name is 24 px and sits this low.
constexpr int GCX = 233, GCY = 226, GR = 96;      // the globe
constexpr int HALO = 12;                          // the amber atmosphere outside the limb
constexpr int BOX = 2 * (GR + HALO + 4), BOX_X0 = GCX - BOX / 2, BOX_Y0 = GCY - BOX / 2;
constexpr int TEX_W = 768, TEX_H = 384;           // the Earth app's pictures
constexpr int BAND = 32, NBAND_TEX = (TEX_H + BAND - 1) / BAND;
constexpr int TRK_BACK = 40, TRK_AHEAD = 90, TRK_N = TRK_BACK + TRK_AHEAD + 1;   // minutes
constexpr float RE_KM = 6378.137f, MU = 398600.4418f;
constexpr float DEG = 57.29578f, RAD = 0.017453293f;

const lv_color_t C_AMBER = LV_COLOR_MAKE(0xF5, 0xB3, 0x42);
const lv_color_t C_INK   = LV_COLOR_MAKE(0xF2, 0xF0, 0xEA);
const lv_color_t C_MUTED = LV_COLOR_MAKE(0xA8, 0xA3, 0x99);

// ---- elements: written by the feed task, read by the worker ----------------------------------
struct Tle { char l1[72], l2[72]; };
portMUX_TYPE  s_mux = portMUX_INITIALIZER_UNLOCKED;
bool          s_tleHave[NTLE] = {};
uint32_t      s_tleAt = 0;                 // unix time of the last good fetch
volatile uint32_t s_tleGen = 0;            // bumped on every new set
volatile bool s_shown = true;
uint32_t      s_nextTryMs = 0;

// ---- the worker's results, published under s_mux --------------------------------------------
enum PassState : uint8_t { PS_WAIT, PS_FOUND, PS_NONE, PS_NEVER, PS_NODATA };
struct Pub {
    uint32_t seq = 0;
    int      sat = -1;
    bool     posOk = false;
    float    lat = 0, lon = 0, alt = 0, speed = 0, az = 0, el = 0;
    int16_t  vis = -2;                     // Sgp4: -2 below horizon, -1 daylight, 1000 lit, else shadow
    float    trk[TRK_N][2] = {};           // lat, lon (radians); index TRK_BACK is now
    int      trkN = 0;
    PassState pass = PS_WAIT;
    uint32_t aos = 0, los = 0;
    float    maxEl = 0, azStart = 0, azStop = 0, incl = 0;
    uint8_t  sight = 0;                    // 0 daylight, 1 shadow, 2 visible
    float    ra = 0, dec = 0, distKm = 0;  // Webb
    bool     tableEnded = false;
    int      lut = -1;                     // which projection table matches the view
};
// The big buffers live in PSRAM, allocated once in init(): as globals they would sit in
// internal RAM, about 5 KB of it, for the life of the firmware.
struct Shared {
    Pub   pub;                    // published by the worker, under s_mux
    Pub   view;                   // the LVGL side's copy
    Pub   w;                      // the worker's working copy
    Tle   tle[NTLE];              // the elements, under s_mux
    float pts[91][2];             // graticule scratch for the LVGL side
};
Shared *S = nullptr;

// ---- LVGL objects and buffers (LVGL side) -----------------------------------------------------
lv_obj_t   *s_scr = nullptr, *s_canvas = nullptr;
lv_obj_t   *s_name = nullptr, *s_sub = nullptr, *s_l1 = nullptr, *s_l2 = nullptr, *s_l3 = nullptr;
lv_obj_t   *s_home = nullptr, *s_msg = nullptr, *s_dot[NSAT] = {};
lv_timer_t *s_timer = nullptr;
lv_color_t *s_pix = nullptr;               // canvas, SCR x SCR
lv_color_t *s_box = nullptr;               // the background behind the globe, BOX x BOX
uint16_t   *s_dayBand[NBAND_TEX] = {};
uint8_t    *s_nightBand[NBAND_TEX] = {};
inline uint16_t *dayRow(int v)   { return s_dayBand[v / BAND] + (v % BAND) * TEX_W; }
inline uint8_t  *nightRow(int v) { return s_nightBand[v / BAND] + (v % BAND) * TEX_W; }

// The projection: for every pixel of the disc, the picture column at longitude zero, the
// picture row and the distance from the limb, packed as in globe_view.cpp.
struct Tables {
    int16_t  x0[2 * GR], x1[2 * GR];      // disc span per row (screen x); x0 > x1 = none
    int32_t  rowStart[2 * GR];
    int16_t  sinLat[TEX_H], cosLat[TEX_H];
    int16_t  cosLon[TEX_W];
    uint8_t  twi[513];
    int      count;
};
Tables   *T = nullptr;
uint32_t *s_lut[2] = {};
float     s_lutLat[2] = { 999, 999 };
volatile int  s_lutInUse = -1;             // the table the last frame was drawn with
volatile bool s_active = false;            // the screen is open and its buffers exist
volatile bool s_busy = false;              // the worker is inside a cycle
volatile int  s_sat = 0;                   // the satellite the knob chose
float     s_homeLat = 0, s_homeLon = 0;
char      s_homeName[40] = "";
uint32_t  s_lastFrameMs = 0, s_drawnSeq = 0;
bool      s_dirty = true;

TaskHandle_t s_task = nullptr;
StaticTask_t s_taskTcb;
constexpr uint32_t TASK_STACK = 16384;

// ---- small helpers ---------------------------------------------------------------------------
uint32_t jd2unix(double jd) { return (uint32_t)((jd - 2440587.5) * 86400.0 + 0.5); }

double gmst_deg(double unix) {
    const double d = unix / 86400.0 + 2440587.5 - 2451545.0;
    double g = fmod(280.46061837 + 360.98564736629 * d, 360.0);
    return g < 0 ? g + 360 : g;
}

float wrap180(float d) { while (d > 180) d -= 360; while (d <= -180) d += 360; return d; }

const char *dir8(float az) {
    static const char *EN[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    static const char *DE[8] = { "N", "NO", "O", "SO", "S", "SW", "W", "NW" };
    int i = (int)lroundf(fmodf(fmodf(az, 360) + 360, 360) / 45.0f) % 8;
    return lang::de() ? DE[i] : EN[i];
}

// 27580 -> "27.580" in German, "27,580" in English
void thousands(char *out, size_t n, long v) {
    char tmp[24]; snprintf(tmp, sizeof(tmp), "%ld", v < 0 ? -v : v);
    const int len = strlen(tmp); size_t o = 0;
    if (v < 0 && o + 1 < n) out[o++] = '-';
    for (int i = 0; i < len && o + 1 < n; ++i) {
        out[o++] = tmp[i];
        if ((len - i - 1) % 3 == 0 && i < len - 1 && o + 1 < n) out[o++] = lang::de() ? '.' : ',';
    }
    out[o] = 0;
}

// "Mo 06:33", or just "06:33" today
void when(char *out, size_t n, uint32_t t, uint32_t now) {
    static const char *EN[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *DE[7] = { "So", "Mo", "Di", "Mi", "Do", "Fr", "Sa" };
    time_t a = t, b = now; struct tm ta, tb;
    localtime_r(&a, &ta); localtime_r(&b, &tb);
    if (ta.tm_yday == tb.tm_yday && ta.tm_year == tb.tm_year) snprintf(out, n, "%02d:%02d", ta.tm_hour, ta.tm_min);
    else snprintf(out, n, "%s %02d:%02d", (lang::de() ? DE : EN)[ta.tm_wday], ta.tm_hour, ta.tm_min);
}

void duration(char *out, size_t n, long s) {
    const long m = s < 0 ? 0 : (s + 30) / 60;
    if (m >= 60) snprintf(out, n, "%ld h %ld min", m / 60, m % 60);
    else snprintf(out, n, "%ld min", m);
}

// ---- elements: storage and parsing ------------------------------------------------------------
bool parse_tle(const char *text, size_t len, Tle &out) {
    // Three lines: a name, then "1 ..." and "2 ...", each 69 characters.
    const char *p = text, *end = text + len; bool h1 = false, h2 = false;
    while (p < end) {
        const char *e = (const char *)memchr(p, '\n', end - p); if (!e) e = end;
        size_t n = e - p; while (n && (p[n - 1] == '\r' || p[n - 1] == ' ')) --n;
        if (n >= 69 && p[0] == '1' && p[1] == ' ') { memcpy(out.l1, p, 69); out.l1[69] = 0; h1 = true; }
        if (n >= 69 && p[0] == '2' && p[1] == ' ') { memcpy(out.l2, p, 69); out.l2[69] = 0; h2 = true; }
        p = e + 1;
    }
    return h1 && h2;
}

void load_saved() {
    Preferences p;
    p.begin("capsuleradar", true);
    s_shown = p.getBool("satShow", true);
    s_tleAt = p.getUInt("tleAt", 0);
    for (int i = 0; i < NTLE; ++i) {
        char key[8]; snprintf(key, sizeof(key), "tle%d", i);
        char buf[150] = "";
        if (p.isKey(key)) p.getString(key, buf, sizeof(buf));
        s_tleHave[i] = parse_tle(buf, strlen(buf), S->tle[i]);
    }
    p.end();
    if (s_tleHave[0] || s_tleHave[1] || s_tleHave[2]) s_tleGen = 1;
}

// ---- the worker ----------------------------------------------------------------------------
Sgp4    *s_sgp[NTLE] = {};       // position
Sgp4    *s_pred[NTLE] = {};      // pass prediction, kept separate so one never disturbs the other
uint32_t s_wGen = 0;             // the element set the worker's objects were built from
int      s_wSat = -1;
uint32_t s_wPosAt = 0, s_wTrkAt = 0, s_wPassAt = 0;
double   s_wHomeLat = 999, s_wHomeLon = 999;

void build_sgp() {
    Tle copy[NTLE]; bool have[NTLE];
    portENTER_CRITICAL(&s_mux);
    memcpy(copy, S->tle, sizeof(copy)); memcpy(have, s_tleHave, sizeof(have));
    portEXIT_CRITICAL(&s_mux);
    for (int i = 0; i < NTLE; ++i) {
        delete s_sgp[i];  s_sgp[i] = nullptr;
        delete s_pred[i]; s_pred[i] = nullptr;
        if (!have[i]) continue;
        char a[130], b[130];
        for (Sgp4 **o : { &s_sgp[i], &s_pred[i] }) {
            *o = new Sgp4();
            strcpy(a, copy[i].l1); strcpy(b, copy[i].l2);
            (*o)->init(SATS[i].name, a, b);
            (*o)->site(s_homeLat, s_homeLon, 100);
        }
    }
}

void webb(uint32_t t, float &ra, float &dec, float &dist, bool &ended) {
    const double days = ((double)t - JWST_EPOCH) / 86400.0;
    int i = (int)floor(days);
    ended = i < 0 || i >= JWST_DAYS - 1;
    if (i < 0) i = 0; if (i > JWST_DAYS - 2) i = JWST_DAYS - 2;
    const float f = (float)(days - i);
    const JwstPoint &a = JWST_TABLE[i], &b = JWST_TABLE[i + 1];
    float r0 = a.raHundredths / 100.0f, r1 = b.raHundredths / 100.0f;
    if (r1 - r0 > 180) r1 -= 360; if (r0 - r1 > 180) r1 += 360;
    ra = fmodf(r0 + (r1 - r0) * f + 360, 360);
    dec = (a.decHundredths + (b.decHundredths - a.decHundredths) * f) / 100.0f;
    dist = (a.distThousandKm + (b.distThousandKm - a.distThousandKm) * f) * 1000.0f;
}

// Webb's place over the Earth and in the sky over home, from right ascension and declination.
void webb_at(uint32_t t, float &lat, float &lon, float &az, float &el, float &ra, float &dec, float &dist, bool &ended) {
    webb(t, ra, dec, dist, ended);
    const float g = (float)gmst_deg(t);
    lat = dec * RAD;
    lon = wrap180(ra - g) * RAD;
    const float H = wrap180(g + s_homeLon - ra) * RAD, phi = s_homeLat * RAD, d = dec * RAD;
    el = asinf(sinf(phi) * sinf(d) + cosf(phi) * cosf(d) * cosf(H)) * DEG;
    az = fmodf(atan2f(-cosf(d) * sinf(H), sinf(d) * cosf(phi) - cosf(d) * sinf(phi) * cosf(H)) * DEG + 360, 360);
}

void work_position(uint32_t now) {
    Pub &w = S->w;
    if (w.sat == JWST) {
        float lat, lon, az, el, ra, dec, dist; bool ended;
        webb_at(now, lat, lon, az, el, ra, dec, dist, ended);
        w.posOk = true; w.lat = lat; w.lon = lon; w.az = az; w.el = el;
        w.ra = ra; w.dec = dec; w.distKm = dist; w.tableEnded = ended; w.alt = dist; w.speed = 0;
        return;
    }
    Sgp4 *s = s_sgp[w.sat];
    if (!s) { w.posOk = false; return; }
    s->findsat((unsigned long)now);
    w.posOk = true;
    w.lat = s->satLat * RAD; w.lon = wrap180(s->satLon) * RAD;
    w.alt = s->satAlt; w.az = s->satAz; w.el = s->satEl; w.vis = s->satVis;
    const float a = s->satrec.a * RE_KM, r = RE_KM + w.alt;
    w.speed = sqrtf(MU * (2.0f / r - 1.0f / a)) * 3600.0f;
    float inc = s->satrec.inclo * DEG; if (inc > 90) inc = 180 - inc;
    w.incl = inc;
}

void work_track(uint32_t now) {
    Pub &w = S->w;
    w.trkN = 0;
    for (int m = -TRK_BACK; m <= TRK_AHEAD && s_active; ++m) {
        const uint32_t t = now + m * 60;
        float lat, lon;
        if (w.sat == JWST) {
            float az, el, ra, dec, dist; bool ended;
            webb_at(t, lat, lon, az, el, ra, dec, dist, ended);
        } else {
            Sgp4 *s = s_sgp[w.sat]; if (!s) return;
            s->findsat((unsigned long)t);
            lat = s->satLat * RAD; lon = wrap180(s->satLon) * RAD;
        }
        w.trk[w.trkN][0] = lat; w.trk[w.trkN][1] = lon; ++w.trkN;
    }
    work_position(now);   // findsat above moved the object off "now"
}

void work_pass(uint32_t now) {
    Pub &w = S->w;
    if (w.sat == JWST) {
        // Webb rises and sets once a day like a star. When it is up, find when it sets;
        // otherwise when it next rises. Ten-minute steps, then a straight line between.
        float lat, lon, az, el, ra, dec, dist; bool ended;
        webb_at(now, lat, lon, az, el, ra, dec, dist, ended);
        const bool up = el > 0; float prev = el;
        w.pass = PS_NONE; w.aos = w.los = 0;
        for (uint32_t t = now + 600; t < now + 30 * 3600; t += 600) {
            webb_at(t, lat, lon, az, el, ra, dec, dist, ended);
            if ((el > 0) != up) {
                const uint32_t at = t - (uint32_t)(600 * el / (el - prev));
                if (up) w.los = at; else w.aos = at;
                w.azStart = az; w.pass = PS_FOUND; break;
            }
            prev = el;
        }
        return;
    }
    Sgp4 *p = s_pred[w.sat];
    if (!p) { w.pass = PS_NODATA; return; }
    // Some orbits never reach home's sky at all: the inclination plus the radius of the
    // footprint is the highest latitude that can see it. Hubble's is 28.5 degrees, which
    // tops out near 50.7 degrees north, so from 51 degrees it is never above the horizon.
    const float foot = acosf(RE_KM / (RE_KM + w.alt)) * DEG;
    if (w.incl + foot < fabsf(s_homeLat) - 0.3f) { w.pass = PS_NEVER; return; }
    w.pass = PS_NONE;
    if (!p->initpredpoint((unsigned long)now, 0.0)) return;
    passinfo pd;
    for (int it = 0; it < 48 && s_active && s_sat == w.sat; ++it) {   // 48 orbits, three days
        if (!p->nextpass(&pd, 1, false, 5.0)) continue;               // under 5 degrees: not worth a look
        const uint32_t los = jd2unix(pd.jdstop);
        if (los <= now) continue;
        w.aos = jd2unix(pd.jdstart); w.los = los;
        w.maxEl = pd.maxelevation; w.azStart = pd.azstart; w.azStop = pd.azstop;
        w.sight = (pd.visstart == lighted || pd.vismax == lighted || pd.visstop == lighted) ? 2
                : (pd.sight == daylight ? 0 : 1);
        w.pass = PS_FOUND;
        return;
    }
}

// The projection table for a view centred on latitude lat0 (radians), into s_lut[idx].
bool work_lut(int idx, float lat0) {
    uint32_t *out = s_lut[idx];
    const float sp = sinf(lat0), cp = cosf(lat0), toCol = TEX_W / 360.0f, toRow = TEX_H / 180.0f;
    for (int r = 0; r < 2 * GR; ++r) {
        if (!s_active) return false;
        const int y = GCY - GR + r;
        const float py = -((y + 0.5f) - GCY) / GR;
        uint32_t *row = out + T->rowStart[r];
        int k = 0;
        for (int x = T->x0[r]; x <= T->x1[r]; ++x) {
            const float px = ((x + 0.5f) - GCX) / GR;
            const float z = sqrtf(fmaxf(0.0f, 1 - px * px - py * py));
            const float lat = asinf(fmaxf(-1.0f, fminf(1.0f, z * sp + py * cp)));
            float lonDeg = 180.0f + atan2f(px, z * cp - py * sp) * DEG;
            if (lonDeg >= 360.0f) lonDeg -= 360.0f; if (lonDeg < 0.0f) lonDeg += 360.0f;
            int u = (int)(lonDeg * toCol); if (u >= TEX_W) u -= TEX_W;
            int v = (int)((90.0f - lat * DEG) * toRow); v = v < 0 ? 0 : (v >= TEX_H ? TEX_H - 1 : v);
            const int zq = (int)(z * 255.0f + 0.5f);
            row[k++] = (uint32_t)u | ((uint32_t)v << 10) | ((uint32_t)zq << 19);
        }
        if ((r & 15) == 15) vTaskDelay(1);   // let core 0's other tasks in
    }
    s_lutLat[idx] = lat0;
    return true;
}

void publish() {
    S->w.seq++;
    portENTER_CRITICAL(&s_mux);
    S->pub = S->w;
    portEXIT_CRITICAL(&s_mux);
}

void worker(void *) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
        if (!s_active) continue;
        s_busy = true;
        const uint32_t now = (uint32_t)time(nullptr);
        if (now < 1700000000u) { s_busy = false; continue; }              // no clock yet
        bool changed = false;
        if (s_wGen != s_tleGen || s_wHomeLat != s_homeLat || s_wHomeLon != s_homeLon) {
            s_wGen = s_tleGen; s_wHomeLat = s_homeLat; s_wHomeLon = s_homeLon;
            build_sgp(); changed = true;
        }
        if (s_wSat != s_sat) { s_wSat = s_sat; changed = true; }
        if (changed) {
            S->w.sat = s_wSat; S->w.trkN = 0; S->w.pass = PS_WAIT;
            s_wTrkAt = s_wPassAt = 0; s_wPosAt = 0;
        }
        bool pub = false;
        if (now != s_wPosAt) { s_wPosAt = now; work_position(now); pub = true; }
        // Projection: rebuild into the spare table when the view has moved a degree, and only
        // once the drawing side has stopped using that table. With no position yet the globe
        // stands over home.
        const float viewLat = S->w.posOk ? S->w.lat : s_homeLat * RAD;
        const int show = S->w.lut;
        if (show < 0 || fabsf(s_lutLat[show] - viewLat) > 1.0f * RAD) {
            const int spare = show < 0 ? 0 : 1 - show;
            if (show < 0 || s_lutInUse != spare) {
                if (work_lut(spare, viewLat)) { S->w.lut = spare; pub = true; }
            }
        }
        if (S->w.posOk) {
            if (!s_wTrkAt || now - s_wTrkAt >= 60) { s_wTrkAt = now; publish(); work_track(now); pub = true; }
            const bool passOver = S->w.pass == PS_FOUND && now > (S->w.los ? S->w.los : S->w.aos) + 5;
            if (!s_wPassAt || passOver || now - s_wPassAt >= 1800) {
                s_wPassAt = now; publish(); work_pass(now); pub = true;
            }
        } else if (S->w.pass != PS_NODATA) { S->w.pass = PS_NODATA; pub = true; }
        if (pub) publish();
        s_busy = false;
    }
}

// ---- pictures (LVGL side, on entry) -----------------------------------------------------------
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
            else nightRow(yy)[xx] = (uint8_t)(((p >> 5) & 0x3F) * 255 / 63);   // grey: green is brightness
        }
    }
    return true;
}

bool load_jpg(const char *path, Target t) {
    size_t len = 0;
    uint8_t *buf = theme_sd::read_whole(path, len, 1024 * 1024);
    if (!buf) { Serial.printf("[sat] cannot read %s\n", path); return false; }
    uint16_t w = 0, h = 0;
    bool ok = TJpgDec.getJpgSize(&w, &h, buf, len) == JDR_OK && w == TEX_W && h == TEX_H;
    if (ok) {
        s_target = t;
        TJpgDec.setJpgScale(1);
        TJpgDec.setSwapBytes(false);
        TJpgDec.setCallback(jpg_out);
        ok = TJpgDec.drawJpg(0, 0, buf, len) == JDR_OK;
    }
    theme_sd::free(buf);
    return ok;
}

// ---- drawing ---------------------------------------------------------------------------------
inline void blend(int x, int y, lv_color_t c, uint8_t a) {
    if ((unsigned)x >= (unsigned)SCR || (unsigned)y >= (unsigned)SCR) return;
    lv_color_t &d = s_pix[y * SCR + x];
    d = lv_color_mix(c, d, a);
}

void disc(float cx, float cy, float r, lv_color_t c, uint8_t a) {
    for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); ++y)
        for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); ++x) {
            const float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
            if (d <= r) blend(x, y, c, a);
            else if (d < r + 1) blend(x, y, c, (uint8_t)(a * (r + 1 - d)));
        }
}

// A line with a dash pattern that carries on across segments (dashOn 0 = solid).
struct Pen { lv_color_t c; uint8_t a; bool thick; int on, off; float pos; };
void seg(Pen &p, float x0, float y0, float x1, float y1) {
    const float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
    const int n = (int)ceilf(len); if (!n) return;
    for (int i = 0; i <= n; ++i) {
        const float t = (float)i / n;
        if (p.on) {
            const float ph = fmodf(p.pos + len * t, (float)(p.on + p.off));
            if (ph >= p.on) continue;
        }
        const int x = (int)lroundf(x0 + dx * t), y = (int)lroundf(y0 + dy * t);
        blend(x, y, p.c, p.a);
        if (p.thick) { blend(x + 1, y, p.c, p.a); blend(x, y + 1, p.c, p.a); }
    }
    p.pos += len;
}

struct Pt { float x, y, z; };
Pt project(float lat, float lon, float sLat0, float cLat0, float lon0) {
    const float d = lon - lon0, cl = cosf(lat), sl = sinf(lat), cd = cosf(d);
    return { GCX + GR * cl * sinf(d), GCY - GR * (sl * cLat0 - cl * sLat0 * cd), sl * sLat0 + cl * cLat0 * cd };
}

// Draws a lat/lon path as far as it is on the near side of the globe.
void path(Pen &pen, const float (*pts)[2], int n, float sLat0, float cLat0, float lon0) {
    bool on = false; Pt last{};
    for (int i = 0; i < n; ++i) {
        const Pt p = project(pts[i][0], pts[i][1], sLat0, cLat0, lon0);
        if (p.z > 0.02f) { if (on) seg(pen, last.x, last.y, p.x, p.y); last = p; on = true; }
        else on = false;
    }
}

void sun_now(int &sunCol, int16_t &sinS, int16_t &cosS) {
    const time_t now = time(nullptr);
    struct tm g;
    gmtime_r(&now, &g);
    const double decl = -23.44 * cos(2 * M_PI / 365.0 * (g.tm_yday + 1 + 10));
    const double h = g.tm_hour + g.tm_min / 60.0 + g.tm_sec / 3600.0;
    const double slon = -(h - 12.0) * 15.0;
    sinS = (int16_t)lround(sin(decl * M_PI / 180) * 16384);
    cosS = (int16_t)lround(cos(decl * M_PI / 180) * 16384);
    double c = fmod(slon + 180.0, 360.0); if (c < 0) c += 360;
    sunCol = (int)(c / 360.0 * TEX_W) % TEX_W;
}

void render_globe(int lut, float lon0) {
    const uint32_t *L = s_lut[lut];
    int rot = (int)lroundf((lon0 * DEG) / 360.0f * TEX_W) % TEX_W; if (rot < 0) rot += TEX_W;
    int sunCol; int16_t sinS, cosS;
    sun_now(sunCol, sinS, cosS);
    for (int r = 0; r < 2 * GR; ++r) {
        if (T->x1[r] < T->x0[r]) continue;
        lv_color_t *out = s_pix + (GCY - GR + r) * SCR;
        const uint32_t *row = L + T->rowStart[r];
        int k = 0;
        for (int x = T->x0[r]; x <= T->x1[r]; ++x) {
            const uint32_t e = row[k++];
            int u = (int)(e & 0x3FF) + rot; while (u >= TEX_W) u -= TEX_W;
            int v = (int)((e >> 10) & 0x1FF); if (v >= TEX_H) v = TEX_H - 1;
            const int z = (int)(e >> 19);
            int du = u - sunCol; if (du < 0) du += TEX_W;
            const int cosz = ((int)T->sinLat[v] * sinS + (((int)T->cosLat[v] * ((cosS * (int)T->cosLon[du]) >> 14)))) >> 14;
            const int t = T->twi[(cosz >> 6) + 256];
            const uint16_t p = dayRow(v)[u];
            const int r8 = (p >> 11) << 3, g8 = ((p >> 5) & 0x3F) << 2, b8 = (p & 0x1F) << 3;
            const int dg = (t * (90 + ((166 * t) >> 8))) >> 8;
            const int nt = 255 - t, Ln = nightRow(v)[u];
            int rr = ((r8 * dg) >> 8) + ((((r8 * 13) >> 8) + Ln) * nt >> 8);
            int gg = ((g8 * dg) >> 8) + ((((g8 * 13) >> 8) + ((Ln * 196) >> 8)) * nt >> 8);
            int bb = ((b8 * dg) >> 8) + ((((b8 * 13) >> 8) + ((Ln * 112) >> 8)) * nt >> 8);
            const int lz = 140 + ((115 * z) >> 8);
            rr = (rr * lz) >> 8; gg = (gg * lz) >> 8; bb = (bb * lz) >> 8;
            if (rr > 255) rr = 255; if (gg > 255) gg = 255; if (bb > 255) bb = 255;
            out[x] = lv_color_make(rr, gg, bb);
        }
    }
}

void reticle(float x, float y) {
    for (float a = 0; a < 360; a += 2) {
        const float s = sinf(a * RAD), c = cosf(a * RAD);
        blend((int)lroundf(x + 8 * c), (int)lroundf(y + 8 * s), C_AMBER, 255);
        blend((int)lroundf(x + 9 * c), (int)lroundf(y + 9 * s), C_AMBER, 200);
    }
    Pen p{ C_AMBER, 255, true, 0, 0, 0 };
    seg(p, x, y - 13, x, y - 4); seg(p, x, y + 4, x, y + 13);
    seg(p, x - 13, y, x - 4, y); seg(p, x + 4, y, x + 13, y);
    disc(x, y, 1.6f, C_AMBER, 255);
}

void draw_frame() {
    const Pub &v = S->view;
    // the background behind the globe, with its halo
    for (int r = 0; r < BOX; ++r) memcpy(s_pix + (BOX_Y0 + r) * SCR + BOX_X0, s_box + r * BOX, BOX * sizeof(lv_color_t));
    const float lat0 = v.posOk ? v.lat : s_homeLat * RAD, lon0 = v.posOk ? v.lon : s_homeLon * RAD;
    if (v.lut >= 0) { render_globe(v.lut, lon0); s_lutInUse = v.lut; }
    const float sLat0 = sinf(lat0), cLat0 = cosf(lat0);

    // graticule, every 30 degrees
    {
        float (*pts)[2] = S->pts;   // not on the stack: the LVGL loop has little to spare
        for (int la = -60; la <= 60; la += 30) {
            for (int i = 0; i <= 90; ++i) { pts[i][0] = la * RAD; pts[i][1] = (-180 + i * 4) * RAD; }
            Pen p{ C_AMBER, 46, false, 0, 0, 0 }; path(p, pts, 91, sLat0, cLat0, lon0);
        }
        for (int lo = -180; lo < 180; lo += 30) {
            for (int i = 0; i <= 45; ++i) { pts[i][0] = (-90 + i * 4) * RAD; pts[i][1] = lo * RAD; }
            Pen p{ C_AMBER, 46, false, 0, 0, 0 }; path(p, pts, 46, sLat0, cLat0, lon0);
        }
    }
    if (v.posOk) {
        // the footprint: where on Earth the satellite is above the horizon right now
        const float rho = acosf(RE_KM / (RE_KM + v.alt));
        if (rho < 80 * RAD) {
            Pen p{ C_AMBER, 140, false, 3, 4, 0 };
            const float rr = GR * sinf(rho); float px = GCX + rr, py = GCY;
            for (int a = 3; a <= 360; a += 3) {
                const float nx = GCX + rr * cosf(a * RAD), ny = GCY + rr * sinf(a * RAD);
                seg(p, px, py, nx, ny); px = nx; py = ny;
            }
        }
        // the ground track: behind dashed and dim, ahead solid
        if (v.trkN == TRK_N) {
            Pen back{ C_AMBER, 110, false, 4, 4, 0 };  path(back, v.trk, TRK_BACK + 1, sLat0, cLat0, lon0);
            Pen ahead{ C_AMBER, 255, true, 0, 0, 0 };  path(ahead, v.trk + TRK_BACK, TRK_AHEAD + 1, sLat0, cLat0, lon0);
        }
    }
    // home
    const Pt h = project(s_homeLat * RAD, s_homeLon * RAD, sLat0, cLat0, lon0);
    if (h.z > 0.05f) {
        disc(h.x, h.y, 2.6f, C_INK, 255);
        for (float a = 0; a < 360; a += 4)
            blend((int)lroundf(h.x + 6 * cosf(a * RAD)), (int)lroundf(h.y + 6 * sinf(a * RAD)), C_INK, 150);
        lv_obj_set_pos(s_home, (lv_coord_t)(h.x + 10), (lv_coord_t)(h.y - 8));
        lv_obj_clear_flag(s_home, LV_OBJ_FLAG_HIDDEN);
    } else lv_obj_add_flag(s_home, LV_OBJ_FLAG_HIDDEN);
    if (v.posOk) reticle(GCX, GCY);
    lv_obj_invalidate(s_canvas);
}

void set_texts(uint32_t now) {
    const Pub &v = S->view;
    const int i = s_sat;
    lv_label_set_text(s_name, SATS[i].name);
    lv_label_set_text(s_sub, lang::de() ? SATS[i].de : SATS[i].en);
    for (int k = 0; k < NSAT; ++k) {
        const bool on = k == i;
        lv_obj_set_size(s_dot[k], on ? 7 : 5, on ? 7 : 5);
        lv_obj_set_style_bg_color(s_dot[k], on ? C_AMBER : C_INK, 0);
        lv_obj_set_style_bg_opa(s_dot[k], on ? LV_OPA_COVER : LV_OPA_40, 0);
        lv_obj_align(s_dot[k], LV_ALIGN_TOP_MID, (k * 2 - (NSAT - 1)) * 7, 111 - (on ? 1 : 0));
    }
    char a[64] = "", b[64] = "", c[80] = "", t1[24], t2[24];
    if (!v.posOk || v.sat != i) {
        snprintf(b, sizeof(b), "%s", (v.pass == PS_NODATA) ? tr("LOADING ORBIT DATA ...", "LADE BAHNDATEN ...")
                                                           : tr("CALCULATING ...", "BERECHNE ..."));
    } else if (i == JWST) {
        // 1.39 million km, and how long its light takes
        if (lang::de()) snprintf(a, sizeof(a), "%.2f Mio. km · Licht %.1f s", v.distKm / 1e6f, v.distKm / 299792.458f);
        else            snprintf(a, sizeof(a), "%.2f million km · light %.1f s", v.distKm / 1e6f, v.distKm / 299792.458f);
        if (lang::de()) for (char *p = a; *p; ++p) if (*p == '.' && p[1] >= '0' && p[1] <= '9') *p = ',';
        if (v.tableEnded) {
            snprintf(b, sizeof(b), "%s", tr("POSITION TABLE ENDED", "POSITIONSTABELLE ABGELAUFEN"));
            snprintf(c, sizeof(c), "%s", tr("update the firmware", "Firmware aktualisieren"));
        } else {
            if (v.el > 0) snprintf(b, sizeof(b), tr("ABOVE YOUR HORIZON · %d° %s", "ÜBER DEM HORIZONT · %d° %s"), (int)lroundf(v.el), dir8(v.az));
            else if (v.pass == PS_FOUND && v.aos) { when(t1, sizeof(t1), v.aos, now); snprintf(b, sizeof(b), tr("RISES AT %s", "GEHT AUF UM %s"), t1); }
            const int raM = (int)lroundf(v.ra / 15.0f * 60.0f);
            if (lang::de()) snprintf(c, sizeof(c), "RA %dh %02dm · Dek. %+.1f°", raM / 60 % 24, raM % 60, v.dec);
            else            snprintf(c, sizeof(c), "RA %dh %02dm · Dec %+.1f°", raM / 60 % 24, raM % 60, v.dec);
            if (lang::de()) for (char *p = c; *p; ++p) if (*p == '.' && p[1] >= '0' && p[1] <= '9') *p = ',';
        }
    } else {
        thousands(t1, sizeof(t1), lroundf(v.alt)); thousands(t2, sizeof(t2), lroundf(v.speed / 10) * 10);
        snprintf(a, sizeof(a), "%s km · %s km/h", t1, t2);
        const char *seen = v.vis >= 1000 ? tr("visible", "sichtbar") : v.vis == -1 ? tr("in daylight", "bei Tag") : tr("in shadow", "im Schatten");
        if (v.el > 0) {
            snprintf(b, sizeof(b), tr("OVERHEAD NOW · %d° %s", "JETZT ÜBER DIR · %d° %s"), (int)lroundf(v.el), dir8(v.az));
            if (v.pass == PS_FOUND && v.aos <= now) { when(t1, sizeof(t1), v.los, now); snprintf(c, sizeof(c), tr("until %s · %s", "bis %s · %s"), t1, seen); }
            else snprintf(c, sizeof(c), "%s", seen);
        } else switch (v.pass) {
            case PS_FOUND: {
                duration(t1, sizeof(t1), (long)v.aos - (long)now);
                snprintf(b, sizeof(b), tr("NEXT PASS IN %s", "ÜBERFLUG IN %s"), t1);
                when(t2, sizeof(t2), v.aos, now);
                const char *how = v.sight == 2 ? tr("visible", "sichtbar") : v.sight == 1 ? tr("in shadow", "im Schatten") : tr("in daylight", "bei Tag");
                snprintf(c, sizeof(c), "%s · %d° · %s → %s · %s", t2, (int)lroundf(v.maxEl), dir8(v.azStart), dir8(v.azStop), how);
                break;
            }
            case PS_NEVER:
                snprintf(b, sizeof(b), "%s", tr("NEVER ABOVE YOUR HORIZON", "NIE ÜBER DEINEM HORIZONT"));
                snprintf(c, sizeof(c), tr("orbit tilted %d°, you are at %d° N", "Bahn %d° geneigt, du bist bei %d° N"),
                         (int)lroundf(v.incl), (int)lroundf(fabsf(s_homeLat)));
                break;
            case PS_NONE: snprintf(b, sizeof(b), "%s", tr("NO PASS IN 3 DAYS", "KEIN ÜBERFLUG IN 3 TAGEN")); break;
            default:      snprintf(b, sizeof(b), "%s", tr("CALCULATING PASSES ...", "BERECHNE ÜBERFLÜGE ...")); break;
        }
    }
    lv_label_set_text(s_l1, a);
    lv_label_set_text(s_l2, b);
    lv_label_set_text(s_l3, c);
}

void tick_cb(lv_timer_t *) {
    if (!s_active) return;
    bool fresh = false;
    portENTER_CRITICAL(&s_mux);
    if (S->pub.seq != s_drawnSeq) { S->view = S->pub; s_drawnSeq = S->pub.seq; fresh = true; }
    portEXIT_CRITICAL(&s_mux);
    const uint32_t ms = millis();
    if (!fresh && !s_dirty && ms - s_lastFrameMs < 1000) return;
    s_lastFrameMs = ms; s_dirty = false;
    draw_frame();
    set_texts((uint32_t)time(nullptr));
}

void release_all() {
    if (s_canvas) { lv_obj_del(s_canvas); s_canvas = nullptr; }
    if (s_pix) { heap_caps_free(s_pix); s_pix = nullptr; }
    if (s_box) { heap_caps_free(s_box); s_box = nullptr; }
    for (auto &b : s_dayBand)   if (b) { heap_caps_free(b); b = nullptr; }
    for (auto &b : s_nightBand) if (b) { heap_caps_free(b); b = nullptr; }
    for (auto &l : s_lut) if (l) { heap_caps_free(l); l = nullptr; }
    s_lutLat[0] = s_lutLat[1] = 999;
    if (T) { heap_caps_free(T); T = nullptr; }
}

void show_msg(const char *t) { lv_label_set_text(s_msg, t); lv_obj_clear_flag(s_msg, LV_OBJ_FLAG_HIDDEN); }

lv_obj_t *label(const lv_font_t *f, lv_color_t c, int spacing, int y, int width) {
    lv_obj_t *l = lv_label_create(s_scr);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_style_text_letter_space(l, spacing, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, width);
    lv_label_set_text(l, "");
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    return l;
}

// The background: the active theme's Settings plate when it has one (the Tagesring dial),
// otherwise black with a thin amber rim. Then the halo, then a copy of what sits behind the
// globe, so every frame can put it back before painting the Earth.
void paint_background() {
    const lv_img_dsc_t *plate = settings_custom_plate();
    if (plate && plate->header.w == SCR && plate->header.h == SCR && plate->data) {
        memcpy(s_pix, plate->data, (size_t)SCR * SCR * sizeof(lv_color_t));
    } else {
        for (int i = 0; i < SCR * SCR; ++i) s_pix[i] = lv_color_make(10, 9, 8);
        for (float a = 0; a < 360; a += 0.25f) blend((int)lroundf(GCX + 229 * cosf(a * RAD)), (int)lroundf(233 + 229 * sinf(a * RAD)), C_AMBER, 160);
    }
    settings_sprite_release();   // the Settings screen decodes its own again when it opens
    for (int y = BOX_Y0; y < BOX_Y0 + BOX; ++y)
        for (int x = BOX_X0; x < BOX_X0 + BOX; ++x) {
            const float d = sqrtf((x + 0.5f - GCX) * (x + 0.5f - GCX) + (y + 0.5f - GCY) * (y + 0.5f - GCY)) - GR;
            if (d > -1 && d < HALO) { const float g = 1.0f - (d + 1) / (HALO + 1); blend(x, y, C_AMBER, (uint8_t)(g * g * 90)); }
        }
    for (int r = 0; r < BOX; ++r) memcpy(s_box + r * BOX, s_pix + (BOX_Y0 + r) * SCR + BOX_X0, BOX * sizeof(lv_color_t));
}

bool build_tables() {
    for (int v = 0; v < TEX_H; ++v) {
        const double lat = (90.0 - (v + 0.5) * 180.0 / TEX_H) * M_PI / 180.0;
        T->sinLat[v] = (int16_t)lround(sin(lat) * 16384);
        T->cosLat[v] = (int16_t)lround(cos(lat) * 16384);
    }
    for (int d = 0; d < TEX_W; ++d) T->cosLon[d] = (int16_t)lround(cos(d * 2 * M_PI / TEX_W) * 16384);
    for (int i = 0; i < 513; ++i) {
        const float c = (i - 256) / 256.0f;
        float t = (c + 0.10f) / 0.20f; t = t < 0 ? 0 : (t > 1 ? 1 : t);
        T->twi[i] = (uint8_t)lroundf(t * 255);
    }
    int n = 0;
    for (int r = 0; r < 2 * GR; ++r) {
        const float dy = (GCY - GR + r + 0.5f) - GCY;
        T->rowStart[r] = n;
        if (fabsf(dy) >= GR) { T->x0[r] = 1; T->x1[r] = 0; continue; }
        const float half = sqrtf((float)GR * GR - dy * dy);
        T->x0[r] = (int16_t)ceilf(GCX - half);
        T->x1[r] = (int16_t)floorf(GCX + half - 1);
        if (T->x1[r] >= T->x0[r]) n += T->x1[r] - T->x0[r] + 1;
    }
    T->count = n;
    for (auto &l : s_lut) { l = (uint32_t *)heap_caps_malloc((size_t)n * 4, MALLOC_CAP_SPIRAM); if (!l) return false; }
    return true;
}

}  // namespace

namespace satview {

void init() {
    // The screen is always built, so the shell has something to show; without the buffers
    // it simply does nothing (every entry point checks S).
    void *mem = heap_caps_calloc(1, sizeof(Shared), MALLOC_CAP_SPIRAM);
    if (mem) { S = new (mem) Shared(); load_saved(); }
    else Serial.println("[sat] no PSRAM for its buffers");
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    s_name = label(&font_sat_24b, C_AMBER, 3, 60, 300);
    s_sub  = label(&font_sat_12, C_INK, 2, 92, 300);
    for (auto &d : s_dot) {
        d = lv_obj_create(s_scr);
        lv_obj_remove_style_all(d);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_size(d, 5, 5);
    }
    s_l1 = label(&font_sat_18, C_INK, 0, 326, 320);
    s_l2 = label(&font_sat_16b, C_AMBER, 1, 349, 312);
    s_l3 = label(&font_sat_14, C_MUTED, 0, 370, 274);
    s_home = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_home, &font_sat_12, 0);
    lv_obj_set_style_text_color(s_home, C_INK, 0);
    lv_obj_set_style_text_letter_space(s_home, 1, 0);
    lv_obj_add_flag(s_home, LV_OBJ_FLAG_HIDDEN);
    s_msg = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_msg, &font_sat_14, 0);
    lv_obj_set_style_text_color(s_msg, C_INK, 0);
    lv_obj_set_style_text_align(s_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_msg, LV_ALIGN_CENTER, 0, -10);
    lv_obj_add_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
    s_timer = lv_timer_create(tick_cb, 40, nullptr);

    if (!S) return;
    StackType_t *stack = (StackType_t *)heap_caps_malloc(TASK_STACK, MALLOC_CAP_SPIRAM);
    if (stack) s_task = xTaskCreateStaticPinnedToCore(worker, "sat", TASK_STACK, nullptr, 1, stack, &s_taskTcb, 0);
    if (!s_task) Serial.println("[sat] could not create the worker");
}

lv_obj_t *screen() { return s_scr; }

void onEnter(double homeLat, double homeLon, const char *homeName) {
    if (!S) return;
    const uint32_t t0 = millis();
    lv_obj_add_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
    s_homeLat = (float)homeLat; s_homeLon = (float)homeLon;
    // The home label in capitals, the German umlauts included (UTF-8: a lower-case umlaut is
    // 0xC3 0xA4/B6/BC and its capital 0xC3 0x84/96/9C).
    snprintf(s_homeName, sizeof(s_homeName), "%s", (homeName && *homeName) ? homeName : tr("HOME", "ZUHAUSE"));
    for (char *p = s_homeName; *p; ++p) {
        if (*p >= 'a' && *p <= 'z') *p -= 32;
        else if ((uint8_t)p[0] == 0xC3 && ((uint8_t)p[1] == 0xA4 || (uint8_t)p[1] == 0xB6 || (uint8_t)p[1] == 0xBC)) { p[1] -= 0x20; ++p; }
    }
    lv_label_set_text(s_home, s_homeName);

    s_pix = (lv_color_t *)heap_caps_malloc((size_t)SCR * SCR * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_box = (lv_color_t *)heap_caps_malloc((size_t)BOX * BOX * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    T = (Tables *)heap_caps_calloc(1, sizeof(Tables), MALLOC_CAP_SPIRAM);
    bool ok = s_pix && s_box && T;
    for (int b = 0; ok && b < NBAND_TEX; ++b) {
        s_dayBand[b]   = (uint16_t *)heap_caps_malloc((size_t)TEX_W * BAND * 2, MALLOC_CAP_SPIRAM);
        s_nightBand[b] = (uint8_t *)heap_caps_malloc((size_t)TEX_W * BAND, MALLOC_CAP_SPIRAM);
        ok = s_dayBand[b] && s_nightBand[b];
    }
    if (ok) ok = build_tables();
    if (!ok) {
        release_all();
        diag::log("sat: no PSRAM");
        show_msg(tr("Not enough memory", "Nicht genug Speicher"));
        return;
    }
    if (!load_jpg("/earth/day.jpg", T_DAY) || !load_jpg("/earth/night.jpg", T_NIGHT)) {
        release_all();
        lv_label_set_text(s_l1, ""); lv_label_set_text(s_l2, ""); lv_label_set_text(s_l3, "");
        show_msg(tr("Earth pictures missing\n/earth/day.jpg, night.jpg\non the SD card",
                    "Erdbilder fehlen\n/earth/day.jpg, night.jpg\nauf der SD-Karte"));
        return;
    }
    paint_background();
    s_canvas = lv_canvas_create(s_scr);
    lv_canvas_set_buffer(s_canvas, s_pix, SCR, SCR, LV_IMG_CF_TRUE_COLOR);
    lv_obj_center(s_canvas);
    lv_obj_move_background(s_canvas);

    s_lutInUse = -1; s_drawnSeq = 0; s_dirty = true;
    portENTER_CRITICAL(&s_mux);
    S->pub = Pub(); S->pub.sat = -2;       // nothing yet; the worker starts over
    portEXIT_CRITICAL(&s_mux);
    S->view = Pub();
    s_active = true;
    if (s_task) xTaskNotifyGive(s_task);
    set_texts((uint32_t)time(nullptr));
    Serial.printf("[sat] ready in %lu ms\n", (unsigned long)(millis() - t0));
}

void onExit() {
    if (!S) return;
    s_active = false;
    // The worker checks s_active between rows and orbits; give it a moment to step out
    // before its tables go.
    for (int i = 0; i < 100 && s_busy; ++i) vTaskDelay(pdMS_TO_TICKS(5));
    // It starts over on the next entry: the projection belongs to buffers about to be freed.
    s_wSat = -1;
    portENTER_CRITICAL(&s_mux);
    S->w = Pub();
    portEXIT_CRITICAL(&s_mux);
    release_all();
}

void onTurn(int delta) {
    if (!delta) return;
    s_sat = ((s_sat + (delta > 0 ? 1 : -1)) % NSAT + NSAT) % NSAT;
    s_dirty = true;
    if (s_task) xTaskNotifyGive(s_task);
    if (s_active) set_texts((uint32_t)time(nullptr));
}

void setShown(bool shown) { s_shown = shown; }

bool tleWanted() {
    if (!S) return false;
    if (!s_shown || (int32_t)(millis() - s_nextTryMs) < 0) return false;
    const uint32_t now = (uint32_t)time(nullptr);
    if (now < 1700000000u) return false;
    for (int i = 0; i < NTLE; ++i) if (!s_tleHave[i]) return true;
    return now - s_tleAt > SAT_TLE_MAX_AGE_S;
}

void fetchTle() {
    if (!S) return;
    Tle got[NTLE]; bool have[NTLE] = {}; int n = 0;
    for (int i = 0; i < NTLE; ++i) {
        char url[96]; snprintf(url, sizeof(url), "%s%lu", SAT_TLE_URL, (unsigned long)SATS[i].catnr);
        uint8_t *body = nullptr; size_t len = 0;
        if (net_fetch_psram(url, ADSB_USER_AGENT, &body, &len, 2048, 5000, 10000)) {
            have[i] = parse_tle((const char *)body, len, got[i]);
            heap_caps_free(body);
        }
        if (have[i]) ++n;
        else Serial.printf("[sat] no elements for %s\n", SATS[i].name);
    }
    if (!n) { s_nextTryMs = millis() + SAT_TLE_RETRY_MS; return; }
    Preferences p;
    p.begin("capsuleradar", false);
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < NTLE; ++i) if (have[i]) { S->tle[i] = got[i]; s_tleHave[i] = true; }
    portEXIT_CRITICAL(&s_mux);
    for (int i = 0; i < NTLE; ++i) if (have[i]) {
        char key[8], val[150];
        snprintf(key, sizeof(key), "tle%d", i);
        snprintf(val, sizeof(val), "%s\n%s", got[i].l1, got[i].l2);
        p.putString(key, val);
    }
    s_tleAt = (uint32_t)time(nullptr);
    p.putUInt("tleAt", s_tleAt);
    p.end();
    s_tleGen = s_tleGen + 1;
    if (n < NTLE) s_nextTryMs = millis() + SAT_TLE_RETRY_MS;   // fill in the missing one later
    Serial.printf("[sat] orbital elements updated (%d of %d)\n", n, NTLE);
}

}  // namespace satview

#else
namespace satview {
void init() {}
lv_obj_t *screen() { return nullptr; }
void onEnter(double, double, const char *) {}
void onExit() {}
void onTurn(int) {}
void setShown(bool) {}
bool tleWanted() { return false; }
void fetchTle() {}
}
#endif
