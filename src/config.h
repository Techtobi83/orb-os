#pragma once
// The Orb OS — build & user configuration.

// Bump this whenever a build goes out that a device could be BEHIND. That is not only
// releases: 1.4.2 sat still through the Intel screen being rebuilt, an app being deleted,
// another renamed, and a serial command being added, so Studio compared "1.4.2" against
// "1.4.2", said "up to date", and left an Orb missing everything in that list. THEME_CAPS
// exists because this stopped moving; it covers theme settings and nothing else, so a new
// command or a deleted screen is invisible to it. Move this too.
#define FW_VERSION "2.31.4"   // shown on the web config page + Stats screen
// Edit pins below: replace every -1 with the value from the Waveshare factory demo
// (see docs/HARDWARE.md and docs/SETUP.md). Do NOT guess them.

// ---------- Which apps this build ships (CUT-01) ----------
// Launch one is exactly Clock, Flight tracker, News and Settings. Weather, Surveillance
// and the Stock Ticker come off the roster.
//
// ABSENT, not present and disabled, and the distinction is UX-042's: every app that ships
// is on every unit, so an app that is not ready is not shipped rather than shipped dark.
// A theme can already hide an app it does not want (theme.json's roster, and the `hidden`
// flag app_shell::add takes); that is a design's choice about a finished app, which is a
// different thing from a product not carrying one yet.
//
// One switch rather than deleted code, because the cut list is explicit that these three
// are "real, wanted, and waiting" and that nothing there is cancelled. Setting this to 0
// brings all three back exactly as they were.
//
// This is uniform across every unit, which is what keeps it inside UX-042: the requirement
// forbids holding a feature back from SOME buyers, not shipping a product that does not
// have it yet.
//
// The firmware still PARSES weather_style.json and ticker_style.json, and Apps/Names keep
// their fields, because TC-008 says a shipped parameter is never removed. THEME_CAPS stays
// at 34 for the same reason; Studio hides the controls behind its own flag rather than the
// device pretending it never understood them.
#define APPS_LAUNCH_ONE 1

// ---------- Weather radar, carried despite APPS_LAUNCH_ONE ----------
// A deliberate exception to CUT-01, asked for by the owner on 2026-09-29: the round
// RainViewer radar centred on the Flight Tracker's home location comes back on its own,
// while Surveillance and the Ticker stay off. The app itself is unchanged; only its gate
// moved. Setting this to 0 (with APPS_LAUNCH_ONE 1) returns the build to exactly launch
// one's roster.
//
// Everything that decides whether the weather app is in the build asks WEATHER_ENABLED,
// never APPS_LAUNCH_ONE directly, so the two switches cannot disagree at one call site.
#define WEATHER_ENABLED (1 || !APPS_LAUNCH_ONE)

// ---------- News (headlines), switched off on this Orb ----------
// The owner has no use for News and asked for it gone (2026-09-30), which departs from
// CUT-01, where launch one is Clock, Flight Tracker, News and Settings. At 0 the build
// carries no News app, no headline fetching and no News typefaces in memory. The source
// stays, dormant, so a theme's intel_style.json is still parsed (TC-008) and 1 brings the
// app back exactly as it was.
#define NEWS_ENABLED 0

// ---------- Forecast ("Vorhersage") ----------
// A second weather screen after the radar, asked for by the owner on 2026-09-30: the
// forecast as a 24-hour dial (forecast_view.cpp), German text. Reads the Open-Meteo
// snapshot already fetched every WEATHER_REFRESH_MS, so it adds no requests. Serves no CUT
// item. Not themeable yet: it has no Orb Studio half.
#define FORECAST_ENABLED 1
#define FORECAST_NAME    tr("Forecast", "Vorhersage")   // the knob menu's label for it (lang.h)

// "Earth": the globe over home with the real sun (globe_view.cpp). Its two pictures live on
// the SD card in /earth; without them the screen says so.
#define GLOBE_ENABLED    1
#define GLOBE_NAME       tr("Earth", "Erde")
#define GLOBE_RADIUS_PX  222    // to the glass: 222 + the 9 px atmosphere = 231 of 233

