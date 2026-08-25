/* Linux board.h -- shadows firmware-idf/main/board.h (ESP-IDF pin maps +
 * sdkconfig). The shared app modules only read the feature flags below; the
 * T3's actual display/touch/audio bring-up is native Linux (fbdev/evdev/ALSA),
 * not pin-defined here. */
#pragma once

#define MMK_BOARD_NAME  "t3-7"   /* Control4 T3-7 (RK3188, glassedge7) — shown on the Settings/info page */
#define MMK_HAS_DISPLAY 1
#define MMK_HAS_TOUCH   1
#define MMK_HAS_AUDIO   1   /* Stage 1: ALSA (tinyalsa) local audio — chimes/announcements */
#define MMK_HAS_SIP     1   /* libre SIP UA (platform/sip_linux.c) */
#define MMK_NET_ETH     1   /* wired ethernet */
#define MMK_NET_WIFI    0   /* AP6330 present but unused for now */

/* Device-manifest attributes. These are the compile-time defaults; the T3
 * refines model + connectivity at runtime (SoC/panel detection, active iface),
 * since one build serves several Control4 glass models (T3-7 / T3-10 / …) on
 * the same RK3188 kernel. See platform/device_t3.c. */
#define MMK_MODEL   "Control4 T3-7"   /* default friendly model name */
#define MMK_SOC     "rk3188"          /* SoC family (refined from eFuse/cpuinfo) */
#define MMK_POWER   "wall"            /* power source: wall | poe | battery */

/* Max driver buttons the shared keypad UI (ui.c) sizes its arrays for. ESP
 * boards set this per-board in firmware-idf/main/board.h; the T3 uses this
 * shadow header instead, so it must define it too. 8 = the keypad-primary grid
 * validated on T3 glass. (Without it ui.c's s_kpLed[KP_MAX] fails to declare and
 * the whole shared UI won't compile for this target.) */
#define MMK_MAX_BUTTONS  8
