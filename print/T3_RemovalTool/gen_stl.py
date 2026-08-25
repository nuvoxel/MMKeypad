#!/usr/bin/env python3
"""
Generate T3_RemovalTool STL(s) — a replica of the Control4 factory fork tool
that releases a T3 In-Wall touch screen from its power box.

Profile reverse-engineered from a ruler-calibrated photo of the real (stamped
sheet-steel) tool and verified by overlaying the outline back on the photo:
two tall OUTER prongs, two small central teeth flanking a wide central pocket,
and a bottom grip tab. Overall ~130 x 92 mm.

Use: two tall prongs slide up the wall behind the screen to release the two
bottom latch tabs; the central pocket clears the centre boss. Slide up, then
pivot the screen off the top tabs.

The real part is ~1 mm steel. A print must stay thin enough for the prongs to
enter the wall/screen gap, which fights print stiffness — see README. Thickness
is parametric here; we emit a couple of options.

No dependencies. Run:  python3 gen_stl.py
All dimensions in mm; +x right, +y DOWN (prong tips at y=0), extruded in +z.
"""
import struct

# Base outline traced from the photo, clockwise, symmetric about x=65. (x, y_down)
_BASE = [
    (2, 8), (8, 0), (18, 0), (18, 30), (38, 30), (38, 21), (41, 21),
    (41, 40), (89, 40), (89, 21), (92, 21), (92, 30), (112, 30),
    (112, 0), (122, 0), (128, 8), (128, 66), (103, 66), (103, 93),
    (27, 93), (27, 66), (2, 66),
]

# The two tall prongs must STRADDLE the power box. First fit test: the base
# gap (prong inner faces at x=18/112 -> 94 mm clear) was ~2-3 mm too narrow to
# clear the box. Shift each prong outboard by PRONG_SHIFT mm (widens the clear
# span by 2*PRONG_SHIFT and the overall width the same). Central features
# (shoulders, teeth, pocket, grip tab) are NOT moved. Bump this and rerun if
# it's still tight; drop it if the prongs no longer line up with the latch tabs.
PRONG_SHIFT = 2.0   # -> 98 mm clear span (box measured to need 96.5 mm; 98 leaves ~1.5 mm slip)
_LEFT = {0, 1, 2, 3, 21}    # left-prong + left body-side points -> shift -x
_RIGHT = {12, 13, 14, 15, 16}  # right-prong + right body-side points -> shift +x


def _widen(base, s):
    out = []
    for i, (x, y) in enumerate(base):
        if i in _LEFT:
            x -= s
        elif i in _RIGHT:
            x += s
        out.append((x, y))
    return out


OUTLINE = _widen(_BASE, PRONG_SHIFT)

THICKNESSES = [
    ("1p2", 1.2),   # fit-first: close to the 1 mm steel, most likely to enter the gap
    ("1p6", 1.6),   # stiffer: try if 1.2 is too floppy and the gap allows it
]


def signed_area(poly):
    s = 0.0
    for i in range(len(poly)):
        x0, y0 = poly[i]
        x1, y1 = poly[(i + 1) % len(poly)]
        s += x0 * y1 - x1 * y0
    return s / 2.0


def _tri_area2(a, b, c):
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])


def _point_in_tri(p, a, b, c):
    d1 = _tri_area2(p, a, b)
    d2 = _tri_area2(p, b, c)
    d3 = _tri_area2(p, c, a)
    neg = (d1 < 0) or (d2 < 0) or (d3 < 0)
    pos = (d1 > 0) or (d2 > 0) or (d3 > 0)
    return not (neg and pos)


def earclip(poly):
    """Triangulate a simple polygon (CCW). Returns list of (i,j,k) index tris."""
    n = len(poly)
    idx = list(range(n))
    tris = []
    guard = 0
    while len(idx) > 3 and guard < 10000:
        guard += 1
        ear = False
        m = len(idx)
        for a in range(m):
            i0, i1, i2 = idx[(a - 1) % m], idx[a], idx[(a + 1) % m]
            A, B, C = poly[i0], poly[i1], poly[i2]
            if _tri_area2(A, B, C) <= 0:      # reflex or degenerate (CCW convex > 0)
                continue
            if any(_point_in_tri(poly[idx[b]], A, B, C)
                   for b in range(m)
                   if idx[b] not in (i0, i1, i2)):
                continue
            tris.append((i0, i1, i2))
            del idx[a]
            ear = True
            break
        if not ear:
            break
    if len(idx) == 3:
        tris.append((idx[0], idx[1], idx[2]))
    return tris


def tri(v0, v1, v2):
    ux, uy, uz = (v1[i] - v0[i] for i in range(3))
    vx, vy, vz = (v2[i] - v0[i] for i in range(3))
    nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
    m = (nx * nx + ny * ny + nz * nz) ** 0.5 or 1.0
    return ((nx / m, ny / m, nz / m), v0, v1, v2)


def build(thickness):
    poly = OUTLINE if signed_area(OUTLINE) > 0 else OUTLINE[::-1]
    n = len(poly)
    bot = [(x, y, 0.0) for x, y in poly]
    top = [(x, y, thickness) for x, y in poly]
    cap = earclip(poly)
    tris = []
    for i, j, k in cap:
        tris.append(tri(bot[i], bot[k], bot[j]))   # bottom faces -z
        tris.append(tri(top[i], top[j], top[k]))   # top faces +z
    for i in range(n):
        j = (i + 1) % n
        # outward wall for CCW poly extruded +z
        tris.append(tri(bot[i], bot[j], top[j]))
        tris.append(tri(bot[i], top[j], top[i]))
    return tris


def write_stl(path, tris):
    with open(path, "wb") as f:
        f.write(b"\0" * 80)
        f.write(struct.pack("<I", len(tris)))
        for nrm, v0, v1, v2 in tris:
            f.write(struct.pack("<3f", *nrm))
            for v in (v0, v1, v2):
                f.write(struct.pack("<3f", *v))
            f.write(struct.pack("<H", 0))


if __name__ == "__main__":
    for suffix, t in THICKNESSES:
        tris = build(t)
        path = f"T3_RemovalTool_{suffix}mm.stl"
        write_stl(path, tris)
        print(f"wrote {path}  ({len(tris)} tris, {t} mm thick)")
