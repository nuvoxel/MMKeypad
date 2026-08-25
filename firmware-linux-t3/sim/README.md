# MMKeypad UI sim — headless LVGL snapshot harness

Render the **real** shared `ui.c` to PNGs on a plain macOS/Linux dev box — no
ESP flash, no T3 hardware, no display. Use it to see and iterate on the LVGL
layout across every panel size/orientation before building firmware.

```sh
cd firmware-linux-t3/sim
./render.sh            # builds + renders the 4 canonical panels into shots/
open shots/            # (macOS) eyeball them
```

`render.sh` emits one PNG per layout "flavor" `ui.c` picks by resolution:

| file                    | logical size | ui.c flavor | real panel            |
| ----------------------- | ------------ | ----------- | --------------------- |
| `shots/nano-landscape`  | 1280×800     | `x4L`       | P4 nano 10.1"         |
| `shots/ws43-portrait`   | 480×800      | `x4P`       | Waveshare 4.3"        |
| `shots/s3-landscape`    | 320×240      | `x4Ls`      | lcdwiki 2.8" (rot)    |
| `shots/s3-portrait`     | 240×320      | `smallP`    | lcdwiki 2.8"          |

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
| `MMK_THEME`    | 0 Control4 X4 / 1 Home Assistant    | `g_settings.theme`              |
| `MMK_LAYOUT`   | 0 Cover / 1 Fit / 2 Compact         | `g_settings.layout`             |
| `MMK_BG`       | 0 Navigator / 1 Ocean / 2 Dusk / 3 Graphite | `g_settings.bg_preset`  |
| `MMK_PLAYING`  | 1 playing / 0 idle                  | now-playing vs powered-down     |

```sh
MMK_HOME=1 MMK_THEME=1 ./build/mmk-sim 480 800 shots/keypad-ha.png
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
