#!/usr/bin/env python3
# Deobfuscate a Control4 tr3 node service (cerebellum.js / broker/*.js). Control4
# hides identifiers as base64 entries in a string-array; decoding them yields the
# cleartext method names, endpoints, error codes, etc.
#
#   python3 decode-c4-node.py cerebellum.js [regex]
#
# With a regex, prints only decoded strings matching it (case-insensitive), e.g.
#   python3 decode-c4-node.py cerebellum.js 'hmac|nonce|pin|register|token|/[a-z]'
import re, base64, sys

data = open(sys.argv[1]).read()
pat  = re.compile(sys.argv[2], re.I) if len(sys.argv) > 2 else None

out = []
for t in set(re.findall(r'[A-Za-z0-9+/]{8,}={0,2}', data)):
    if len(t) % 4:
        continue
    try:
        s = base64.b64decode(t).decode('ascii')
    except Exception:
        continue
    if s.isprintable() and re.search(r'[A-Za-z]', s):
        out.append(s)

out = sorted(set(out))
if pat:
    out = [x for x in out if pat.search(x)]
print(f"# {len(out)} decoded strings"
      + (f" matching /{sys.argv[2]}/" if pat else "") , file=sys.stderr)
for x in out:
    print(x)
