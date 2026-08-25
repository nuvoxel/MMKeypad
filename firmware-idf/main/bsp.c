#include "bsp.h"
#include "board.h"
#include "config.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp";

static lv_display_t *s_disp;   // for runtime orientation changes

// Map the saved orientation (0 landscape … 3 portrait-flipped) to an LVGL software
// rotation. Touch is reported in the native panel frame, so LVGL re-rotates input
// to match — no per-orientation touch calibration needed (landscape was verified).
static lv_display_rotation_t orient_to_rot(uint8_t o)
{
    switch (o & 3) {
        case 1:  return LV_DISPLAY_ROTATION_0;     // portrait
        case 2:  return LV_DISPLAY_ROTATION_270;   // landscape flipped
        case 3:  return LV_DISPLAY_ROTATION_180;   // portrait flipped
        default: return LV_DISPLAY_ROTATION_90;    // 0 = landscape (default)
    }
}

void bsp_apply_orientation(uint8_t orient)
{
    if (s_disp) lv_display_set_rotation(s_disp, orient_to_rot(orient));
}

// ── FT6336 touch (direct) ───────────────────────────────────────────────────
// The generic esp_lcd_touch_ft5x06 driver writes FT5x06-specific threshold/config
// registers at init; on the FT6336 those addresses differ and it stops scanning
// (reads succeed but always report 0 points). The proven approach for this panel
// (from the Arduino build) is dead simple: reset + read reg 0x02 for the point
// count and the 4 coordinate bytes. No config writes.
#define FT6336_ADDR        0x38
#define FT6336_REG_STATUS  0x02   // [3:0] = number of touch points; then P1 XH/XL/YH/YL
static i2c_master_dev_handle_t s_tdev;
static i2c_master_bus_handle_t s_i2c_bus;   // shared: touch + ES8311 codec

i2c_master_bus_handle_t bsp_i2c_bus(void) { return s_i2c_bus; }

// Read one point. Returns true if pressed; rawx/rawy are in the controller's
// NATIVE 240x320 portrait frame (0..239 / 0..319).
static bool ft6336_read(uint16_t *rawx, uint16_t *rawy)
{
    uint8_t reg = FT6336_REG_STATUS;
    uint8_t b[5] = {0};
    if (i2c_master_transmit_receive(s_tdev, &reg, 1, b, sizeof(b), pdMS_TO_TICKS(50)) != ESP_OK) {
        return false;
    }
    if ((b[0] & 0x0F) == 0) return false;
    *rawx = (((uint16_t)(b[1] & 0x0F)) << 8) | b[2];
    *rawy = (((uint16_t)(b[3] & 0x0F)) << 8) | b[4];
    return true;
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    static int32_t last_x = 0, last_y = 0;
    uint16_t rx = 0, ry = 0;
    if (ft6336_read(&rx, &ry)) {
        // The FT6336 reports in native pixels (rawY 0..319, rawX 0..239), so map
        // directly like the Arduino build: landscape x = rawY, y = 239 - rawX.
        int lx = (int)ry;
        int ly = 239 - (int)rx;
        if (lx < 0) lx = 0; else if (lx > 319) lx = 319;
        if (ly < 0) ly = 0; else if (ly > 239) ly = 239;
        // Pre-rotate: LVGL re-rotates indev coords for a ROTATION_90 display
        // (x = ver_res - y - 1, y = x; ver_res = 320), so feed native-frame coords;
        // its rotation then yields (rawY, 239-rawX) — same as the Arduino build.
        int px = ly, py = 319 - lx;
        last_x = px; last_y = py;
        data->point.x = px; data->point.y = py;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->point.x = last_x; data->point.y = last_y;
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// ── ILI9341V vendor init sequence ───────────────────────────────────────────
// Verbatim power/gamma setup from the lcdwiki vendor SDK (proven on this panel).
static const ili9341_lcd_init_cmd_t ili9341v_init[] = {
    {0xCF, (uint8_t[]){0x00, 0xC1, 0x30}, 3, 0},
    {0xED, (uint8_t[]){0x64, 0x03, 0x12, 0x81}, 4, 0},
    {0xE8, (uint8_t[]){0x85, 0x00, 0x78}, 3, 0},
    {0xCB, (uint8_t[]){0x39, 0x2C, 0x00, 0x34, 0x02}, 5, 0},
    {0xF7, (uint8_t[]){0x20}, 1, 0},
    {0xEA, (uint8_t[]){0x00, 0x00}, 2, 0},
    {0xC0, (uint8_t[]){0x13}, 1, 0},
    {0xC1, (uint8_t[]){0x13}, 1, 0},
    {0xC5, (uint8_t[]){0x22, 0x35}, 2, 0},
    {0xC7, (uint8_t[]){0xBD}, 1, 0},
    {0xB6, (uint8_t[]){0x0A, 0x82}, 2, 0},
    {0xF6, (uint8_t[]){0x01, 0x30}, 2, 0},
    {0xB1, (uint8_t[]){0x00, 0x1B}, 2, 0},
    {0xF2, (uint8_t[]){0x00}, 1, 0},
    {0x26, (uint8_t[]){0x01}, 1, 0},
    {0xE0, (uint8_t[]){0x0F, 0x35, 0x31, 0x0B, 0x0E, 0x06, 0x49, 0xA7,
                       0x33, 0x07, 0x0F, 0x03, 0x0C, 0x0A, 0x00}, 15, 0},
    {0xE1, (uint8_t[]){0x00, 0x0A, 0x0F, 0x04, 0x11, 0x08, 0x36, 0x58,
                       0x4D, 0x07, 0x10, 0x0C, 0x32, 0x34, 0x0F}, 15, 0},
};

static void backlight_on(void)
{
    // LEDC PWM on the backlight pin (active HIGH) so the UI can dim for screensaver.
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&t);
    ledc_channel_config_t c = {
        .gpio_num = PIN_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 255,
        .hpoint = 0,
    };
    ledc_channel_config(&c);
}

void bsp_set_backlight(uint8_t pct)
{
    if (pct > 100) pct = 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (uint32_t)pct * 255 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static esp_lcd_panel_handle_t panel_start(esp_lcd_panel_io_handle_t *out_io)
{
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_LCD_SCK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_LCD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * 40 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = PIN_LCD_DC,
        .pclk_hz = LCD_SPI_FREQ_HZ,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
                                             &io_cfg, &io));

    ili9341_vendor_config_t vendor = {
        .init_cmds = ili9341v_init,
        .init_cmds_size = sizeof(ili9341v_init) / sizeof(ili9341v_init[0]),
    };
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,            // -1: tied to chip EN
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io, &panel_cfg, &panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));  // IPS
    // Native 240x320 portrait; landscape via software rotation (see below).
    // mirror_y flips the native long axis = landscape X (corrects L/R mirror).
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, false, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    *out_io = io;
    return panel;
}

