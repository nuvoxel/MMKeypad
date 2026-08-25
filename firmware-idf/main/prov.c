// MMKeypad — BLE Wi-Fi provisioning (see prov.h).
//
// Wraps Espressif's provisioning manager with the NimBLE BLE scheme, modeled on
// $IDF_PATH/examples/provisioning/wifi_prov_mgr. We use the `network_provisioning`
// MANAGED component, not the in-IDF `wifi_provisioning` one: on the P4 WiFi lives
// on the C6 via esp_wifi_remote, so the in-IDF component (gated on native
// CONFIG_ESP_WIFI_ENABLED) is headers-only and won't link. network_provisioning
// is Espressif's drop-in successor that builds against esp_wifi_remote — same BLE
// transport, PoP security-1, and QR payload; the API is just renamed
// wifi_prov_* → network_prov_* / WIFI_PROV_* → NETWORK_PROV_*.
//
// Two deviations from the stock example, both because our BT controller is NOT
// on-chip — it runs on the onboard ESP32-C6 over the esp_hosted VHCI:
//
//   1) Before the manager touches BLE we bring the C6 controller up ourselves
//      with esp_hosted_bt_controller_init()/_enable() (exactly like ble_spike.c).
//      protocomm's NimBLE transport then just calls nimble_port_init() — it does
//      NOT init a controller — so it binds to the already-running VHCI.
//   2) scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE, not FREE_BTDM: the
//      FREE_BTDM handler frees on-chip Bluedroid/BTDM memory (guarded by
//      CONFIG_BT_CONTROLLER_ENABLED, OFF here), so it's a no-op anyway, but NONE
//      states the intent — there is no local controller memory to free.
//
// Received creds persist to NVS (the manager flips storage back to flash before
// esp_wifi_set_config) and the STA connects; IP_EVENT_STA_GOT_IP then fires and
// wifi.c's on_evt sets BIT_CONNECTED — the SAME signal the portal/saved-cred
// paths use, so the rest of the app can't tell which path provisioned.

#include "prov.h"

#ifdef MMK_HAS_BLE_PROV

#include <string.h>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"

#ifdef MMK_BLE_HOSTED
// esp_hosted BT controller (runs on the C6 over the SDIO VHCI). Hosted boards only.
#include "esp_hosted.h"
#endif

#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"  // pulls protocomm.h + protocomm_ble.h

static const char *TAG = "prov";
static bool s_active;

// Catches provisioning / BLE-transport / security-session events. Registered on
// the default event loop, so it runs ALONGSIDE wifi.c's on_evt — that handler
// owns IP_EVENT_STA_GOT_IP → BIT_CONNECTED, so we deliberately don't duplicate
// the "connected" signal here; we only log + drive the prov state machine.
static void prov_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == NETWORK_PROV_EVENT) {
        switch (id) {
        case NETWORK_PROV_START:
            ESP_LOGI(TAG, "BLE provisioning started");
            break;
        case NETWORK_PROV_WIFI_CRED_RECV: {
            wifi_sta_config_t *c = (wifi_sta_config_t *)data;
            ESP_LOGI(TAG, "CRED_RECV ssid='%s'", (const char *)c->ssid);
            break;
        }
        case NETWORK_PROV_WIFI_CRED_FAIL: {
            network_prov_wifi_sta_fail_reason_t *r =
                (network_prov_wifi_sta_fail_reason_t *)data;
            ESP_LOGE(TAG, "CRED_FAIL: %s — phone should re-send creds",
                     (*r == NETWORK_PROV_WIFI_STA_AUTH_ERROR) ? "auth/password"
                                                              : "AP not found");
            break;
        }
        case NETWORK_PROV_WIFI_CRED_SUCCESS:
            ESP_LOGI(TAG, "CRED_SUCCESS — creds good, STA connected");
            break;
        case NETWORK_PROV_END:
            // Auto-fired after a successful provision (or on prov_stop). Release
            // the manager; the C6 controller stays up (harmless, low idle cost).
            ESP_LOGI(TAG, "provisioning end — deinit manager");
            network_prov_mgr_deinit();
            s_active = false;
            break;
        default:
            break;
        }
    } else if (base == PROTOCOMM_TRANSPORT_BLE_EVENT) {
        if (id == PROTOCOMM_TRANSPORT_BLE_CONNECTED)
            ESP_LOGI(TAG, "BLE transport: phone connected");
        else if (id == PROTOCOMM_TRANSPORT_BLE_DISCONNECTED)
            ESP_LOGI(TAG, "BLE transport: phone disconnected");
    } else if (base == PROTOCOMM_SECURITY_SESSION_EVENT) {
        if (id == PROTOCOMM_SECURITY_SESSION_SETUP_OK)
            ESP_LOGI(TAG, "secure session established (PoP ok)");
        else if (id == PROTOCOMM_SECURITY_SESSION_CREDENTIALS_MISMATCH)
            ESP_LOGE(TAG, "PoP mismatch — QR/pop is wrong");
        else if (id == PROTOCOMM_SECURITY_SESSION_INVALID_SECURITY_PARAMS)
            ESP_LOGE(TAG, "invalid security params");
    }
}