// ---------- Touch: swipe sideways to change app ----------
// The Orb is knob-first (docs/ARCHITECTURE.md), and the knob still does everything. This adds
// ONE touch gesture on top, asked for by the owner on 2026-09-30: a sideways swipe moves to
// the next or previous app, the way the knob menu would. Taps do nothing. Any touch wakes a
// dimmed screen. Setting this to 0 takes the touch chip out of the build again.
//
// What a swipe does lives in input_router::swipe(), beside the knob, so both inputs obey the
// same modal rules. Device only: the simulator has no touch chip.
#ifdef ARDUINO
#define TOUCH_SWIPE_ENABLED 1
#else
#define TOUCH_SWIPE_ENABLED 0
#endif
#define TOUCH_POLL_MS       15     // how often the CST9217 is read while the loop runs
#define TOUCH_SWIPE_MIN_PX  90     // sideways travel a swipe needs, of the 466 px glass
#define TOUCH_SWIPE_MAX_MS  700    // slower than this is a drag, not a swipe
#define TOUCH_MENU_WAIT_MS  5000   // the menu a pull-down opens waits this long for the knob (2 s after a turn)
#define TOUCH_TAP_MAX_PX    24     // a tap moves less than this either way...
#define TOUCH_TAP_MAX_MS    500    // ...and lifts within this

// ---------- Livestream (network camera app) ----------
// Shows one camera's MJPEG stream (or snapshot URL) full screen. The URL is set on the
// device, in Settings > Livestream (knob), or from a browser at http://theorb.local/livestream,
// and kept in NVS. Nothing about a camera is compiled in: a fork is public, and a camera
// address in the source would be too.
//
// Optional: src/secrets.h (gitignored; see secrets.example.h) may define LIVECAM_URL as the
// URL to use until one has been set on the device. Handy while developing, never needed.
//
// Always built for the device; never for the desktop simulator, which has no socket code
// for it. Independent of APPS_LAUNCH_ONE: this brings back nothing else.
#if defined(ARDUINO) && __has_include("secrets.h")
#include "secrets.h"
#endif
#ifdef ARDUINO
#define LIVECAM_ENABLED 1
#else
#define LIVECAM_ENABLED 0
#endif
#ifndef LIVECAM_URL
#define LIVECAM_URL         ""           // no camera until one is set on the device
#endif
#define LIVECAM_NAME        "Livestream" // the knob menu's label for it
#define LIVECAM_URL_MAX     160          // longest URL kept, terminator included
#define LIVECAM_JPEG_MAX    (128 * 1024) // largest frame accepted; 640x360 at -q:v 28 is ~20-40 KB
#define LIVECAM_CONNECT_MS  3000         // TCP connect timeout
#define LIVECAM_ANSWER_MS   15000        // first reply; go2rtc starts ffmpeg + RTSP on demand, which is slow
#define LIVECAM_READ_MS     10000        // no complete frame for this long = stream lost
// Once frames flow, a shorter gap already means lost: on a real Orb the stream stalled for
// 10 s at a time while go2rtc, measured from a PC, never paused longer than 0.36 s. The
// stalls track internal RAM (WiFi RX buffers must live there), and a new connection gets
// frames again sooner than waiting out TCP's retransmit backoff.
#define LIVECAM_STALL_MS       3000
#define LIVECAM_STALL_RETRY_MS 100       // reconnect almost at once after a stall
#define LIVECAM_STALE_MS    4000         // keep the last picture up, without a message, this long
#define LIVECAM_RETRY_MS    2000         // pause before reconnecting after a failure
#define LIVECAM_SNAPSHOT_MS 50           // pause between snapshot fetches (go2rtc already waits for the next frame)
#define LIVECAM_TICK_MS     40           // how often the screen looks for a new frame

// ---------- Home location ----------
// A PLACEHOLDER, and deliberately not a place. The device has exactly two location inputs
// (UX-025): the network it joins, looked up at the end of WiFi setup and retried on any
// later boot that finds none set, and a location the owner picks in Settings, which always
// wins. Neither is compiled in.
//
// This was Phoenix, the owner's own city, and before it a pushed design's coordinates could
// override even that. Either way a stranger whose lookup had not run yet was shown somebody
// else's sky with nothing on the dial saying so. Now `locSet` in NVS records whether a real
// location has ever been established, the Flight Tracker says "Location not set" until one
// has, and the aircraft feed is not polled at all — so these two numbers are never drawn
// and never queried. They exist so the projection maths has a valid double to hold.
//
// The simulator keeps its own centre (SIM_HOME_LAT in sim_main.cpp, ORBLAT/ORBLON to move
// it), because there is no NVS on the desktop and nothing to look up an IP against.
#define HOME_LAT_DEFAULT   0.0
#define HOME_LON_DEFAULT   0.0

