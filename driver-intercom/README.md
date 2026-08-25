# NuVoxel Keypad Intercom — Control4 driver

The standalone intercom endpoint driver. It exists separately from
[`../driver-keypad/`](../driver-keypad/) because Control4's Communication agent
only enrols a third-party intercom when `intercomproxy` is the device's
**primary** proxy — which it cannot be inside the multi-proxy keypad driver.

This driver holds no network binding of its own. It reaches the device by
relaying SIP and call traffic through the keypad driver's existing `:6700`
connection over a control binding (`MMKEYPAD_INTERCOM`), because the firmware's
`:6700` server is single-client. The wire messages are documented in
[`../PROTOCOL.md`](../PROTOCOL.md).

## Build: you must supply Control4's proxy templates

`build.sh` packages an `intercom_proxy/` directory that is **not in this
repository**:

    intercom_proxy/intercom_command.lua
    intercom_proxy/intercom_constants.lua
    intercom_proxy/intercom_debug.lua
    intercom_proxy/intercom_notify.lua
    intercom_proxy/intercom_protocol.lua

Those files are Copyright Control4 Corporation, All Rights Reserved. They come
from Control4's DriverWorks SDK — the reference *Universal SIP Phone
(Communication)* driver, `intercom_universal.c4z` — and they define the
authoritative `intercomproxy` contract the Director expects. They are not ours to
redistribute, so you need SDK access to obtain them.

Once they are in place:

    ./build.sh          # -> NuVoxelKeypadIntercom.c4z

Of the five, `intercom_constants.lua` is used verbatim; the others are the
Control4 template with the `%DRIVER_DEVELOPER%` bodies filled in. Our side of
that integration is the host function `MMK_SendDevice()` provided by
[`driver.lua`](driver.lua), which the protocol layer calls instead of opening a
socket — so porting the fill-ins is a small job against whatever version of the
templates your SDK ships.

## Filename is load-bearing

Control4 fixes a driver's proxy set when the driver is first **added** to a
project. Changing the proxies under an already-installed driver corrupts the
project, so this must only ever ship under a filename that has never been
installed. `build.sh` enforces the output name.
