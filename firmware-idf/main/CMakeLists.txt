# One source tree builds multiple boards, selected by the MMK_BOARD cache var
# (pass -D MMK_BOARD=<name>; defaults to the S3 lcdwiki keypad). The value is also
# turned into a MMK_BOARD_<UPPER> compile definition that board.h keys off.
#   s3_lcdwiki  — lcdwiki 2.8" ESP32-S3 (SPI ILI9341 + FT6336), WiFi, audio
#   p4_poe_eth  — Waveshare ESP32-P4-POE-ETH-NH (headless, Ethernet, audio)
# IDF collects REQUIRES by evaluating this file in a scripting-mode SUBPROCESS
# ("early expansion") where -D cache vars don't exist — only the environment
# survives. board.sh exports MMK_BOARD as well as passing -D, so per-board
# requirements resolve correctly in both passes. (A bare idf.py build without
# the env var silently expands as the s3 default — use board.sh.)
if(NOT DEFINED MMK_BOARD)
    if(DEFINED ENV{MMK_BOARD})
        set(MMK_BOARD "$ENV{MMK_BOARD}")
    else()
        set(MMK_BOARD "s3_lcdwiki")
    endif()
endif()
message(STATUS "main: MMK_BOARD='${MMK_BOARD}'")

# Shared (every board): protocol server, discovery, settings web/OTA, config.
set(srcs   "main.c" "config.c" "net.c" "sddp.c" "device.c"
           "nv_open.c" "nv_identity_open.c" "nv_ota_open.c")
set(reqs   nvs_flash esp_netif esp_event lwip json esp_hw_support
           esp_http_server esp_http_client esp_https_ota esp-tls mbedtls
           app_update esp_timer esp_driver_i2c esp_driver_gpio
           spi_flash esp_psram)  # device.c hw inventory: esp_flash.h / esp_psram.h

# LVGL UI sources shared by the display boards.
# nav_poc.c removed: a navigator-browse render proof-of-concept that was
# compiled into every display build but had no caller -- nothing ever invoked
# nav_poc_show(). It cost flash on every board to render a screen no user
# could reach. Recoverable from git if the browse work resumes.
set(ui_srcs "ui.c" "art.c" "lodepng.c"
            "mmk_text_14.c" "mmk_text_16.c" "mmk_text_24.c" "mmk_text_32.c"
            "mmk_icons_24.c" "mmk_icons_34.c" "mmk_c4icons.c" "mmk_ha_icons.c")
# Audio/SIP intercom sources shared by the audio boards (s3 + p4_poe_eth).
set(audio_srcs "audio.c" "sip.c")
set(audio_reqs esp_driver_i2s espressif__media_lib_sal)
# Embedded notification samples (raw 16 kHz mono PCM): doorbell + connect beep.
set(audio_embeds "beep.pcm" "doorbell.pcm")
# NuVoxel boot-splash PNG (decoded on-device via lodepng), display boards only.
set(ui_embeds "splash.png" "logo.png")   # boot splash (white bg) + transparent logo (setup screen)
set(embeds "")

if(MMK_BOARD STREQUAL "p4_nano")
    # Waveshare ESP32-P4-NANO KIT-D — 10.1" DSI + PoE Ethernet + ES8311 audio.
    # Display/touch/backlight via Waveshare's official BSP component (gated to
    # this board in idf_component.yml via the MMK_BOARD env var).
    list(APPEND srcs "bsp_nano.c" "eth.c" ${ui_srcs} ${audio_srcs})
    list(APPEND embeds ${ui_embeds} ${audio_embeds})
    list(APPEND reqs esp_eth esp32_p4_nano ${audio_reqs})
elseif(MMK_BOARD STREQUAL "ws43")
    # Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3 — 480x800 portrait ST7701 DSI +
    # GT911 touch + ES8311/ES7210 audio. Display+touch bring-up first: no eth.c,
    # and WiFi (C6/esp_hosted) is deferred, so wifi.c is left out for the first
    # boot. The esp_lcd_st7701 managed dep is gated to this board in
    # idf_component.yml via the MMK_BOARD env var (board.sh exports it).
    # Audio ENABLED (MMK_HAS_AUDIO=1): ES8311 speaker + ES7210 dual-mic (HW AEC).
    # audio.c builds a two-handle codec_dev on this board (ES8311 out + ES7210 in);
    # the ES7210 driver ships inside espressif/esp_codec_dev (audio_reqs pulls the
    # esp_codec_dev + media_lib deps; the codec_dev managed dep is gated to p4 in
    # idf_component.yml, ES7210 needs no separate component).
    # WiFi via the onboard ESP32-C6 over esp_hosted 1.4.x (Waveshare's factory
    # stack — see the esp_hosted `matches` entry in idf_component.yml, which pins
    # 1.4.* for ws43 vs 2.12.x for the CrowPanels). wifi.c is version-agnostic
    # (standard esp_wifi_* over esp_wifi_remote) + runs the softAP captive portal.
    # c6_ota.c: host-driven OTA of the onboard ESP32-C6 esp_hosted slave from the
    # `model` partition (bench tool; the call in app_main is gated by MMK_C6_OTA,
    # default OFF — see board.h). esp_partition + esp_app_format let it read/parse
    # the raw slave image; esp_hosted carries the esp_hosted_slave_ota_* RPC.
    # ble_spike.c: BLE advertise spike (MMK_BLE_SPIKE, default OFF) — NimBLE host
    # on the P4 with the C6 as controller over esp_hosted VHCI (needs the 2.12.x
    # slave with BT). `bt` req pulls NimBLE.
    # prov.c: BLE Wi-Fi provisioning (NimBLE BLE scheme, C6 controller over the
    # esp_hosted VHCI — see board.h MMK_HAS_BLE_PROV). Uses the managed
    # `network_provisioning` component (NOT the in-IDF wifi_provisioning, which is
    # headers-only under esp_wifi_remote); it's gated to ws43 in idf_component.yml
    # and pulls protocomm (its public REQUIRE) for the protocomm BLE + security-
    # session event bases prov.c references. The on-screen QR uses the LVGL QR
    # widget (LV_USE_QRCODE), so no esp_qrcode component is needed.
    list(APPEND srcs "bsp_ws43.c" "wifi.c" "c6_ota.c" "ble_spike.c" "prov.c" ${ui_srcs} ${audio_srcs})
    list(APPEND embeds ${ui_embeds} ${audio_embeds})
    list(APPEND reqs esp_lcd esp_lcd_st7701 esp_lcd_touch_gt911 esp_driver_ledc
                     esp_wifi esp_wifi_remote esp_hosted bt
                     espressif__network_provisioning protocomm
                     esp_partition esp_app_format ${audio_reqs})
