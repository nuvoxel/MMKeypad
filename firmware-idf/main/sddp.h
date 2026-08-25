#pragma once
#include <stdint.h>

// Control4 SDDP (Simple Device Discovery Protocol) — multicast 239.255.255.250:1902.
// Lets Composer/the Director discover the keypad. Starts its own task; waits for
// an IP before joining the group.
void sddp_start(uint16_t control_port);
