// c6_ota — host-driven OTA of the onboard ESP32-C6 esp_hosted slave (ws43).
//
// DELIVERY METHOD: local partition. The slave app image (network_adapter.bin,
// built from managed_components/espressif__esp_hosted/slave for esp32c6, 2.12.9)
// is raw-flashed into the P4's `model` data partition (partitions.csv: 3MB @
// 0x820000, currently unused/reserved for esp-sr models). At runtime the host
// reads the raw image straight from that partition and streams it to the C6 over
// the already-up esp_hosted SDIO transport — so the OTA needs NO WiFi join and
// no HTTP server (the board is not provisioned yet). This mirrors the "Partition
// OTA" method in examples/host_performs_slave_ota (components/ota_partition), but
// collapsed into one self-contained file and pointed at our `model` label.
//
// FLOW: find `model` partition -> validate it holds a real ESP image (magic) ->
// parse the image header to get exact firmware size + embedded version string ->
// read current C6 slave version over RPC and SKIP if it already matches ->
// esp_hosted_slave_ota_begin() -> chunked esp_hosted_slave_ota_write() ->
// esp_hosted_slave_ota_end() -> esp_hosted_slave_ota_activate() (C6 reboots) ->
// reboot the host to re-sync the transport with the freshly-booted slave.
//
// Everything here is only reached when MMK_C6_OTA is defined at the app_main call
// site (board.h ws43 section, DEFAULT OFF). This TU always compiles on ws43.

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_partition.h"
#include "esp_app_format.h"
#include "esp_app_desc.h"

#include "esp_hosted.h"          // esp_hosted_get_coprocessor_fwversion()
#include "esp_hosted_ota.h"      // esp_hosted_slave_ota_{begin,write,end,activate}()
#include "esp_hosted_api_types.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "c6_ota.h"

static const char *TAG = "c6_ota";

// Partition holding the raw slave image. `model` is a 3MB DATA/spiffs region in
// partitions.csv; we read it as raw bytes (subtype ANY), no filesystem mounted.
#define C6_OTA_PARTITION_LABEL "model"

// SDIO transfer chunk. 1500 matches the example's default and the transport MTU.
#define C6_OTA_CHUNK_SIZE 1500

// Parse the ESP32 app image header at the start of the partition to recover the
// exact image size (so we stream only the real bytes, not the trailing 0xFF) and
// the embedded esp_app_desc version string. Adapted from ota_partition.c.
static esp_err_t c6_ota_parse_header(const esp_partition_t *part,
                                     size_t *out_size,
                                     char *out_version, size_t version_len)
{
    esp_image_header_t img_hdr;
    esp_image_segment_header_t seg_hdr;
    esp_app_desc_t app_desc;
    esp_err_t ret;

    ret = esp_partition_read(part, 0, &img_hdr, sizeof(img_hdr));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "read image header: %s", esp_err_to_name(ret));
        return ret;
    }
    if (img_hdr.magic != ESP_IMAGE_HEADER_MAGIC) {
        ESP_LOGE(TAG, "no valid image in '%s' (magic 0x%02x != 0x%02x) — did you flash "
                      "network_adapter.bin into the model partition?",
                 part->label, img_hdr.magic, ESP_IMAGE_HEADER_MAGIC);
        return ESP_ERR_INVALID_ARG;
    }

    size_t offset = sizeof(img_hdr);
    size_t total  = sizeof(img_hdr);
    for (int i = 0; i < img_hdr.segment_count; i++) {
        ret = esp_partition_read(part, offset, &seg_hdr, sizeof(seg_hdr));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "read segment %d header: %s", i, esp_err_to_name(ret));
            return ret;
        }
        // The esp_app_desc immediately follows the first segment header.
        if (i == 0) {
            ret = esp_partition_read(part, offset + sizeof(seg_hdr), &app_desc, sizeof(app_desc));
            if (ret == ESP_OK && app_desc.magic_word == ESP_APP_DESC_MAGIC_WORD) {
                // app_desc.version is a fixed 32-byte field (not guaranteed NUL-
                // terminated); copy bounded and terminate ourselves.
                size_t n = strnlen(app_desc.version, sizeof(app_desc.version));
                if (n > version_len - 1) n = version_len - 1;
                memcpy(out_version, app_desc.version, n);
                out_version[n] = '\0';
            } else {
                strncpy(out_version, "unknown", version_len - 1);
                out_version[version_len - 1] = '\0';
            }
        }
        total  += sizeof(seg_hdr) + seg_hdr.data_len;
        offset += sizeof(seg_hdr) + seg_hdr.data_len;
    }

    // 16-byte align, then the 1-byte checksum.
    total += (16 - (total % 16)) % 16;
    total += 1;
    // Optional appended SHA-256 (16-byte aligned before the 32-byte hash).
    if (img_hdr.hash_appended == 1) {
        total += (16 - (total % 16)) % 16;
        total += 32;
    }

    if (total == 0 || total > part->size) {
        ESP_LOGE(TAG, "computed image size %u invalid (partition %" PRIu32 ")",
                 (unsigned)total, part->size);
        return ESP_ERR_INVALID_SIZE;
    }
    *out_size = total;
    return ESP_OK;
}

