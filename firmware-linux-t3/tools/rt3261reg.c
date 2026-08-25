/* Read/write RT3261 codec registers directly over I2C (bus 4, addr 0x1c).
 * The codec uses an 8-bit register index and a 16-bit big-endian value.
 * Used to replicate the stock capture register state, which DAPM under our
 * Linux does not reproduce (opening a capture stream changes no registers). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#define ADDR 0x1c

static int rd(int fd, unsigned char reg, unsigned *val)
{
    unsigned char buf[2];
    struct i2c_msg m[2] = {
        { .addr=ADDR, .flags=0,        .len=1, .buf=&reg },
        { .addr=ADDR, .flags=I2C_M_RD, .len=2, .buf=buf  },
    };
    struct i2c_rdwr_ioctl_data d = { .msgs=m, .nmsgs=2 };
    if (ioctl(fd, I2C_RDWR, &d) < 0) return -1;
    *val = (buf[0]<<8)|buf[1];
    return 0;
}
static int wr(int fd, unsigned char reg, unsigned val)
{
    unsigned char buf[3] = { reg, (val>>8)&0xff, val&0xff };
    struct i2c_msg m = { .addr=ADDR, .flags=0, .len=3, .buf=buf };
    struct i2c_rdwr_ioctl_data d = { .msgs=&m, .nmsgs=1 };
    return ioctl(fd, I2C_RDWR, &d) < 0 ? -1 : 0;
}

int main(int argc, char **argv)
{
    int fd = open("/dev/i2c-4", O_RDWR);
    if (fd < 0) { perror("open /dev/i2c-4"); return 1; }
    if (argc >= 3 && !strcmp(argv[1], "w")) {
        /* w <reg> <val> [<reg> <val> ...] */
        for (int i = 2; i + 1 < argc; i += 2) {
            unsigned r = strtoul(argv[i], 0, 16), v = strtoul(argv[i+1], 0, 16), back = 0;
            if (wr(fd, r, v) < 0) { printf("  w %02x=%04x FAILED (%s)\n", r, v, strerror(errno)); continue; }
            rd(fd, r, &back);
            printf("  w %02x=%04x -> reads %04x %s\n", r, v, back, back==v ? "ok" : "<< MISMATCH");
        }
    } else if (argc >= 2 && !strcmp(argv[1], "r")) {
        for (int i = 2; i < argc; i++) {
            unsigned r = strtoul(argv[i], 0, 16), v = 0;
            if (rd(fd, r, &v) == 0) printf("  %02x: %04x\n", r, v);
        }
    } else {
        printf("usage: %s r <reg>...   |   %s w <reg> <val> ...\n", argv[0], argv[0]);
    }
    close(fd);
    return 0;
}
