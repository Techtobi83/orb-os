// Livestream: one network camera, full screen, as a stream of JPEGs.
//
// The source is any plain-HTTP URL that answers with either a multipart MJPEG stream
// (multipart/x-mixed-replace, which is what go2rtc's /api/stream.mjpeg serves) or a single
// JPEG (a snapshot URL, fetched again and again). The owner sets it on the device, in
// Settings > Livestream or at http://theorb.local/livestream; main.cpp keeps it in NVS and
// hands it over through init() and setUrl(). Nothing about a camera is compiled in.
//
// Two threads, one job each:
//   - stream_task (core 0) owns the socket. It is created at boot and sleeps until onEnter
//     wakes it; onExit sends it back to sleep, closing the connection and freeing its
//     buffers on the way, so the TCP buffers in internal RAM (the scarce thing on this
//     board, not PSRAM) are only in use while you are watching.
//   - tick_cb (LVGL, core 1) owns the canvas. It decodes the newest complete frame, if
//     there is one, and never waits on the network.
// They meet at three JPEG buffers in PSRAM. The task always writes into one that is
// neither the newest finished frame nor the one being decoded, so neither side ever waits
// for the other; a slow decode only means some frames are skipped.
//
// Frames must be baseline JPEG: TJpgDec cannot do progressive, and ffmpeg's mjpeg encoder
// only produces baseline. Any size works: the picture is zoomed to fill the round screen
// and what overhangs is cut off (see prepare_for). The best source is one already 466 wide
// or tall, e.g. go2rtc with ffmpeg -vf scale=466:262, so little zooming is needed.
//
// TJpgDec is one global object, shared with photo_client and cloud_image_client. Decoding
// here happens on the LVGL thread, as Spy Cam's did.
#include "livecam_view.h"
#include "lang.h"
#include "config.h"

#if LIVECAM_ENABLED

#include <Arduino.h>
#include <WiFi.h>
#include <TJpg_Decoder.h>
#include <esp_heap_caps.h>
#include "diag_log.h"
#include "plate_sprite.h"
#include "theme_style.h"

namespace {
    enum State : uint8_t { ST_IDLE, ST_CONNECTING, ST_LIVE, ST_ERROR };

    // Error codes in s_lastErr. 100 and up is an HTTP status the server answered with.
    enum : int {
        ERR_NO_WIFI     = -1,
        ERR_CONNECT     = -2,   // nothing answered at host:port
        ERR_WRITE       = -3,
        ERR_NO_ANSWER   = -4,   // connected, but no status line in time
        ERR_BAD_ANSWER  = -5,   // not HTTP
        ERR_SNAPSHOT    = -6,   // single image with no length, too large, or cut short
        ERR_STREAM_LOST = -7,   // no complete frame for LIVECAM_READ_MS
        ERR_NO_LENGTH   = -8,   // a stream part without Content-Length (not supported)
        ERR_NO_PSRAM    = -10,
        ERR_NO_TASK     = -11,  // the stream task could not be created at boot
    };

    // The camera URL. It is set at boot (init) and may change at any time after, from
    // Settings or the web page, on a thread that is not the stream task's. So a new URL is
    // only ever PARKED here (s_urlNext, under s_mux), and the task picks it up between
    // requests; s_host/s_port/s_path belong to the task alone once it runs.
    enum UrlState : uint8_t { URL_NONE, URL_BAD, URL_OK };
    char              s_host[64];
    uint16_t          s_port  = 80;
    char              s_path[LIVECAM_URL_MAX];
    char              s_urlNext[LIVECAM_URL_MAX];
    volatile bool     s_urlChanged = false;
    volatile UrlState s_urlState   = URL_NONE;   // of the newest URL; read by the LVGL side

    lv_obj_t   *s_screen = nullptr;
    lv_obj_t   *s_canvas = nullptr;   // exists only between onEnter and onExit
    lv_color_t *s_pix    = nullptr;   // its buffer, same lifetime
    lv_obj_t   *s_msg    = nullptr;   // "CONNECTING" / "NO SIGNAL", over the last frame

