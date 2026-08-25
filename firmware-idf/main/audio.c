// MMKeypad — Phase-3 audio: ES8311 codec + I2S + FM8002E amp.
// Brought up from the proven audio-test bring-up (see firmware-idf/audio-test).
// The codec shares the touch I2C bus; I2S is full-duplex (speaker DAC + mic ADC).
// NOTE: board.h I2S DOUT/DIN are hardware-verified (the lcdwiki "in/out" labels
// are from the codec's side — swapping them kills both directions).

#include "audio.h"
#include "board.h"
#if MMK_HAS_DISPLAY
#include "bsp.h"          // shared touch I2C bus on display boards
#endif

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

static const char *TAG = "audio";

#define SAMPLE_RATE   16000          // voice/intercom rate; ES8311 + MEMS mic OK
#define CHIME_FRAMES  256
// ES7210/ES8311 capture gain. 30 dB left the ws43 mic at ~0.5% of full scale
// (selftest peak 184 / rms 16 of 32767) -- audible to the RMS meter, inaudible in
// the loopback. MAX is used for the diagnostic leg only.
#define MIC_GAIN_DEFAULT_DB  30.0f
#define MIC_GAIN_MAX_DB      37.5f

static esp_codec_dev_handle_t s_dev;         // OUTPUT (also INPUT on single-codec boards)
#if defined(MMK_HAS_ES7210) && MMK_HAS_ES7210
// ws43 has a dedicated ES7210 4-ch ADC (2 mics + HW AEC ref). ES8311 is DAC-only
// here; capture comes from this second codec_dev handle on the same I2S/I2C buses.
static esp_codec_dev_handle_t s_dev_in;      // ES7210 capture handle
#define AUDIO_IN_DEV  s_dev_in
#else
#define AUDIO_IN_DEV  s_dev                   // ES8311 ADC (mic on its own codec)
#endif
static i2s_chan_handle_t       s_tx, s_rx;
static bool                    s_ready;
static int16_t                 s_up_last;    // prev 8k sample (call upsample seam)

bool audio_ready(void) { return s_ready; }

static uint8_t s_speaker_vol = 80;     // CURRENT codec DAC volume (call level while in a call)
static uint8_t s_user_vol    = 80;     // the Sound > Volume slider: the panel's master level

static bool s_amp_on;      // tracked so a beep won't switch the amp off mid-call
void audio_amp(bool on)
{
    gpio_set_direction(PIN_AMP_ENABLE, GPIO_MODE_OUTPUT);
    // Enable level is board-specific: FM8002E (S3) is active-LOW, NS4150B (P4) active-HIGH.
    int enable_level = AMP_ACTIVE_LOW ? 0 : 1;
    gpio_set_level(PIN_AMP_ENABLE, on ? enable_level : !enable_level);
    s_amp_on = on;
}

static bool i2s_start(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    if (i2s_new_channel(&chan_cfg, &s_tx, &s_rx) != ESP_OK) return false;

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        // Stereo frame: ES8311 is mono but its ADC/DAC sit on a specific L/R slot.
        // Mic is on LEFT; speaker is fed by duplicating L+R.
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws   = PIN_I2S_LRCLK,
            .dout = PIN_I2S_DOUT,   // GPIO8 -> codec DSDIN (speaker)
            .din  = PIN_I2S_DIN,    // GPIO6 <- codec ASDOUT (mic)
            .invert_flags = { 0 },
        },
    };
    // esp_codec_dev enables/reconfigs the channels on open; just init here.
    if (i2s_channel_init_std_mode(s_tx, &std_cfg) != ESP_OK) return false;
    if (i2s_channel_init_std_mode(s_rx, &std_cfg) != ESP_OK) return false;
    return true;
}

// Headless boards have no touch controller, so there's no shared bus to borrow —
// create a dedicated I2C master for the ES8311 on the board's audio I2C pins.
#if !MMK_HAS_DISPLAY
static i2c_master_bus_handle_t audio_i2c_create(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = AUDIO_I2C_PORT,
        .sda_io_num = PIN_AUDIO_SDA,
        .scl_io_num = PIN_AUDIO_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_new_master_bus(&cfg, &bus) != ESP_OK) return NULL;
    return bus;
}
#endif

