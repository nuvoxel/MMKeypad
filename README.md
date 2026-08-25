# MMKeypad

A touchscreen **now-playing display, media controller, and SIP intercom
endpoint for Control4**, running on off-the-shelf ESP32 and ARM touch panels.
Shows what's playing in a selected room with cover art, provides transport /
volume / source controls and programmable buttons, and joins Control4's native
intercom as a standards-based SIP endpoint — all through a custom **Control4
DriverWorks `.c4z` driver**.

This repository is the open part of the project: the device firmware, the
Control4 drivers, the protocol specification, and the hardware documentation
(supported ESP32 boards and a full Control4 T3 panel teardown).
Licensed under the **Apache License 2.0** — see [Licensing](#licensing).

UX inspired by [`esphome-media-player`](https://github.com/jtenniswood/esphome-media-player)
and [`esphome-intercom`](https://github.com/n-IA-hane/esphome-intercom), but
driven entirely by Control4.

## The pieces

| Part | Path | What it is |
|------|------|------------|
| ESP32 firmware | [`firmware-idf/`](firmware-idf/) | Native ESP-IDF + LVGL, one source tree, five boards (`s3` / `poe` / `nano` / `ws43` / `matrix` via `./board.sh`). **The device is the TCP server** (`:6700`); the driver dials it. |
| Linux firmware | [`firmware-linux-t3/`](firmware-linux-t3/) | A from-scratch musl/BusyBox Linux + LVGL app for ARM panel hardware, sharing the same UI and protocol code as the ESP build. Includes a **headless simulator** that renders the real UI to PNGs on a desktop. |
| Keypad driver | [`driver-keypad/`](driver-keypad/) | DriverWorks Lua `.c4z` — multi-proxy: primary `keypad` proxy (now-playing, buttons, LEDs) plus an `intercomproxy` sub-proxy, sharing one device connection. |
| Agent driver | [`driver-agent/`](driver-agent/) | DriverWorks Lua agent — one per project; installs and configures the per-device drivers from an account roster. |
| Intercom driver | [`driver-intercom/`](driver-intercom/) | Standalone intercom endpoint driver. **Requires files from Control4's DriverWorks SDK that are not in this repo** — see [`driver-intercom/README.md`](driver-intercom/README.md). |
| Removal tool | [`print/T3_RemovalTool/`](print/T3_RemovalTool/) | A printable (or laser-cut) spudger for releasing a T3 panel from its wall bracket — a service aid. OpenSCAD source plus a generator script. |
| Supported hardware | [`docs/HARDWARE.md`](docs/HARDWARE.md) | The ESP32 boards the firmware targets, with vendor links, plus UI screenshots at each panel size. |
| T3 reference | [`reference/t3-control4/`](reference/t3-control4/) | Full teardown of the Control4 T3 touch panel — SoC, audio, display, camera, the two variants, and how to run our firmware on one you own. |

## Documentation

- **[PROTOCOL.md](PROTOCOL.md)** — the wire contract. One JSON object per line
  over TCP; the device listens on `:6700` and the driver connects out.
- **[ARCHITECTURE.md](ARCHITECTURE.md)** — design rationale and phasing.
- **[OTA.md](OTA.md)** — the firmware update control plane (check-in → manifest →
  verify → apply → rollback), for both ESP native A/B and the Linux split-init
  overlay path.
- **[PHASE3-INTERCOM-SIP.md](PHASE3-INTERCOM-SIP.md)** — how the SIP intercom
  endpoint works.
- **[HA-INTEGRATION.md](HA-INTEGRATION.md)** — a scoped Home Assistant
  integration path as an alternative to Control4.
- **[END-TO-END-TEST.md](END-TO-END-TEST.md)** — bring-up checklist.

## Building

**ESP32 firmware** needs ESP-IDF v5.4. Managed components (LVGL, `esp_codec_dev`,
`esp_media_protocols`, `esp-sr`) are fetched by the build from
`main/idf_component.yml`; they are not vendored here.

```sh
cd firmware-idf
./board.sh s3 build          # or: poe | nano | ws43 | matrix
./board.sh s3 flash monitor
```

See [`firmware-idf/README.md`](firmware-idf/README.md) for the board table and
the pinmap in [`main/board.h`](firmware-idf/main/board.h).

**Linux firmware** cross-compiles with Zig standing in for a musl toolchain, and
builds BusyBox / Dropbear / wpa_supplicant / libre from source. See
[`firmware-linux-t3/README.md`](firmware-linux-t3/README.md) — it has
prerequisites this repo does not ship.

**Drivers** are packaged by `build.sh` in each `driver-*/` directory, which
produces the `.c4z` and drops it in `~/Documents/Control4/Drivers`. Built `.c4z`
files are build artifacts and are not committed.

## Flashing a device and connecting it to Control4

The firmware in this repository is **standalone**: it talks to no online service,
is not license-gated, and connects to a Control4 system entirely over the local
network. There is no account, no activation, and no cloud check-in — a freshly
flashed unit boots straight to a working keypad.

### 1. Flash the firmware

**ESP32 boards** (see [`docs/HARDWARE.md`](docs/HARDWARE.md) for which board is
which):

```sh
cd firmware-idf
./board.sh s3 flash monitor      # or: poe | nano | ws43 | matrix
```

**Control4 T3 panel** — build and flash the Linux image onto a panel you own; you
supply its own kernel and a couple of tools. Full procedure:
[`firmware-linux-t3/README.md`](firmware-linux-t3/README.md), with the teardown,
access method, and variants in [`reference/t3-control4/`](reference/t3-control4/).

On boot the device runs the protocol server on `:6700` and announces itself over
**SDDP**, so a Control4 Director on the same network discovers it automatically.

### 2. Add the driver in Composer Pro

```sh
cd driver-keypad && ./build.sh   # -> NuVoxelKeypad.c4z, copied to ~/Documents/Control4/Drivers
```

Then in **Composer Pro**: the keypad advertises over SDDP, so add it from
discovered devices — or **System Design → Search → Add Driver**, place it in the
keypad's room, and bind its **Media Keypad Network** connection to the device.
That is the whole connection: the driver dials the device's `:6700`, reads the
room's now-playing state, and relays transport / volume / source. See
[`driver-keypad/README.md`](driver-keypad/README.md).

> The [`driver-agent/`](driver-agent/) driver (account roster / auto-install) is
> part of the hosted service and is **not** needed for a standalone install —
> add the keypad driver directly as above.

For the SIP intercom endpoint, also build `driver-intercom` (it needs Control4
SDK files — see [`driver-intercom/README.md`](driver-intercom/README.md)).

### Updating firmware

The open build does not auto-update from a cloud. Instead, on the panel go to
**Settings → Check for update**: it lists the firmware images published on this
project's [GitHub Releases](https://github.com/nuvoxel/MMKeypad/releases) for the
board's SKU, and applies the one you pick (the panel downloads it and restarts).
Local **USB flashing** (`./board.sh <board> flash`, or the T3 flash tooling) also
works and is the fallback if a panel has no network route.

## What is not in this repository

This is the open part of a product that also has a closed part. Deliberately
absent:

- **Control4's own SDK material.** The DriverWorks intercom proxy templates that
  `driver-intercom` builds against are Copyright Control4 Corporation and are not
  redistributable. You supply them from the SDK.
- **Control4-proprietary binaries.** The teardown and RE notes are published
  (see [`reference/t3-control4/`](reference/t3-control4/)), but the extracted
  Control4 firmware images, APKs, and `/system` contents are not redistributed —
  supply those from a unit you own.
- **The nuvoxel hosted service** — the account portal, device licensing, and the
  roster the agent driver reads. The open firmware does not use or need it (it is
  not license-gated and never phones home); it is simply the closed half of the
  commercial product. [OTA.md](OTA.md) documents the hosted OTA design for
  reference; the open build updates locally and from GitHub Releases instead.

## Licensing

Licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE) for the
full text and [NOTICE](NOTICE) for third-party attributions.

You may use, modify, and redistribute this — including commercially — provided
you keep the license and NOTICE, state your changes, and do not use the project's
trademarks. Apache 2.0 also grants you a patent license from contributors.

Note that some components the build *fetches* are copyleft. In particular, a
built Linux image contains **BusyBox (GPL-2.0)**; distributing that image carries
GPL source-availability obligations that are yours to meet, not this project's.
See [NOTICE](NOTICE).

Contributions are accepted under the same license (Apache 2.0 §5).

---

Control4, Composer, DriverWorks, and Navigator are trademarks of their respective
owners. This project is not affiliated with, endorsed by, or supported by
Control4, Snap One, or Resideo.
