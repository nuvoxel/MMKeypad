#pragma once
#include "sdkconfig.h"   // CONFIG_IDF_TARGET_* — must be visible before the checks below
// MMKeypad board pinmap + feature flags. One source tree, multiple boards. The
// board is chosen by a MMK_BOARD_<NAME> compile define passed from CMake (via
// -D MMK_BOARD=<name>); it defaults to the S3 lcdwiki keypad when none is set.
//   s3_lcdwiki  — lcdwiki 2.8" ESP32-S3 (SPI ILI9341 + FT6336), WiFi, audio
//   p4_poe_eth  — Waveshare ESP32-P4-POE-ETH-NH (headless, Ethernet, audio)
//   p4_nano     — Waveshare ESP32-P4-NANO KIT-D (10.1" 800x1280 DSI + PoE + audio)
//   ws43        — Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3 (480x800 ST7701 DSI + GT911)
//
// Feature flags each board exposes (used to compile in/out subsystems):
//   MMK_HAS_DISPLAY  LVGL display + panel + UI screens
//   MMK_HAS_TOUCH    cap touch (implies an I2C bus for the controller)
//   MMK_HAS_AUDIO    codec/amp + I2S + SIP intercom
//   MMK_NET_WIFI     join via esp_wifi (native, or esp_wifi_remote over a C6)
//   MMK_NET_ETH      join via the internal EMAC + external RMII PHY

// ─────────────────────────────────────────────────────────────────────────────
#if defined(MMK_BOARD_P4_NANO)
// ── Waveshare ESP32-P4-NANO (KIT-D: PoE module + 10.1" 800x1280 DSI touch) ───
// The wired-flagship dev unit. Display/touch/backlight come from Waveshare's
// OFFICIAL BSP component (waveshare/esp32_p4_nano): JD9365 10.1" over 2-lane
// DSI, GT911 on the FPC's I2C, backlight via an I2C controller @0x45 — all
// selected by the BSP's Kconfig (defaults to the KIT-D 800x1280 panel).
// bsp_nano.c is only a thin shim onto our bsp.h API.
// Audio is chip- and PIN-identical to the P4-POE-ETH: ES8311 @0x18 + NS4150B,
// but the codec SHARES the BSP's touch I2C bus (audio.c display-board path).
// Ethernet (IP101GRI) + PoE is the point of this kit — enable after first
// display bring-up. ESP32-C6 (WiFi) present too; later.
#define MMK_BOARD_NAME   "p4-nano"
#define MMK_HAS_DISPLAY  1
#define MMK_HAS_TOUCH    1
#define MMK_HAS_AUDIO    1
#define MMK_NET_WIFI     0   // C6 over SDIO exists — bring up after wired path
#define MMK_NET_ETH      1   // IP101GRI + PoE module (KIT-D) — CONFIRMED working
#define MMK_WIFI_MAC_OVERRIDE 0

// 10.1" KIT-D panel (BSP Kconfig: BSP_LCD_TYPE_800_1280_10_1_INCH) — native
// portrait 800x1280, but the 10" is always mounted LANDSCAPE. Rotate the LVGL
// display 90° so it renders 1280x800 (which also enables the X4 card UI).
#define LCD_WIDTH        800
#define LCD_HEIGHT       1280
#define MMK_DISPLAY_ROTATE_90  1

// 12 programmable buttons. Rendered 1280x800 (landscape after the 90° rotate), so
// layoutKeypad() uses 4 columns → 12 is exactly 3 full rows. At that size each tile
// is ~290x150 px on a 10.1" panel (~55x28 mm) — still far above a comfortable touch
// target, and 3 rows leave the header gap intact. This is the "wall panel that
// replaces a bank of keypads" SKU, so it gets the biggest grid we lay out cleanly.
#define MMK_MAX_BUTTONS  12

// Ethernet (IP101GRI over RMII; EMAC default pins as in eth.c). PHY reset GPIO +
// addr are identical to the POE-ETH sibling — VERIFIED on the NANO (link + DHCP).
#define ETH_PHY_RST_GPIO 51
#define ETH_PHY_ADDR     1

