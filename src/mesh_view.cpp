#include "mesh_view.h"
#include "config.h"
#include "lang.h"

#if MESH_ENABLED

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <algorithm>
#include <math.h>
#include <new>
#include <time.h>
#include "diag_log.h"
#include "net_fetch.h"
#include "roads_sd.h"
#include "settings_sprite.h"

LV_FONT_DECLARE(font_sat_24b);   // Sora, shared with the Satellites screen
LV_FONT_DECLARE(font_sat_18);
LV_FONT_DECLARE(font_sat_16b);
LV_FONT_DECLARE(font_sat_14);
LV_FONT_DECLARE(font_sat_12);

// How it is put together.
//
// The feed task fetches two small JSON documents from the analyzer (detail, 29 KB, and
// reach, 4 KB) every MESH_REFRESH_MS while this page is open, keeps only the fields shown,
// and publishes them under s_mux. Nothing about the network runs on the LVGL thread.
//
// The LVGL side draws in two layers. The base is drawn once per node position: the active
// theme's Settings plate, the weather map's roads from the SD card (roads_sd::project_flat,
// the same tiles and colour) and the distance rings. Every frame copies the base and draws
// the moving part over it: the links, the light running along them and the chosen neighbour.
// Both layers are PSRAM and given back on exit.
//
// The map is linear, MESH_RANGE_KM to the rim, so the roads and the neighbours on them agree;
// a neighbour further out sits on the rim with its line still pointing the right way.

