/* Headless LVGL snapshot harness for the MMKeypad UI.
 *
 * Runs the REAL shared ui.c (firmware-idf/main/ui.c) against a dummy in-memory
 * display at a chosen resolution, feeds it a canned now-playing state, renders
 * one frame, and writes a PNG. No window, no GPU, no hardware -- so it works on
 * a plain macOS/Linux dev box and its output can be viewed directly.
 *
 *   usage:  mmk-sim <width> <height> <out.png>
 *
 *   env knobs (applied to g_settings before ui_begin):
 *     MMK_THEME=0|1       0 Control4 X4, 1 Home Assistant
 *     MMK_LAYOUT=0|1|2    Cover / Fit / Compact
 *     MMK_BG=0..3         Navigator / Ocean / Dusk / Graphite
 *     MMK_ORIENT=0..3     stored orientation byte (informational; size wins)
 *     MMK_PLAYING=0|1     1 = playing media (default), 0 = idle / powered-down
 *     MMK_NBTN=<n>        number of sample keypad buttons (default 6)
 *     MMK_ROT=0|1|2       pin the now-playing bar's rotating line
 *     MMK_IC=1            open the intercom target picker
 *     MMK_CALL=<event>    call screen: incoming | outgoing | active
 *     MMK_CALLPEER=<name> who is calling (default "Front Door")
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#include "lvgl.h"
#include "others/snapshot/lv_snapshot.h"
#include "ui.h"
#include "config.h"   /* g_settings */
#include "net.h"      /* media_state_t */
#include "lodepng.h"  /* lodepng_encode32_file (LVGL-bundled lodepng) */

/* ── LVGL host plumbing ─────────────────────────────────────────────────── */
static uint32_t tick_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

/* Dummy flush -- we never present to a real panel; snapshot reads the widget
 * tree directly. Just acknowledge so LVGL's refresh state machine advances. */
static void noop_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    (void)area; (void)px;
    lv_display_flush_ready(disp);
}

static int env_int(const char *name, int dflt)
{
    const char *v = getenv(name);
    return v && *v ? atoi(v) : dflt;
}

/* ── Canned Control4 now-playing state ──────────────────────────────────── */
static void fill_sample_state(media_state_t *st)
{
    memset(st, 0, sizeof(*st));
    int playing = env_int("MMK_PLAYING", 1);

    strncpy(st->room, "Kitchen", sizeof(st->room) - 1);
    st->power   = playing;
    st->playing = playing;
    strncpy(st->media_type, playing ? "media" : "", sizeof(st->media_type) - 1);

    strncpy(st->title,  "Redbone",              sizeof(st->title)  - 1);
    strncpy(st->artist, "Childish Gambino",     sizeof(st->artist) - 1);
    strncpy(st->album,  "\"Awaken, My Love!\"", sizeof(st->album)  - 1);
    st->art_url[0] = 0;  /* art stubbed -> placeholder */

    strncpy(st->source.id,   "spotify", sizeof(st->source.id)   - 1);
    strncpy(st->source.name, "Spotify", sizeof(st->source.name) - 1);

    st->volume   = 42;
    st->muted    = false;
    st->duration = 326;
    st->position = 97;

    st->n_meta = 2;
    strncpy(st->meta[0].id, "Bitrate", 23); strncpy(st->meta[0].name, "320 kbps", 47);
    strncpy(st->meta[1].id, "Codec",   23); strncpy(st->meta[1].name, "AAC",      47);

    st->show_title = st->show_artist = st->show_info = st->show_progress = true;
    st->can_pause = st->can_stop = st->can_next = st->can_prev = true;
    st->can_thumbs_up = st->can_thumbs_down = true;
    st->can_shuffle = st->can_repeat = true;
    st->shuffle_on = true;
    st->repeat_on  = false;

    /* Programmable keypad buttons (dealer-configured). Six by default because
     * that is what the real panels carry -- Playroom / Master Bedroom / Kitchen
     * each have 6 button bindings, the Office 8. MMK_NBTN trims/extends so the
     * merged-grid layouts can be checked at both counts. */
    struct { int id; const char *label; bool on; const char *color; const char *icon; } b[] = {
        {1, "Ceiling Lights", true,  "ffd166", "Lights"},
        {2, "Lamps",          true,  "ffd166", "Lights"},
        {3, "Ceiling Fan",    false, "4cc9f0", "Fan"},
        {4, "Good Night",     false, "b5179e", "Scene"},
        {5, "Shades",         false, "4cc9f0", "Shade"},
        {6, "Front Door",     false, "06d6a0", "Lock"},
        {7, "Exit Gate",      false, "06d6a0", "Door"},
        {8, "Garage Door",    false, "06d6a0", "Garage"},
    };
    int nb = env_int("MMK_NBTN", 6);
    if (nb < 0) nb = 0;
    if (nb > (int)(sizeof(b) / sizeof(b[0]))) nb = (int)(sizeof(b) / sizeof(b[0]));
    st->n_buttons = nb;
    for (int i = 0; i < st->n_buttons; i++) {
        st->buttons[i].id = b[i].id;
        strncpy(st->buttons[i].label, b[i].label, sizeof(st->buttons[i].label) - 1);
        st->buttons[i].on = b[i].on;
        strncpy(st->buttons[i].color, b[i].color, sizeof(st->buttons[i].color) - 1);
        strncpy(st->buttons[i].icon,  b[i].icon,  sizeof(st->buttons[i].icon)  - 1);
    }
}

