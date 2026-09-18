#pragma once
#include "lvgl.h"
#include "lodepng.h"
#include <stdlib.h>
#include <string.h>

// Album-art canvas. Fetch + JPEG decode runs on a pinned background task; tick()
// publishes the decoded image to the LVGL canvas (call on the LVGL task).
void art_begin(lv_obj_t *parent, int x, int y, int w, int h, bool fit);
void art_set_bg(uint32_t top, uint32_t bot, int screen_h);  // screen gradient for icon/letterbox blend
void art_load(const char *url);   // queue a URL (non-blocking; "" clears)
void art_tick(void);              // publish decoded art (LVGL task)
void art_clear(void);             // blank it
void art_detach(void);            // drop the canvas ref before a UI rebuild deletes it

// Mirror sinks: extra small canvases that show a cover-cropped copy of the
// primary album art (home mini-player tile, Listen card). Create after art_begin;
// they auto-update whenever the primary art changes. Returns the canvas (NULL if
// the pool is full or the alloc failed). art_detach() frees them.
lv_obj_t *art_add_mirror(lv_obj_t *parent, int x, int y, int w, int h);
bool art_has(void);               // true when real album art (not a placeholder) is shown

// ── Favourite-tile thumbnails ───────────────────────────────────────────────
// Small independent images (the favourites grid), fetched + decoded by the same
// background task as the album art, one at a time, so they never race it. Each
// tile owns its buffer. Call art_thumb_clear() before rebuilding the grid.
lv_obj_t *art_thumb_add(lv_obj_t *parent, int x, int y, int w, int h, const char *url);
void art_thumb_tick(void);    // publish decoded thumbnails (LVGL task)
void art_thumb_clear(void);   // drop all tiles + free their buffers

// Decode a PNG to a tightly packed RGBA8888 buffer the caller free()s. Every PNG
// decode in the app goes through this because WHICH lodepng answers
// lodepng_decode32() depends on the build: the ESP boards link the vendored
// decoder (PSRAM allocators in art.c), which hands back raw pixels, but any build with
// LV_USE_LODEPNG (the T3 and the desktop sim) links LVGL's patched copy instead,
// whose "pixels" are really an lv_draw_buf_t *. Reading that struct as RGBA ran
// off the end of a ~60-byte allocation: an intermittent SIGBUS on the setup
// screen in the sim, and the same read on a T3 for every PNG source icon.
// Header-only so ui.c can use it in the sim, which stubs art.c out entirely.
static inline unsigned char *art_png_decode_rgba(const uint8_t *png, size_t len, unsigned *w, unsigned *h)
{
    unsigned char *out = NULL;
    unsigned err = lodepng_decode32(&out, w, h, png, len);
#if LV_USE_LODEPNG
    lv_draw_buf_t *db = (lv_draw_buf_t *)out;
    unsigned char *rgba = NULL;
    if (!err && db) {
        size_t sz = (size_t)*w * *h * 4;
        rgba = malloc(sz);
        if (rgba) memcpy(rgba, db->data, sz);   // created with stride 4*w: already packed
    }
    if (db) lv_draw_buf_destroy(db);
    return rgba;
#else
    if (err && out) { free(out); out = NULL; }
    return out;
#endif
}
