#pragma once
// Swipes on the touch glass, reduced to one result per stroke: sideways changes app, a pull
// down from the upper half opens the app menu. Device only: the
// CST9217 is polled over the shared I2C bus from loop(), the same task every other I2C user
// runs on. What a swipe then DOES is input_router::swipe(), beside what the knob does, so
// both inputs obey the same modal rules.
#include <stdint.h>

namespace touch_swipe {
    enum Result {
        NONE,        // nothing new this poll
        TOUCH,       // a finger just went down (wakes the screen, does nothing else)
        NEXT,        // finger moved right to left: the next app, as a phone would
        PREV,        // finger moved left to right: the previous app
        MENU,        // finger pulled down from the upper half: the app menu, as the rock opens it
    };
    void   begin();                 // after imu_begin(), which brings the I2C bus up
    Result poll(uint32_t nowMs);    // every loop pass; reads the chip every TOUCH_POLL_MS
}
