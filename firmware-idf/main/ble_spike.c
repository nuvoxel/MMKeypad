// MMKeypad — BLE bring-up SPIKE.
//
// Proves the esp_hosted Bluetooth-over-SDIO HCI path works from the P4: the
// NimBLE HOST runs here on the P4, the BT CONTROLLER lives on the onboard
// ESP32-C6 (its factory slave already exposes BT over the same SDIO link WiFi
// uses — capabilities 0xd), and esp_hosted tunnels HCI between them as a VHCI.
// The spike just advertises a connectable BLE device NAME — no GATT services,
// no provisioning. It de-risks a future BLE WiFi-provisioning feature (memory
// coexistence with WiFi + audio + display is the real thing being proven).
//
// Init/advertise flow is modeled directly on the esp_hosted example
//   managed_components/espressif__esp_hosted/examples/host_nimble_bleprph_host_only_vhci
// trimmed to the peripheral-advertise essentials. Gated by MMK_BLE_SPIKE
// (defined only in the ws43 board.h section); the CMakeLists only compiles this
// file for ws43, and main.c only calls ble_spike_start() under the same guard.
//
// Structured to grow into wifi_provisioning: add a GATT service + on_sync hook
// that registers provisioning characteristics, then feed creds into wifi.c.

#include "board.h"
#include "device.h"   // device_variant_tag()

#ifdef MMK_BLE_SPIKE

#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"

// esp_hosted BT controller (runs on the C6 over the SDIO VHCI).
#include "esp_hosted.h"

// NimBLE host.
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

#include "ble_spike.h"

static const char *TAG = "ble_spike";

static uint8_t s_own_addr_type;
static char    s_dev_name[24];   // "<MODEL>-XXXXXX", e.g. T3-10-82A3A6

static void ble_spike_advertise(void);

// Build the advertised name from the WiFi STA MAC (same <MODEL>-<mac> scheme
// wifi.c uses for the softAP), so the BLE and WiFi identities line up. This runs
// after wifi_start(), so esp_wifi is up and the remote C6 MAC is readable.
static void ble_spike_make_name(void)
{
    uint8_t mac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK &&
        esp_read_mac(mac, ESP_MAC_BASE) != ESP_OK) {
        // No MAC available — fall back to a static name so the spike still runs.
        strlcpy(s_dev_name, "Keypad-BLE", sizeof(s_dev_name));
        return;
    }
    {
        char tag[24];
        device_variant_tag(tag, sizeof(tag));
        snprintf(s_dev_name, sizeof(s_dev_name), "%s-%02X%02X%02X",
                 tag, mac[3], mac[4], mac[5]);
    }
}

// GAP event callback — the spike only needs to keep advertising: re-advertise
// whenever a connection drops or an advertising round completes.
static int ble_spike_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
#if defined(BLE_GAP_EVENT_LINK_ESTAB)
    case BLE_GAP_EVENT_LINK_ESTAB:
#else
    case BLE_GAP_EVENT_CONNECT:
#endif
        ESP_LOGI(TAG, "connection %s; status=%d",
                 event->connect.status == 0 ? "established" : "failed",
                 event->connect.status);
        if (event->connect.status != 0) {
            ble_spike_advertise();   // failed — resume advertising
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnect; reason=%d", event->disconnect.reason);
        ble_spike_advertise();       // resume advertising
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "advertise complete; reason=%d", event->adv_complete.reason);
        ble_spike_advertise();
        return 0;

    default:
        return 0;
    }
}

// Connectable, general-discoverable undirected advertising carrying the name.
static void ble_spike_advertise(void)
{
    struct ble_hs_adv_fields fields;
    struct ble_gap_adv_params adv_params;
    int rc;

    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields failed; rc=%d", rc);
        return;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;   // connectable
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;    // general discoverable
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_spike_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start failed; rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "advertising as \"%s\"", name);
}

// Host<->controller are in sync (VHCI up) — pick an address and advertise.
static void ble_spike_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) { ESP_LOGE(TAG, "ensure_addr failed; rc=%d", rc); return; }

    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) { ESP_LOGE(TAG, "infer_auto failed; rc=%d", rc); return; }

    uint8_t addr[6] = {0};
    ble_hs_id_copy_addr(s_own_addr_type, addr, NULL);
    ESP_LOGI(TAG, "BLE synced; addr=%02x:%02x:%02x:%02x:%02x:%02x",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);

    ble_spike_advertise();
}

static void ble_spike_on_reset(int reason)
{
    ESP_LOGW(TAG, "nimble reset; reason=%d", reason);
}

static void ble_spike_host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "BLE host task started");
    nimble_port_run();               // returns only on nimble_port_stop()
    nimble_port_freertos_deinit();
}

void ble_spike_start(void)
{
    ESP_LOGI(TAG, "BLE spike: init hosted BT controller (C6 over SDIO VHCI)");

    // 1) Bring up the BT controller on the C6 across the esp_hosted VHCI. WiFi
    //    already ran esp_hosted_connect_to_slave() during wifi_start(), so the
    //    SDIO link to the slave exists; these just enable its BT function.
    if (esp_hosted_bt_controller_init() != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_bt_controller_init failed — aborting spike");
        return;
    }
    if (esp_hosted_bt_controller_enable() != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_bt_controller_enable failed — aborting spike");
        return;
    }

    // 2) Init the NimBLE host (its HCI transport is the hosted VHCI, selected by
    //    CONFIG_ESP_HOSTED_NIMBLE_HCI_VHCI).
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed; err=%d — aborting spike", err);
        return;
    }

    // 3) Host config: sync/reset callbacks only (no security, no GATT).
    ble_hs_cfg.reset_cb = ble_spike_on_reset;
    ble_hs_cfg.sync_cb  = ble_spike_on_sync;

    // 4) Device name (<MODEL>-<mac>, matching wifi.c's softAP scheme).
    ble_spike_make_name();
    int rc = ble_svc_gap_device_name_set(s_dev_name);
    if (rc != 0) ESP_LOGW(TAG, "device_name_set rc=%d", rc);

    // 5) Run the host task; on_sync fires once the C6 controller is ready and
    //    kicks off advertising.
    nimble_port_freertos_init(ble_spike_host_task);
    ESP_LOGI(TAG, "BLE spike started (advertising will begin on host sync)");
}

#endif // MMK_BLE_SPIKE