// Audio: ES8311 + NS4150B, same pins as P4-POE-ETH, codec on the BSP's shared
// I2C bus (no PIN_AUDIO_SDA/SCL here → audio.c uses bsp_i2c_bus()).
#define ES8311_I2C_ADDR  0x18
#define AUDIO_I2C_PORT   0
#define PIN_I2S_MCLK     13
#define PIN_I2S_BCLK     12
#define PIN_I2S_LRCLK    10
#define PIN_I2S_DOUT     9    // ESP -> codec DSDIN (speaker)
#define PIN_I2S_DIN      11   // codec ASDOUT -> ESP (mic)
#define PIN_AMP_ENABLE   53   // NS4150B PA_CTRL, drive HIGH to enable
#define AMP_ACTIVE_LOW   0

// ─────────────────────────────────────────────────────────────────────────────
#elif defined(MMK_BOARD_P4_POE_ETH)
// ── Waveshare ESP32-P4-POE-ETH-NH ───────────────────────────────────────────
// RISC-V P4, 32MB PSRAM/flash in-package, 100M Ethernet (IP101 PHY) with PoE.
// No WiFi radio, no screen on this unit — a wired, headless intercom/keypad node.
#define MMK_BOARD_NAME   "p4-poe-eth"
#define MMK_HAS_DISPLAY  0
#define MMK_HAS_TOUCH    0
#define MMK_HAS_AUDIO    1   // ES8311 + NS4150B onboard (pins confirmed from schematic)
#define MMK_NET_WIFI     0
#define MMK_NET_ETH      1
// No MMK_MAX_BUTTONS override on purpose: headless (MMK_HAS_TOUCH 0), and device.c
// already reports caps.buttons = 0 for a touchless board regardless of this value.
// The common fallback below just keeps the net.h receive array a legal size.

// Ethernet (IP101 over RMII). The pins below are documentation only — they exactly
// match ESP-IDF's ETH_ESP32_EMAC_DEFAULT_CONFIG() for the P4, which eth.c uses
// verbatim: MDC=31 MDIO=52 REF_CLK(EXT_IN)=50 TX_EN=49 TXD0=34 TXD1=35
// CRS_DV=28 RXD0=29 RXD1=30. PHY reset GPIO and address are board-specific:
#define ETH_PHY_RST_GPIO 51
#define ETH_PHY_ADDR     1

// Audio: ES8311 codec (I2C 0x18) + NS4150B amp. Pins confirmed from the
// ESP32-P4-ETH datasheet + WIFI6-POE-ETH schematic. No touch on this board, so
// audio.c creates its OWN I2C master bus on these pins (the S3 shares the touch bus).
#define ES8311_I2C_ADDR  0x18
#define PIN_AUDIO_SDA    7    // ESP_I2C_SDA (ES8311 CDATA)
#define PIN_AUDIO_SCL    8    // ESP_I2C_SCL (ES8311 CCLK)
#define AUDIO_I2C_PORT   0
#define PIN_I2S_MCLK     13
#define PIN_I2S_BCLK     12
#define PIN_I2S_LRCLK    10
#define PIN_I2S_DOUT     9    // ESP -> codec DSDIN (speaker)
#define PIN_I2S_DIN      11   // codec ASDOUT -> ESP (mic)
#define PIN_AMP_ENABLE   53   // NS4150B PA_CTRL
#define AMP_ACTIVE_LOW   0    // NS4150B: drive HIGH to ENABLE the amp
// ES8311 CE is tied high via 10K on-board (always enabled) — no GPIO needed.

// ─────────────────────────────────────────────────────────────────────────────
#elif defined(MMK_BOARD_WS43)
// ── Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3 (480x800 portrait DSI) ────────────
// 2-lane MIPI-DSI to an ST7701 panel controller (espressif/esp_lcd_st7701, DPI
// video mode), GT911 cap touch on a shared I2C bus, backlight on a DIRECT P4
// GPIO (LEDC PWM, INVERTED output — see bsp_ws43.c). ES8311 + ES7210 audio share
// the touch I2C bus (codecs on the same bus as p4_nano; audio.c display-board
// path). WiFi is an onboard ESP32-C6 (esp_hosted) — left out until the display
// path is proven. Pins/timings/ST7701 init cmds all lifted from Waveshare's
// factory BSP (esp32_p4_wifi6_touch_lcd_4_3), the proven bring-up for this unit.
#define MMK_BOARD_NAME   "ws43"
#define MMK_HAS_DISPLAY  1
#define MMK_HAS_TOUCH    1
#define MMK_HAS_AUDIO    1   // ES8311 (speaker) + ES7210 (dual-mic + HW AEC).
                             // I2S/codec pins below VERIFIED vs Waveshare's
                             // factory BSP (esp32_p4_wifi6_touch_lcd_4_3.[ch]).
