#pragma once
#include <lvgl.h>
// The 24-hour day ring: a track round the edge of the dial with noon at the top, today's
// daylight lit between sunrise and sunset, the two times inside it, and a sun or moon riding
// it at the current time. One implementation for every screen that wears it (the forecast,
// and a clock whose theme asks for "dayRing"), so they cannot drift apart.
//
// Reads the stored Open-Meteo snapshot and the local clock. Symbols are flash alpha images;
// nothing here takes PSRAM.
namespace day_ring {
    constexpr int SCR = 466, C = SCR / 2;
    constexpr int RING_R = 214, RING_W = 10;      // centre line and width of the track
    constexpr int LABEL_R = 183;                  // sunrise/sunset times, inside the ring
    constexpr int MARK_HALO = 68, MARK_DISC = 54; // the marker's faint halo, the black disc under it

    struct Ring {
        lv_obj_t *arc = nullptr, *rise = nullptr, *set = nullptr;
        lv_obj_t *halo = nullptr, *disc = nullptr, *mark = nullptr;
        bool complete = false;   // the last update had both the sun times and the clock
    };

    // Two calls, so a screen can put its own layers between the track and the marker.
    void create_track(Ring &r, lv_obj_t *parent);    // the arc and the two time labels
    void create_marker(Ring &r, lv_obj_t *parent);   // halo, disc and symbol, created last = on top

    // Redraw from the snapshot and the clock. Returns whether it is daytime now (between
    // today's sunrise and sunset; 06:00-18:00 when those are unknown).
    bool update(Ring &r);

    bool local_now(int &minutes);   // local minutes after midnight; false until the clock is set
}