    // Shared between the task and the LVGL side. Indexes and pointers only change under s_mux.
    portMUX_TYPE   s_mux = portMUX_INITIALIZER_UNLOCKED;
    uint8_t       *s_jpg[3]    = { nullptr, nullptr, nullptr };
    uint32_t       s_jpgLen[3] = { 0, 0, 0 };
    int            s_ready     = -1;      // newest complete frame, not yet taken
    int            s_busy      = -1;      // frame the LVGL side is decoding right now
    volatile bool  s_stop      = true;
    volatile State s_state     = ST_IDLE;
    volatile int   s_lastErr   = 0;
    volatile uint32_t s_lastFrameMs = 0;  // millis() of the last frame the task finished reading

    // Task side only: this connection, for the failure log.
    uint32_t s_connFrames = 0;
    uint32_t s_connStart  = 0;

    // LVGL side only.
    bool  s_active   = false;
    State    s_shown    = ST_IDLE;
    int      s_shownErr = 0;
    UrlState s_shownUrl = URL_NONE;

    // Split "http://host[:port][/path]". Returns false, and leaves the outputs alone, for
    // anything else (https included: there is no TLS here).
    bool parse_url(const char *url, char (&host)[64], uint16_t &port, char (&path)[LIVECAM_URL_MAX]) {
        if (strncmp(url, "http://", 7) != 0) return false;
        const char *p       = url + 7;
        const char *slash   = strchr(p, '/');
        const char *hostEnd = slash ? slash : p + strlen(p);
        const char *colon   = (const char *)memchr(p, ':', hostEnd - p);
        const char *nameEnd = colon ? colon : hostEnd;
        const size_t hn = (size_t)(nameEnd - p);
        if (hn == 0 || hn >= sizeof(host)) return false;
        const long pn = colon ? atol(colon + 1) : 80;
        if (pn <= 0 || pn > 65535) return false;
        const char *rest = slash ? slash : "/";
        if (strlen(rest) >= sizeof(path)) return false;
        memcpy(host, p, hn);
        host[hn] = 0;
        port = (uint16_t)pn;
        strcpy(path, rest);
        return true;
    }

    UrlState classify(const char *url) {
        if (!url || !url[0]) return URL_NONE;
        char h[64]; uint16_t pt; char pa[LIVECAM_URL_MAX];
        return parse_url(url, h, pt, pa) ? URL_OK : URL_BAD;
    }

    // I/O gives up as soon as the app is left OR a new URL is waiting.
    bool io_abort() { return s_stop || s_urlChanged; }

    // Task side: take a parked URL, if there is one. Returns true if it changed.
    bool take_new_url() {
        if (!s_urlChanged) return false;
        char next[LIVECAM_URL_MAX];
        portENTER_CRITICAL(&s_mux);
        memcpy(next, s_urlNext, sizeof(next));
        s_urlChanged = false;
        portEXIT_CRITICAL(&s_mux);
        if (!parse_url(next, s_host, s_port, s_path)) s_host[0] = 0;
        return true;
    }

    // ---- stream task (core 0) ----------------------------------------------------------

    // One header line, without Arduino String. Gives up on timeout, on a closed socket, or
    // when onExit asks the task to stop, so leaving the app never waits out a full timeout.
    int read_line(WiFiClient &c, char *out, size_t cap, uint32_t deadline) {
        size_t n = 0;
        while (!io_abort() && (int32_t)(millis() - deadline) < 0) {
            const int ch = c.read();
            if (ch < 0) {
                if (!c.connected()) return -1;
                delay(1);
                continue;
            }
            if (ch == '\n') {
                if (n && out[n - 1] == '\r') --n;
                out[n] = 0;
                return (int)n;
            }
            if (n + 1 < cap) out[n++] = (char)ch;
        }
        return -1;
    }

    // Exactly `len` bytes into dst, or thrown away when dst is null.
    bool read_exact(WiFiClient &c, uint8_t *dst, size_t len, uint32_t deadline) {
        uint8_t sink[256];
        size_t got = 0;
        while (got < len) {
            if (io_abort() || (int32_t)(millis() - deadline) >= 0) return false;
            const int avail = c.available();
            if (avail <= 0) {
                if (!c.connected()) return false;
                delay(2);
                continue;
            }
            size_t want = len - got;
            if ((size_t)avail < want) want = (size_t)avail;
            if (!dst && want > sizeof(sink)) want = sizeof(sink);
            const int n = c.read(dst ? dst + got : sink, want);
            if (n > 0) got += (size_t)n;
        }
        return true;
    }