namespace {

constexpr int SCR = SCREEN_W;
constexpr int CX = 233, CY = 222, RIM = 204;          // the repeater, and the map's edge
constexpr float PX_PER_KM = (float)RIM / MESH_RANGE_KM;
constexpr int MAX_NB = 24;
constexpr uint32_t FRAME_MS = 80;                     // about 12 frames a second for the light
constexpr float TAU = 6.2831853f, RAD = 0.017453293f;

const lv_color_t C_AMBER = LV_COLOR_MAKE(0xF5, 0xB3, 0x42);
const lv_color_t C_INK   = LV_COLOR_MAKE(0xF2, 0xF0, 0xEA);
const lv_color_t C_MUTED = LV_COLOR_MAKE(0xA8, 0xA3, 0x99);
const lv_color_t C_ROAD  = LV_COLOR_MAKE(0x4B, 0x44, 0x3D);   // the weather map's road colour
const lv_color_t C_LIGHT = LV_COLOR_MAKE(0xFF, 0xE2, 0xA3);

// ---- the data -------------------------------------------------------------------------------
struct Nb { char name[32]; float lat, lon, km; uint16_t we, they; bool two, low; };
struct Data {
    bool     ok = false;
    char     name[40] = "";
    float    lat = 0, lon = 0;
    uint32_t lastSeen = 0;           // unix
    uint32_t relay1h = 0, relay24h = 0;
    int      bestSnr = 0;
    int      n = 0;
    Nb       nb[MAX_NB];
};
struct Shared {
    Data     pub;                    // published, under s_mux
    Data     work;                   // the feed task's
    Data     view;                   // the LVGL side's copy
};
Shared *S = nullptr;
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t s_seq = 0;
char      s_id[80] = "";             // the node, 64 hex characters
volatile uint32_t s_idGen = 0;       // bumped when the node changes
volatile bool s_shown = true, s_active = false;
uint32_t  s_fetchedAt = 0, s_nextTryMs = 0, s_fetchedGen = 0;
volatile int s_err = 0;              // last fetch: 0 fine, else an HTTP status or -1

// ---- the LVGL side --------------------------------------------------------------------------
lv_obj_t   *s_scr = nullptr, *s_canvas = nullptr;
lv_obj_t   *s_title = nullptr, *s_sub = nullptr, *s_l1 = nullptr, *s_l2 = nullptr, *s_l3 = nullptr, *s_l4 = nullptr;
lv_obj_t   *s_ring[2] = {}, *s_names[3] = {};
lv_timer_t *s_timer = nullptr;
lv_color_t *s_pix = nullptr, *s_base = nullptr;
float       s_baseLat = 999, s_baseLon = 999;
uint32_t    s_viewSeq = 0, s_lastTurn = 0, s_textAt = 0;
int         s_sel = -1;                               // the chosen neighbour, -1 = overview

struct Pt { float x, y, b, len; int flow; };          // per neighbour, in compass order
Pt  s_pt[MAX_NB];
int s_order[MAX_NB];                                  // indexes into view.nb, clockwise from north
int s_maxFlow = 1;

// ---- small helpers -----------------------------------------------------------------------------
uint32_t parse_iso(const char *s) {   // "2026-10-11T03:00:54Z" (fraction ignored) -> unix
    int y, mo, d, h, mi, se;
    if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
    y -= mo <= 2;                                      // days from civil (H. Hinnant)
    const int era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    const int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long days = (long)era * 146097 + doe - 719468;
    return (uint32_t)(days * 86400L + h * 3600 + mi * 60 + se);
}

void thousands(char *out, size_t n, long v) {
    char tmp[24]; snprintf(tmp, sizeof(tmp), "%ld", v);
    const int len = strlen(tmp); size_t o = 0;
    for (int i = 0; i < len && o + 1 < n; ++i) {
        out[o++] = tmp[i];
        if ((len - i - 1) % 3 == 0 && i < len - 1 && o + 1 < n) out[o++] = lang::de() ? '.' : ',';
    }
    out[o] = 0;
}

// "DE-NW-MK-Hemer-DH" -> "HEMER DH": the country, state and district codes say nothing on a
// map of one valley. Drops a leading DE/D, then NW/NRW, then one more short all-capitals code.
void short_name(char *out, size_t n, const char *in) {
    char buf[40]; snprintf(buf, sizeof(buf), "%s", in);
    char *tok[12]; int nt = 0;
    for (char *p = strtok(buf, "-_ "); p && nt < 12; p = strtok(nullptr, "-_ ")) tok[nt++] = p;
    auto caps = [](const char *t) { int l = strlen(t); if (l < 2 || l > 3) return false;
                                    for (int i = 0; i < l; ++i) if (t[i] < 'A' || t[i] > 'Z') return false; return true; };
    int i = 0;
    if (i < nt - 1 && (!strcmp(tok[i], "DE") || !strcmp(tok[i], "D"))) ++i;
    if (i < nt - 1 && (!strcmp(tok[i], "NW") || !strcmp(tok[i], "NRW"))) ++i;
    if (i < nt - 2 && caps(tok[i])) ++i;
    size_t o = 0;
    for (; i < nt && o + 1 < n; ++i) {
        if (o && o + 1 < n) out[o++] = ' ';
        for (const char *p = tok[i]; *p && o + 1 < n; ++p) out[o++] = (*p >= 'a' && *p <= 'z') ? *p - 32 : *p;
    }
    out[o] = 0;
}

float bearing(float lat1, float lon1, float lat2, float lon2) {
    const float p1 = lat1 * RAD, p2 = lat2 * RAD, dl = (lon2 - lon1) * RAD;
    float b = atan2f(sinf(dl) * cosf(p2), cosf(p1) * sinf(p2) - sinf(p1) * cosf(p2) * cosf(dl));
    return b < 0 ? b + TAU : b;
}

const char *dir_name(float b) {
    static const char *EN[8] = { "north", "northeast", "east", "southeast", "south", "southwest", "west", "northwest" };
    static const char *DE[8] = { "Nord", "Nordost", "Ost", "Südost", "Süd", "Südwest", "West", "Nordwest" };
    const int i = (int)lroundf(b / (TAU / 8)) % 8;
    return lang::de() ? DE[i] : EN[i];
}

// ---- the feed task's side ----------------------------------------------------------------------
// One GET, tried twice: the analyzer's TLS connections fail now and then (connection lost
// part way, measured on a real Orb), and a second try a second later usually goes through.
bool get_json(const char *path, JsonDocument &doc, JsonDocument &filter) {
    char url[200];
    snprintf(url, sizeof(url), "%s%s/%s", MESH_API_BASE, s_id, path);
    uint8_t *body = nullptr; size_t len = 0;
    bool got = false;
    NetTlsPermit tls;                                  // the only caller that is given TLS
    for (int attempt = 0; attempt < 2 && !got; ++attempt) {
        if (attempt) vTaskDelay(pdMS_TO_TICKS(1000));
        got = net_fetch_psram(url, ADSB_USER_AGENT, &body, &len, 96 * 1024, 6000, 20000);
    }
    if (!got) { s_err = -1; return false; }
    const DeserializationError e = deserializeJson(doc, (const char *)body, len, DeserializationOption::Filter(filter));
    heap_caps_free(body);
    if (e) { Serial.printf("[mesh] %s: %s\n", path, e.c_str()); s_err = -2; return false; }
    return true;
}

// ---- drawing ---------------------------------------------------------------------------------
inline void blend(lv_color_t *buf, int x, int y, lv_color_t c, uint8_t a) {
    if ((unsigned)x >= (unsigned)SCR || (unsigned)y >= (unsigned)SCR || !a) return;
    lv_color_t &d = buf[y * SCR + x];
    d = lv_color_mix(c, d, a);
}

void disc(lv_color_t *buf, float cx, float cy, float r, lv_color_t c, uint8_t a) {
    for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); ++y)
        for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); ++x) {
            const float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
            if (d <= r) blend(buf, x, y, c, a);
            else if (d < r + 1) blend(buf, x, y, c, (uint8_t)(a * (r + 1 - d)));
        }
}