static void touch_start(void)
{
    // Shared I2C bus (touch + ES8311 later). Created here.
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = TOUCH_I2C_PORT,
        .sda_io_num = PIN_TOUCH_SDA,
        .scl_io_num = PIN_TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c_bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus));
    s_i2c_bus = i2c_bus;   // shared with the ES8311 codec (audio.c)

    // Reset the FT6336: pulse RST low, then wait ~300ms for it to boot (the
    // generic driver only waited 10ms — too short for this controller).
    gpio_config_t rst = {.pin_bit_mask = 1ULL << PIN_TOUCH_RST, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&rst);
    gpio_set_level(PIN_TOUCH_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(300));

    gpio_config_t intp = {.pin_bit_mask = 1ULL << PIN_TOUCH_INT, .mode = GPIO_MODE_INPUT};
    gpio_config(&intp);

    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = FT6336_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus, &dev, &s_tdev));
}

lv_display_t *bsp_display_start(void)
{
    backlight_on();

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_handle_t panel = panel_start(&io);
    touch_start();

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    // Native panel is 240x320 portrait; rotate to landscape in software. Use a
    // PARTIAL draw buffer (40 rows) so each SPI flush is small — a full-frame
    // PSRAM transfer needs a large internal bounce buffer that fails to allocate
    // once WiFi has claimed internal RAM (white screen / "spi transmit color failed").
    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = LCD_HEIGHT * 40,          // partial (240 x 40)
        .double_buffer = true,
        .hres = LCD_HEIGHT,   // 240 = native-portrait width
        .vres = LCD_WIDTH,    // 320 = native-portrait height
        .monochrome = false,
        .flags = {
            // #1 freeze fix: draw buffers in INTERNAL DMA RAM, not PSRAM. Flushing a PSRAM
            // buffer over SPI-DMA stalls under PSRAM/bus contention (art TLS + album art +
            // audio), and the flush-done callback is lost → LVGL wedges forever in
            // wait_for_flushing (confirmed via Task-WDT backtrace). The partial 40-row
            // buffers are only ~19 KB each, so internal DMA RAM is affordable.
            .buff_dma = true,
            .sw_rotate = true,
            .swap_bytes = true,
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    s_disp = disp;
    lv_display_set_rotation(disp, orient_to_rot(g_settings.orientation));   // landscape default

    // Touch input device: our own FT6336 read_cb (the LVGL task polls it). Guard
    // it with the port lock since the LVGL task owns it.
    lvgl_port_lock(0);
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
    lv_indev_set_display(indev, disp);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "display + touch up (%dx%d)", LCD_WIDTH, LCD_HEIGHT);
    return disp;
}
