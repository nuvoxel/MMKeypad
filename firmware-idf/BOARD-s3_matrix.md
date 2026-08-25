# Board: `s3_matrix` — ESP32-S3 + 64×64 HUB75 LED matrix (album-art display)

An "art-only" SKU of the MMKeypad firmware. It reuses the entire driver/protocol/
album-art pipeline but renders the now-playing **cover art to a 64×64 RGB LED
matrix** instead of an SPI LCD. No touch, no audio — just the art (plus whatever
you add). The Control4 driver needs **zero changes**: this board is just another
TCP client speaking [PROTOCOL.md](../PROTOCOL.md); the driver hands it the same
`artUrl`, and the device fetches + decodes it exactly as the keypad does.

## What's shared vs. new

| Layer | Source | Status |
|-------|--------|--------|
| Protocol / TCP client / reconnect | `net.c` | **reused as-is** |
| Album-art fetch (HTTPS) + JPEG/PNG decode + scale | `art.c` | **reused as-is** (scaled into a 64×64 canvas) |
| Wi-Fi + softAP captive-portal onboarding | `wifi.c`, `web.c` | **reused as-is** |
| Config / OTA / SDDP discovery | `config.c`, `web.c`, `sddp.c` | **reused as-is** |
| Display bring-up | **`bsp_matrix.cpp`** | new — HUB75 DMA driver + a 64×64 LVGL display |
| UI | **`ui_matrix.c`** | new — full-screen art canvas + stub hooks to extend |

The HUB75 driver is [`esphome/esp-hub75`](https://components.espressif.com/components/esphome/esp-hub75)
(MIT, native ESP-IDF, GDMA on the S3), pulled as a managed component.

## Pinout (proposed default — rewire freely)

A **generic ESP32-S3-WROOM-1 N16R8 devkit**, *not* the lcdwiki keypad board.
Single source of truth is [`main/board.h`](main/board.h) (the `MMK_BOARD_S3_MATRIX`
block); this table must stay in sync with it.

A HUB75 header is a 2×8 (16-pin) IDC connector. 64×64 panels are **1/32 scan**, so
all five address lines **A–E** are required (a common mistake: leaving E floating —
that gives you a squashed / doubled image).

| HUB75 signal | ESP32-S3 GPIO | Notes |
|--------------|:-------------:|-------|
| R1 | 1  | upper-half red |
| G1 | 2  | upper-half green |
| B1 | 42 | upper-half blue |
| R2 | 41 | lower-half red |
| G2 | 40 | lower-half green |
| B2 | 39 | lower-half blue |
| A  | 38 | row addr bit 0 |
| B  | 48 | row addr bit 1 |
| C  | 47 | row addr bit 2 |
| D  | 21 | row addr bit 3 |
| E  | 14 | row addr bit 4 (**required** for 64-high) |
| CLK | 18 | pixel clock |
| LAT (STB) | 17 | latch |
| OE | 16 | output enable (active low) |
| GND | GND | **tie the panel's GND to the S3 GND** (multiple GND pins on the header) |

GDMA routes through the S3 GPIO matrix, so **any** free GPIO works. When picking
your own, avoid: strapping pins **0/3/45/46**, USB **19/20**, and the octal
PSRAM/flash pins **26–37** (reserved on the N16R8 module). UART **43/44** too.

### Power (the real gotcha)

- Drive the panel's **5 V rail from a dedicated 5 V supply**, not the S3's USB.
  A 64×64 panel can pull **~3–4 A at full white**; USB can't source that and
  you'll get brownouts / color shift / resets.
- **Common ground** between that 5 V supply and the ESP32-S3 is mandatory.
- The HUB75 logic is 5 V but tolerates 3.3 V drive on most panels; if you see
  ghosting or dim upper/lower halves, add a 3.3→5 V level shifter (74HCT245) on
  the 14 signal lines.
- The firmware caps brightness via the driver's intensity (mapped from the
  Control4 "brightness" setting). Album art is rarely full-white, so typical
  draw is far below the worst case — but size the PSU for worst case anyway.

## Build & flash

```sh
source ~/esp/esp-idf/export.sh
cd firmware-idf
./board.sh matrix build
./board.sh matrix -p /dev/cu.usbmodem* flash monitor
```

`board.sh matrix` uses its own build dir (`build.matrix`) + `MMK_BOARD=s3_matrix`,
so it never clobbers the other boards. (A bare `idf.py` build **must**
`export MMK_BOARD=s3_matrix` first, or the component manager won't pull `hub75`.)

## First-boot bring-up checklist

If the panel misbehaves, these three `board.h` knobs cover the usual causes:

1. **Byte-garbled / wrong colors** → flip `MATRIX_RGB565_BE` (0↔1). esp_lvgl_port
   renders byte-swapped RGB565; the default (1) matches that. This is a byte-order
   swap, not an R/B swap.
2. **Panel stays dark or shows ghost columns** → flip `MATRIX_SHIFT_FM6126A`
   (0↔1). FM6126A/FM6124 panels need an init sequence; "GENERIC" panels don't.
3. **Image squashed vertically / rows doubled** → the E line isn't reaching the
   panel (check `PIN_HUB75_E` wiring). Required for 1/32-scan 64×64.

Colors look right but the top and bottom halves are offset → `scan_wiring` in
`bsp_matrix.cpp` (`STANDARD_TWO_SCAN`) doesn't match your panel; some 1/32 panels
use a different scan map. See the esp-hub75 docs for `Hub75ScanWiring` options.

## Extending it

`ui_matrix.c` shows *only* the cover art today; every other `ui.h` entry point is
a deliberate stub. The display, network, and art plumbing are already wired, so
adding features is just LVGL widgets:

- **Scrolling track title** — a `lv_label` with `LV_LABEL_LONG_SCROLL`, updated
  from `ui_set_state()` (`st->title` / `st->artist`).
- **Clock / VU meter / now-playing glyph** — LVGL objects created in `ui_begin()`.
- **Idle dim** — implement `ui_tick_screensaver()` to lower panel intensity.
- **Announcement marquee** — `ui_announce()` is stubbed; scroll `text` across.

`ui_show_setup()` already paints the panel solid amber while it's in Wi-Fi setup /
AP mode (a 64×64 panel can't show a QR usefully), and `ui_identify()` flashes it
white for the Control4 "identify" action.
