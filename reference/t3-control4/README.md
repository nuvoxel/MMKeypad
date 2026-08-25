# Control4 T3 Touchscreen — hardware teardown & firmware reference

Field notes from probing a **Control4 T3-7 in-wall touchscreen** (2026-07-04),
captured for anyone wanting to understand, back up, or repurpose these EOL units.
All access here was **read-only** — the device was never written to and boots
Control4 normally afterward.

> **Likely a commodity Rockchip RK30xx reference design.** `ro.product.board =
> rk30sdk` is Rockchip's generic RK3188 SDK board, and the whole BOM below is the
> stock RK3188-reference parts list. Control4 wrapped a standard RK3188 module in
> a touchscreen. RK3188 shipped in millions of ~2013–15 TV sticks/tablets, so it
> has deep community support (kernels, tooling, Android/Linux builds) — a big
> advantage for any repurpose. Exact module vendor (Waveshare / Radxa Rock / ODM)
> still to confirm.

## Models
T3-7 (7") and T3-10 (10"), in-wall + tabletop. **Both share the same RK3188 SoC
and the same board software baseline.**

> **(2026-07-18) On kernels being identical across models:** an earlier edit
> claimed they differ, based on comparing the 7"'s *live* kernel partition
> (`04790edf…`) against the 10"'s *pristine* backups (`a41831866c…`). That was
> custom-vs-stock, not 7"-vs-10", so it proved nothing. Evidence now favours the
> ORIGINAL claim: the genuine stock **boot** images from the 7"-lineage USB backup
> and both 10" backups are **byte-identical** (`e1663ddc…`), and that stock image
> booted fine on the 7". Treat the models as sharing a boot/kernel baseline.
>
> The safe rule is unchanged regardless: the kernel carries the panel/DTB, so
> prefer a unit's own backup, and `tools/flash.sh`'s refusal to guess across units
> is a reasonable default. Note `build/backup-000fffxxxxxx/` is a **10"** backup
> despite its serial-style name, so label backup dirs by model, not just serial.

### Telling the models apart at runtime (2026-07-19)
**The kernel version string carries the model.** `uname -v` reads
`#1-glassedge7.2.0` on the 7" and `#1-glassedge10.2.0` on the 10", with a trailing
`p` on the tabletop variants. One build serves every T3, so this is how a single
binary identifies the unit it is running on — previously everything hardcoded
`t3-7` and the 10" mislabelled itself.

Control4's own identifiers, from the launcher's per-model configs
(`phoenix-navigator.apk` → `assets/glassedge*.conf`). These are what the unit
**declares over SDDP**, so they are the right thing for device identity:

| kernel tag | Control4 model | ModelName |
|---|---|---|
| `glassedge7` | `C4-TS-INWALL7` | T3 7" In-Wall Touch Screen |
| `glassedge7p` | `C4-TS-PORTABLE7` | T3 7" Tabletop Touch Screen |
| `glassedge10` | `C4-TS-INWALL10` | T3 10" In-Wall Touch Screen |
| `glassedge10p` | `C4-TS-PORTABLE10` | T3 10" Tabletop Touch Screen |

> These are **not** the orderable part numbers on the Control4 datasheet, which are
> `C4-WALL7-BL` / `C4-WALL7-WH` / `C4-WALL10-BL` / `C4-WALL10-WH`. That suffix is
> the bezel **colour**, which nothing on the device exposes — so software cannot
> derive the sales SKU, and shouldn't pretend to. Datasheet specs for reference:
> 1280×800, capacitive touch, 720p camera, PoE 802.3af 13 W peak or 100–240 VAC.

Note `ro.product.model` in `build.prop` is the generic Rockchip SDK value
(`rk3188`) on every unit — it is useless for identification. Implementation:
`firmware-linux-t3/platform/device_t3.c` (`t3_variant` / `board_name` /
`model_code` / `model_friendly`).

