/* Sim board.h -- shadows firmware-idf/main/board.h (ESP pin maps) AND the T3
 * platform/board.h. Put -I. (this sim dir) FIRST on the include path so this
 * wins. The headless snapshot sim only needs the feature flags the shared UI
 * (ui.c) and its headers (net.h/sip.h) read at compile time -- there is no real
 * display/audio/SIP here.
 *
 * SIP + audio are forced OFF so sip.h expands to inline no-ops (fewer stubs);
 * MMK_CAN_ROTATE stays on so the Settings "Orientation" control still builds. */
#pragma once

#define MMK_BOARD_NAME  "sim"
#define MMK_HAS_DISPLAY 1
#define MMK_HAS_TOUCH   1
#define MMK_HAS_AUDIO   0   /* no ALSA in the sim */
/* The sim has no SIP stack, but the intercom UI is gated on this -- with it 0 the
 * Intercom tile and picker cannot be previewed at all. sip.h still supplies inline
 * no-op stubs, so nothing is actually dialled. */
#define MMK_HAS_SIP     1
#define MMK_CAN_ROTATE  1   /* Settings orientation cycler compiles */

#define MMK_MODEL   "MMKeypad Sim"
#define MMK_SOC     "host"
#define MMK_POWER   "wall"

/* Sizes the shared keypad UI's button arrays (s_kpLed[] etc.). Matches the
 * ESP/T3 builds so layout math is identical. */
#define MMK_MAX_BUTTONS  8
