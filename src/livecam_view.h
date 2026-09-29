#pragma once
#include <lvgl.h>
// "Live Cam" app for the shell: one network camera, full screen. The source is an MJPEG
// stream or a snapshot URL set in src/secrets.h; without that file the app is not built
// (LIVECAM_ENABLED in config.h). The connection exists only while this app is the current
// one, so it costs nothing anywhere else (see livecam_view.cpp).
namespace livecamview {
    void      init();      // parse the URL, build the screen, start the frame timer
    lv_obj_t* screen();    // the app's LVGL screen (hand to app_shell::add)
    void      onEnter();   // open the stream and take the canvas
    void      onExit();    // close the stream, give the canvas and frame buffers back
}
