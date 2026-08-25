/* reboot2 <reason> -- reboot with a RESTART2 reason string, so our custom
 * Linux can drop into Rockchip Loader (rockusb) mode on its own:
 *   reboot2 loader
 * This is exactly what Android's `adb reboot loader` does under the hood
 * (RESTART2 with arg "loader"); the RK3188 kernel's machine_restart handler
 * reads the string and sets the maskrom/loader boot flag. Lets us reflash
 * over the network with zero physical button presses.
 *
 * musl's reboot() wrapper only takes an int, so call the raw syscall to pass
 * the reason string.
 */
#include <linux/reboot.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(int argc, char **argv) {
    const char *reason = argc > 1 ? argv[1] : "loader";
    sync();
    long r = syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
                     LINUX_REBOOT_CMD_RESTART2, reason);
    /* If we get here the syscall returned (failure) -- normally it never does. */
    fprintf(stderr, "reboot2(%s) returned %ld\n", reason, r);
    return 1;
}
