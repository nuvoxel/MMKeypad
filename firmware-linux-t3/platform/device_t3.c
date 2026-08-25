/*
 * device.h implementation for the T3 (RK3188 / Linux).
 *
 * The native counterpart of firmware-idf/main/device.c: same public contract
 * (identity + the `hello` handshake), built on POSIX instead of ESP-IDF.
 * Identity is nv_identity_t3.c (eFuse-rooted). The open build talks to no
 * online service, so there is no check-in, licensing, or registration here.
 */
#define _GNU_SOURCE   // strcasestr
#include "device.h"
#include "nuvoxel_device.h"
#include "config.h" // fw_version(), g_settings
#include "board.h"  // MMK_BOARD_NAME
#include "net.h"    // net_current_room / net_get_ip / net_connected / net_peer_ip

#include <dirent.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/fb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <unistd.h>

static nv_identity_t s_id;

// ── model identity (runtime-detected) ───────────────────────────────────────
// The T3's model is only discoverable at RUNTIME, from the kernel version string:
// `uname -v` reads "#1-glassedge7.2.0 SMP ..." on the 7" and "#1-glassedge10.2.0 ..."
// on the 10", with a trailing "p" for the tabletop variants. Both panels run the
// SAME binary (one build, deployed to both), so the compile-time MMK_BOARD_NAME
// mislabels one of them -- every unit previously reported itself as a 7".
//
// Control4's own identifiers, from the launcher's per-model configs
// (phoenix-navigator.apk assets/glassedge*.conf) -- these are what the unit
// DECLARES to Control4 over SDDP, so they are the right thing for device identity:
//   glassedge7   C4-TS-INWALL7     T3 7" In-Wall Touch Screen
//   glassedge7p  C4-TS-PORTABLE7   T3 7" Tabletop Touch Screen
//   glassedge10  C4-TS-INWALL10    T3 10" In-Wall Touch Screen
//   glassedge10p C4-TS-PORTABLE10  T3 10" Tabletop Touch Screen
//
// NOTE these are NOT the orderable part numbers on the Control4 datasheet, which
// are C4-WALL7-BL / C4-WALL7-WH / C4-WALL10-BL / C4-WALL10-WH. Those encode the
// bezel COLOUR, which nothing on the device exposes, so we deliberately do not
// guess one; the model code + hardware fingerprint disambiguate it if
// ever needs the sales SKU.
static bool t3_variant(int *inches, bool *tabletop) {
  struct utsname u;
  const char *tag = (uname(&u) == 0) ? strstr(u.version, "glassedge") : NULL;
  if (!tag) return false;
  const char *p = tag + strlen("glassedge");
  int n = atoi(p);
  if (n <= 0) return false;
  while (*p >= '0' && *p <= '9') p++;
  *inches = n;
  *tabletop = (*p == 'p' || *p == 'P');
  return true;
}

// Short internal id: "t3-7" | "t3-7p" | "t3-10" | "t3-10p".
static const char *board_name(void) {
  static char cached[16];
  if (cached[0]) return cached;
  int in; bool tt;
  if (t3_variant(&in, &tt)) snprintf(cached, sizeof(cached), "t3-%d%s", in, tt ? "p" : "");
  else                      snprintf(cached, sizeof(cached), "%s", MMK_BOARD_NAME);
  return cached;
}

// Control4 model code as declared over SDDP (see table above).
static const char *model_code(void) {
  static char cached[24];
  if (cached[0]) return cached;
  int in; bool tt;
  if (t3_variant(&in, &tt))
    snprintf(cached, sizeof(cached), "C4-TS-%s%d", tt ? "PORTABLE" : "INWALL", in);
  else
    snprintf(cached, sizeof(cached), "%s", "C4-TS-INWALL7");
  return cached;
}

// Friendly name, matching Control4's ModelName exactly.
static const char *model_friendly(void) {
  static char cached[48];
  if (cached[0]) return cached;
  int in; bool tt;
  if (t3_variant(&in, &tt))
    snprintf(cached, sizeof(cached), "T3 %d\" %s Touch Screen", in, tt ? "Tabletop" : "In-Wall");
  else
    snprintf(cached, sizeof(cached), "%s", MMK_MODEL);
  return cached;
}

