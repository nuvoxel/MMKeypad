#!/usr/bin/env bash
# Regenerate main/mmk_roboto.c: the embedded Roboto TTF bytes used by
# lv_tiny_ttf for resolution-aware runtime font scaling (see ui.c). Regular =
# body/small text, Medium = titles. Source is the Control4 X4.app C4UIKit
# bundle (the Navigator typeface, Apache-2.0) if present, else a local copy.
set -euo pipefail
cd "$(dirname "$0")"
OUT="../../main/mmk_roboto.c"
C4="/Applications/Control4.app/Wrapper/X4.app/C4UIKit_C4UIKit.bundle"

for f in Roboto-Regular.ttf Roboto-Medium.ttf; do
  [ -f "$f" ] || cp "$C4/$f" .
done

{
  echo '/* Embedded Roboto TTFs (Apache-2.0) for lv_tiny_ttf runtime scaling.'
  echo ' * Source: Control4 X4.app C4UIKit bundle (the Navigator typeface).'
  echo ' * Regenerate: tools/fonts/embed-roboto.sh. Regular=body/small, Medium=titles. */'
  echo '#include <stdint.h>'
  xxd -i -n mmk_roboto_regular Roboto-Regular.ttf | sed 's/unsigned char/const uint8_t/; s/unsigned int/const unsigned int/'
  xxd -i -n mmk_roboto_medium  Roboto-Medium.ttf  | sed 's/unsigned char/const uint8_t/; s/unsigned int/const unsigned int/'
} > "$OUT"
echo "wrote $OUT"
