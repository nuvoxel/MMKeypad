// MMKeypad — Ethernet bring-up for the ESP32-P4-POE-ETH-NH (IP101 over RMII).
// The P4 has an internal EMAC; this board wires it to an IP101 PHY and feeds the
// 50 MHz RMII reference clock IN from the PHY side. ESP-IDF's P4
// ETH_ESP32_EMAC_DEFAULT_CONFIG() already encodes this board's exact RMII pinout
// (MDC=31 MDIO=52 CLK_EXT_IN=50 TX_EN=49 TXD0=34 TXD1=35 CRS_DV=28 RXD0=29 RXD1=30),
// so we use it verbatim and only set the board-specific PHY reset GPIO + address.

#include <inttypes.h>
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


// ── IP101 Rx-to-Tx loopback guard ───────────────────────────────────────────
// The IP101G/GA datasheet requires an external pull-up on INTR for normal mode:
//
//   "an external pulled-up resistor is needed for normal mode operation. Another
//    operation mode is Rx to Tx loopback debugging test (reflect on Register
//    P1R23[13] RX2TX_LPBK) when connect INTR pin to GND."
//
// Absent that pull-up the part can come out of reset with RX2TX_LPBK SET, and it
// then reflects every received frame back out the wire. A reflecting PHY makes a
// managed switch see its own spanning-tree frames return and block the port, so
// the symptom is "link is perfect, no traffic passes".
//
// Clear it during bring-up, before autonegotiation completes, so the PHY is never
// linked while reflecting. The bit latches from the pin at every reset, so this
// must run on each boot — it is not a one-time fix.
//
// Register 20[4:0] is the page select for regs 16-31 (default 0x10 = page 16).
#define IP101_PAGE_SEL_REG   20
#define IP101_PAGE_DEFAULT   0x10
#define IP101_P1R23          23
#define IP101_RX2TX_LPBK_BIT (1u << 13)

static void ip101_clear_rx2tx_loopback(esp_eth_handle_t h)
{
    uint32_t page = 1, v = 0, restore = IP101_PAGE_DEFAULT;
    esp_eth_phy_reg_rw_data_t sel = { .reg_addr = IP101_PAGE_SEL_REG, .reg_value_p = &page };
    esp_eth_phy_reg_rw_data_t r23 = { .reg_addr = IP101_P1R23,       .reg_value_p = &v };
    esp_eth_phy_reg_rw_data_t rst = { .reg_addr = IP101_PAGE_SEL_REG, .reg_value_p = &restore };

    if (esp_eth_ioctl(h, ETH_CMD_WRITE_PHY_REG, &sel) != ESP_OK) return;
    if (esp_eth_ioctl(h, ETH_CMD_READ_PHY_REG,  &r23) != ESP_OK) goto out;

    if (v & IP101_RX2TX_LPBK_BIT) {
        uint32_t nv = v & ~IP101_RX2TX_LPBK_BIT;
        esp_eth_phy_reg_rw_data_t w = { .reg_addr = IP101_P1R23, .reg_value_p = &nv };
        esp_eth_ioctl(h, ETH_CMD_WRITE_PHY_REG, &w);
        esp_eth_ioctl(h, ETH_CMD_READ_PHY_REG, &r23);
        ESP_LOGW(TAG, "IP101 came up in Rx->Tx loopback (P1R23=0x%04" PRIx32 "); cleared -> 0x%04" PRIx32,
                 v | IP101_RX2TX_LPBK_BIT, v);
    } else {
        ESP_LOGI(TAG, "IP101 RX2TX_LPBK already clear (P1R23=0x%04" PRIx32 ")", v);
    }
out:
    esp_eth_ioctl(h, ETH_CMD_WRITE_PHY_REG, &rst);
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
    // NOT fatal: the same image runs on a bare ws43 with no carrier, where the
    // PHY does not answer on MDIO. A keypad must not panic because Ethernet is
    // absent — log it and carry on (WiFi is still there).
    esp_err_t ie = esp_eth_driver_install(&eth_cfg, &handle);
    if (ie != ESP_OK) {
        ESP_LOGW(TAG, "no Ethernet PHY (%s) — carrier absent or PHY fault; continuing",
                 esp_err_to_name(ie));
        return;
    }

    // Must run before esp_eth_start() — see the comment block above.
    ip101_clear_rx2tx_loopback(handle);

    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(handle)));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_evt, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_ip_evt, NULL));

    ESP_ERROR_CHECK(esp_eth_start(handle));

    ESP_LOGI(TAG, "waiting for DHCP (IP101 @ phy %d, reset gpio %d)...", ETH_PHY_ADDR, ETH_PHY_RST_GPIO);
    // 8s, not 20: when this is the preferred transport with WiFi behind it, a
    // long stall here delays every fallback boot (no cable, no carrier, dead port).
    EventBits_t b = xEventGroupWaitBits(s_eg, BIT_GOT_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(8000));
    if (!(b & BIT_GOT_IP)) ESP_LOGW(TAG, "no IP yet; continuing (will come up async on link)");
}

bool eth_is_up(void) { return s_up; }
