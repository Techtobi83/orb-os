#pragma once
#include <lvgl.h>
// "Livestream" app for the shell: one network camera, full screen. The source is an MJPEG
// stream or a snapshot URL, set in Settings > Livestream or at http://theorb.local/livestream
// and kept in NVS by main.cpp. The connection exists only while this app is the current one,
// so it costs nothing anywhere else (see livecam_view.cpp).
namespace livecamview {
    void      init(const char *url);   // build the screen, start the (sleeping) stream task
    lv_obj_t* screen();                // the app's LVGL screen (hand to app_shell::add)
    void      onEnter();               // open the stream and take the canvas
    void      onExit();                // close the stream, give the canvas and frame buffers back
    // Use a new URL from now on; "" means none. Safe from any thread, and takes effect at
    // once if the stream is open. Does not save it: main.cpp's host_livestream_url_set does.
    void      setUrl(const char *url);
}
