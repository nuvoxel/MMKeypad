// MMKeypad — audio bring-up / speaker + mic placement test.
//
// Standalone image: ES8311 codec (mic ADC + spkr DAC) + FM8002E amp over I2S,
// controlled over the shared I2C bus. No UI, no WiFi — flash it, open the
// serial monitor, and it loops a placement-test cycle so you can move the
// speaker/mic around the enclosure and judge each position by ear + by the
// printed level meter.
//
// Cycle (repeats forever):
//   1. TONE SWEEP   — 440 / 1000 / 2000 / 4000 Hz tones out the speaker.
//                     Listen for buzz/rattle and loudness at each spk position.
//   2. MIC METER    — amp OFF; prints mic RMS / peak dBFS for ~6 s. Tap or
//                     speak near candidate mic holes; watch the numbers.
//   3. LOOPBACK     — amp ON; mic -> speaker live for ~8 s. Reveals acoustic
//                     coupling / feedback between a given mic+spk placement.
//
// Pinmap comes from ../../main/board.h (single source of truth).

#include <math.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_log.h"

#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "board.h"

static const char *TAG = "audiotest";

#define SAMPLE_RATE   16000          // voice/intercom-relevant; MEMS mic happy here
#define BITS_PER_SMP  16
#define FRAME_SAMPLES 256            // ~16 ms chunks

static esp_codec_dev_handle_t s_dev;
static i2s_chan_handle_t       s_tx, s_rx;

// ── On-screen meter (LVGL) ──────────────────────────────────────────────────
static lv_obj_t *s_lbl_phase, *s_lbl_db, *s_bar, *s_lbl_hint;

static void display_start(void);     // fwd

static void ui_set_phase(const char *txt, lv_color_t accent)
{
    if (!s_lbl_phase) return;
    lvgl_port_lock(0);
    lv_label_set_text(s_lbl_phase, txt);
    lv_obj_set_style_text_color(s_lbl_phase, accent, 0);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lvgl_port_unlock();
}

// Show the mic level (dBFS, -60..0) on the big number + bar.
static void ui_set_mic(double dbfs)
{
    if (!s_lbl_db) return;
    int pct = (int)((dbfs + 60.0) / 60.0 * 100.0);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    lvgl_port_lock(0);
    lv_label_set_text_fmt(s_lbl_db, "%.0f", dbfs);
    lv_bar_set_value(s_bar, pct, LV_ANIM_OFF);
    // green when loud, dim when quiet
    lv_color_t c = pct > 55 ? lv_palette_main(LV_PALETTE_GREEN)
                 : pct > 25 ? lv_palette_main(LV_PALETTE_YELLOW)
                            : lv_palette_main(LV_PALETTE_GREY);
    lv_obj_set_style_bg_color(s_bar, c, LV_PART_INDICATOR);
    lvgl_port_unlock();
}

static void ui_set_hint(const char *txt)
{
    if (!s_lbl_hint) return;
    lvgl_port_lock(0);
    lv_label_set_text(s_lbl_hint, txt);
    lvgl_port_unlock();
}

// ── Codec bring-up ──────────────────────────────────────────────────────────

static i2c_master_bus_handle_t i2c_start(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = TOUCH_I2C_PORT,          // ES8311 shares the touch I2C bus
        .sda_io_num = PIN_TOUCH_SDA,
        .scl_io_num = PIN_TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));
    return bus;
}