#define MMK_NET_WIFI     1   // onboard ESP32-C6 via esp_hosted 1.4.x (Waveshare
                             // factory stack); wifi.c provisions over softAP
#define MMK_NET_ETH      1   // backbox-poe carrier: IP101GA over RMII through J1.
                             // Absent carrier, eth_start() logs and returns.
#define MMK_WIFI_MAC_OVERRIDE 0

// BLE Wi-Fi provisioning (prov.c): the phone hands over Wi-Fi creds over BLE via
// Espressif's wifi_provisioning manager (NimBLE BLE scheme, C6 controller over
// the esp_hosted VHCI). Runs ALONGSIDE the softAP captive portal — the user can
// provision with the ESP BLE Provisioning app OR the browser portal, whichever
// they prefer; the on-screen QR carries the BLE payload. Pulls the
// network_provisioning component + adds prov.c to the srcs (see main/CMakeLists.txt).
// The S3 also defines this (native on-chip BLE); the ws43 additionally sets
// MMK_BLE_HOSTED because its controller is REMOTE on the C6 (needs the 2.12.x C6
// slave WITH BT, flashed via MMK_C6_OTA) — prov.c brings that up manually, whereas
// the S3's on-chip controller is started by nimble_port_init itself.
#define MMK_HAS_BLE_PROV 1
#define MMK_BLE_HOSTED   1   // BT controller is on the C6 over esp_hosted VHCI (not on-chip)
#define MMK_CAN_ROTATE   1   // DSI sw_rotate → show the Orientation setting
// ── backbox-poe carrier ─────────────────────────────────────────────────────
// The carrier is purely additive to the ws43 (Ethernet + a 24-LED halo ring),
// so there is ONE board target for both. On a bare module the PHY simply does
// not answer and the halo clocks data into nothing.
//
// IP101GA over RMII. ESP-IDF's P4 ETH_ESP32_EMAC_DEFAULT_CONFIG() already
// encodes this pinout (MDC=31 MDIO=52 CLK_EXT_IN=50 TX_EN=49 TXD0=34 TXD1=35
// CRS_DV=28 RXD0=29 RXD1=30), so eth.c uses it verbatim.
#define ETH_PHY_RST_GPIO 51      // J1.36
#define ETH_PHY_ADDR     1       // AD0 pulled high, AD1/AD2 low

// Halo: 24x SK6812SIDE-A around the perimeter, driven from GPIO48 through a
// 74AHCT level shifter. AHCT is TTL-threshold (VIH 2.0V at 5V VCC), and GPIO48
// sits on the GPIO39-48 LDO domain — IO_LDO_CHAN must be acquired before use or
// the pin cannot reach a valid logic high and the ring stays dark.
#define PIN_RGB_LED      48
#define HALO_COUNT       24
// Scale the requested brightness down on a ring. The Control4 driver's Halo
// Brightness percentage was chosen when the halo was a SINGLE onboard LED; 24 px
// emit far more light for the same number, on a wall, often in a dark room. This
// rescales rather than changing the driver default so it applies to projects
// that already store a value. 50 => the driver's 100% is half power.
#define HALO_BRIGHT_SCALE 50

#define MMK_SNAPSHOT     0   // OFF: on the 480x800 panel the framebuffer-over-serial dump starves LVGL -> task WDT reboot loop. Dev-only; flip to 1 on the bench.

