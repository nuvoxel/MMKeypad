# Cross-build the full MMKeypad app for the T3 (RK3188), static musl.
# Compiles the shared app (ui/net/config/art from firmware-idf/main via the
# shared/ symlinks) + generated fonts + the LVGL 9.3 core + the platform
# shims + mbedtls (album-art HTTPS) + esp_jpeg/tjpgd (decode), into one
# static ARM binary.

ZIG      := zig cc
TARGET   := arm-linux-musleabihf
MCPU     := cortex_a9

LVGL_DIR := ../../firmware-idf/managed_components/lvgl__lvgl
IDF_MAIN := ../../firmware-idf/main
PLAT     := ../platform
COMPAT   := ../platform/compat
CJSON    := ../thirdparty/cJSON
MBEDTLS  := ../thirdparty/mbedtls-3.6.2
ESPJPEG  := ../thirdparty/esp_jpeg
TINYALSA := ../thirdparty/tinyalsa
LIBRE    := ../thirdparty/libre
BUILD    := build

# Shared nuvoxel device-plane client (identity/check-in/OTA/entitlement),
# same component the S3 uses. Cross-repo sibling by default; override with
# NUVOXEL_COMPONENTS_DIR to relocate (mirrors firmware-idf/CMakeLists.txt).
NVDEV     ?= $(if $(NUVOXEL_COMPONENTS_DIR),$(NUVOXEL_COMPONENTS_DIR)/nuvoxel_device,../../../../firmware/components/nuvoxel_device)
NVDEV_INC := $(NVDEV)/include

# The version this binary REPORTS at check-in. It must match, byte for byte, the
# version publish-fw.sh records in the OTA manifest -- device.c updates whenever
# manifest != current, so a build that misreports its version re-downloads the
# same image forever, exits each time, and init's crash-loop guard rolls it back
# to factory. publish-fw.sh writes version.txt BEFORE invoking this build, so
# reading it here keeps the two ends automatically in lockstep. The `dev` default
# is for local builds that were never published.
FW_VERSION ?= $(shell cat ../../firmware-idf/version.txt 2>/dev/null || echo dev)

CFLAGS := -target $(TARGET) -mcpu=$(MCPU) -static -Os \
          -DMMK_FW_VERSION='"$(FW_VERSION)"' \
          -DMMK_BOARD_T3 \
          -I. -Iinclude_alias -I$(PLAT) -I$(COMPAT) -I$(CJSON) \
          -I$(ESPJPEG) -I$(ESPJPEG)/tjpgd -I$(MBEDTLS)/include \
          -I$(TINYALSA)/include -I$(NVDEV_INC) -I$(LIBRE)/include \
          -I$(IDF_MAIN) -I$(LVGL_DIR) -I$(LVGL_DIR)/src \
          -DLV_CONF_INCLUDE_SIMPLE \
          -ffunction-sections -fdata-sections

LDFLAGS := -Wl,--gc-sections -lpthread -lm

# Phase-3 SIP intercom stack (libre + librem), built by tools/build-libre.sh into
# a static ARM archive. Listed as a link input rather than a source set because
# it's a fetched third-party tree with its own feature defines. Must appear on the
# link line AFTER our objects so the linker can resolve into it.
LIBRE_A := $(LIBRE)/libre.a

# LVGL core (minus examples/demos/tests).
LV_SRCS := $(shell find $(LVGL_DIR)/src -name '*.c' | grep -vE '/examples/|/demos/|/tests/')

# Shared app (symlinks in shared/) + generated fonts.
APP_SRCS := shared/ui.c shared/net.c shared/config.c shared/art.c shared/sddp.c \
            $(IDF_MAIN)/mmk_text_14.c $(IDF_MAIN)/mmk_text_16.c \
            $(IDF_MAIN)/mmk_text_24.c $(IDF_MAIN)/mmk_text_32.c \
            $(IDF_MAIN)/mmk_icons_24.c $(IDF_MAIN)/mmk_icons_34.c \
            $(IDF_MAIN)/mmk_roboto.c $(IDF_MAIN)/mmk_lucide.c \
            $(IDF_MAIN)/mmk_c4icons.c $(IDF_MAIN)/mmk_ha_icons.c

