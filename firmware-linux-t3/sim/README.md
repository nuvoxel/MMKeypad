# MMKeypad UI sim — headless LVGL snapshot harness

Render the **real** shared `ui.c` to PNGs on a plain macOS/Linux dev box — no
ESP flash, no T3 hardware, no display. Use it to see and iterate on the LVGL
layout across every panel size/orientation before building firmware.

```sh
cd firmware-linux-t3/sim
./render.sh            # builds + renders the 5 canonical panels into shots/
open shots/            # (macOS) eyeball them
```

`render.sh` renders **every page on every supported panel** (`shots/<panel>-<page>.png`).
Layout follows ORIENTATION, not board identity: portrait is a single column,
landscape is two columns, and `s_uiscale` carries the size. The WS43's "Screen
Rotation" driver property makes both of its rows real, deployed configs (the
office install runs landscape) -- a hero-circle layout that overflowed a
480px-tall screen shipped to that exact panel unnoticed when only portrait was
rendered. Always look at every orientation a board can be set to.

| prefix            | logical size | ui.c layout | real panel                          |
| ----------------- | ------------ | ----------- | ----------------------------------- |
| `ws43-portrait`   | 480×800      | portrait    | Waveshare 4.3" (primary target)     |
| `ws43-landscape`  | 800×480      | landscape   | Waveshare 4.3" rotated              |
| `t3-landscape`    | 1280×800     | landscape   | T3 7"/10", P4 nano 10.1"            |

The T3 has no portrait layout: `bsp_linux.c` rotates a portrait framebuffer
(the 7") into landscape itself, so `ui.c` only ever sees 1280×800 there. The
240×320 S3 panel is no longer a UI target.

## One-off render

```sh
make
./build/mmk-sim <width> <height> <out.png>
./build/mmk-sim 800 480 shots/custom.png
```

`<width> <height>` are the **logical** (post-rotation) dimensions `ui.c` sees —
i.e. what `lv_display_get_horizontal_resolution()` returns.

## Preview knobs (env vars)

Tweak the mock state/settings without touching code:

| env            | values                              | effect                          |
| -------------- | ----------------------------------- | ------------------------------- |
| `MMK_LAYOUT`   | 0 Cover / 1 Fit / 2 Compact         | `g_settings.layout`             |
| `MMK_BG`       | 0 Navigator / 1 Ocean / 2 Dusk / 3 Graphite | `g_settings.bg_preset`  |
| `MMK_PLAYING`  | 1 playing / 0 idle                  | now-playing vs powered-down     |
| `MMK_PIN`      | 1 (with `MMK_SEC_PANEL=1`)          | open the arm-code PIN pad       |
| `MMK_SEC_ID`   | partition id (with `MMK_SEC=2`)     | open one partition's page; 2685 is the armed sample |
| `MMK_CHURN`    | 1 replay pushes / 2 also force rebuilds | regression check for update fights: flips a favourite, a button LED and playback WHILE a page is open (the page must survive), and prints the top-layer object count across rebuilds (must not grow) |

```sh
MMK_PLAYING=0 ./build/mmk-sim 480 800 shots/idle.png
```

## How it works (and its limits)

- Compiles `firmware-idf/main/ui.c` **verbatim** (via `shared/ui.c`, like the T3
  build) + the generated fonts + LVGL 9.3 core, against a dummy in-memory
  `lv_display`. After `ui_begin()` + a canned `ui_set_state()`, it forces a
  layout pass and calls `lv_snapshot_take(..., ARGB8888)`, then writes a PNG.
- **`stubs.c`** replaces the non-LVGL surface `ui.c` links against
  (`net_*`, `art_*`, `device_*`, `bsp_*`, `settings`) with canned, inert values.
  Album art is disabled → you'll see the placeholder tile, not real cover art.
  SIP/audio are compiled out (`board.h` `MMK_HAS_SIP/AUDIO 0`).
- **Static snapshot only** — no touch, no animation, no live driver. It shows a
  frame, not an interactive UI. (For clicking through screens, the next step up
  is an Emscripten/WASM build driven in a browser.)

### Files

- `render_main.c` — the harness (display setup, mock state, snapshot→PNG).
- `stubs.c` — canned `net/art/device/bsp/settings` + the embedded-PNG linker syms.
- `board.h`, `bsp.h` — sim shadows (feature flags; two no-op bsp calls).
- `lv_conf.h` — **generated** from `../lvgl-app/lv_conf.h` with 4 flags flipped:
  `LV_USE_SNAPSHOT 1`, `LV_USE_LINUX_FBDEV 0`, `LV_USE_EVDEV 0` (the last two so
  the LVGL Linux backends' `<linux/*.h>` includes don't break a macOS host), and
  `LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB` (host malloc, so big snapshots don't hit
  the 4 MB LVGL pool). Regenerate when the T3 config changes:
  ```sh
  sed -E -e 's/^#define LV_USE_SNAPSHOT 0/#define LV_USE_SNAPSHOT 1/' \
         -e 's/^#define LV_USE_LINUX_FBDEV 1/#define LV_USE_LINUX_FBDEV 0/' \
         -e 's/^#define LV_USE_EVDEV 1/#define LV_USE_EVDEV 0/' \
         -e 's/^#define LV_USE_STDLIB_MALLOC    LV_STDLIB_BUILTIN/#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CLIB/' \
         ../lvgl-app/lv_conf.h > lv_conf.h
  ```
- `shared/` — symlinks to the app `.c`/`.h` (minus `board.h`/`bsp.h`, so those
  resolve to the sim shadows via the include path — same trick the T3 build uses).

The first build compiles the whole LVGL tree and is slow (~1 min); rebuilds after
editing `ui.c` recompile everything too (no object caching). If iteration gets
painful, precompile LVGL to a static lib.