// ---------- Radar ----------
#define RANGE_KM_DEFAULT    30.0f          // display range (outer ring). Query is wider, see below.
// Feed query radius = display range × MULT, clamped to [MIN, MAX]. Querying a bit wider than
// the display shows off-range traffic as edge arrows. The floor MUST stay small: the old 50 km
// floor made small display ranges still pull a huge aircraft list in busy airspace, which timed
// out the poll (feed permanently amber near big hubs). Fix contributed by @alexzogh (STLWarehouse).
#define ADSB_QUERY_MULT     1.4f
#define ADSB_QUERY_MIN_KM   12.0f
#define ADSB_QUERY_MAX_KM   150.0f
static const float RANGE_STEPS_KM[] = {10.0f, 20.0f, 30.0f, 50.0f, 100.0f};
// Be gentle with the free API. This was 2000, then 5000, and is now 10000 — each step
// taken because the feed kept answering with 403/429.
//
// The last step is the counter-intuitive one and it is worth writing down: polling LESS
// delivers MORE. Measured over a 10-minute soak at 5 s, the feed refused four times, and
// each refusal costs a 60 s backoff by design (see the refusal branch in main.cpp). That is
// roughly four of the ten minutes spent deliberately silent, in bursts, so the dial went
// stale for a minute at a time. At 10 s the request rate halves to about 360/hour, which
// should stay under whatever window is being tripped, and steady updates every 10 s beat
// bursts of updates every 5 s separated by minute-long penalties.
//
// Aircraft do not move far in ten seconds at any range this dial draws: 400 kt is about
// 2 km, which at the default range is a couple of pixels.
#define POLL_INTERVAL_MS    10000
#define POLL_INTERVAL_BATTERY_MS 15000      // slower polling when running on battery
#define MOTION_INTERP       1              // 1 = glyphs glide between polls; 0 = snap to new pos

// A contact is kept on the table (and on the scope) for as long as it is this old or
// younger, dimming continuously between the first two numbers, held at its dimmest from
// the second to the third, then dropped. This is what lets one missed poll (routine ADS-B
// reception gaps, not a fault) read as a plane going briefly quiet rather than the scope
// flickering it in and out of existence. See ac_freshness() in radar_view.cpp.
// Full brightness up to here, then fading to a floor.
//
// 25 s, down from 60. Polls land every ~10 s, so 60 meant a contact could go unheard for
// five whole polls and still be drawn exactly like one confirmed a moment ago: a 25 second
// outage produced no visible change at all, which is how Zion ended up asking whether the
// aircraft were stuck. 25 s is two and a half poll intervals, so ordinary jitter and one
// dropped poll stay invisible and a real silence starts showing immediately.
#define AC_DIM_START_MS      25000
#define AC_DIM_FLOOR_MS      75000         // dimmest steady state from here
// How faint a contact gets once it is being drawn from memory rather than from a report.
// It was 0.22 and Zion asked for considerably dimmer. Deliberately not zero: the point is
// to SAY a contact has gone quiet, not to make it vanish ahead of actually being dropped.
#define AC_DIM_FLOOR_OPA     0.15f
// When the screen SAYS so, as opposed to when it starts showing it.
//
// Separate from AC_DIM_START_MS, and the order between them is the thing that matters. On
// 2026-08-24 Zion found the banner claiming "No aircraft data" over a dial full of
// full-brightness traffic, because the banner fired at 45 s and the first dimming at 60.
// The instrument contradicted itself. The fix was one clock for both.
//
// One clock is stricter than the fault required, though. A banner appearing BEFORE anything
// looks stale is the contradiction; aircraft fading before the banner arrives is a graduated
// warning, which is what this is for. So the invariant is an ORDER, not an equality, and
// radar_view.cpp asserts it at compile time rather than trusting this comment.
#define ADSB_NO_DATA_MS      60000
#define AC_HARD_EXPIRE_MS    180000        // dropped from the table entirely past here

