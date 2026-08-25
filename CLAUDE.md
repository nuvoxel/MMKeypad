# CLAUDE.md — MMKeypad

Conventions for AI agents working in this repo. Read [README.md](README.md)
first for what the project *is*; this file is about how not to break it.

## This repo is the only source

MMKeypad used to be half of a closed product with a hosted backend. It isn't any
more: **this repository is canonical**, it talks to no online service, and there
is no private mirror to sync with. If you find a doc telling you to edit
something "in the other repo first", that doc is stale.

The company's private repo still holds the physical design (KiCad), Control4's
SDK templates, and its own Director-access notes. Nothing here depends on it.

## The shape

- `firmware-idf/` — ESP-IDF + LVGL, one tree, five boards via `./board.sh <b>`.
  A bare `idf.py build` is wrong: each board needs its own `MMK_BOARD`, target
  and sdkconfig, and they clobber each other's build dir otherwise.
- `firmware-linux-t3/` — the same UI and protocol code cross-compiled for
  RK3188 panels. `lvgl-app/shared/*` are **symlinks into `firmware-idf/main/`** —
  edit the ESP copy, never the link.
- `driver-keypad/`, `driver-intercom/` — DriverWorks Lua, packaged by each
  directory's `build.sh`.
- `docs/`, `reference/` — design notes and the Control4 RE writeups.

## Things that will bite you

- **The device is the TCP server** (`:6700`); the driver dials it. Not the other
  way round.
- **[PROTOCOL.md](PROTOCOL.md) is the contract.** Change both sides together,
  and ignore unknown message types rather than erroring — that is what makes
  mixed-version fleets survivable.
- **A driver's proxy set is frozen once installed.** Adding, removing or
  retyping a `<proxy>` corrupts an installed project; it has crashed a Director.
  That is why the intercom is a separate driver — see
  [docs/INTERCOM-SPIKE-FINDINGS.md](docs/INTERCOM-SPIKE-FINDINGS.md). Connections
  may only be *added*. Don't remove or rename events, commands or variables.
- **Registering is not enrolling.** A SIP endpoint that registers fine still
  answers "Can't find user" until Control4's Communication agent re-scans. See
  [docs/INTERCOM-ENROLLMENT.md](docs/INTERCOM-ENROLLMENT.md).
- **Don't add polling.** Streaming audio once made a Director "basically
  unusable"; [docs/DATA-PLANE.md](docs/DATA-PLANE.md) is the post-mortem and the
  design that replaced it.
- **Versioning** is date-based, `YYYY.MM.DD.NNN` + `FW` or `DRV`, from
  `tools/nvversion.sh`. `PROTO_VERSION` is *not* this — it is the wire-compat
  integer, bumped by hand only on a breaking PROTOCOL.md change.

## What must not be committed

- Control4's DriverWorks templates (`driver-intercom/intercom_proxy/*.lua`) —
  Copyright Control4, supplied by the user from the SDK.
- Extracted Control4 firmware, APKs or `/system` contents, and the TR2
  Navigator templates. The `.gitignore` rules that exclude them are load-bearing;
  removing one un-ignores proprietary files, and a later `git add -A` will
  quietly commit them.
- Anything identifying a real installation. The RE notes were deidentified on
  the way out — generic room names, `Music A`… for per-person sources,
  placeholder serials and MACs. Don't paste raw project dumps back in.
