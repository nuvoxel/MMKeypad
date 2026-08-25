#!/usr/bin/env bash
# Regenerate main/mmk_lucide.c: a subset of the Lucide icon font (MIT,
# lucide-static@1.22.0) trimmed to the ~50 PUA glyphs ui.c uses, embedded for
# lv_tiny_ttf icon scaling (see ui.c). The subset (lucide-mmk.ttf, ~19KB vs the
# full 841KB) is committed; this script rebuilds it from npm + pyftsubset when
# the glyph set changes, then embeds it. Keep the codepoint list in sync with
# gen-icons.sh's CPS24.
set -euo pipefail
cd "$(dirname "$0")"
OUT="../../main/mmk_lucide.c"
SUBSET="lucide-mmk.ttf"

# Codepoints ui.c uses (must match gen-icons.sh CPS24, without 0x).
U="E059,E064,E06D,E070,E0B7,E0D2,E0F2,E0F5,E0F9,E106,E10B,E10C,E118,E11E,E122,E12E,E133,E134,E13C,E140,E146,E154,E158,E15E,E15F,E160,E165,E166,E167,E176,E178,E186,E189,E18A,E195,E1A4,E1A5,E1AB,E1AC,E1AE,E1B2,E1C2,E379,E37F,E3C0,E3D6,E3E6,E412,E55A"

# Rebuild the subset only if it's missing (needs network + pyftsubset).
if [ ! -f "$SUBSET" ]; then
  TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
  ( cd "$TMP" && npm pack lucide-static@1.22.0 >/dev/null 2>&1 && tar xzf lucide-static-*.tgz )
  pyftsubset "$TMP/package/font/lucide.ttf" --unicodes="$U" \
    --output-file="$SUBSET" --no-hinting --desubroutinize
fi

{
  echo '/* Embedded Lucide icon subset (MIT), for lv_tiny_ttf icon scaling.'
  echo ' * Subset of lucide-static@1.22.0 to the ~50 glyphs ui.c uses (see'
  echo ' * gen-icons.sh CPS24). Regenerate: tools/fonts/embed-lucide.sh. */'
  echo '#include <stdint.h>'
  xxd -i -n mmk_lucide "$SUBSET" | sed 's/unsigned char/const uint8_t/; s/unsigned int/const unsigned int/'
} > "$OUT"
echo "wrote $OUT"