// C6 slave OTA (bench only): flip to 1 to run a ONE-SHOT OTA of the onboard
// ESP32-C6 esp_hosted slave from the `model` partition on the next boot (see
// c6_ota.c). Needed once to move the factory (BT-less) C6 slave to a matching
// 2.12.x slave WITH the BT controller, so NimBLE's esp_hosted_bt_controller_init()
// stops timing out on Req_FeatureControl (0x183). LEAVE UNDEFINED for normal
// builds — when defined, app_main streams the slave image + reboots the C6.
// #define MMK_C6_OTA 1

// BLE bring-up spike (advertise-only) via the C6 over esp_hosted VHCI. Needs the
// 2.12.x C6 slave with BT (flashed via MMK_C6_OTA). Set to 1 to advertise
// "MMKeypad-<mac>" over BLE; leave off for normal builds until wifi_provisioning
// lands. (Was proven to fit memory alongside WiFi+audio+display.)
// #define MMK_BLE_SPIKE 1

// Display: native portrait 480x800 over 2-lane MIPI-DSI @ 500 Mbps/lane, DPI
// (pixel) clock 30 MHz, RGB565. The DSI PHY is powered from on-chip LDO channel 3
// @ 2.5V (VDD_MIPI_DPHY = LDO_VO3) — must be acquired before DSI init or dark.
#define LCD_WIDTH        480
#define LCD_HEIGHT       800

// 8 programmable buttons. Native portrait 480x800, so layoutKeypad() uses 2 columns
// → 8 is 4 full rows. Tiles come out ~210x150 px (~29x21 mm on a 4.3" panel), which
// is a generous touch target, and 4 rows still fit under the header on 800 px of
// height.
#define MMK_MAX_BUTTONS  8

#define DSI_LANES        2
#define DSI_LANE_MBPS    500
#define DSI_DPI_CLK_MHZ  30
#define DSI_PHY_LDO_CHAN 3      // esp_ldo channel powering the MIPI D-PHY

// GPIO39-48 on the P4 are NOT on the main 3.3V IO rail — they sit on their own
// internal LDO domain (LDO_VO4 on the ws43, where it also feeds the microSD
// pull-ups). Unacquired it idles around 1.2V, so any of those pins used as an
// output cannot reach a valid 3.3V logic high, and anything downstream of them
// silently does nothing. 3.3V is also the correct level for the SD domain
// (1.8V is the optional UHS-I mode only).
#define IO_LDO_CHAN      4      // esp_ldo channel powering GPIO39-48
#define IO_LDO_MV        3300
#define DSI_PHY_LDO_MV   2500
// DPI video timings (from Waveshare's factory BSP for the ST7701):
#define DSI_HSYNC_PULSE  12
#define DSI_HBP          42
#define DSI_HFP          42
#define DSI_VSYNC_PULSE  8
#define DSI_VBP          2
#define DSI_VFP          60

// Backlight: direct GPIO, LEDC PWM. Factory uses 10-bit @ 5 kHz with the LEDC
// output INVERTED (handled in bsp_ws43.c). LCD reset is a direct GPIO too.
#define PIN_LCD_BL       26
#define LCD_BL_PWM_HZ    5000
#define PIN_LCD_RST      27

// Touch: GT911 over the shared I2C bus (SCL=8, SDA=7). INT is NOT connected on
// this board (GT911 polled). Touch reset on its own GPIO.
#define TOUCH_I2C_PORT   0
#define PIN_TOUCH_SDA    7
#define PIN_TOUCH_SCL    8
#define PIN_TOUCH_RST    23
#define PIN_TOUCH_INT    -1    // NC → esp_lcd_touch polls the GT911