void prov_start(const char *service_name, const char *pop)
{
    if (s_active) return;

#ifdef MMK_BLE_HOSTED
    // 1) HOSTED (ws43): the BT controller runs on the onboard C6 across the
    //    esp_hosted VHCI — bring it up ourselves. WiFi already ran
    //    esp_hosted_connect_to_slave() during wifi_start(), so the SDIO link to
    //    the slave exists; these just enable its BT function (same sequence
    //    ble_spike.c proved). The scheme's nimble_port_init() finds NO local
    //    controller (CONFIG_BT_CONTROLLER_DISABLED) and just binds to this VHCI.
    if (esp_hosted_bt_controller_init() != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_bt_controller_init failed — BLE prov disabled");
        return;
    }
    if (esp_hosted_bt_controller_enable() != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_bt_controller_enable failed — BLE prov disabled");
        return;
    }
#else
    // 1) NATIVE (s3): the on-chip BLE controller is initialized AND enabled by the
    //    scheme's nimble_port_init() itself (esp_bt_controller_init +
    //    esp_bt_controller_enable(ESP_BT_MODE_BLE) — see IDF
    //    components/bt/host/nimble/nimble_port.c), so there is nothing to do here.
#endif

    // 2) Our event handler (NETWORK_PROV / BLE transport / security session). The
    //    default event loop + the STA netif already exist (wifi_start ran).
    ESP_ERROR_CHECK(esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID,
                                               prov_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(PROTOCOMM_TRANSPORT_BLE_EVENT, ESP_EVENT_ANY_ID,
                                               prov_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(PROTOCOMM_SECURITY_SESSION_EVENT, ESP_EVENT_ANY_ID,
                                               prov_event_handler, NULL));

    // 3) Init the manager with the NimBLE BLE scheme. NONE scheme-handler: the
    //    controller lives on the C6, there's no local BTDM memory to free.
    network_prov_mgr_config_t config = {
        .scheme = network_prov_scheme_ble,
        .scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
    };
    ESP_ERROR_CHECK(network_prov_mgr_init(config));

    // 4) Start advertising. service_name = BLE device name (MMKeypad-<mac>),
    //    security-1 with proof-of-possession `pop` (carried in the on-screen QR).
    //    service_key (softAP password) is irrelevant to the BLE scheme → NULL.
    esp_err_t err = network_prov_mgr_start_provisioning(NETWORK_PROV_SECURITY_1,
                                                        (const void *)pop,
                                                        service_name, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start_provisioning failed err=0x%x — BLE prov disabled", err);
        network_prov_mgr_deinit();
        return;
    }
    s_active = true;
    ESP_LOGI(TAG, "BLE provisioning up: name='%s' pop='%s' (security-1)",
             service_name, pop ? pop : "(none)");
}

void prov_stop(void)
{
    if (!s_active) return;
    // Triggers NETWORK_PROV_END → our handler deinits the manager + clears s_active.
    network_prov_mgr_stop_provisioning();
}

#endif // MMK_HAS_BLE_PROV
