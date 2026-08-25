/* MMKeypad app entry point for the T3 (RK3188) Linux image.
 * Drives the shared UI (ui.c) on the native Linux platform: lv_linux_fbdev
 * (/dev/fb0, RGB565, rotated 90 to landscape), our own gslX680 evdev touch,
 * the NuVoxel splash, config, and the net.c TCP/JSON protocol client. Mirrors
 * the ESP-IDF app_main() init sequence, with a pthread mutex standing in for
 * esp_lvgl_port's lock (net.c's server runs in its own thread and calls back
 * into the UI). */
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "lvgl.h"

#include "audio.h"
#include "sip.h"
#include "board.h"
#include "bsp.h"
#include "config.h"
#include "device.h"
#include "net.h"
#include "sddp.h"
#include "ui.h"
#include "wifi_setup.h"

/* Set by bsp_linux.c; point it at our fbdev display so bsp_apply_orientation
 * (called from on_net_display_change) can rotate. */
extern lv_display_t *g_bsp_display;

/* Logical landscape dimensions the display was created at (bsp_linux.c derives
 * them from the real framebuffer, not a hardcoded panel size). Touch scales to
 * these so the same build maps correctly on any T3 panel. */
extern int g_bsp_lw, g_bsp_lh;
/* 1 = fb is portrait and the display flush rotates 90° (7"); 0 = fb is already
 * landscape and blits direct (10"). Touch must use the SAME convention. */
extern int g_bsp_rotate;

/* Stands in for esp_lvgl_port's lock: net.c's server thread calls the UI
 * callbacks below concurrently with the main lv_timer_handler loop, and LVGL
 * is not thread-safe. */
static pthread_mutex_t g_lv_lock = PTHREAD_MUTEX_INITIALIZER;
static int app_lock(void) { pthread_mutex_lock(&g_lv_lock); return 1; }
static void app_unlock(void) { pthread_mutex_unlock(&g_lv_lock); }

/* Non-static wrappers so wifi_setup.c's scan/connect worker threads can take the
 * same LVGL lock when they mutate the onboarding screen. */
void ui_lock(void) { app_lock(); }
void ui_unlock(void) { app_unlock(); }

static uint32_t tick_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* --- Touch input (gslX680) ---
 * We do NOT use LVGL's lv_evdev: on this 32-bit musl build its
 * `struct input_event` is 24 bytes (64-bit time_t) while this stock 3.0.36
 * kernel emits 16-byte records, so it reads type/code/value at the wrong
 * offsets and sees nothing. So read the kernel's exact 16-byte layout here.
 * The panel reports multitouch protocol A (ABS_MT_*), 1280x800 landscape. */
struct kevent {
    uint32_t tv_sec, tv_usec;
    uint16_t type, code;
    int32_t value;
};
#define EV_ABS 0x03
#define EV_SYN 0x00
#define EV_KEY 0x01
#define BTN_TOUCH 0x14a
#define ABS_MT_POSITION_X 0x35
#define ABS_MT_POSITION_Y 0x36
#define ABS_MT_TRACKING_ID 0x39

/* Query the touch controller's raw coordinate range (EVIOCGABS) so the mapping
 * scales to whatever panel is attached instead of assuming the 7"'s range.
 * input_absinfo is all int32 (no timeval), so it has none of the input_event
 * ABI mismatch that forces the by-hand event read below. Falls back to the 7"'s
 * known 1280x800 raw range if the query fails. */
struct t3_absinfo { int32_t value, minimum, maximum, fuzz, flat, resolution; };
#ifndef EVIOCGABS
#define EVIOCGABS(a) _IOR('E', 0x40 + (a), struct t3_absinfo)
#endif
static int32_t s_tx_max = 1280;   /* raw ABS_MT_POSITION_X max */
static int32_t s_ty_max = 800;    /* raw ABS_MT_POSITION_Y max */

static void touch_read_ranges(int fd) {
    struct t3_absinfo ai;
    if (fd >= 0 && ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ai) == 0 && ai.maximum > 0)
        s_tx_max = ai.maximum;
    if (fd >= 0 && ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ai) == 0 && ai.maximum > 0)
        s_ty_max = ai.maximum;
    printf("touch: raw range %dx%d -> logical %dx%d\n", s_tx_max, s_ty_max, g_bsp_lw, g_bsp_lh);
}

static int touch_fd = -1;
static int32_t touch_x = 0, touch_y = 0;
static bool touch_pressed = false;
static bool s_touch_has_btn = false; /* panel reports BTN_TOUCH (authoritative) */
static lv_indev_t *g_touch;