void c6_ota_run(void)
{
    ESP_LOGW(TAG, "=== C6 slave OTA (MMK_C6_OTA build) ===");

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, C6_OTA_PARTITION_LABEL);
    if (!part) {
        ESP_LOGE(TAG, "partition '%s' not found — aborting OTA", C6_OTA_PARTITION_LABEL);
        return;
    }
    ESP_LOGI(TAG, "source partition '%s' @0x%" PRIx32 ", size %" PRIu32,
             part->label, part->address, part->size);

    size_t fw_size = 0;
    char new_ver[32] = {0};
    if (c6_ota_parse_header(part, &fw_size, new_ver, sizeof(new_ver)) != ESP_OK) {
        ESP_LOGE(TAG, "image validation failed — aborting OTA");
        return;
    }
    ESP_LOGI(TAG, "slave image OK: %u bytes, version '%s'", (unsigned)fw_size, new_ver);

    // Version gate: skip if the running C6 slave already reports this version.
    esp_hosted_coprocessor_fwver_t cur = {0};
    if (esp_hosted_get_coprocessor_fwversion(&cur) == ESP_OK) {
        char cur_str[32];
        snprintf(cur_str, sizeof(cur_str), "%" PRIu32 ".%" PRIu32 ".%" PRIu32,
                 cur.major1, cur.minor1, cur.patch1);
        ESP_LOGI(TAG, "current C6 slave version: %s", cur_str);
        if (strcmp(cur_str, new_ver) == 0) {
            ESP_LOGW(TAG, "C6 slave already at %s — OTA not required, skipping", cur_str);
            return;
        }
        ESP_LOGW(TAG, "upgrading C6 slave %s -> %s", cur_str, new_ver);
    } else {
        ESP_LOGW(TAG, "could not read current C6 slave version (older/BT-less slave) — "
                      "proceeding with OTA");
    }

    // Stream the image to the C6 over the esp_hosted transport.
    esp_err_t ret = esp_hosted_slave_ota_begin();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ota_begin failed: %s", esp_err_to_name(ret));
        return;
    }

    static uint8_t chunk[C6_OTA_CHUNK_SIZE];
    size_t offset = 0;
    uint32_t last_pct = 0;
    while (offset < fw_size) {
        size_t n = (fw_size - offset > C6_OTA_CHUNK_SIZE) ? C6_OTA_CHUNK_SIZE : (fw_size - offset);
        ret = esp_partition_read(part, offset, chunk, n);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "partition read @%u: %s", (unsigned)offset, esp_err_to_name(ret));
            esp_hosted_slave_ota_end();
            return;
        }
        ret = esp_hosted_slave_ota_write(chunk, n);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "ota_write @%u: %s", (unsigned)offset, esp_err_to_name(ret));
            esp_hosted_slave_ota_end();
            return;
        }
        offset += n;
        uint32_t pct = (uint32_t)((uint64_t)offset * 100 / fw_size);
        if (pct >= last_pct + 10 || offset == fw_size) {
            ESP_LOGI(TAG, "OTA progress: %" PRIu32 "%% (%u/%u bytes)",
                     pct, (unsigned)offset, (unsigned)fw_size);
            last_pct = pct;
        }
    }

    ret = esp_hosted_slave_ota_end();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ota_end (image verify) failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGW(TAG, "C6 slave image written + verified (%u bytes)", (unsigned)fw_size);

    // Activate: switch the C6 to the new slot; the C6 reboots into it.
    ret = esp_hosted_slave_ota_activate();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ota_activate failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGW(TAG, "C6 slave firmware activated — C6 rebooting into %s", new_ver);

    // The slave just rebooted; our transport state is now stale. Reboot the host
    // to re-establish the SDIO link cleanly. On the next boot the version gate
    // above sees the slave already at the target and skips (normal boot proceeds).
    ESP_LOGW(TAG, "rebooting host in 3s to re-sync transport with the new C6 slave...");
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
}
