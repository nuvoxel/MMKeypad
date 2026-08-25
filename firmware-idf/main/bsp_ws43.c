// MMKeypad — board support for the Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3.
//   480x800 PORTRAIT ST7701 panel over 2-lane MIPI-DSI (espressif/esp_lcd_st7701,
//   DPI video mode), GT911 cap touch on a shared I2C bus, backlight on a direct
//   P4 GPIO (LEDC PWM — but INVERTED on this board, see backlight_init). ES8311 +
//   ES7210 audio share the touch I2C bus (audio wiring lives in audio.c; not
//   touched here — first bring-up is display + touch only). WiFi is an onboard
//   ESP32-C6 (esp_hosted) — left out until the display path is proven.
//
// Exposes the same bsp.h API as the S3 SPI bsp.c / crowpanel bsp so main.c / ui.c
// stay board-agnostic. Panel config (pins/timings/ST7701 init cmds)
// mirrors Waveshare's factory BSP (esp32_p4_wifi6_touch_lcd_4_3), the proven
// bring-up for this exact unit.

#include "bsp.h"
#include "board.h"
#include "config.h"

#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_st7701.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp";

static i2c_master_bus_handle_t s_i2c;     // GT911 touch (+ ES8311/ES7210 codecs)
static lv_display_t           *s_disp;

i2c_master_bus_handle_t bsp_i2c_bus(void) { return s_i2c; }

// ── ST7701 vendor-specific init sequence (verbatim from Waveshare's factory BSP:
//    components/esp32_p4_wifi6_touch_lcd_4_3 — vendor_specific_init_default[]).
//    Format: {cmd, {params...}, param_count, delay_ms}. 39 entries, ending in
//    sleep-out (0x11, 120 ms) + display-on (0x29). ──────────────────────────────
static const st7701_lcd_init_cmd_t st7701_init_cmds[] = {
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xEF, (uint8_t[]){0x08}, 1, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    {0xC0, (uint8_t[]){0x63, 0x00}, 2, 0},
    {0xC1, (uint8_t[]){0x0D, 0x02}, 2, 0},
    {0xC2, (uint8_t[]){0x17, 0x08}, 2, 0},
    {0xCC, (uint8_t[]){0x10}, 1, 0},
    {0xB0, (uint8_t[]){0x40, 0xC9, 0x94, 0x0E, 0x10, 0x05, 0x0B, 0x09, 0x08, 0x26, 0x04, 0x52, 0x10, 0x69, 0x6B, 0x69}, 16, 0},
    {0xB1, (uint8_t[]){0x40, 0xD2, 0x98, 0x0C, 0x92, 0x07, 0x09, 0x08, 0x07, 0x25, 0x02, 0x0E, 0x0C, 0x6E, 0x78, 0x55}, 16, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
    {0xB0, (uint8_t[]){0x5D}, 1, 0},
    {0xB1, (uint8_t[]){0x4E}, 1, 0},
    {0xB2, (uint8_t[]){0x87}, 1, 0},
    {0xB3, (uint8_t[]){0x80}, 1, 0},
    {0xB5, (uint8_t[]){0x4E}, 1, 0},
    {0xB7, (uint8_t[]){0x85}, 1, 0},
    {0xB8, (uint8_t[]){0x21}, 1, 0},
    {0xB9, (uint8_t[]){0x10, 0x1F}, 2, 0},
    {0xBB, (uint8_t[]){0x03}, 1, 0},
    {0xBC, (uint8_t[]){0x00}, 1, 0},
    {0xC1, (uint8_t[]){0x78}, 1, 0},
    {0xC2, (uint8_t[]){0x78}, 1, 0},
    {0xD0, (uint8_t[]){0x88}, 1, 0},
    {0xE0, (uint8_t[]){0x00, 0x3A, 0x02}, 3, 0},
    {0xE1, (uint8_t[]){0x04, 0xA0, 0x00, 0xA0, 0x05, 0xA0, 0x00, 0xA0, 0x00, 0x40, 0x40}, 11, 0},
    {0xE2, (uint8_t[]){0x30, 0x00, 0x40, 0x40, 0x32, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00}, 13, 0},
    {0xE3, (uint8_t[]){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE4, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE5, (uint8_t[]){0x09, 0x2E, 0xA0, 0xA0, 0x0B, 0x30, 0xA0, 0xA0, 0x05, 0x2A, 0xA0, 0xA0, 0x07, 0x2C, 0xA0, 0xA0}, 16, 0},
    {0xE6, (uint8_t[]){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE7, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE8, (uint8_t[]){0x08, 0x2D, 0xA0, 0xA0, 0x0A, 0x2F, 0xA0, 0xA0, 0x04, 0x29, 0xA0, 0xA0, 0x06, 0x2B, 0xA0, 0xA0}, 16, 0},
    {0xEB, (uint8_t[]){0x00, 0x00, 0x4E, 0x4E, 0x00, 0x00, 0x00}, 7, 0},
    {0xEC, (uint8_t[]){0x08, 0x01}, 2, 0},
    {0xED, (uint8_t[]){0xB0, 0x2B, 0x98, 0xA4, 0x56, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xF7, 0x65, 0x4A, 0x89, 0xB2, 0x0B}, 16, 0},
    {0xEF, (uint8_t[]){0x08, 0x08, 0x08, 0x45, 0x3F, 0x54}, 6, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},
    {0x11, (uint8_t[]){0x00}, 0, 120},   // sleep out
    {0x29, (uint8_t[]){0x00}, 0, 0},     // display on
};

// ── Backlight: direct GPIO, LEDC PWM. Waveshare drives this INVERTED (the LEDC
//    output is inverted at the channel), 10-bit @ 5 kHz — reproduce both or the
//    backlight reads backwards (0% = full bright). ──────────────────────────────
static void backlight_init(void)
{
    ledc_timer_config_t tcfg = {
        .clk_cfg = LEDC_AUTO_CLK,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz = LCD_BL_PWM_HZ,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&tcfg));
    ledc_channel_config_t ccfg = {
        .gpio_num = PIN_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = 1,   // factory BSP inverts the backlight output
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ccfg));
}

void bsp_set_backlight(uint8_t pct)
{
    if (pct > 100) pct = 100;
    uint32_t duty = (1023 * pct) / 100;   // 10-bit; the channel inverts it in HW
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

// Map the saved orientation (0 landscape … 3 portrait-flipped) to an LVGL
// software rotation. Native panel is portrait (480x800): portrait = 0°,
// landscape = 90°. Same convention as the S3 bsp so the driver's Display
// Orientation values (Landscape=0, Portrait=1, …) behave identically here.
static lv_display_rotation_t orient_to_rot(uint8_t o)
{
    switch (o) {
        case 1:  return LV_DISPLAY_ROTATION_0;     // portrait (native)
        case 2:  return LV_DISPLAY_ROTATION_270;   // landscape flipped
        case 3:  return LV_DISPLAY_ROTATION_180;   // portrait flipped
        default: return LV_DISPLAY_ROTATION_90;    // 0 = landscape
    }
}

void bsp_apply_orientation(uint8_t orient)
{
    // sw_rotate is enabled on the DSI display (see bsp_display_start), so LVGL
    // rotates the rendered areas into the fixed portrait framebuffer. Touch is
    // rotated to match by esp_lvgl_port's read path. main.c calls this + a UI
    // rebuild whenever the driver/web changes Display Orientation.
    if (s_disp) lv_display_set_rotation(s_disp, orient_to_rot(orient));
}

// ── I2C bus (shared: GT911 touch + ES8311/ES7210 codecs) ─────────────────────
static void i2c_start(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = TOUCH_I2C_PORT,
        .sda_io_num = PIN_TOUCH_SDA,
        .scl_io_num = PIN_TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_i2c));
}