bool audio_start(void)
{
    if (s_ready) return true;
#if MMK_HAS_DISPLAY
    i2c_master_bus_handle_t bus = bsp_i2c_bus();   // shared with the FT6336 touch
    if (!bus) { ESP_LOGE(TAG, "no I2C bus (call after bsp_display_start)"); return false; }
#else
    i2c_master_bus_handle_t bus = audio_i2c_create();
    if (!bus) { ESP_LOGE(TAG, "audio I2C bus create failed"); return false; }
#endif

    if (!i2s_start()) { ESP_LOGE(TAG, "I2S init failed"); return false; }

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .tx_handle = s_tx, .rx_handle = s_rx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = AUDIO_I2C_PORT,
        .addr = ES8311_I2C_ADDR << 1,     // esp_codec_dev wants the 8-bit address
        .bus_handle = bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (!data_if || !ctrl_if || !gpio_if) { ESP_LOGE(TAG, "codec ifaces failed"); return false; }

#if defined(MMK_HAS_ES7210) && MMK_HAS_ES7210
    // ── ws43: ES8311 = DAC/speaker OUTPUT ONLY; ES7210 = dual-mic INPUT ──────────
    // ES8311 stays DAC-only so its ADC never drives the shared I2S DIN line that
    // the ES7210 owns (bus collision otherwise). Mirrors Waveshare's factory BSP.
    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,    // speaker only (ES7210 does mic)
        .pa_pin = PIN_AMP_ENABLE,
        .pa_reverted = AMP_ACTIVE_LOW ? true : false,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0f, .codec_dac_voltage = 3.3f },
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    if (!codec_if) { ESP_LOGE(TAG, "es8311_codec_new failed"); return false; }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if  = data_if,
    };
    s_dev = esp_codec_dev_new(&dev_cfg);
    if (!s_dev) { ESP_LOGE(TAG, "esp_codec_dev_new(out) failed"); return false; }

    // ES7210 capture codec on the SAME I2S data_if + I2C bus (its own address).
    audio_codec_i2c_cfg_t in_i2c_cfg = {
        .port = AUDIO_I2C_PORT,
        .addr = ES7210_I2C_ADDR << 1,     // 8-bit addr for esp_codec_dev
        .bus_handle = bus,
    };
    const audio_codec_ctrl_if_t *in_ctrl_if = audio_codec_new_i2c_ctrl(&in_i2c_cfg);
    if (!in_ctrl_if) { ESP_LOGE(TAG, "es7210 i2c ctrl failed"); return false; }
    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = in_ctrl_if,            // mic_selected=0 → driver defaults to MIC1|MIC2
    };
    const audio_codec_if_t *in_codec_if = es7210_codec_new(&es7210_cfg);
    if (!in_codec_if) { ESP_LOGE(TAG, "es7210_codec_new failed"); return false; }
    esp_codec_dev_cfg_t in_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = in_codec_if,
        .data_if  = data_if,
    };
    s_dev_in = esp_codec_dev_new(&in_dev_cfg);
    if (!s_dev_in) { ESP_LOGE(TAG, "esp_codec_dev_new(in) failed"); return false; }
#else
    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,   // mic + speaker
        .pa_pin = PIN_AMP_ENABLE,
        .pa_reverted = AMP_ACTIVE_LOW ? true : false, // FM8002E LOW-enable vs NS4150B HIGH-enable
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0f, .codec_dac_voltage = 3.3f },
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    if (!codec_if) { ESP_LOGE(TAG, "es8311_codec_new failed"); return false; }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = codec_if,
        .data_if  = data_if,
    };
    s_dev = esp_codec_dev_new(&dev_cfg);
    if (!s_dev) { ESP_LOGE(TAG, "esp_codec_dev_new failed"); return false; }