As of **2026-07-18 both the 7" and the 10" have been directly
probed** (not just the 7"). The units are NOT pixel-for-pixel identical, though —
the panel is a **different physical size AND native orientation**, and this
particular pair shipped with **different touch controllers and WiFi modules**.
See [Per-model differences](#per-model-differences-7-vs-10--direct-probed-2026-07-18).

- **7" (`C4-TS-INWALL7`, `glassedge7`)** — panel native **800×1280 portrait**
  (shown rotated to landscape); touch **Silead GSL1680** (`gslX680`, i2c 2-0040);
  WiFi **AMPAK AP6330**.
- **10" (`C4-TS-INWALL10`, `glassedge10`, "T3 10\" In-Wall Touch Screen")** —
  panel native **1280×800 landscape** (no rotation needed); touch **Goodix**
  (`goodix-ts`, `/dev/input/event1`); WiFi **RK903** (`/sys/class/rkwifi/chip =
  OOB_RK903`, Broadcom-family). Model strings extracted from the stock launcher
  confs (`glassedge10.conf`).

## Hardware inventory
| Block | Part | Notes |
|---|---|---|
| SoC | **Rockchip RK3188** | quad Cortex-A9 ~1.6GHz, 28nm (board `rk30sdk`, platform `rk3188`) |
| GPU | **ARM Mali-400 MP4** | drivers `mali.ko` + `ump.ko` |
| VPU (video) | **Rockchip VPU** | `vpu_service.ko` — HW **H.264** 1080p decode/encode, **NO H.265** |
| 2D accel | Rockchip IPP | `rk29-ipp.ko` (scale/rotate), `rk30_mirroring.ko` |
| RAM | DDR3 | size not read (typ. 1–2 GB for this class) |
| Storage | **Hynix NAND ~8 GB** | flash ID `AD` = Hynix; 16KB page, 4096KB block, 40-bit ECC |
| PMIC | **Active-Semi ACT8846** | standard RK3188 PMIC |
| Audio codec | **Realtek RT3261** (ALC3261, w/ DSP) | LIVE-VERIFIED: sound card `RK29_RT3261`, i2c `4-001c`, chip id 0x6. (Kernel *also* carries an ES8323 driver, but RT3261 is the populated part — README earlier guessed ES8323 from the SDK list.) **Speaker is on DAC2 via `aif2`/PCM dev1 only, and 44.1 kHz can never open there** — see [Audio subsystem](#audio-subsystem-rt3261--live-probed-on-the-10-2026-07-18). |
| Microphone | present, **WORKING** | Capture on **`pcmC0D0c` (dev0/aif1)**; playback is dev1/aif2 — *different DAIs*, so no shared-rate constraint. Was dead until 2026-07-19: our own replay of the idle mixer snapshot was killing it. Hardware AEC in the codec DSP gives 24–33 dB echo suppression. See [Audio subsystem](#audio-subsystem-rt3261--live-probed-on-the-10-2026-07-18). |
| Touch | **Silead GSL1680** (`gslX680`) — 7"; **Goodix** on the 10" | LIVE-VERIFIED (7"): input `gslX680`, i2c 2-0040. **The 10" ships a Goodix (`goodix-ts`) instead** — different controller AND protocol; see [Per-model differences](#per-model-differences-7-vs-10--direct-probed-2026-07-18). |
| Camera | **Novatek NT99141** (1MP, 720p) | LIVE-VERIFIED: i2c `3-002a` `nt99141`, via `rk_cam_cif` CIF. (Not OV2659 — that was the SDK guess.) |
| WiFi/BT | **AMPAK AP6330** (Broadcom BCM4330, SDIO) — 7"; **RK903** on the 10" | LIVE-VERIFIED (7"): `[BT_RFKILL] ap6330 device registered`. **The 10" reports `OOB_RK903`** (`/sys/class/rkwifi/chip`) — both Broadcom-family, both driven by `rkwifi.oob.ko`. `c4.feature.wifi5ghz=true` flag present (BCM4330 is 2.4GHz — aspirational/model-dependent). |
| RTC | **Haoyu HYM8563** | LIVE-VERIFIED: i2c `1-0051` `rtc_hym8563` |
| IR receiver | **NONE (earlier claim was wrong)** | Re-measured 2026-07-19 on BOTH panels, under our Linux *and* stock: no `/sys/class/rc/rc0`, `/sys/class/lirc` exists but is **empty**, no `/dev/lirc*`, and no IR entry in `/proc/bus/input/devices` (only `rk29-keypad` + touch). The dmesg lines registering NEC/RC5/RC6/JVC/Sony are the kernel's **generic protocol decoders**, not a wired receiver — that is what the original "LIVE-VERIFIED" note mistook them for. Detecting IR by the existence of `/sys/class/lirc` is a false positive on every unit. |
| Ethernet | **Realtek RTL8152** USB 2.0 10/100 NIC (`eth0`) | LIVE-VERIFIED (10"): USB `0bda:8152` on `usb20_host`, driver **`r8152`**, product "USB 10/100 LAN", MAC from the unit serial (`00:0f:ff:<serial>`). It's a **USB NIC, not a SoC MAC** — the RJ45 + PoE PD live in the backbox, this chip is on the front board. Kernel also carries `cdc_ether`/`asix`/`smsc95xx` (multi-NIC). **Gotcha: its `carrier` sysfs mis-reports link** → bring-up must retry DHCP, not fire-and-forget. |
| Display | 1280×800 IPS (7" native portrait, 10" native landscape) | RK3188 display controller; see [Per-model differences](#per-model-differences-7-vs-10--direct-probed-2026-07-18) |
| Power | **PoE 802.3af**, 13W peak (in-wall) or 100–240 VAC | tabletop has a 3100 mAh battery |

**Other notable silicon (LIVE-VERIFIED on the 10" under our Linux, 2026-07-18):**
- **Multi-vendor touch** — like the WiFi, the kernel carries several touch drivers
  and binds whatever's populated: this 10" = **Goodix** (i2c `2-0014`), with a
  **FocalTech `ft5x0x`** driver also probing `2-0038` (the harmless "No ack at
  0x38"); the 7" = **Silead GSL1680**. Three vendors supported across the line.
- **Bluetooth is live** (AP6330/RK903 combo) — full stack up over HCI-UART/HCILL
  (RFCOMM/L2CAP/SCO). Classic BT **+ BLE** available, not just WiFi.
- **RT3261 has an on-chip DSP + two I²S audio interfaces** (`aif1`/`aif2`) — the
  DSP offers `AEC+NS+FENS`/`HFBF`/`Far Field Pick-up`, but appears **inert** (no
  firmware blob on `/system`, powers up-then-down on every capture), and stock
  Android used **speex software AEC** instead. `aif2` is the voice path AND the
  only one that reaches the speaker. Full detail:
  [Audio subsystem](#audio-subsystem-rt3261--live-probed-on-the-10-2026-07-18).
- **ADC + ADC-based battery/charge gauge** (`rk30-adc`, `rk30_adc_battery`).
- **No IR receiver.** The `lirc_dev` class and the NEC/RC5/RC6/JVC/Sony decoders
  register at boot on every unit, but no device is ever bound (empty
  `/sys/class/lirc`, no `/dev/lirc*`, no input node). A **button-backlight LED**
  provision exists (GPIO unpopulated on this unit).
- Red herring: `/sys/class` also lists **Roccat gaming-mouse drivers**
  (`arvo/kone/koneplus/kovaplus/pyra`) — generic-kernel-config leftovers, NOT
  hardware on this board.

## Display panel (LIVE-VERIFIED — for reuse off the RK3188)
For driving this LCD from a *different* controller, the measured timing:
- **Native panel geometry differs by model** (same SoC/kernel, panel auto-detected):
  - **7": 800×1280 PORTRAIT** (framebuffer `U:800x1280p-54`), shown rotated to
    1280×800 landscape (`sys.display.oritation=2`, `wm size` 1280×800, density
    213/tvdpi). Stock fb 32 bpp, stride 3200 (=800×4).
  - **10": 1280×800 LANDSCAPE** (native, no rotation). LIVE-VERIFIED under our
    custom Linux: `/sys/class/graphics/fb0` `virtual_size 1280,800`, **16 bpp
    RGB565, stride 2560** (=1280×2), `modes U:1280x800p-0`. (Our firmware picks
    rotate-vs-direct from this aspect at runtime.)
- **Pixel clock (DCLK) = 64.0 MHz** (`lcdc0: dclk:64000000`), **~55 Hz** refresh
  (measured on the 7"; the 10" runs the same LCDC driver).
- fb reserved 12 MB @ 0x97c00000.
- **Interface (7"): 24-bit parallel RGB (RGB888) — CONFIRMED.** Evidence: RK3188
  has **no MIPI-DSI**; the 7"'s kernel loads `rk30-lcdc` in **parallel-RGB mode**
  with **zero LVDS-init messages**; 64 MHz is a normal parallel pixel clock (no
  serialization); and the 7" LCD FFC is a **fine-pitch 0.5 mm ~40-pin** connector
  (FCC photos) — a ~40-conductor bus fits 24-bit RGB, whereas single-link LVDS is
  only ~10–20 conductors. So the 7" drives from **any RGB/DPI source with no LVDS
  bridge**. (At 800×1280@64 MHz you need a capable RGB source — ESP32-P4-class or a
  Linux SoC — not a small ESP32-S3.)
- **Interface (10"): 24-bit parallel RGB — CONFIRMED (2026-07-18).** Three
  independent checks agree: the **10" LCD FFC was physically counted at ~40 pins**
  (a parallel-RGB-width bus; LVDS would be ~10–20); live `dmesg` shows `rk30-lcdc`
  at the **same `dclk 64 MHz / 56 fps`** as the 7" with **no LVDS/MIPI init and no
  LVDS-bridge IC**; fb is 1280×800 RGB565. So **both sizes are parallel RGB** and
  drive from any RGB/DPI source with no LVDS bridge. (RK3188's LCDC *can* do LVDS,
  but neither T3 uses it.)
- **Backlight: PWM** via `rk28_bl` (`/sys/class/backlight/rk28_bl`, max 255).

**Power rails to recreate if repurposing the panel/board (ACT8846 PMIC, measured):**
DCDC1 1.2 V · DCDC2(vdd_core) 1.1 V · DCDC3(vdd_cpu) 1.075 V · DCDC4 3.3 V ·
LDO1 1.0 · LDO2 1.2 · LDO3 1.8 · LDO4 3.3 · LDO5 3.3 · LDO6 1.8 · LDO7 1.8 ·
LDO8 3.3 · LDO9 1.2 (off). Full dumps: [`hwinfo/live-display.txt`](hwinfo/live-display.txt),
[`hwinfo/live-peripherals.txt`](hwinfo/live-peripherals.txt).

- **LCD↔mainboard link:** **~40-pin 0.5 mm FFC = 24-bit parallel RGB (RGB888),
  CONFIRMED** (see Display panel section). **Panel back stamp: `T00250A25EA-C01A`,
  date `2017-06-08`** (likely a C4/vendor internal code, not the raw panel P/N).
  The glass drives from any RGB/DPI-capable source with **no LVDS bridge** — but
  RGB FFC *pinouts are vendor-specific*, so a replacement board needs an adapter
  matched to THIS panel's pin order (the connector alone isn't a compatibility spec).

## Audio subsystem (RT3261) — LIVE-PROBED on the 10", 2026-07-18

Everything below was measured on our custom Linux (tinyalsa), not inferred. Audio
on this board is the single hardest thing to bring up, and most of the obvious
assumptions about it are wrong — so the traps are documented alongside the facts.

### Two I²S links, and only one reaches the speaker
The codec exposes `rt3261-aif1` (PCM **dev0**, hifi) and `rt3261-aif2` (PCM
**dev1**, voice). `/proc/asound/pcm` lists both with `playback 1 : capture 1`.

- **The speaker is analog-wired to DAC2, which only `aif2` (dev1) feeds.** dev0
  reaches DAC1, whose path to the speaker this board doesn't use. Playing to dev0
  is silent no matter what the mixer says.
- **`aif2`'s SYSCLK is pinned at 24.576 MHz**, and the codec requires
  `sysclk == rate × 256 × pd` for `pd ∈ {1,2,3,4,6,8,12,16}`. So
  `24576000/(44100×256) = 2.177` — **44.1 kHz can never open on dev1** and fails
  with `Unsupported clock setting`. Usable rates are **48000 / 16000 / 8000**.
- Playback and capture **share the aif2 DAI and must run at the same rate**.
  Opening capture at 8 k while playback runs at 48 k fails outright with
  `cannot set hw params`.

> The long-standing "silent speaker" bug was never a DAPM/mixer problem — it was
> the wrong PCM device at an impossible rate (dev0 @ 44100). The DAC2→speaker
> route was already enabled.

### Capture: 48 kHz works, 8 kHz is silently broken
- **dev1 @ 48 kHz is correctly hardware-paced**: 10 reads of 480 frames take
  102 ms against 100 ms expected.
- **dev1 @ 8 kHz is NOT paced**: the same reads take 69 ms against 400 ms
  expected. The driver *accepts* an 8 kHz capture open and then ignores it.
  **Consequence: capture at 48 kHz and downsample in software** — do not open the
  ADC at 8 k for G.711/SIP.
- **dev0 capture is dead**: six successive reads return six *identical*
  checksums. Don't use it for capture.

### The microphone: WORKING (2026-07-19) — our own mixer replay was killing it
**Capture is on `pcmC0D0c` (dev0 / aif1); playback is on dev1 (aif2). They are
DIFFERENT DAIs**, so there is no shared-rate constraint between them.

**Root cause of the long-dead mic: replaying the idle mixer snapshot.** Our app
replayed the 159-control `mixer_route.h` snapshot at every boot. That snapshot was
captured from stock while IDLE — capture torn down — so replaying it left the
codec in a state a capture stream cannot start from, and every read returned exact
digital zero. Leaving the codec in its **driver-reset state** makes the mic work
immediately, and playback still works (the playback fix was always device+rate,
aif2/dev1 @48 kHz, never the route).

Proof, by the only test that counts — gain sensitivity on an ANALOG control:

| `IN1 Boost` | 0 | 4 | 8 |
|---|---|---|---|
| replay skipped | peak 28 | peak 61 | peak 113 |
| replay applied | 0 | 0 | 0 |

Acoustic check: with a tone playing, mic RMS rises 17 → 10516.

How it was isolated: `tinycap` on stock captures real audio with the EXACT config
that returned zero under our Linux (dev0, 48 kHz, stereo, 1024×6) while bypassing
the Android HAL entirely. Same kernel, same PCM node, same tinyalsa calls,
opposite result — so the fault had to be in our boot environment, not userspace,
the HAL, the mixer route, or the device. The only thing our boot does to the codec
that stock does not is the snapshot replay.

`/data/nvx/force-mixer-route` restores the old behaviour for A/B testing.

### AEC: WORKING in the codec's hardware DSP (~24 dB)
- **RK3188 has no echo cancellation.** It is an application processor with an I²S
  controller; there is no AEC block in it.
- **The RT3261 codec does**, via its voice DSP: `DSP Function Switch` =
  `Disable / AEC+NS+FENS / HFBF / Far Field Pick-up`. `FENS` is far-end noise
  suppression, `HFBF` hands-free beamforming.
- **MEASURED WORKING 2026-07-19.** Same acoustic loopback, toggling only the DSP:

  | DSP mode | mic RMS while the speaker plays |
  |---|---|
  | `Disable` | 4722 |
  | `AEC+NS+FENS` | **299** |

  ≈ **24 dB of echo suppression**, in hardware, for free — `AEC+NS+FENS` is the
  driver-reset default, so we inherit it by simply not overwriting the codec state.
- Earlier notes here speculated the DSP was inert (no firmware blob on `/system`,
  PMU-then-PMD on every capture). That was wrong: the PMU/PMD churn was just DAPM
  tearing down short-lived test streams, and the DSP demonstrably cancels.
- Stock Android also linked **speex** software AEC in its HAL, but we do not need
  it: the hardware AEC is engaged and effective.
- **Test design note:** a working AEC deliberately removes the speaker signal from
  the mic, and NS suppresses steady tones — so a fixed-tone loopback UNDER-reports
  a working system. Compare DSP-on vs DSP-off, or use a chirp/noise burst and
  cross-correlate, rather than trusting a single Goertzel bin.

### Recovering the stock mixer routes from the Android HAL
`/system/lib/hw/audio.primary.rk30board.so` contains the authoritative
known-good routes. It's stripped, but the tables are recoverable: they are arrays
of `struct { char *ctl_name; char *strval; int val1; int val2; }` (**16 bytes**,
two ints for stereo pairs) living in `.data.rel.ro.local`, with the name pointers
resolving into `.rodata`. Script:
[`firmware-linux-t3/tools/route_extract.py`](../../firmware-linux-t3/tools/route_extract.py)
— parses the ELF sections, indexes strings by vaddr, then scans for runs of
records whose first word is a known control-name address. Pull the HAL off a
panel first: `ssh root@<panel> 'cat /system/lib/hw/audio.primary.rk30board.so' >
hal.so && ./route_extract.py hal.so`.

The recovered **rt3261 capture route matches what we already set** (`RECMIXR
BST1=1`, `Mono ADC R1 Mux=ADCR`, `Mono ADC MIXR ADC1=1`, `IF2 ADC R Mux=Mono ADC
MIXR`, `ADC IF2 Data Switch="right copy to left"`, `IN1 Mode=Differential`) —
confirming the analog mixer route was never the missing piece. Note the HAL's
`Capture MIC Path` control belongs to the *other* codecs it supports
(wm8960/rt5616/rk616) and **does not exist on the rt3261**.

### How to validate audio claims here (learned the hard way)
Signal *level* proved misleading three separate times. Use instead:
1. **Timing** — reading N seconds of audio must take N seconds. This is what
   exposed the broken 8 kHz rate.
2. **Per-period checksums** — six successive reads must differ. Aggregate RMS is
   too coarse: a periodic signal holds a stable 1-second RMS even while every
   period differs, which reads as "not live" and produces false negatives.
3. **Buffer poisoning** — `memset(0xA5)` before each read. Without it, "nothing
   was transferred" and "the same data was transferred" are indistinguishable.
4. **Response to real sound**, and **gain sensitivity** on a control that is
   actually in the path (`ADC Capture Switch=0` mutes to exactly rms 0.0 — a
   useful proof the samples are a live stream and not a stale buffer).

Known false positive: **opening capture on dev0 flips subsequent dev1 reads from
zeroes to non-zero stale data.** That looks exactly like a fix if you only check
level. An "ADC prime" built on it was committed and reverted.

## Backbox interconnect + FCC teardown (FCC ID R33C4WALL5G)
Public FCC internal photos of the in-wall T3 (7"/10"): the front assembly holds
**everything** (SoC, WiFi, USB-Ethernet, camera, speakers, battery, LCD); the
**backbox** holds only the RJ45 + Ethernet magnetics + **PoE PD** (or an AC-DC
supply in the 120 V variant). They join over a single ribbon.

- **Backbox ribbon connector = a 2×10 = 20-pin, 1.27 mm-pitch shrouded header —
  "Cortex-20" form factor** (the standard ARM 20-pin debug connector footprint).
  PHYSICALLY COUNTED on the 10" (2026-07-18) — this supersedes an earlier
  photo-based estimate of ~2×12/2.54 mm. Upside: the mating 1.27 mm 2×10 IDC
  socket + ribbon are cheap/off-the-shelf. Pin 1 keyed/marked; **pins are NOT
  silkscreen-labeled** and functions route on inner layers → the per-pin map needs
  continuity/active-stimulus probing, not photo-tracing (FCC photos don't give it).
- **What actually crosses it (software-confirmed, signals not pins):** wired
  **Ethernet — 2 pairs** (the front-board **RTL8152 USB 10/100** NIC on
  `usb20_host` drives out to the RJ45 in the backbox; `eth0` carrier went 0→1 the
  instant the backbox was connected) **+ PoE-derived DC power** (`power_supply
  ac=Mains online=1`, battery began charging). **Confirmed 2026-07-18 the backbox
  is otherwise passive** — with it unplugged, the running unit showed **no USB
  device or peripheral disappear** (the only USB device, the RTL8152, is
  front-side); pulling the ribbon just drops `eth0` link + PoE power. The remaining
  ~16 pins of the 20 are almost certainly **interleaved grounds** for the Ethernet
  pairs plus maybe a backbox-detect line.
- **Bring-up test points on the main board (FCC photo silkscreen):** a **UART
  console** (`VCC RX TX GND`) and a **USB test point** (`VBUS DM DP PWR`) — useful
  if driving this panel/SoC standalone. (These are pads, separate from the 20-pin
  backbox header.)
- **Main board ID:** `W-SC IL-14#PAD_V5_20150527`. **WiFi/BT module `AP6330`**
  visually confirmed. Filing + photos:
  https://fcc.report/FCC-ID/R33C4WALL5G (exhibit 4014009 = internal photos, 7").

## Software baseline
- **Android 4.4.2 (KitKat, API 19)**, Rockchip `rk30sdk` BSP.
- **Linux kernel 3.0.36** (from `.ko` suffixes `*.3.0.36+`).
- **Control4 OS 4.0.0** Navigator — the last officially-supported T3 version.
- EOL: superseded by Control4 **T4** (1920×1200, dual-mic) and **T5** (8"/11",
  quad-mic). T4/T5 use their **own** back boxes — they are **not** T3-back-box
  compatible, so there is no drop-in hardware-swap path from a T4/T5.

### Kernel modules (driver baseline) — from `/system/lib/modules`
`mali.ko`, `ump.ko` (GPU) · `vpu_service.ko` (video codec) · `rk29-ipp.ko`,
`rk30_mirroring.ko` (display/2D) · WiFi: `8188eu 8189es 8192cu 8723as 8723au`
(Realtek), `mt5931 mt7601*` (MediaTek), `esp8089` (ESP), `rkwifi* wlan.ko`
(Rockchip wrapper), `fw_RK901*.bin`. The board loads whichever matches its
actual WiFi chip.

## NAND partition map (`mtdparts`, from the parameter file; units = 512B sectors)
```
misc      0x00002000 @ 0x00002000
kernel    0x00006000 @ 0x00004000
boot      0x00006000 @ 0x0000a000
recovery  0x00010000 @ 0x00010000
backup    0x00020000 @ 0x00020000
cache     0x00200000 @ 0x00040000   (~1 GB)
userdata  0x00500000 @ 0x00240000   (~2.5 GB)
metadata  0x00002000 @ 0x00740000
kpanic    0x00002000 @ 0x00742000
system    0x00200000 @ 0x00744000   (~1 GB)
user      -          @ 0x00944000   (rest)
```

## How to access it (read/backup) — the method that worked
1. **Two buttons on the back**: a labeled **RESET pinhole** (plain SoC reset) and
   a **second UNLABELED button = RECOVERY**.
2. Connect the **micro-USB** (OTG) on the back to a host.
3. **Hold the unlabeled RECOVERY button while plugging in USB / powering on** →
   the device enters Rockchip **rockusb Loader mode** (USB `2207:310b`) and stays
   there. (A normal boot's maskrom window is too brief to catch by polling; ADB
   is locked off, so this button combo is the way in.)
4. Talk to it with **`rkdeveloptool`** (built from source; on macOS needs
   `make CXXFLAGS="-Wno-error"` for a Clang VLA warning):
   - `rkdeveloptool ld` — confirm Loader mode
   - `rkdeveloptool rfi` — flash info · `rkdeveloptool rid` — flash id
   - `rkdeveloptool rl <start_sector> <count> <outfile>` — read region (~16 MB/s)
5. `system`/`userdata` are **ext4** → extract on macOS without mounting via
   e2fsprogs: `debugfs -R "ls -l /app" system.img`, `debugfs -R "dump /path out" system.img`.
6. To boot Control4 normally again: power-cycle **without** holding recovery.

> ⚠️ **Identify a boot image by its RAMDISK, not by strings on the whole file.**
> Two "restore to stock" attempts on the 7" (2026-07-18) reinstalled our own Linux
> instead, and cost a long detour into invented theories (mtdblock write caching, a
> hidden boot source). The real cause was mundane: **the file flashed as stock was
> not stock.** `build/backup-net-000fffxxxxxx/boot.orig` is our own custom image —
> a `--net` backup dumps whatever is *currently* in the partition, so a file named
> `boot.orig` captured after a custom flash contains the custom system.
>
> The false negative that sold it as stock: the ramdisk is **gzipped**, so grepping
> the raw image for `dropbear`/`mmkeypad` finds nothing even when they are there.
> Parse the `ANDROID!` header, extract and gunzip the ramdisk, then check markers:
>
> | image | ramdisk contains |
> |---|---|
> | genuine stock (`e1663ddc…`) | `zygote`, `init.rc`, `ueventd` |
> | ours (`cc5b0524…`, `4ed88be5…`) | `dropbear`, `mmkeypad`, `mmkinit` |
>
> **The `boot` partition (mtd2, LBA 40960) IS the boot source and works fine.**
> Writing the genuine stock image there via the USB loader booted stock Android
> first try. Both `--net` dd and `rkdeveloptool wl` commit correctly.
>
> Verify a boot flash by what actually boots — a read-back md5 only proves the
> bytes landed, not that they were the bytes you meant.
>
> **Loader mode without the recovery button:** from our Linux,
> `reboot(LINUX_REBOOT_CMD_RESTART2, "loader")` drops the unit straight into
> rockusb (`Vid=0x2207,Pid=0x310b`); from stock Android, `adb reboot loader`. That
> makes boot-level experiments fully remote and recoverable.

**Secure-boot / running custom code — TESTED (2026-07-04), see
[`JAILBREAK.md`](JAILBREAK.md).** Findings: `/system` is a plain ext4 with **no
dm-verity** → an **unsigned `/system` reflash boots**, which yields **persistent
root adb** (via `sys.rkadb.root=1` in `build.prop`) with `boot` left untouched.
The `boot` partition **is** integrity-checked (tampering → recovery fallback),
but the efuse shows **no OEM RSA key fused** (`efuse_val` key region all zeros),
so that check is a CRC, not a signature. **CONFIRMED 2026-07-18: a properly
repacked `boot.img` (this unit's own kernel + our own initramfs) boots and runs a
fully custom Linux/musl userspace on BOTH the 7" and the 10".** Net: these T3s are
**open** for repurposing. See
[Custom-Linux post-flash access](#custom-linux--post-flash-access-2026-07-18) and
the working image at [`firmware-linux-t3/`](../../firmware-linux-t3/).

## Per-model differences (7" vs 10") — direct-probed (2026-07-18)
Both units were dumped over Loader USB and (10") booted with our custom image.
**The two are SEPARATE (but very similar) PCB designs** — physically distinct
boards (different outline, panel-connector position, and the different populated
touch/WiFi chips below), *not* one board in two shells. What they share is the
**platform + software image**, which is why one build/toolchain serves both.
**Identical across both:** RK3188 SoC & ARMv7 userspace · **boot partition +
kernel are byte-for-byte identical**
(`md5 e1663ddc619a3f8f6ddebeea68b63a72`, kernel region
`12b283f2101ed60c2d34805fc76c0607`, 8,822,820 B) · NAND partition map · Android
4.4.2 / kernel 3.0.36 baseline · `/system` module set · the RT3261 audio codec ·
the NT99141 camera · the Loader-mode + jailbreak method.

The **same kernel auto-detects the panel/peripherals at runtime**, so the parts
that differ are carried by the hardware, not the image:

| Aspect | T3-7 | T3-10 |
|---|---|---|
| Model string | `C4-TS-INWALL7` (`glassedge7`) | `C4-TS-INWALL10` (`glassedge10`) |
| Framebuffer (native) | **800×1280 portrait** (`/dev/fb0`), UI rotated 90° to landscape | **1280×800 landscape**, UI drawn direct (no rotation) |
| Touch controller | **Silead GSL1680** (`gslX680`, i2c 2-0040) | **Goodix** (`goodix-ts`, `/dev/input/event1`) |
| Touch protocol | (type-B, uses `ABS_MT_TRACKING_ID`) | **MT protocol-A, range 800×480, NO `TRACKING_ID=-1`** — must use **`BTN_TOUCH`** (EV_KEY `0x14a`) for press/release, or the press sticks and no click ever fires |
| WiFi module | **AMPAK AP6330** | **RK903** (`/sys/class/rkwifi/chip = OOB_RK903`) — both Broadcom-family, both load `rkwifi.oob.ko` |
| Input nodes | — | `event0` = `rk29-keypad`, `event1` = `goodix-ts` touch |

Because the fb orientation and touch controller genuinely differ, the firmware
**detects both at runtime** (fb aspect → rotate-or-direct flush; `EVIOCGABS` →
touch range; `BTN_TOUCH` primary press signal) rather than hardcoding a panel.

## Custom-Linux / post-flash access (2026-07-18)
Our image ([`firmware-linux-t3/`](../../firmware-linux-t3/)) replaces only the
**initramfs** in a repacked `boot.img` (keeps the unit's own kernel), so it runs
on any T3. Build + flash with [`tools/flash.sh`](../../firmware-linux-t3/tools/flash.sh):
- **USB (Loader) path** — hold RECOVERY + plug USB → Loader → `flash.sh` backs up
  boot, builds app+init+ramdisk, repacks against the unit's own kernel, writes
  `boot` (LBA 40960) via `rkdeveloptool wl`, resets. `--restore` reverts.
- **Network path** — `flash.sh --net <ip>` dumps the unit's boot from `mtd2` over
  SSH, repacks, `dd`s it back, reboots. **Reboot trap: plain `reboot` no-ops on
  this jailbreak — use `reboot -f`** (busybox direct `RB_AUTOBOOT`), or a freshly
  written boot partition keeps running the OLD in-RAM kernel and the flash looks
  like it "didn't take."

**Ways to reach the unit once it's running our image:**
- **SSH (dropbear on :22)** over `eth0` (PoE) or `wlan0` — the primary channel.
  Root, key auth (`~/.ssh/id_rsa`). `eth0` gets a DHCP lease automatically; the
  on-screen WiFi picker joins `wlan0`. (`eth0`'s `carrier` sysfs is unreliable —
  reports no-link even when up — so init uses a **retrying `udhcpc`**, not a
  single fire-and-forget attempt.)
- **USB gadget (composite `rndis,acm`)**, brought up by init on every boot, so an
  in-wall unit with no PoE/WiFi is still reachable over the micro-USB:
  - **RNDIS ethernet** — `rndis0 @ 10.55.0.1/24` on the device; SSH in from a
    **Linux/Windows** host (set host side to `10.55.0.2`). **Not** usable from
    Apple-Silicon macOS (no RNDIS driver).
  - **CDC-ACM serial console** — a root shell on `/dev/ttyGS0`, which enumerates
    **natively on macOS** as `/dev/cu.usbmodem*` (`screen /dev/cu.usbmodem* 115200`).
    This is the Mac-native "always reachable" path. The kernel's `android_usb`
    gadget exposes `adb/mtp/ptp/rndis/acm/mass_storage/…` functions; we select
    `functions=rndis,acm` with an IAD device class.
- **Hardware UART** (fallback, needs wiring) — kernel console is `ttyFIQ0`; on the
  10" that's **UART2 @ 1.5 Mbaud** on the main-board test pads (`VCC RX TX GND`).
- **Loader-mode forensic read (unit fully dead/frozen)** — hold RECOVERY + plug
  USB → Loader, then `rkdeveloptool rl 2359296 5242880 userdata.img` dumps the
  `userdata` (`/data`) partition; read it on macOS with `debugfs -R "cat
  /mmkinit-boot.log" userdata.img` (ext4, no mount). Init writes its boot trail
  there with **`O_SYNC`+`fsync`** so it survives a hard power-cut (delayed-alloc
  otherwise drops the un-synced log — which cost us a blind debugging round).

**Fast app iteration without reflashing:** init prefers an **OTA overlay** at
`/data/mmkeypad` over the factory binary and respawns it, so
`ssh root@<ip> 'cat > /data/mmkeypad' < mmk-app; kill <app-pid>` swaps the running
UI in seconds (crash-loop rollback quarantines a bad overlay to `/data/mmkeypad.bad`).
Grab the screen over SSH: `dd if=/dev/fb0 bs=<stride> count=<h>` then convert
RGB565 (7": 800×1280 → rotate; 10": 1280×800 direct).

**Peripherals confirmed live under our image (for future intercom/video):**
camera `/dev/video0` (NT99141 via RK CIF — enumerates, but `get cif ldo failed!`
so power needs sorting before capture) · audio ALSA card `RK29_RT3261` with
playback **and** capture PCMs (`pcmC0D0p/c`, `pcmC0D1p/c`).

## Control4 app stack (extracted from `/system/app`; APKs kept local, not committed)
- **`phoenix-navigator.apk` (121 MB)** — the main C4 Navigator UI.
- **`c4-launcher.apk` (17 MB)** — home/launcher.
- `c4-component-navigator.apk` (1.35 MB), `c4settings-provider.apk` (197 KB).

**Architecture:** Phoenix is a **hybrid app** — native shell (libs are
**armeabi-v7a only** → needs 32-bit ARM; newest arm64-only devices can't run it)
+ a **JS API bridge** (`assets/api/c4.js`, `manifest.json`) — the UI is largely
web-rendered and bridges to the **local Control4 agent** in `/system/control4`
(busybox + `control4_core.sh` / `control4_init.sh`), which is what actually talks
to the Director. So "sideload Phoenix on a stock tablet = Control4" **won't just
work** (needs the agent + 32-bit ARM + KitKat compat). Its real value is as a
**reference for how C4's official client talks to the Director** (the `c4.js` web
API + agent) — directly useful for the DriverWorks driver and the HA integration.

## Repurposing the T3 (second-life / LVGL panel)
Verdict from the teardown: **worth it as a lifecycle-extension of the INSTALLED
T3 base** (reuse quality in-wall glass + camera + speakers + PoE/120 V + mount
that's already on walls); **not worth it as a new-product platform** (EOL 2013
silicon you can't buy — that's what the P4 / RK3566 tiers are for).

**Compute headroom:** RK3188 (quad A9 + Mali-400 + 1 GB) is ~100× an ESP32 for a
2D LVGL UI — massively overkill, so the value is in *added* features. The standout:
the RK3188 **VPU does HW H.264 decode @1080p** → it can DISPLAY a Control4
door-station H.264 stream, the exact thing the ESP32-P4 tier CANNOT do (P4 has no
HW video decode). Caveat: **H.264 only, no H.265**; 720p camera; Mali-400 weak for
ML.

**Path A — keep RK3188, run our LVGL app on Linux (RECOMMENDED, lowest effort).**
No hardware work; reuses every connector + the vendor's already-working drivers.
- Our LVGL **UI code + PROTOCOL.md transport port as-is**; only the platform layer
  (display flush / touch / audio) gets a Linux backend instead of `esp_lvgl_port`.
- Reuse the **vendor 3.0.36 kernel** (display/GSL1680/RT3261/AP6330/VPU already
  work) → don't need a mainline bring-up. Swap Android userspace for a small
  Buildroot rootfs that launches LVGL on `/dev/fb0` + GSL1680 evdev + RT3261 ALSA.
- **Days-long PoC:** cross-compile an LVGL fbdev binary (armeabi-v7a — the device
  runs it), `adb push`, stop SurfaceFlinger to own `/dev/graphics/fb0` +
  `/dev/input`. Proves our UI on the panel with zero flashing.
- Precursor done this session: Control4 watchdog killed + C4 launcher disabled →
  the panel is a de-Control4'd, rooted Android that can host our binary. See JAILBREAK.md.

**Path B — custom carrier w/ a modern SoM (only if H.265 or a modern OS is required).**
There is **NO off-the-shelf drop-in board** — the T3 main board is a custom C4
design (bespoke outline, mount holes, connector positions, and the backbox header),
and RGB FFC pinouts are vendor-specific. A brain-transplant = a **designed carrier
PCB**: mate THIS panel's 40-pin RGB pinout + GSL1680 I²C + backlight PWM + the
20-pin (1.27 mm Cortex-20) backbox ribbon (Ethernet+power), carrying a parallel-RGB-capable module —
**RK3566/RK3568** (HW H.264+H.265, the sweet-spot upgrade), Allwinner T113/A133,
or i.MX8M. Real schematic/layout/mechanical work, not a purchase.

**Modern Android is a dead end:** RK3188 tops out at community Android 5.1.1 (2016)
— still too old for the HA app (needs Android 6.0 / API 23) or modern web. Skip it.

## Files here (binaries are `.gitignore`d — local only, Control4-proprietary)
- `firmware/00_head_fw.bin` (128 MB) — loader+parameter+misc+kernel+boot+recovery+backup
- `firmware/10_system.bin` (1 GB, ext4) — Android system + the C4 app stack
- `firmware/20_userdata.bin` (2.5 GB, ext4) — `/data` (was wiped by a C4 factory reset → `/data/app` empty)
- `apks/*.apk` — the four Control4 apps
- `control4-system/` — extracted `/system/control4` (the agent scripts + busybox)
- `hwinfo/build.prop` — Android build/hardware properties (tracked)