// ── 2-lane MIPI-DSI panel (ST7701, DPI video mode) ───────────────────────────
static esp_lcd_dsi_bus_handle_t  s_dsi_bus;
static esp_lcd_panel_io_handle_t s_dbi_io;

static esp_lcd_panel_handle_t dsi_panel_start(void)
{
    // The MIPI D-PHY is powered from an on-chip LDO (VDD_MIPI_DPHY = LDO_VO3) —
    // without this the bus is dead and panel init fails silently.
    static esp_ldo_channel_handle_t phy_ldo;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = DSI_PHY_LDO_CHAN,
        .voltage_mv = DSI_PHY_LDO_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &phy_ldo));

    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = DSI_LANES,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = DSI_LANE_MBPS,
    };
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_cfg, &s_dsi_bus));

    esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(s_dsi_bus, &dbi_cfg, &s_dbi_io));

    // RGB888, not RGB565. At 16bpp the X4 home gradient (0x312B63 -> 0x235E97 down
    // 800px) banded badly: 5-bit red gives only THREE distinct values over the whole
    // screen (~266px bands), 5-bit blue seven. Confirmed by eye on this panel. The
    // glass is true 24-bit, so 888 renders it smooth. DSI budget allows it:
    // 30 MHz DPI clk x 24 bits = 720 Mbps against 2 lanes x 500 = 1000 Mbps.
    static const esp_lcd_dpi_panel_config_t dpi_cfg = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = DSI_DPI_CLK_MHZ,
        .virtual_channel = 0,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB888,
        .num_fbs = 1,
        .video_timing = {
            .h_size = LCD_WIDTH,
            .v_size = LCD_HEIGHT,
            .hsync_pulse_width = DSI_HSYNC_PULSE,
            .hsync_back_porch = DSI_HBP,
            .hsync_front_porch = DSI_HFP,
            .vsync_pulse_width = DSI_VSYNC_PULSE,
            .vsync_back_porch = DSI_VBP,
            .vsync_front_porch = DSI_VFP,
        },
        .flags.use_dma2d = true,   // 2D-DMA copies draw buffer → FB off-CPU
    };
    // The ST7701 driver carries the init cmds AND the DSI/DPI config in its
    // vendor config (use_mipi_interface tells it to talk over DSI, not SPI).
    st7701_vendor_config_t vendor_cfg = {
        .init_cmds = st7701_init_cmds,
        .init_cmds_size = sizeof(st7701_init_cmds) / sizeof(st7701_lcd_init_cmd_t),
        .flags = {
            .use_mipi_interface = 1,
        },
        .mipi_config = {
            .dsi_bus = s_dsi_bus,
            .dpi_config = &dpi_cfg,
        },
    };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 24,   // 24bpp: this glass is true RGB888 (verified on hw)
        .vendor_config = &vendor_cfg,
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7701(s_dbi_io, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    return panel;
}

