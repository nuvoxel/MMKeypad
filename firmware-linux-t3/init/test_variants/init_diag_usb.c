/* Isolated test of the USB-ACM gadget switch, run live under adb on the
 * still-stock Android (not as PID 1). Every single log line is opened,
 * written, and closed immediately (O_APPEND, no buffering) so that even if
 * this process is killed mid-sequence by its own USB reconfiguration, every
 * step up to that point is durably on disk -- no reliance on stdio
 * buffering/flushing surviving an abrupt death. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define LOGFILE "/data/local/tmp/usb_diag.log"

static void log_line(const char *msg) {
    int fd = open(LOGFILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0)
        return;
    write(fd, msg, strlen(msg));
    write(fd, "\n", 1);
    close(fd);
}

static void write_sysfs(const char *path, const char *value) {
    char buf[160];
    snprintf(buf, sizeof(buf), "about to write_sysfs(%s, %s)", path, value);
    log_line(buf);

    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        snprintf(buf, sizeof(buf), "  open failed errno=%d (%s)", errno, strerror(errno));
        log_line(buf);
        return;
    }
    ssize_t n = write(fd, value, strlen(value));
    int werrno = errno;
    close(fd);
    snprintf(buf, sizeof(buf), "  write n=%zd errno=%d (%s)", n, werrno, strerror(werrno));
    log_line(buf);
}

int main(void) {
    unlink(LOGFILE);
    log_line("=== usb diag test starting, pid follows ===");
    char pidbuf[32];
    snprintf(pidbuf, sizeof(pidbuf), "pid=%d", getpid());
    log_line(pidbuf);

    /* Stock values confirmed live via adb: idVendor=2207 (Rockchip's own,
     * not Google's 18d1), idProduct=0006, functions=adb. Only "functions"
     * needs to change for this test. */
    log_line("switching to acm...");
    write_sysfs("/sys/class/android_usb/android0/enable", "0");
    write_sysfs("/sys/class/android_usb/android0/functions", "acm");
    write_sysfs("/sys/class/android_usb/android0/enable", "1");
    log_line("acm switch sequence complete, waiting 8s before reverting...");

    sleep(8);

    log_line("reverting to adb...");
    write_sysfs("/sys/class/android_usb/android0/enable", "0");
    write_sysfs("/sys/class/android_usb/android0/functions", "adb");
    write_sysfs("/sys/class/android_usb/android0/enable", "1");
    log_line("=== reverted, done ===");

    return 0;
}
