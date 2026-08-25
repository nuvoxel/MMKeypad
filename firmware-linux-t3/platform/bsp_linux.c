/* Linux display + bsp for the T3 (RK3188).
 *
 * We drive the framebuffer ourselves because LVGL's lv_linux_fbdev only
 * software-rotates in PARTIAL mode, and that partial rotated-area write
 * corrupts this panel on tap-time redraws. Instead: LVGL renders the whole
 * 1280x800 landscape frame in DIRECT mode (single persistent buffer) and hands
 * us the dirty area; we rotate just that area 90 deg into the 800x1280 portrait
 * fb -- correct offsets (no corruption) and small writes (a progress tick
 * doesn't rewrite the whole panel). Touch is mapped in main_linux.c (display
 * rotation is 0; we do the rotation). */
#include "bsp.h"

#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define BACKLIGHT_PATH "/sys/class/backlight/rk28_bl/brightness"
/* Writing 0 to rk28_bl/brightness does NOT turn the panel off -- the driver
 * treats 0 as out-of-range and drives the PWM to full scale (verified on the
 * 7" and 10": brightness=0 -> actual_brightness=255). So a "dim to off"
 * screensaver level came back as MAXIMUM brightness. Real off is the fbdev
 * blank path: bl_power = FB_BLANK_POWERDOWN. Anything above 0 goes through
 * brightness with bl_power forced back to UNBLANK. */
#define BL_POWER_PATH "/sys/class/backlight/rk28_bl/bl_power"
#define BL_UNBLANK 0
#define BL_POWERDOWN 4

/* Fallback geometry if the fb driver reports nothing usable (the 7"/10" panels
 * are 800x1280 portrait; all T3 sizes share this fb — see the identical stock
 * kernel across sizes). Real values come from FBIOGET_VSCREENINFO at runtime. */
#define FB_W_FALLBACK 800
#define FB_H_FALLBACK 1280

lv_display_t *g_bsp_display = NULL;

/* Logical LANDSCAPE dimensions the UI renders at, derived from the physical fb.
 * Exposed so the touch transform in main_linux.c scales to the real panel. */
int g_bsp_lw = FB_H_FALLBACK;   /* logical landscape width  */
int g_bsp_lh = FB_W_FALLBACK;   /* logical landscape height */
/* Whether the flush rotates the render 90° into a PORTRAIT fb (the 7": fb is
 * 800x1280, panel wants landscape) or blits DIRECTLY into a LANDSCAPE fb (the
 * 10": fb is already 1280x800 — rotating it is the 90°-off bug). Published so
 * the touch transform matches. Decided at runtime from the real fb aspect. */
int g_bsp_rotate = 1;

static uint16_t *s_fb;      /* mmap'd framebuffer, RGB565 */
static int s_fbh;           /* physical fb height (rows), used by the rotated path */
static int s_lw;            /* logical landscape width = render-buffer row stride */
static int s_rotate = 1;    /* mirror of g_bsp_rotate for the flush hot path */
static int s_pxstride, s_fd = -1;

/* Wait for the panel's vertical blank before writing, so a redraw isn't caught
 * mid-scanout (tearing/flicker). This driver ignores FBIOPAN page-flipping, but
 * may still support vsync-wait; harmless no-op if not. */
#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC 0x40044620u
#endif

/* 4x4 ordered (Bayer) dither, 0..15 -- spreads the 8->5/6/5-bit quantization
 * error so darkened-art gradients stay smooth instead of banding/edge-detecting. */
static const uint8_t BAYER[4][4] = {
    { 0, 8, 2, 10}, {12, 4, 14, 6}, { 3, 11, 1, 9}, {15, 7, 13, 5}};

static inline uint16_t rgb565_dither(uint32_t argb, int dx, int dy) {
    int r = (argb >> 16) & 0xFF, g = (argb >> 8) & 0xFF, b = argb & 0xFF;
    int t = BAYER[dy & 3][dx & 3];
    r += t >> 1; if (r > 255) r = 255;   /* +0..7 into the 8-level R gap */
    b += t >> 1; if (b > 255) b = 255;
    g += t >> 2; if (g > 255) g = 255;   /* +0..3 into the 4-level G gap */
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* Rotate the CHANGED region of the 32-bit landscape frame into the RGB565
 * portrait fb, dithering on the way down: logical(lx,ly) -> fb(x=ly, y=fbh-1-lx). */
/* Block until the panel's vblank. Called ONCE per frame from the main loop,
 * OUTSIDE the LVGL lock -- so the render cycle is paced to vblank (tear
 * reduction) without stalling touch input or waiting per dirty area. Returns
 * nonzero if the driver actually supports the wait. */
int bsp_wait_vsync(void) {
    if (s_fd < 0) return 0;
    unsigned vs = 0;
    return ioctl(s_fd, FBIO_WAITFORVSYNC, &vs) == 0;
}

static void direct_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    const uint32_t *src = (const uint32_t *)px_map;   /* ARGB8888, stride s_lw */
    if (s_rotate) {
        /* PORTRAIT fb (7"): rotate the landscape render 90° into it.
         * fb x = logical y; fb y = fbh-1 - logical x. */
        for (int ly = area->y1; ly <= area->y2; ly++) {
            const uint32_t *srow = src + (size_t)ly * s_lw;
            const int fx = ly;
            for (int lx = area->x1; lx <= area->x2; lx++) {
                const int fy = s_fbh - 1 - lx;
                s_fb[(size_t)fy * s_pxstride + fx] = rgb565_dither(srow[lx], fx, fy);
            }
        }
    } else {
        /* LANDSCAPE fb (10"): the render already matches the panel — blit 1:1,
         * no rotation (rotating here is exactly the 90°-off bug). */
        for (int ly = area->y1; ly <= area->y2; ly++) {
            const uint32_t *srow = src + (size_t)ly * s_lw;
            uint16_t *drow = s_fb + (size_t)ly * s_pxstride;
            for (int lx = area->x1; lx <= area->x2; lx++)
                drow[lx] = rgb565_dither(srow[lx], lx, ly);
        }
    }
    lv_display_flush_ready(disp);
}

