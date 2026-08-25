#pragma once
#include "lvgl.h"

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
