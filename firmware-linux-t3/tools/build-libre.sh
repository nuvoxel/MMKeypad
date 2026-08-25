#!/usr/bin/env bash
# Cross-build libre (+ the bundled librem) for the T3 (armv7 musl, static) as the
# Phase-3 SIP intercom stack, replacing the ESP32's esp_rtc. Fetches the source
# tree into the (gitignored) thirdparty/ dir on first run, then builds:
#   thirdparty/libre/libre.a   (SIP + SDP + RTP + G.711, static ARM)
#
# The lvgl-app Makefile links this .a; it runs this script if the .a is missing.
#
# Why libre and not baresip: baresip is an application framework whose codec and
# audio modules load via dlopen, which a single STATIC binary can't do. libre
# alone already gives the whole surface we need — sipreg (REGISTER), sipsess
# (INVITE/answer/BYE), sdp, rtp — and the bundled librem adds G.711 µ-law coding
# plus an aubuf jitter buffer, so we don't hand-roll ITU tables like sip.c does
# on the ESP32.
#
# CRYPTO: libre ships a first-class mbedtls backend (the MBEDTLS_MD_C branches in
# md5/sha/hmac/rand). We already link mbedtls-3.6.2 for the album-art HTTPS
# fetch, so -DUSE_MBEDTLS gets digest auth working with NO OpenSSL and no shim.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TP="$ROOT/thirdparty"
LIBRE="$TP/libre"
MBED="$TP/mbedtls-3.6.2"
LIBRE_REPO="https://github.com/baresip/re.git"
LIBRE_TAG="v3.21.0"
CC="zig cc -target arm-linux-musleabihf -mcpu=cortex_a9"

mkdir -p "$TP"

# 0) Fetch on first run (gitignored — not ours to commit).
if [[ ! -d "$LIBRE/src" ]]; then
  echo "==> fetching libre $LIBRE_TAG"
  git clone --depth 1 --branch "$LIBRE_TAG" "$LIBRE_REPO" "$LIBRE"
fi

# 0b) Apply our local libre change. thirdparty/ is gitignored and fetched fresh,
# so this MUST be reapplied here or the fix silently disappears on a new checkout.
#
# WHY: sipreg_set_srcport() couples two things -- the port advertised in Contact,
# and the local source port that outgoing connections bind to (dialog ->
# sip_conncfg -> tcp_connect_bind). That is libre's intended "listen and
# originate on one port" model and it relies on SO_REUSEPORT, which does not
# exist before Linux 3.9; the RK3188 panels run 3.0.36 ("SO_REUSEPORT: Protocol
# not available", measured on hardware). Binding an outgoing socket to the port
# our own listener holds fails EADDRINUSE, so the request is never sent and the
# transaction times out -- seen as REGISTER -> 401 -> no authenticated retry.
#
# We still need Contact to name our LISTENING port: FreeSWITCH does not offer RFC
# 5626 outbound (its 401 advertises only "timer, path, replaces"), so it connects
# to the Contact rather than reusing the flow.
#
# sipreg_set_contact_port() sets only reg->srcport: Contact names the listening
# port, connections originate from an ephemeral one. Idempotent.
REG_C="$LIBRE/src/sipreg/reg.c"
REG_H="$LIBRE/include/re_sipreg.h"
if ! grep -q 'sipreg_set_contact_port' "$REG_C"; then
  echo "==> applying local patch: sipreg_set_contact_port()"
  cat >> "$REG_C" <<'PATCH'


/**
 * Set ONLY the port advertised in Contact/Via, without binding outgoing
 * connections to it. See tools/build-libre.sh for why this exists.
 *
 * @param reg   SIP registration client
 * @param port  Port to advertise in Contact/Via
 */
void sipreg_set_contact_port(struct sipreg *reg, uint16_t port)
{
	if (!reg)
		return;

	reg->srcport = port;
}
PATCH
  printf '\nvoid sipreg_set_contact_port(struct sipreg *reg, uint16_t port);\n' >> "$REG_H"
