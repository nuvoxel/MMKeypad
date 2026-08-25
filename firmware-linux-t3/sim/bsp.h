/* Sim bsp.h -- shadows firmware-idf/main/bsp.h (which pulls driver/i2c_master.h,
 * esp_lvgl_port, etc.) and the T3 platform/bsp.h. The shared UI (ui.c) calls
 * exactly two bsp entry points; both are no-ops in the sim (backlight is
 * meaningless, and rotation is chosen up-front by render_main via the display
 * size, not applied live). Put -I. first so this wins. */
#pragma once
#include <stdint.h>

void bsp_set_backlight(uint8_t pct);
void bsp_apply_orientation(uint8_t orient);
