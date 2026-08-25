#include "art.h"
#include "net.h"     // net_art_debug: thumbnail outcome telemetry
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "jpeg_decoder.h"
#include "lodepng.h"

static const char *TAG = "art";

// lodepng allocators -> PSRAM (LODEPNG_NO_COMPILE_ALLOCATORS set for lodepng.c);
// a decoded icon would otherwise blow the WiFi-constrained internal heap.
void *lodepng_malloc(size_t size) { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM); }
void *lodepng_realloc(void *ptr, size_t newsize) { return heap_caps_realloc(ptr, newsize, MALLOC_CAP_SPIRAM); }
void lodepng_free(void *ptr) { heap_caps_free(ptr); }

#define ART_BG       0x000000   // letterbox/placeholder background (icons sit on black)
#define JPEG_CAP     (384 * 1024)   // max downloaded image size (SSRF/DoS cap)

static lv_obj_t *s_canvas;
static uint16_t *s_buf;     // canvas-visible buffer (w*h)
static uint16_t *s_work;    // decoded+scaled, swapped in by tick()
static uint8_t  *s_jpeg;    // downloaded JPEG bytes

static int  s_w, s_h;
static bool s_fit;
static char s_req_url[600];
static char s_last_url[600];

static volatile bool s_req;
static volatile bool s_ready;
static volatile bool s_busy;
static volatile bool s_have_art;   // true once a real image (not just gradient) is shown

// Mirror sinks: extra small canvases (home mini-player tile, Listen card) that
// show a cover-cropped copy of the primary album art. They sample the primary
// canvas buffer (s_buf) so they stay in lockstep with the now-playing art, and
// need no second fetch/decode. All mirror ops run on the LVGL task.
#define ART_MAX_MIRROR 3
static struct { lv_obj_t *canvas; uint16_t *buf; int w, h; } s_mir[ART_MAX_MIRROR];
static int s_nmir;

