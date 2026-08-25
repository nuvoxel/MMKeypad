// bsp_matrix_disp.c — LVGL display wiring for the s3_matrix board (C half).
//
// Implements the bsp.h contract main.c uses: bring up LVGL via esp_lvgl_port,
// create a 64x64 display whose flush pushes pixels into the HUB75 panel (the
// C++ half, bsp_matrix.cpp, via bsp_matrix.h), and return the lv_display_t.
// This stays in C on purpose — esp_lvgl_port.h pulls esp_lcd headers that won't
// compile under C++ (extern-"C" overload conflict), so the panel driver is
// isolated behind the plain-C wrapper in bsp_matrix.h.

#include "bsp.h"
#include "bsp_matrix.h"
#include "board.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

static const char *TAG = "bsp_matrix_disp";
static lv_display_t *s_disp;

// One full-frame LVGL render buffer (64*64*2 = 8 KB). RENDER_MODE_FULL means the
// flush always covers the whole panel, so the flush is a single blit.
static uint16_t s_lvbuf[MATRIX_WIDTH * MATRIX_HEIGHT];

// LVGL (esp_lvgl_port task) -> HUB75. The rendered buffer is native RGB565; the
// panel blit is synchronous (draw_pixels copies into the DMA buffer), so signal
// flush-ready immediately.
static void matrix_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const uint16_t w = area->x2 - area->x1 + 1;
    const uint16_t h = area->y2 - area->y1 + 1;
    matrix_panel_draw(area->x1, area->y1, w, h, px_map, MATRIX_RGB565_BE);
    lv_display_flush_ready(disp);
}

lv_display_t *bsp_display_start(void)
{
    if (!matrix_panel_init()) return NULL;   // HUB75 DMA panel first

    // LVGL runtime (task + tick + lock), same as the LCD boards — but we register
    // our own display below instead of lvgl_port_add_disp() (no esp_lcd panel).
    lvgl_port_cfg_t pcfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&pcfg));

    if (lvgl_port_lock(0)) {
        s_disp = lv_display_create(MATRIX_WIDTH, MATRIX_HEIGHT);
        lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
        lv_display_set_flush_cb(s_disp, matrix_flush);
        lv_display_set_buffers(s_disp, s_lvbuf, NULL, sizeof(s_lvbuf),
                               LV_DISPLAY_RENDER_MODE_FULL);
        lvgl_port_unlock();
    }
    ESP_LOGI(TAG, "LVGL display %dx%d -> HUB75", MATRIX_WIDTH, MATRIX_HEIGHT);
    return s_disp;
}

// Driver "brightness" (0..100%) maps to the panel intensity multiplier (0..1).
void bsp_set_backlight(uint8_t pct)
{
    if (pct > 100) pct = 100;
    matrix_panel_set_intensity((float)pct / 100.0f);
}

// The driver's runtime display-orientation field. A 64x64 square LED panel gains
// nothing from rotation (and LVGL stays 64x64), so this is a no-op — orientation
// is a wiring/build-time concern here. Kept to satisfy the bsp.h contract that
// main.c calls into.
void bsp_apply_orientation(uint8_t orient) { (void)orient; }