void ring(lv_color_t *buf, float cx, float cy, float r, lv_color_t c, uint8_t a) {
    const int n = (int)(TAU * r);
    for (int i = 0; i < n; ++i) { const float t = TAU * i / n; blend(buf, (int)lroundf(cx + r * cosf(t)), (int)lroundf(cy + r * sinf(t)), c, a); }
}

// A line `w` pixels wide, solid or dashed (on/off in pixels), clipped to the map's disc.
void line(lv_color_t *buf, float x0, float y0, float x1, float y1, float w, lv_color_t c, uint8_t a, int on = 0, int off = 0) {
    const float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
    if (len < 0.5f) return;
    const float nx = -dy / len, ny = dx / len;
    const int steps = (int)ceilf(len);
    for (int i = 0; i <= steps; ++i) {
        if (on && fmodf((float)i, (float)(on + off)) >= on) continue;
        const float t = (float)i / steps, px = x0 + dx * t, py = y0 + dy * t;
        for (float o = -(w - 1) / 2; o <= (w - 1) / 2 + 0.01f; o += 1.0f) {
            const int x = (int)lroundf(px + nx * o), y = (int)lroundf(py + ny * o);
            if ((x - 233) * (x - 233) + (y - 233) * (y - 233) > RIM * RIM) continue;
            blend(buf, x, y, c, a);
        }
    }
}

