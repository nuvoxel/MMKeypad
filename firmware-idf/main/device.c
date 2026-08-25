#include "device.h"
#include "nuvoxel_device.h"
#include "config.h"   // fw_version()
#include "board.h"    // MMK_BOARD_NAME
#include "net.h"      // mmk_default_netif()
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"   // esp_restart (auto-OTA)
#include "esp_chip_info.h" // hw inventory
#include "esp_flash.h"     // flash size
#include "esp_idf_version.h"
#if defined(CONFIG_SPIRAM)
#include "esp_psram.h"
#endif
#include "nvs.h"
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// NVS storage for the perpetual license token (survives reboots; applied offline).
#define NV_LIC_NS  "nvx_dev"
#define NV_LIC_KEY "license"

static const char *TAG = "device";

extern esp_netif_t *mmk_default_netif(void);

static nv_identity_t s_id;

static struct {
  bool have_status;       // a check-in (or register) completed at least once
  bool registered;        // the platform recognizes this device (claimed)
  char pairing_code[12];  // shown when unregistered, to claim it in the app
  bool update_available;
  char latest_version[32];
  bool licensed;          // a valid, hardware-bound license token is present
  char tier[24];
  char features[256];
  bool trial;             // the current license is a time-limited trial
  int trial_days;         // days left in the trial (0 if not on trial)
} s_st;

// Map the compiled board to its platform SKU.
static const char *device_sku(void) {
  const char *b = MMK_BOARD_NAME;
  if (strcmp(b, "s3-lcdwiki") == 0) return "mmk-s3";
  if (strcmp(b, "p4-nano") == 0) return "mmk-nano";
  if (strcmp(b, "p4-poe-eth") == 0) return "mmk-poe";
  if (strcmp(b, "ws43") == 0) return "mmk-ws43";
  return "mmk-unknown";
}

const char *device_hardware_id(void) { return s_id.hardware_id; }
const char *device_secret_hex(void) { return s_id.device_secret; }
const char *device_sku_id(void) { return device_sku(); }

// Open build: no cloud enrollment. The device is always "registered" to itself.
const char *device_reg_text(void) { return "Standalone"; }
const char *device_pair_code(void) { return ""; }

// Open build: not license-gated. Every unit is fully licensed with all
// features, so the activation screen never shows and nothing is feature-gated.
bool device_is_licensed(void) { return true; }
const char *device_tier_text(void) { return "Open"; }
bool device_has_feature(const char *name) { (void)name; return true; }

// Verify a token against this device's identity + sku; update in-RAM status.
static void license_apply_status(const char *token) {
  nv_entitlement_t e;
  if (nv_entitlement_verify(token, &s_id, device_sku(), &e) == NV_OK && e.valid) {
    s_st.licensed = true;
    snprintf(s_st.tier, sizeof(s_st.tier), "%s", e.tier);
    snprintf(s_st.features, sizeof(s_st.features), "%s", e.features);
    s_st.trial = e.trial;
    s_st.trial_days = 0;
    if (e.trial && e.exp > 0) {
      long now = (long)time(NULL);
      if (now > 1700000000L && e.exp > now)
        s_st.trial_days = (int)((e.exp - now + 86399) / 86400);
    }
  } else {
    s_st.licensed = false;
    s_st.tier[0] = '\0';
    s_st.features[0] = '\0';
    s_st.trial = false;
    s_st.trial_days = 0;
  }
}

bool device_is_trial(void) { return s_st.licensed && s_st.trial; }

const char *device_license_label(void) { return "Open build"; }

// Load a stored license token from NVS and verify it (offline, no network).
static void license_load(void) {
  nvs_handle_t h;
  if (nvs_open(NV_LIC_NS, NVS_READONLY, &h) != ESP_OK) return;
  size_t len = 0;
  if (nvs_get_str(h, NV_LIC_KEY, NULL, &len) == ESP_OK && len > 1 && len < 1600) {
    char *tok = malloc(len);
    if (tok && nvs_get_str(h, NV_LIC_KEY, tok, &len) == ESP_OK) {
      license_apply_status(tok);
      ESP_LOGI(TAG, "license from NVS: %s %s", s_st.licensed ? "valid tier" : "INVALID", s_st.tier);
    }
    free(tok);
  }
  nvs_close(h);
}