lv_display_t *bsp_display_create(void) {
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) { perror("open /dev/fb0"); return NULL; }
    s_fd = fd;   /* kept open for FBIO_WAITFORVSYNC in the flush */
    struct fb_var_screeninfo vi;
    struct fb_fix_screeninfo fi;
    memset(&vi, 0, sizeof vi);
    memset(&fi, 0, sizeof fi);
    ioctl(fd, FBIOGET_VSCREENINFO, &vi);
    ioctl(fd, FBIOGET_FSCREENINFO, &fi);

    /* Real fb geometry from the driver; fall back only if it reports junk. This
     * is what makes one build work across every T3 panel (7"/10"/…): we read the
     * actual resolution AND aspect instead of assuming the 7"'s 800x1280. */
    int fb_w = (vi.xres > 0) ? (int)vi.xres : FB_W_FALLBACK;
    int fb_h = (vi.yres > 0) ? (int)vi.yres : FB_H_FALLBACK;
    s_fbh = fb_h;
    /* line_length is bytes/row; /2 => RGB565 pixels/row. Fall back to fb_w. */
    s_pxstride = (fi.line_length > 0) ? (int)(fi.line_length / 2) : fb_w;

    int lw, lh;
    if (fb_h > fb_w) {
        /* PORTRAIT fb (7", 800x1280): rotate 90° -> landscape lw=fb_h, lh=fb_w. */
        s_rotate = 1;
        lw = fb_h;
        lh = fb_w;
    } else {
        /* LANDSCAPE fb (10", 1280x800): already landscape, no rotation. */
        s_rotate = 0;
        lw = fb_w;
        lh = fb_h;
    }
    s_lw = lw;                       /* render-buffer row stride */
    g_bsp_lw = lw;                   /* publish for the touch transform */
    g_bsp_lh = lh;
    g_bsp_rotate = s_rotate;
    size_t smem = (fi.smem_len > 0) ? fi.smem_len : (size_t)s_pxstride * fb_h * 2;
    printf("bsp: fb %dx%d (%s), stride %d px -> logical %dx%d landscape\n",
           fb_w, fb_h, s_rotate ? "portrait/rotated" : "landscape/direct",
           s_pxstride, lw, lh);

    s_fb = mmap(NULL, smem, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (s_fb == MAP_FAILED) { perror("mmap fb"); close(fd); return NULL; }

    lv_display_t *disp = lv_display_create(s_lw, lh);
    static uint8_t *buf;
    size_t bytes = (size_t)s_lw * lh * 4;   /* ARGB8888 render buffer */
    buf = malloc(bytes);
    lv_display_set_buffers(disp, buf, NULL, bytes, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, direct_flush);
    g_bsp_display = disp;
    return disp;
}

static void bl_write(const char *path, int val) {
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return;
    char buf[8];
    int n = snprintf(buf, sizeof(buf), "%d", val);
    write(fd, buf, n);
    close(fd);
}

void bsp_set_backlight(uint8_t pct) {
    if (pct > 100)
        pct = 100;
    if (pct == 0) {
        /* Blank first, then park brightness at 1 (not 0 -- see BL_POWER_PATH):
         * if anything re-unblanks the panel behind our back it comes back at
         * the dimmest step rather than full white. */
        bl_write(BL_POWER_PATH, BL_POWERDOWN);
        bl_write(BACKLIGHT_PATH, 1);
        return;
    }
    int val = pct * 255 / 100;
    if (val < 1)
        val = 1;
    bl_write(BACKLIGHT_PATH, val);
    bl_write(BL_POWER_PATH, BL_UNBLANK);
}

void bsp_apply_orientation(uint8_t orient) {
    (void)orient; /* fixed landscape via direct_flush rotation */
}
