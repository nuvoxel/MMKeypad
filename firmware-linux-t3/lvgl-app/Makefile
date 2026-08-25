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

# The version this binary REPORTS. It must match the version of the published
# image (the on-screen updater compares versions), so reading it from
# firmware-idf/version.txt keeps the build and the release tag in lockstep. The
# `dev` default is for local builds that were never published.
FW_VERSION ?= $(shell cat ../../firmware-idf/version.txt 2>/dev/null || echo dev)

CFLAGS := -target $(TARGET) -mcpu=$(MCPU) -static -Os \
          -DMMK_FW_VERSION='"$(FW_VERSION)"' \
          -DMMK_BOARD_T3 \
          -I. -Iinclude_alias -I$(PLAT) -I$(COMPAT) -I$(CJSON) \
          -I$(ESPJPEG) -I$(ESPJPEG)/tjpgd -I$(MBEDTLS)/include \
          -I$(TINYALSA)/include -I$(LIBRE)/include \
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
            $(IDF_MAIN)/mmk_c4icons.c $(IDF_MAIN)/mmk_ha_icons.c \
            $(IDF_MAIN)/fwupdate.c

# Platform layer (incl. real album-art HTTPS client + tinyalsa audio backend).
PLAT_SRCS := $(PLAT)/bsp_linux.c $(PLAT)/stubs.c $(PLAT)/nvs_file.c \
             $(PLAT)/httpc_linux.c $(PLAT)/audio_linux.c $(PLAT)/ca_bundle.c \
             $(PLAT)/wifi_linux.c $(PLAT)/wifi_setup.c $(PLAT)/sip_linux.c $(CJSON)/cJSON.c

# Device-plane, target-native pieces: identity from the RK3188 eFuse, the
# device.h implementation, the Linux app-overlay OTA, and the baked license
# device identity (eFuse) + the T3 overlay OTA.
PLAT_SRCS += $(PLAT)/nv_identity_t3.c $(PLAT)/device_t3.c \
             $(PLAT)/nv_ota_t3.c

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
# with no source change must still relink. Without it a version bump in
# version.txt would leave `make` saying "nothing to be done", shipping a binary
# stamped with the PREVIOUS version -- which then never matches the release
# itself forever (the exact loop the stamping was added to prevent).
HDR_DEPS := lv_conf.h Makefile ../../firmware-idf/version.txt \
            $(wildcard $(COMPAT)/*.h) $(wildcard $(PLAT)/*.h)

.PHONY: all clean
all: $(BUILD)/mmk-app

# Fetch + cross-build libre on first use (mirrors how build-wpa.sh populates the
# gitignored thirdparty/ tree). Rebuilding it is slow and it changes only when the
# pinned tag moves, so this is an order-only-style existence check, not a timestamp dep.
$(LIBRE_A):
	../tools/build-libre.sh

$(BUILD)/mmk-app: $(SRCS) $(HDR_DEPS) $(LIBRE_A)
	mkdir -p $(BUILD)
	$(ZIG) $(CFLAGS) -o $@ $(SRCS) $(LIBRE_A) $(LDFLAGS)
	@echo "built $@"

clean:
	rm -rf $(BUILD)