/* read_cb just reports the latest state gathered by touch_thread (below);
 * the thread is what actually reads the evdev fd. LVGL indev is in EVENT
 * mode, so this only runs when the thread calls lv_indev_read(). */
/* Map the panel's raw touch (x 0..1280, y 0..800) into the LANDSCAPE logical
 * frame (1280x800). We rotate the display ourselves, so LVGL doesn't transform
 * input -- we do it here. First-guess transform derived from the reported
 * corner behaviour; confirmed/corrected from /tmp/touch.log corner taps. */
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    /* Map raw touch -> logical landscape, scaling by the detected touch range
     * (s_tx/ty_max) and display size (g_bsp_lw/lh). The transform must match the
     * display flush: on a portrait/rotated panel (7") swap+flip the axes; on a
     * landscape/direct panel (10") map straight through. Magnitudes are
     * data-driven so the same build fits any T3 panel. */
    const int32_t LW = g_bsp_lw, LH = g_bsp_lh;
    int32_t x, y;
    if (g_bsp_rotate) {
        x = LW - (int32_t)touch_y * LW / s_ty_max;
        y = (int32_t)touch_x * LH / s_tx_max;
    } else {
        x = (int32_t)touch_x * LW / s_tx_max;
        y = (int32_t)touch_y * LH / s_ty_max;
    }
    if (x < 0) x = 0; else if (x > LW - 1) x = LW - 1;
    if (y < 0) y = 0; else if (y > LH - 1) y = LH - 1;
    data->point.x = x;
    data->point.y = y;
    data->state = touch_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* Event-driven (interrupt-based) touch: block on the evdev fd until the
 * kernel delivers a touch event, drain the batch, then push the new state
 * into LVGL. No polling. */
static void *touch_thread(void *arg) {
    (void)arg;
    struct pollfd pfd = {.fd = touch_fd, .events = POLLIN};
    for (;;) {
        if (poll(&pfd, 1, -1) <= 0)
            continue; /* sleeps here until a touch interrupt wakes us */
        struct kevent ev;
        while (read(touch_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
                /* When present, BTN_TOUCH is the authoritative press/release. The
                 * 10"'s goodix is MT protocol-A and NEVER sends TRACKING_ID=-1, so
                 * relying on that alone left press stuck on -> no click ("frozen"),
                 * hence BTN_TOUCH. Once we've seen it, TRACKING_ID must NOT also set
                 * press (that would re-stick the goodix). */
                s_touch_has_btn = true;
                touch_pressed = (ev.value != 0);
            } else if (ev.type == EV_ABS) {
                if (ev.code == ABS_MT_POSITION_X)
                    touch_x = ev.value;
                else if (ev.code == ABS_MT_POSITION_Y)
                    touch_y = ev.value;
                else if (ev.code == ABS_MT_TRACKING_ID) {
                    /* type-B path for panels that report no BTN_TOUCH (e.g. some
                     * 7" Silead GSL1680): -1 = up, >=0 = down. We only drive PRESS
                     * from here when the panel has no BTN_TOUCH, so the goodix
                     * (which never sends -1) can't get stuck. Release on -1 is
                     * always safe (goodix never sends it). */
                    if (ev.value == -1)
                        touch_pressed = false;
                    else if (!s_touch_has_btn)
                        touch_pressed = true;
                }
            } else if (ev.type == EV_SYN && g_touch) {
                /* Deliver EACH frame separately, not once per poll-wakeup:
                 * a crisp tap's press+release frames can arrive in the same
                 * wakeup, and collapsing them into one read would drop the
                 * press so LVGL never registers a click (swipes still worked
                 * because their motion spans many frames/wakeups). */
                app_lock();
                lv_indev_read(g_touch);
                app_unlock();
            }
        }
    }
    return NULL;
}

/* ── net callbacks (run on net.c's server thread) ───────────────────────────
 * Each marshals onto the LVGL side under the lock, mirroring main.c. */
static void on_net_connect(void) {
    printf("net: driver connected\n");
    if (app_lock()) { ui_set_connected(true); app_unlock(); }
}
static void on_net_disconnect(void) {
    printf("net: driver disconnected\n");
    if (app_lock()) { ui_set_connected(false); app_unlock(); }
}
static void on_net_state(const media_state_t *st) {
    if (app_lock()) { ui_set_state(st); app_unlock(); }
}
static void on_net_identify(void) {
    if (app_lock()) { ui_identify(); app_unlock(); }
}
static void on_net_announce(const char *text, bool chime) {
#if MMK_HAS_AUDIO
    if (chime) audio_chime_async();   // out the speaker (non-blocking)
#else
    (void)chime;
#endif
    if (app_lock()) { ui_announce(text); app_unlock(); }
}
/* Call state -> on-screen call UI. Fired from the libre SIP thread, so it must
 * take the app lock before touching LVGL (same rule as the ESP32's handler). */
