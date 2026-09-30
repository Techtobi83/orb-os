#include "touch_swipe.h"
#include "touch_cst9217.h"
#include "config.h"
#include <Arduino.h>
#include <stdlib.h>

// One stroke is: finger down, any path, finger up. Only the endpoints and the time between
// them are judged, when the finger lifts, so a stroke is never half-acted-on and a finger
// resting on the glass does nothing however long it stays.
//
// A stroke counts as a swipe when it travelled far enough sideways, was clearly more
// sideways than up or down, and was quick. Everything else (a tap, a slow drag, a diagonal,
// a palm while picking the Orb up) is logged and ignored.

namespace touch_swipe {
namespace {

bool     s_down      = false;
uint16_t s_x0 = 0, s_y0 = 0;     // where the finger went down
uint16_t s_x  = 0, s_y  = 0;     // where it was last seen
uint32_t s_t0        = 0;
uint8_t  s_upPolls   = 0;         // consecutive empty reads while down
uint32_t s_lastPoll  = 0;

// The chip reports an empty frame now and then in the middle of a stroke. Ending the stroke
// on the first one would cut a swipe in two, each half too short to count.
constexpr uint8_t RELEASE_POLLS = 2;

Result judge(uint32_t nowMs) {
    const int dx = (int)s_x - (int)s_x0;
    const int dy = (int)s_y - (int)s_y0;
    const uint32_t ms = nowMs - s_t0;
    const bool far      = abs(dx) >= TOUCH_SWIPE_MIN_PX;
    const bool sideways = abs(dx) >= 2 * abs(dy);
    const bool quick    = ms <= TOUCH_SWIPE_MAX_MS;
    const Result r = (far && sideways && quick) ? (dx < 0 ? NEXT : PREV) : NONE;
    Serial.printf("[touch] stroke %u,%u -> %u,%u  dx=%d dy=%d  %lu ms  => %s\n",
                  s_x0, s_y0, s_x, s_y, dx, dy, (unsigned long)ms,
                  r == NEXT ? "next" : r == PREV ? "prev"
                  : !far ? "ignored (short)" : !sideways ? "ignored (not sideways)"
                  : "ignored (slow)");
    return r;
}

}   // namespace

void begin() {
    touch_begin();
}

Result poll(uint32_t nowMs) {
    if (nowMs - s_lastPoll < TOUCH_POLL_MS) return NONE;
    s_lastPoll = nowMs;

    uint16_t x, y;
    if (touch_read(&x, &y)) {
        s_upPolls = 0;
        if (!s_down) {
            s_down = true;
            s_x0 = s_x = x; s_y0 = s_y = y;
            s_t0 = nowMs;
            return TOUCH;
        }
        s_x = x; s_y = y;
        return NONE;
    }
    if (!s_down) return NONE;
    if (++s_upPolls < RELEASE_POLLS) return NONE;
    s_down = false;
    s_upPolls = 0;
    return judge(nowMs);
}

}   // namespace touch_swipe
