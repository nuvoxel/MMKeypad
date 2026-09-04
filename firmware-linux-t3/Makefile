# Cross-build for the custom T3 (RK3188) Linux image.
# Host: macOS (arm64). Cross toolchain: zig cc -target arm-linux-musleabihf
# (musl.cc's prebuilt toolchains are Linux-x86_64-hosted and won't run here;
# Zig cross-compiles natively from macOS with no separate sysroot download).

ZIG        := zig cc
TARGET     := arm-linux-musleabihf
MCPU       := cortex_a9
CFLAGS     := -target $(TARGET) -mcpu=$(MCPU) -static -Os -Wall -s
EXTRA_CFLAGS ?=

BUILD_DIR  := build
ROOTFS_DIR := rootfs

# Reuse the stock kernel + original boot header verbatim (see plan). These
# come from the Phase-0 extraction of the device's own boot.orig.
STOCK       := ../reference/t3-control4/firmware/extracted
KERNEL      := $(STOCK)/kernel.img
BOOT_ORIG   := $(STOCK)/boot.orig

.PHONY: all init ramdisk bootimg wifi-tools selftest bundle clean

all: bootimg

init: $(BUILD_DIR)/init

$(BUILD_DIR)/init: init/init.c
	mkdir -p $(BUILD_DIR)
	$(ZIG) $(CFLAGS) $(EXTRA_CFLAGS) -o $@ $<
	cp $@ $(ROOTFS_DIR)/init

# WiFi userspace (AP6330): stage the cross-built wpa_supplicant + wpa_cli into
# the rootfs so a fresh flash ships them at /usr/sbin. tools/build-wpa.sh fetches
# the sources + builds on first run; the driver/firmware/NVRAM live on stock
# /system (init.c insmods rkwifi.oob.ko). udhcpc is a busybox applet.
WPA_BIN := thirdparty/wpa_supplicant-2.10/wpa_supplicant
wifi-tools:
	@[ -x $(WPA_BIN)/wpa_supplicant ] || tools/build-wpa.sh
	mkdir -p $(ROOTFS_DIR)/usr/sbin
	cp $(WPA_BIN)/wpa_supplicant $(ROOTFS_DIR)/usr/sbin/wpa_supplicant
	cp $(WPA_BIN)/wpa_cli        $(ROOTFS_DIR)/usr/sbin/wpa_cli
	chmod +x $(ROOTFS_DIR)/usr/sbin/wpa_supplicant $(ROOTFS_DIR)/usr/sbin/wpa_cli

# SSH-triggerable A/V bring-up test (audio loopback + camera capture). Staged at
# /usr/sbin/mmk-selftest. Cross-builds tinyalsa + the camera V4L2 capture.
selftest:
	tools/build-selftest.sh

# Ramdisk: build the newc cpio with forced root (0:0) ownership via mkcpio.py
# (dropbear rejects a non-root-owned authorized_keys; host cpio would stamp
# our uid), then gzip.
# MMK_DEV=1 stages the developer SSH key into the image. It is NOT in the rootfs
# any more: it used to be committed at rootfs/root/.ssh/authorized_keys, which put
# one developer's key into the boot image of every unit ever flashed from this tree
# -- root over the network on every panel, for as long as that key exists. Dev
# builds opt in; shipped images have no authorized_keys at all.
ramdisk: init wifi-tools selftest
ifeq ($(MMK_DEV),1)
	mkdir -p $(ROOTFS_DIR)/root/.ssh
	cp dev-keys/authorized_keys $(ROOTFS_DIR)/root/.ssh/authorized_keys
	@echo "*** MMK_DEV=1: developer SSH key staged into the image (do NOT ship) ***"
else
	rm -f $(ROOTFS_DIR)/root/.ssh/authorized_keys
endif
	python3 tools/mkcpio.py $(ROOTFS_DIR) $(BUILD_DIR)/ramdisk.cpio
	gzip -9 -c $(BUILD_DIR)/ramdisk.cpio > $(BUILD_DIR)/ramdisk.cpio.gz

# Boot image: original kernel + our ramdisk, repacked with the correct AOSP
# `id` hash (see tools/mkbootimg.py) and padded to the boot partition size.
bootimg: ramdisk
	python3 tools/mkbootimg.py pack \
		--kernel $(KERNEL) \
		--ramdisk $(BUILD_DIR)/ramdisk.cpio.gz \
		--header-from $(BOOT_ORIG) \
		-o $(BUILD_DIR)/mmkeypad_boot.img

# ── OTA bundle ──────────────────────────────────────────────────────────────
# The T3's Releases artifact is NOT a flashable image: it is a flat tar holding
# the app and the init overlay, applied by platform/nv_ota_t3.c as an overlay
# swap. That file extracts with `busybox tar -xf` and then looks for exactly
# these members at the top level:
#
#   meta.json      must contain "t3-bundle" and "version": N, 1 <= N <= T3_BUNDLE_MAX
#   mmkeypad       the app       -> /data/mmkeypad
#   init.overlay   the init      -> /data/init.overlay  (triggers a reboot if changed)
#
# Both payload members are optional to the updater, but a release should carry
# both. Version comes from the shared scheme in ../firmware-idf/version.txt so the
# tag, the asset name and what the panel reports all agree.
#
#   make bundle                 -> build/mmk-t3-<version>.tar
#
# NOTE: the app must be built first (cd lvgl-app && make); it is a separate tree.
BUNDLE_VER := $(shell sed 's/FW$$//' ../firmware-idf/version.txt 2>/dev/null)
BUNDLE_APP := lvgl-app/build/mmk-app
BUNDLE_OUT := $(BUILD_DIR)/mmk-t3-$(BUNDLE_VER).tar

bundle: init
	@test -n "$(BUNDLE_VER)" || { echo "no version (../firmware-idf/version.txt)"; exit 1; }
	@test -f $(BUNDLE_APP) || { echo "missing $(BUNDLE_APP) — run: cd lvgl-app && make"; exit 1; }
	rm -rf $(BUILD_DIR)/bundle && mkdir -p $(BUILD_DIR)/bundle
	cp $(BUNDLE_APP)        $(BUILD_DIR)/bundle/mmkeypad
	cp $(BUILD_DIR)/init    $(BUILD_DIR)/bundle/init.overlay
	chmod 755 $(BUILD_DIR)/bundle/mmkeypad $(BUILD_DIR)/bundle/init.overlay
	printf '{\n  "type": "t3-bundle",\n  "version": 1,\n  "fw": "%s",\n  "sku": "mmk-t3"\n}\n' \
		"$(BUNDLE_VER)" > $(BUILD_DIR)/bundle/meta.json
	# COPYFILE_DISABLE stops macOS tar adding ._ AppleDouble members, which
	# busybox tar would extract as junk alongside the real files.
	cd $(BUILD_DIR)/bundle && COPYFILE_DISABLE=1 tar -cf ../$(notdir $(BUNDLE_OUT)) \
		meta.json mmkeypad init.overlay
	@echo "built $(BUNDLE_OUT)"
	@tar -tf $(BUNDLE_OUT)

clean:
	rm -rf $(BUILD_DIR)
	rm -f $(ROOTFS_DIR)/init
