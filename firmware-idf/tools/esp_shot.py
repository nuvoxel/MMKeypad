#!/usr/bin/env python3
# Dev screenshot for the ESP display boards (s3 / ws43): triggers the firmware's
# MMK_SNAPSHOT hook, reads the LVGL framebuffer it streams over serial, and writes
# a PNG. The firmware (ui.c snap_task) frames it as:
#     <<SNAP w h stride>>\n  <raw RGB565 bytes>  \n<<SNAPEND>>\n
# and dumps once ~3.5s after boot, then whenever it receives a 0x02 byte.
#
# --tap takes coordinates in LOGICAL/on-screen space (what you see in the PNG).
# LVGL's indev pipeline auto-rotates raw touch input to match the display's
# software rotation, and our injected indev feeds "raw" coordinates — so in
# landscape (rotation 90, the firmware's default) a logical tap must be
# pre-rotated into native-frame coordinates before sending, or it lands on the
# wrong widget. This tool takes an initial snapshot to learn the current w/h,
# derives the transform from that (landscape => apply the rotation-90 inverse;
# portrait => identity), then sends the corrected raw tap. Only covers the
# default landscape rotation (90) — the flipped rotations (180/270) aren't
# auto-corrected; pass raw coordinates directly if you need those.
#
# Usage:
#   tools/esp_shot.py <serial-port> [out.png]              # capture a screenshot
#   tools/esp_shot.py <serial-port> --tap <x> <y> [out.png] # tap (x,y) then capture
#   e.g. tools/esp_shot.py /dev/cu.usbmodem5101 s3.png
#        tools/esp_shot.py /dev/cu.usbmodem5101 --tap 120 200 after.png
import sys, time, re
import serial
from PIL import Image

argv = sys.argv[1:]
port = argv.pop(0) if argv else "/dev/cu.usbmodem5101"
tap = None
if argv and argv[0] == "--tap":
    argv.pop(0)
    tap = (int(argv.pop(0)), int(argv.pop(0)))
out = argv.pop(0) if argv else "esp_shot.png"

s = serial.Serial(port, 115200, timeout=2)  # baud ignored on USB-CDC/JTAG
hdr = re.compile(rb"<<SNAP (\d+) (\d+) (\d+)>>")


def snapshot():
    s.reset_input_buffer()
    s.write(b"\x02")   # request a fresh snapshot (firmware also auto-dumps after boot)
    s.flush()
    deadline = time.time() + 30
    buf = b""
    while time.time() < deadline:
        buf += s.read(256)
        m = hdr.search(buf)
        if m:
            w, h, stride = int(m.group(1)), int(m.group(2)), int(m.group(3))
            buf = buf[m.end():]
            if buf[:1] == b"\n":
                buf = buf[1:]
            break
    else:
        print("timeout: no <<SNAP>> header (is MMK_SNAPSHOT flashed? try again)"); sys.exit(1)
    need = stride * h
    data = buf
    while len(data) < need and time.time() < deadline:
        data += s.read(min(4096, need - len(data)))
    if len(data) < need:
        print(f"short read: {len(data)}/{need}"); sys.exit(1)
    return w, h, stride, data[:need]


def send_tap(x, y):
    s.reset_input_buffer()
    s.write(bytes([0x03, (x >> 8) & 0xff, x & 0xff, (y >> 8) & 0xff, y & 0xff]))
    s.flush()
    time.sleep(0.4)   # let the click's event handler + relayout run before the shot


if tap is not None:
    lx, ly = tap
    w0, h0, _, _ = snapshot()   # learn current orientation first
    if w0 > h0:
        # Landscape (rotation 90, the default): raw_x=logical_y, raw_y=(native_h-1-logical_x).
        # native_h is the ORIGINAL (pre-rotation) height, i.e. this display's logical width.
        rx, ry = ly, w0 - 1 - lx
    else:
        rx, ry = lx, ly   # portrait: identity
    send_tap(rx, ry)

w, h, stride, data = snapshot()
s.close()
print(f"snapshot {w}x{h} stride={stride} ({len(data)} bytes)…")

# decode RGB565 (little-endian) → PNG, honoring stride
img = Image.new("RGB", (w, h))
px = img.load()
for y in range(h):
    base = y * stride
    for x in range(w):
        lo = data[base + x*2]; hi = data[base + x*2 + 1]
        v = lo | (hi << 8)
        r = (v >> 11) & 0x1f; g = (v >> 5) & 0x3f; b = v & 0x1f
        px[x, y] = ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))
img.save(out)
print(f"wrote {out} ({w}x{h})")