fi

[[ -d "$MBED/include" ]] || { echo "missing $MBED — libre needs its mbedtls crypto backend" >&2; exit 1; }

# 1) Source selection.
#
# Excluded, and why:
#   tls/            — needs OpenSSL, and Control4 SIP rides plain TCP (see the
#                     TCP-transport finding in PHASE3-INTERCOM-SIP.md). No TLS, no OpenSSL.
#   dns/res.c       — calls res_ninit(); musl has no resolv.h equivalent. libre
#                     falls back to reading /etc/resolv.conf, which is what we want.
#   thread/posix.c  — the pre-C11 fallback; musl has <threads.h>, so thread/thread.c
#                     handles it and posix.c would just fail to see pthread decls.
#   win32|apple|darwin|bsd — wrong platform.
#   rem/{vid,avc,aac} — video mixing / H.264 bitstream / AAC; the intercom is audio,
#                     and our H.264 path is a separate software codec.
#   rtmp|bfcp|pcp|av1|h264|h265|dd|srtp|ice|turn|trice|unixsock
#                   — streaming/conferencing/NAT-traversal subsystems a LAN SIP
#                     intercom never touches. (rtmp also wants VER_MAJOR/MINOR/PATCH,
#                     which only the cmake build defines.)
#
# NOTE stun/ and websock/ must stay even though we use neither directly: rtp pulls
# STUN symbols and sip registers a WebSocket transport, so dropping them breaks the link.
SRCS=$(cd "$LIBRE" && find src rem -name '*.c' \
  | grep -vE 'win32|/apple/|/darwin/|/bsd/|/tls/|/aes/openssl/|/hmac/openssl/|dns/res\.c|thread/posix\.c|rem/vid|rem/avc|rem/aac|/rtmp/|/bfcp/|/pcp/|/av1/|/h264/|/h265/|/dd/|/srtp/|/ice/|/turn/|/trice/|/unixsock/')

# 2) Feature defines. These stand in for the cmake feature probes (re-config.cmake),
# which we can't run against a cross target.
DEFS=(
  -DUSE_MBEDTLS
  -DHAVE_PTHREAD -DHAVE_EPOLL -DHAVE_SELECT -DHAVE_SELECT_H -DHAVE_ATOMIC
  -DHAVE_THREADS -DHAVE_UNISTD_H -DHAVE_STRINGS_H -DHAVE_SYS_TIME_H
  -DHAVE_SIGNAL -DHAVE_GETIFADDRS -DHAVE_INET6 -DHAVE_INET_NTOP
  -DRE_VERSION=\"$LIBRE_TAG\" -D_GNU_SOURCE
)

OBJ="$LIBRE/.obj"
rm -rf "$OBJ"; mkdir -p "$OBJ"

# zig's compile cache races when one invocation compiles many files in parallel
# (spurious "CacheCheckFailed"). A per-build private cache dir avoids it.
export ZIG_LOCAL_CACHE_DIR="$OBJ/.zig-cache"
export ZIG_GLOBAL_CACHE_DIR="$OBJ/.zig-global"

echo "==> building libre + librem ($(echo "$SRCS" | wc -l | tr -d ' ') files)"
for f in $SRCS; do
  o="$OBJ/$(echo "$f" | tr '/' '_' | sed 's/\.c$/.o/')"
  # shellcheck disable=SC2086
  $CC -static -Os -c "$LIBRE/$f" -o "$o" \
      -I"$LIBRE/include" -I"$LIBRE/src" -I"$MBED/include" \
      "${DEFS[@]}" -w
done

zig ar rcs "$LIBRE/libre.a" "$OBJ"/*.o
echo "built $LIBRE/libre.a ($(du -h "$LIBRE/libre.a" | cut -f1), $(ls "$OBJ"/*.o | wc -l | tr -d ' ') objects)"
