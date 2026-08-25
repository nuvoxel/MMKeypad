#pragma once
#include <stdint.h>
#include "esp_err.h"

typedef struct esp_netif_obj esp_netif_t;
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { esp_ip4_addr_t ip, netmask, gw; } esp_netif_ip_info_t;

/* Non-NULL sentinel so callers proceed; the real IP comes from eth0. */
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *ifkey);
esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *out);
char *esp_ip4addr_ntoa(const esp_ip4_addr_t *addr, char *dst, uint8_t size);
