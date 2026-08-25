/* Framebuffer bring-up test for the T3 (RK3188), Phase 2.
 * Opens /dev/fb0, reads real geometry, mmaps it, and paints vertical color
 * bars (red, green, blue, white, black) so we can eyeball on the physical
 * panel that (a) the framebuffer works and (b) the RGB565 pixel byte order
 * is what we think. Prints the geometry it found. Also nudges the backlight
 * to full so a blank-but-working panel doesn't look dead.
 *
 * Panel is 800x1280 portrait, 16bpp RGB565 (stride 1600).
 */
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

int main(void) {
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        perror("open /dev/fb0");
        return 1;
    }

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        perror("FBIOGET_VSCREENINFO");
        return 1;
    }
    if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        perror("FBIOGET_FSCREENINFO");
        return 1;
    }

    printf("res=%ux%u virtual=%ux%u bpp=%u\n", vinfo.xres, vinfo.yres,
           vinfo.xres_virtual, vinfo.yres_virtual, vinfo.bits_per_pixel);
    printf("line_length=%u smem_len=%u\n", finfo.line_length, finfo.smem_len);
    printf("R off=%u len=%u  G off=%u len=%u  B off=%u len=%u\n",
           vinfo.red.offset, vinfo.red.length, vinfo.green.offset,
           vinfo.green.length, vinfo.blue.offset, vinfo.blue.length);

    if (vinfo.bits_per_pixel != 16) {
        printf("WARNING: expected 16bpp, got %u -- test assumes RGB565\n",
               vinfo.bits_per_pixel);
    }

    size_t maplen = finfo.line_length * vinfo.yres_virtual;
    uint8_t *fb = mmap(NULL, maplen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    /* 5 vertical color bars over the visible area. */
    uint16_t bars[5] = {
        rgb565(255, 0, 0),   /* red */
        rgb565(0, 255, 0),   /* green */
        rgb565(0, 0, 255),   /* blue */
        rgb565(255, 255, 255),/* white */
        rgb565(0, 0, 0),     /* black */
    };
    unsigned barw = vinfo.xres / 5;
    for (unsigned y = 0; y < vinfo.yres; y++) {
        uint16_t *row = (uint16_t *)(fb + (size_t)y * finfo.line_length);
        for (unsigned x = 0; x < vinfo.xres; x++) {
            unsigned b = x / barw;
            if (b > 4)
                b = 4;
            row[x] = bars[b];
        }
    }

    printf("painted %u color bars (R G B W K)\n", 5u);
    munmap(fb, maplen);
    close(fd);

    /* backlight to full */
    int bf = open("/sys/class/backlight/rk28_bl/brightness", O_WRONLY);
    if (bf >= 0) {
        write(bf, "255", 3);
        close(bf);
        printf("backlight set to 255\n");
    }
    return 0;
}