    // A buffer that is neither the newest finished frame nor the one being decoded. With
    // three buffers there is always one.
    int pick_write_slot() {
        portENTER_CRITICAL(&s_mux);
        int w = 0;
        while (w == s_ready || w == s_busy) ++w;
        portEXIT_CRITICAL(&s_mux);
        return w;
    }

    void publish(int w, uint32_t len) {
        portENTER_CRITICAL(&s_mux);
        s_jpgLen[w] = len;
        s_ready = w;
        portEXIT_CRITICAL(&s_mux);
        s_state = ST_LIVE;
        s_lastFrameMs = millis();
        ++s_connFrames;
    }

    void fail(int err) {
        if (io_abort()) return;   // leaving the app, or a new URL, is not an error
        // Internal RAM is what the TCP stack lives on, and what this board runs short of,
        // so every failure says how much there was.
        Serial.printf("[livecam] failed: %d after %u frames in %u ms, last frame %u ms ago, "
                      "internal free=%u largest=%u, RSSI %d dBm\n",
                      err, (unsigned)s_connFrames, (unsigned)(millis() - s_connStart),
                      s_lastFrameMs ? (unsigned)(millis() - s_lastFrameMs) : 0u,
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                      (int)WiFi.RSSI());
        s_lastErr = err;
        s_state = ST_ERROR;
    }