#endif

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 2,                 // stereo frame (mic on left slot)
        .sample_rate = SAMPLE_RATE,
    };
    if (esp_codec_dev_open(s_dev, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open failed");
        return false;
    }
#if defined(MMK_HAS_ES7210) && MMK_HAS_ES7210
    if (esp_codec_dev_open(s_dev_in, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open(in/ES7210) failed");
        return false;
    }
#endif
    esp_codec_dev_set_out_vol(s_dev, 75);
    esp_codec_dev_set_in_gain(AUDIO_IN_DEV, MIC_GAIN_DEFAULT_DB);
    audio_amp(false);                 // speaker silent until something plays

    s_ready = true;
#if defined(MMK_HAS_ES7210) && MMK_HAS_ES7210
    ESP_LOGI(TAG, "codec up @ %d Hz (ES8311 out @0x%02x + ES7210 in @0x%02x, amp off)",
             SAMPLE_RATE, ES8311_I2C_ADDR, ES7210_I2C_ADDR);
#else
    ESP_LOGI(TAG, "codec up @ %d Hz (ES8311 @0x%02x, amp off)", SAMPLE_RATE, ES8311_I2C_ADDR);
#endif
    return true;
}

static bool s_in_call;
bool audio_in_call(void) { return s_in_call; }

bool audio_call_begin(int sample_rate)
{
    if (!s_ready) return false;
    s_in_call = true;
    // Deliberately NO codec reopen. Reopening as 8 kHz mono moved the mic off the
    // LEFT I2S slot (dead mic) and changed the clock under the proven path
    // (garbled audio). The codec stays 16 kHz stereo; sip.c resamples 16k<->8k.
    s_up_last = 0;
    esp_codec_dev_set_out_vol(s_dev, 85);
    esp_codec_dev_set_in_gain(AUDIO_IN_DEV, MIC_GAIN_DEFAULT_DB);
    audio_amp(true);
    ESP_LOGI(TAG, "call mode: codec 16 kHz stereo, %d Hz mono on the SIP side",
             sample_rate);
    return true;
}

void audio_call_end(void)
{
    s_in_call = false;
    if (!s_ready) return;
    audio_amp(false);
    // Restore the USER's master level, not a hardcoded 75 -- otherwise every call
    // silently redefines the panel's volume.
    esp_codec_dev_set_out_vol(s_dev, s_user_vol);
    esp_codec_dev_set_in_gain(AUDIO_IN_DEV, MIC_GAIN_DEFAULT_DB);
    ESP_LOGI(TAG, "call mode ended");
}

// ── Call-path codec: G.711 µ-law <-> codec 16 kHz stereo PCM ─────────────────
// esp_rtc does ONLY RTP framing, NOT sample coding — its send/receive_audio
// callbacks exchange RAW µ-law bytes (1 byte/sample @ 8 kHz for PCMU). So we do
// the G.711 encode/decode here, plus the 8k<->16k resample (exact 2:1) and the
// mono<->stereo mapping (mic on the LEFT slot; speaker wants L+R duplicated).
// Buffers are static (one per direction — send/receive run on separate esp_rtc
// tasks) to keep them off the callback stacks.
#define CALL_CHUNK_8K  160                       // 20 ms G.711 frame @ 8 kHz

// Standard ITU-T G.711 µ-law sample codec (no library needed).
static inline int16_t ulaw_to_pcm(uint8_t u)
{
    u = ~u;
    int sign = u & 0x80, exp = (u >> 4) & 0x07, man = u & 0x0F;
    int s = ((man << 3) + 0x84) << exp;
    s -= 0x84;
    return (int16_t)(sign ? -s : s);
}

static inline uint8_t pcm_to_ulaw(int16_t pcm)
{
    int sign = (pcm >> 8) & 0x80;
    if (sign) pcm = (int16_t)-pcm;
    if (pcm > 32635) pcm = 32635;
    pcm += 0x84;
    int exp = 7;
    for (int mask = 0x4000; (pcm & mask) == 0 && exp > 0; exp--, mask >>= 1) {}
    int man = (pcm >> (exp + 3)) & 0x0F;
    return (uint8_t)~(sign | (exp << 4) | man);
}

int audio_call_read(void *dst_ulaw, int n)        // mic -> SIP: fill n µ-law bytes
{
    if (!s_ready) return -1;
    static int16_t st[CALL_CHUNK_8K * 2 * 2];     // up to CHUNK*2 stereo frames
    uint8_t *out = dst_ulaw;
    int done = 0, mpeak = 0, cnt = 0;
    long long sumsq = 0;
    while (done < n) {
        int n8 = n - done;
        if (n8 > CALL_CHUNK_8K) n8 = CALL_CHUNK_8K;
        int frames16 = n8 * 2;                    // 16 kHz stereo frames to read
        if (esp_codec_dev_read(AUDIO_IN_DEV, st, frames16 * 2 * (int)sizeof(int16_t))
                != ESP_CODEC_DEV_OK) break;
        for (int i = 0; i < n8; i++) {            // LEFT = mic; avg 2 (16k->8k anti-alias)
            int l0 = st[(2 * i) * 2], l1 = st[(2 * i + 1) * 2];
            int16_t s = (int16_t)((l0 + l1) / 2);
            int a = s < 0 ? -s : s;
            if (a > mpeak) mpeak = a;
            sumsq += (long long)s * s; cnt++;
            out[done + i] = pcm_to_ulaw(s);       // encode to µ-law for esp_rtc
        }
        done += n8;
    }
    static int calls;                             // DIAG: mic level + frame size, ~1/sec
    if (++calls >= 50) {
        int rms = cnt ? (int)sqrt((double)sumsq / cnt) : 0;
        ESP_LOGI(TAG, "call_read: req=%dB peak=%d rms=%d %s", n, mpeak, rms,
                 mpeak > 200 ? "MIC-OK" : "mic-silent");
        calls = 0;
    }
    return done;
}

int audio_call_write(const void *src_ulaw, int n)  // SIP -> speaker: play n µ-law bytes
{
    if (!s_ready) return -1;
    static int16_t st[CALL_CHUNK_8K * 2 * 2];      // CHUNK*2 stereo frames (16 kHz)
    const uint8_t *in = src_ulaw;
    int done = 0, dpeak = 0;
    while (done < n) {
        int n8 = n - done;
        if (n8 > CALL_CHUNK_8K) n8 = CALL_CHUNK_8K;
        int k = 0;
        for (int i = 0; i < n8; i++) {             // µ-law decode, 8k->16k interp, dup L+R
            int16_t cur = ulaw_to_pcm(in[done + i]);
            int a = cur < 0 ? -cur : cur;
            if (a > dpeak) dpeak = a;
            int16_t mid = (int16_t)(((int)s_up_last + cur) / 2);
            st[k++] = mid; st[k++] = mid;
            st[k++] = cur; st[k++] = cur;
            s_up_last = cur;
        }
        esp_codec_dev_write(s_dev, st, k * (int)sizeof(int16_t));
        done += n8;
    }
    static int calls;                              // DIAG: remote audio level + size, ~1/sec
    if (++calls >= 50) {
        ESP_LOGI(TAG, "call_write: got=%dB peak=%d %s", n, dpeak,
                 dpeak > 200 ? "RX-AUDIO" : "rx-silent");
        calls = 0;
    }
    return done;
}

void audio_set_volume(uint8_t pct)
{
    s_speaker_vol = pct > 100 ? 100 : pct;
    if (s_ready) esp_codec_dev_set_out_vol(s_dev, s_speaker_vol);
}

// The Settings "Sound > Volume" slider. This is the panel's MASTER output level and is
// what every local sound (tones, diagnostics, loopback) should follow. Kept separate
// from audio_set_volume() because the intercom proxy pushes a CALL volume over sipvol
// and legitimately sends 0 while idle -- that used to leak into everything else and
// silence the diagnostics with no way to tell from the UI. SIP may still override
// during a call; this baseline is what local playback restores to.
void audio_set_user_volume(uint8_t pct)
{
    s_user_vol = pct > 100 ? 100 : pct;
    audio_set_volume(s_user_vol);
}

// Notification tones (chime/doorbell/ring/beep) must be loud regardless of the call SPEAKER
// level — their loudness is set by the RINGER volume (sample amplitude). The codec's output
// volume otherwise attenuates them by the speaker slider too (that's why they were quiet). So
// run tones at FULL codec output and restore the speaker level afterward.
static void tone_vol_full(void)    { if (s_ready) esp_codec_dev_set_out_vol(s_dev, 100); }
static void tone_vol_restore(void) { if (s_ready) esp_codec_dev_set_out_vol(s_dev, s_speaker_vol); }
// Local playback (diagnostics) runs at the user's master level, not the call level.
static void user_vol_apply(void)   { if (s_ready) esp_codec_dev_set_out_vol(s_dev, s_user_vol); }

void audio_set_mic_gain(float db)
{
    if (s_ready) esp_codec_dev_set_in_gain(AUDIO_IN_DEV, db);
}

static uint8_t s_ringer_vol = 100;     // chime loudness 0..100 (intercom ringer)
void audio_set_ringer_volume(uint8_t pct)
{
    s_ringer_vol = pct > 100 ? 100 : pct;
}

int audio_read(void *buf, int bytes)
{
    if (!s_ready) return -1;
    return esp_codec_dev_read(AUDIO_IN_DEV, buf, bytes) == ESP_CODEC_DEV_OK ? bytes : -1;
}

int audio_write(const void *buf, int bytes)
{
    if (!s_ready) return -1;
    return esp_codec_dev_write(s_dev, (void *)buf, bytes) == ESP_CODEC_DEV_OK ? bytes : -1;
}

// Generate `ms` of a sine at `freq` into the speaker (stereo, duplicated L+R).
// A short raised-cosine fade in/out on each note removes the hard click/pop at
// note edges (the main thing that makes pure-tone beeps sound cheap/harsh).
static void play_tone(int freq, int ms, float amp)
{
    static int16_t buf[CHIME_FRAMES * 2];
    int total = (SAMPLE_RATE * ms) / 1000;
    int fade  = SAMPLE_RATE / 200;        // ~5 ms attack/release
    if (fade > total / 2) fade = total / 2;
    double phase = 0.0, step = 2.0 * M_PI * freq / SAMPLE_RATE;
    float peak = amp * 32767.0f;
    for (int done = 0; done < total; done += CHIME_FRAMES) {
        int n = (total - done < CHIME_FRAMES) ? (total - done) : CHIME_FRAMES;
        for (int i = 0; i < n; i++) {
            int pos = done + i;
            float env = 1.0f;
            if (fade > 0) {
                if (pos < fade)              env = 0.5f * (1.0f - cosf((float)M_PI * pos / fade));
                else if (pos > total - fade) env = 0.5f * (1.0f - cosf((float)M_PI * (total - pos) / fade));
            }
            int16_t s = (int16_t)(sinf((float)phase) * peak * env);
            buf[2 * i] = buf[2 * i + 1] = s;
            phase += step;
            if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
        }
        esp_codec_dev_write(s_dev, buf, n * 2 * sizeof(int16_t));
    }
}

// Embedded notification samples (raw 16 kHz mono PCM; see main/CMakeLists EMBED_FILES).
extern const uint8_t beep_pcm_start[]     asm("_binary_beep_pcm_start");
extern const uint8_t beep_pcm_end[]       asm("_binary_beep_pcm_end");
extern const uint8_t doorbell_pcm_start[] asm("_binary_doorbell_pcm_start");
extern const uint8_t doorbell_pcm_end[]   asm("_binary_doorbell_pcm_end");

// Play an embedded mono 16 kHz PCM clip on the speaker, scaled by the ringer volume,
// duplicated L+R. Caller manages the amp + codec output volume.
static void play_pcm(const uint8_t *start, const uint8_t *end)
{
    const int16_t *mono = (const int16_t *)start;
    int nsamples = (int)((end - start) / sizeof(int16_t));
    float scale = s_ringer_vol / 100.0f;
    static int16_t buf[CHIME_FRAMES * 2];
    for (int done = 0; done < nsamples; done += CHIME_FRAMES) {
        int n = (nsamples - done < CHIME_FRAMES) ? (nsamples - done) : CHIME_FRAMES;
        for (int i = 0; i < n; i++) {
            int16_t s = (int16_t)(mono[done + i] * scale);
            buf[2 * i] = buf[2 * i + 1] = s;
        }
        esp_codec_dev_write(s_dev, buf, n * 2 * sizeof(int16_t));
    }
}

void audio_play_chime(void)
{
    if (!s_ready) { ESP_LOGW(TAG, "chime: audio not ready"); return; }
    ESP_LOGI(TAG, "chime: ringer_vol=%u speaker_vol=%u amp_was=%d",
             (unsigned)s_ringer_vol, (unsigned)s_speaker_vol, (int)s_amp_on);
    audio_amp(true);
    tone_vol_full();                  // full codec output; ringer volume sets loudness
    vTaskDelay(pdMS_TO_TICKS(8));     // let the amp settle to avoid a turn-on pop
    float amp = 0.85f * (s_ringer_vol / 100.0f);
    play_tone(784, 180, amp);         // G5
    play_tone(1047, 260, amp);        // C6 — pleasant rising two-note notification chime
    tone_vol_restore();
    audio_amp(false);
}

// Doorbell sound: the linphone "oldphone" telephone-ring sample (doorbell.pcm). Used for
// Play Door Chime and as the door-station ring. Linphone ships no true chime, so this
// stock C4-softphone ring stands in for the doorbell.
void audio_play_doorbell(void)
{
    if (!s_ready) return;
    audio_amp(true);
    tone_vol_full();
    vTaskDelay(pdMS_TO_TICKS(8));
    play_pcm(doorbell_pcm_start, doorbell_pcm_end);
    tone_vol_restore();
    audio_amp(false);
}

static volatile bool s_chime_busy;
static void (*s_chime_fn)(void) = audio_play_chime;   // which sound chime_task plays
static void chime_task(void *arg)
{
    (void)arg;
    s_chime_fn();
    s_chime_busy = false;
    vTaskDelete(NULL);
}

static void chime_async_run(void (*fn)(void))
{
    if (!s_ready || s_chime_busy) return;     // ignore overlap (one chime at a time)
    s_chime_busy = true;
    s_chime_fn = fn;
    if (xTaskCreate(chime_task, "chime", 3072, NULL, 4, NULL) != pdPASS)
        s_chime_busy = false;
}

void audio_chime_async(void)    { chime_async_run(audio_play_chime); }
void audio_doorbell_async(void) { chime_async_run(audio_play_doorbell); }

// ── Incoming-call ring tone (looping) + connect beep ─────────────────────────
static volatile bool s_ringing;      // ring requested (cleared by audio_ring_stop)
static volatile bool s_ringRunning;  // ring task alive (for a synchronous stop)
static volatile bool s_ringDoorbell; // ring with the doorbell sample (door-station call)

static void ring_task(void *arg)
{
    (void)arg;
    s_ringRunning = true;
    tone_vol_full();                               // loud regardless of speaker slider
    float amp = 0.85f * (s_ringer_vol / 100.0f);
    while (s_ringing) {
        if (s_ringDoorbell) {
            // Door-station call: repeat the "ding-dong" doorbell sample with a gap.
            play_pcm(doorbell_pcm_start, doorbell_pcm_end);
            for (int i = 0; i < 25 && s_ringing; i++) vTaskDelay(pdMS_TO_TICKS(100)); // ~2.5s gap
        } else {
            // Classic phone cadence: a "ring-ring" double burst, clear silent gap, repeat.
            // burst 1
            play_tone(659, 280, amp);                   // E5
            if (s_ringing) play_tone(523, 280, amp);    // C5
            if (s_ringing) for (int i = 0; i < 3 && s_ringing; i++) vTaskDelay(pdMS_TO_TICKS(100)); // ~0.3s
            // burst 2
            if (s_ringing) play_tone(659, 280, amp);
            if (s_ringing) play_tone(523, 280, amp);
            // clear gap between ring cycles (~2.5s of silence)
            for (int i = 0; i < 25 && s_ringing; i++) vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    tone_vol_restore();
    s_ringRunning = false;
    vTaskDelete(NULL);
}

// doorbell=true rings with the door chime sample instead of the phone-ring cadence.
void audio_ring_start_ex(bool doorbell)
{
    if (!s_ready || s_ringing) return;
    audio_amp(true);                          // amp on; caller owns amp-off (see sip.c)
    s_ringDoorbell = doorbell;
    s_ringing = true;
    if (xTaskCreate(ring_task, "ring", 3072, NULL, 4, NULL) != pdPASS) {
        s_ringing = false;
        audio_amp(false);
    }
}

void audio_ring_start(void) { audio_ring_start_ex(false); }

void audio_ring_stop(void)
{
    if (!s_ringing) return;
    s_ringing = false;                        // wait for the tone task to finish its write
    for (int i = 0; i < 100 && s_ringRunning; i++) vTaskDelay(pdMS_TO_TICKS(5));  // <=0.5s
}

static void beep_task(void *arg)
{
    (void)arg;
    bool was_on = s_amp_on;                   // if amp already on (in a call), leave it on
    if (!was_on) { audio_amp(true); vTaskDelay(pdMS_TO_TICKS(8)); }
    tone_vol_full();
    play_pcm(beep_pcm_start, beep_pcm_end);   // connect/confirmation beep sample
    tone_vol_restore();
    if (!was_on) audio_amp(false);
    vTaskDelete(NULL);
}

void audio_beep(void)
{
    if (!s_ready) return;
    xTaskCreate(beep_task, "beep", 3072, NULL, 4, NULL);
}

// Blocking beep: plays the heads-up tone synchronously so it actually sounds before the
// caller (auto-answer in sip.c) answers and the call audio reconfigures the codec.
void audio_play_beep(void)
{
    if (!s_ready) return;
    ESP_LOGI(TAG, "heads-up beep (ringer_vol=%d)", s_ringer_vol);
    bool was_on = s_amp_on;
    if (!was_on) { audio_amp(true); vTaskDelay(pdMS_TO_TICKS(8)); }
    tone_vol_full();
    play_pcm(beep_pcm_start, beep_pcm_end);   // connect beep sample (blocking)
    tone_vol_restore();
    if (!was_on) audio_amp(false);
}

// ── Self-test (bring-up / field diagnostic) ──────────────────────────────────
// Exercises the full audio path and reports what can't be seen remotely:
//   1) chime + a steady 440 Hz tone  → speaker + amp (listen)
//   2) 2 s mic capture → logs peak/RMS → mic ADC (watch the log; make noise)
//   3) 5 s mic→speaker loopback        → both at once (talk, hear yourself)
// Runs on its own task; logs progress so a remote operator can follow along.
static volatile bool s_selftest_busy;
static void selftest_task(void *arg)
{
    (void)arg;
    static int16_t buf[256 * 2];     // 16 kHz stereo frames (mic on LEFT slot)

    // Every stage runs at FULL codec output. The 440 Hz tone and the loopback below
    // used to play at s_speaker_vol -- the CALL volume, which the intercom driver
    // legitimately pushes to 0 (sipvol speaker=0) when not in a call. That made the
    // whole test appear dead: the chime was audible (it forces full output) and then
    // nothing, which is exactly the reported symptom. A diagnostic has to exercise the
    // hardware independently of the call mixer.
    ESP_LOGI(TAG, "selftest: (1/3) chime + 440 Hz tone — listen (user_vol=%u call_vol=%u)",
             (unsigned)s_user_vol, (unsigned)s_speaker_vol);
    audio_play_chime();
    vTaskDelay(pdMS_TO_TICKS(250));
    audio_amp(true);
    user_vol_apply();
    vTaskDelay(pdMS_TO_TICKS(8));
    play_tone(440, 1000, 0.8f);
    audio_amp(false);

    ESP_LOGI(TAG, "selftest: (2/3) capturing mic 2s — make some noise...");
    int total = SAMPLE_RATE * 2;     // 2 s of frames
    long long sumsq = 0;
    int peak = 0, count = 0, got = 0;
    long long sqL = 0, sqR = 0; int peakL = 0, peakR = 0, cntL = 0, cntR = 0;
    while (got < total) {
        int n = (total - got < 256) ? (total - got) : 256;
        int b = audio_read(buf, n * 2 * (int)sizeof(int16_t));
        if (b <= 0) break;
        int frames = b / (2 * (int)sizeof(int16_t));
        for (int i = 0; i < frames; i++) {
            int s = buf[2 * i];                      // LEFT = mic 1
            int a = s < 0 ? -s : s;
            if (a > peak) peak = a;
            sumsq += (long long)s * s;
            count++;
            int r = buf[2 * i + 1];                  // RIGHT = mic 2 (ES7210 boards)
            int ar = r < 0 ? -r : r;
            if (a  > peakL) peakL = a;
            if (ar > peakR) peakR = ar;
            sqL += (long long)s * s; cntL++;
            sqR += (long long)r * r; cntR++;
        }
        got += frames;
    }
    int rms = count ? (int)sqrt((double)sumsq / count) : 0;
    ESP_LOGI(TAG, "selftest: mic peak=%d rms=%d (full scale 32767) — %s",
             peak, rms, peak > 200 ? "SIGNAL OK" : "very quiet / check mic");
#if defined(MMK_HAS_ES7210) && MMK_HAS_ES7210
    // This board has TWO mics on the ES7210 (MIC1 -> LEFT slot, MIC2 -> RIGHT).
    // Report them separately: one dead mic is invisible in a combined figure, and
    // "both silent" vs "one silent" are completely different faults.
    ESP_LOGI(TAG, "selftest: mic1(L) peak=%d rms=%d | mic2(R) peak=%d rms=%d",
             peakL, cntL ? (int)sqrt((double)sqL / cntL) : 0,
             peakR, cntR ? (int)sqrt((double)sqR / cntR) : 0);
#endif

    // RECORD-then-PLAY, not live loopback. A simultaneous open mic + speaker at the
    // gain this mic needs howls -- acoustic feedback, observed on hardware. Capturing
    // first and replaying after breaks the loop, and it is the better diagnostic
    // anyway: you hear exactly what the mic captured, at a level you can judge.
    // Buffer lives in PSRAM (3s stereo @16k = 192 KB).
    const int REC_SEC = 3;
    const int rec_frames = SAMPLE_RATE * REC_SEC;
    int16_t *rec = heap_caps_malloc((size_t)rec_frames * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!rec) {
        ESP_LOGW(TAG, "selftest: (3/3) skipped — no PSRAM for the record buffer");
    } else {
        ESP_LOGI(TAG, "selftest: (3/3) recording %ds — say something...", REC_SEC);
        esp_codec_dev_set_in_gain(AUDIO_IN_DEV, MIC_GAIN_MAX_DB);
        int rgot = 0, rpeak = 0;
        while (rgot < rec_frames) {
            int n = (rec_frames - rgot < 256) ? (rec_frames - rgot) : 256;
            int b = audio_read(buf, n * 2 * (int)sizeof(int16_t));
            if (b <= 0) break;
            int frames = b / (2 * (int)sizeof(int16_t));
            for (int i = 0; i < frames; i++) {
                // Mix both mics where present (ES7210 MIC1+MIC2): averaging two
                // capsules of the same sound is ~3 dB better SNR than either alone.
                // On single-mic boards the RIGHT slot mirrors LEFT, so this is a no-op.
                int mixed = ((int)buf[2 * i] + (int)buf[2 * i + 1]) / 2;
                int16_t sm = (int16_t)(mixed > 32767 ? 32767 : (mixed < -32768 ? -32768 : mixed));
                int a = sm < 0 ? -sm : sm; if (a > rpeak) rpeak = a;
                rec[2 * (rgot + i)]     = sm;            // duplicate to both slots
                rec[2 * (rgot + i) + 1] = sm;
            }
            rgot += frames;
        }
        esp_codec_dev_set_in_gain(AUDIO_IN_DEV, MIC_GAIN_DEFAULT_DB);

        // Normalise to ~70% full scale so a quiet mic is still clearly audible on
        // playback; this is a diagnostic, not a fidelity path.
        if (rpeak > 64) {
            int gain256 = (int)((0.70f * 32767.0f) * 256.0f) / rpeak;
            if (gain256 > 64 * 256) gain256 = 64 * 256;      // cap at +36 dB
            if (gain256 > 256) {
                for (int i = 0; i < rgot * 2; i++) {
                    int v = (rec[i] * gain256) >> 8;
                    rec[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
                }
                ESP_LOGI(TAG, "selftest: playback gain x%d.%02d (peak was %d)",
                         gain256 / 256, ((gain256 % 256) * 100) / 256, rpeak);
            }
        }

        ESP_LOGI(TAG, "selftest: (3/3) playing it back — peak=%d", rpeak);
        audio_amp(true);
        user_vol_apply();
        vTaskDelay(pdMS_TO_TICKS(8));
        for (int off = 0; off < rgot; off += 256) {
            int n = (rgot - off < 256) ? (rgot - off) : 256;
            audio_write(&rec[2 * off], n * 2 * (int)sizeof(int16_t));
        }
        audio_amp(false);
        heap_caps_free(rec);
    }

    ESP_LOGI(TAG, "selftest: done");
    s_selftest_busy = false;
    vTaskDelete(NULL);
}

// Ringback reuses the ring cadence on this board: audible is the point, and the
// ESP ring task is already the tested path. The T3 uses a distinct tone.
void audio_ringback_start(void) { audio_ring_start(); }
void audio_ringback_stop(void)  { audio_ring_stop(); }

void audio_selftest_async(void)
{
    if (!s_ready || s_selftest_busy) return;
    s_selftest_busy = true;
    if (xTaskCreate(selftest_task, "atest", 4096, NULL, 5, NULL) != pdPASS)
        s_selftest_busy = false;
}