static void fill565(uint16_t *p, int n, uint32_t rgb)
{
    // Native LVGL RGB565 (little-endian). esp_lvgl_port's swap_bytes handles the
    // panel byte order at flush, same as for the rest of the UI — don't pre-swap.
    uint16_t c = (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
    for (int i = 0; i < n; i++) p[i] = c;
}

// ── background gradient ──────────────────────────────────────────────────────
// So transparent C4 icons and letterbox margins blend into the screen's vertical
// gradient instead of a black/white box, we reproduce that gradient per row at this
// canvas's screen-Y. Colors + screen height come from art_set_bg(); y0 from art_begin.
static uint32_t s_bg_top = 0x000000, s_bg_bot = 0x000000;
static int      s_bg_y0 = 0, s_bg_h = 240;   // placeholder until art_set_bg()

static inline uint16_t to565(uint32_t rgb)
{
    return (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
}

static uint32_t bg_at(int cy)
{
    int span = (s_bg_h > 1) ? (s_bg_h - 1) : 1;
    int y = s_bg_y0 + cy;
    if (y < 0) y = 0; else if (y > span) y = span;
    int tr = (s_bg_top >> 16) & 0xFF, tg = (s_bg_top >> 8) & 0xFF, tb = s_bg_top & 0xFF;
    int br = (s_bg_bot >> 16) & 0xFF, bg = (s_bg_bot >> 8) & 0xFF, bb = s_bg_bot & 0xFF;
    int r = tr + (br - tr) * y / span;
    int g = tg + (bg - tg) * y / span;
    int b = tb + (bb - tb) * y / span;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

// ── decode target ───────────────────────────────────────────────────────────
// The decode/scale path used to write straight into s_work at s_w x s_h. It now
// writes into the SELECTED target, so the same fetch + JPEG/PNG path can also fill
// a favourite tile. All decoding happens on art_task (a single thread), so a plain
// set-target-before-decode is safe.
static uint16_t *s_dst; static int s_dstw, s_dsth; static bool s_dstfit;

static void fill_bg_into(uint16_t *buf, int w, int h)
{
    for (int cy = 0; cy < h; cy++) {
        uint16_t c = to565(bg_at(cy));
        uint16_t *row = &buf[cy * w];
        for (int cx = 0; cx < w; cx++) row[cx] = c;
    }
}
static void fill_bg(uint16_t *buf) { fill_bg_into(buf, s_w, s_h); }

// Cover-fill (or letterbox if fit) the decoded image (dw x dh) into s_work (s_w x s_h).
static void scale_into_work(const uint16_t *dec, int dw, int dh, bool fit)
{
    uint16_t *out = s_dst;  const int ow = s_dstw, oh = s_dsth;
    if (!out || ow <= 0 || oh <= 0) return;
    if (dw <= 0 || dh <= 0) { fill_bg_into(out, ow, oh); return; }
    float rx = (float)dw / ow, ry = (float)dh / oh;
    float r = fit ? (rx > ry ? rx : ry)      // fit: whole image, letterbox
                  : (rx < ry ? rx : ry);     // cover: fill, crop overflow
    int useW = (int)(ow * r + 0.5f), useH = (int)(oh * r + 0.5f);
    // Do NOT clamp to the source size: for letterbox (fit) useW/useH may exceed the
    // source, so the out-of-range samples below fall to the gradient (the margins that
    // keep an icon at its real aspect ratio). Cover stays within the source either way.
    int ox = (dw - useW) / 2, oy = (dh - useH) / 2;
    for (int cy = 0; cy < oh; cy++) {
        for (int cx = 0; cx < ow; cx++) {
            int sxv = ox + (int)((long)cx * useW / ow);
            int syv = oy + (int)((long)cy * useH / oh);
            if (sxv < 0 || syv < 0 || sxv >= dw || syv >= dh) {
                out[cy * ow + cx] = to565(bg_at(cy));   // letterbox margin -> gradient
            } else {
                out[cy * ow + cx] = dec[syv * dw + sxv];
            }
        }
    }
}

// HTTPS only. Artwork URLs arrive over the :6700 link, which is unauthenticated,
// so they are attacker-settable: plain http meant the panel could be aimed at any
// host on the network and its response fed straight into the JPEG decoder. Every
// real source (the Control4 CDN, our own catalogue) is https.
static bool scheme_ok(const char *url)
{
    return strncmp(url, "https://", 8) == 0;
}

static int fetch(const char *url)   // returns bytes read, or -1
{
    if (!scheme_ok(url)) { ESP_LOGW(TAG, "blocked non-http(s) url"); return -1; }
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 8000,
        // Validate https against the CA bundle. (mbedtls allocates from PSRAM —
        // CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC — so the TLS handshake doesn't compete with the
        // LVGL draw buffers for internal RAM, which was starving it: SSL_ALLOC_FAILED.)
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return -1;
    int total = -1;
    if (esp_http_client_open(cli, 0) == ESP_OK) {
        esp_http_client_fetch_headers(cli);
        total = 0;
        int r;
        while ((r = esp_http_client_read(cli, (char *)s_jpeg + total, JPEG_CAP - total)) > 0) {
            total += r;
            if (total >= JPEG_CAP) { ESP_LOGW(TAG, "art too large, capped"); break; }
        }
        esp_http_client_close(cli);
    }
    esp_http_client_cleanup(cli);
    return total > 0 ? total : -1;
}

// tjpgd decodes out of a fixed work pool. esp_jpeg's built-in default is only
// JPEG_WORK_BUF_SIZE = 3100 bytes -- tjpgd's bare minimum -- and real album art from
// the media services overflows it, failing with JDR_MEM1 (3) "Insufficient memory pool
// for the image" and no art on screen. Hand it a generous PSRAM pool instead; this is
// the pool size, NOT the image buffer, so it costs one small allocation for the life
// of the app.
#define ART_JPEG_WORK_SIZE (32 * 1024)
static uint8_t *s_jwork;

static bool decode_jpeg(int jlen)
{
    if (!s_jwork) s_jwork = heap_caps_malloc(ART_JPEG_WORK_SIZE, MALLOC_CAP_SPIRAM);
    esp_jpeg_image_cfg_t cfg = {
        .indata = s_jpeg,
        .indata_size = jlen,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags = { .swap_color_bytes = 0 },   // native RGB565; flush handles byte order
        .advanced = { .working_buffer = s_jwork,
                      .working_buffer_size = s_jwork ? ART_JPEG_WORK_SIZE : 0 },
    };
    esp_jpeg_image_output_t info;
    if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK) return false;

    // Pick a downscale so the decoded image still covers the canvas but stays small.
    int dw = info.width, dh = info.height;
    esp_jpeg_image_scale_t scales[4] = { JPEG_IMAGE_SCALE_0, JPEG_IMAGE_SCALE_1_2,
                                         JPEG_IMAGE_SCALE_1_4, JPEG_IMAGE_SCALE_1_8 };
    int chosen = 0;
    for (int s = 3; s >= 0; s--) {
        int w = info.width >> s, h = info.height >> s;
        if (w >= s_w && h >= s_h) { chosen = s; dw = w; dh = h; break; }
    }
    cfg.out_scale = scales[chosen];

    size_t need = (size_t)dw * dh * 2;
    uint16_t *dec = heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
    if (!dec) return false;
    cfg.outbuf = (uint8_t *)dec;
    cfg.outbuf_size = need;
    bool ok = (esp_jpeg_decode(&cfg, &info) == ESP_OK);
    if (ok) scale_into_work(dec, info.width, info.height, s_fit);   // album art: cover
    heap_caps_free(dec);
    return ok;
}

// Letterbox-scale an RGBA icon into s_work, alpha-compositing every pixel (and the
// margins) over the screen gradient so transparent areas blend in — no black/white box.
static void scale_icon_into_work(const unsigned char *rgba, int dw, int dh)
{
    uint16_t *out = s_dst;  const int ow = s_dstw, oh = s_dsth;
    if (!out || ow <= 0 || oh <= 0) return;
    if (dw <= 0 || dh <= 0) { fill_bg_into(out, ow, oh); return; }
    float rx = (float)dw / ow, ry = (float)dh / oh;
    float r = (rx > ry ? rx : ry);                    // fit: whole icon, letterboxed
    int useW = (int)(ow * r + 0.5f), useH = (int)(oh * r + 0.5f);
    int ox = (dw - useW) / 2, oy = (dh - useH) / 2;
    for (int cy = 0; cy < oh; cy++) {
        uint32_t bgc = bg_at(cy);
        int bgr = (bgc >> 16) & 0xFF, bgg = (bgc >> 8) & 0xFF, bgb = bgc & 0xFF;
        uint16_t bg565 = to565(bgc);
        for (int cx = 0; cx < ow; cx++) {
            int sx = ox + (int)((long)cx * useW / ow);
            int sy = oy + (int)((long)cy * useH / oh);
            if (sx < 0 || sy < 0 || sx >= dw || sy >= dh) { out[cy * ow + cx] = bg565; continue; }
            const unsigned char *p = &rgba[((size_t)sy * dw + sx) * 4];
            unsigned a = p[3];
            if (a == 0)   { out[cy * ow + cx] = bg565; continue; }
            if (a == 255) { out[cy * ow + cx] = to565(((uint32_t)p[0] << 16) | (p[1] << 8) | p[2]); continue; }
            int rr = (p[0] * a + bgr * (255 - a)) / 255;   // src over gradient
            int gg = (p[1] * a + bgg * (255 - a)) / 255;
            int bb = (p[2] * a + bgb * (255 - a)) / 255;
            out[cy * ow + cx] = to565(((uint32_t)rr << 16) | ((uint32_t)gg << 8) | (uint32_t)bb);
        }
    }
}

// PNG source-tile icons (Control4 sends these for no-art sources). lodepng uses our
// PSRAM allocators (below). Decoded WITH alpha and composited over the screen gradient.
static bool decode_png(int len)
{
    unsigned char *rgba = NULL;
    unsigned w = 0, h = 0;
    if (lodepng_decode32(&rgba, &w, &h, s_jpeg, (size_t)len) != 0 || !rgba) return false;
    scale_icon_into_work(rgba, (int)w, (int)h);
    lodepng_free(rgba);
    return true;
}

static bool decode_and_scale(int len)
{
    if (len >= 8 && s_jpeg[0] == 0x89 && s_jpeg[1] == 'P' && s_jpeg[2] == 'N' && s_jpeg[3] == 'G') {
        return decode_png(len);
    }
    if (len >= 2 && s_jpeg[0] == 0xFF && s_jpeg[1] == 0xD8) {
        return decode_jpeg(len);
    }
    return false;   // unknown format
}

// ── favourite-tile thumbnails ───────────────────────────────────────────────
// The favourites grid needs N small images, but this module is a single-primary
// pipeline (one URL, one canvas). Rather than a second fetch/decode stack -- which
// would race the primary over s_jpeg and the tjpgd pool -- thumbnails are a queue
// serviced by THIS task, one at a time, reusing the same fetch + decode path via
// the decode target above. Each tile owns its canvas buffer; the primary art is
// untouched.
#define ART_MAX_THUMB 12
// TH_BUSY = the decode task has CLAIMED this slot and owns its buffer. Ownership
// is what makes the teardown safe: exactly one side may free a buffer, and which
// side that is gets decided by an atomic state swap rather than by timing.
enum { TH_FREE = 0, TH_QUEUED, TH_BUSY, TH_READY, TH_DONE };
static struct {
    lv_obj_t *canvas; uint16_t *buf; int w, h;
    char url[600]; volatile uint8_t st; bool ok;
} s_th[ART_MAX_THUMB];
static volatile int s_nth;

// Decode one queued thumbnail (art_task context). Returns true if it did work.
// True while thumb_step() is decoding INTO a tile buffer. art_thumb_clear() must not
// free those buffers underneath it: a fetch+decode takes seconds, and clearing s_nth
// alone does not stop a decode already in flight -- it has already captured the buffer
// pointer. Freeing it there corrupts the heap (seen once as a TLSF Store access fault).
static volatile bool s_thBusy;
// Bumped by every art_thumb_clear(). A decode in flight captures it and refuses to
// publish its result if it changed underneath -- see thumb_step().
static volatile uint32_t s_thEpoch;

// Decode SCRATCH. The decoder used to write straight into the buffer that is
// attached to a live on-screen canvas, so a fetch+decode -- seconds of work --
// mutated pixels the UI could be drawing at that very moment. Any redraw in that
// window (a scroll, a state push, the progress tick invalidating a region) painted
// a half-written image: the "favourite artwork looks corrupted" report. Decoding
// into private scratch and having the LVGL TASK copy the finished image across
// means the on-screen buffer only ever goes from one complete image to another.
// One buffer is enough: only one decode runs at a time (single art task) and
// thumb_step will not start another until the tick has consumed the last one.
static uint16_t *s_thScratch;
static size_t    s_thScratchPx;

// Thumbnail telemetry is RECORDED here by the decode task and SENT by the UI task.
// Sending it from the decode task overflowed that task's 6 KB stack -- it already
// runs the JPEG decode, and cJSON serialisation plus the socket write on top of
// that panicked the panel the moment the driver connected and the first favourite
// started decoding. Anything on the art task has to stay allocation- and
// serialisation-free.
typedef struct { int slot, bytes, w, h; bool ok, used; } th_report_t;
static volatile th_report_t s_thReports[ART_MAX_THUMB];

static void thumb_report(int slot, bool ok, int bytes, int w, int h)
{
    for (int i = 0; i < ART_MAX_THUMB; i++) {
        if (s_thReports[i].used) continue;
        s_thReports[i].slot = slot; s_thReports[i].ok = ok;
        s_thReports[i].bytes = bytes; s_thReports[i].w = w; s_thReports[i].h = h;
        __sync_synchronize();
        s_thReports[i].used = true;
        return;
    }
}

static bool scratch_ensure(size_t px)
{
    if (s_thScratch && s_thScratchPx >= px) return true;
    uint16_t *n = heap_caps_realloc(s_thScratch, px * 2, MALLOC_CAP_SPIRAM);
    if (!n) return false;
    s_thScratch = n; s_thScratchPx = px;
    return true;
}

static bool thumb_step(void)
{
    // A finished image is still sitting in scratch until art_thumb_tick() copies it
    // out; starting another decode now would overwrite it.
    for (int i = 0; i < s_nth; i++) if (s_th[i].st == TH_READY) return false;

    for (int i = 0; i < s_nth; i++) {
        if (s_th[i].st != TH_QUEUED) continue;
        // CLAIM the slot. If art_thumb_clear() gets there first the swap fails and
        // it owns the buffer instead -- no window where both sides think they do.
        if (!__sync_bool_compare_and_swap(&s_th[i].st, TH_QUEUED, TH_BUSY)) continue;
        const uint32_t ep = s_thEpoch;   // this decode belongs to THIS grid
        // Work from LOCAL copies: once claimed, the slot may be recycled underneath
        // us by the next grid's art_thumb_add(), and s_th[i].buf would then be a
        // different tile's buffer.
        uint16_t *mybuf = s_th[i].buf;
        const int mw = s_th[i].w, mh = s_th[i].h;
        char myurl[sizeof(s_th[i].url)];
        strncpy(myurl, s_th[i].url, sizeof(myurl) - 1); myurl[sizeof(myurl) - 1] = 0;
        s_thBusy = true;
        if (!scratch_ensure((size_t)mw * mh)) {
            ESP_LOGW(TAG, "thumb %d: no scratch; skipping", i);
            __sync_bool_compare_and_swap(&s_th[i].st, TH_BUSY, TH_DONE);
            s_thBusy = false;
            return true;
        }
        // Decode into SCRATCH, never into the live canvas buffer.
        s_dst = s_thScratch; s_dstw = mw; s_dsth = mh;
        int jlen = fetch(myurl);
        bool ok = (jlen >= 0) && decode_and_scale(jlen);
        if (!ok) fill_bg_into(s_thScratch, mw, mh);
        ESP_LOGI(TAG, "thumb %d %s '%.44s' (%d bytes)", i, ok ? "OK" : "FAILED", myurl, jlen);
        // Reported below, AFTER the publish decision -- see the epoch check. Saying
        // "OK" here (as this first did) meant "decoded", which is not the same as
        // "displayed": a tile whose grid was rebuilt mid-decode logs a perfectly
        // healthy OK and then shows nothing.
        // Restore the primary target so a following album-art decode is unaffected.
        s_dst = s_work; s_dstw = s_w; s_dsth = s_h;
        // Do NOT publish into a slot that has been recycled underneath us. When
        // art_thumb_clear() gives up waiting (decode still busy) it returns early
        // WITHOUT resetting the slots, and the next grid's art_thumb_add() starts
        // again at index 0 -- so this completion would land on a DIFFERENT
        // favourite's slot, flipping it to READY before its own decode ran. The
        // tick then published that tile (showing the placeholder or the wrong
        // image) and marked it DONE, so its real artwork never appeared. That is
        // the "thumbnails corrupt or vanish" case.
        if (ep != s_thEpoch) {
            // The grid was torn down mid-decode. art_thumb_clear() saw TH_BUSY and
            // deliberately left this buffer alone, so freeing it is OUR job -- that
            // is the ownership handoff that replaces the old "leak it and hope".
            ESP_LOGW(TAG, "thumb %d abandoned (grid rebuilt mid-decode)", i);
            thumb_report(i, false, jlen, mw, mh);   // decoded, but never displayed
            heap_caps_free(mybuf);
            // Release the slot only if it has not already been recycled: if the next
            // grid re-queued it, the swap fails and its entry is left untouched.
            __sync_bool_compare_and_swap(&s_th[i].st, TH_BUSY, TH_FREE);
            s_thBusy = false;
            return true;
        }
        s_th[i].ok = ok;
        __sync_synchronize();
        s_th[i].st = TH_READY;
        thumb_report(i, ok, jlen, mw, mh);          // queued for the UI to publish
        s_thBusy = false;
        return true;
    }
    return false;
}

static void art_task(void *arg)
{
    for (;;) {
        if (s_req && !s_ready) {
            s_req = false;
            s_busy = true;
            char url[600];
            strncpy(url, s_req_url, sizeof(url) - 1);
            url[sizeof(url) - 1] = 0;
            // Retry on failure. Right after a reboot-while-playing the driver re-pushes
            // the SAME artUrl (so art_load() dedups it), but the network/TLS may not be
            // ready for the first fetch — without a retry that transient miss would leave
            // a permanent placeholder until the track changes. Bail early if a newer URL
            // arrives (track change) so we don't keep hammering a stale one.
            bool ok = false;
            int jlen = -1;
            s_dst = s_work; s_dstw = s_w; s_dsth = s_h;   // decode into the primary canvas
            for (int attempt = 0; attempt < 6 && !s_req; attempt++) {
                jlen = fetch(url);
                if (jlen >= 0 && decode_and_scale(jlen)) { ok = true; break; }
                vTaskDelay(pdMS_TO_TICKS(1500));
            }
            ESP_LOGI(TAG, "art %s '%.48s' (%d bytes)", ok ? "OK" : "FAILED", url, jlen);
            if (!ok) fill_bg(s_work);
            s_have_art = ok;        // read by art_tick (LVGL task) to toggle placeholders
            __sync_synchronize();   // publish s_work before s_ready (other core)
            s_ready = true;
            s_busy = false;
        }
        // Album art always wins; thumbnails fill the idle time.
        if (!s_req && !s_busy) thumb_step();
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

// Cover-sample the primary art (s_buf, s_w x s_h) into mirror i (mw x mh): scale
// so the image fills the tile and crop the overflow, centered. Handles any aspect
// (e.g. a near-square now-playing card into a square mini tile).
static void render_mirror(int i)
{
    if (!s_buf || s_w <= 0 || s_h <= 0 || !s_mir[i].buf) return;
    uint16_t *dst = s_mir[i].buf;
    const int mw = s_mir[i].w, mh = s_mir[i].h;
    float rx = (float)s_w / mw, ry = (float)s_h / mh;
    float r = rx < ry ? rx : ry;               // cover: fill, crop overflow
    int useW = (int)(mw * r + 0.5f), useH = (int)(mh * r + 0.5f);
    int ox = (s_w - useW) / 2, oy = (s_h - useH) / 2;
    for (int cy = 0; cy < mh; cy++) {
        int sy = oy + (int)((long)cy * useH / mh);
        if (sy < 0) sy = 0; else if (sy >= s_h) sy = s_h - 1;
        const uint16_t *srow = &s_buf[sy * s_w];
        uint16_t *drow = &dst[cy * mw];
        for (int cx = 0; cx < mw; cx++) {
            int sx = ox + (int)((long)cx * useW / mw);
            if (sx < 0) sx = 0; else if (sx >= s_w) sx = s_w - 1;
            drow[cx] = srow[sx];
        }
    }
}

static void render_mirrors(void)
{
    for (int i = 0; i < s_nmir; i++) {
        render_mirror(i);
        if (s_mir[i].canvas) lv_obj_invalidate(s_mir[i].canvas);
    }
}

lv_obj_t *art_add_mirror(lv_obj_t *parent, int x, int y, int w, int h)
{
    if (s_nmir >= ART_MAX_MIRROR || w <= 0 || h <= 0) return NULL;
    uint16_t *buf = heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_SPIRAM);
    if (!buf) return NULL;
    lv_obj_t *cv = lv_canvas_create(parent);
    lv_canvas_set_buffer(cv, buf, w, h, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(cv, x, y);
    int i = s_nmir++;
    s_mir[i].canvas = cv; s_mir[i].buf = buf; s_mir[i].w = w; s_mir[i].h = h;
    render_mirror(i);   // show the current art (or gradient placeholder) right away
    return cv;
}

bool art_has(void) { return s_have_art; }

void art_begin(lv_obj_t *parent, int x, int y, int w, int h, bool fit)
{
    // Wait out any in-flight decode before changing geometry (rebuild).
    s_req = false;
    for (int i = 0; i < 250 && s_busy; i++) vTaskDelay(pdMS_TO_TICKS(2));

    s_w = w; s_h = h; s_fit = fit;
    s_bg_y0 = y;   // canvas's screen-Y, so bg_at() matches the screen gradient row-for-row
    s_last_url[0] = 0; s_req = false; s_ready = false;
    // Buffers are sized by the CALLER's canvas geometry — the screen layout
    // drives w/h (500x500 on the 7" 1024x600 panel vs 220x220 on the S3), so a
    // fixed size is a heap-corrupting overflow on larger panels (the 0x3ab3
    // blue-gradient tlsf store-faults on crowpanel7). Grown on rebuild if needed.
    static size_t cap = 0;
    size_t need = (size_t)w * (size_t)h * 2;
    if (need > cap) {
        heap_caps_free(s_buf);  s_buf  = NULL;
        heap_caps_free(s_work); s_work = NULL;
        cap = 0;
    }
    if (!s_buf)  s_buf  = heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
    if (!s_work) s_work = heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
    if (!s_jpeg) s_jpeg = heap_caps_malloc(JPEG_CAP, MALLOC_CAP_SPIRAM);
    if (!s_buf || !s_work || !s_jpeg) {
        ESP_LOGE("art", "buffer alloc failed (%dx%d) — album art disabled", w, h);
        s_canvas = NULL;
        return;
    }
    cap = need;
    fill_bg(s_buf);

    s_canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(s_canvas, s_buf, w, h, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_canvas, x, y);
    // Rounded corners when the art is a card (not full-bleed cover art). Clip the
    // canvas draw to the rounded rect so the album cover has soft corners.
    {
        int sw = lv_display_get_horizontal_resolution(NULL);
        int sh = lv_display_get_vertical_resolution(NULL);
        if (w < sw && h < sh) {
            lv_obj_set_style_radius(s_canvas, w / 14, 0);
            lv_obj_set_style_clip_corner(s_canvas, true, 0);
        }
    }

    static bool started = false;
    if (!started) {
        // NO_AFFINITY (not pinned to core 0): the album-art HTTPS fetch does a
        // CPU-heavy TLS handshake, and core 0 is saturated by the WiFi + esp_rtc
        // `_sip_task` (prio ~5) once the intercom registers. Pinned to core 0 at
        // prio 3 the fetch got preempted and timed out, so art took ~45 s (6 retries
        // x 8 s timeout). Let the scheduler run it on the idle core; small prio bump.
        xTaskCreatePinnedToCore(art_task, "art", 6144, NULL, 4, NULL, tskNO_AFFINITY);
        started = true;
    }
}

void art_load(const char *url)
{
    if (!url) url = "";
    if (strcmp(url, s_last_url) == 0) return;   // no change
    strncpy(s_last_url, url, sizeof(s_last_url) - 1);
    s_last_url[sizeof(s_last_url) - 1] = 0;
    if (url[0] == 0) { art_clear(); return; }
    strncpy(s_req_url, url, sizeof(s_req_url) - 1);
    s_req_url[sizeof(s_req_url) - 1] = 0;
    __sync_synchronize();
    s_req = true;
}

void art_tick(void)
{
    if (s_ready) {
        __sync_synchronize();
        if (s_buf && s_work) memcpy(s_buf, s_work, s_w * s_h * 2);
        if (s_canvas) lv_obj_invalidate(s_canvas);
        render_mirrors();   // push the new art into the mini-player/Listen tiles
        s_ready = false;
    }
}

void art_clear(void)
{
    s_have_art = false;
    if (s_buf) fill_bg(s_buf);
    if (s_canvas) lv_obj_invalidate(s_canvas);
    render_mirrors();   // blank the mini-player/Listen tiles too
}

// Tell the art module the screen's vertical gradient (top/bottom colors + full screen
// height) so transparent icons/margins blend into it. Call before art_begin().
void art_set_bg(uint32_t top, uint32_t bot, int screen_h)
{
    s_bg_top = top; s_bg_bot = bot; s_bg_h = (screen_h > 1) ? screen_h : 240;
}

void art_detach(void)
{
    s_canvas = NULL;
    // Drop the mirror canvases + buffers before the screen rebuild deletes their
    // parents. Safe here: doRebuild() calls this synchronously right before
    // lv_obj_clean(), with no redraw in between, so no canvas is drawn from a
    // freed buffer. (The primary s_buf is retained/reused, same as before.)
    for (int i = 0; i < s_nmir; i++) {
        heap_caps_free(s_mir[i].buf);
        s_mir[i].buf = NULL; s_mir[i].canvas = NULL;
    }
    s_nmir = 0;
}


// ── favourite-tile thumbnail API ────────────────────────────────────────────
// The canvas is going away: release its pixels and forget the slot that pointed at
// it. Keyed off the OBJECT, not a slot index -- indices are recycled by the next
// grid, so an index captured here would clear the wrong entry.
static void thumb_canvas_deleted(lv_event_t *e)
{
    lv_obj_t *cv = lv_event_get_target(e);
    void *buf = lv_event_get_user_data(e);
    for (int i = 0; i < ART_MAX_THUMB; i++) {
        if (s_th[i].canvas == cv) {
            s_th[i].canvas = NULL;
            s_th[i].buf = NULL;      // freed below; nothing may use it again
            s_th[i].st = TH_FREE;
        }
    }
    heap_caps_free(buf);
}

lv_obj_t *art_thumb_add(lv_obj_t *parent, int x, int y, int w, int h, const char *url)
{
    if (s_nth >= ART_MAX_THUMB || !url || !url[0] || w <= 0 || h <= 0) return NULL;
    if (!scheme_ok(url)) return NULL;
    int i = s_nth;
    s_th[i].buf = heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_SPIRAM);
    if (!s_th[i].buf) return NULL;
    s_th[i].w = w; s_th[i].h = h;
    fill_bg_into(s_th[i].buf, w, h);          // gradient until the fetch lands
    s_th[i].canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(s_th[i].canvas, s_th[i].buf, w, h, LV_COLOR_FORMAT_RGB565);
    // The CANVAS owns its pixels. Every previous attempt had this module freeing the
    // buffer on its own schedule and trying to keep the LVGL object in step -- which
    // is how two tiles ended up sharing one block (freed here, re-malloc'd there,
    // both canvases still live), and how deleting the object instead left a tile
    // blank. Hanging the free off the object's own delete event makes the two
    // lifetimes the same thing by construction: whoever destroys the canvas -- the
    // parent being cleaned, or us -- releases the pixels exactly once.
    lv_obj_add_event_cb(s_th[i].canvas, thumb_canvas_deleted, LV_EVENT_DELETE, s_th[i].buf);
    lv_obj_set_pos(s_th[i].canvas, x, y);
    lv_obj_set_style_radius(s_th[i].canvas, w / 8, 0);
    lv_obj_set_style_clip_corner(s_th[i].canvas, true, 0);
    strncpy(s_th[i].url, url, sizeof(s_th[i].url) - 1);
    s_th[i].url[sizeof(s_th[i].url) - 1] = 0;
    s_th[i].ok = false;
    __sync_synchronize();
    s_th[i].st = TH_QUEUED;
    s_nth = i + 1;
    return s_th[i].canvas;
}

// Runs on the LVGL task: publish a finished decode by copying it into the canvas
// buffer here, where nothing else can be drawing it, then invalidate.
void art_thumb_tick(void)
{
    // Drain any telemetry the decode task recorded. Sent from HERE because this runs
    // on the LVGL task, which has the stack for it and already talks to the driver.
    for (int i = 0; i < ART_MAX_THUMB; i++) {
        if (!s_thReports[i].used) continue;
        net_art_debug(s_thReports[i].slot, s_thReports[i].ok, s_thReports[i].bytes,
                      s_thReports[i].w, s_thReports[i].h);
        s_thReports[i].used = false;
    }

    for (int i = 0; i < s_nth; i++) {
        if (s_th[i].st != TH_READY) continue;
        if (s_th[i].buf && s_thScratch) {
            size_t px = (size_t)s_th[i].w * s_th[i].h;
            if (px <= s_thScratchPx) memcpy(s_th[i].buf, s_thScratch, px * 2);
        }
        s_th[i].st = TH_DONE;
        if (s_th[i].canvas) lv_obj_invalidate(s_th[i].canvas);
    }
}

void art_thumb_clear(void)
{
    // Stop tracking the current grid. Nothing is freed and nothing is deleted here:
    // each tile's pixels belong to its canvas and are released by that object's
    // delete handler when the grid is torn down (lv_obj_clean on the parent). This
    // used to free the buffers itself -- which raced the still-live canvases -- and
    // then, briefly, delete the canvases too, which blanked tiles whose grid was not
    // actually being rebuilt.
    //
    // The epoch bump still matters: a decode already in flight must not publish into
    // a grid that has gone away. It decodes into scratch, never into a tile, so
    // there is nothing of its own left to clean up.
    s_nth = 0;
    s_thEpoch++;
    __sync_synchronize();
}