// Audio: ES8311 (@0x18, DAC/speaker) + ES7210 (@0x40, dual-mic ADC + HW AEC ref)
// on the BSP's shared I2C bus (no PIN_AUDIO_SDA/SCL here → audio.c uses
// bsp_i2c_bus(), like p4_nano). ALL pins/addrs below VERIFIED against Waveshare's
// factory BSP esp32_p4_wifi6_touch_lcd_4_3.h (include/bsp/*.h) — they happen to
// match the p4_nano placeholders exactly (Waveshare reuses this P4 audio pinmap):
//   BSP_I2S_MCLK=13  BSP_I2S_SCLK(BCLK)=12  BSP_I2S_LCLK(WS)=10
//   BSP_I2S_DOUT=9 (ESP→codec)  BSP_I2S_DSIN(DIN)=11 (codec→ESP)
//   BSP_POWER_AMP_IO=53 (pa_reverted=false → active HIGH)  BSP_I2C_SDA=7 SCL=8
//   ES8311 = ES8311_CODEC_DEFAULT_ADDR 0x30 (7-bit 0x18)
//   ES7210 = ES7210_CODEC_DEFAULT_ADDR 0x80 (7-bit 0x40)
#define ES8311_I2C_ADDR  0x18
#define AUDIO_I2C_PORT   0
#define PIN_I2S_MCLK     13
#define PIN_I2S_BCLK     12
#define PIN_I2S_LRCLK    10
#define PIN_I2S_DOUT     9    // ESP -> codec DSDIN (speaker)
#define PIN_I2S_DIN      11   // ES7210 SDOUT -> ESP (dual mic)
#define PIN_AMP_ENABLE   53   // power-amp enable, drive HIGH to enable
#define AMP_ACTIVE_LOW   0

// This board captures on a DEDICATED ES7210 4-ch ADC (2 analog mics + hardware
// AEC reference), NOT the ES8311's own single-mic ADC like the other boards. So
// audio.c builds a two-handle esp_codec_dev: ES8311 = OUTPUT-only (DAC), ES7210 =
// INPUT (dual mic). The ES8311 MUST stay DAC-only here — if it also drove its ADC
// onto the shared I2S DIN it would collide with the ES7210. Mirrors the BSP's
// bsp_audio_codec_speaker_init()/bsp_audio_codec_microphone_init() split.
#define MMK_HAS_ES7210   1
#define ES7210_I2C_ADDR  0x40   // 7-bit (0x80 >> 1); audio.c shifts <<1 for esp_codec_dev

// ─────────────────────────────────────────────────────────────────────────────
#elif defined(MMK_BOARD_S3_MATRIX)
// ── ESP32-S3 + 64x64 HUB75 RGB LED matrix (album-art display) ────────────────
// A generic ESP32-S3-WROOM-1 N16R8 devkit driving a single 64x64 HUB75 panel.
// "Art-only" SKU: it reuses the whole driver/net/art pipeline but renders the
// cover art to the LED matrix instead of an SPI LCD. No touch, no audio.
//   • Display layer  -> bsp_matrix.cpp (esphome/esp-hub75 DMA driver + LVGL)
//   • UI layer       -> ui_matrix.c (full-screen art canvas; a stub others can
//                        extend with a clock, VU meter, scrolling title, …)
// The pins below are a PROPOSED default for a bare S3 devkit — rewire freely and
// keep BOARD-s3_matrix.md in sync. GDMA on the S3 routes through the GPIO matrix,
// so any non-reserved GPIO works (avoid strapping 0/3/45/46, USB 19/20, and the
// octal-PSRAM/flash pins 26–37 on the N16R8 module).
#define MMK_BOARD_NAME   "s3-matrix"
#define MMK_HAS_DISPLAY  1   // the matrix IS the display (LVGL renders to it)
#define MMK_HAS_TOUCH    0
#define MMK_HAS_AUDIO    0
#define MMK_NET_WIFI     1
#define MMK_NET_ETH      0
#define MMK_WIFI_MAC_OVERRIDE 0
#define MMK_SNAPSHOT     0   // no serial-framebuffer snapshot on this board
// No MMK_MAX_BUTTONS override: 64x64 of LEDs is an art canvas, not a touch target,
// and MMK_HAS_TOUCH 0 already makes device.c report caps.buttons = 0.
// No BLE prov (same S3 RAM constraint as s3_lcdwiki) — softAP captive portal.
// No live rotation UI: orientation is a build-time HUB75 config, not runtime.