// The base: plate, roads, rings. Redrawn only when the node's position changes.
void build_base(float lat, float lon) {
    const lv_img_dsc_t *plate = settings_custom_plate();
    if (plate && plate->header.w == SCR && plate->header.h == SCR && plate->data)
        memcpy(s_base, plate->data, (size_t)SCR * SCR * sizeof(lv_color_t));
    else for (int i = 0; i < SCR * SCR; ++i) s_base[i] = lv_color_make(10, 9, 8);
    settings_sprite_release();     // Settings decodes its own again when it opens
    if (lat != 0 || lon != 0) {
        constexpr size_t MAX_PTS = 24000, MAX_POLYS = 12000;
        lv_point_t *pts = (lv_point_t *)heap_caps_malloc(MAX_PTS * sizeof(lv_point_t), MALLOC_CAP_SPIRAM);
        uint16_t *lens = (uint16_t *)heap_caps_malloc(MAX_POLYS * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        bool ok = false; size_t polys = 0;
        if (pts && lens) polys = roads_sd::project_flat(lat, lon, MESH_RANGE_KM, CX, CY, RIM, pts, MAX_PTS, lens, MAX_POLYS, ok);
        size_t k = 0;
        for (size_t p = 0; p < polys; ++p) {
            for (uint16_t i = 1; i < lens[p]; ++i)
                line(s_base, pts[k + i - 1].x, pts[k + i - 1].y, pts[k + i].x, pts[k + i].y, 1.4f, C_ROAD, 255);
            k += lens[p];
        }
        heap_caps_free(pts); heap_caps_free(lens);
        Serial.printf("[mesh] map: %u road polylines\n", (unsigned)polys);
        s_baseLat = lat; s_baseLon = lon;
    }
    for (int km : { 10, 20, 30 }) ring(s_base, CX, CY, km * PX_PER_KM, C_AMBER, 40);
}

// Where everything goes for the current data: bearing, distance on the map, traffic.
void layout() {
    const Data &v = S->view;
    s_maxFlow = 1;
    for (int i = 0; i < v.n; ++i) {
        const Nb &n = v.nb[i];
        Pt &p = s_pt[i];
        p.b = bearing(v.lat, v.lon, n.lat, n.lon);
        p.len = fminf(n.km * PX_PER_KM, RIM - 4);
        p.x = CX + p.len * sinf(p.b); p.y = CY - p.len * cosf(p.b);
        p.flow = n.two ? (n.we < n.they ? n.we : n.they) : (n.we > n.they ? n.we : n.they);
        if (p.flow > s_maxFlow) s_maxFlow = p.flow;
        s_order[i] = i;
    }
    for (int i = 1; i < v.n; ++i)                      // compass order, for the knob
        for (int j = i; j > 0 && s_pt[s_order[j]].b < s_pt[s_order[j - 1]].b; --j) { int t = s_order[j]; s_order[j] = s_order[j - 1]; s_order[j - 1] = t; }
}

void draw_link(int i, uint8_t alpha, bool chosen) {
    const Nb &n = S->view.nb[i]; const Pt &p = s_pt[i];
    if (n.low) alpha /= 2;
    const float w = n.two ? 1.0f + 2.5f * sqrtf((float)p.flow / s_maxFlow) : 1.0f;
    if (chosen) line(s_pix, CX, CY, p.x, p.y, w + 4, C_AMBER, 50);          // a soft glow under it
    line(s_pix, CX, CY, p.x, p.y, w + (chosen ? 1 : 0), n.two ? C_AMBER : C_AMBER, n.two ? alpha : alpha * 2 / 5, n.two ? 0 : 3, n.two ? 0 : 4);
    disc(s_pix, p.x, p.y, chosen ? 4.5f : 3.2f, n.two ? C_INK : C_MUTED, alpha);
    if (chosen) ring(s_pix, p.x, p.y, 8, C_INK, 220);
}

// Light along a link: inward as often as the repeater hears the neighbour, outward as often as
// the neighbour hears it; a one-way link pulses one way only.
void draw_pulses(int i, uint32_t now, uint8_t alpha) {
    const Nb &n = S->view.nb[i]; const Pt &p = s_pt[i];
    const float speed = 46.0f;                         // px per second
    const int counts[2] = { n.we, n.they };
    for (int d = 0; d < 2; ++d) {
        if (!counts[d]) continue;
        const int k = 1 + (int)lroundf(3 * sqrtf((float)counts[d] / s_maxFlow));
        const float period = p.len / speed;
        for (int j = 0; j < k; ++j) {
            float t = fmodf(now / 1000.0f / period + (float)j / k + (d == 0 ? 0.5f / k : 0), 1.0f);
            if (d == 0) t = 1 - t;                     // inward
            const float x = CX + (p.x - CX) * t, y = CY + (p.y - CY) * t, a = alpha * sinf(3.14159f * t);
            disc(s_pix, x, y, 3.5f, C_LIGHT, (uint8_t)(a * 0.25f));
            disc(s_pix, x, y, 1.8f, C_LIGHT, (uint8_t)(a * 0.95f));
        }
    }
}

// Keeps a name wholly inside the dial. A neighbour beyond the rim is drawn on the rim, and its
// name then hung past the glass, cut in half by the bezel (seen on the owner's Orb, 2026-10-11).
// The box is pulled in along its own row until all four corners are within NAME_R of the centre.
constexpr float NAME_R = 200.0f;   // 10 px inside the Tagesring minute ring (radius 214)

void keep_inside(lv_obj_t *o) {
    lv_obj_update_layout(o);
    const int w = lv_obj_get_width(o), h = lv_obj_get_height(o);
    int x = lv_obj_get_x(o), y = lv_obj_get_y(o);
    const int c = SCR / 2;
    y = LV_CLAMP(c - (int)NAME_R + 2, y, c + (int)NAME_R - 2 - h);
    const int far = std::max(abs(y - c), abs(y + h - c));          // the row edge farther from the centre
    const int half = (int)sqrtf(std::max(0.0f, NAME_R * NAME_R - (float)far * far));
    x = LV_CLAMP(c - half, x, c + half - w);
    lv_obj_set_pos(o, x, y);
}

void place_name(int slot, int i) {
    char nm[32]; short_name(nm, sizeof(nm), S->view.nb[i].name);
    lv_label_set_text(s_names[slot], nm);
    const Pt &p = s_pt[i];
    lv_point_t sz; lv_txt_get_size(&sz, nm, &font_sat_12, 1, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const bool right = sinf(p.b) >= 0;
    const int x = (int)(p.x + 9 * sinf(p.b)) + (right ? 3 : -3 - sz.x);
    const int y = (int)(p.y - 9 * cosf(p.b)) - sz.y / 2;
    lv_obj_set_pos(s_names[slot], x, y);
    keep_inside(s_names[slot]);
    lv_obj_clear_flag(s_names[slot], LV_OBJ_FLAG_HIDDEN);
}

void set_texts(uint32_t now) {
    const Data &v = S->view;
    char a[64] = "", b[64] = "", c[64] = "", d[64] = "", t1[24], t2[24];
    if (!s_id[0]) {
        snprintf(b, sizeof(b), "%s", tr("NO NODE CHOSEN", "KEIN KNOTEN GEWÄHLT"));
        snprintf(c, sizeof(c), "%s", tr("set it at", "festlegen unter"));
        snprintf(d, sizeof(d), "theorb.local/meshcore");
    } else if (!v.ok) {
        snprintf(b, sizeof(b), "%s", s_err ? tr("ANALYZER NOT ANSWERING", "ANALYZER ANTWORTET NICHT") : tr("LOADING ...", "LADE DATEN ..."));
    } else if (s_sel >= 0) {
        const int i = s_order[s_sel]; const Nb &n = v.nb[i];
        short_name(a, sizeof(a), n.name);
        char km[16]; snprintf(km, sizeof(km), "%.1f", n.km); if (lang::de()) for (char *p = km; *p; ++p) if (*p == '.') *p = ',';
        snprintf(b, sizeof(b), "%s km · %s", km, dir_name(s_pt[i].b));
        thousands(t1, sizeof(t1), n.we); thousands(t2, sizeof(t2), n.they);
        snprintf(c, sizeof(c), tr("you hear it %s× · it hears you %s×", "du hörst ihn %s× · er hört dich %s×"), t1, t2);
        snprintf(d, sizeof(d), "%s%s", n.two ? tr("a two-way link", "in beide Richtungen") : tr("one way only", "nur in eine Richtung"),
                 n.low ? tr(" · uncertain", " · unsicher") : "");
    } else {
        thousands(t1, sizeof(t1), v.relay24h);
        snprintf(a, sizeof(a), tr("%s relayed · 24 h", "%s weitergeleitet · 24 h"), t1);
        thousands(t2, sizeof(t2), v.relay1h);
        snprintf(b, sizeof(b), tr("%s IN THE LAST HOUR", "%s IN DER LETZTEN STUNDE"), t2);
        // The best SNR line went at the owner's request; "last heard" takes its place a size up.
        if (v.lastSeen) {
            time_t ls = v.lastSeen; struct tm tm; localtime_r(&ls, &tm);
            snprintf(c, sizeof(c), tr("last heard %02d:%02d", "zuletzt gemeldet %02d:%02d"), tm.tm_hour, tm.tm_min);
        }
    }
    const bool overview = s_id[0] && v.ok && s_sel < 0;
    lv_obj_set_style_text_font(s_l3, overview ? &font_sat_18 : &font_sat_14, 0);
    lv_label_set_text(s_l1, a); lv_label_set_text(s_l2, b); lv_label_set_text(s_l3, c); lv_label_set_text(s_l4, d);
    if (v.ok) {
        char up[40]; snprintf(up, sizeof(up), "%s", v.name);
        for (char *p = up; *p; ++p) if (*p >= 'a' && *p <= 'z') *p -= 32;
        lv_label_set_text(s_sub, up);
    } else lv_label_set_text(s_sub, "");
    s_textAt = now;
}

void frame(uint32_t now) {
    memcpy(s_pix, s_base, (size_t)SCR * SCR * sizeof(lv_color_t));
    const Data &v = S->view;
    for (auto &l : s_names) lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    if (v.ok) {
        const int chosen = s_sel >= 0 ? s_order[s_sel] : -1;
        for (int pass = 0; pass < 2; ++pass)           // one-way links under the two-way ones
            for (int i = 0; i < v.n; ++i) if ((int)v.nb[i].two == pass && i != chosen) draw_link(i, chosen >= 0 ? 56 : 255, false);
        for (int i = 0; i < v.n; ++i) if (i != chosen) draw_pulses(i, now, chosen >= 0 ? 40 : 255);
        if (chosen >= 0) {
            draw_link(chosen, 255, true); draw_pulses(chosen, now, 255); place_name(0, chosen);
            for (int i = 0; i < v.n; ++i) {             // where in the list this one is
                const float a = 1.5708f + (i - (v.n - 1) / 2.0f) * 0.055f;
                disc(s_pix, 233 + 194 * cosf(a), 233 + 194 * sinf(a), i == s_sel ? 3.0f : 1.8f, i == s_sel ? C_AMBER : C_INK, i == s_sel ? 255 : 100);
            }
        } else {
            // the three busiest two-way links by name, never on top of each other
            int top[3] = { -1, -1, -1 };
            for (int i = 0; i < v.n; ++i) {
                if (!v.nb[i].two) continue;
                for (int k = 0; k < 3; ++k) if (top[k] < 0 || s_pt[i].flow > s_pt[top[k]].flow) { for (int m = 2; m > k; --m) top[m] = top[m - 1]; top[k] = i; break; }
            }
            int placed[3]; int np = 0;
            for (int k = 0; k < 3; ++k) {
                if (top[k] < 0) continue;
                place_name(k, top[k]);
                lv_coord_t y = lv_obj_get_y(s_names[k]);
                for (int guard = 0; guard < 4; ++guard) {
                    bool clash = false;
                    for (int m = 0; m < np; ++m) if (abs(placed[m] - y) < 13) clash = true;
                    if (!clash) break;
                    y += cosf(s_pt[top[k]].b) >= 0 ? -13 : 13;
                }
                lv_obj_set_y(s_names[k], y); keep_inside(s_names[k]); placed[np++] = lv_obj_get_y(s_names[k]);
            }
        }
    }
    disc(s_pix, CX, CY, 5, C_AMBER, 255);
    ring(s_pix, CX, CY, 9, C_AMBER, 255);
    lv_obj_invalidate(s_canvas);
}

void tick_cb(lv_timer_t *) {
    if (!s_active || !s_pix) return;
    const uint32_t now = millis();
    bool fresh = false;
    portENTER_CRITICAL(&s_mux);
    if (s_seq != s_viewSeq) { S->view = S->pub; s_viewSeq = s_seq; fresh = true; }
    portEXIT_CRITICAL(&s_mux);
    if (fresh) {
        layout();
        if (s_sel >= S->view.n) s_sel = -1;
        if (S->view.ok && (fabsf(S->view.lat - s_baseLat) > 0.001f || fabsf(S->view.lon - s_baseLon) > 0.001f)) build_base(S->view.lat, S->view.lon);
        set_texts(now);
    }
    if (s_sel >= 0 && now - s_lastTurn > MESH_IDLE_MS) { s_sel = -1; set_texts(now); }   // back to the whole picture
    if (now - s_textAt > 30000) set_texts(now);
    frame(now);
}

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

void release_all() {
    if (s_canvas) { lv_obj_del(s_canvas); s_canvas = nullptr; }
    if (s_pix)  { heap_caps_free(s_pix);  s_pix = nullptr; }
    if (s_base) { heap_caps_free(s_base); s_base = nullptr; }
    s_baseLat = s_baseLon = 999;
}

// The words and labels exist only while the page is open. LVGL keeps every allocation under
// 2 KB in internal RAM (lv_psram_alloc.h), and these fifteen objects held about 3 KB of it for
// the life of the device, out of the pool the WiFi driver receives into. Built on entry,
// deleted on exit.
void build_labels() {
    // Positions and widths were measured with the Sora glyphs against the Tagesring dial:
    // nothing comes within 10 px of its minute ring (radius 214), in either language.
    s_title = label(&font_sat_24b, C_AMBER, 3, 60, 300);
    lv_label_set_text(s_title, "MESHCORE");
    s_sub = label(&font_sat_16b, C_INK, 2, 90, 290);   // the repeater's name, a size up at the owner's request
    s_l1 = label(&font_sat_18, C_INK, 0, 324, 330);
    s_l2 = label(&font_sat_16b, C_AMBER, 1, 348, 314);
    s_l3 = label(&font_sat_14, C_MUTED, 0, 369, 278);
    s_l4 = label(&font_sat_14, C_MUTED, 0, 387, 224);
    const int ringKm[2] = { 10, 20 };
    for (int i = 0; i < 2; ++i) {
        s_ring[i] = lv_label_create(s_scr);
        lv_obj_set_style_text_font(s_ring[i], &font_sat_12, 0);
        lv_obj_set_style_text_color(s_ring[i], C_MUTED, 0);
        char t[12]; snprintf(t, sizeof(t), "%d km", ringKm[i]);
        lv_label_set_text(s_ring[i], t);
        lv_obj_set_pos(s_ring[i], CX + 4, (lv_coord_t)(CY - ringKm[i] * PX_PER_KM + 1));
    }
    for (auto &n : s_names) {
        n = lv_label_create(s_scr);
        lv_obj_set_style_text_font(n, &font_sat_12, 0);
        lv_obj_set_style_text_color(n, C_INK, 0);
        lv_obj_set_style_text_letter_space(n, 1, 0);
        lv_obj_add_flag(n, LV_OBJ_FLAG_HIDDEN);
    }
}

void delete_labels() {
    for (lv_obj_t **o : { &s_title, &s_sub, &s_l1, &s_l2, &s_l3, &s_l4, &s_ring[0], &s_ring[1], &s_names[0], &s_names[1], &s_names[2] })
        if (*o) { lv_obj_del(*o); *o = nullptr; }
}

}  // namespace

namespace meshview {

void init() {
    void *mem = heap_caps_calloc(1, sizeof(Shared), MALLOC_CAP_SPIRAM);
    if (mem) S = new (mem) Shared();
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    s_timer = lv_timer_create(tick_cb, FRAME_MS, nullptr);
}

lv_obj_t *screen() { return s_scr; }

void onEnter() {
    if (!S) return;
    s_pix  = (lv_color_t *)heap_caps_malloc((size_t)SCR * SCR * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_base = (lv_color_t *)heap_caps_malloc((size_t)SCR * SCR * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    if (!s_pix || !s_base) { release_all(); diag::log("mesh: no PSRAM"); return; }
    s_canvas = lv_canvas_create(s_scr);
    lv_canvas_set_buffer(s_canvas, s_pix, SCR, SCR, LV_IMG_CF_TRUE_COLOR);
    lv_obj_center(s_canvas);
    lv_obj_move_background(s_canvas);
    build_labels();
    s_sel = -1; s_viewSeq = 0;
    portENTER_CRITICAL(&s_mux);
    S->view = S->pub; s_viewSeq = s_seq;
    portEXIT_CRITICAL(&s_mux);
    layout();
    build_base(S->view.ok ? S->view.lat : 0, S->view.ok ? S->view.lon : 0);
    s_active = true;
    set_texts(millis());
    frame(millis());
}

void onExit() {
    s_active = false;
    delete_labels();
    release_all();
}

void onTurn(int delta) {
    if (!S || !s_active || !S->view.ok || !S->view.n || !delta) return;
    const int n = S->view.n, d = delta > 0 ? 1 : -1;
    s_sel = s_sel < 0 ? (d > 0 ? 0 : n - 1) : ((s_sel + d) % n + n) % n;
    s_lastTurn = millis();
    set_texts(s_lastTurn);
}

void setShown(bool shown) { s_shown = shown; }

void setNodeId(const char *id) {
    snprintf(s_id, sizeof(s_id), "%s", id ? id : "");
    s_idGen = s_idGen + 1;
    s_nextTryMs = 0;
    if (S) {                                           // the old node's figures are not this one's
        portENTER_CRITICAL(&s_mux);
        S->pub = Data(); s_seq = s_seq + 1;
        portEXIT_CRITICAL(&s_mux);
    }
}

bool fetchWanted() {
    if (!S || !s_shown || !s_active || !s_id[0]) return false;
    if ((int32_t)(millis() - s_nextTryMs) < 0) return false;
    return s_fetchedGen != s_idGen || !s_fetchedAt || millis() - s_fetchedAt > MESH_REFRESH_MS;
}

void fetch() {
    if (!S) return;
    const uint32_t gen = s_idGen;
    Data &w = S->work;
    w = Data();
    {
        JsonDocument filter;
        filter["node"]["name"] = true; filter["node"]["lat"] = true; filter["node"]["lon"] = true; filter["node"]["lastSeen"] = true;
        filter["stat"]["relayCount1h"] = true; filter["stat"]["relayCount24h"] = true; filter["stat"]["bestSnr"] = true;
        JsonDocument doc;
        if (!get_json("detail?hours=24", doc, filter)) { s_nextTryMs = millis() + MESH_RETRY_MS; return; }
        snprintf(w.name, sizeof(w.name), "%s", doc["node"]["name"] | "");
        w.lat = doc["node"]["lat"] | 0.0f; w.lon = doc["node"]["lon"] | 0.0f;
        w.lastSeen = parse_iso(doc["node"]["lastSeen"] | "");
        w.relay1h = doc["stat"]["relayCount1h"] | 0; w.relay24h = doc["stat"]["relayCount24h"] | 0;
        w.bestSnr = (int)lroundf(doc["stat"]["bestSnr"] | 0.0f);
    }
    {
        JsonDocument filter;
        JsonObject f = filter["neighbourList"].add<JsonObject>();
        for (const char *k : { "name", "lat", "lon", "distanceKm", "weHear", "theyHear", "twoWay", "confidence" }) f[k] = true;
        JsonDocument doc;
        // Without the neighbour list the figures are still worth showing: publish what there
        // is, keep the last list if there was one, and ask again soon.
        if (!get_json("reach?days=7", doc, filter)) {
            portENTER_CRITICAL(&s_mux);
            w.n = S->pub.n; memcpy(w.nb, S->pub.nb, sizeof(w.nb));
            portEXIT_CRITICAL(&s_mux);
            s_nextTryMs = millis() + MESH_RETRY_MS;
        } else for (JsonObject o : doc["neighbourList"].as<JsonArray>()) {
            if (w.n >= MAX_NB) break;
            Nb &n = w.nb[w.n];
            snprintf(n.name, sizeof(n.name), "%s", o["name"] | "?");
            n.lat = o["lat"] | 0.0f; n.lon = o["lon"] | 0.0f; n.km = o["distanceKm"] | 0.0f;
            n.we = (uint16_t)min(65535, (int)(o["weHear"] | 0)); n.they = (uint16_t)min(65535, (int)(o["theyHear"] | 0));
            n.two = o["twoWay"] | false;
            n.low = !strcmp(o["confidence"] | "", "low");
            if (n.lat != 0 || n.lon != 0) ++w.n;       // a neighbour with no position cannot be placed
        }
    }
    if (gen != s_idGen) return;                        // the node changed while this was in flight
    w.ok = w.name[0] && (w.lat != 0 || w.lon != 0);
    s_err = w.ok ? 0 : -3;
    portENTER_CRITICAL(&s_mux);
    S->pub = w; s_seq = s_seq + 1;
    portEXIT_CRITICAL(&s_mux);
    if ((int32_t)(millis() - s_nextTryMs) >= 0) s_fetchedAt = millis();   // a missing list is asked for again sooner
    s_fetchedGen = gen;
    Serial.printf("[mesh] %s: %u relayed in 24 h, %d neighbours\n", w.name, (unsigned)w.relay24h, w.n);
}

}  // namespace meshview

#else
namespace meshview {
void init() {}
lv_obj_t *screen() { return nullptr; }
void onEnter() {}
void onExit() {}
void onTurn(int) {}
void setShown(bool) {}
void setNodeId(const char *) {}
bool fetchWanted() { return false; }
void fetch() {}
}
#endif
