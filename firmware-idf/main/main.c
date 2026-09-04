// MMKeypad — native ESP-IDF firmware.
// Wires the ported stack together. Two board profiles share this entry point,
// selected at compile time via board.h feature flags:
//   • s3-lcdwiki  — display + touch UI, WiFi, audio/SIP intercom.
//   • p4-poe-eth  — headless wired node: Ethernet + the TCP/protocol server
//                   and SDDP discovery. (Display + audio off.)

#include "config.h"
#include "net.h"
#include "sddp.h"
#include "board.h"
#include "device.h"  // device identity + manifest
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include <string.h>

#if MMK_NET_WIFI
#include "wifi.h"
#endif
#ifdef MMK_C6_OTA
#include "c6_ota.h"           // one-shot C6 esp_hosted-slave OTA (bench build)
#endif
#ifdef MMK_BLE_SPIKE
#include "ble_spike.h"        // BLE advertise spike (C6 controller over VHCI)
#endif
#if MMK_NET_ETH
#include "eth.h"
#endif
#if MMK_NET_WIFI || MMK_NET_ETH
#include "esp_netif_sntp.h"   // SNTP block below runs for ANY networked board
#endif
#if MMK_HAS_DISPLAY
#include "bsp.h"
#include "ui.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#endif
#if MMK_HAS_AUDIO
#include "audio.h"
#include "sip.h"
#endif
#ifdef PIN_RGB_LED
#include "halo.h"
#endif

static const char *TAG = "main";

// ── net callbacks (net task) ─────────────────────────────────────────────────
// On display boards these marshal UI work onto the LVGL task via the port lock.
// On headless boards they just log (no UI), and announce still drives the chime.
static void on_net_connect(void)    { ESP_LOGI(TAG, "driver connected");
#if MMK_HAS_DISPLAY
    if (lvgl_port_lock(0)) { ui_set_connected(true); lvgl_port_unlock(); }
#endif
}
static void on_net_disconnect(void) { ESP_LOGI(TAG, "driver disconnected");
#if MMK_HAS_DISPLAY
    if (lvgl_port_lock(0)) { ui_set_connected(false); lvgl_port_unlock(); }
#endif
}

static void on_net_state(const media_state_t *st)
{
#if MMK_HAS_DISPLAY
    if (lvgl_port_lock(0)) { ui_set_state(st); lvgl_port_unlock(); }
#else
    ESP_LOGI(TAG, "state: room=\"%s\" power=%d playing=%d src=\"%s\" title=\"%s\" vol=%d",
             st->room, st->power, st->playing, st->source.name, st->title, st->volume);
#endif
}

static void on_net_identify(void)
{
#if MMK_HAS_DISPLAY
    if (lvgl_port_lock(0)) { ui_identify(); lvgl_port_unlock(); }
#else
    ESP_LOGI(TAG, "identify");
#endif
}

static void on_net_announce(const char *text, bool chime)
{
    ESP_LOGI(TAG, "announce: chime=%d text=\"%s\"", chime, text ? text : "");
#if MMK_HAS_DISPLAY
    if (lvgl_port_lock(0)) { ui_announce(text); lvgl_port_unlock(); }
#endif
#if MMK_HAS_AUDIO
    if (chime) audio_chime_async();      // non-blocking; out the speaker
#endif
}

// Driver-pushed multiroom list -> on-screen add-rooms panel (LVGL task).
static void on_net_rooms(const room_t *rooms, int n)
{
    ESP_LOGI(TAG, "rooms: %d", n);
#if MMK_HAS_DISPLAY
    if (lvgl_port_lock(0)) { ui_set_rooms(rooms, n); lvgl_port_unlock(); }
#else
    (void)rooms; (void)n;
#endif
}
// Driver-pushed room navigator favorites -> on-screen favorites grid (LVGL task).
static void on_net_favorites(const favorite_t *favs, int n)
{
    ESP_LOGI(TAG, "favorites: %d", n);
#if MMK_HAS_DISPLAY
    if (lvgl_port_lock(0)) { ui_set_favorites(favs, n); lvgl_port_unlock(); }
#else
    (void)favs; (void)n;
#endif
}
static void on_net_endpoints(const intercom_target_t *eps, int n)
{
#if MMK_HAS_DISPLAY
    if (lvgl_port_lock(0)) { ui_set_endpoints(eps, n); lvgl_port_unlock(); }
#else
    (void)eps; (void)n;
#endif
}

#if MMK_HAS_AUDIO && MMK_HAS_DISPLAY
// SIP call state (esp_rtc task) -> on-screen call UI (marshalled onto LVGL task).
static void on_call_event(const char *event, const char *peer)
{
    ESP_LOGI(TAG, "call: %s peer=\"%s\"", event ? event : "", peer ? peer : "");
    if (lvgl_port_lock(0)) { ui_call(event, peer); lvgl_port_unlock(); }
}
#endif

