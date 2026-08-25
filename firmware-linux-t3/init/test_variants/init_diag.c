/* Diagnostic-only variant of init.c: does NOT touch the USB gadget, does NOT
 * become PID 1 (just a normal test process), writes to stdout/stderr instead
 * of /dev/console. Purpose: isolate "does our init logic (mount/fork/exec/
 * wait loop) work at all on this kernel" from "does the USB-ACM gadget
 * switch work" -- run live under adb with output visible the whole time. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void run(char *const argv[], const char *label) {
    pid_t pid = fork();
    if (pid == 0) {
        execv(argv[0], argv);
        fprintf(stderr, "[diag] execv(%s) failed: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    printf("[diag] %s: forked pid=%d\n", label, pid);
    if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
        printf("[diag] %s: child exited, status=%d\n", label, status);
    }
}

int main(void) {
    printf("[diag] alive, testing mounts...\n");
    fflush(stdout);

    int r = mount("proc", "/data/local/tmp/diag_proc", "proc", 0, NULL);
    printf("[diag] mount proc-like test: r=%d errno=%d (%s)\n", r, errno, strerror(errno));

    printf("[diag] testing fork/exec of busybox echo (expected to fail, no busybox here)...\n");
    fflush(stdout);
    char *argv1[] = {"/data/local/tmp/busybox", "echo", "hi-from-busybox", NULL};
    run(argv1, "busybox-echo-test");

    printf("[diag] testing sysfs read (safe, read-only)...\n");
    fflush(stdout);
    int fd = open("/sys/class/android_usb/android0/functions", O_RDONLY);
    if (fd >= 0) {
        char buf[64] = {0};
        int n = read(fd, buf, sizeof(buf) - 1);
        printf("[diag] read functions sysfs: n=%d content=%s\n", n, buf);
        close(fd);
    } else {
        printf("[diag] open functions sysfs failed: errno=%d (%s)\n", errno, strerror(errno));
    }

    printf("[diag] done, exiting cleanly.\n");
    return 0;
}