# Platform layer (incl. real album-art HTTPS client + tinyalsa audio backend).
PLAT_SRCS := $(PLAT)/bsp_linux.c $(PLAT)/stubs.c $(PLAT)/nvs_file.c \
             $(PLAT)/httpc_linux.c $(PLAT)/audio_linux.c $(PLAT)/ca_bundle.c \
             $(PLAT)/wifi_linux.c $(PLAT)/wifi_setup.c $(PLAT)/sip_linux.c $(CJSON)/cJSON.c

# Device-plane, target-native pieces: identity from the RK3188 eFuse, the
# device.h implementation, the Linux app-overlay OTA, and the baked license
# public key. These stand in for nuvoxel_device's nv_identity.c / nv_ota.c.
PLAT_SRCS += $(PLAT)/nv_identity_t3.c $(PLAT)/device_t3.c \
             $(PLAT)/nv_ota_t3.c $(PLAT)/nv_license_pubkey.c

# The rest of nuvoxel_device, reused verbatim but compiled with -DESP_PLATFORM
# so it takes the embedded backends (mbedtls + esp_http_client shim) instead of
# the host OpenSSL/libcurl ones. Built as separate objects because the main app
# must NOT define ESP_PLATFORM.
NVDEV_SRCS  := nv_crypto.c nv_http.c nv_client.c nv_entitlement.c nv_json.c
NVDEV_OBJS  := $(addprefix $(BUILD)/nvdev/,$(NVDEV_SRCS:.c=.o))

# tinyalsa (hardware backend; snd_card_plugin.c provides snd_utils_* dev-node
# helpers the hw backend needs — its dlopen plugin path is unused for hw cards).
TINYALSA_SRCS := $(TINYALSA)/src/pcm.c $(TINYALSA)/src/pcm_hw.c \
                 $(TINYALSA)/src/mixer.c $(TINYALSA)/src/mixer_hw.c \
                 $(TINYALSA)/src/snd_card_plugin.c $(TINYALSA)/src/limits.c

# Album-art JPEG decode (esp_jpeg wrapper over bundled tjpgd).
JPEG_SRCS := $(ESPJPEG)/jpeg_decoder.c $(ESPJPEG)/jpeg_default_huffman_table.c \
             $(ESPJPEG)/tjpgd/tjpgd.c

# TLS for the HTTPS album-art fetch.
MBED_SRCS := $(wildcard $(MBEDTLS)/library/*.c)

SRCS := main_linux.c $(APP_SRCS) $(PLAT_SRCS) $(TINYALSA_SRCS) $(JPEG_SRCS) $(MBED_SRCS) $(LV_SRCS)

# Header/config prereqs -- config-only changes (e.g. sdkconfig.h JD_FASTDECODE,
# lv_conf.h) MUST force a rebuild since we compile in one shot with no object
# tracking. (Missing this silently shipped a stale decoder buffer once.)
# version.txt is a REAL dependency: it feeds -DMMK_FW_VERSION, so a version bump
# with no source change must still relink. Without it `publish-fw.sh auto` bumps
# version.txt, make says "nothing to be done", and we ship a binary stamped with
# the PREVIOUS version -- which then never matches the manifest and re-installs
# itself forever (the exact loop the stamping was added to prevent).
HDR_DEPS := lv_conf.h Makefile ../../firmware-idf/version.txt \
            $(wildcard $(COMPAT)/*.h) $(wildcard $(PLAT)/*.h)

.PHONY: all clean
all: $(BUILD)/mmk-app

# nuvoxel_device reuse objects — compiled with -DESP_PLATFORM (embedded backend).
$(BUILD)/nvdev/%.o: $(NVDEV)/src/%.c
	mkdir -p $(BUILD)/nvdev
	$(ZIG) $(CFLAGS) -DESP_PLATFORM -c $< -o $@

# Fetch + cross-build libre on first use (mirrors how build-wpa.sh populates the
# gitignored thirdparty/ tree). Rebuilding it is slow and it changes only when the
# pinned tag moves, so this is an order-only-style existence check, not a timestamp dep.
$(LIBRE_A):
	../tools/build-libre.sh

$(BUILD)/mmk-app: $(SRCS) $(NVDEV_OBJS) $(HDR_DEPS) $(LIBRE_A)
	mkdir -p $(BUILD)
	$(ZIG) $(CFLAGS) -o $@ $(SRCS) $(NVDEV_OBJS) $(LIBRE_A) $(LDFLAGS)
	@echo "built $@"

clean:
	rm -rf $(BUILD)