    // One request on `c`, opening the socket first if it is not open already. Returns true
    // only for a snapshot URL that delivered its image, which is the one case where coming
    // back at once is the plan rather than a retry.
    //
    // Snapshots keep the socket open between requests (HTTP keep-alive). A fresh connection
    // per image would leave one closed socket in TIME_WAIT per frame, and lwIP's table of
    // them is about a dozen; adsb_client.cpp learned that the hard way.
    //
    // Measured on a real Orb against go2rtc (2026-09-29). An MJPEG stream of 640x360 frames
    // at ~6 KB each, which a PC received at a steady 5 fps, reached the Orb at about one
    // frame every two seconds and then stalled for 10 s at a time. The same camera at
    // 466x262, ~3.7 KB a frame and 3 fps, ran for minutes without one failure. Why is not
    // proven; the Orb's TCP receive window is about 5.7 KB, which the first stream's frames
    // just exceeded. Keep frames small. go2rtc's frame.jpeg is no better on its own: with
    // no other viewer it restarts ffmpeg per request and took 1.3 s a frame.
    bool run_request(WiFiClient &c) {
        if (WiFi.status() != WL_CONNECTED) { c.stop(); fail(ERR_NO_WIFI); return false; }
        const bool fresh = !c.connected();
        if (fresh) {
            c.stop();
            s_connFrames = 0;
            s_connStart  = millis();
            if (!c.connect(s_host, s_port, LIVECAM_CONNECT_MS)) { fail(ERR_CONNECT); return false; }
        }

        char req[320];
        const int n = snprintf(req, sizeof(req),
            "GET %s HTTP/1.1\r\n"
            "Host: %s:%u\r\n"
            "User-Agent: %s\r\n"
            "Accept: multipart/x-mixed-replace, image/jpeg\r\n"
            "Connection: keep-alive\r\n\r\n",
            s_path, s_host, (unsigned)s_port, ORB_USER_AGENT);
        if (n <= 0 || n >= (int)sizeof(req) || c.write((const uint8_t *)req, (size_t)n) != (size_t)n) {
            c.stop();
            if (!fresh) return run_request(c);   // the server had closed the idle socket
            fail(ERR_WRITE); return false;
        }

        // The first answer can be slow: go2rtc starts ffmpeg and the camera's RTSP on demand.
        uint32_t deadline = millis() + (fresh ? LIVECAM_ANSWER_MS : LIVECAM_READ_MS);
        char line[160];
        if (read_line(c, line, sizeof(line), deadline) < 0) {
            c.stop();
            if (!fresh && !io_abort()) return run_request(c);   // same, noticed one step later
            fail(ERR_NO_ANSWER); return false;
        }
        int status = 0;
        { const char *sp = strchr(line, ' '); if (sp) status = atoi(sp + 1); }
        if (status <= 0)   { c.stop(); fail(ERR_BAD_ANSWER); return false; }

        bool multipart = false;
        bool keepAlive = true;
        long bodyLen = -1;
        for (;;) {
            const int len = read_line(c, line, sizeof(line), deadline);
            if (len < 0)  { c.stop(); fail(ERR_NO_ANSWER); return false; }
            if (len == 0) break;
            if (!strncasecmp(line, "Content-Type:", 13) && strcasestr(line, "multipart")) multipart = true;
            else if (!strncasecmp(line, "Content-Length:", 15)) bodyLen = atol(line + 15);
            else if (!strncasecmp(line, "Connection:", 11) && strcasestr(line, "close")) keepAlive = false;
        }
        if (status != 200) { c.stop(); fail(status); return false; }

        if (!multipart) {
            // A snapshot URL: one JPEG, and the socket stays open for the next one if allowed.
            const int w = pick_write_slot();
            if (bodyLen <= 0 || bodyLen > LIVECAM_JPEG_MAX ||
                !read_exact(c, s_jpg[w], (size_t)bodyLen, deadline)) {
                c.stop(); fail(ERR_SNAPSHOT); return false;
            }
            if (!keepAlive) c.stop();
            publish(w, (uint32_t)bodyLen);
            return true;
        }

        // A stream: parts of "--boundary / headers / blank line / JPEG", for ever. Only the
        // part's Content-Length matters; the boundary line and the blank line after each
        // image are just lines without one.
        for (;;) {
            // Once frames are flowing, a gap of LIVECAM_STALL_MS means TCP has stalled (see
            // config.h), and a fresh connection is faster than waiting for it to recover.
            deadline = millis() + (s_connFrames ? LIVECAM_STALL_MS : LIVECAM_READ_MS);
            long partLen = -1;
            int  lines   = 0;
            for (;;) {
                const int len = read_line(c, line, sizeof(line), deadline);
                if (len < 0) { c.stop(); fail(ERR_STREAM_LOST); return false; }
                if (len == 0) { if (partLen > 0) break; continue; }
                if (!strncasecmp(line, "Content-Length:", 15)) partLen = atol(line + 15);
                if (++lines > 16) { c.stop(); fail(ERR_NO_LENGTH); return false; }
            }
            if (partLen > LIVECAM_JPEG_MAX) {
                // Too big to keep, but the stream is still good: skip it and read on.
                if (!read_exact(c, nullptr, (size_t)partLen, deadline)) { c.stop(); fail(ERR_STREAM_LOST); return false; }
                Serial.printf("[livecam] skipped a %ld-byte frame (LIVECAM_JPEG_MAX)\n", partLen);
                continue;
            }
            const int w = pick_write_slot();
            if (!read_exact(c, s_jpg[w], (size_t)partLen, deadline)) { c.stop(); fail(ERR_STREAM_LOST); return false; }
            publish(w, (uint32_t)partLen);
        }
    }

    // Sleep in short steps so a stop request is noticed at once.
    void pause_ms(uint32_t ms) {
        const uint32_t until = millis() + ms;
        while (!io_abort() && (int32_t)(millis() - until) < 0) vTaskDelay(pdMS_TO_TICKS(20));
    }

