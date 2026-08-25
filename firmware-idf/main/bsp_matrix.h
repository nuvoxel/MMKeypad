// bsp_matrix.h — thin C-callable wrapper around the C++ HUB75 driver
// (esphome/esp-hub75). The LVGL display wiring lives in bsp_matrix_disp.c (C);
// keeping the esp_lvgl_port/esp_lcd headers out of the C++ translation unit
// avoids the esp_lcd_io_i2c.h extern-"C"-overload conflict that breaks C++.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bring up the HUB75 DMA panel (pins/dims from board.h). Returns false on failure.
bool matrix_panel_init(void);

// Blit a tightly-packed RGB565 block into the panel. `big_endian` selects the
// buffer byte order (see MATRIX_RGB565_BE). Synchronous (copies into the DMA buffer).
void matrix_panel_draw(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                       const uint8_t *rgb565, bool big_endian);

// Panel brightness as an intensity multiplier (0.0–1.0).
void matrix_panel_set_intensity(float intensity);

#ifdef __cplusplus
}
#endif
