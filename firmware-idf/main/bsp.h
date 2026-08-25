#pragma once
#include <stdint.h>
#include "lvgl.h"
#include "driver/i2c_master.h"

// The shared I2C master bus (touch + ES8311 codec). Valid after
// bsp_display_start(); audio.c adds the codec device to it.
i2c_master_bus_handle_t bsp_i2c_bus(void);

// Backlight duty 0..100% (LEDC PWM).
void bsp_set_backlight(uint8_t pct);

// Apply a display orientation (0 landscape … 3 portrait-flipped) at runtime.
// Call under the LVGL port lock; follow with a UI rebuild for the new resolution.
void bsp_apply_orientation(uint8_t orient);

#ifdef MMK_DISPLAY_ROTATE_90
// Rotate a native-portrait panel to landscape: both the LVGL display AND the
// touch controller (the esp_lvgl_port touch cb doesn't follow the display
// rotation). Call once under the LVGL port lock after bsp_display_start().
void bsp_rotate_landscape(void);
#endif

// Brings up SPI display (ILI9341V), I2C touch (FT6336/FT5x06), and esp_lvgl_port.
// Returns the LVGL display. UI code must wrap LVGL calls in
// lvgl_port_lock()/lvgl_port_unlock() since esp_lvgl_port runs its own task.
lv_display_t *bsp_display_start(void);