// ---------- Weather forecast (Open-Meteo, no API key) ----------
#define WEATHER_REFRESH_MS  1800000UL      // 30 minutes; forecast data changes slowly
// How long the start screen holds before it fades to the clock. Read by both the splash's
// own timer (ui.cpp) and the end-of-setup loop that drives it (main.cpp), which used to
// carry the same 3000 each: change one alone and the splash is cut short or the boot waits
// on a timer that already fired. 5 s since 2026-10-01, at the owner's request.
#define SPLASH_HOLD_MS      5000UL
#define WX_RADAR_REFRESH_MS 300000UL       // RainViewer frames update about every 5 minutes
// Weakest tile pixel that counts as precipitation. RainViewer's scheme 2 draws precipitation
// fully opaque (blues) and the weak-echo band below it (ground clutter, virga, drizzle too
// light to measure) as a translucent grey-beige, alpha 20..190. Drawing that band made a dry
// day in the Sauerland on 2026-09-29 look like widespread rain: 96% of the "rain" pixels in
// that tile were translucent, and every forecast app said 0 mm. 255 = measurable only.
#define WX_RADAR_MIN_ALPHA  255
// The weather map's radius, edge of the dial from the centre, and the RainViewer zoom that
// covers it. One definition read by both the fetch/crop (wx_radar_client.cpp) and the range
// shown on screen (ui.cpp), which used to be two tables that had to be kept equal by hand.
// 40 km at zoom 7 since 2026-09-30 (was 80.47 km, 50 mi, at zoom 6): the owner's town in
// detail, and half the road map to project. Zoom 7 is assumed to cover ~75 km (see WX_ZOOM
// in wx_radar_client.cpp); the radius must stay below that or the crop runs off the tile.
#define WX_RADAR_RANGE_KM     40.0
#define WX_RADAR_FETCH_ZOOM   7
#define WX_RADAR_FETCH_KM     75.0
static_assert(WX_RADAR_RANGE_KM <= WX_RADAR_FETCH_KM, "the weather map would crop past the tile it fetched");
#define CLOUD_IMAGE_REFRESH_MS 600000UL    // EUMETSAT MTG cloud imagery; cache for 10 minutes

// ---------- Screen (CO5300 AMOLED) ----------
#define SCREEN_W            466
#define SCREEN_H            466
#define SCREEN_CX           233
#define SCREEN_CY           233
#define RADAR_R_OUTER_PX    218            // outer ring radius in pixels
#define LV_COLOR_DEPTH_BITS 16
#define LCD_COL_OFFSET      6              // CO5300 column (x) gap on this panel (esp_lcd set_gap 0x06)
#define LCD_ROW_OFFSET      0              // no row (y) gap
#define LCD_QSPI_HZ         80000000       // CO5300 QSPI clock (vendor uses 40 MHz; 80 = faster, verify no artifacts)
#define BRIGHTNESS_DEFAULT  200            // 0..255, panel brightness via cmd 0x51
#define TZ_STR              "MST7"                        // POSIX TZ default: Arizona (UTC-7, no DST).
                                                          // Overridden at runtime by IP geolocation on
                                                          // auto-locate (host_locate_current) and by the
                                                          // web config; this is just the out-of-box value.
#define BRIGHTNESS_IDLE     25             // dimmed after no touch for IDLE_DIM_MS
#define IDLE_DIM_MS         3600000UL      // default: dim the screen after 1 hour idle (Settings > Display)
// Waking a dimmed screen by moving it. The accelerometer is read every 50 ms and the sum
// of the three axis deltas between reads is compared with this, in LSB at ±2 g (16384
// per g). 500 is about 30 mg: a knuckle on the desk, a hand on the arm, a mug set down
// hard enough to feel. Sensor noise sits around 10 to 20. Raise it if a rattling desk
// keeps the screen awake, lower it if a tap does not wake it. Zion, 2026-09-14: "if
// somebody is working at their desk, it'll just wake up because it'll feel the vibration."
#define MOTION_WAKE_LSB     500

