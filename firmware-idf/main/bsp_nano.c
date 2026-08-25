// MMKeypad — board support for the Waveshare ESP32-P4-NANO (KIT-D 10.1" DSI).
// Unlike the other boards, the heavy lifting lives in Waveshare's OFFICIAL BSP
// component (waveshare/esp32_p4_nano): panel init (JD9365 et al, chosen via the
// BSP's Kconfig), GT911 touch, esp_lvgl_port wiring, and the I2C-controlled
// backlight. Its bsp_display_start() has the exact prototype our bsp.h declares,
// so that symbol links straight through — this file only adapts the rest of the
// bsp.h API onto the vendor BSP.

#include "bsp.h"
#include "board.h"
#include "bsp/esp32_p4_nano.h"   // Waveshare P4 BSP (multi-panel via Kconfig)
#include "esp_lcd_touch.h"       // flip the touch controller to cancel a 180° offset
#include "lvgl.h"

// bsp_display_start(): provided by the vendor BSP (esp32_p4_nano.c) — it inits
// lvgl_port + panel + touch and leaves the backlight OFF, matching main.c's
// expectation of raising it after the first UI draw.

i2c_master_bus_handle_t bsp_i2c_bus(void)
{
    // The vendor BSP owns the bus (created during display/touch init); the
    // ES8311 shares it (audio.c display-board path).
    return bsp_i2c_get_handle();
}

void bsp_set_backlight(uint8_t pct)
{
    // 10.1" KIT-D backlight is an I2C controller on the panel FPC (@0x45),
    // driven by the vendor BSP.
    bsp_display_brightness_set(pct);
}

void bsp_apply_orientation(uint8_t orient)
{
    // The 10" is always landscape (bsp_rotate_landscape at boot); no per-room
    // reorientation on this board.
    (void)orient;
}

#ifdef MMK_DISPLAY_ROTATE_90
void bsp_rotate_landscape(void)
{
    // Rotate LVGL 90° → the logical resolution reads 1280x800 landscape. LVGL
    // core's indev_pointer_proc rotates the raw touch point by the same 90° on
    // every read, so display + touch share this rotation automatically.
    lv_display_t *d = lv_display_get_default();
    if (d) lv_disp_set_rotation(d, LV_DISPLAY_ROTATION_90);

    // ...but on THIS unit that leaves touch a clean 180° off the display
    // (measured: tap top-left → registered bottom-right, all four corners
    // inverted). The Waveshare BSP's native touch config (mirror_x=1,mirror_y=1)
    // doesn't match this panel/digitizer pairing. Flip BOTH mirrors: a 180°
    // point-inversion commutes with the 90° rotation, so it cancels the offset
    // exactly (verified by re-measuring the corners). swap_xy stays 0.
    lv_indev_t *indev = bsp_display_get_input_dev();
    if (indev) {
        void *drv = lv_indev_get_driver_data(indev);
        if (drv) {
            esp_lcd_touch_handle_t tp = *(esp_lcd_touch_handle_t *)drv;
            if (tp) {
                esp_lcd_touch_set_swap_xy(tp, false);
                esp_lcd_touch_set_mirror_x(tp, false);
                esp_lcd_touch_set_mirror_y(tp, false);
            }
        }
    }
}
#endif