/* ── ARGB8888 draw-buf -> RGBA PNG ──────────────────────────────────────── */
static int write_png(const char *path, const lv_draw_buf_t *snap)
{
    uint32_t w = snap->header.w, h = snap->header.h, stride = snap->header.stride;
    unsigned char *rgba = malloc((size_t)w * h * 4);
    if (!rgba) return -1;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *row = snap->data + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *p = row + (size_t)x * 4;   /* LVGL ARGB8888 mem order: B,G,R,A */
            unsigned char *o = rgba + ((size_t)y * w + x) * 4;
            o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = p[3];
        }
    }
    /* Encode to memory, then write the file ourselves -- the LVGL-bundled
     * lodepng's own file layer (LODEPNG_COMPILE_DISK) is disabled, so
     * lodepng_encode32_file() just returns error 79. */
    unsigned char *png = NULL; size_t pngsize = 0;
    unsigned err = lodepng_encode32(&png, &pngsize, rgba, w, h);
    free(rgba);
    if (err) { fprintf(stderr, "lodepng %u: %s\n", err, lodepng_error_text(err)); return -1; }
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "open %s: ", path); perror(NULL); free(png); return -1; }
    size_t n = fwrite(png, 1, pngsize, f);
    fclose(f);
    free(png);
    return (n == pngsize) ? 0 : -1;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <width> <height> <out.png>\n", argv[0]);
        return 2;
    }
    int W = atoi(argv[1]), H = atoi(argv[2]);
    const char *out = argv[3];
    if (W < 64 || H < 64 || W > 4096 || H > 4096) {
        fprintf(stderr, "bad size %dx%d\n", W, H);
        return 2;
    }

    lv_init();
    lv_tick_set_cb(tick_ms);

    lv_display_t *disp = lv_display_create(W, H);
    /* Full-frame ARGB8888 scratch buffer for the render pipeline. */
    static void *buf1;
    buf1 = malloc((size_t)W * H * 4);
    lv_display_set_buffers(disp, buf1, NULL, (uint32_t)((size_t)W * H * 4),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, noop_flush);

    /* Preview knobs -> settings, before the UI is built. */
    g_settings.theme       = (uint8_t)env_int("MMK_THEME",    g_settings.theme);
    g_settings.layout      = (uint8_t)env_int("MMK_LAYOUT",   g_settings.layout);
    g_settings.bg_preset   = (uint8_t)env_int("MMK_BG",       g_settings.bg_preset);
    g_settings.orientation = (uint8_t)env_int("MMK_ORIENT",   g_settings.orientation);
    /* MMK_ROT=0|1|2 pins the compact bar's rotating line (title/artist/album). */
    g_ui_rot_preview = env_int("MMK_ROT", -1);

    ui_begin();
    /* Sample favourites for the landing page's favourite ROWS. Set after ui_begin (the
     * UI must exist) but before the timer pump below, so the rebuild it requests is
     * serviced and the snapshot shows the rows -- setting them later renders an
     * out-of-date page. */
    {
        static favorite_t favs[] = {
            { .id = "ra.1", .title = "Mike DeLuca's Station", .kind = "stream", .art_url = "https://x/1.jpg" },
            { .id = "ra.2", .title = "Electronic Station",    .kind = "stream", .art_url = "https://x/2.jpg" },
            { .id = "ra.3", .title = "Chill Jazz",            .kind = "stream", .art_url = "https://x/3.jpg" },
            { .id = "ra.4", .title = "Classic Rock",          .kind = "stream" },
            { .id = "ra.5", .title = "Morning Coffee",        .kind = "stream" },
        };
        ui_set_favorites(favs, 5);   /* 2 action cards + 5 favourites = scrolls */
    }
    /* MMK_OFFLINE=1 previews the not-connected landing page (no driver link). */
    ui_set_connected(env_int("MMK_OFFLINE", 0) ? false : true);

    media_state_t st;
    fill_sample_state(&st);
    ui_set_state(&st);

    /* MMK_SETUP=1 previews the Wi-Fi setup overlay instead of the main UI. It
     * lives on the top layer (lv_layer_top), so snapshot that below. Sample
     * AP/pass mirror what wifi.c passes; pop=NULL = the S3's portal-only path. */
    const bool setup_scene = env_int("MMK_SETUP", 0) != 0;
    if (setup_scene)
        ui_show_setup("MKeypad-2E70", "kp7f3a9c21b8", NULL);

    /* Let deferred work + layout settle: pump timers over a little virtual time,
     * then force a full layout + refresh so every widget has final geometry. */
    for (int i = 0; i < 8; i++) {
        ui_tick_progress();
        lv_timer_handler();
    }
    /* Open the intercom scenes AFTER the settle pump: opening one hides the home
     * panel, and a rebuild queued by the pump (e.g. ic_available flipping) would
     * otherwise rebuild it hidden and leave now-playing showing instead. */
    /* MMK_ROOMS=1 previews the multiroom add/remove-rooms panel with a sample
     * house, so its layout can be checked without a live driver. */
    if (env_int("MMK_ROOMS", 0)) {
        static const room_t rooms[] = {
            { .name = "Office",        .grouped = true,  .playing = true,  .active = true  },
            { .name = "Kitchen",       .grouped = false, .playing = true,  .active = true  },
            { .name = "Living Room",   .grouped = true,  .playing = true,  .active = true  },
            { .name = "Master Bedroom",.grouped = false, .playing = false, .active = false },
            { .name = "Playroom",      .grouped = false, .playing = false, .active = false },
            { .name = "Garage",        .grouped = false, .playing = false, .active = false },
            { .name = "Patio",         .grouped = false, .playing = false, .active = false },
            { .name = "Dining Room",   .grouped = false, .playing = false, .active = false },
        };
        ui_set_rooms(rooms, (int)(sizeof(rooms) / sizeof(rooms[0])));
    }

    /* Intercom: a sample roster (doors, groups, rooms) so the picker and the call
     * screen can be reviewed without a driver or a SIP stack. */
    {
        static const intercom_target_t eps[] = {
            { .name = "Front Door",     .user = "door1",   .door = true,
              .n_actions = 2,
              .actions = { { .id = "relay1", .label = "Unlock" },
                           { .id = "relay2", .label = "Open Gate" } } },
            { .name = "Back Gate",      .user = "door2",   .door = true,
              .n_actions = 1,
              .actions = { { .id = "relay1", .label = "Open Gate" } } },
            { .name = "All Rooms",      .user = "all",     .group = true },
            { .name = "Upstairs",       .user = "up",      .group = true },
            { .name = "Kitchen",        .user = "kitchen" },
            { .name = "Office",         .user = "office"  },
            { .name = "Master Bedroom", .user = "mbr"     },
            { .name = "Playroom",       .user = "play"    },
        };
        ui_set_endpoints(eps, (int)(sizeof(eps) / sizeof(eps[0])));
    }
    if (env_int("MMK_ROOMS", 0)) ui_show_rooms_panel();
    if (env_int("MMK_IC", 0)) ui_show_intercom_panel();
    const char *call_ev = getenv("MMK_CALL");
    const bool call_scene = (call_ev && *call_ev);
    if (call_scene) {
        /* MMK_CALLPEER picks which roster entry is calling: Front Door has two
         * door actions, Back Gate one, a room none. */
        const char *pe = getenv("MMK_CALLPEER");
        ui_call(call_ev, (pe && *pe) ? pe : "Front Door");
    }
    for (int i = 0; i < 4; i++) lv_timer_handler();

    /* The call overlay lives on lv_layer_top() (like the setup screen), so it has
     * to be snapshotted there -- off the active screen it renders invisibly. */
    lv_obj_t *root = (setup_scene || call_scene) ? lv_layer_top() : lv_screen_active();
    lv_obj_update_layout(root);
    lv_refr_now(disp);

    lv_draw_buf_t *snap = lv_snapshot_take(root, LV_COLOR_FORMAT_ARGB8888);
    if (!snap) { fprintf(stderr, "snapshot failed\n"); return 1; }

    int rc = write_png(out, snap);
    lv_draw_buf_destroy(snap);
    if (rc) { fprintf(stderr, "png write failed: %s\n", out); return 1; }

    printf("wrote %s (%dx%d)\n", out, W, H);
    return 0;
}
