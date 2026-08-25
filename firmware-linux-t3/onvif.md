# ONVIF camera on the T3 — feasibility & plan

Goal: make the repurposed Control4 T3 (RK3188 glass) *also* present itself as a
standard **ONVIF Profile S** IP camera, so Control4 / any NVR (Frigate, Blue Iris,
Synology…) can auto-discover it, pull an H.264 RTSP stream, and receive motion
events. Turns every wall keypad into a house-wide 720p camera + motion sensor.

Status: **scoped, not built.** The hardware pieces are confirmed present (below).

## What's on the panel (verified on a live T3, Kitchen 192.168.174.207, 2026-07-26)

- **Camera:** Novatek **NT99141** (1.3MP / 720p) on the RK3188 CIF interface
  (`/dev/video0`), scaled/rotated by the IPP (`/dev/rk29-ipp`). Kernel log:
  `rk_cam_cif … rk3066b-camera v0.3.25 Zoom by ipp`, `sensor_probe … nt99141.c`.
  Outputs **raw YUV** — the sensor gives no MJPEG/H.264, so something must encode.
- **VPU:** `/dev/vpu_service` present and initialized — `VPU_SERVICE: init success`
  (hw id 4831). The RK3188 VPU is one block with **both** a decoder (VDPU) *and* an
  H.264 **encoder (VEPU)**. Hardware encode — what a camera actually needs — is
  therefore available. (Note: `vpu_service` is **built into the kernel**; do NOT
  insmod the module build — that double-reserves vepu_io/vdpu_io and panics. See
  the init/firmware notes.)
- **Today:** firmware does **not** encode video. The keypad intercom is audio-only
  (no SDP `m=video`; only an audio-RTP `Encode:` path in the app). So nothing
  currently owns the camera — an ONVIF streamer can have `/dev/video0` to itself.
- **CPU:** 4× ARMv7 (RK3188, ~1.4–1.6 GHz). Modest, but fine once the VPU does the
  heavy lifting.

## What ONVIF takes — 4 layers

1. **Capture** — ✅ already solved. The self-test (`tools/selftest/mmk-selftest-camera.c`)
   grabs YUV frames via V4L2 + ION overlay (`V4L2_MEMORY_OVERLAY`, 68-byte
   `k_v4l2_buffer`).
2. **Encode** — YUV → H.264 on the VEPU. **This is the crux and the risk** (below).
3. **RTSP/RTP server** — serve the H.264 stream. Reuse OSS: `live555` (tiny,
   static-linkable) with a custom framer fed by our encoder, or patch
   `v4l2rtspserver` (mpromonet). H.264-over-RTP is well-trodden.
4. **ONVIF web services + WS-Discovery** — so NVRs/Control4 auto-find it. Reuse
   **`onvif_srvd`** (small gSOAP daemon: Device + Media services + WS-Discovery),
   pointed at our RTSP URL, plus WS-UsernameToken auth. Cross-compile gSOAP for
   ARMv7 musl.

Layers 3–4 are mostly integration of existing code. **Layer 2 is the real work.**

## The encode risk (honest)

Efficient path = Rockchip's userspace VPU encoder lib (`libvpu` / `vpu_api`) driving
VEPU H.264. Catch: the stock lib is an **Android/bionic blob**, and our rootfs is
**musl** — the same mismatch that made `tinyplay` segfault. Ways through, best first:

- **Build a musl-compatible VEPU encoder** from Rockchip's open `libvpu` sources
  (RK3188-era, pre-MPP) and drive `/dev/vpu_service` directly. (MPP/`libmpp` is
  RK3288+, does **not** cover RK3188 — use the older on2/`vpu_api`.)
- A thin **bionic-compat shim** to load the vendor blob.
- **SW `x264` fallback** at 480p/10fps — quick working demo, but burns CPU; stopgap
  only.

**First move: a one-frame VEPU encode spike** — get a single H.264 NAL out of the
encoder on our rootfs. That derisks ~60% of the project; everything after is
plumbing.

## Motion detection — nearly free

Once the VEPU is encoding it already computes **per-macroblock motion vectors**. Tap
those (or bitrate spikes) → threshold → fire **ONVIF motion events** (Profile S
`MotionAlarm` topic). Near-zero extra CPU; Control4 / Frigate / Blue Iris get real
motion triggers (lights, recording, notifications). Fallback: cheap luma frame-diff
on a downscaled plane. Either way each keypad becomes a **camera *and* a motion
sensor**.

## Integration notes

- **Camera contention:** single V4L2 consumer. Free today (intercom is audio-only).
  If video intercom is added later, need a capture-fanout (capture once → encode/use
  twice) or mutual exclusion.
- **Packaging:** cross-compile for the RK3188 musl rootfs (ARMv7); add the daemon to
  the init spawn list; gate via a settings/driver toggle + credentials + resolution.
- **Orientation:** camera is fixed in the housing; use the IPP to rotate upright.
- **Control4 side:** ONVIF → C4 sees it via a generic ONVIF/IP-camera driver (or
  Chowmain's). Alternatively our own `NuVoxelKeypad` driver could expose the RTSP URL
  directly. ONVIF is the universal path (works with any NVR).

## Effort

- **~1–2 weeks** for a working ONVIF Profile S camera (VEPU encode ≈ most of it),
  **+ a few days** for motion-vector events — *contingent on the encode spike
  succeeding*.
- **Lighter alternative** (just want the cam in Control4, skip ONVIF): our driver
  exposes an **MJPEG-over-HTTP** stream (JPEG per frame via IPP / soft-encode, no
  VPU). Quick; higher bandwidth / lower quality. ONVIF+H.264 is the "real" answer.

## Next step

Run the **VEPU one-frame H.264 encode spike** on a T3 to settle the single real
unknown (vendor-lib/musl) before committing.