// Probe the bus: list every ACKing address, then read the ES8311 chip-ID
// registers (0xFD=0x83, 0xFE=0x11) to prove we're really talking to the codec.
static void i2c_diag(i2c_master_bus_handle_t bus)
{
    ESP_LOGI(TAG, "── I2C scan (port %d, sda=%d scl=%d):",
             TOUCH_I2C_PORT, PIN_TOUCH_SDA, PIN_TOUCH_SCL);
    for (uint8_t a = 1; a < 0x7f; a++) {
        if (i2c_master_probe(bus, a, 50) == ESP_OK)
            ESP_LOGI(TAG, "   ACK @ 0x%02x%s", a,
                     a == ES8311_I2C_ADDR ? "  <- ES8311 (expected)" :
                     a == TOUCH_I2C_ADDR  ? "  <- FT6336 touch" : "");
    }

    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8311_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t es = NULL;
    if (i2c_master_bus_add_device(bus, &dc, &es) != ESP_OK) {
        ESP_LOGE(TAG, "   could not add ES8311 @0x%02x to bus", ES8311_I2C_ADDR);
        return;
    }
    uint8_t id0 = 0, id1 = 0, reg = 0xFD;
    esp_err_t e0 = i2c_master_transmit_receive(es, &reg, 1, &id0, 1, pdMS_TO_TICKS(50));
    reg = 0xFE;
    esp_err_t e1 = i2c_master_transmit_receive(es, &reg, 1, &id1, 1, pdMS_TO_TICKS(50));
    if (e0 == ESP_OK && e1 == ESP_OK)
        ESP_LOGI(TAG, "   ES8311 chip id: 0x%02x 0x%02x (want 0x83 0x11)%s",
                 id0, id1, (id0 == 0x83 && id1 == 0x11) ? "  OK" : "  MISMATCH");
    else
        ESP_LOGE(TAG, "   ES8311 id read failed (e0=%d e1=%d) — codec not responding",
                 e0, e1);
    i2c_master_bus_rm_device(es);
}

static void i2s_start(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx, &s_rx));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        // Stereo frame: ES8311 is mono but its ADC drives / DAC reads a specific
        // L/R slot. Running both slots and duplicating the tone L+R makes the
        // speaker work regardless, and lets the mic meter report L vs R so we
        // can see which slot the ADC is actually on.
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws   = PIN_I2S_LRCLK,
            .dout = PIN_I2S_DOUT,   // GPIO8 (board.h, hardware-verified)
            .din  = PIN_I2S_DIN,    // GPIO6 — codec ASDOUT (mic)
            .invert_flags = { 0 },
        },
    };
    // esp_codec_dev enables/disables the channels on open/close; just init here.
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &std_cfg));
}

static void codec_start(i2c_master_bus_handle_t bus)
{
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .tx_handle = s_tx,
        .rx_handle = s_rx,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    assert(data_if);

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = TOUCH_I2C_PORT,
        .addr = ES8311_I2C_ADDR << 1,        // esp_codec_dev wants the 8-bit addr
        .bus_handle = bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(ctrl_if);

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,   // mic + speaker
        .pa_pin = PIN_AMP_ENABLE,
        .pa_reverted = true,                          // FM8002E: LOW = enabled
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0f, .codec_dac_voltage = 3.3f },
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    assert(codec_if);

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = codec_if,
        .data_if  = data_if,
    };
    s_dev = esp_codec_dev_new(&dev_cfg);
    assert(s_dev);

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = BITS_PER_SMP,
        .channel = 2,                 // stereo frame (see i2s slot note)
        .sample_rate = SAMPLE_RATE,
    };
    ESP_ERROR_CHECK(esp_codec_dev_open(s_dev, &fs));
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(s_dev, 75));   // % of DAC full scale
    ESP_ERROR_CHECK(esp_codec_dev_set_in_gain(s_dev, 30.0f)); // dB mic gain
}

// ── Amp gate — silence the speaker path during the mic-meter phase ──────────

static void amp_enable(bool on)
{
    // The codec's PA pin is driven by esp_codec_dev, but explicitly parking it
    // lets us run a clean mic-only phase with no speaker output.
    gpio_set_direction(PIN_AMP_ENABLE, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_AMP_ENABLE, on ? 0 : 1);   // active LOW
}

// ── Test phases ─────────────────────────────────────────────────────────────

static void play_tone(int freq_hz, int ms, float amp)
{
    static int16_t buf[FRAME_SAMPLES * 2];   // interleaved L/R
    int total = (SAMPLE_RATE * ms) / 1000;
    double phase = 0.0;
    double step  = 2.0 * M_PI * freq_hz / SAMPLE_RATE;
    int16_t peak = (int16_t)(amp * 32767.0f);

    for (int done = 0; done < total; done += FRAME_SAMPLES) {
        int n = (total - done < FRAME_SAMPLES) ? (total - done) : FRAME_SAMPLES;
        for (int i = 0; i < n; i++) {
            int16_t s = (int16_t)(sin(phase) * peak);
            buf[2 * i] = s;          // L
            buf[2 * i + 1] = s;      // R — duplicate so either codec slot sounds
            phase += step;
            if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
        }
        esp_codec_dev_write(s_dev, buf, n * 2 * sizeof(int16_t));
    }
}

