/* Real implementations for the esp_* shims that aren't header-inline:
 * device MAC (from eth0), restart, and app version. */
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_system.h"

#include <linux/reboot.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type) {
    (void)type; /* one NIC on the T3 -- always eth0 */
    FILE *f = fopen("/sys/class/net/eth0/address", "r");
    if (!f)
        return ESP_FAIL;
    unsigned v[6] = {0};
    int n = fscanf(f, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
    fclose(f);
    if (n != 6)
        return ESP_FAIL;
    for (int i = 0; i < 6; i++)
        mac[i] = (uint8_t)v[i];
    return ESP_OK;
}

void esp_restart(void) {
    sync();
    syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
            LINUX_REBOOT_CMD_RESTART, 0);
    for (;;) {
    } /* not reached */
}

const esp_app_desc_t *esp_app_get_description(void) {
    static esp_app_desc_t d;
    if (d.version[0] == 0) {
        /* MMK_FW_VERSION is injected by the Makefile from firmware-idf/version.txt,
         * It must match the published release version (the on-screen updater
         * compares versions), so a hardcoded placeholder here would misreport
         * what the panel is running. */
#ifdef MMK_FW_VERSION
        strncpy(d.version, MMK_FW_VERSION, sizeof(d.version) - 1);
#else
        strncpy(d.version, "t3-linux-0.1", sizeof(d.version) - 1);
#endif
        strncpy(d.project_name, "mmkeypad", sizeof(d.project_name) - 1);
    }
    return &d;
}

/* Album-art HTTP fetch is now real (platform/httpc_linux.c, mbedtls) and JPEG
 * decode is real (platform/jpeg_linux.c, tjpgd) -- no stubs here anymore. */

/* ui.c's ui_splash() embeds splash.png via these ESP linker symbols. We use
 * our own NuVoxel splash (show_splash in main) and never call ui_splash(), but
 * the symbols must resolve. Provide empty ones. */
#include <stdint.h>
const uint8_t _mmk_splash_dummy_start[1] __asm__("_binary_splash_png_start") = {0};
const uint8_t _mmk_splash_dummy_end[1] __asm__("_binary_splash_png_end") = {0};

/* --- esp_netif (SDDP needs the device IP) --- */
#include "esp_netif.h"
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

static struct esp_netif_obj { int dummy; } g_eth_netif;

esp_netif_t *esp_netif_get_handle_from_ifkey(const char *ifkey) {
    /* one NIC (eth0) on the T3 -- return a non-NULL handle for the ETH key */
    (void)ifkey;
    return (esp_netif_t *)&g_eth_netif;
}

esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *out) {
    (void)netif;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return ESP_FAIL;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, "eth0", IFNAMSIZ - 1);
    int r = ioctl(s, SIOCGIFADDR, &ifr);
    close(s);
    if (r < 0)
        return ESP_FAIL;
    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
    out->ip.addr = sin->sin_addr.s_addr;
    out->netmask.addr = 0;
    out->gw.addr = 0;
    return ESP_OK;
}

char *esp_ip4addr_ntoa(const esp_ip4_addr_t *addr, char *dst, uint8_t size) {
    struct in_addr in;
    in.s_addr = addr->addr;
    const char *s = inet_ntoa(in);
    strncpy(dst, s, size - 1);
    dst[size - 1] = 0;
    return dst;
}
