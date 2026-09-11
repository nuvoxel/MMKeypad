// "Halo" — onboard WS2812B RGB LED (nightlight / accent + ring-pulse status).
// See halo.h. RMT-driven via espressif/led_strip. No-op on boards with no LED.
#include "halo.h"
#include "board.h"
#include "config.h"

// HALO_COUNT lets a board drive a whole ring instead of one onboard pixel
// (backbox-poe carrier = 24). Defaults to 1 so every other board is unchanged.
#ifndef HALO_COUNT
#define HALO_COUNT 1
#endif

// Board-level ceiling on halo output, percent. Boards with a ring set this well
// below 100 (see board.h); single-LED boards leave it at 100 and are unchanged.
#ifndef HALO_BRIGHT_SCALE
#define HALO_BRIGHT_SCALE 100
#endif
#define HALO_EFFECTIVE(br) (((br) * HALO_BRIGHT_SCALE) / 100)

// Order MUST match driver-keypad/driver.xml's Halo Idle/Call Color <items> and
// driver.lua's HALO_COLORS -- an index crossing the wire means the same color on
// both ends only because both tables agree on position.
const uint8_t HALO_PALETTE[HALO_PALETTE_COUNT][3] = {
    {0, 0, 0},       {255, 255, 255}, {255, 170, 80},  {0, 0, 255},
    {0, 200, 255},   {0, 180, 150},   {0, 255, 40},    {255, 130, 0},
    {255, 0, 0},     {150, 0, 255},   {255, 40, 120},  {255, 0, 180},
};
const char *const HALO_PALETTE_NAMES[HALO_PALETTE_COUNT] = {
    "Off", "White", "Warm White", "Blue", "Cyan", "Teal",
    "Green", "Amber", "Red", "Purple", "Pink", "Magenta",
};
void halo_palette_rgb(uint8_t idx, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (idx >= HALO_PALETTE_COUNT) idx = 0;
    *r = HALO_PALETTE[idx][0]; *g = HALO_PALETTE[idx][1]; *b = HALO_PALETTE[idx][2];
}

#ifndef PIN_RGB_LED   // board has no onboard RGB LED -> everything is a no-op
void halo_init(void) {}
void halo_set_color(uint8_t r, uint8_t g, uint8_t b) { (void)r; (void)g; (void)b; }
void halo_set_pulse_color(uint8_t r, uint8_t g, uint8_t b) { (void)r; (void)g; (void)b; }
void halo_set_brightness(uint8_t pct) { (void)pct; }
void halo_pulse(bool on) { (void)on; }
void halo_apply_settings(void) {}
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
    int br = HALO_EFFECTIVE(s_bright);
    for (int i = 0; i < HALO_COUNT; i++)
        led_strip_set_pixel(s_strip, i, (r * br) / 100, (g * br) / 100, (b * br) / 100);
    led_strip_refresh(s_strip);
}

#if HALO_COUNT > 1
// Ring chase ("comet"): one bright head with a fading tail, travelling around
// the perimeter. Used for the ring-indication on a multi-LED halo, where a
// whole-ring breathe reads as a flash rather than an indicator.
// Deliberately dim — this lives on a wall in a dark room, and on the
// a 24 px ring at full white draws roughly 1.4 A / 7 W.
#define HALO_CHASE_TAIL   6      // px of fading tail behind the head
#define HALO_CHASE_MS     55     // ms per step -> ~1.3 s per lap over 24 px
#define HALO_CHASE_LEVEL  35     // % of the pulse colour at the head


static void show_chase(int r, int g, int b, int head)
{
    if (!s_strip) return;
    int br = (HALO_EFFECTIVE(s_bright) * HALO_CHASE_LEVEL) / 100;
    for (int i = 0; i < HALO_COUNT; i++) {
        int d = (head - i + HALO_COUNT) % HALO_COUNT;        // px behind the head
        int w = (d < HALO_CHASE_TAIL) ? (255 * (HALO_CHASE_TAIL - d)) / HALO_CHASE_TAIL : 0;
        led_strip_set_pixel(s_strip, i, (r * w / 255) * br / 100,
                                        (g * w / 255) * br / 100,
                                        (b * w / 255) * br / 100);
    }
    led_strip_refresh(s_strip);
}
#endif

// One worker owns the LED: animate a breathing pulse, else hold the idle color.
static void halo_task(void *arg)
{
    (void)arg;
    float phase = 0.0f;
    int   head  = 0;
    bool was_pulsing = false;
    (void)phase; (void)head;
    for (;;) {
        if (s_pulsing) {
#if HALO_COUNT > 1
            show_chase(s_pr, s_pg, s_pb, head);
            head = (head + 1) % HALO_COUNT;
            was_pulsing = true;
            vTaskDelay(pdMS_TO_TICKS(HALO_CHASE_MS));
#else
            float e = 0.12f + 0.88f * (0.5f * (1.0f - cosf(phase)));   // breathe 0.12..1.0
            show((int)(s_pr * e), (int)(s_pg * e), (int)(s_pb * e));
            phase += 0.16f;
            if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
            was_pulsing = true;
            vTaskDelay(pdMS_TO_TICKS(25));
#endif
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
        .max_leds = HALO_COUNT,
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
#if HALO_COUNT > 1
    // Boot self-test: one dim blue lap. Confirms every pixel is alive, that the
    // chain order is what we think, and that GPIO48's LDO domain came up — the
    // ring is simply dark if IO_LDO_CHAN was not acquired.
    for (int i = 0; i < HALO_COUNT; i++) {
        show_chase(0, 0, 255, i);
        vTaskDelay(pdMS_TO_TICKS(HALO_CHASE_MS));
    }
#else
    // Single onboard LED: R -> G -> B confirms it works and the colour order.
    show(40,0,0); vTaskDelay(pdMS_TO_TICKS(180));
    show(0,40,0); vTaskDelay(pdMS_TO_TICKS(180));
    show(0,0,40); vTaskDelay(pdMS_TO_TICKS(180));
#endif
    led_strip_clear(s_strip);
    halo_apply_settings();   // g_settings (NVS) is authoritative -- see config.h
    xTaskCreate(halo_task, "halo", 2560, NULL, 3, NULL);
    ESP_LOGI(TAG, "halo ready: %d px on GPIO%d", HALO_COUNT, PIN_RGB_LED);
}

void halo_set_color(uint8_t r, uint8_t g, uint8_t b) { s_ir = r; s_ig = g; s_ib = b; }
void halo_set_pulse_color(uint8_t r, uint8_t g, uint8_t b) { s_pr = r; s_pg = g; s_pb = b; }
void halo_set_brightness(uint8_t pct) { s_bright = pct > 100 ? 100 : pct; }
void halo_pulse(bool on) { s_pulsing = on; }

void halo_apply_settings(void)
{
    uint8_t r, g, b;
    halo_palette_rgb(g_settings.halo_idle_color, &r, &g, &b);
    halo_set_color(r, g, b);
    halo_palette_rgb(g_settings.halo_ring_color, &r, &g, &b);
    halo_set_pulse_color(r, g, b);
    halo_set_brightness(g_settings.halo_brightness);
}

#endif // PIN_RGB_LED
