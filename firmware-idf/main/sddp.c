#include "sddp.h"
#include "net.h"
#include "device.h"   // device_sku_id() -> the model suffix advertised below
#include <string.h>
#include <stdio.h>
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_log.h"

static const char *TAG = "sddp";

#define SDDP_GROUP   "239.255.255.250"
#define SDDP_PORT    1902
#define SDDP_MAX_AGE 1800
#define ANNOUNCE_MS  600000   // 10 min (< Max-Age)

static char     s_host[48];      // "<MODEL>-<hardware_id>", e.g. T3-10-75d45f4b3dba0e83c4d4dad0
static uint16_t s_ctrl_port = 0; // advertised control port

// Build an SDDP message into `out`. status != NULL => SEARCH reply; else NOTIFY.
static int build_msg(char *out, size_t n, const char *notify_verb, const char *status, const char *localip)
{
    int len = 0;
    if (status) {
        len += snprintf(out + len, n - len, "SDDP/1.0 %s\r\n", status);
    } else {
        len += snprintf(out + len, n - len, "%s SDDP/1.0\r\n", notify_verb);
    }
    len += snprintf(out + len, n - len, "From: \"%s:%u\"\r\n", localip, SDDP_PORT);
    len += snprintf(out + len, n - len, "Host: \"%s\"\r\n", s_host);
    len += snprintf(out + len, n - len, "Max-Age: %d\r\n", SDDP_MAX_AGE);
    len += snprintf(out + len, n - len, "Type: \"NuVoxel:keypad:M Keypad\"\r\n");
    /* These must name the proxies the driver ACTUALLY declares — Director exposes
     * them to drivers as primary_proxy/proxies via C4:GetDiscoveryInfo(). They went
     * stale when the driver moved to NuVoxelKeypad.c4z: the real proxy is
     * "keypad_proxy" (there is no proxy named "keypad" — an unresolvable name is
     * silently skipped by Director), and generic_http was dropped in favour of the
     * intercom sub-proxy. */
    len += snprintf(out + len, n - len, "Primary-Proxy: \"keypad_proxy\"\r\n");
    len += snprintf(out + len, n - len, "Proxies: \"keypad_proxy,intercomproxy\"\r\n");
    len += snprintf(out + len, n - len, "Manufacturer: \"NuVoxel\"\r\n");
    // Advertise the MAC explicitly. Host is "<variant>-<hardware_id>", and on a T3
    // the hardware id is the eFuse id, which does NOT contain the MAC -- so a driver
    // that only knows this panel by MAC (it has not managed a `hello` yet, or knew
    // it before the hwid existed) could never recognise the announcement. Two panels
    // sat unreachable for exactly that reason after a DHCP move: replying to every
    // SEARCH, ignored every time. Cheap to send, and it makes discovery work from
    // either identifier.
    {
        const char *m = device_mac();
        if (m && m[0]) len += snprintf(out + len, n - len, "MAC: \"%s\"\r\n", m);
    }
    /* Model is DISPLAY-ONLY -- it is what Composer lists in Discovered. The
     * "Type:" header above is what the driver matches on (driver.xml <type>), so
     * that one must never change; this one is free to identify the variant.
     *
     * Derived from the SKU so every board self-labels with no per-board table:
     * "mmk-t3-10" -> "M Keypad (T3-10)", "mmk-s3" -> "M Keypad (S3)". Without
     * this every unit showed a bare "M Keypad" and two panels on one project were
     * indistinguishable except by MAC. */
    {
        char tag[24];
        device_variant_tag(tag, sizeof(tag));
        if (tag[0])
            len += snprintf(out + len, n - len, "Model: \"M Keypad (%s)\"\r\n", tag);
        else
            len += snprintf(out + len, n - len, "Model: \"M Keypad\"\r\n");
    }
    /* Was "mediakeypad.c4i" — the retired MediaKeypad.c4z. Discovery itself matches
     * on "Type:" above, so this being stale did not break Composer's Discovered
     * list, but it pointed a double-click at a driver that no longer ships. */
    len += snprintf(out + len, n - len, "Driver: \"nuvoxelkeypad.c4i\"\r\n");
    len += snprintf(out + len, n - len, "Config-URL: \"http://%s/\"\r\n", localip);
    return len;
}

static bool current_ip(char *out, size_t n)
{
    esp_netif_t *netif = mmk_default_netif();
    esp_netif_ip_info_t ip;
    if (!netif || esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.ip.addr == 0) return false;
    esp_ip4addr_ntoa(&ip.ip, out, n);
    return true;
}

