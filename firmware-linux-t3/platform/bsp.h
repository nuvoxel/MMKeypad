/* Linux bsp.h -- shadows the ESP-IDF firmware-idf/main/bsp.h (which pulls in
 * driver/i2c_master.h, esp_lvgl_port, etc). The shared app modules only use
 * two bsp entry points; the Linux display/touch bring-up lives in
 * main_linux.c via lv_linux_fbdev, not here. Put platform/ ahead of
 * firmware-idf/main on the include path so this wins. */
#pragma once
#include <stdint.h>
#include "lvgl.h"

/* Backlight duty 0..100% -> /sys/class/backlight/rk28_bl/brightness (0..255). */
void bsp_set_backlight(uint8_t pct);

/* Apply a display orientation (0 landscape .. 3 portrait-flipped). On the T3
 * the fb is fixed 800x1280 portrait shown landscape; maps to LVGL rotation. */
void bsp_apply_orientation(uint8_t orient);

/* Create the T3's full-frame rotating fbdev display (see bsp_linux.c). Renders
 * landscape 1280x800 and rotates the whole frame into the 800x1280 panel each
 * flush -- avoids LVGL fbdev's partial-rotation corruption. */
lv_display_t *bsp_display_create(void);

/* Block until the panel's vblank (pace the render loop; tear reduction).
 * Returns nonzero if the driver supports the wait. */
int bsp_wait_vsync(void);