    // Everything between one onEnter and the matching onExit: take the buffers, keep a
    // connection going, give the buffers back.
    void run_session() {
        uint8_t *bufs[3];
        bool ok = true;
        for (int i = 0; i < 3; ++i) {
            bufs[i] = (uint8_t *)heap_caps_malloc(LIVECAM_JPEG_MAX, MALLOC_CAP_SPIRAM);
            if (!bufs[i]) ok = false;
        }
        if (ok) {
            portENTER_CRITICAL(&s_mux);
            for (int i = 0; i < 3; ++i) s_jpg[i] = bufs[i];
            s_ready = -1;
            portEXIT_CRITICAL(&s_mux);

            WiFiClient c;
            while (!s_stop) {
                if (take_new_url()) {
                    c.stop();                    // the old camera's connection
                    s_lastErr = 0;
                    s_state = ST_CONNECTING;
                    if (s_host[0]) Serial.printf("[livecam] URL is now %s:%u%s\n", s_host, (unsigned)s_port, s_path);
                    else           Serial.println("[livecam] URL is now none (empty or not http://)");
                }
                if (!s_host[0]) { pause_ms(500); continue; }   // no usable URL: wait for one
                const bool snapshot = run_request(c);
                pause_ms(snapshot                      ? LIVECAM_SNAPSHOT_MS
                       : s_lastErr == ERR_STREAM_LOST ? LIVECAM_STALL_RETRY_MS
                                                      : LIVECAM_RETRY_MS);
            }
            c.stop();
        } else {
            diag::log("livecam PSRAM alloc for frame buffers failed");
            fail(ERR_NO_PSRAM);
            while (!s_stop) vTaskDelay(pdMS_TO_TICKS(50));
        }

        // Reached after onExit. A quick way back in can have the LVGL side decoding a frame
        // it took just before this, so wait for that to finish before freeing its buffer.
        portENTER_CRITICAL(&s_mux);
        for (int i = 0; i < 3; ++i) s_jpg[i] = nullptr;
        s_ready = -1;
        portEXIT_CRITICAL(&s_mux);
        for (;;) {
            portENTER_CRITICAL(&s_mux);
            const bool busy = s_busy >= 0;
            portEXIT_CRITICAL(&s_mux);
            if (!busy) break;
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        for (int i = 0; i < 3; ++i) if (bufs[i]) heap_caps_free(bufs[i]);
    }

    // Created once in init() and never deleted: it sleeps on a notification until onEnter
    // wakes it. Starting it per visit was tried and failed on a real Orb: after a spell in
    // the Flight Tracker the largest free internal block was 3.5 KB, smaller than this
    // stack, so the task could not be created and the screen said CONNECTING for ever.
    // Reserving it at boot, while internal RAM is still in one piece, costs 4 KB always and
    // works always. It cannot go in PSRAM: lwIP asserts that no socket call is made from a
    // PSRAM stack (tcp_isn_default.c), and did. TASK_STACK is sized from the high-water
    // mark printed each time a visit ends (peak seen ~3.1 KB).
    constexpr uint32_t TASK_STACK = 4096;
    TaskHandle_t s_task = nullptr;

    void stream_task(void * /*arg*/) {
        for (;;) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            if (s_stop) continue;
            while (!s_stop) run_session();
            s_state = ST_IDLE;
            Serial.printf("[livecam] stream closed, stack never came within %u B of its %u\n",
                          (unsigned)uxTaskGetStackHighWaterMark(nullptr), (unsigned)TASK_STACK);
        }
    }

    // ---- LVGL side (core 1) ------------------------------------------------------------

    // The picture FILLS the round screen: it is scaled until both sides reach 466, centred,
    // and whatever overhangs is cut off. A 16:9 frame therefore loses its left and right
    // edges rather than showing black bars above and below. TJpgDec only scales down, and
    // only by powers of two, so the frame is decoded at its own size into s_src and then
    // resampled (nearest neighbour, through the two lookup tables) onto the canvas.
    uint16_t   *s_src    = nullptr;   // decoded frame, s_srcW x s_srcH; lives with the canvas
    size_t      s_srcCap = 0;         // pixels s_src can hold
    int         s_srcW   = 0, s_srcH = 0;
    // The two lookup tables (1.9 KB) live in PSRAM beside s_src and only while the app is
    // open; as file-scope arrays they were internal RAM for the life of the firmware.
    int16_t    *s_mapX   = nullptr;   // canvas column -> s_src column, SCREEN_W entries
    int16_t    *s_mapY   = nullptr;   // canvas row    -> s_src row, SCREEN_H entries

    bool jpg_out(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bmp) {
        for (int j = 0; j < h; ++j) {
            const int yy = y + j;
            if (yy < 0 || yy >= s_srcH) continue;
            const int n = (x + w > s_srcW) ? s_srcW - x : w;
            if (x < 0 || n <= 0) continue;
            memcpy(s_src + (size_t)yy * s_srcW + x, bmp + (size_t)j * w, (size_t)n * sizeof(uint16_t));
        }
        return true;
    }

