#pragma once
#include <stdint.h>

// Control4 SDDP (Simple Device Discovery Protocol) — multicast 239.255.255.250:1902.
// Lets Composer/the Director discover the keypad. Starts its own task; waits for
// an IP before joining the group.
void sddp_start(uint16_t control_port);

// The name we announce as -- "<MODEL>-<hardware_id>", e.g. "WS43-e8f60ae4207c".
// Control4 stores this string in the driver's UUID binding, so it is the value to
// check when a panel is on the network but Director will not discover it: a
// mismatch against the stored binding means Director is waiting for a UUID nobody
// sends. Empty until sddp_start() has run. See the block above its construction.
const char *sddp_host(void);