static void sddp_task(void *arg)
{
    char localip[16];
    while (!current_ip(localip, sizeof(localip))) vTaskDelay(pdMS_TO_TICKS(500));  // wait for DHCP

    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) { ESP_LOGE(TAG, "socket failed"); vTaskDelete(NULL); }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in la = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(SDDP_PORT),
    };
    if (bind(s, (struct sockaddr *)&la, sizeof(la)) < 0) {
        ESP_LOGE(TAG, "bind :%d failed", SDDP_PORT);
        close(s);
        vTaskDelete(NULL);
    }
    // Join the multicast group on the STA interface.
    struct ip_mreq mreq = {0};
    inet_aton(SDDP_GROUP, &mreq.imr_multiaddr);
    inet_aton(localip, &mreq.imr_interface);
    setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in grp = {
        .sin_family = AF_INET,
        .sin_port = htons(SDDP_PORT),
    };
    inet_aton(SDDP_GROUP, &grp.sin_addr);

    ESP_LOGI(TAG, "listening as %s (ip %s)", s_host, localip);

    char msg[700];
    // Startup burst: SDDP recommends announcing NOTIFY ALIVE a few times on boot
    // so a listening driver (re)connects promptly even if a multicast packet is
    // dropped -- otherwise the first now-playing update waits on the driver's own
    // reconnect interval. Then fall into the periodic re-announce below.
    for (int i = 0; i < 3; i++) {
        if (current_ip(localip, sizeof(localip))) {
            int len = build_msg(msg, sizeof(msg), "NOTIFY ALIVE", NULL, localip);
            sendto(s, msg, len, 0, (struct sockaddr *)&grp, sizeof(grp));
        }
        vTaskDelay(pdMS_TO_TICKS(700));
    }
    TickType_t last_announce = xTaskGetTickCount();
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        if (last_announce == 0 || (now - last_announce) > pdMS_TO_TICKS(ANNOUNCE_MS)) {
            last_announce = now;
            if (current_ip(localip, sizeof(localip))) {
                int len = build_msg(msg, sizeof(msg), "NOTIFY ALIVE", NULL, localip);
                sendto(s, msg, len, 0, (struct sockaddr *)&grp, sizeof(grp));
            }
        }

        struct sockaddr_in ra;
        socklen_t rl = sizeof(ra);
        char buf[700];
        int n = recvfrom(s, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&ra, &rl);
        if (n <= 0) continue;
        buf[n] = 0;
        if (strncmp(buf, "SEARCH", 6) == 0) {
            // SECURITY (review fix): reply ONLY to the actual UDP sender, never to
            // an attacker-supplied "Host:" header (that was a reflection primitive).
            if (current_ip(localip, sizeof(localip))) {
                int len = build_msg(msg, sizeof(msg), NULL, "200 OK", localip);
                sendto(s, msg, len, 0, (struct sockaddr *)&ra, rl);
                char rip[16];
                esp_ip4addr_ntoa((esp_ip4_addr_t *)&ra.sin_addr, rip, sizeof(rip));
                ESP_LOGI(TAG, "replied to SEARCH from %s:%u", rip, ntohs(ra.sin_port));
            }
        }
    }
}

void sddp_start(uint16_t control_port)
{
    s_ctrl_port = control_port;
    /* Host is the unique instance id the C4 keypad driver matches its device on,
     * and Composer renders the Discovered "Address" column as Type + "-" + Host.
     *
     * Use the device's stable hardware_id -- the SAME id the device reports in its
     * manifest -- NOT a network-interface
     * MAC. The interface MAC is the wrong key on multi-transport hardware:
     *   - P4 (WS43/PoE): WiFi runs on the ESP32-C6 co-processor, so the netif MAC is
     *     the C6's; on Ethernet it is the P4 EMAC's -- neither equals hardware_id.
     *   - T3 (RK3188): the netif MAC is the panel's eth/WiFi MAC; hardware_id is a
     *     provisioned eFuse-SHA id.
     * It also CHANGES depending on whether the unit is cabled vs wireless, so a
     * MAC-keyed announce would break discovery when the transport changes. This is
     * why WS43/T3 showed "Not bound": the driver searched SDDP for hardware_id and
     * never matched the announced interface MAC. Announcing hardware_id makes
     * discovery work on either transport. On the S3, hardware_id IS the WiFi MAC, so
     * the announced Address is unchanged there.
     *
     *     T3-10-<hardware_id>  ->  NuVoxel:keypad:M Keypad-T3-10-<hardware_id>
     *
     * Falls back to a plain "Keypad-<id>" when the model tag is unknown. */
    {
        char tag[24];
        device_variant_tag(tag, sizeof(tag));
        const char *hwid = device_hardware_id();
        snprintf(s_host, sizeof(s_host), "%s-%s",
                 tag[0] ? tag : "Keypad", (hwid && hwid[0]) ? hwid : "unknown");
    }
    xTaskCreate(sddp_task, "sddp", 4096, NULL, 4, NULL);
}
