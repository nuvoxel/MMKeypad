#!/bin/sh
# Grab the T3's framebuffer over SSH and convert to a PNG for UI tuning.
# The panel is 800x1280 portrait RGB565, viewed as 1280x800 landscape.
# Usage: tools/screenshot.sh [ip] [out.png]
set -e
IP="${1:-192.168.1.208}"
OUT="${2:-/tmp/t3_screen.png}"
KEY=~/.ssh/id_rsa
RAW=/tmp/fb.raw

ssh -p 22 -i "$KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -o ConnectTimeout=10 root@"$IP" "dd if=/dev/fb0 bs=1600 count=1280 2>/dev/null" > "$RAW"

python3 - "$RAW" "$OUT" <<'PY'
import sys
from PIL import Image
raw, out = sys.argv[1], sys.argv[2]
W, H = 800, 1280
data = open(raw, 'rb').read()
img = Image.new('RGB', (W, H))
px = img.load()
for y in range(H):
    base = y * W * 2
    for x in range(W):
        v = data[base + x*2] | (data[base + x*2 + 1] << 8)
        r = (v >> 11) & 0x1f; g = (v >> 5) & 0x3f; b = v & 0x1f
        px[x, y] = ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))
img = img.rotate(-90, expand=True)   # portrait fb -> landscape view
img.save(out)
print("wrote", out, img.size)
PY