static void on_net_display_change(void)
{
#if MMK_HAS_DISPLAY
    // net.c already persisted g_settings (orientation/layout). Apply both live:
    // rotation immediately, then rebuild the UI for the (possibly new) resolution.
    if (lvgl_port_lock(0)) {
        bsp_apply_orientation(g_settings.orientation);
        ui_request_rebuild();
        lvgl_port_unlock();
    }
#endif
}

#if MMK_HAS_DISPLAY
// ── periodic UI ticks (run on the LVGL task) ────────────────────────────────
static volatile int64_t s_lvgl_beat_us = 0;   // last time the LVGL task ran a tick
static void tick_progress_cb(lv_timer_t *t) {
    (void)t;
    static bool s_wdt_sub = false;
    if (!s_wdt_sub) { esp_task_wdt_add(NULL); s_wdt_sub = true; }   // watch THIS (LVGL) task
    esp_task_wdt_reset();
    s_lvgl_beat_us = esp_timer_get_time();
    ui_tick_progress();
}
static void tick_screensaver_cb(lv_timer_t *t) { (void)t; ui_tick_screensaver(); }

// #1: software watchdog + heap-health log on an INDEPENDENT task (survives an LVGL wedge).
// Reboots only if the LVGL task hasn't ticked for STALL_US — the exact "display + touch
// both frozen" symptom — so the panel recovers instead of bricking until power-cycle.
// Kept separate from the Task WDT to avoid false reboots from legitimate idle starvation.
static void health_task(void *arg)
{
    (void)arg;
    const int64_t STALL_US = 15LL * 1000 * 1000;   // 15s with no LVGL tick => frozen
    int sec = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int64_t age = esp_timer_get_time() - s_lvgl_beat_us;
        if (++sec >= 60) {   // heap health once a minute (surfaces a slow leak / fragmentation)
            sec = 0;
            ESP_LOGI(TAG, "health: free=%u largest=%u lvgl_age_ms=%lld",
                     (unsigned)esp_get_free_heap_size(),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
                     (long long)(age / 1000));
        }
        if (s_lvgl_beat_us != 0 && age > STALL_US) {
            ESP_LOGE(TAG, "LVGL stalled %lld ms (free=%u) -- rebooting",
                     (long long)(age / 1000), (unsigned)esp_get_free_heap_size());
            esp_restart();
        }
    }
}
#endif

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    settings_load();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_LOGI(TAG, "board: %s (display=%d audio=%d eth=%d wifi=%d)",
             MMK_BOARD_NAME, MMK_HAS_DISPLAY, MMK_HAS_AUDIO, MMK_NET_ETH, MMK_NET_WIFI);

    // Device identity (local): a stable id from the device MAC.
    device_init();

#if MMK_HAS_DISPLAY
    // Display init probes the panel — on the DSI boards the vendor BSP READS the
    // panel ID over DSI to auto-select the panel driver (so one firmware serves
    // the whole panel family: 4"/5"/7"/10.1"). That read busy-waits with NO
    // timeout in esp_lcd; a marginal panel ribbon wedges boot forever, pinning a
    // core so even IDLE starves. Arm a panic watchdog over BOTH idle cores around
    // the probe: a good ribbon reads in ~1s and never trips it; a marginal one
    // REBOOTS and retries (self-heals); a truly-absent panel reboot-loops, which
    // is the correct signal for a manufacturing fault. Runtime WDT (idle checks
    // OFF, for heavy TLS+audio) is set below, after the display is up.
    { esp_task_wdt_config_t w = { .timeout_ms = 6000,
        .idle_core_mask = (1u << 0) | (1u << 1), .trigger_panic = true };
      esp_task_wdt_reconfigure(&w); }
    // Display + touch first (claims internal SPI/DMA RAM before WiFi).
    bsp_display_start();
#ifdef MMK_DISPLAY_ROTATE_90
    // Native-portrait DSI panel mounted landscape: rotate BOTH the LVGL display
    // (→ 1280x800, enables the X4 UI) and the touch controller to match (the port
    // doesn't rotate touch with the display). bsp_rotate_landscape does both.
    if (lvgl_port_lock(0)) { bsp_rotate_landscape(); lvgl_port_unlock(); }
#endif
#endif

#if MMK_HAS_AUDIO
    // Audio codec (ES8311 over the shared touch I2C bus + I2S). Before WiFi so its
    // I2S DMA buffers come from internal RAM. Non-fatal if it fails.
    audio_start();
    {   // apply saved Sound settings: ringer amplitude AND master output level
        uint8_t v0 = g_settings.muted ? 0 : g_settings.ringer_volume;
        audio_set_ringer_volume(v0);
        audio_set_user_volume(v0);
    }
#endif

