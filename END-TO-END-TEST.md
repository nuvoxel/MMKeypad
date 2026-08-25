# MMKeypad — bring-up checklist

A standalone bring-up: flash a keypad, add the driver in Composer, and verify the
end-to-end behaviour against a live Control4 Director. There is no account,
portal, or online step — everything here is local.

> Tip: set the driver's **Diagnostics Sink** property to an `http://<host>:<port>`
> you control to capture the driver's own trace while testing.

## A. Onboarding (the main flow)

1. **Flash the firmware.** `cd firmware-idf && ./board.sh <board> flash monitor`
   (see [`docs/HARDWARE.md`](docs/HARDWARE.md)). The device boots straight to the
   keypad UI — no activation screen.
   - CONFIRM: the UI shows, and the serial log shows the `:6700` server and SDDP
     announce starting.

2. **Add the driver.** Build it (`cd driver-keypad && ./build.sh`) so
   `NuVoxelKeypad.c4z` is in `~/Documents/Control4/Drivers`. In Composer Pro, add
   the keypad from **discovered (SDDP) devices**, or **System Design → Search →
   Add Driver**, and place it in the keypad's room.
   - CONFIRM: the driver appears with its `keypad` (5002) and `intercomproxy`
     (5003) proxies.

3. **Bind + connect.** Bind the **Media Keypad Network** connection to the device
   (SDDP auto-binds it; otherwise set the device's IP).
   - CONFIRM: **Link Status** → `Connected`; the device's now-playing screen shows
     the room's current media.

## B. Driver live-verifies

4. **Now-playing + transport.** Play media in the room; change track / volume /
   source from another controller.
   - CONFIRM: title/artist/album/art update on the panel, and transport / volume /
     source controls from the panel drive the room.

5. **Settings apply from Composer.** Change **Display Orientation**, **Active
   Brightness**, **Idle Timeout**, a **Show …** toggle.
   - CONFIRM: each takes effect on the device promptly (the driver pushes the
     change over `:6700` — no online round-trip).

6. **Programmable buttons.** Link a button to a device/scene in Composer's native
   Button Settings; on panels reporting >6 buttons (nano=12, ws43=8), reboot the
   Director.
   - CONFIRM: the button actuates its target, and the binding survives the reboot.

7. **Halo LED.** Change **Halo Color** / **Halo Brightness**.
   - CONFIRM: the RGB accent LED follows the property.

8. **Intercom.** With `driver-intercom` built and bound (needs the Control4 SDK
   proxy files — see [`driver-intercom/README.md`](driver-intercom/README.md)),
   place a Control4 intercom/announcement call to the room.
   - CONFIRM: two-way audio (or the announcement) plays on the keypad; the
     `intercomproxy` (5003) receives the SIP/call commands.

## C. Firmware update

9. **On-screen update.** Publish a newer build to GitHub Releases (see
   [OTA.md](OTA.md)); on the panel go to **Settings → Check for update**.
   - CONFIRM: the list shows the published version, and installing it downloads,
     applies, and reboots into the new firmware.
