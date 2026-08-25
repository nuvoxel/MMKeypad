#pragma once
#include <stdint.h>
#include <stdbool.h>

// "Halo" — the board's single onboard WS2812B RGB LED (GPIO42 on the lcdwiki S3).
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