elseif(MMK_BOARD STREQUAL "p4_poe_eth")
    # Waveshare ESP32-P4-POE-ETH-NH — headless wired node.
    list(APPEND srcs "eth.c" ${audio_srcs})
    list(APPEND reqs esp_eth ${audio_reqs})
    list(APPEND embeds ${audio_embeds})
elseif(MMK_BOARD STREQUAL "s3_matrix")
    # ESP32-S3 + 64x64 HUB75 RGB LED matrix — album-art-only display (no touch,
    # no audio). Reuses the whole net/protocol/art stack: only the display layer
    # (bsp_matrix.cpp) and a slim UI (ui_matrix.c) differ from s3_lcdwiki. Art is
    # art.c (JPEG/PNG decode + scale) rendered into a 64x64 LVGL canvas that
    # flushes to the panel. WiFi via the softAP captive portal (wifi.c). The
    # `hub75` DMA driver (esphome/esp-hub75, MIT) is gated to this board in
    # idf_component.yml via the MMK_BOARD env var — a bare idf.py build must
    # `export MMK_BOARD=s3_matrix` first (board.sh does). lvgl/esp_lvgl_port/
    # esp_jpeg come from idf_component.yml (target esp32s3), auto-required by main.
    list(APPEND srcs "bsp_matrix.cpp" "bsp_matrix_disp.c" "ui_matrix.c" "wifi.c" "art.c" "lodepng.c")
    list(APPEND reqs esp_wifi esphome__esp-hub75)
else()
    # s3_lcdwiki — display + touch UI, WiFi, audio/SIP intercom, halo RGB LED.
    # NO BLE provisioning: native NimBLE doesn't fit the S3's internal RAM alongside
    # WiFi + LVGL (crash/OOM — see board.h), so the S3 is softAP-captive-portal only.
    # BLE provisioning is a C6-board feature (ws43).
    list(APPEND srcs "wifi.c" "bsp.c" "halo.c" ${ui_srcs} ${audio_srcs})
    list(APPEND reqs esp_wifi espressif__led_strip ${audio_reqs})
    list(APPEND embeds ${audio_embeds} ${ui_embeds})
endif()

idf_component_register(SRCS ${srcs} INCLUDE_DIRS "." REQUIRES ${reqs} EMBED_FILES ${embeds})

# Expose the selected board to the C code as MMK_BOARD_<UPPER>.
string(TOUPPER "${MMK_BOARD}" _board_uc)
target_compile_definitions(${COMPONENT_LIB} PUBLIC "MMK_BOARD_${_board_uc}=1")

# Display boards: LVGL font include form + vendored lodepng decoder-only flags.
if(MMK_BOARD STREQUAL "s3_lcdwiki" OR MMK_BOARD STREQUAL "p4_nano" OR MMK_BOARD STREQUAL "ws43")
    set_source_files_properties(mmk_text_14.c mmk_text_16.c mmk_text_24.c mmk_text_32.c
                                mmk_icons_24.c mmk_icons_34.c PROPERTIES
        COMPILE_DEFINITIONS "LV_LVGL_H_INCLUDE_SIMPLE")
    set_source_files_properties(lodepng.c PROPERTIES COMPILE_DEFINITIONS
        "LODEPNG_NO_COMPILE_ENCODER;LODEPNG_NO_COMPILE_DISK;LODEPNG_NO_COMPILE_ALLOCATORS;LODEPNG_NO_COMPILE_CPP;LODEPNG_NO_COMPILE_ERROR_TEXT"
        COMPILE_OPTIONS "-Wno-error")
endif()

# s3_matrix builds art.c + lodepng.c (album-art decode) but no LVGL fonts/icons.
if(MMK_BOARD STREQUAL "s3_matrix")
    set_source_files_properties(lodepng.c PROPERTIES COMPILE_DEFINITIONS
        "LODEPNG_NO_COMPILE_ENCODER;LODEPNG_NO_COMPILE_DISK;LODEPNG_NO_COMPILE_ALLOCATORS;LODEPNG_NO_COMPILE_CPP;LODEPNG_NO_COMPILE_ERROR_TEXT"
        COMPILE_OPTIONS "-Wno-error")
endif()
