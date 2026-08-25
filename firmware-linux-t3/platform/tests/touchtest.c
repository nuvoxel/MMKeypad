/* Touch bring-up test for the T3 (RK3188), Phase 2.
 *
 * IMPORTANT: musl uses 64-bit time_t even on 32-bit ARM, so musl's
 * `struct input_event` is LARGER than what this stock 3.0.36 kernel writes
 * (which uses a 32-bit timeval -> 16-byte records). Reading into musl's
 * struct and checking `n == sizeof(struct input_event)` drops every event.
 * So we define the kernel's exact 16-byte layout by hand. (Phase 3's LVGL
 * touch backend must do the same.)
 */
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

/* Kernel 3.0.36 (32-bit time) input_event ABI, 16 bytes. */
struct kevent {
    uint32_t tv_sec;
    uint32_t tv_usec;
    uint16_t type;
    uint16_t code;
    int32_t value;
};

#define EV_KEY 0x01
#define EV_ABS 0x03
#define BTN_TOUCH 0x14a
#define ABS_X 0x00
#define ABS_Y 0x01
#define ABS_MT_TOUCH_MAJOR 0x30
#define ABS_MT_POSITION_X 0x35
#define ABS_MT_POSITION_Y 0x36
#define ABS_MT_TRACKING_ID 0x39

int main(void) {
    int fd = open("/dev/input/event1", O_RDONLY);
    if (fd < 0) {
        perror("open /dev/input/event1");
        return 1;
    }
    printf("reading touch (16-byte kevent) for 20s -- tap/drag now...\n");
    fflush(stdout);

    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    time_t start = time(NULL);
    long count = 0;
    while (time(NULL) - start < 20) {
        int pr = poll(&pfd, 1, 1000);
        if (pr <= 0)
            continue;
        struct kevent ev;
        ssize_t n = read(fd, &ev, sizeof(ev));
        if (n != (ssize_t)sizeof(ev))
            continue;
        count++;
        if (ev.type == EV_ABS) {
            const char *nm = "ABS?";
            switch (ev.code) {
                case ABS_X: nm = "ABS_X"; break;
                case ABS_Y: nm = "ABS_Y"; break;
                case ABS_MT_POSITION_X: nm = "MT_X"; break;
                case ABS_MT_POSITION_Y: nm = "MT_Y"; break;
                case ABS_MT_TRACKING_ID: nm = "MT_ID"; break;
                case ABS_MT_TOUCH_MAJOR: nm = "MT_MAJOR"; break;
                default: break;
            }
            printf("EV_ABS %-8s (0x%02x) = %d\n", nm, ev.code, ev.value);
            fflush(stdout);
        } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            printf("BTN_TOUCH = %d\n", ev.value);
            fflush(stdout);
        }
    }
    printf("done. total events read: %ld\n", count);
    close(fd);
    return 0;
}