// ── HUB75 64x64 panel (1/32 scan, 5 address lines A–E) ───────────────────────
#define MATRIX_WIDTH     64
#define MATRIX_HEIGHT    64
// RGB data (two half-panels: upper = R1/G1/B1, lower = R2/G2/B2)
#define PIN_HUB75_R1     1
#define PIN_HUB75_G1     2
#define PIN_HUB75_B1     42
#define PIN_HUB75_R2     41
#define PIN_HUB75_G2     40
#define PIN_HUB75_B2     39
// Row-address lines (A–E select which of the 32 row-pairs is lit)
#define PIN_HUB75_A      38
#define PIN_HUB75_B      48
#define PIN_HUB75_C      47
#define PIN_HUB75_D      21
#define PIN_HUB75_E      14   // required for 64-high (1/32-scan) panels
// Clock / latch / output-enable
#define PIN_HUB75_CLK    18
#define PIN_HUB75_LAT    17
#define PIN_HUB75_OE     16
// RGB565 byte order into the panel. bsp_matrix.cpp sets the LVGL display to the
// native LV_COLOR_FORMAT_RGB565 (little-endian on xtensa), so draw_pixels() reads
// it non-byte-swapped (0 = false). This knob is a pure byte-swap toggle: if the
// art comes out byte-garbled on first boot (not just R/B swapped), flip to 1.
#define MATRIX_RGB565_BE 0
// Some panels use FM6126A shift registers (need an init sequence); most are
// GENERIC. Flip in bsp_matrix.cpp if the panel stays dark / shows ghost columns.
#define MATRIX_SHIFT_FM6126A 1

// ─────────────────────────────────────────────────────────────────────────────
#else
// ── lcdwiki 2.8" ESP32-S3 Display (ESP32-S3-WROOM-1 N16R8) — default ─────────
// Single source of truth for the Arduino build lives in ../firmware/include/board.h
// — keep these in sync until the Arduino tree is retired.
// The Control4 T3 (RK3188 Linux port) has no board macro of its own and lands
// here, harmlessly reusing the S3 pin/LCD defaults (its real bring-up is native
// Linux). But it is WIRED, so its net/identity flags must NOT take the S3 wlan0
// defaults — the app would then report/probe wlan0 instead of eth0. The T3 build
// passes -DMMK_BOARD_T3 (lvgl-app/Makefile) so EVERY translation unit overrides
// consistently; a plain -I "shadow" board.h couldn't, since headers that pull
// "board.h" from this directory bypass the shadow. Values match platform/board.h
// byte-for-byte so the two definitions coexist without a redefinition warning.
#if defined(MMK_BOARD_T3)
#define MMK_BOARD_NAME   "t3-7"
#else
#define MMK_BOARD_NAME   "s3-lcdwiki"
#endif
#define MMK_HAS_DISPLAY  1
#define MMK_HAS_TOUCH    1
#define MMK_HAS_AUDIO    1
#if defined(MMK_BOARD_T3)
#define MMK_NET_WIFI     0
#define MMK_NET_ETH      1
#else
#define MMK_NET_WIFI     1
#define MMK_NET_ETH      0
#endif
// Use the FACTORY WiFi MAC (wifi.c defaults the override to 1, which is a C6/
// esp_hosted-only workaround). On the S3's native WiFi the override gave the softAP
// an invalid derived AP MAC and it never beaconed (SSID absent from every scan).
#define MMK_WIFI_MAC_OVERRIDE 0
#define MMK_SNAPSHOT     0   // OFF for production (dev framebuffer-over-serial dump; flip to 1 on the bench). tools/esp_shot.py
// NO BLE Wi-Fi provisioning on the S3: the on-chip NimBLE controller doesn't fit
// alongside WiFi + LVGL in the S3's ~512KB internal RAM — native BLE crashes/OOMs
// (INTERNAL crash-loops, EXTERNAL isn't discoverable, WiFi-in-PSRAM → HCI init fails).
// The C6 boards (ws43) do BLE fine because the controller is a separate chip. The S3
// uses the softAP captive portal for Wi-Fi setup (works). (MMK_HAS_BLE_PROV undefined.)
#define MMK_CAN_ROTATE   1   // SPI panel: live SW rotation → show the Orientation setting

// ── Display: ILI9341V, 320x240, SPI @ 40 MHz ───────────────────────────────
#define LCD_WIDTH        320
#define LCD_HEIGHT       240

