#pragma once
// "Satellites": the ISS, Tiangong, Hubble and James Webb over NASA's day and night pictures.
//
// The globe follows the satellite, which sits in a reticle at the centre; its next 90
// minutes are drawn ahead of it and the last 40 behind. Below: height and speed, and when it
// next comes over home, how high, from where to where and whether it can be seen. Turning
// the knob steps through the four. Shown or hidden in Settings > Satellites.
//
// The three in Earth orbit come from two-line elements (CelesTrak, plain HTTP, once a day)
// and SGP4. Webb circles the Sun-Earth L2 point instead and comes from a table built into
// the firmware (src/jwst_ephem.h). The pictures are the Earth app's, /earth on the SD card.
#include <lvgl.h>

namespace satview {
void      init();                     // build the screen, read the saved elements, start the worker
lv_obj_t *screen();
void      onEnter(double homeLat, double homeLon, const char *homeName);
void      onExit();
void      onTurn(int delta);          // the next or previous satellite
void      setShown(bool shown);       // Settings > Satellites > Show app; no fetching while hidden

// The network side, called from the feed task (core 0), which owns the sockets.
bool      tleWanted();                // shown, the clock is set, and elements are missing or a day old
void      fetchTle();                 // blocking: one HTTP GET per satellite, then saves them
}