static void phase_tone_sweep(void)
{
    static const int freqs[] = { 440, 1000, 2000, 4000 };
    ESP_LOGI(TAG, "── PHASE 1: TONE SWEEP (speaker) — listen for rattle/loudness");
    ui_set_phase("TONE SWEEP", lv_palette_main(LV_PALETTE_BLUE));
    ui_set_hint("listen at the speaker");
    amp_enable(true);
    for (size_t i = 0; i < sizeof(freqs) / sizeof(freqs[0]); i++) {
        ESP_LOGI(TAG, "   tone %d Hz", freqs[i]);
        lvgl_port_lock(0);
        lv_label_set_text_fmt(s_lbl_db, "%d", freqs[i]);
        lvgl_port_unlock();
        play_tone(freqs[i], 700, 0.6f);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

static void phase_mic_meter(int seconds)
{
    static int16_t buf[FRAME_SAMPLES * 2];   // interleaved L/R
    ESP_LOGI(TAG, "── PHASE 2: MIC METER (amp off) — tap/speak near mic holes");
    ui_set_phase("MIC LEVEL", lv_palette_main(LV_PALETTE_GREEN));
    ui_set_hint("tap / speak by the mic");
    amp_enable(false);
    int frames = (SAMPLE_RATE * seconds) / FRAME_SAMPLES;
    int report_every = SAMPLE_RATE / FRAME_SAMPLES / 4;   // ~4 lines/sec
    for (int f = 0; f < frames; f++) {
        if (esp_codec_dev_read(s_dev, buf, sizeof(buf)) != ESP_CODEC_DEV_OK) continue;
        double sqL = 0, sqR = 0;
        for (int i = 0; i < FRAME_SAMPLES; i++) {
            sqL += (double)buf[2 * i]     * buf[2 * i];
            sqR += (double)buf[2 * i + 1] * buf[2 * i + 1];
        }
        if (f % report_every == 0) {
            double rmsL = sqrt(sqL / FRAME_SAMPLES), rmsR = sqrt(sqR / FRAME_SAMPLES);
            // Mic is on the LEFT slot; right slot is the codec's unused channel.
            double dbMic = rmsL > 0 ? 20.0 * log10(rmsL / 32768.0) : -99.0;
            double dbR   = rmsR > 0 ? 20.0 * log10(rmsR / 32768.0) : -99.0;
            int bars = (int)((dbMic + 60.0) / 60.0 * 30.0);   // bar tracks the mic
            if (bars < 0) bars = 0;
            if (bars > 30) bars = 30;
            char bar[31];
            memset(bar, '#', bars);
            bar[bars] = 0;
            ESP_LOGI(TAG, "   mic %6.1f dBFS (R %6.1f) |%-30s|", dbMic, dbR, bar);
            ui_set_mic(dbMic);
        }
    }
}

#define LOOPBACK_GAIN 12.0f      // mic is quiet; boost so playback is obvious

static void phase_loopback(int seconds)
{
    static int16_t in[FRAME_SAMPLES * 2], out[FRAME_SAMPLES * 2];
    ESP_LOGI(TAG, "── PHASE 3: LIVE LOOPBACK (mic -> speaker) — listen for feedback");
    ui_set_phase("LOOPBACK", lv_palette_main(LV_PALETTE_ORANGE));
    ui_set_hint("mic -> speaker (feedback?)");
    lvgl_port_lock(0);
    lv_label_set_text(s_lbl_db, "<->");
    lvgl_port_unlock();
    amp_enable(true);
    int frames = (SAMPLE_RATE * seconds) / FRAME_SAMPLES;
    for (int f = 0; f < frames; f++) {
        if (esp_codec_dev_read(s_dev, in, sizeof(in)) != ESP_CODEC_DEV_OK) continue;
        // Mic is the LEFT slot. Amplify it and send to BOTH output slots so the
        // DAC plays it regardless of which slot it reads (right slot is DC junk).
        for (int i = 0; i < FRAME_SAMPLES; i++) {
            int v = (int)(in[2 * i] * LOOPBACK_GAIN);
            if (v >  32767) v =  32767;
            if (v < -32768) v = -32768;
            out[2 * i] = out[2 * i + 1] = (int16_t)v;
        }
        esp_codec_dev_write(s_dev, out, sizeof(out));
    }
}

// ── Display bring-up (ILI9341V via esp_lcd + LVGL), copied from main fw bsp.c.
//    Fixed landscape, no touch — just enough to render the meter. ───────────
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

static void display_start(void)
{
    // Backlight full on (LEDC PWM, active HIGH).
    ledc_timer_config_t lt = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&lt);
    ledc_channel_config_t lc = {
        .gpio_num = PIN_LCD_BL, .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 255, .hpoint = 0,
    };
    ledc_channel_config(&lc);

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_LCD_SCK, .mosi_io_num = PIN_LCD_MOSI, .miso_io_num = PIN_LCD_MISO,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * 40 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS, .dc_gpio_num = PIN_LCD_DC, .pclk_hz = LCD_SPI_FREQ_HZ,
        .spi_mode = 0, .trans_queue_depth = 10, .lcd_cmd_bits = 8, .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &io));

    ili9341_vendor_config_t vendor = {
        .init_cmds = ili9341v_init,
        .init_cmds_size = sizeof(ili9341v_init) / sizeof(ili9341v_init[0]),
    };
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16, .vendor_config = &vendor,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));   // IPS
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, false, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));
    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io, .panel_handle = panel,
        .buffer_size = LCD_HEIGHT * 40, .double_buffer = true,
        .hres = LCD_HEIGHT, .vres = LCD_WIDTH, .monochrome = false,
        .flags = { .buff_spiram = false, .sw_rotate = true, .swap_bytes = true },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90);   // landscape (320x240)

    // Build the meter UI.
    lvgl_port_lock(0);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    s_lbl_phase = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lbl_phase, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_lbl_phase, lv_color_white(), 0);
    lv_label_set_text(s_lbl_phase, "AUDIO TEST");
    lv_obj_align(s_lbl_phase, LV_ALIGN_TOP_MID, 0, 12);

    s_lbl_db = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lbl_db, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_lbl_db, lv_color_white(), 0);
    lv_label_set_text(s_lbl_db, "--");
    lv_obj_align(s_lbl_db, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t *unit = lv_label_create(scr);
    lv_obj_set_style_text_color(unit, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_label_set_text(unit, "dBFS");
    lv_obj_align(unit, LV_ALIGN_CENTER, 0, 26);

    s_bar = lv_bar_create(scr);
    lv_obj_set_size(s_bar, 280, 18);
    lv_obj_align(s_bar, LV_ALIGN_BOTTOM_MID, 0, -36);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);

    s_lbl_hint = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lbl_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_hint, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_label_set_text(s_lbl_hint, "");
    lv_obj_align(s_lbl_hint, LV_ALIGN_BOTTOM_MID, 0, -12);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "display up (320x240 landscape)");
}

void app_main(void)
{
    ESP_LOGI(TAG, "MMKeypad audio bring-up / placement test");
    ESP_LOGI(TAG, "ES8311 @0x%02x  I2S mclk=%d bclk=%d ws=%d dout=%d din=%d  amp=%d",
             ES8311_I2C_ADDR, PIN_I2S_MCLK, PIN_I2S_BCLK, PIN_I2S_LRCLK,
             PIN_I2S_DOUT, PIN_I2S_DIN, PIN_AMP_ENABLE);

    display_start();
    i2c_master_bus_handle_t bus = i2c_start();
    i2c_diag(bus);
    i2s_start();
    codec_start(bus);
    ESP_LOGI(TAG, "codec up @ %d Hz / 16-bit (mic on left slot)", SAMPLE_RATE);

    for (int cycle = 1;; cycle++) {
        ESP_LOGI(TAG, "════════ cycle %d ════════", cycle);
        phase_tone_sweep();
        vTaskDelay(pdMS_TO_TICKS(500));
        phase_mic_meter(6);
        vTaskDelay(pdMS_TO_TICKS(500));
        phase_loopback(8);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