// 6 programmable buttons. 320x240 landscape → 4 columns, so 6 is a row of 4 plus a
// row of 2, and the grid gets ~72x86 px tiles. Going to 8 would fill both rows but
// on a 2.8" diagonal that is ~13x15 mm per tile — at/below the point where a
// fingertip starts hitting neighbours. Geometry, not the driver, is the limit here.
// (T3 overrides to 8 to match platform/board.h — the keypad grid validated on glass.)
#if defined(MMK_BOARD_T3)
#define MMK_MAX_BUTTONS  8
#else
#define MMK_MAX_BUTTONS  6
#endif

#define LCD_SPI_FREQ_HZ  40000000
#define LCD_SPI_HOST     SPI2_HOST

#define PIN_LCD_SCK      12
#define PIN_LCD_MOSI     11
#define PIN_LCD_MISO     13
#define PIN_LCD_CS       10
#define PIN_LCD_DC       46
#define PIN_LCD_RST      -1   // no dedicated reset; tied to chip EN
#define PIN_LCD_BL       45   // backlight, active HIGH

// ── Capacitive touch: FT6336G on I2C @ 0x38 (FT5x06 protocol) ───────────────
#define TOUCH_I2C_ADDR   0x38
#define PIN_TOUCH_SDA    16
#define PIN_TOUCH_SCL    15
#define PIN_TOUCH_INT    17   // active LOW when touched
#define PIN_TOUCH_RST    18   // active LOW reset
#define TOUCH_I2C_PORT   0

// ── "Halo": onboard WS2812B RGB LED (single-wire). lcdwiki IO table: IO42. ───
// Used as a nightlight / design accent (idle solid color) + a status pulse
// (e.g. blue breathing while a call rings). See halo.c.
#define PIN_RGB_LED      42

// ── Audio: ES8311 codec + FM8002E amp (Phase 3) ────────────────────────────
// The codec shares the touch I2C bus (created in bsp.c), so no PIN_AUDIO_SDA/SCL
// here — audio.c calls bsp_i2c_bus(). AUDIO_I2C_PORT must match the touch port.
#define ES8311_I2C_ADDR  0x18   // confirmed on hardware (chip id 0x83/0x11 @ 0x18)
#define AUDIO_I2C_PORT   TOUCH_I2C_PORT
#define PIN_I2S_MCLK     4
#define PIN_I2S_BCLK     5
// GPIO6/8 are swapped vs the lcdwiki IO table (which names them from the codec's
// side). Verified on hardware: these values capture mic + play speaker; swapping
// kills both. dout=ESP->codec DSDIN (speaker), din=codec ASDOUT->ESP (mic).
#define PIN_I2S_DOUT     8
#define PIN_I2S_LRCLK    7
#define PIN_I2S_DIN      6
#define PIN_AMP_ENABLE   1    // FM8002E enable; drive LOW to ENABLE the amp
#define AMP_ACTIVE_LOW   1    // FM8002E: drive LOW to ENABLE the amp

#endif

// SIP intercom capability. Defaults to "has audio" — a board can override to 0
// (local audio without the SIP UA). Defined here (not sip.h) so no-audio boards,
// which never include sip.h, still resolve MMK_HAS_SIP (net.c advertises it).
#ifndef MMK_HAS_SIP
#define MMK_HAS_SIP MMK_HAS_AUDIO
#endif

// How many programmable keypad buttons this panel's UI can lay out ("up to N").
// It is a PER-BOARD constant chosen from the panel's usable geometry (see each
// board's block above) — deliberately not derived from LCD_WIDTH/LCD_HEIGHT by a
// formula, because the honest input is physical touch-target size, which pixels
// alone don't give you (a 320x240 2.8" and a 320x240 5" want different answers).
//
// It sizes three things that must agree or buttons silently vanish:
//   * caps.buttons in the `hello` manifest (device.c) — what the driver creates
//   * NET_MAX_BUTTONS (net.h) — the inbound `state` array
//   * KP_MAX (ui.c) — the grid's tile pool
// Touchless boards leave this at the fallback and report 0 via MMK_HAS_TOUCH; the
// fallback must stay >= 1 so net.h's fixed-size array is legal C.
#ifndef MMK_MAX_BUTTONS
#define MMK_MAX_BUTTONS  6
#endif