// ---------- ADS-B API (free, non-commercial) ----------
// Plain HTTP on purpose, and it is what makes the radar work at all.
//
// A TLS handshake needs two contiguous ~16 KB buffers from internal RAM, and this board
// cannot offer them: the largest free internal block is ~17 KB no matter what else is
// freed. That was measured, not assumed. Routing every LVGL allocation to PSRAM changed
// the number by exactly zero bytes, because it is a property of the memory map rather than
// fragmentation anyone can tidy up. So every HTTPS fetch died with '-32512 SSL memory
// allocation failed', which left the radar with no aircraft, the weather blank, and the
// device rebooting itself every ~27 minutes on the stuck-feed watchdog.
//
// Dropping to HTTP costs almost nothing here, because ADSB_HTTPS_INSECURE was already 1:
// certificates were never verified, so TLS was only hiding public flight data from
// eavesdroppers, never authenticating the source. No credentials are sent. adsb.lol serves
// this endpoint over HTTP.
//
// There is no fallback host. The airplanes.live entry that used to sit here was never
// referenced by any code, and as of 2026-08-23 that service answers 403 to every request
// with a note demanding you email them for permission first, so it was a fallback in name
// only. The nearest free replacement, opendata.adsb.fi, redirects HTTP to HTTPS and so is
// unreachable from this device at all (see the TLS note above). Until a gateway exists to
// hold real failover off-device, this feed is genuinely single-sourced, and adsb.lol's
// slow spells (a 32.2 s TCP connect measured 2026-08-22) are felt directly by the scope.
#define ADSB_PRIMARY_HOST   "api.adsb.lol"          // GET /v2/point/{lat}/{lon}/{radius_nm}
// What that host is called ON SCREEN, kept next to the host so the two cannot drift. UX-039
// wants a failure to name the source that could not be reached, and the full hostname is too
// long for a banner in a 466 px circle — this is the short form a person can act on.
#define ADSB_SOURCE_NAME    "adsb.lol"
#define ADSB_PRIMARY_TLS    0
// Who this device says it is, to every service it calls.
//
// Not a cosmetic string. A User-Agent is how an API operator sees who is calling and where
// to complain, and this one named the upstream project and linked to its author's
// repository. Every request this Orb made was attributed to him: a rate limit earned here
// would have been counted against his project, and anyone at adsb.lol wanting to reach the
// maintainer would have reached the wrong person.
//
// Version comes from FW_VERSION by string concatenation, so it can never go stale the way a
// hand-written "1.0" did while the firmware climbed to 1.34.
// zionbrock.com/orb rather than the workers.dev address on purpose: this string is
// compiled into every Orb ever flashed and an API operator may read it years from now, so
// it has to be the address that will still be his. The deployment behind it can move.
// The Orb's name on the local network. ONE definition, because this was nine separately
// typed copies of the same string across the firmware and one of them would have been
// missed. ORB_MDNS_ADDR is built from it so the two can never disagree.
//
// Renamed from `capsuleradar` in 1.40. Deliberately NOT kept as a second name: ESPmDNS
// registers one hostname, and the IDF call that could add another is for advertising OTHER
// devices and wants a fixed address list, so an alias built on it would go stale on the
// next DHCP renewal. An address that confidently resolves to the wrong place is worse than
// one that stops existing.
//
// Unrelated to the `capsuleradar` NVS namespace, which is invisible, holds every setting,
// and must never be renamed.
#define ORB_MDNS_HOST       "theorb"
#define ORB_MDNS_ADDR       ORB_MDNS_HOST ".local"

#define ORB_USER_AGENT      "TheOrbOS/" FW_VERSION " (ESP32-S3 hobby; +https://zionbrock.com/orb)"
// Named after one feed and used by nine clients: aircraft, weather, weather radar, cloud
// imagery, route lookup, photos and the news gateway. Kept as an alias rather than renamed
// at all nine call sites, which would be a large diff to say one small thing.
#define ADSB_USER_AGENT     ORB_USER_AGENT
#define ADSB_HTTPS_INSECURE 1               // 1 = setInsecure() (hobby). 0 = use pinned root CA.
// How many distinct addresses to keep for the feed. Rate limiting is per-edge, so having
// somewhere else to ask is worth more than any backoff. Learned by DNS at runtime.
#define ADSB_EDGE_POOL      5
// Edges attempted per poll. Deliberately small: see the socket-exhaustion note in
// adsb_client.cpp. The pool still rotates BETWEEN polls, so all of it stays in play.
// Three, not two. Measured against the live service 2026-08-29: the same three addresses
// behind api.adsb.lol answered a laptop on Zion's own network as open, open, dead one
// minute and timeout, timeout, 200-in-355ms ten minutes later. The service is not down, it
// FLAPS, and which door opens changes by the minute. Roughly one attempt in six succeeded.
// Two tries against that is a coin toss lost most of the time, which on the dial looks like
// aircraft that have stopped moving under a banner saying there is no data.
#define ADSB_TRIES_PER_POLL 3
// Connect and read budgets. The read budget was 8000 and the service was measured taking
// 11.6 s to answer during a bad spell on 2026-08-22, so every request failed for a reason
// that had nothing to do with this device.
// Patience, because the measurements demand it. During one of this provider's slow spells
// a plain TCP connect to api.adsb.lol took 32.2 SECONDS from a laptop on the same network,
// while adsb.fi and adsbdb answered in 0.0-0.2 s. At a 6 s budget the Orb was hanging up on
// a server that was going to answer, then reporting a transport error, then backing off —
// guaranteeing no aircraft for the whole of an outage the device could have ridden out.
//
// The cost of waiting is bounded and lands on the feed task, not the UI: worst case is this
// times ADSB_TRIES_PER_POLL. The cost of NOT waiting is measured, and it is every aircraft.
// Five seconds, down from fifteen. A server that is going to answer answers in about a
// third of one; the fifteen was there to be patient with a slow server, and what it actually
// bought was fifteen seconds of a ten-second poll cycle spent waiting on a socket that was
// never going to open. Being patient with a dead door is not patience, it is the whole poll.
#define ADSB_CONNECT_MS     5000
#define ADSB_READ_MS        20000

