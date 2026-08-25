# T3 camera (NT99141 / RK3188 CIF) — capture bring-up findings

Field notes from getting `/dev/video0` to produce frames from our own bare-Linux
userspace on the repurposed T3-10. **All panels involved are dev units** — no
production installs — so experiment freely.

## TL;DR status

- **Capture WORKS on stock firmware.** A real 1280×720 NV12 frame was captured
  from `/dev/video0` via our own static V4L2 program on a T3-10 running stock
  Android (kernel `61cd5393…`, with the `rk29-ipp` module loaded). Proven with a
  real image (ceiling/plant scene), sentinel-verified (our `0xAA` fill was
  overwritten with live data, frame N ≠ frame N+1).
- **Capture is BLOCKED on our custom Linux** because the `rk29-ipp` kernel module
  isn't loaded there — see [The IPP dependency](#the-ipp-dependency) and
  [The kernel/vermagic wall](#the-kernelvermagic-wall). This is the open problem.

## The capture mechanism (this is the non-obvious part)

The RK30/RK3188 CIF driver (`rk30_camera_oneframe.c`, `rk_cam_cif`) does **not**
accept standard `V4L2_MEMORY_MMAP` or `V4L2_MEMORY_USERPTR` from userspace. Its
`buf_prepare` gates on `vb->boff != 0`, and only the **`V4L2_MEMORY_OVERLAY`**
QBUF path sets `boff` (videobuf-core: `case V4L2_MEMORY_OVERLAY: buf->boff =
b->m.offset`). So the working sequence is:

1. `VIDIOC_S_FMT` NV12 1280×720 (sensor native).
2. `VIDIOC_REQBUFS` with `.memory = V4L2_MEMORY_OVERLAY`.
3. Allocate a physically-contiguous buffer from **`/dev/ion`** (the vendor
   carveout "norheap", `ION_NOR_HEAP_ID = 0`), get its **physical address** via
   the Rockchip-private `ION_CUSTOM_GET_PHYS` ioctl, and `mmap` its fd for CPU
   readback.
4. `VIDIOC_QBUF` with `.memory = V4L2_MEMORY_OVERLAY` and **`buf.m.offset =
   phys_addr`** (the ion physical address, NOT a virtual pointer).
5. `STREAMON` → `DQBUF` → read the frame from the mmap'd VA.

Confirmed two independent ways: reading the driver source AND the actual stock
HAL source (`Nu3001/hardware_rk29_camera/CameraHal.cpp`: `CAMERA_IS_RKSOC_CAMERA`
→ `mCamDriverV4l2MemType = V4L2_MEMORY_OVERLAY`, `buffer.m.offset =
phy_addr`). UVC webcams use MMAP; the on-SoC CIF sensor uses OVERLAY.

## The ABI bug that blocked everything first (GENERAL GOTCHA)

Before any of the above worked, plain `VIDIOC_QUERYBUF`/`QBUF` failed `EINVAL`
with no driver dprintk — because **zig/musl uses 64-bit `time_t`**, making
`struct timeval` 16 bytes and `struct v4l2_buffer` **80 bytes** where the 3.0.36
kernel expects **68** (32-bit `time_t`, 8-byte timeval). That (a) shifts every
field after `timestamp` and (b) bakes the wrong size into the `_IOC` number
(`VIDIOC_QBUF` came out `0xc050560f` vs the kernel's `0xc044560f`), so the kernel
rejects at `check_fmt` before reaching videobuf.

**Fix:** for any ioctl whose struct contains a `timeval` (v4l2_buffer, and watch
for others), hand-roll a kernel-exact struct with a 32-bit `{long tv_sec,
tv_usec}` timeval and recompute the ioctl number with that struct. `REQBUFS`/
`S_FMT` were fine (no timeval → 20/204 bytes, correct). **This affects the whole
firmware**, not just the camera — any timeval-bearing kernel ABI is at risk.

## The IPP dependency

The CIF driver captures sensor→CIF→`vipmem` (a reserved 6MB PMEM region), then
copies vipmem→our buffer via **`ipp_blit_sync()`** (the RK29 IPP 2D block — the
version string even says "Zoom by ipp"). Critically, `ipp_blit_sync` is a
**builtin kernel symbol with an `ipp_blit_sync_default` no-op stub**: without the
`rk29-ipp` module loaded, the blit silently does nothing, `DQBUF` still returns
`VIDEOBUF_DONE`, and **our buffer is never written** (sentinel `0xAA` stays
intact). That is exactly the failure on our custom Linux.

Stock Android's `init.rc` loads the whole vendor graphics/media stack that our
init does not:
```
insmod ump.ko  mali.ko  rk30_mirroring.ko  rk29-ipp.ko  vpu_service.ko
```
For our roadmap: **`rk29-ipp` (camera, now)** and **`vpu_service` (H.264
door-cam decode, later)** are the ones that matter; `mali`/`ump` are GPU-only
(we render via fbdev/LVGL, so optional).

`firmware-linux-t3/init/init.c` now has these `insmod`s wired in (after `/system`
mounts) via a new `insmod_p` helper — but they are **currently no-ops** because
the running kernel rejects the modules (next section). They're staged for when
the kernel issue is resolved.

## The kernel/vermagic wall (the actual open problem)

The vendor graphics/media blobs (`mali`, `ump`, `rk29-ipp`, `vpu_service`) carry
vermagic **`3.0.8+`**, while our units run a **`3.0.36+`** kernel. Only the WiFi
and NAND modules were rebuilt at 3.0.36.

- Stock loads the 3.0.8 blobs anyway because its kernel has **`CONFIG_MODVERSIONS`**
  — it ignores the version string and CRC-checks symbols instead.
- The kernel our `firmware-linux-t3` repacks against (**`12b283f2…`**) does
  **strict full-string vermagic** (no MODVERSIONS), so it rejects them
  (`version magic '3.0.8+' should be '3.0.36+'`). A faithful `init_module` load
  fails identically — it's the kernel, not the loader.

So the camera needs a kernel that is BOTH able to run our hardware AND has
MODVERSIONS to load the IPP blob. Two candidate paths (unresolved):

1. **Rebuild `rk29-ipp.ko` (and `vpu_service.ko`) from source** against our exact
   `12b283f2` kernel so vermagic matches. Source is public
   (`olegk0/rk3x_kernel_3.0.36`, `Nu3001/kernel_rk3188`); toolchain is GCC
   4.6–4.8. Most promising, least risky. **← recommended next step, on a spare.**
2. Find/flash a kernel that both boots our hardware and has MODVERSIONS. Risky —
   see the hardware-variance note; the "wrong kernel" theory below was a dead end.

### Dead end to not repeat: the `61cd5393` kernel swap

A long detour assumed `12b283f2` was the "wrong kernel" and tried swapping in
`61cd5393…` (dumped from a *different* unit, which has MODVERSIONS). This was
wrong: `12b283f2` is the correct, working kernel for our units (boots our Linux +
display + app fine, and always did). The swap attempt never cleanly completed
(interrupted write, wedged loader) and a coincident **loose display cable**
produced a "black screen" that was misread as a kernel/display failure. **Whether
`61cd5393` even boots our hardware was never actually confirmed** — treat it as
unknown, and don't chase it. Trust "it worked before with `12b283f2`."

## Hardware variance across "identical" T3-10 units (IMPORTANT)

Two T3-10 units reporting the *identical* kernel build string
(`#1-glassedge10.2.0 … Thu Mar 13 02:06:56 MDT 2025`) had **different NAND**:

| unit | NAND | page | block |
|---|---|---|---|
| one dev unit | **MICRON** | 8 KB | 2048 KB |
| another dev unit | **HYNIX** | 16 KB | 4096 KB |

The MICRON unit was also **dramatically slower under the rkdeveloptool loader**
(≈30 KB/s vs 16 MB/s — minutes per 12MB op). Implication: **the web flasher and
firmware must be NAND / hardware-rev aware** — a single boot image + module set
may not fit every T3-10, and loader timeouts need to tolerate slow chips.

## Reproducing the capture (approach)

The working capture program (static ARM, `zig cc -target
arm-linux-musleabihf -mcpu=cortex_a9 -static`) does: S_FMT → REQBUFS(OVERLAY) →
ion alloc from norheap + GET_PHYS → mmap ion fd → QBUF(OVERLAY, m.offset=phys) →
STREAMON → DQBUF → read VA. Uses kernel-exact `k_v4l2_buffer` (68 bytes) and
hand-computed `K_VIDIOC_QBUF/DQBUF`. ion structs/ioctls transcribed from
`drivers/gpu/ion/ion.h` (`ION_IOC_ALLOC`, `ION_IOC_MAP`, `ION_CUSTOM_GET_PHYS =
_IOWR('I',7,struct ion_phys_data)`). Sensor native mode is 1280×720 NV12; give it
~30 frames of warmup for auto-exposure.
