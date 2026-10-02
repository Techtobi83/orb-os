// Portable LVGL boot/hello screen (M0). Shared by the device and the SDL sim.
// Pure LVGL — uptime comes from lv_tick_get() so it works on both targets.
#include "ui_boot.h"
#include <lvgl.h>
#include <stdio.h>

static void uptime_timer_cb(lv_timer_t *t) {
    lv_obj_t *label = (lv_obj_t *)t->user_data;
    char buf[40];
    snprintf(buf, sizeof(buf), "uptime %lus", (unsigned long)(lv_tick_get() / 1000));
    lv_label_set_text(label, buf);
}

void ui_boot_create(void) {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);   // true black for AMOLED
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    const lv_color_t green = lv_color_hex(0x39FF14);        // phosphor green (mockup palette)

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "PLANE RADAR 2.0");
    lv_obj_set_style_text_color(title, green, 0);
    lv_obj_set_style_text_font(title, &font_de_28, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -52);

    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, "M0  -  display + LVGL OK");
    lv_obj_set_style_text_color(sub, lv_color_hex(0x80FF80), 0);
    lv_obj_set_style_text_font(sub, &font_de_16, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, -14);

    lv_obj_t *up = lv_label_create(scr);
    lv_label_set_text(up, "uptime 0s");
    lv_obj_set_style_text_color(up, lv_color_hex(0x3CE0FF), 0);
    lv_obj_align(up, LV_ALIGN_CENTER, 0, 16);
    lv_timer_create(uptime_timer_cb, 500, up);

    // Animated spinner = proof the LVGL tick + render loop are running.
    lv_obj_t *spin = lv_spinner_create(scr, 1200, 50);
    lv_obj_set_size(spin, 96, 96);
    lv_obj_align(spin, LV_ALIGN_CENTER, 0, 96);
    lv_obj_set_style_arc_color(spin, lv_color_hex(0x0E2A0E), LV_PART_MAIN);
    lv_obj_set_style_arc_color(spin, green, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spin, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spin, 6, LV_PART_INDICATOR);
}
