// bsp_matrix.cpp — HUB75 DMA driver (esphome/esp-hub75, MIT) for the s3_matrix
// board. This is the C++ half: it owns the Hub75Driver and exposes a small
// C-callable surface (bsp_matrix.h). The LVGL display + flush wiring is in the
// C file bsp_matrix_disp.c — deliberately, so the esp_lvgl_port/esp_lcd headers
// (which have an extern-"C" overload that won't compile under C++) never reach
// this translation unit. Single 64x64 panel ≈ 57 KB internal SRAM (GDMA on S3).

#include "hub75.h"            // esphome/esp-hub75 C++ API

extern "C" {
#include "bsp_matrix.h"
#include "board.h"
#include "esp_log.h"
}

static const char *TAG = "bsp_matrix";
static Hub75Driver *s_panel;

extern "C" bool matrix_panel_init(void)
{
    Hub75Config cfg{};
    cfg.panel_width  = MATRIX_WIDTH;
    cfg.panel_height = MATRIX_HEIGHT;
    cfg.scan_wiring  = Hub75ScanWiring::STANDARD_TWO_SCAN;   // typical 64x64 wiring
#if MATRIX_SHIFT_FM6126A
    cfg.shift_driver = Hub75ShiftDriver::FM6126A;
#else
    cfg.shift_driver = Hub75ShiftDriver::GENERIC;
#endif
    cfg.pins.r1 = PIN_HUB75_R1; cfg.pins.g1 = PIN_HUB75_G1; cfg.pins.b1 = PIN_HUB75_B1;
    cfg.pins.r2 = PIN_HUB75_R2; cfg.pins.g2 = PIN_HUB75_G2; cfg.pins.b2 = PIN_HUB75_B2;
    cfg.pins.a = PIN_HUB75_A; cfg.pins.b = PIN_HUB75_B; cfg.pins.c = PIN_HUB75_C;
    cfg.pins.d = PIN_HUB75_D; cfg.pins.e = PIN_HUB75_E;
    cfg.pins.clk = PIN_HUB75_CLK; cfg.pins.lat = PIN_HUB75_LAT; cfg.pins.oe = PIN_HUB75_OE;

    s_panel = new Hub75Driver(cfg);
    if (!s_panel->begin()) {
        ESP_LOGE(TAG, "HUB75 begin() failed — check pins/power");
        return false;
    }
    s_panel->clear();
    s_panel->set_intensity(0.8f);   // sane default until the driver sets brightness
    ESP_LOGI(TAG, "HUB75 %dx%d up", MATRIX_WIDTH, MATRIX_HEIGHT);
    return true;
}

extern "C" void matrix_panel_draw(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                  const uint8_t *rgb565, bool big_endian)
{
    if (!s_panel) return;
    s_panel->draw_pixels(x, y, w, h, rgb565,
                         Hub75PixelFormat::RGB565, Hub75ColorOrder::RGB, big_endian);
}

extern "C" void matrix_panel_set_intensity(float intensity)
{
    if (s_panel) s_panel->set_intensity(intensity);
}
