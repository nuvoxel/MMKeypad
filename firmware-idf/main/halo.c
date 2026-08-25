// "Halo" — onboard WS2812B RGB LED (nightlight / accent + ring-pulse status).
// See halo.h. RMT-driven via espressif/led_strip. No-op on boards with no LED.
#include "halo.h"
#include "board.h"

#ifndef PIN_RGB_LED   // board has no onboard RGB LED -> everything is a no-op
void halo_init(void) {}
void halo_set_color(uint8_t r, uint8_t g, uint8_t b) { (void)r; (void)g; (void)b; }
void halo_set_pulse_color(uint8_t r, uint8_t g, uint8_t b) { (void)r; (void)g; (void)b; }
void halo_set_brightness(uint8_t pct) { (void)pct; }
void halo_pulse(bool on) { (void)on; }
#else

#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "led_strip.h"

static const char *TAG = "halo";
static led_strip_handle_t s_strip;
static volatile uint8_t s_ir, s_ig, s_ib;            // idle color (nightlight/halo)
static volatile uint8_t s_pr = 0, s_pg = 0, s_pb = 255;  // pulse color (default blue)
static volatile uint8_t s_bright = 100;              // 0..100
static volatile bool    s_pulsing;

static void show(int r, int g, int b)
{
    if (!s_strip) return;
    int br = s_bright;
    led_strip_set_pixel(s_strip, 0, (r * br) / 100, (g * br) / 100, (b * br) / 100);
    led_strip_refresh(s_strip);
}

// One worker owns the LED: animate a breathing pulse, else hold the idle color.
static void halo_task(void *arg)
{
    (void)arg;
    float phase = 0.0f;
    bool was_pulsing = false;
    for (;;) {
        if (s_pulsing) {
            float e = 0.12f + 0.88f * (0.5f * (1.0f - cosf(phase)));   // breathe 0.12..1.0
            show((int)(s_pr * e), (int)(s_pg * e), (int)(s_pb * e));
            phase += 0.16f;
            if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
            was_pulsing = true;
            vTaskDelay(pdMS_TO_TICKS(25));
        } else {
            // Refresh the idle color (also repaints once right after a pulse ends).
            show(s_ir, s_ig, s_ib);
            phase = 0.0f;
            was_pulsing = false;
            vTaskDelay(pdMS_TO_TICKS(was_pulsing ? 25 : 120));
        }
    }
}

void halo_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = PIN_RGB_LED,
        .max_leds = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,   // WS2812 is GRB
        .led_model = LED_MODEL_WS2812,
        .flags = { .invert_out = false },
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,    // 10 MHz
        .flags = { .with_dma = false },
    };
    if (led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip) != ESP_OK) {
        ESP_LOGE(TAG, "led_strip init failed (GPIO%d)", PIN_RGB_LED);
        s_strip = NULL;
        return;
    }
    // Boot self-test: R -> G -> B so you can confirm the LED works + color order.
    led_strip_set_pixel(s_strip, 0, 40, 0, 0); led_strip_refresh(s_strip); vTaskDelay(pdMS_TO_TICKS(180));
    led_strip_set_pixel(s_strip, 0, 0, 40, 0); led_strip_refresh(s_strip); vTaskDelay(pdMS_TO_TICKS(180));
    led_strip_set_pixel(s_strip, 0, 0, 0, 40); led_strip_refresh(s_strip); vTaskDelay(pdMS_TO_TICKS(180));
    led_strip_clear(s_strip);
    halo_set_color(0, 0, 24);                 // default: a dim blue nightlight glow
    xTaskCreate(halo_task, "halo", 2560, NULL, 3, NULL);
    ESP_LOGI(TAG, "halo WS2812 ready on GPIO%d", PIN_RGB_LED);
}

void halo_set_color(uint8_t r, uint8_t g, uint8_t b) { s_ir = r; s_ig = g; s_ib = b; }
void halo_set_pulse_color(uint8_t r, uint8_t g, uint8_t b) { s_pr = r; s_pg = g; s_pb = b; }
void halo_set_brightness(uint8_t pct) { s_bright = pct > 100 ? 100 : pct; }
void halo_pulse(bool on) { s_pulsing = on; }

#endif // PIN_RGB_LED
