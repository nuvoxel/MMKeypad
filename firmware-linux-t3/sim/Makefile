# Headless UI snapshot simulator -- builds the REAL shared ui.c against a dummy
# in-memory LVGL display on the host (macOS/Linux), renders a PNG. No SDL, no
# window, no hardware. See README.md.
#
# It reuses, verbatim: firmware-idf/main/ui.c + the generated fonts + the LVGL
# 9.3 core + the ESP compat shims (esp_timer/esp_heap_caps). Everything else
# (net/art/device/bsp/config/sip) is replaced by sim stubs.c.

CC       ?= cc
LVGL_DIR := ../../firmware-idf/managed_components/lvgl__lvgl
IDF_MAIN := ../../firmware-idf/main
COMPAT   := ../platform/compat
CJSON    := ../thirdparty/cJSON
BUILD    := build

# Include order matters for the header-shadowing trick (mirrors the T3 build):
#   -Ishared : app HEADERS (ui.h/net.h/sip.h/...) are symlinked here but board.h
#              and bsp.h are deliberately NOT -- so the app include CHAIN stays
#              rooted in shared/ (quote-include base = the symlink's dir) and
#              board.h/bsp.h fall through to...
#   -I.      : the sim shadows (board.h, bsp.h, lv_conf.h) win here.
# Compiling shared/ui.c (not IDF_MAIN/ui.c) is what keeps that chain in shared/.
# -DLV_LVGL_H_INCLUDE_SIMPLE makes the generated fonts include "lvgl.h" (found
# via -ILVGL_DIR) instead of the ESP component path "lvgl/lvgl.h".
CFLAGS := -Ishared -I. -I$(COMPAT) -I$(CJSON) -I$(IDF_MAIN) -I$(LVGL_DIR) -I$(LVGL_DIR)/src \
          -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -O2 -g -Wall -Wno-unused-parameter \
          -ffunction-sections -fdata-sections
LDFLAGS := -lm -lpthread

# LVGL core (minus examples/demos/tests). Backend drivers (fbdev/evdev/sdl/drm)
# are compiled but self-elide -- their feature flags are 0 in sim/lv_conf.h, so
# the <linux/*.h> includes that would break a macOS host never fire. The bundled
# lodepng (LV_USE_LODEPNG 1) provides BOTH ui.c's decode and our PNG encode.
LV_SRCS := $(shell find $(LVGL_DIR)/src -name '*.c' | grep -vE '/examples/|/demos/|/tests/')

# Shared UI (via the shared/ symlink -- see CFLAGS note) + generated fonts.
# Font list is the SAME set the T3 firmware compiles (known-good).
APP_SRCS := shared/ui.c \
            $(IDF_MAIN)/mmk_text_14.c $(IDF_MAIN)/mmk_text_16.c \
            $(IDF_MAIN)/mmk_text_24.c $(IDF_MAIN)/mmk_text_32.c \
            $(IDF_MAIN)/mmk_icons_24.c $(IDF_MAIN)/mmk_icons_34.c \
            $(IDF_MAIN)/mmk_roboto.c $(IDF_MAIN)/mmk_lucide.c \
            $(IDF_MAIN)/mmk_c4icons.c $(IDF_MAIN)/mmk_ha_icons.c

SIM_SRCS := render_main.c stubs.c logo_png.c
SRCS     := $(SIM_SRCS) $(APP_SRCS) $(LV_SRCS)
HDRS     := lv_conf.h board.h bsp.h

.PHONY: all clean
all: $(BUILD)/mmk-sim

# One-shot compile (no object tracking) -- config/header changes force a rebuild.
$(BUILD)/mmk-sim: $(SRCS) $(HDRS) Makefile
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SIM_SRCS) $(APP_SRCS) $(LV_SRCS) $(LDFLAGS)
	@echo "built $@"

clean:
	rm -rf $(BUILD) shots