// Platform SKU -- the SIZE-SPECIFIC product id, matching the portal's registry
// (nuvoxel apps/web/src/lib/devices/skus.ts):
//   mmk-t3-7 / mmk-t3-10 / mmk-t3-portable-7 / mmk-t3-portable-10
// with "mmk-t3" as the generic fallback for a T3 that cannot identify its size.
//
// One binary still serves the whole family: the registry's SkuDef carries a
// separate `fw` field (all T3 SKUs point at the shared "mmk-t3" image), so
// reporting the specific SKU does NOT fork the OTA channel. Reporting the
// generic id instead -- which is what this used to do -- left the portal unable
// to tell a 7" from a 10", showing every unit as plain "Control4 T3".
static const char *device_sku(void) {
  static char cached[24];
  if (cached[0]) return cached;
  int in; bool tt;
  if (t3_variant(&in, &tt))
    snprintf(cached, sizeof(cached), tt ? "mmk-t3-portable-%d" : "mmk-t3-%d", in);
  else
    snprintf(cached, sizeof(cached), "mmk-t3");
  return cached;
}

const char *device_hardware_id(void) { return s_id.hardware_id; }
const char *device_sku_id(void) { return device_sku(); }

void device_init(void) {
  if (nv_identity_init(&s_id) == NV_OK) {
    fprintf(stderr, "device: identity %s (provisioned=%d)\n", s_id.hardware_id,
            s_id.provisioned);
  } else {
    fprintf(stderr, "device: identity init failed\n");
  }
  // Model is runtime-detected (see board_name()); log it so a misdetect is
  // obvious in the boot log rather than silently mislabelling the unit.
  fprintf(stderr, "device: board=%s model=%s (%s) sku=%s\n", board_name(), model_friendly(), model_code(), device_sku());
}

// Open build: no cloud check-in / enrollment / cloud OTA. device_start() has
// nothing to spawn -- the panel is fully functional at boot and connects to
// Control4 over the :6700 link + SDDP. Updates come from the on-screen updater
// (GitHub Releases) and local/USB flashing, never a server.
void device_start(void) {
}

// The on-screen updater applies an image directly via nv_ota_apply(); no cloud
// "check now" in the open build.
void device_ota_check_now(void) {
}

// ---- device manifest: model + hardware identifiers + connectivity ----------

static char s_driver_ver[32];
void device_set_driver_version(const char *ver) {
  if (ver) snprintf(s_driver_ver, sizeof(s_driver_ver), "%s", ver);
}
const char *device_driver_version(void) { return s_driver_ver; }

// Runtime-detected hardware facts (computed once, then cached).
static struct {
  bool done;
  int disp_w, disp_h;  // physical panel, from fbdev
  char mac[24];        // primary NIC MAC
  char link[12];       // "ethernet" | "wifi"
  char model[48];      // friendly, resolved from SoC + panel
} s_hw;

static void read_line(const char *path, char *out, size_t cap) {
  out[0] = '\0';
  FILE *f = fopen(path, "r");
  if (!f) return;
  if (fgets(out, cap, f)) {
    size_t n = strlen(out);
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = '\0';
  }
  fclose(f);
}

