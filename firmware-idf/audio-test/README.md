# Audio bring-up / speaker + mic placement test

Standalone ESP-IDF image for bench-testing the **ES8311 codec + FM8002E amp +
MEMS mic** audio path on the lcdwiki 2.8" ESP32-S3 board — separate from the
shipping firmware in `../` so it can't disturb it. Use it to judge where the
speaker and mic should sit in the enclosure.

It does the first real Phase-3 audio bring-up: I2C control of the ES8311, I2S
full-duplex data, amp enable. Pins come from `../main/board.h` (single source of
truth). No UI, no WiFi.

## Build & flash

```sh
source ~/esp/esp-idf/export.sh
cd firmware-idf/audio-test
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

(First IDF flash on a unit still running the Arduino image may need BOOT-download
mode — hold BOOT, tap RESET — same as the main firmware.)

To go back to the real firmware afterward: `cd ../ && idf.py -p … flash`.

## What it does

Loops this cycle forever, logging each phase over USB serial:

1. **TONE SWEEP** (amp on) — 440 / 1000 / 2000 / 4000 Hz tones out the speaker.
   Listen for rattle/buzz and relative loudness as you move the speaker around.
2. **MIC METER** (amp off) — prints mic RMS / peak in dBFS with a bar, ~4×/sec
   for 6 s. Tap or speak near candidate mic-hole positions and watch the level.
   Amp is parked off here so the meter reads ambient/mic only, no speaker bleed.
3. **LIVE LOOPBACK** (amp on) — mic → speaker in real time for 8 s. Reveals
   acoustic coupling / feedback between a given mic + speaker placement.

## Tuning knobs (top of `main/audio_test_main.c`)

- `SAMPLE_RATE` — 16 kHz (voice/intercom-relevant). Bump to 48000 for full-range
  speaker evaluation.
- `esp_codec_dev_set_out_vol(s_dev, 75)` — DAC output volume %.
- `esp_codec_dev_set_in_gain(s_dev, 30.0)` — mic gain in dB.
- Tone list / durations in `phase_tone_sweep`.

## If there's no sound / no mic signal

- ES8311 I2C address: `board.h` has `0x18` (CE low). If bring-up asserts on the
  codec, scan the bus — some boards strap it to `0x19`.
- Amp polarity: `board.h` says `PIN_AMP_ENABLE` is **active LOW**; the codec cfg
  uses `pa_reverted = true`. Flip if the speaker is silent but the DAC is writing.
- I2S pins (MCLK/BCLK/WS/DOUT/DIN) and amp pin are all in `../main/board.h`.
