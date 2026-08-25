#include <unistd.h>
#include <sys/syscall.h>
#include <linux/reboot.h>
int main(void){
    sync();
    /* RK bootrom re-enters the Loader when the kernel reboots with cmd "loader" */
    syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
            LINUX_REBOOT_CMD_RESTART2, "loader");
    return 0;
}