// Pick the interface carrying a routable IPv4 (prefer wired); returns its name.
static void active_iface(char *ifname, size_t cap) {
  snprintf(ifname, cap, "%s", MMK_NET_ETH ? "eth0" : "wlan0");
  struct ifaddrs *ifa, *p;
  if (getifaddrs(&ifa) != 0) return;
  char wifi[32] = "";
  for (p = ifa; p; p = p->ifa_next) {
    if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET || !p->ifa_name) continue;
    uint32_t a = ((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr;
    if (a == 0 || a == htonl(0x7f000001)) continue;
    if (strncmp(p->ifa_name, "eth", 3) == 0) { // wired wins
      snprintf(ifname, cap, "%s", p->ifa_name);
      freeifaddrs(ifa);
      return;
    }
    if (!wifi[0] && (strncmp(p->ifa_name, "wlan", 4) == 0 ||
                     strncmp(p->ifa_name, "wl", 2) == 0))
      snprintf(wifi, sizeof(wifi), "%s", p->ifa_name);
  }
  if (wifi[0]) snprintf(ifname, cap, "%s", wifi);
  freeifaddrs(ifa);
}

static void hw_detect(void) {
  if (s_hw.done) return;

  // Physical panel dimensions from the framebuffer.
  int fd = open("/dev/fb0", O_RDONLY);
  if (fd >= 0) {
    struct fb_var_screeninfo v;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) == 0) {
      s_hw.disp_w = (int)v.xres;
      s_hw.disp_h = (int)v.yres;
    }
    close(fd);
  }

  // Active interface → MAC + link type.
  char ifn[32];
  active_iface(ifn, sizeof(ifn));
  char path[64];
  snprintf(path, sizeof(path), "/sys/class/net/%s/address", ifn);
  read_line(path, s_hw.mac, sizeof(s_hw.mac));
  snprintf(s_hw.link, sizeof(s_hw.link), "%s",
           strncmp(ifn, "eth", 3) == 0 ? "ethernet" : "wifi");

  // Model. The variant IS knowable at runtime -- the kernel version string
  // carries it (see t3_variant / model_friendly above) -- so report Control4's
  // real ModelName. An older comment here claimed the 7" and 10" were
  // indistinguishable because they share an 800x1280 panel; that is true of the
  // PANEL but not of the kernel, which is built per model (glassedge7 vs
  // glassedge10). Fall back to a descriptive geometry string only if the kernel
  // tag is missing, so an unknown/new variant still self-identifies.
  int w = s_hw.disp_w, h = s_hw.disp_h;
  int lo = w < h ? w : h, hi = w < h ? h : w; // orientation-independent
  int in_; bool tt_;
  if (t3_variant(&in_, &tt_))
    snprintf(s_hw.model, sizeof(s_hw.model), "%s", model_friendly());
  else if (lo && hi)
    snprintf(s_hw.model, sizeof(s_hw.model), "Control4 T3 (RK3188 %dx%d)", hi, lo);
  else
    snprintf(s_hw.model, sizeof(s_hw.model), "%s", MMK_MODEL);

  s_hw.done = true;
}

const char *device_model_name(void) { hw_detect(); return s_hw.model; }
const char *device_mac(void) { hw_detect(); return s_hw.mac; }
const char *device_link_type(void) { hw_detect(); return s_hw.link; }
const char *device_power_source(void) { return MMK_POWER; }

/* ── Full hardware inventory for the `hello` manifest ────────────────────────
 * Beta units — portables especially, and later SKU revisions — may carry
 * different touch/WiFi/panel/camera silicon than the units we've profiled. So the
 * manifest reports EVERYTHING readable at runtime (raw), plus a stable
 * fingerprint, so variants/revisions can be told apart even when they
 * share a SKU, and we can refine the model mapping server-side instead of
 * hardcoding it. All best-effort — missing bits are simply omitted. */

// whole small file, trimmed of trailing whitespace
static void slurp(const char *path, char *out, size_t cap) {
  out[0] = 0;
  int fd = open(path, O_RDONLY);
  if (fd < 0) return;
  ssize_t n = read(fd, out, cap - 1);
  close(fd);
  if (n < 0) n = 0;
  out[n] = 0;
  while (n && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ' ||
               out[n - 1] == '\t'))
    out[--n] = 0;
}

// pull "key=..." (whitespace-terminated) out of a buffer like /proc/cmdline
static void kv_from(const char *buf, const char *key, char *out, size_t cap) {
  out[0] = 0;
  const char *p = strstr(buf, key);
  if (!p) return;
  p += strlen(key);
  size_t i = 0;
  while (*p && *p != ' ' && *p != '\n' && i < cap - 1) out[i++] = *p++;
  out[i] = 0;
}

