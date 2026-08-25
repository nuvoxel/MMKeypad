# MMKeypad — Linux firmware (ARM panel target)

A from-scratch Linux userspace for an ARM (RK3188, Cortex-A9) in-wall touch
panel: a static `init`, a BusyBox rootfs, Dropbear, wpa_supplicant, and an LVGL
application that shares its UI and protocol code with the ESP-IDF firmware.

The result is a single `boot.img` — the device's **stock kernel** plus **our**
ramdisk — so nothing about the kernel or its drivers is modified or
redistributed here.

## What you need that this repository does not ship

The build reuses the panel's own kernel and boot header, extracted from the
device you are building for:

    ../reference/t3-control4/firmware/extracted/kernel.img
    ../reference/t3-control4/firmware/extracted/boot.orig

That directory is **not** part of this repository — it is vendor firmware from
your own hardware, and it is neither ours to distribute nor useful to anyone with
a different unit. Create it yourself from your device's boot partition before
running `make`. `tools/flash.sh` similarly expects a Rockchip flashing tool at a
path under that directory.

Getting a panel into a state where it will boot an unsigned image is a
device-specific matter that is out of scope for this repository.

## Layout

| Path | What |
|------|------|
| `init/` | Static PID 1 — mounts, module load, framebuffer splash, service supervision. |
| `rootfs/` | The skeleton staged into the ramdisk (`etc/`, logo, DHCP script). BusyBox, Dropbear, wpa_supplicant, and the SIP stack are built into it by `tools/build-*.sh`. |
| `lvgl-app/` | The application. `shared/` (`ui.c`, `net.c`, `sip.c`, `art.c`, `config.c`, `sddp.c`) is the same code the ESP firmware builds. |
| `platform/` | The Linux backing for the ESP-IDF APIs `shared/` calls — plus `compat/` headers that let the shared sources compile unchanged off ESP-IDF. |
| `sim/` | **Headless UI simulator.** Renders the real `ui.c` to PNGs on a desktop, at every panel resolution, with no hardware. Start here. See [`sim/README.md`](sim/README.md). |
| `tools/` | Cross-build scripts for the third-party userspace, `mkcpio.py` / `mkbootimg.py`, and bring-up utilities. |
| `toolchain/` | Thin wrappers that make `zig cc` act as an `arm-linux-musleabihf` toolchain. Zig cross-compiles from macOS with no sysroot download. |
| `reference/` | Captured RT3261 audio-codec mixer/register states used to derive the working routes. |

## Build

Requires `zig`, `python3`, and a network connection on first run (the
`tools/build-*.sh` scripts fetch their sources).

    make bootimg        # -> build/mmkeypad_boot.img

## Licenses

Our code here is Apache-2.0. The image you build also contains third-party
software with its own terms — notably **BusyBox, which is GPL-2.0**. If you
distribute a built image you take on those obligations. See the repository
[NOTICE](../NOTICE).
