#pragma once
#include <lvgl.h>
// "Vorhersage": the forecast as a dial. A 24-hour ring round the edge with the daylight lit
// and a sun or moon at the current time; the weather's symbol large and faint behind the
// temperature; tomorrow in a capsule at the foot. German text. Reads the Open-Meteo snapshot
// the network task already fetches every WEATHER_REFRESH_MS (weather.h), so it costs no
// requests of its own, and its symbols are alpha images in flash, so it holds no PSRAM.
//
// Not yet themeable: it has no Orb Studio half (docs/adding-a-screen.md). Its look is the
// constants at the top of forecast_view.cpp.
namespace forecastview {
    void      init();         // build the screen (hidden until the shell shows it)
    lv_obj_t* screen();       // hand to app_shell::add
    void      onEnter();      // redraw from the latest snapshot and the clock
    void      onExit();
    void      onWeather();    // a new snapshot was stored (loop task)
}