// FNV-1a over the identifying fields → a stable per-hardware-config fingerprint
static void fp_add(unsigned *h, const char *s) {
  if (!s) return;
  for (; *s; s++) { *h ^= (unsigned char)*s; *h *= 16777619u; }
}

// add every i2c device (bus-addr : name) — the richest revision-catcher
static void scan_i2c(cJSON *arr, char *touch_at, char *touch_name, size_t tcap) {
  touch_at[0] = touch_name[0] = 0;
  DIR *d = opendir("/sys/bus/i2c/devices");
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.' || strncmp(e->d_name, "i2c-", 4) == 0) continue;
    char p[160], name[64] = "";
    snprintf(p, sizeof p, "/sys/bus/i2c/devices/%s/name", e->d_name);
    read_line(p, name, sizeof name);
    if (!name[0]) continue;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "at", e->d_name);
    cJSON_AddStringToObject(o, "name", name);
    cJSON_AddItemToArray(arr, o);
    // remember whichever looks like the touch controller
    if (strcasestr(name, "goodix") || strcasestr(name, "gsl") ||
        strcasestr(name, "ft5x0x") || strcasestr(name, "-ts")) {
      snprintf(touch_at, tcap, "%s", e->d_name);
      snprintf(touch_name, tcap, "%s", name);
    }
  }
  closedir(d);
}

static void dir_has(const char *path, bool *out) {
  DIR *d = opendir(path);
  *out = (d != NULL);
  if (d) closedir(d);
}

// True only if `path` is a directory containing at least one real entry.
// Needed because several /sys/class/* directories exist merely because a driver
// registered the CLASS, with no device bound -- see the IR case in device_hw_json().
static void dir_has_entry(const char *path, bool *out) {
  DIR *d = opendir(path);
  *out = false;
  if (!d) return;
  for (struct dirent *e; (e = readdir(d)); ) {
    if (e->d_name[0] == '.') continue;
    *out = true;
    break;
  }
  closedir(d);
}