static void on_sip_call(const char *event, const char *peer) {
    printf("call: %s peer=\"%s\"\n", event ? event : "", peer ? peer : "");
    if (app_lock()) { ui_call(event, peer); app_unlock(); }
}
static void on_net_endpoints(const intercom_target_t *eps, int n) {
    if (app_lock()) { ui_set_endpoints(eps, n); app_unlock(); }
}
static void on_net_rooms(const room_t *rooms, int n) {
    if (app_lock()) { ui_set_rooms(rooms, n); app_unlock(); }
}
static void on_net_favorites(const favorite_t *favs, int n) {
    if (app_lock()) { ui_set_favorites(favs, n); app_unlock(); }
}
static void on_net_display_change(void) {
    if (app_lock()) {
        bsp_apply_orientation(g_settings.orientation);
        ui_request_rebuild();
        app_unlock();
    }
}

/* ── periodic UI ticks (run inside lv_timer_handler -> already under lock) ── */
static void tick_progress_cb(lv_timer_t *t) { (void)t; ui_tick_progress(); }
static void tick_screensaver_cb(lv_timer_t *t) { (void)t; ui_tick_screensaver(); }

/* NuVoxel boot splash: full-screen logo on the top layer, removed after a
 * short delay to reveal the UI. Rendered through LVGL so it inherits the
 * display's 90-degree rotation (correct orientation, unlike a raw fb blit). */
static void splash_opa_cb(void *obj, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}
static void splash_zoom_cb(void *obj, int32_t v) {
    lv_image_set_scale((lv_obj_t *)obj, (uint16_t)v); /* 256 = 1.0x */
}
static void splash_fadeout_done(lv_anim_t *a) {
    lv_obj_delete((lv_obj_t *)a->user_data); /* the splash root */
}
static void splash_done(lv_timer_t *t) {
    lv_obj_t *splash = lv_timer_get_user_data(t);
    lv_timer_delete(t);
    /* fade the whole splash out, then delete it, so the reveal isn't a hard cut */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, splash);
    lv_anim_set_exec_cb(&a, splash_opa_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, 350);
    lv_anim_set_user_data(&a, splash);
    lv_anim_set_completed_cb(&a, splash_fadeout_done);
    lv_anim_start(&a);
}

/* Animated NuVoxel boot splash — logo fades up and gently "breathes" (subtle
 * zoom in/out), evoking the old Control4 boot animation, then fades out to
 * reveal the UI. T3-local (main_linux), so it inherits the display rotation. */
