# MMKeypad

A touchscreen **now-playing display, media controller, and SIP intercom
endpoint for Control4**, running on off-the-shelf ESP32 and ARM touch panels.
Shows what's playing in a selected room with cover art, provides transport /
volume / source controls and programmable buttons, and joins Control4's native
intercom as a standards-based SIP endpoint — all through a custom **Control4
DriverWorks `.c4z` driver**.

This repository is the open part of the project: the device firmware, the
Control4 drivers, and the protocol specification.
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

## What is not in this repository

This is the open part of a product that also has a closed part. Deliberately
absent:

- **Control4's own SDK material.** The DriverWorks intercom proxy templates that
  `driver-intercom` builds against are Copyright Control4 Corporation and are not
  redistributable. You supply them from the SDK.
- **Reverse-engineering notes on Control4 internals**, device teardown material,
  and stock vendor firmware images.
- **The nuvoxel cloud backend** — device licensing, the OTA manifest service, and
  the account roster the agent driver reads. The protocol it speaks is
  documented in [OTA.md](OTA.md); the implementation is not open.

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
