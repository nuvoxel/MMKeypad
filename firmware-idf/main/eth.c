// MMKeypad — Ethernet bring-up for the ESP32-P4-POE-ETH-NH (IP101 over RMII).
// The P4 has an internal EMAC; this board wires it to an IP101 PHY and feeds the
// 50 MHz RMII reference clock IN from the PHY side. ESP-IDF's P4
// ETH_ESP32_EMAC_DEFAULT_CONFIG() already encodes this board's exact RMII pinout
// (MDC=31 MDIO=52 CLK_EXT_IN=50 TX_EN=49 TXD0=34 TXD1=35 CRS_DV=28 RXD0=29 RXD1=30),
// so we use it verbatim and only set the board-specific PHY reset GPIO + address.

#include "eth.h"
#include "board.h"

#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_phy.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "eth";

static EventGroupHandle_t s_eg;
#define BIT_GOT_IP BIT0
static bool s_up;

static void on_eth_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    switch (id) {
    case ETHERNET_EVENT_CONNECTED:    ESP_LOGI(TAG, "link up"); break;
    case ETHERNET_EVENT_DISCONNECTED: ESP_LOGW(TAG, "link down"); s_up = false; break;
    case ETHERNET_EVENT_START:        ESP_LOGI(TAG, "started"); break;
    case ETHERNET_EVENT_STOP:         ESP_LOGI(TAG, "stopped"); break;
    default: break;
    }
}

static void on_ip_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;
    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
    s_up = true;
    ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
    xEventGroupSetBits(s_eg, BIT_GOT_IP);
}

void eth_start(void)
{
    s_eg = xEventGroupCreate();

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&netif_cfg);

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t esp32_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();  // P4 defaults == this board
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&esp32_cfg, &mac_cfg);

    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = ETH_PHY_ADDR;            // 1
    phy_cfg.reset_gpio_num = ETH_PHY_RST_GPIO;  // 51
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_cfg);

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t handle = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_cfg, &handle));

    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(handle)));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_evt, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_ip_evt, NULL));

    ESP_ERROR_CHECK(esp_eth_start(handle));

    ESP_LOGI(TAG, "waiting for DHCP (IP101 @ phy %d, reset gpio %d)...", ETH_PHY_ADDR, ETH_PHY_RST_GPIO);
    EventBits_t b = xEventGroupWaitBits(s_eg, BIT_GOT_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(20000));
    if (!(b & BIT_GOT_IP)) ESP_LOGW(TAG, "no IP yet; continuing (will come up async on link)");
}

bool eth_is_up(void) { return s_up; }
