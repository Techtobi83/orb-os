#pragma once
// "Earth": the globe seen from space over the owner's home, with the real sun.
//
// Day side from NASA's Blue Marble, night side from Black Marble's city lights, a soft
// twilight band between them and a thin atmosphere at the limb. Home is at the centre;
// turning the knob spins the globe, and a few seconds after the last turn it glides back.
// The two pictures live on the SD card in /earth (day.jpg, night.jpg); see
// tools/gen_earth_textures.py.
#include <lvgl.h>

namespace globeview {
void      init();
lv_obj_t *screen();
void      onEnter(double homeLat, double homeLon);   // loads the pictures, builds the view
void      onExit();                                  // gives every buffer back
void      onTurn(int delta);
}
