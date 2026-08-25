#!/usr/bin/env python3
"""Recover the Rockchip audio HAL's mixer route tables from a stripped .so.

The RK30 HAL drives ALSA through arrays of:

    struct route_setting { char *ctl_name; int intval; char *strval; };   /* 12 bytes */

terminated by a zeroed entry. The library is stripped, so we can't look the
tables up by symbol -- but the ctl_name pointers still point at real strings in
.rodata. So: index every string by address, then scan the image for runs of
12-byte records whose first word is a known string address. Any run long enough
to be a real table is one.

This gives us the stock, known-good capture route for the rt3261 -- the config
Android used when the microphone demonstrably worked.
"""
import struct
import sys
import re

path = sys.argv[1] if len(sys.argv) > 1 else "hal.so"
data = open(path, "rb").read()

# --- minimal ELF32 section parse ---------------------------------------------
(e_shoff,) = struct.unpack_from("<I", data, 0x20)
e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x2E)

sections = []
for i in range(e_shnum):
    off = e_shoff + i * e_shentsize
    name, stype, flags, addr, offset, size = struct.unpack_from("<IIIIII", data, off)
    sections.append({"name_off": name, "addr": addr, "off": offset, "size": size, "type": stype})

shstr = sections[e_shstrndx]
def secname(s):
    b = data[shstr["off"] + s["name_off"]:]
    return b[:b.index(b"\0")].decode("ascii", "replace")

for s in sections:
    s["name"] = secname(s)

def vaddr_to_off(va):
    for s in sections:
        if s["type"] != 8 and s["addr"] <= va < s["addr"] + s["size"]:   # not .bss
            return s["off"] + (va - s["addr"])
    return None

# --- index all NUL-terminated strings by virtual address ----------------------
strings = {}
for s in sections:
    if s["name"] not in (".rodata", ".data", ".data.rel.ro", ".data.rel.ro.local"):
        continue
    blob = data[s["off"]:s["off"] + s["size"]]
    for m in re.finditer(rb"[\x20-\x7e]{2,120}\x00", blob):
        va = s["addr"] + m.start()
        strings[va] = m.group()[:-1].decode("ascii", "replace")

# Mixer control names look like "Stereo ADC MIXL ADC1 Switch" -- capitalised
# words, often ending in Switch/Volume/Mux/Boost/Control.
def looks_like_ctl(v):
    return (len(v) > 3 and v[0].isupper()
            and not v.startswith("_Z")
            and ("Switch" in v or "Volume" in v or "Mux" in v or "Boost" in v
                 or "Control" in v or "Select" in v or "Gain" in v or "Playback" in v
                 or "Capture" in v))

# --- scan for arrays of {char*, int, char*} ----------------------------------
tables = []
for s in sections:
    if s["name"] not in (".rodata", ".data", ".data.rel.ro", ".data.rel.ro.local"):
        continue
    base, blob = s["addr"], data[s["off"]:s["off"] + s["size"]]
    i = 0
    while i + 12 <= len(blob):
        entries, j = [], i
        while j + 12 <= len(blob):
            p1, iv, p2 = struct.unpack_from("<IiI", blob, j)
            nm = strings.get(p1)
            if nm is None or not looks_like_ctl(nm):
                break
            sv = strings.get(p2) if p2 else None
            entries.append((nm, iv, sv))
            j += 12
        if len(entries) >= 2:
            tables.append((base + i, entries))
            i = j
        else:
            i += 4

print(f"recovered {len(tables)} route tables from {path}\n")
want = re.compile(r"ADC|Capture|Mic|BST|RECMIX|IN1|IN2|DSP|TxDP|IF2", re.I)
for addr, entries in tables:
    names = " ".join(e[0] for e in entries)
    if not want.search(names):
        continue                      # only show capture-ish tables
    print(f"--- table @ 0x{addr:x}  ({len(entries)} entries) ---")
    for nm, iv, sv in entries:
        print(f"    {nm:<38} = {sv if sv is not None else iv}")
    print()
