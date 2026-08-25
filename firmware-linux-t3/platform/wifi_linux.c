#define _GNU_SOURCE
#include "wifi_linux.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// OOB driver on the stock /system; config persisted on /data.
#define WMOD "/system/lib/modules/rkwifi.oob.ko"
#define WCONF "/data/wpa_supplicant.conf"

// The wpa tools are baked into the rootfs at /usr/sbin on a shipped image, but
// early test units received them in /data/bin. Resolve once, preferring the
// rootfs copy, so both layouts work without a reflash.
static const char *tool_dir(void) {
    static const char *d;
    if (!d) d = (access("/usr/sbin/wpa_supplicant", X_OK) == 0) ? "/usr/sbin" : "/data/bin";
    return d;
}

// Build a `wpa_cli … <sub>` command line into buf.
static void wcli(char *buf, int n, const char *sub) {
    snprintf(buf, n, "%s/wpa_cli -i wlan0 -p /var/run/wpa_supplicant %s", tool_dir(), sub);
}

// Run a shell command, capture up to cap-1 bytes of stdout. Returns bytes read.
static int run_capture(const char *cmd, char *out, int cap) {
  if (out && cap) out[0] = 0;
  FILE *f = popen(cmd, "r");
  if (!f) return -1;
  int n = 0;
  if (out && cap > 1) {
    n = (int)fread(out, 1, cap - 1, f);
    if (n < 0) n = 0;
    out[n] = 0;
  }
  pclose(f);
  return n;
}

static bool proc_running(const char *name) {
  char c[128];
  snprintf(c, sizeof(c), "pgrep -f %s >/dev/null 2>&1", name);
  return system(c) == 0;
}

bool wifi_ensure_up(void) {
  // Driver (idempotent): init also loads it, but a from-boot-without-init or a
  // manual run may need it. `wlan` is the module name (see init.c / build-wpa.sh).
  if (system("lsmod | grep -q '^wlan ' 2>/dev/null") != 0)
    system("insmod " WMOD " 2>/dev/null");
  system("mkdir -p /var/run/wpa_supplicant /data/bin 2>/dev/null");
  if (!proc_running("wpa_supplicant")) {
    // Seed a config if none yet (ctrl_interface is what wpa_cli talks to).
    if (access(WCONF, F_OK) != 0) {
      FILE *f = fopen(WCONF, "w");
      if (f) {
        fputs("ctrl_interface=/var/run/wpa_supplicant\nupdate_config=1\n", f);
        fclose(f);
      }
    }
    char sup[192];
    snprintf(sup, sizeof(sup),
             "%s/wpa_supplicant -B -i wlan0 -c " WCONF " -D nl80211 >/dev/null 2>&1",
             tool_dir());
    system(sup);
    usleep(700 * 1000);
  }
  return proc_running("wpa_supplicant");
}

static int by_signal(const void *a, const void *b) {
  return ((const wifi_net_t *)b)->signal - ((const wifi_net_t *)a)->signal;
}

int wifi_scan(wifi_net_t *out, int max) {
  if (!wifi_ensure_up() || !out || max <= 0) return -1;
  char cmd[192];
  wcli(cmd, sizeof(cmd), "scan >/dev/null 2>&1");
  system(cmd);
  sleep(4);
  // scan_results: bssid \t freq \t signal \t flags \t ssid  (header on line 1)
  char *buf = malloc(64 * 1024);
  if (!buf) return -1;
  wcli(cmd, sizeof(cmd), "scan_results 2>/dev/null");
  int got = run_capture(cmd, buf, 64 * 1024);
  int n = 0;
  if (got > 0) {
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
      if (strncmp(line, "bssid", 5) == 0) continue; // header
      char *f = NULL;
      char *bssid = strtok_r(line, "\t", &f);
      char *freq  = strtok_r(NULL, "\t", &f);
      char *sig   = strtok_r(NULL, "\t", &f);
      char *flags = strtok_r(NULL, "\t", &f);
      char *ssid  = strtok_r(NULL, "\t", &f);
      (void)bssid; (void)freq;
      if (!ssid || !ssid[0]) continue; // hidden SSID
      bool secured = flags && (strstr(flags, "WPA") || strstr(flags, "WEP"));
      int signal = sig ? atoi(sig) : -100;
      // Dedup by SSID (keep the strongest).
      int j = -1;
      for (int k = 0; k < n; k++)
        if (strcmp(out[k].ssid, ssid) == 0) { j = k; break; }
      if (j >= 0) {
        if (signal > out[j].signal) { out[j].signal = signal; out[j].secured = secured; }
        continue;
      }
      if (n >= max) continue;
      snprintf(out[n].ssid, sizeof(out[n].ssid), "%s", ssid);
      out[n].signal = signal;
      out[n].secured = secured;
      n++;
    }
  }
  free(buf);
  qsort(out, n, sizeof(*out), by_signal);
  return n;
}

