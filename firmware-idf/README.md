# MMKeypad — native ESP-IDF firmware


> **Open build — standalone.** This firmware is not license-gated and talks to no
> online service. Identity, entitlement, and OTA-apply are provided locally by
> `main/nv_open.c`, `main/nv_identity_open.c`, and `main/nv_ota_open.c`; there is
> no cloud check-in. It connects to Control4 purely over the local network
> (`:6700` + SDDP).

This is the firmware tree — native **ESP-IDF + LVGL**, one FreeRTOS scheduler,
no Arduino core underneath. It implements the now-playing/keypad UI, the
protocol client + SDDP discovery, cover-art decode, and (where the board has
audio) the SIP/RTP intercom stack (`esp_media_protocols`), the ES8311 codec
driver (`esp_codec_dev`), and on-device AEC (`esp-sr`).

ESP-IDF **v5.4** (`~/esp/esp-idf`). Board: lcdwiki 2.8" ESP32-S3 (ESP32-S3-WROOM-1
N16R8) is the reference board; see the boards table below for the full set.
Pinmap in [`main/board.h`](main/board.h) — the single source of truth for pins,
per-board sections.

## Boards / targets

One source tree builds multiple boards, selected by `CONFIG_IDF_TARGET`.
[`main/board.h`](main/board.h) maps each target to a pinmap + **feature flags**
(`MMK_HAS_DISPLAY` / `MMK_HAS_TOUCH` / `MMK_HAS_AUDIO` / `MMK_NET_WIFI` /
`MMK_NET_ETH`); `main/CMakeLists.txt` and `main/idf_component.yml` compile in/out
the matching sources and managed components.

| Target    | Board (`board.sh` alias)       | Net      | Display | Audio        |
|-----------|--------------------------------|----------|---------|--------------|
| `esp32s3` | lcdwiki 2.8" (ILI9341 + FT6336) — `s3` | WiFi | yes | ES8311 + FM8002E + SIP |
| `esp32s3` | ESP32-S3 + 64×64 HUB75 LED matrix — `matrix` | WiFi | 64×64 art-only | none |
| `esp32p4` | Waveshare ESP32-P4-POE-ETH-NH — `poe` | Ethernet | headless| ES8311 + NS4150B + SIP |
| `esp32p4` | Waveshare ESP32-P4-NANO KIT-D (10.1" 800×1280 DSI) — `nano` | Ethernet (PoE) + WiFi | yes | ES8311 + ES7210 (dual-mic + HW AEC) + SIP |
| `esp32p4` | Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3 (480×800 portrait DSI) — `ws43` | WiFi (onboard ESP32-C6, esp_hosted) | yes | ES8311 + ES7210 (dual-mic + HW AEC) + SIP |

The **`matrix`** target is an "art-only" SKU: it reuses the whole net/protocol/
album-art pipeline but renders the now-playing cover art to a 64×64 HUB75 RGB LED
matrix (esphome/esp-hub75 DMA driver) instead of an SPI LCD — no touch, no audio.
Only the display layer (`main/bsp_matrix*.{cpp,c}`) and a slim UI (`main/ui_matrix.c`)
differ from the S3 keypad. Pinout, power, and bring-up notes:
[`BOARD-s3_matrix.md`](BOARD-s3_matrix.md). Build: `./board.sh matrix build`.

The P4 build is a **headless wired node**: Ethernet (internal EMAC + IP101 RMII
PHY, see [`main/eth.c`](main/eth.c)) replaces WiFi, and the display/touch/UI/LVGL
stack is compiled out. It runs the protocol server (`:6700`), SDDP discovery, the
settings web/OTA server (`:80`), and the **ES8311 audio + SIP intercom** path.
Audio pins differ from the S3 (see [`main/board.h`](main/board.h)): the codec gets
its **own** I2C bus (GPIO7/8 — no touch bus to share) and the NS4150B amp is
**active-high** on GPIO53 (the S3's FM8002E is active-low).

## Build / flash

Per-board config lives in `sdkconfig.defaults.<target>` (auto-applied on top of the
neutral `sdkconfig.defaults`). The live `sdkconfig` is gitignored and regenerated
per board. **Always build via `./board.sh <s3|poe|nano|ws43|matrix> build`** —
each board needs its own `MMK_BOARD`, target, and build dir / sdkconfig (they'd
otherwise clobber each other's build); a bare `idf.py build` fails.

```sh
. ~/esp/esp-idf/export.sh                 # once per shell (resets cwd)
cd firmware-idf

./board.sh s3 build                       # lcdwiki 2.8" display keypad
./board.sh s3 -p /dev/cu.usbmodemXXXX flash monitor

./board.sh poe build                      # P4 PoE headless node
./board.sh poe -p /dev/cu.usbmodemYYYY flash monitor

./board.sh nano build                     # P4-NANO 10.1" DSI kit
./board.sh ws43 build                     # P4 WiFi6 4.3" DSI panel
./board.sh matrix build                   # S3 + HUB75 LED matrix (art-only)
```

**Console.** The S3 board uses its **USB-Serial-JTAG** console; the P4-POE board
flashes/logs over an onboard **USB-UART bridge** (so its console is plain UART0).
Each is set in the per-target defaults.

**First S3 IDF flash may need download mode.** If the unit still runs the Arduino
firmware (native USB-OTG / TinyUSB), `idf.py flash` may not sync — hold **BOOT**,
tap **RESET**, release BOOT, then flash. After the first IDF flash it resets cleanly.

## Layout
- [`main/bsp.c`](main/bsp.c) — display + touch + `esp_lvgl_port` bring-up. Carries
  the **exact ILI9341V vendor init** (power/gamma) from the proven Arduino build,
  plus BGR + invert + landscape `swap_xy`.
- [`main/main.c`](main/main.c) — `app_main`: builds the Stage-1 UI.
- [`partitions.csv`](partitions.csv) — spike layout; reserves a `model` region for
  esp-sr. Switches to dual-OTA for production.

## Panel + touch bring-up notes (resolved on hardware)
- **Display:** the panel is native 240x320 portrait. Driving landscape via the HW
  `swap_xy` (MADCTL MV) sheared the image, so we drive it **native + software
  rotation** (`sw_rotate` + `lv_display_set_rotation(_90)`), with `mirror_y` to fix
  the left/right flip. Colors are BGR + inverted (IPS). See `bsp.c`.
- **Touch:** small **direct FT6336 driver** in `bsp.c` (reset + 300ms + read reg
  0x02). The generic `esp_lcd_touch_ft5x06` driver writes FT5x06-specific config
  registers that stop the FT6336 scanning — do not use it here. Landscape mapping
  is calibrated from corner taps (`TX_R*` constants); refine if taps drift.
- Backlight is GPIO45 driven HIGH (full brightness). LEDC dimming comes with the
  settings port (porting stage).