static void show_splash(void) {
    lv_obj_t *splash = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(splash);
    lv_obj_set_size(splash, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(splash, lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_opa(splash, LV_OPA_COVER, 0);
    lv_obj_clear_flag(splash, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *logo = lv_image_create(splash);
    lv_image_set_src(logo, "A:/usr/share/nuvoxel-logo.png");
    lv_obj_center(logo);

    /* fade the logo in */
    lv_anim_t fin;
    lv_anim_init(&fin);
    lv_anim_set_var(&fin, logo);
    lv_anim_set_exec_cb(&fin, splash_opa_cb);
    lv_anim_set_values(&fin, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&fin, 650);
    lv_anim_set_path_cb(&fin, lv_anim_path_ease_out);
    lv_anim_start(&fin);

    /* gentle continuous breathing (≈0.92x ↔ 1.06x), ping-pong forever */
    lv_anim_t br;
    lv_anim_init(&br);
    lv_anim_set_var(&br, logo);
    lv_anim_set_exec_cb(&br, splash_zoom_cb);
    lv_anim_set_values(&br, 236, 272);
    lv_anim_set_duration(&br, 1100);
    lv_anim_set_playback_duration(&br, 1100);
    lv_anim_set_repeat_count(&br, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&br, lv_anim_path_ease_in_out);
    lv_anim_start(&br);

    lv_timer_create(splash_done, 2200, splash);
}

int main(void) {
    /* init redirects stdout to /data/mmkeypad.log, so libc makes it FULLY
     * buffered (4K blocks). That silently hid diagnostics for hours: SIP events
     * (libre's re_printf) sat unflushed while other subsystems' lines appeared,
     * making a registered panel look like it had never received SIP config at
     * all. Line-buffer it so the log reflects reality as it happens. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);

    lv_init();
    lv_tick_set_cb(tick_ms);

    /* Full-frame rotating fbdev (bsp_linux.c): renders landscape and rotates
     * the whole frame into the portrait panel each flush -- avoids LVGL fbdev's
     * partial-rotation corruption. We do the rotation, so display rotation is 0
     * and touch is mapped manually below. */
    lv_display_t *disp = bsp_display_create();

    touch_fd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
    touch_read_ranges(touch_fd);   /* after bsp_display_create() set g_bsp_lw/lh */
    lv_indev_t *touch = lv_indev_create();
    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch, touch_read_cb);
    lv_indev_set_display(touch, disp);
    lv_indev_set_mode(touch, LV_INDEV_MODE_EVENT); /* driven by touch_thread, not a timer */
    g_touch = touch;
    pthread_t touch_tid;
    pthread_create(&touch_tid, NULL, touch_thread, NULL);
    pthread_detach(touch_tid);

    /* Load persisted settings (flat-file NVS shim on /data). */
    settings_load();

    /* Compute the eFuse-rooted device identity + load any stored license
     * (offline). The cloud check-in itself is started later, after net is up. */
    device_init();

#if MMK_HAS_AUDIO
    /* Bring up ALSA (RT3261) + replay the validated mixer route, which now also
     * enables the capture (mic) path for the SIP intercom. Playback goes out
     * rt3261-aif2 (PCM dev 1) at 48 kHz: the speaker is wired to DAC2, reachable
     * only via aif2, whose SYSCLK is pinned to 24.576 MHz — and the codec requires
     * sysclk == rate * 256 * pd, which 44.1 kHz can never satisfy. */
    audio_start();
    {   /* Apply saved Sound settings: ringer amplitude AND the master output level.
         * The ESP path does both (see firmware-idf/main/main.c); the T3 applied only
         * the ringer, so s_vol -- which scales every locally generated sound here --
         * stayed at its compiled default and never followed the saved slider. */
        uint8_t v0 = g_settings.muted ? 0 : g_settings.ringer_volume;
        audio_set_ringer_volume(v0);
        audio_set_user_volume(v0);
    }
    /* Phase-3 intercom: SIP UA on libre. Control rides the same :6700 link
     * (net.c routes `sip`/`call`/`callcfg` to sip_handle), so there is no second
     * port to bind in Composer. */
    if (audio_ready()) {
        sip_init();
        sip_set_call_cb(on_sip_call);
    }
#endif

    /* Build the real UI + NuVoxel splash, apply saved brightness. */
    app_lock();
    ui_begin();
    show_splash();
    bsp_set_backlight(g_settings.brightness ? g_settings.brightness : 80);
    lv_timer_create(tick_progress_cb, 100, NULL);
    lv_timer_create(tick_screensaver_cb, 1000, NULL);
    app_unlock();

    /* Bring up the TCP/JSON protocol client (net.c spawns its own server
     * thread; callbacks marshal onto the UI under g_lv_lock). */
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
    sddp_start(6700);   /* SDDP discovery so the C4 Director finds us */

    /* Best-effort cloud check-in (registration / license sync / OTA). Runs on
     * its own thread and waits for connectivity; never required to function. */
    device_start();

    /* If the device has no network yet (BYOH T3 ships WiFi-only), put up the
     * on-screen WiFi picker over the top layer. Runs its blocking probe on a
     * worker thread; closes itself once wlan0 gets an IP. */
    wifi_setup_maybe_show();

    printf("mmkeypad app running (fw_version=\"%s\")\n", fw_version());
    fflush(stdout);

    for (;;) {
        // Pace the render loop to ~60 fps. FBIO_WAITFORVSYNC "succeeds" on this
        // kernel but does NOT block, so relying on it alone spins the loop at 100%
        // CPU and flickers the panel. Cap each cycle to a 16 ms frame budget:
        // lv_timer_handler only redraws dirty areas, so a static screen costs
        // ~nothing and the loop mostly sleeps. bsp_wait_vsync() stays as a
        // best-effort tear reducer before the flush.
        uint32_t t0 = tick_ms();
        bsp_wait_vsync();
        app_lock();
        lv_timer_handler();
        app_unlock();
        uint32_t dt = tick_ms() - t0;
        if (dt < 16)
            usleep((16 - dt) * 1000);
    }
    return 0;
}