// Escape a value for a wpa_supplicant.conf quoted string (backslash + quote) and
// DROP control characters. wpa_supplicant parses its config line by line, so a
// newline inside a value ends the quoted string early and everything after it is
// read as further directives -- an SSID is arbitrary bytes chosen by whoever runs
// the AP, so a hostile network name could inject config (extra network blocks, a
// different ctrl_interface). Escaping quotes alone was not enough.
static void conf_quote(const char *in, char *out, int cap) {
  int o = 0;
  out[o++] = '"';
  for (const char *p = in; *p && o < cap - 3; p++) {
    unsigned char ch = (unsigned char)*p;
    if (ch < 0x20 || ch == 0x7F) continue;          // no CR/LF/NUL-ish control bytes
    if (ch == '\\' || ch == '"') out[o++] = '\\';
    out[o++] = (char)ch;
  }
  out[o++] = '"';
  out[o] = 0;
}

bool wifi_connect(const char *ssid, const char *psk) {
  if (!ssid || !ssid[0]) return false;
  if (!wifi_ensure_up()) return false;

  char qssid[160], qpsk[160];
  conf_quote(ssid, qssid, sizeof(qssid));
  FILE *f = fopen(WCONF, "w");
  if (!f) return false;
  fputs("ctrl_interface=/var/run/wpa_supplicant\nupdate_config=1\n\nnetwork={\n", f);
  fprintf(f, "\tssid=%s\n", qssid);
  if (psk && psk[0]) {
    conf_quote(psk, qpsk, sizeof(qpsk));
    fprintf(f, "\tpsk=%s\n", qpsk);
  } else {
    fputs("\tkey_mgmt=NONE\n", f);
  }
  fputs("}\n", f);
  fclose(f);

  char cmd[192];
  wcli(cmd, sizeof(cmd), "reconfigure >/dev/null 2>&1");
  system(cmd);

  return wifi_await_dhcp(15);
}

bool wifi_await_dhcp(int timeout_s) {
  // Poll for association (wpa_state=COMPLETED).
  char cmd[192];
  wcli(cmd, sizeof(cmd), "status 2>/dev/null");
  bool assoc = false;
  for (int i = 0; i < timeout_s * 2; i++) {
    char st[512];
    run_capture(cmd, st, sizeof(st));
    if (strstr(st, "wpa_state=COMPLETED")) { assoc = true; break; }
    usleep(500 * 1000);
  }
  if (!assoc) return false;

  // DHCP (udhcpc is a busybox applet on the device). -n: exit if no lease;
  // -q: quit after obtaining one.
  system("udhcpc -i wlan0 -n -q -t 6 >/dev/null 2>&1");
  return wifi_have_ip(NULL, 0);
}

bool wifi_have_ip(char *ip, int n) {
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) return false;
  struct ifreq ifr = {0};
  snprintf(ifr.ifr_name, IFNAMSIZ, "wlan0");
  bool have = false;
  if (ioctl(s, SIOCGIFADDR, &ifr) == 0) {
    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
    if (sin->sin_addr.s_addr != 0) {
      have = true;
      if (ip && n > 0) snprintf(ip, n, "%s", inet_ntoa(sin->sin_addr));
    }
  }
  close(s);
  return have;
}

const char *wifi_current_ssid(void) {
  static char ssid[80];
  ssid[0] = 0;
  char st[512], cmd[192];
  wcli(cmd, sizeof(cmd), "status 2>/dev/null");
  if (run_capture(cmd, st, sizeof(st)) > 0) {
    // Find a line "ssid=..." that is NOT "bssid=".
    char *p = st;
    while ((p = strstr(p, "ssid=")) != NULL) {
      if (p == st || *(p - 1) == '\n') {
        p += 5;
        char *e = strchr(p, '\n');
        int len = e ? (int)(e - p) : (int)strlen(p);
        if (len >= (int)sizeof(ssid)) len = sizeof(ssid) - 1;
        memcpy(ssid, p, len);
        ssid[len] = 0;
        break;
      }
      p += 5;
    }
  }
  return ssid;
}
