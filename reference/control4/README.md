# Control4 reverse-engineering notes

Everything here was worked out by **observing Control4's own devices on the wire**
— a Halo remote and a Director talking to each other on a private network, plus
the Director's own logs and process memory. Nothing here comes from Control4's
SDK, documentation, or source; where a Control4-authored file matters, it is
named and *not* included (see `../../driver-intercom/README.md`).

All of it was done on hardware we own, for one purpose: making a third-party
keypad interoperate with a system the owner already has. That is why the notes
read as they do — they answer "what does the Director actually send when a
source changes?", not "how is Control4 implemented?".

## What is here

| File | What it documents |
|---|---|
| [`media-commands.md`](media-commands.md) | The verified room/source command vocabulary the keypad driver uses — what actually works, tested live, versus what the docs imply |
| [`nowplaying-var1031.md`](nowplaying-var1031.md) | The schema inside room variable 1031 (`CURRENT_MEDIA_INFO`), including which field names the real source device rather than the zone |
| [`freeswitch-intercom.md`](freeswitch-intercom.md) | How Control4's intercom is wired on a Director (its internal FreeSWITCH, registration, groups and door stations) and where a third-party endpoint fits |
| [`screensaver-and-uidevice.md`](screensaver-and-uidevice.md) | Director screensaver behaviour and the `uidevice` proxy — including the settled finding that a third-party `uidevice` is instantiated but never commanded |
| [`tr2-remote/`](tr2-remote/) | Protocol capture and analysis scripts from a Halo remote, used to learn how it gets browse and now-playing data |

## Reading them

These are **working notes**, kept as they were written: dated, with the dead ends
left in. That is deliberate — the wrong turns are often the useful part, because
they record what looked right and wasn't.

Two things to know before you trust a line of it:

- **Device and room identifiers are examples, not constants.** Every `deviceId`,
  room id and variable id here came from one specific project. Yours will differ.
  What transfers is the *shape* — which command, which variable, which direction.
- **Some notes reference an internal helper** for driving a Director over SOAP and
  SSH. That helper is not published and neither are the access details; the notes
  still tell you what was sent and what came back, which is the part that matters.

Room names and per-person source labels have been replaced with generic ones
(`Bedroom`, `Music A`…). They were someone's home.
