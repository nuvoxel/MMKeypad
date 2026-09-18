#pragma once
#include <stdint.h>
#include <stdbool.h>

// "Halo" — the board's single onboard WS2812B RGB LED (board.h PIN_RGB_LED).
// Two roles: a nightlight / design accent (idle solid color) and a status light
// (a gentle breathing pulse, e.g. blue while an intercom call rings). A small
// worker task owns the LED; all entry points are safe to call from any task and
// are no-ops on boards without an onboard LED (PIN_RGB_LED undefined).
void halo_init(void);

// Idle ("nightlight"/halo) color, shown whenever not pulsing. 0,0,0 = off.
void halo_set_color(uint8_t r, uint8_t g, uint8_t b);

// Color of the breathing pulse (ring indicator). Defaults to blue.
void halo_set_pulse_color(uint8_t r, uint8_t g, uint8_t b);

// Overall brightness 0..100 (scales whatever color is shown).
void halo_set_brightness(uint8_t pct);

// Start/stop the gentle breathing pulse (uses the configured pulse color, e.g.
// blue while a call rings). Stopping returns to the idle color.
void halo_pulse(bool on);

// Fixed 12-color palette, same order as the Composer driver's Halo Idle/Call
// Color LISTs (driver-keypad/driver.xml) and its HALO_COLORS table (driver.lua).
// Settings persist a palette INDEX rather than raw RGB so the device and the
// driver always agree on a name -- there is no lossy RGB->name guess in either
// direction. A pure lookup, safe to call even on boards with no halo hardware
// (e.g. to build the settings report the UI/driver expect).
#define HALO_PALETTE_COUNT 12
extern const uint8_t HALO_PALETTE[HALO_PALETTE_COUNT][3];
extern const char *const HALO_PALETTE_NAMES[HALO_PALETTE_COUNT];
void halo_palette_rgb(uint8_t idx, uint8_t *r, uint8_t *g, uint8_t *b);

// Push g_settings' halo_idle_color/halo_ring_color/halo_brightness (config.h) to
// the LED. Call after ANY change to those fields regardless of source (loaded at
// boot, a driver-pushed `state`, or a local UI edit) so the LED and the persisted
// setting can never drift apart -- there is exactly one path that makes the halo
// show something.
void halo_apply_settings(void);