    // Size the scratch buffer and the lookup tables for frames of w x h. Only when it changes.
    bool prepare_for(int w, int h) {
        if (w == s_srcW && h == s_srcH && s_src) return true;
        if (!s_mapX) s_mapX = (int16_t *)heap_caps_malloc((SCREEN_W + SCREEN_H) * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!s_mapX) { diag::log("livecam PSRAM alloc for the maps failed"); return false; }
        s_mapY = s_mapX + SCREEN_W;
        const size_t need = (size_t)w * h;
        if (need > s_srcCap) {
            if (s_src) heap_caps_free(s_src);
            s_src = (uint16_t *)heap_caps_malloc(need * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
            s_srcCap = s_src ? need : 0;
            if (!s_src) { diag::log("livecam PSRAM alloc for a %dx%d frame failed", w, h); s_srcW = s_srcH = 0; return false; }
        }
        s_srcW = w; s_srcH = h;
        // Cover: the larger of the two ratios, so neither side falls short of the screen.
        const float f = fmaxf((float)SCREEN_W / w, (float)SCREEN_H / h);
        for (int x = 0; x < SCREEN_W; ++x) {
            int sx = (int)((x + 0.5f - SCREEN_W / 2.0f) / f + w / 2.0f);
            s_mapX[x] = (int16_t)(sx < 0 ? 0 : sx >= w ? w - 1 : sx);
        }
        for (int y = 0; y < SCREEN_H; ++y) {
            int sy = (int)((y + 0.5f - SCREEN_H / 2.0f) / f + h / 2.0f);
            s_mapY[y] = (int16_t)(sy < 0 ? 0 : sy >= h ? h - 1 : sy);
        }
        Serial.printf("[livecam] frames are %dx%d, zoomed %.2fx to fill the screen\n", w, h, f);
        return true;
    }

    // The theme's bezel over the picture (theme_style::Livecam). Decoded on entry, released
    // on exit, like every other screen's plate. Per row, the span INSIDE the circle is the
    // camera's and everything either side of it is copied from the plate, so the cost is two
    // memcpy per row rather than a test per pixel.
    plate_sprite::Plate s_frameArt { "livecam_plate.png", "livecam_frame" };
    const uint16_t *s_frame = nullptr;     // 466x466 RGB565, or nullptr for no frame
    int     s_frameR = 0;                  // the camera circle's radius
    // The camera's span on row y. Worked out per row, per frame (466 square roots, nothing
    // next to a JPEG decode), not kept in a table: a table here was 1.8 KB of internal RAM
    // held for the life of the firmware, and internal RAM is what the downloads run out of.
    inline void span(int y, int &x0, int &x1) {
        const float dy = y + 0.5f - SCREEN_H / 2.0f;
        if (fabsf(dy) >= s_frameR) { x0 = SCREEN_W; x1 = -1; return; }
        const float half = sqrtf((float)s_frameR * s_frameR - dy * dy);
        x0 = (int)ceilf(SCREEN_W / 2.0f - half);
        x1 = (int)floorf(SCREEN_W / 2.0f + half - 1.0f);
    }

    void frame_prepare() {
        s_frame = nullptr;
        const int R = theme_style::livecam().frameR;
        if (R <= 0) return;
        const lv_img_dsc_t *art = plate_sprite::get(s_frameArt);
        if (!art || art->header.w != SCREEN_W || art->header.h != SCREEN_H) return;
        s_frame = (const uint16_t *)art->data;
        s_frameR = R;
        Serial.printf("[livecam] theme frame over the picture, inside r=%d\n", R);
    }

    // Lay the bezel over whatever is in the canvas.
    void frame_apply() {
        if (!s_frame || !s_pix) return;
        for (int y = 0; y < SCREEN_H; ++y) {
            lv_color_t *out = s_pix + (size_t)y * SCREEN_W;
            const uint16_t *src = s_frame + (size_t)y * SCREEN_W;
            int x0, x1;
            span(y, x0, x1);
            if (x0 > x1) { memcpy(out, src, SCREEN_W * sizeof(uint16_t)); continue; }
            memcpy(out, src, (size_t)x0 * sizeof(uint16_t));
            memcpy(out + x1 + 1, src + x1 + 1, (size_t)(SCREEN_W - 1 - x1) * sizeof(uint16_t));
        }
    }

    void draw_frame(const uint8_t *jpg, uint32_t len) {
        uint16_t w = 0, h = 0;
        if (TJpgDec.getJpgSize(&w, &h, (uint8_t *)jpg, len) != JDR_OK || !w || !h) {
            diag::log("livecam frame is not a readable baseline JPEG");
            return;
        }
        // Decode smaller when the frame is at least twice what filling the screen needs.
        uint8_t scale = 1;
        while (scale < 8 && (w < h ? w : h) / (scale * 2) >= SCREEN_H) scale *= 2;
        if (!prepare_for(w / scale, h / scale)) return;

        TJpgDec.setJpgScale(scale);
        TJpgDec.setSwapBytes(false);
        TJpgDec.setCallback(jpg_out);
        const JRESULT jr = TJpgDec.drawJpg(0, 0, (uint8_t *)jpg, len);
        if (jr != JDR_OK) { diag::log("livecam decode fail (jr=%d)", (int)jr); return; }

        for (int y = 0; y < SCREEN_H; ++y) {
            const uint16_t *row = s_src + (size_t)s_mapY[y] * s_srcW;
            lv_color_t *out = s_pix + (size_t)y * SCREEN_W;
            for (int x = 0; x < SCREEN_W; ++x) out[x].full = row[s_mapX[x]];
        }
        frame_apply();
        lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(s_canvas);
    }

    // Only called when there is no recent picture: a reconnect the viewer cannot see is not
    // worth announcing, and flipping between messages every couple of seconds was worse.
    void show_state(State st, int err, UrlState us) {
        char t[128];
        // Where to set it is on the screen itself: the person looking at this has no reason
        // to have read a README.
        if (us == URL_NONE)         snprintf(t, sizeof(t), tr("NO STREAM SET\n\nSettings > Livestream\nor theorb.local/livestream", "KEIN STREAM GESETZT\n\nEinstellungen > Livestream\noder theorb.local/livestream"));
        else if (us == URL_BAD)     snprintf(t, sizeof(t), tr("THE STREAM URL MUST\nSTART WITH http://\n\nSettings > Livestream", "DIE STREAM-URL MUSS\nMIT http:// BEGINNEN\n\nEinstellungen > Livestream"));
        else if (st != ST_ERROR)    snprintf(t, sizeof(t), tr("CONNECTING...", "VERBINDE..."));
        else if (err >= 100)        snprintf(t, sizeof(t), tr("NO SIGNAL\nHTTP %d", "KEIN SIGNAL\nHTTP %d"), err);
        else if (err == ERR_NO_WIFI) snprintf(t, sizeof(t), tr("NO SIGNAL\nno WiFi", "KEIN SIGNAL\nkein WLAN"));
        else                        snprintf(t, sizeof(t), tr("NO SIGNAL\nretrying (%d)", "KEIN SIGNAL\nneuer Versuch (%d)"), err);
        lv_label_set_text(s_msg, t);
        lv_obj_clear_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
    }

    void tick_cb(lv_timer_t * /*t*/) {
        if (!s_active) return;

        const UrlState us   = s_urlState;
        const uint32_t last = s_lastFrameMs;
        const bool recent = us == URL_OK && last && (millis() - last) < LIVECAM_STALE_MS;
        // A stream that has stalled past LIVECAM_STALE_MS without failing yet reads as
        // connecting, which is what it is doing.
        State st = us != URL_OK ? ST_ERROR : (recent ? ST_LIVE : s_state);
        if (st == ST_LIVE && !recent) st = ST_CONNECTING;
        const int err = s_lastErr;
        if (st != s_shown || us != s_shownUrl || (st == ST_ERROR && err != s_shownErr)) {
            if (st == ST_LIVE) lv_obj_add_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
            else               show_state(st, err, us);
            s_shown = st; s_shownErr = err; s_shownUrl = us;
        }

        int idx;
        uint32_t len = 0;
        const uint8_t *jpg = nullptr;
        portENTER_CRITICAL(&s_mux);
        idx = s_ready;
        if (idx >= 0) { s_busy = idx; s_ready = -1; jpg = s_jpg[idx]; len = s_jpgLen[idx]; }
        portEXIT_CRITICAL(&s_mux);
        if (idx < 0) return;

        if (jpg && s_canvas) draw_frame(jpg, len);

        portENTER_CRITICAL(&s_mux);
        s_busy = -1;
        portEXIT_CRITICAL(&s_mux);
    }

} // namespace

void livecamview::setUrl(const char *url) {
    if (!url) url = "";
    portENTER_CRITICAL(&s_mux);
    strncpy(s_urlNext, url, sizeof(s_urlNext) - 1);
    s_urlNext[sizeof(s_urlNext) - 1] = 0;
    s_urlChanged = true;
    portEXIT_CRITICAL(&s_mux);
    s_urlState = classify(url);
    s_lastFrameMs = 0;   // the picture on screen belongs to the old URL
}

void livecamview::init(const char *url) {
    // Before the task exists, so it is applied directly rather than parked.
    if (!url) url = "";
    s_urlState = classify(url);
    if (s_urlState != URL_OK || !parse_url(url, s_host, s_port, s_path)) s_host[0] = 0;
    Serial.printf("[livecam] URL %s\n", s_urlState == URL_OK   ? url
                                      : s_urlState == URL_NONE ? "(none set)" : "(not http://)");

    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    s_msg = lv_label_create(s_screen);
    lv_label_set_text(s_msg, "");
    lv_obj_set_style_text_align(s_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_msg, lv_color_hex(0xB0B6BE), 0);
    lv_obj_set_style_text_font(s_msg, &font_de_16, 0);
    lv_obj_set_style_bg_color(s_msg, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_msg, 200, 0);
    lv_obj_set_style_pad_hor(s_msg, 10, 0);
    lv_obj_set_style_pad_ver(s_msg, 6, 0);
    lv_obj_center(s_msg);
    lv_obj_add_flag(s_msg, LV_OBJ_FLAG_HIDDEN);

    lv_timer_create(tick_cb, LIVECAM_TICK_MS, nullptr);

    if (xTaskCreatePinnedToCore(stream_task, "livecam", TASK_STACK, nullptr, 1, &s_task, 0) != pdPASS) {
        s_task = nullptr;
        Serial.println("[livecam] could not create the stream task");
        diag::log("livecam could not create its stream task");
    }
}

lv_obj_t *livecamview::screen() { return s_screen; }

void livecamview::onEnter() {
    // The canvas is 434 KB of PSRAM, so it lives only while the app is open.
    const size_t bytes = (size_t)SCREEN_W * SCREEN_H * sizeof(lv_color_t);
    s_pix = (lv_color_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_pix) {
        s_canvas = lv_canvas_create(s_screen);
        lv_canvas_set_buffer(s_canvas, s_pix, SCREEN_W, SCREEN_H, LV_IMG_CF_TRUE_COLOR);
        lv_obj_center(s_canvas);
        lv_canvas_fill_bg(s_canvas, lv_color_black(), LV_OPA_COVER);
        lv_obj_move_background(s_canvas);                 // under the status label
        frame_prepare();
        if (s_frame) frame_apply();                       // the bezel is there before the picture
        else lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);   // until the first frame decodes
    } else {
        diag::log("livecam PSRAM alloc for canvas failed");
    }
    s_shown = ST_IDLE; s_shownErr = 0;
    s_lastErr = 0;
    s_lastFrameMs = 0;
    s_state = ST_CONNECTING;
    s_active = true;
    // The task is woken even without a usable URL: one can arrive from the web page while
    // this screen is open, and the task is what picks it up.
    if (!s_task) { s_lastErr = ERR_NO_TASK; s_state = ST_ERROR; return; }
    s_stop = false;
    xTaskNotifyGive(s_task);
}

void livecamview::onExit() {
    s_active = false;
    s_stop = true;   // the task closes the socket and frees the JPEG buffers itself
    if (s_canvas) { lv_obj_del(s_canvas); s_canvas = nullptr; }
    if (s_pix)    { heap_caps_free(s_pix); s_pix = nullptr; }
    if (s_src)    { heap_caps_free(s_src); s_src = nullptr; s_srcCap = 0; s_srcW = s_srcH = 0; }
    if (s_mapX)   { heap_caps_free(s_mapX); s_mapX = s_mapY = nullptr; }
    s_frame = nullptr;
    plate_sprite::release(s_frameArt);
    lv_obj_add_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
}

#endif // LIVECAM_ENABLED