// Apply a license token delivered out-of-band (settings paste / QR / C4 driver).
// Verifies offline against the baked-in key; persists to NVS iff valid. Returns 0 ok.
int device_apply_license(const char *token) {
  if (!token) return -1;
  nv_entitlement_t e;
  if (nv_entitlement_verify(token, &s_id, device_sku(), &e) != NV_OK || !e.valid) {
    ESP_LOGW(TAG, "license rejected (bad signature or binding)");
    return -1;
  }
  nvs_handle_t h;
  if (nvs_open(NV_LIC_NS, NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_str(h, NV_LIC_KEY, token);
    nvs_commit(h);
    nvs_close(h);
  }
  license_apply_status(token);
  ESP_LOGI(TAG, "license applied: tier %s features [%s]", s_st.tier, s_st.features);
  return 0;
}

void device_init(void) {
  if (nv_identity_init(&s_id) == NV_OK) {
    ESP_LOGI(TAG, "identity %s (provisioned=%d)", s_id.hardware_id, s_id.provisioned);
  } else {
    ESP_LOGW(TAG, "identity init failed");
  }
  license_load();
}

// Open build: no cloud check-in, no enrollment, no cloud OTA. device_start()
// has nothing to spawn -- the device is fully functional the moment it boots
// and connects to Control4 over the :6700 link + SDDP. Firmware updates come
// from the on-screen updater (GitHub Releases) and local/USB flashing.
void device_start(void) {
}

// The on-screen updater triggers an OTA directly via nv_ota_apply(); there is
// no cloud "check now" in the open build.
void device_ota_check_now(void) {
}

// ---- device manifest: model + hardware identifiers + connectivity ----------

// SoC + power defaults (ESP boards are fixed-config, so this is compile-time).
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#define MMK_SOC "esp32-p4"
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
#define MMK_SOC "esp32-s3"
#else
#define MMK_SOC "esp32"
#endif
#ifndef MMK_HAS_SIP
#define MMK_HAS_SIP 0
#endif
#ifndef LCD_WIDTH
#define LCD_WIDTH 0
#endif
#ifndef LCD_HEIGHT
#define LCD_HEIGHT 0
#endif
// Max programmable keypad buttons this device's UI can show ("up to N"). Every board
// sets this in board.h from its own panel geometry (there is no formula here on
// purpose — see the note at the bottom of board.h), so this guard only ever fires if
// board.h somehow didn't get included. Non-touch devices report 0 regardless.
#ifndef MMK_MAX_BUTTONS
#define MMK_MAX_BUTTONS 6
#endif
// PoE where we bring IP in over the wire (the eth SKUs); USB/wall otherwise.
#ifndef MMK_POWER
#define MMK_POWER (MMK_NET_ETH ? "poe" : "wall")
#endif

static char s_driver_ver[24];
void device_set_driver_version(const char *ver) {
  if (ver) snprintf(s_driver_ver, sizeof(s_driver_ver), "%s", ver);
}
const char *device_driver_version(void) { return s_driver_ver; }

const char *device_model_name(void) {
  const char *b = MMK_BOARD_NAME;
  if (strcmp(b, "s3-lcdwiki") == 0) return "M Keypad 2.8\"";
  if (strcmp(b, "p4-nano") == 0) return "M Keypad Nano";
  if (strcmp(b, "p4-poe-eth") == 0) return "M Keypad PoE";
  if (strcmp(b, "ws43") == 0) return "M Keypad 4.3\"";
  return b;
}

// The ESP identity's hardware_id is the MAC as 12 hex nibbles; present it
// with the usual colons for the settings page / manifest.
const char *device_mac(void) {
  static char mac[18];
  const char *h = s_id.hardware_id;
  if (strlen(h) >= 12) {
    snprintf(mac, sizeof(mac), "%c%c:%c%c:%c%c:%c%c:%c%c:%c%c", h[0], h[1], h[2],
             h[3], h[4], h[5], h[6], h[7], h[8], h[9], h[10], h[11]);
  } else {
    mac[0] = '\0';
  }
  return mac;
}

const char *device_link_type(void) { return MMK_NET_ETH ? "ethernet" : "wifi"; }
const char *device_power_source(void) { return MMK_POWER; }

// Runtime hardware inventory (mirrors the T3's device_hw_json). ESP boards are
// far more homogeneous than the repurposed T3 glass, but chip revision, flash/
// PSRAM size and the MAC still vary across units/revisions — report them + a
// stable fingerprint so the platform can spot a silent hardware change in beta.
static void hwfp_add(unsigned *h, const char *s) {
  if (!s) return;
  for (; *s; s++) { *h ^= (unsigned char)*s; *h *= 16777619u; }
}
static void device_hw_json(cJSON *hw) {
  unsigned fp = 2166136261u;
  char buf[48];

  cJSON *soc = cJSON_AddObjectToObject(hw, "soc");
  esp_chip_info_t ci;
  esp_chip_info(&ci);
  const char *model =
      ci.model == CHIP_ESP32S3 ? "ESP32-S3" :
      ci.model == CHIP_ESP32P4 ? "ESP32-P4" :
      ci.model == CHIP_ESP32C6 ? "ESP32-C6" :
      ci.model == CHIP_ESP32   ? "ESP32"    : "esp";
  cJSON_AddStringToObject(soc, "model", model);       hwfp_add(&fp, model);
  cJSON_AddNumberToObject(soc, "rev", ci.revision);
  cJSON_AddNumberToObject(soc, "cores", ci.cores);
  { char feat[48] = "";
    if (ci.features & CHIP_FEATURE_WIFI_BGN) strcat(feat, "wifi ");
    if (ci.features & CHIP_FEATURE_BT)       strcat(feat, "bt ");
    if (ci.features & CHIP_FEATURE_BLE)      strcat(feat, "ble ");
    if (feat[0]) cJSON_AddStringToObject(soc, "features", feat); }
  cJSON_AddStringToObject(soc, "idf", esp_get_idf_version());

  uint32_t fsz = 0;
  if (esp_flash_get_size(NULL, &fsz) == ESP_OK && fsz) {
    cJSON_AddNumberToObject(hw, "flashBytes", (double)fsz);
    snprintf(buf, sizeof buf, "%u", (unsigned)fsz); hwfp_add(&fp, buf);
  }
#if defined(CONFIG_SPIRAM)
  { size_t ps = esp_psram_get_size();
    if (ps) { cJSON_AddNumberToObject(hw, "psramBytes", (double)ps);
              snprintf(buf, sizeof buf, "%u", (unsigned)ps); hwfp_add(&fp, buf); } }
#endif
  cJSON_AddStringToObject(hw, "board", MMK_BOARD_NAME); hwfp_add(&fp, MMK_BOARD_NAME);

  cJSON *pan = cJSON_AddObjectToObject(hw, "panel");
  cJSON_AddNumberToObject(pan, "w", LCD_WIDTH);
  cJSON_AddNumberToObject(pan, "h", LCD_HEIGHT);
  snprintf(buf, sizeof buf, "%dx%d", LCD_WIDTH, LCD_HEIGHT); hwfp_add(&fp, buf);

  cJSON_AddStringToObject(hw, "mac", device_mac());    hwfp_add(&fp, device_mac());

  char fph[12];
  snprintf(fph, sizeof fph, "%08x", fp);
  cJSON_AddStringToObject(hw, "fp", fph);
}

void device_manifest_to_json(cJSON *m) {
  cJSON_AddStringToObject(m, "sku", device_sku());
  cJSON_AddStringToObject(m, "model", device_model_name());
  cJSON_AddStringToObject(m, "board", MMK_BOARD_NAME);
  cJSON_AddStringToObject(m, "soc", MMK_SOC);
  cJSON_AddStringToObject(m, "fw", fw_version());
  cJSON_AddStringToObject(m, "hwid", s_id.hardware_id);
  cJSON_AddStringToObject(m, "mac", device_mac());
  if (s_driver_ver[0]) cJSON_AddStringToObject(m, "driverVersion", s_driver_ver);

  cJSON *disp = cJSON_AddObjectToObject(m, "display");
  cJSON_AddNumberToObject(disp, "w", LCD_WIDTH);
  cJSON_AddNumberToObject(disp, "h", LCD_HEIGHT);

  // Runtime hardware inventory + fingerprint (chip rev, flash/PSRAM, MAC) — catch
  // silent hardware changes across beta units, same as the T3.
  device_hw_json(cJSON_AddObjectToObject(m, "hw"));

  cJSON *net = cJSON_AddObjectToObject(m, "net");
  cJSON_AddStringToObject(net, "link", device_link_type());

  cJSON *pwr = cJSON_AddObjectToObject(m, "power");
  cJSON_AddStringToObject(pwr, "source", device_power_source());
  // Candidate power sources for this hardware (operator picks the real one).
  // These boards are mains-powered (no battery); PoE is possible only on the
  // wired-ethernet SKUs.
  cJSON *opts = cJSON_AddArrayToObject(pwr, "options");
  cJSON_AddItemToArray(opts, cJSON_CreateString("wall"));
  if (MMK_NET_ETH) cJSON_AddItemToArray(opts, cJSON_CreateString("poe"));

  cJSON *caps = cJSON_AddObjectToObject(m, "caps");
  cJSON_AddBoolToObject(caps, "display", MMK_HAS_DISPLAY);
  cJSON_AddBoolToObject(caps, "touch", MMK_HAS_TOUCH);
  cJSON_AddBoolToObject(caps, "audio", MMK_HAS_AUDIO);
  cJSON_AddBoolToObject(caps, "intercom", MMK_HAS_SIP);
  // A "halo" RGB LED (PIN_RGB_LED) and live display rotation (MMK_CAN_ROTATE) are
  // per-board — report them so the driver hides the Halo / Orientation settings on
  // hardware that lacks them.
#if defined(PIN_RGB_LED)
  cJSON_AddBoolToObject(caps, "led", 1);
#else
  cJSON_AddBoolToObject(caps, "led", 0);
#endif
#if defined(MMK_CAN_ROTATE) && MMK_CAN_ROTATE
  cJSON_AddBoolToObject(caps, "rotate", 1);
#else
  cJSON_AddBoolToObject(caps, "rotate", 0);
#endif
  // Max programmable keypad buttons this device can show (0 = no keypad). The
  // driver treats it as an "up to" ceiling and clamps to its own capacity.
  cJSON_AddNumberToObject(caps, "buttons", MMK_HAS_TOUCH ? MMK_MAX_BUTTONS : 0);

  if (s_st.licensed && s_st.features[0])
    cJSON_AddStringToObject(m, "features", s_st.features);
}

void device_report_to_json(cJSON *r) {
  device_manifest_to_json(cJSON_AddObjectToObject(r, "manifest"));
  cJSON *s = cJSON_AddObjectToObject(r, "status");
  const char *room = net_current_room();
  if (room[0]) cJSON_AddStringToObject(s, "room", room);
  char ip[40] = {0};
  net_get_ip(ip, sizeof(ip));
  if (ip[0]) cJSON_AddStringToObject(s, "ip", ip);
  cJSON_AddBoolToObject(s, "driverConnected", net_connected());
  if (net_peer_ip()[0]) cJSON_AddStringToObject(s, "director", net_peer_ip());
  cJSON_AddNumberToObject(s, "orientation", g_settings.orientation);
  cJSON_AddNumberToObject(s, "brightness", g_settings.brightness);
  // The rest of the panel's own settings. These change how the panel BEHAVES (how it
  // looks, when it dims), and none of it was visible from the cloud or the driver --
  // the driver's properties read "Auto (device setting)", i.e. the device decides and
  // never says what it decided. Debugging meant walking up to the panel, and on an
  // ESP board the value lives in NVS with no way to read it back at all.
  cJSON_AddNumberToObject(s, "theme",          g_settings.theme);
  cJSON_AddNumberToObject(s, "layout",         g_settings.layout);
  cJSON_AddNumberToObject(s, "bgPreset",       g_settings.bg_preset);
  cJSON_AddNumberToObject(s, "screensaverSec", g_settings.screensaver_sec);
  cJSON_AddNumberToObject(s, "dimBrightness",  g_settings.dim_brightness);
  cJSON_AddNumberToObject(s, "ringerVolume",   g_settings.ringer_volume);
  cJSON_AddBoolToObject(  s, "muted",          g_settings.muted ? 1 : 0);
  if (s_driver_ver[0]) cJSON_AddStringToObject(s, "driverVersion", s_driver_ver);
}

void device_status_to_json(cJSON *d) {
  cJSON_AddStringToObject(d, "deviceId", s_id.hardware_id);
  // deviceSecret is exposed here for the OFFLINE claim path (a phone reads it from
  // this local, basic-auth'd page and claims by identity). Not for public exposure.
  cJSON_AddStringToObject(d, "deviceSecret", s_id.device_secret);
  cJSON_AddStringToObject(d, "sku", device_sku());
  if (s_st.have_status) {
    cJSON_AddStringToObject(d, "reg", s_st.registered ? "registered" : "unregistered");
    if (!s_st.registered && s_st.pairing_code[0]) {
      cJSON_AddStringToObject(d, "pairCode", s_st.pairing_code);
    }
    cJSON_AddBoolToObject(d, "otaUpdate", s_st.update_available);
    if (s_st.update_available && s_st.latest_version[0]) {
      cJSON_AddStringToObject(d, "otaLatest", s_st.latest_version);
    }
  } else {
    cJSON_AddStringToObject(d, "reg", "unknown");
  }
  cJSON_AddBoolToObject(d, "licensed", s_st.licensed);
  if (s_st.licensed) {
    cJSON_AddStringToObject(d, "tier", s_st.tier);
    cJSON_AddStringToObject(d, "features", s_st.features);
  }
}