void device_hw_json(cJSON *hw) {
  unsigned fp = 2166136261u; // FNV offset
  char buf[512], a[64], b[64];

  // ---- SoC / firmware ----
  cJSON *soc = cJSON_AddObjectToObject(hw, "soc");
  slurp("/proc/cpuinfo", buf, sizeof buf); // Hardware line
  { char hwl[64]; kv_from(buf, "Hardware\t: ", hwl, sizeof hwl);
    if (!hwl[0]) kv_from(buf, "Hardware", hwl, sizeof hwl);
    if (hwl[0]) cJSON_AddStringToObject(soc, "hw", hwl); fp_add(&fp, hwl); }
  cJSON_AddStringToObject(soc, "soc", MMK_SOC);
  slurp("/sys/devices/system/cpu/efuse_val", a, sizeof a);
  if (a[0]) { cJSON_AddStringToObject(soc, "efuse", a); fp_add(&fp, a); }
  { int ncpu = (int)sysconf(_SC_NPROCESSORS_CONF); if (ncpu > 0) cJSON_AddNumberToObject(soc, "cpus", ncpu); }

  cJSON *fwo = cJSON_AddObjectToObject(hw, "fw");
  slurp("/proc/version", buf, sizeof buf); if (buf[0]) cJSON_AddStringToObject(fwo, "kernel", buf);
  slurp("/proc/cmdline", buf, sizeof buf);
  kv_from(buf, "bootver=", a, sizeof a); if (a[0]) cJSON_AddStringToObject(fwo, "bootver", a);
  kv_from(buf, "firmware_ver=", b, sizeof b); if (b[0]) cJSON_AddStringToObject(fwo, "firmwareVer", b);

  // ---- panel ----
  cJSON *pan = cJSON_AddObjectToObject(hw, "panel");
  cJSON_AddNumberToObject(pan, "w", s_hw.disp_w);
  cJSON_AddNumberToObject(pan, "h", s_hw.disp_h);
  cJSON_AddStringToObject(pan, "orient", s_hw.disp_w >= s_hw.disp_h ? "landscape" : "portrait");
  slurp("/sys/class/graphics/fb0/bits_per_pixel", a, sizeof a); if (a[0]) cJSON_AddNumberToObject(pan, "bpp", atoi(a));
  slurp("/sys/class/graphics/fb0/stride", a, sizeof a); if (a[0]) cJSON_AddNumberToObject(pan, "stride", atoi(a));
  { char pf[32]; snprintf(pf, sizeof pf, "%dx%d", s_hw.disp_w, s_hw.disp_h); fp_add(&fp, pf); }

  // ---- touch (from the full i2c scan) + input devices ----
  cJSON *i2c = cJSON_AddArrayToObject(hw, "i2c");
  char tat[64], tname[64];
  scan_i2c(i2c, tat, tname, sizeof tat);
  if (tname[0]) { cJSON *t = cJSON_AddObjectToObject(hw, "touch");
    cJSON_AddStringToObject(t, "at", tat); cJSON_AddStringToObject(t, "name", tname); fp_add(&fp, tname); }
  cJSON *inp = cJSON_AddArrayToObject(hw, "inputs");
  { FILE *f = fopen("/proc/bus/input/devices", "r");
    if (f) { char l[256]; while (fgets(l, sizeof l, f)) {
      char *n = strstr(l, "Name=\"");
      if (n) { n += 6; char *q = strchr(n, '"'); if (q) { *q = 0; cJSON_AddItemToArray(inp, cJSON_CreateString(n)); } } }
      fclose(f); } }

  // ---- wifi ----
  slurp("/sys/class/rkwifi/chip", a, sizeof a);
  if (a[0]) { cJSON *w = cJSON_AddObjectToObject(hw, "wifi");
    cJSON_AddStringToObject(w, "chip", a); fp_add(&fp, a);
    char wm[24]; read_line("/sys/class/net/wlan0/address", wm, sizeof wm);
    if (wm[0]) cJSON_AddStringToObject(w, "mac", wm); }

  // ---- ethernet (USB NIC id + driver + mac) ----
  { char vid[16] = "", pid[16] = "", drv[64] = "", link[256] = "", em[24] = "";
    // eth0/device is the USB *interface* (e.g. 2-1:1.0); the VID:PID lives on its
    // parent USB *device* (2-1), one level up.
    slurp("/sys/class/net/eth0/device/../idVendor", vid, sizeof vid);
    slurp("/sys/class/net/eth0/device/../idProduct", pid, sizeof pid);
    ssize_t ln = readlink("/sys/class/net/eth0/device/driver", link, sizeof link - 1);
    if (ln > 0) { link[ln] = 0; char *s = strrchr(link, '/'); snprintf(drv, sizeof drv, "%s", s ? s + 1 : link); }
    read_line("/sys/class/net/eth0/address", em, sizeof em);
    if (vid[0] || drv[0]) { cJSON *e = cJSON_AddObjectToObject(hw, "eth");
      if (vid[0]) { char usb[24]; snprintf(usb, sizeof usb, "%s:%s", vid, pid); cJSON_AddStringToObject(e, "usb", usb); fp_add(&fp, usb); }
      if (drv[0]) cJSON_AddStringToObject(e, "driver", drv);
      if (em[0]) cJSON_AddStringToObject(e, "mac", em); } }

  // ---- camera / audio / bt / ir ----
  slurp("/sys/class/video4linux/video0/name", a, sizeof a);
  if (a[0]) { cJSON_AddStringToObject(hw, "camera", a); fp_add(&fp, a); }
  { char card[64] = ""; FILE *f = fopen("/proc/asound/cards", "r");
    if (f) { char l[128]; if (fgets(l, sizeof l, f)) { char *br = strchr(l, ']'); if (br) { char *nm = br + 1; while (*nm == ' ') nm++; char *nl = strchr(nm, '\n'); if (nl) *nl = 0; snprintf(card, sizeof card, "%s", nm); } } fclose(f); }
    if (card[0]) { cJSON_AddStringToObject(hw, "audio", card); fp_add(&fp, card); } }
  // IR: require an actual DEVICE, not just the class directory. /sys/class/lirc
  // exists on every T3 because the lirc_dev module registers the class at boot,
  // but it is EMPTY -- measured on both the 7" and the 10", under our Linux and
  // under stock: no /sys/class/rc/rc0, no /sys/class/lirc/lirc*, no /dev/lirc*,
  // and no IR entry in /proc/bus/input/devices (only the keypad and touch).
  // The old `dir_has("/sys/class/lirc")` fallback therefore reported ir:true on
  // every unit. The dmesg lines about NEC/RC5/RC6/JVC/Sony are the kernel's
  // generic protocol decoders registering, not evidence of a wired receiver.
  { bool bt, ir; dir_has("/sys/class/bluetooth", &bt);
    dir_has("/sys/class/rc/rc0", &ir); if (!ir) dir_has_entry("/sys/class/lirc", &ir);
    cJSON_AddBoolToObject(hw, "bt", bt);
    cJSON_AddBoolToObject(hw, "ir", ir); }

  // ---- storage (NAND flash id) ----
  slurp("/sys/class/mtd/mtd0/name", a, sizeof a);
  slurp("/proc/mtd", buf, sizeof buf); // presence only; ids not always exposed

  // ---- stable fingerprint of the identifying config ----
  { char fph[12]; snprintf(fph, sizeof fph, "%08x", fp);
    cJSON_AddStringToObject(hw, "fp", fph); }
}