// ── GT911 capacitive touch (on the shared I2C bus; INT not connected → polled) ─
static esp_lcd_touch_handle_t touch_start(void)
{
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
        .control_phase_bytes = 1,
        .dc_bit_offset = 0,
        .lcd_cmd_bits = 16,
        .flags = { .disable_control_phase = 1 },
        .scl_speed_hz = 400000,
    };
    if (esp_lcd_new_panel_io_i2c(s_i2c, &io_cfg, &io) != ESP_OK) {
        ESP_LOGE(TAG, "GT911 panel io failed");
        return NULL;
    }
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_WIDTH,
        .y_max = LCD_HEIGHT,
        .rst_gpio_num = PIN_TOUCH_RST,
        .int_gpio_num = PIN_TOUCH_INT,   // -1 (NC) → esp_lcd_touch polls the GT911
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
    };
    // The GT911's first probe after power-on often NAKs (it's still completing its
    // own reset/address sequence). Give it a few attempts with a delay — same
    // first-failure-then-succeed behaviour seen on the CrowPanel GT911.
    esp_lcd_touch_handle_t tp = NULL;
    for (int attempt = 1; attempt <= 4; attempt++) {
        if (esp_lcd_touch_new_i2c_gt911(io, &tp_cfg, &tp) == ESP_OK) {
            ESP_LOGI(TAG, "GT911 up (attempt %d)", attempt);
            return tp;
        }
        ESP_LOGW(TAG, "GT911 init attempt %d failed; retrying", attempt);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    ESP_LOGE(TAG, "GT911 init failed after retries");
    return NULL;
}

lv_display_t *bsp_display_start(void)
{
    i2c_start();
    backlight_init();
    bsp_set_backlight(0);                 // up after UI is drawn

    esp_lcd_panel_handle_t panel = dsi_panel_start();
    esp_lcd_touch_handle_t tp = touch_start();

    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_stack = 8192 * 2;       // big panels need a deeper LVGL task stack
    lvgl_cfg.task_max_sleep_ms = 10;      // snappier touch (matches crowpanel7)
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    lvgl_port_display_cfg_t disp_cfg = {
        .panel_handle = panel,
        .control_handle = panel,
        // DPI path: a SMALL partial draw buffer in INTERNAL DMA RAM (50 lines),
        // single-buffered; DMA2D blits each dirty area into the PSRAM framebuffer.
        // A full-screen PSRAM draw buffer here corrupts the heap adjacent to the
        // FB (the same PSRAM-draw-buffer lesson as the crowpanel7 DSI path).
        .buffer_size = LCD_WIDTH * 50,
        .double_buffer = false,
        .hres = LCD_WIDTH,
        .vres = LCD_HEIGHT,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB888,
        .rotation = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        .flags = {
            // RGB888: esp_lvgl_port refuses a DMA draw buffer for anything but
            // RGB565 ("DMA buffer can be used only in display color format
            // RGB565"), so this path uses a plain internal-RAM buffer.
            .buff_dma = false,
            .buff_spiram = false,   // internal RAM (see buffer_size note)
            .sw_rotate = true,      // LVGL rotates areas → supports landscape (driver Display Orientation)
            // DPI panels scan the framebuffer out natively 16 bits at a time, so
            // LVGL's RGB565 maps straight through — NO byte swap (that's an
            // SPI-panel thing; enabling it here scrambles the colors).
            .swap_bytes = false,
        },
    };
    lvgl_port_display_dsi_cfg_t dsi_cfg = { .flags = { .avoid_tearing = false } };
    s_disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);

    // Apply the persisted orientation (driver/web Display Orientation, default
    // portrait) now that the display exists — comes up landscape if so configured.
    lv_display_set_rotation(s_disp, orient_to_rot(g_settings.orientation));

    if (tp) {
        lvgl_port_touch_cfg_t touch_cfg = { .disp = s_disp, .handle = tp };
        lvgl_port_add_touch(&touch_cfg);
    }

    ESP_LOGI(TAG, "%s display up %dx%d + GT911 touch%s",
             MMK_BOARD_NAME, LCD_WIDTH, LCD_HEIGHT, tp ? "" : " (FAILED)");
    return s_disp;
}