#ifdef PIN_RGB_LED
    // "Halo" RGB LED (nightlight/accent + ring pulse). Driver pushes the configured
    // color via a `halo` message; default idle off, pulse blue.
    halo_init();
#endif

#if MMK_HAS_DISPLAY
    if (lvgl_port_lock(0)) {
        ui_begin();
        ui_splash();                 // NuVoxel logo over the UI for ~1.8s at boot
        bsp_set_backlight(g_settings.brightness ? g_settings.brightness : 80);
        // 100ms: publishes decoded cover art promptly + advances progress/reconcile
        // (the 1s-cadence parts self-throttle internally).
        lv_timer_create(tick_progress_cb, 100, NULL);
        lv_timer_create(tick_screensaver_cb, 1000, NULL);
        lvgl_port_unlock();
    }
#if MMK_SNAPSHOT
    ui_snapshot_start();   // dev: LVGL framebuffer-over-serial (tools/esp_shot.py)
#endif
    // #1: watch the LVGL task with the Task WDT so a wedge PANICS with a backtrace — that
    // tells us exactly where it's stuck (SPI flush / touch-I2C / a lock). Idle-core checks
    // off (avoid false reboots under heavy TLS+audio); 8s > any legit LVGL op, < the ~16s wedge.
    { esp_task_wdt_config_t w = { .timeout_ms = 8000, .idle_core_mask = 0, .trigger_panic = true };
      esp_task_wdt_reconfigure(&w); }
    // #1: independent watchdog/heap-health monitor (heap trend + backstop reboot on stall).
    xTaskCreate(health_task, "health", 3072, NULL, 6, NULL);
#endif

    // Bring up the network transport (blocks until an IP, or a timeout).
    // Network services only run when a transport is configured. Starting the TCP
    // server / SDDP / httpd with NO network interface up (e.g. crowpanel before its
    // C6 WiFi is wired) leaves lwip's select() over zero netifs corrupting the heap.
    // The transport is brought up first so a netif exists before these bind.
#if MMK_NET_WIFI || MMK_NET_ETH
    // WIRED FIRST. On the backbox-poe carrier Ethernet is the link the product is
    // built around (it is the same cable that powers it), so it takes precedence
    // and WiFi is the fallback. Both flags are set on ws43 because one image runs
    // with and without the carrier: eth_start() returns quickly when no PHY
    // answers, and WiFi then comes up as it always did.
    //
    // Deliberately NOT both at once — two interfaces on one LAN means two IPs,
    // ambiguous ARP and SDDP announcing twice.
#if MMK_NET_ETH
    eth_start();
#endif
#if MMK_NET_WIFI
#if MMK_NET_ETH
    if (!eth_is_up())
#endif
        wifi_start();
#endif

#ifdef MMK_C6_OTA
    // Bench one-shot: the esp_hosted transport to the C6 is up now (esp_wifi_init
    // inside wifi_start brings up the SDIO link + RPC), so OTA the C6 slave from
    // the `model` partition. Reads locally — needs no WiFi join to fetch the image.
    // Skips if the slave already reports the target version; on success it
    // activates the new slave and reboots the host. See board.h (MMK_C6_OTA).
    c6_ota_run();
#endif
#ifdef MMK_BLE_SPIKE
    // BLE advertise spike: NimBLE host on the P4, C6 as controller over the
    // esp_hosted VHCI. Transport is up (wifi_start ran); needs the 2.12.x C6
    // slave with BT (flashed via MMK_C6_OTA).
    ble_spike_start();
#endif

    // Real clock via SNTP — required for TLS cert validation (album-art HTTPS + OTA).
    // Without it the clock sits near 1970 and every https fetch fails on cert dates. Async;
    // art retries + per-track re-fetch pick it up once time syncs.
    {
        esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        esp_netif_sntp_init(&sc);
    }

    static const net_callbacks_t cb = {
        .on_connect = on_net_connect,
        .on_disconnect = on_net_disconnect,
        .on_state = on_net_state,
        .on_identify = on_net_identify,
        .on_display_change = on_net_display_change,
        .on_announce = on_net_announce,
        .on_endpoints = on_net_endpoints,
        .on_rooms = on_net_rooms,
        .on_favorites = on_net_favorites,
    };
    net_start(6700, &cb);
    sddp_start(6700);

    // Start device services (open build: a no-op).
    device_start();

#if MMK_HAS_AUDIO
    // Phase-3 intercom: SIP UA. Control rides the :6700 link (net.c routes
    // `sip`/`call` to sip_handle), so there's no second port to bind in Composer.
    if (audio_ready()) {
        sip_init();
#if MMK_HAS_DISPLAY
        sip_set_call_cb(on_call_event);   // drive the on-screen call UI
#endif
    }
#endif
#else
    ESP_LOGW(TAG, "no network transport for this board — net/sddp/web disabled");
#endif

    ESP_LOGI(TAG, "boot complete (fw %s)", fw_version());
}
