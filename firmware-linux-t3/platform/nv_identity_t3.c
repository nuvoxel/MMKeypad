/*
 * nv_identity for the repurposed Control4 T3 (RK3188 / Linux).
 *
 * Replaces the nuvoxel_device component's ESP (eFuse-HMAC) and generic-host
 * (random keyfile) identity backends with one rooted in this SoC's hardware:
 *
 *   hardware_id  = SHA-256(/sys/devices/system/cpu/efuse_val)[:12]  (24 hex)
 *                  The RK3188 exposes its per-chip eFuse contents here; the
 *                  bytes past the "RK\x31\x88" marker are a unique, read-only,
 *                  immutable chip UID. Hashing the whole node gives a stable,
 *                  fixed-width fingerprint that survives NAND reflashes and MAC
 *                  changes. Falls back to the eth0 MAC (a real Control4 OUI)
 *                  only if the eFuse can't be read.
 *
 *   device_secret = 32 random bytes, generated once and persisted to
 *                   /data/nvx/secret (0600). Unlike the ESP32, the RK3188 has
 *                   no application-readable *secret* hardware key (efuse_val is
 *                   world-readable), so the secret is provisioned locally. The
 *                   platform records it at registration; it is stable across
 *                   reboots and only re-mints if /data is wiped (full reflash),
 *                   which forces a re-register — acceptable and expected.
 *
 * Linked in place of nuvoxel_device's nv_identity.c (which is simply not
 * compiled for this target); every other nv_* file takes an nv_identity_t by
 * pointer, so nothing else needs to change.
 */
#include "nuvoxel_device.h"

#include "mbedtls/sha256.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define EFUSE_PATH  "/sys/devices/system/cpu/efuse_val"
#define MAC_PATH    "/sys/class/net/eth0/address"
#define SECRET_DIR  "/data/nvx"
#define SECRET_PATH SECRET_DIR "/secret"

static const char HEX[] = "0123456789abcdef";

static void to_hex(const unsigned char *in, int n, char *out) {
  for (int i = 0; i < n; i++) {
    out[i * 2] = HEX[in[i] >> 4];
    out[i * 2 + 1] = HEX[in[i] & 0xf];
  }
  out[n * 2] = '\0';
}

/* Read up to cap-1 bytes of a small sysfs/file into buf (NUL-terminated).
 * Returns the byte count, or -1 on error. */
static int read_small(const char *path, char *buf, size_t cap) {
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  size_t n = fread(buf, 1, cap - 1, f);
  fclose(f);
  buf[n] = '\0';
  return (int)n;
}

/* hardware_id from the RK3188 eFuse; MAC fallback. Returns 0 on success. */
static int derive_hardware_id(char *out /* NV_HWID_MAX */) {
  char raw[128];
  int n = read_small(EFUSE_PATH, raw, sizeof(raw));
  if (n >= 16) {
    /* Hash the whole eFuse node → a stable, opaque 24-hex fingerprint. */
    unsigned char dg[32];
    mbedtls_sha256((const unsigned char *)raw, (size_t)n, dg, 0);
    to_hex(dg, 12, out); /* 24 hex chars, well within NV_HWID_MAX (40) */
    return 0;
  }
  /* eFuse unreadable — fall back to the NIC MAC (Control4 OUI, still stable). */
  char mac[32];
  if (read_small(MAC_PATH, mac, sizeof(mac)) > 0) {
    int j = 0;
    for (int i = 0; mac[i] && j < NV_HWID_MAX - 1; i++) {
      char c = mac[i];
      if (c == ':' || c == '\n' || c == '\r') continue;
      out[j++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    out[j] = '\0';
    return j > 0 ? 0 : -1;
  }
  return -1;
}

/* device_secret: load the persisted one, else generate + persist. 0 on success. */
static int load_or_make_secret(char *out /* NV_SECRET_MAX */) {
  char hex[NV_SECRET_MAX];
  int n = read_small(SECRET_PATH, hex, sizeof(hex));
  if (n >= 64) { /* 32 bytes hex */
    memcpy(out, hex, 64);
    out[64] = '\0';
    return 0;
  }
  /* Generate 32 fresh random bytes and persist them (0600). */
  unsigned char rnd[32];
  FILE *ur = fopen("/dev/urandom", "rb");
  if (!ur) return -1;
  size_t got = fread(rnd, 1, sizeof(rnd), ur);
  fclose(ur);
  if (got != sizeof(rnd)) return -1;
  to_hex(rnd, 32, out);

  mkdir(SECRET_DIR, 0700);
  FILE *f = fopen(SECRET_PATH, "wb");
  if (f) {
    fchmod(fileno(f), 0600);
    fwrite(out, 1, 64, f);
    fclose(f);
  }
  return 0;
}

nv_err_t nv_identity_init(nv_identity_t *out) {
  if (!out) return NV_ERR_ARG;
  memset(out, 0, sizeof(*out));

  if (derive_hardware_id(out->hardware_id) != 0) return NV_ERR_IDENTITY;
  if (load_or_make_secret(out->device_secret) != 0) return NV_ERR_IDENTITY;
  out->provisioned = true;
  return NV_OK;
}