void device_manifest_to_json(cJSON *m) {
  hw_detect();
  cJSON_AddStringToObject(m, "sku", device_sku());
  cJSON_AddStringToObject(m, "model", s_hw.model);
  cJSON_AddStringToObject(m, "modelCode", model_code());  // C4 SDDP model id
  cJSON_AddStringToObject(m, "board", board_name());
  cJSON_AddStringToObject(m, "soc", MMK_SOC);
  cJSON_AddStringToObject(m, "fw", fw_version());
  cJSON_AddStringToObject(m, "hwid", s_id.hardware_id);
  cJSON_AddStringToObject(m, "mac", s_hw.mac);
  if (s_driver_ver[0]) cJSON_AddStringToObject(m, "driverVersion", s_driver_ver);

  cJSON *disp = cJSON_AddObjectToObject(m, "display");
  cJSON_AddNumberToObject(disp, "w", s_hw.disp_w);
  cJSON_AddNumberToObject(disp, "h", s_hw.disp_h);

  // Full runtime hardware inventory + fingerprint (beta: catches portable
  // variants and silent SKU revisions with different touch/WiFi/panel silicon).
  device_hw_json(cJSON_AddObjectToObject(m, "hw"));

  cJSON *net = cJSON_AddObjectToObject(m, "net");
  cJSON_AddStringToObject(net, "link", s_hw.link);

  cJSON *pwr = cJSON_AddObjectToObject(m, "power");
  cJSON_AddStringToObject(pwr, "source", MMK_POWER);
  // Candidate power sources for THIS hardware (the operator picks the real one;
  // PoE vs wall are indistinguishable to the SoC). The T3 is mains-only — its
  // "battery" power_supply node is the RTC backup, not a main battery — so it
  // can only be wall or PoE, never battery.
  cJSON *opts = cJSON_AddArrayToObject(pwr, "options");
  cJSON_AddItemToArray(opts, cJSON_CreateString("wall"));
  cJSON_AddItemToArray(opts, cJSON_CreateString("poe"));

  cJSON *caps = cJSON_AddObjectToObject(m, "caps");
  cJSON_AddBoolToObject(caps, "display", MMK_HAS_DISPLAY);
  cJSON_AddBoolToObject(caps, "touch", MMK_HAS_TOUCH);
  cJSON_AddBoolToObject(caps, "audio", MMK_HAS_AUDIO);
  cJSON_AddBoolToObject(caps, "intercom", MMK_HAS_SIP);
}


void device_status_to_json(cJSON *d) {
  cJSON_AddStringToObject(d, "deviceId", s_id.hardware_id);
  cJSON_AddStringToObject(d, "sku", device_sku());
}