// A deliberate product ceiling, not a RAM guess: twelve is what a 466 px scope can show
// without the icons piling into each other over a busy city. Note it does NOT shrink the
// download, which is whatever the provider decides to send (~95 KB / ~200 aircraft over
// Phoenix); the trim happens here, after the whole response is on the device.
#define ADSB_MAX_AIRCRAFT   12              // hard cap parsed per poll

// ---------- Intel (headlines) ----------
// The gateway, not a publisher. Plain HTTP for the same reason as everything else here:
// no TLS on this board. See the long note above intel_fetch.
#define INTEL_GATEWAY_HOST  "buildtheorb.zionbrock.workers.dev"
#define INTEL_SOURCE_NAME   "the Orb gateway"      // the same, for the News screen. See ADSB_SOURCE_NAME.
// The poll interval is theme data now (theme_style::Intel::pollMinutes, THEME_CAPS 11),
// defaulting to the ten minutes that used to be welded here as INTEL_POLL_MS.
// A failed poll should not leave the screen empty for the rest of the interval.
#define INTEL_RETRY_MS      60000UL
#define INTEL_CONNECT_MS    6000
#define INTEL_READ_MS       9000
// The "what a fresh Orb shows before anyone picks" defaults now live on
// theme_style::Intel's own compiled-in values (general / bbc / 3), not here: THEME_CAPS 9
// made this screen theme-driven, and a single default belongs in the one struct that owns
// it, not duplicated into a macro nothing reads any more.

// ---------- Debug ----------
#define DEBUG_MEM           0               // 1 = print a [mem] heap/fps line every 5s on serial

// ---------- Pin map ----------
// VERIFIED (ESPHome def, cross-checked against the Waveshare board definition in
// xiaozhi-esp32 and a working Arduino_GFX port for this exact panel):
#define PIN_LCD_CS          12
#define PIN_LCD_RST         39
#define PIN_TP_INT          11
#define PIN_TP_RST          40
#define TP_MIRROR_X         true
#define TP_MIRROR_Y         true

// CONFIRMED — CO5300 QSPI databus (LCD_CS=12, LCD_RST=39 above match too):
#define PIN_LCD_SCLK        38             // QSPI PCLK
#define PIN_LCD_D0          4
#define PIN_LCD_D1          5
#define PIN_LCD_D2          6
#define PIN_LCD_D3          7

// CONFIRMED — shared I2C bus (touch + IMU + RTC + PMIC + audio codec):
#define PIN_I2C_SDA         15
#define PIN_I2C_SCL         14

// CONFIRMED — ES8311 codec over I2S (M4 alert ping). MCLK/DIN/PA included for completeness:
#define PIN_I2S_MCLK        42
#define PIN_I2S_BCLK        9
#define PIN_I2S_LRCLK       45             // a.k.a. WS
#define PIN_I2S_DOUT        8              // ESP32 -> codec (speaker)
#define PIN_I2S_DIN         10             // codec -> ESP32 (mics)
#define PIN_AUDIO_PA        46             // speaker amp enable
#define PIN_BOOT_BUTTON     0              // BOOT button (held on boot = captive portal, later)

// I2C addresses:
#define I2C_ADDR_TOUCH      0x5A           // CST9217 (corrected from vendor driver; was 0x15)
#define I2C_ADDR_IMU        0x6B
#define I2C_ADDR_RTC        0x51
#define I2C_ADDR_PMIC       0x34

// Safety net: should never fire now that pins are filled in. Keeps future edits honest.
#if (PIN_LCD_SCLK < 0) || (PIN_I2C_SDA < 0)
#  error "config.h: QSPI/I2C pins are back to placeholders (-1). Restore the real values."
#endif
