// MMKeypad — backbox-poe CARRIER diagnostic console.
//
// The ws43 mated to a backbox-poe carrier is not one of board.sh's five targets:
// `ws43` is the bare module (no Ethernet; its halo.c drives a SINGLE onboard
// WS2812) and `poe` is a different headless Waveshare board. This image proves
// the carrier hardware before that combined target is worth building.
//
// Boots, runs every check once, then drops to a REPL on the console UART.
// Type `help`.
//
// Pin map, through the ws43 40-pin back header:
//   J1.39 -> GPIO48  HALO_DIN  (via a 74AHCT level shifter + 330R series)
//   J1.36 -> GPIO51  PHY_nRST  (R5 10k pullup on carrier)
//   J1.4/6 -> GPIO7/8   SDA/SCL   (NO carrier pullups — REVC.md #3)
//   J1.35/37 -> GPIO46/47 QW_SDA/SCL (Qwiic, pull-ups fitted)
//   RMII MDC=31 MDIO=52 CLK_EXT_IN=50 TX_EN=49 TXD0=34 TXD1=35 CRS_DV=28
//        RXD0=29 RXD1=30 — ETH_ESP32_EMAC_DEFAULT_CONFIG() encodes these.
//
// NOT OBSERVABLE: the carrier brings no PoE-presence or current-sense signal to
// the P4, so `power` reports a COMPUTED estimate and `poe` says so plainly.
// Never present either as a measurement.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_phy.h"
#include "esp_console.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/temperature_sensor.h"
#include "led_strip.h"
#include "nvs_flash.h"
#include "esp_ldo_regulator.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_cpu.h"
#include "freertos/task.h"

static const char *TAG = "carrier";

static void eth_init(void);
static inline void eth_init_fwd(void) { eth_init(); }

#define PIN_HALO_DIN 48
#define HALO_COUNT   24
#define PIN_PHY_RST  51
#define PIN_SDA       7
#define PIN_SCL       8
#define PIN_QW_SDA   46
#define PIN_QW_SCL   47

// 24 SK6812 at FULL white is ~1.4 A / 7 W — about double the halo's budget and
// enough to cook M1 (2026-09-03 bring-up). Default to 20% and clamp all input.
#define HALO_DEFAULT_BRIGHT 20

static led_strip_handle_t   s_strip;
static uint8_t              s_px[HALO_COUNT][3];
static int                  s_bright = HALO_DEFAULT_BRIGHT;
static esp_eth_handle_t     s_eth;
static esp_netif_t         *s_netif;
static volatile bool        s_link;
static temperature_sensor_handle_t s_tsens;

// ─── halo ────────────────────────────────────────────────────────────────────
static void halo_push(void)
{
    if (!s_strip) { printf("  !! halo strip handle is NULL — init failed at boot\n"); return; }
    for (int i = 0; i < HALO_COUNT; i++) {
        esp_err_t e = led_strip_set_pixel(s_strip, i,
                            s_px[i][0] * s_bright / 100,
                            s_px[i][1] * s_bright / 100,
                            s_px[i][2] * s_bright / 100);
        if (e != ESP_OK) { printf("  !! set_pixel(%d) failed: %s\n", i, esp_err_to_name(e)); return; }
    }
    esp_err_t e = led_strip_refresh(s_strip);
    if (e != ESP_OK) printf("  !! led_strip_refresh FAILED: %s\n", esp_err_to_name(e));
}
static void halo_all(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < HALO_COUNT; i++) { s_px[i][0]=r; s_px[i][1]=g; s_px[i][2]=b; }
    halo_push();
}

// Estimate only — there is no sense resistor on this board. ~20 mA per channel
// at full scale, plus ~1 mA per pixel quiescent for the SK6812 controller.
static float halo_amps(void)
{
    float mA = 0;
    for (int i = 0; i < HALO_COUNT; i++) {
        for (int c = 0; c < 3; c++) mA += (s_px[i][c] * s_bright / 100) / 255.0f * 20.0f;
        mA += 1.0f;
    }
    return mA / 1000.0f;
}

static int cmd_halo(int argc, char **argv)
{
    if (argc < 2) { printf("halo test|set <r> <g> <b>|px <i> <r> <g> <b>|bright <0-100>|off\n"); return 0; }

    if (!strcmp(argv[1], "off"))  { halo_all(0,0,0); printf("halo off\n"); return 0; }

    if (!strcmp(argv[1], "bright")) {
        if (argc < 3) { printf("brightness = %d%%\n", s_bright); return 0; }
        int v = atoi(argv[2]);
        if (v < 0)   v = 0;
        if (v > 100) v = 100;
        s_bright = v; halo_push();
        printf("brightness = %d%%  (est %.2f A on +5V)\n", s_bright, halo_amps());
        if (v > 60) printf("  WARNING: >60%% on all 24 px approaches the halo power budget\n");
        return 0;
    }
    if (!strcmp(argv[1], "set")) {
        if (argc < 5) { printf("halo set <r> <g> <b>   (0-255)\n"); return 0; }
        halo_all(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
        printf("all px = %s,%s,%s @ %d%%  (est %.2f A)\n", argv[2],argv[3],argv[4], s_bright, halo_amps());
        return 0;
    }
    if (!strcmp(argv[1], "px")) {
        if (argc < 6) { printf("halo px <index 0-%d> <r> <g> <b>\n", HALO_COUNT-1); return 0; }
        int i = atoi(argv[2]);
        if (i < 0 || i >= HALO_COUNT) { printf("index out of range\n"); return 1; }
        s_px[i][0]=atoi(argv[3]); s_px[i][1]=atoi(argv[4]); s_px[i][2]=atoi(argv[5]);
        halo_push(); printf("px %d set (est %.2f A)\n", i, halo_amps());
        return 0;
    }
    if (!strcmp(argv[1], "test")) {
        printf("chase: expect %d distinct positions, in order, no gaps/dead spots\n", HALO_COUNT);
        for (int i = 0; i < HALO_COUNT; i++) {
            halo_all(0,0,0);
            s_px[i][0]=s_px[i][1]=s_px[i][2]=255; halo_push();
            printf("  px %2d\n", i); fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(150));
        }
        const struct { const char *n; uint8_t r,g,b; } st[] = {
            {"RED",255,0,0},{"GREEN",0,255,0},{"BLUE",0,0,255},{"WHITE",255,255,255} };
        for (int k=0;k<4;k++){
            halo_all(st[k].r,st[k].g,st[k].b);            // set first, THEN report
            printf("  all %-5s (est %.2f A on +5V)\n", st[k].n, halo_amps());
            vTaskDelay(pdMS_TO_TICKS(900));
        }
        halo_all(0,0,0); printf("halo test done\n");
        return 0;
    }
    printf("unknown: %s\n", argv[1]); return 1;
}

// ─── i2c ─────────────────────────────────────────────────────────────────────
// bus 0 = main SDA/SCL (GPIO7/8), no carrier pullups so use the internal ones.
// bus 1 = Qwiic (GPIO46/47), pull-ups fitted, so no internal ones.
static esp_err_t bus_open(int bus, i2c_master_bus_handle_t *out)
{
    i2c_master_bus_config_t bc = {
        .i2c_port = -1,
        .sda_io_num = bus ? PIN_QW_SDA : PIN_SDA,
        .scl_io_num = bus ? PIN_QW_SCL : PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = bus ? false : true,
    };
    return i2c_new_master_bus(&bc, out);
}
static void scan_one(int bus)
{
    const char *nm = bus ? "qwiic(46/47)" : "main(7/8)";
    i2c_master_bus_handle_t h = NULL;
    esp_err_t e = bus_open(bus, &h);
    if (e != ESP_OK) { printf("  %s: init failed: %s\n", nm, esp_err_to_name(e)); return; }
    int n = 0;
    for (uint8_t a = 0x08; a < 0x78; a++)
        if (i2c_master_probe(h, a, 50) == ESP_OK) { printf("  %s: 0x%02x\n", nm, a); n++; }
    if (!n) printf("  %s: no devices\n", nm);
    i2c_del_master_bus(h);
}
static int cmd_i2c(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "dump")) {
        if (argc < 4) { printf("i2c dump <bus 0|1> <addr hex>\n"); return 0; }
        int bus = atoi(argv[2]); uint8_t addr = strtol(argv[3], NULL, 16);
        i2c_master_bus_handle_t h = NULL;
        if (bus_open(bus, &h) != ESP_OK) { printf("bus init failed\n"); return 1; }
        i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                                   .device_address = addr, .scl_speed_hz = 100000 };
        i2c_master_dev_handle_t d = NULL;
        if (i2c_master_bus_add_device(h, &dc, &d) != ESP_OK) { printf("add dev failed\n"); i2c_del_master_bus(h); return 1; }
        printf("dump 0x%02x on bus %d:\n     ", addr, bus);
        for (int c=0;c<16;c++) printf(" %02x", c);
        for (int r = 0; r < 256; r++) {
            uint8_t reg = r, val = 0;
            if ((r % 16) == 0) printf("\n %02x: ", r);
            if (i2c_master_transmit_receive(d, &reg, 1, &val, 1, 100) == ESP_OK) printf(" %02x", val);
            else printf(" --");
        }
        printf("\n");
        i2c_master_bus_rm_device(d); i2c_del_master_bus(h);
        return 0;
    }
    printf("i2c scan:\n"); scan_one(0); scan_one(1);
    return 0;
}

// ─── phy / ethernet ──────────────────────────────────────────────────────────
static uint32_t phy_rd(uint32_t reg)
{
    uint32_t v = 0;
    esp_eth_phy_reg_rw_data_t rw = { .reg_addr = reg, .reg_value_p = &v };
    if (!s_eth) return 0xFFFF;
    esp_eth_ioctl(s_eth, ETH_CMD_READ_PHY_REG, &rw);
    return v;
}
static int cmd_phy(int argc, char **argv)
{
    if (!s_eth) { printf("PHY not initialised (driver install failed at boot)\n"); return 1; }
    if (argc >= 2) { uint32_t r = strtol(argv[1], NULL, 0); printf("reg %"PRIu32" = 0x%04"PRIx32"\n", r, phy_rd(r)); return 0; }
    printf("PHY register dump (IP101GA):\n");
    for (int r = 0; r < 32; r++) {
        if (r % 8 == 0) printf("\n %2d: ", r);
        printf(" %04"PRIx32, phy_rd(r));
    }
    printf("\n\n");
    uint32_t bmcr = phy_rd(0), bmsr = phy_rd(1); bmsr = phy_rd(1);
    printf(" BMCR 0x%04"PRIx32": %s %s autoneg=%s loopback=%s\n", bmcr,
           (bmcr & (1<<13)) ? "100M":"10M", (bmcr & (1<<8)) ? "FD":"HD",
           (bmcr & (1<<12)) ? "on":"off", (bmcr & (1<<14)) ? "ON":"off");
    printf(" BMSR 0x%04"PRIx32": link=%s autoneg-done=%s remote-fault=%s\n", bmsr,
           (bmsr & (1<<2)) ? "UP":"DOWN", (bmsr & (1<<5)) ? "yes":"no",
           (bmsr & (1<<4)) ? "YES":"no");
    printf(" PHYID 0x%04"PRIx32" 0x%04"PRIx32"  (IP101 = 0x0243 0x0C54)\n", phy_rd(2), phy_rd(3));
    return 0;
}
static int cmd_eth(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!s_eth) { printf("ETH not initialised\n"); return 1; }
    int32_t addr = -1; esp_eth_ioctl(s_eth, ETH_CMD_G_PHY_ADDR, &addr);
    eth_speed_t sp = 0; eth_duplex_t dx = 0;
    esp_eth_ioctl(s_eth, ETH_CMD_G_SPEED, &sp);
    esp_eth_ioctl(s_eth, ETH_CMD_G_DUPLEX_MODE, &dx);
    uint8_t mac[6] = {0}; esp_eth_ioctl(s_eth, ETH_CMD_G_MAC_ADDR, mac);
    printf("PHY addr   : %"PRId32"\n", addr);
    printf("MAC        : %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    printf("link       : %s\n", s_link ? "UP" : "down");
    printf("speed      : %s\n", sp == ETH_SPEED_100M ? "100M" : "10M");
    printf("duplex     : %s\n", dx == ETH_DUPLEX_FULL ? "full" : "half");
    esp_netif_ip_info_t ip;
    if (s_netif && esp_netif_get_ip_info(s_netif, &ip) == ESP_OK)
        printf("ip         : " IPSTR "  gw " IPSTR "\n", IP2STR(&ip.ip), IP2STR(&ip.gw));
    return 0;
}

// ─── chip / power / poe ──────────────────────────────────────────────────────
static int cmd_chip(int argc, char **argv)
{
    (void)argc; (void)argv;
    esp_chip_info_t ci; esp_chip_info(&ci);
    uint8_t mac[6]={0}; esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);
    uint32_t fs = 0; esp_flash_get_size(NULL, &fs);
    printf("ESP32-P4 rev v%d.%d, %d core(s)\n", ci.revision/100, ci.revision%100, ci.cores);
    printf("MAC        : %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    printf("flash      : %"PRIu32" MB\n", fs/(1024*1024));
    printf("reset      : %d\n", (int)esp_reset_reason());
    printf("heap free  : %u (min %u)\n", (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());
    float t = 0;
    if (s_tsens && temperature_sensor_get_celsius(s_tsens, &t) == ESP_OK)
        printf("die temp   : %.1f C\n", t);
    return 0;
}
static int cmd_power(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("POWER — COMPUTED ESTIMATE, NOT A MEASUREMENT.\n");
    printf("  rev-C has no shunt, no INA and no divider to the P4. Nothing on\n");
    printf("  this board can measure current. Numbers below are from the halo\n");
    printf("  buffer at ~20 mA/channel full scale + ~1 mA/px quiescent.\n\n");
    printf("  halo      : %.2f A  (%.2f W @5V)  brightness %d%%\n", halo_amps(), halo_amps()*5.0f, s_bright);
    printf("  halo max  : %.2f A  if all 24 px were full white at 100%%\n", (24*(60.0f+1.0f))/1000.0f);
    printf("  PHY+LDO   : ~0.10 A (~0.5 W) typical, not measured\n");
    printf("  ws43      : not visible from the carrier\n");
    return 0;
}
static int cmd_poe(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("PoE — NOT OBSERVABLE ON REV-C.\n");
    printf("  Verified against the carrier netlist:\n");
    printf("    +5V_POE reaches only TP7, D1, C6, C7, M1 and R19->D24. Never J1.\n");
    printf("    VIN+/VIN- reach only J2, M1, D30, TP15. Isolated from board GND.\n");
    printf("  So the P4 cannot tell whether it is running on PoE or USB, and\n");
    printf("  cannot see the PD negotiate, class, or drop out.\n\n");
    printf("  NEXT REV: divide +5V_POE into a spare P4 GPIO for presence (GPIO3/4/\n");
    printf("  5/21/37/38 are free on J7), and/or fit an INA219 on the Qwiic bus\n");
    printf("  for real current — that needs no new P4 pins and the pullups exist.\n");
    return 0;
}
static int cmd_gpio(int argc, char **argv)
{
    (void)argc; (void)argv;
    // NB: GPIO37/38 are J7.5/J7.6 but are ALSO this build's console UART pins
    // ("GPIO 38 and 37 are used as console UART I/O pins" at boot) - reading
    // them reconfigures the console out from under us. Excluded deliberately.
    const int pins[] = {3,4,5,21}; // J7.8, J7.9, J7.7, J7.10
    printf("J7 breakout (floating unless something is attached):\n");
    printf("  GPIO37/38 (J7.5/6) skipped - console UART pins on this build\n");
    for (unsigned i = 0; i < sizeof(pins)/sizeof(pins[0]); i++) {
        gpio_config_t g = { .pin_bit_mask = 1ULL<<pins[i], .mode = GPIO_MODE_INPUT };
        gpio_config(&g);
        printf("  GPIO%-2d = %d\n", pins[i], gpio_get_level(pins[i]));
    }
    return 0;
}
// REF_CLK (GPIO50, J1.34) is GENERATED BY THE PHY and fed to the P4. Its
// presence splits the diagnosis in one shot:
//   oscillating -> U1 is powered, out of reset and its 25MHz crystal runs, so a
//                  failed MDIO detect means MDC/MDIO wiring (J1.24 / J1.38).
//   static      -> the PHY is not running at all: power, Y1, or nRST.
// Sampled far below 50MHz, so an active clock aliases into a ~50/50 mix of
// levels with many transitions; a dead line reads all-0 or all-1.
static int cmd_refclk(int argc, char **argv)
{
    (void)argc; (void)argv;
    const int PIN = 50;
    gpio_config_t g = { .pin_bit_mask = 1ULL<<PIN, .mode = GPIO_MODE_INPUT };
    gpio_config(&g);
    int ones=0, trans=0, last=-1;
    const int N = 20000;
    for (int i=0;i<N;i++) {
        int v = gpio_get_level(PIN);
        if (v) ones++;
        if (last >= 0 && v != last) trans++;
        last = v;
    }
    printf("REF_CLK GPIO%d (J1.34, PHY->P4 50MHz):\n", PIN);
    printf("  %d samples, high=%d (%d%%), transitions=%d\n", N, ones, ones*100/N, trans);
    if (trans > 100) {
        printf("  => CLOCK PRESENT. U1 is powered, out of reset, crystal running.\n");
        printf("     A failed MDIO detect therefore points at MDC (J1.24) or\n");
        printf("     MDIO (J1.38) not getting through J1.\n");
    } else {
        printf("  => NO CLOCK (line is static). The PHY is not running at all.\n");
        printf("     Suspect, in order: PHY_nRST (J1.36) stuck low, the 25MHz\n");
        printf("     crystal Y1, or U1 power/soldering. Check TP14 and TP2.\n");
    }
    return 0;
}

// Manually drive PHY reset so it can be metered at TP14 while held.
static int cmd_nrst(int argc, char **argv)
{
    gpio_config_t g = { .pin_bit_mask = 1ULL<<PIN_PHY_RST, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&g);
    if (argc >= 2) {
        int v = atoi(argv[1]);
        gpio_set_level(PIN_PHY_RST, v);
        printf("PHY_nRST (GPIO%d, J1.36) driven %s - meter TP14 now\n", PIN_PHY_RST, v ? "HIGH (released)" : "LOW (asserted)");
        return 0;
    }
    printf("nrst <0|1>   0=hold in reset, 1=release\n");
    return 0;
}

// Retry detection on demand, so MDC can be scoped at TP12 while it runs.
static int cmd_retry(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (s_eth) { printf("already installed\n"); return 0; }
    printf("re-attempting PHY detect (scope TP12/MDC now)...\n");
    eth_init_fwd();
    printf(s_eth ? "SUCCESS\n" : "still no PHY\n");
    return 0;
}

// Square-wave a pin so a DMM can trace it: a 1kHz 50%% duty square reads ~1.65V
// average on any meter, vs a hard 0V/3.3V on a dead or stuck node. Use it to
// walk MDC (GPIO31 -> J1.24 -> TP12 -> U1.25) and MDIO (GPIO52 -> J1.38 ->
// U1.26) and find exactly where the signal stops.
static int cmd_wig(int argc, char **argv)
{
    if (argc < 2) {
        printf("wig <gpio> [seconds]   square-wave a pin at ~1kHz for DMM tracing\n");
        printf("  MDC  = GPIO31 -> J1.24 -> TP12 -> U1.25\n");
        printf("  MDIO = GPIO52 -> J1.38 -> U1.26   (R43 1k pull-up to +3V3_PHY)\n");
        printf("  a live node meters ~1.65 V; 0 V or 3.3 V means the path is broken\n");
        return 0;
    }
    int pin = atoi(argv[1]);
    int secs = (argc >= 3) ? atoi(argv[2]) : 5;
    if (secs < 1) secs = 1;
    if (secs > 60) secs = 60;
    gpio_config_t g = { .pin_bit_mask = 1ULL<<pin, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&g);
    printf("wiggling GPIO%d at ~1kHz for %ds - meter now (expect ~1.65 V)\n", pin, secs);
    fflush(stdout);
    int64_t end = esp_timer_get_time() + (int64_t)secs * 1000000;
    while (esp_timer_get_time() < end) {
        gpio_set_level(pin, 1); esp_rom_delay_us(500);
        gpio_set_level(pin, 0); esp_rom_delay_us(500);
    }
    printf("done, GPIO%d left low\n", pin);
    return 0;
}

// Bit-banged MDIO (IEEE 802.3 clause 22) straight on GPIO31/GPIO52, bypassing
// the EMAC entirely. This separates two very different faults:
//   finds the PHY  -> wiring and silicon are fine; the ESP32 EMAC's MDIO is
//                     misconfigured (clock divider, pin matrix) and it's a
//                     firmware problem.
//   finds nothing  -> MDC/MDIO genuinely do not reach U1, or U1 is dead.
// ~100kHz, far below the IP101's 2.5MHz limit, so marginal wiring still works.
#define MDC_PIN  31
#define MDIO_PIN 52
static void mb_wr(int b)
{
    gpio_set_level(MDIO_PIN, b);
    gpio_set_level(MDC_PIN, 0); esp_rom_delay_us(5);
    gpio_set_level(MDC_PIN, 1); esp_rom_delay_us(5);
}
static int mb_rd(void)
{
    gpio_set_level(MDC_PIN, 0); esp_rom_delay_us(5);
    gpio_set_level(MDC_PIN, 1); esp_rom_delay_us(2);
    int v = gpio_get_level(MDIO_PIN);
    esp_rom_delay_us(3);
    return v;
}
static uint16_t mdio_read(int phy, int reg)
{
    gpio_set_direction(MDIO_PIN, GPIO_MODE_OUTPUT);
    for (int i = 0; i < 32; i++) mb_wr(1);          // preamble
    mb_wr(0); mb_wr(1);                              // ST = 01
    mb_wr(1); mb_wr(0);                              // OP = 10 (read)
    for (int i = 4; i >= 0; i--) mb_wr((phy >> i) & 1);
    for (int i = 4; i >= 0; i--) mb_wr((reg >> i) & 1);
    gpio_set_direction(MDIO_PIN, GPIO_MODE_INPUT);   // turnaround
    mb_rd();
    uint16_t v = 0;
    for (int i = 0; i < 16; i++) v = (v << 1) | mb_rd();
    return v;
}
static int cmd_mdio(int argc, char **argv)
{
    int reg = (argc >= 2) ? atoi(argv[1]) : 2;
    gpio_config_t gc = { .pin_bit_mask = 1ULL<<MDC_PIN, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&gc);
    gpio_config_t gd = { .pin_bit_mask = 1ULL<<MDIO_PIN, .mode = GPIO_MODE_INPUT_OUTPUT,
                         .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&gd);
    printf("bit-banged MDIO scan, reg %d, all 32 addresses:\n", reg);
    int hits = 0;
    for (int a = 0; a < 32; a++) {
        uint16_t v = mdio_read(a, reg);
        if (v != 0xFFFF && v != 0x0000) { printf("  addr %2d -> 0x%04x  <== RESPONSE\n", a, v); hits++; }
    }
    if (!hits) {
        printf("  nothing answered (all reads 0x0000/0xFFFF)\n");
        printf("  => MDC/MDIO are not reaching U1, or U1 is not responding.\n");
        printf("     MDC is confirmed live at TP12 and MDIO idles high at J1.38,\n");
        printf("     so suspect the trace run to U1 or U1's own solder joints.\n");
    } else {
        printf("  => PHY IS ALIVE on MDIO. The wiring and the chip are fine;\n");
        printf("     the ESP32 EMAC's own MDIO setup is what is failing.\n");
    }
    return 0;
}

static esp_err_t phy_wr(uint32_t reg, uint32_t val)
{
    esp_eth_phy_reg_rw_data_t w = { .reg_addr = reg, .reg_value_p = &val };
    return esp_eth_ioctl(s_eth, ETH_CMD_WRITE_PHY_REG, &w);
}
static int cmd_phyw(int argc, char **argv)
{
    if (!s_eth) { printf("ETH not initialised\n"); return 1; }
    if (argc < 3) { printf("phyw <reg> <val>   (val may be 0x...)\n"); return 0; }
    uint32_t r = strtoul(argv[1], NULL, 0), v = strtoul(argv[2], NULL, 0);
    esp_err_t e = phy_wr(r, v);
    printf("write reg %"PRIu32" = 0x%04"PRIx32" -> %s (readback 0x%04"PRIx32")\n",
           r, v, esp_err_to_name(e), phy_rd(r));
    return 0;
}

// RX2TX_LPBK — page 1, register 23, bit 13. The IP101GA reflects every received
// frame back out the wire in this mode, which makes a switch see its own BPDUs
// return and block the port for a "network loop".
//
// The datasheet requires a pull-up on INTR for normal operation; without one the
// part can come out of reset in loopback. `lpbk 0` clears it in software.
//
// Register 20[4:0] is the page select for regs 16-31; default 0x10 (page 16).
static int cmd_lpbk(int argc, char **argv)
{
    if (!s_eth) { printf("ETH not initialised\n"); return 1; }
    uint32_t v = 0;
    phy_wr(20, 1);                       // select page 1
    v = phy_rd(23);
    printf("P1R23 = 0x%04"PRIx32"   RX2TX_LPBK (bit13) = %"PRIu32"\n", v, (v >> 13) & 1);
    if ((v >> 13) & 1)
        printf("  *** LOOPBACK IS ON — this board reflects frames; that is the network loop ***\n");
    if (argc >= 2) {
        uint32_t nv = atoi(argv[1]) ? (v | (1u << 13)) : (v & ~(1u << 13));
        phy_wr(23, nv);
        v = phy_rd(23);
        printf("after write: P1R23 = 0x%04"PRIx32"   RX2TX_LPBK = %"PRIu32"\n", v, (v >> 13) & 1);
    }
    phy_wr(20, 0x10);                    // restore page 16
    return 0;
}

// Bit-banged WS2812 frame on GPIO48, bypassing RMT entirely — the same trick
// that separated wiring from peripheral config on MDIO. If this lights the ring
// and RMT does not, the fault is in led_strip/RMT. If neither lights it, the
// break is in P4 GPIO48 -> J1.39 -> U3 pin 2, the one segment the floating-input
// test never exercised (that noise is picked up on the carrier side of J1).
//
// 360 MHz core: 1 cycle ~2.78 ns. T0H 350ns=126cy, T1H 700ns=252cy, bit 1250ns=450cy.
#define BB_T0H 126
#define BB_T1H 252
#define BB_BIT 450
static IRAM_ATTR void bb_send(const uint8_t *buf, int n)
{
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    taskENTER_CRITICAL(&mux);
    for (int i = 0; i < n; i++) {
        for (int b = 7; b >= 0; b--) {
            uint32_t hi = (buf[i] & (1 << b)) ? BB_T1H : BB_T0H;
            uint32_t t0 = esp_cpu_get_cycle_count();
            gpio_set_level(PIN_HALO_DIN, 1);
            while (esp_cpu_get_cycle_count() - t0 < hi) { }
            gpio_set_level(PIN_HALO_DIN, 0);
            while (esp_cpu_get_cycle_count() - t0 < BB_BIT) { }
        }
    }
    taskEXIT_CRITICAL(&mux);
}
static int cmd_halobb(int argc, char **argv)
{
    uint8_t r = 0, g = 0, b = 60;
    if (argc >= 4) { r = atoi(argv[1]); g = atoi(argv[2]); b = atoi(argv[3]); }
    static uint8_t frame[HALO_COUNT * 3];
    for (int i = 0; i < HALO_COUNT; i++) { frame[i*3+0] = g; frame[i*3+1] = r; frame[i*3+2] = b; }  // GRB
    gpio_config_t gc = { .pin_bit_mask = 1ULL << PIN_HALO_DIN, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&gc);
    gpio_set_level(PIN_HALO_DIN, 0);
    esp_rom_delay_us(300);                      // reset/latch
    bb_send(frame, sizeof(frame));
    esp_rom_delay_us(300);
    printf("bit-banged %d px as GRB(%d,%d,%d) on GPIO%d, RMT bypassed\n",
           HALO_COUNT, g, r, b, PIN_HALO_DIN);
    printf("  NOTE: this steals GPIO%d from RMT — reboot before using `halo` again\n", PIN_HALO_DIN);
    return 0;
}

// Drive any GPIO to a static level so a DMM reads the ACTUAL high/low voltage
// rather than a square wave's average. Needed to check whether GPIO48's high
// level clears U3's 74AHCT TTL threshold (VIH 2.0 V min at 5 V VCC).
static int cmd_pinset(int argc, char **argv)
{
    if (argc < 3) { printf("pinset <gpio> <0|1>\n"); return 0; }
    int pin = atoi(argv[1]), lvl = atoi(argv[2]) ? 1 : 0;
    gpio_config_t g = { .pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&g);
    gpio_set_level(pin, lvl);
    printf("GPIO%d driven STATIC %s — meter it now\n", pin, lvl ? "HIGH (expect ~3.3V)" : "LOW (expect ~0V)");
    return 0;
}

// Distinguish "pin is loaded" from "pin cannot source current".
//  pinpu <gpio>  : input + internal pull-up. Reads 3.3V if nothing loads the net.
//  pinod <gpio>  : report what the pin does driven high vs low, with readback.
static int cmd_pinpu(int argc, char **argv)
{
    if (argc < 2) { printf("pinpu <gpio>   input + internal pull-up\n"); return 0; }
    int pin = atoi(argv[1]);
    gpio_config_t g = { .pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_INPUT,
                        .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE };
    gpio_config(&g);
    vTaskDelay(pdMS_TO_TICKS(50));
    printf("GPIO%d = INPUT + internal pull-up, reads back %d\n", pin, gpio_get_level(pin));
    printf("  ~3.3V on the meter => nothing loads this net; the OUTPUT driver is the problem\n");
    printf("  ~1.2V on the meter => something really is loading it\n");
    return 0;
}
static int cmd_pinod(int argc, char **argv)
{
    if (argc < 2) { printf("pinod <gpio>\n"); return 0; }
    int pin = atoi(argv[1]);
    gpio_config_t g = { .pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_INPUT_OUTPUT };
    gpio_config(&g);
    gpio_set_level(pin, 1); vTaskDelay(pdMS_TO_TICKS(20));
    int hi = gpio_get_level(pin);
    gpio_set_level(pin, 0); vTaskDelay(pdMS_TO_TICKS(20));
    int lo = gpio_get_level(pin);
    gpio_set_level(pin, 1);
    printf("GPIO%d driven HIGH reads back %d, driven LOW reads back %d\n", pin, hi, lo);
    printf("  1/0 = the pad really does follow the driver\n");
    printf("  0/0 = the pad never reaches a logic high — cannot source\n");
    printf("  (left driven HIGH for metering)\n");
    return 0;
}

static int cmd_diag(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("\n===== CARRIER DIAGNOSTIC =====\n"); cmd_chip(0,NULL);
    printf("\n--- i2c ---\n");  cmd_i2c(0,NULL);
    printf("\n--- refclk ---\n"); cmd_refclk(0,NULL);
    printf("\n--- eth ---\n");  cmd_eth(0,NULL);
    printf("\n--- phy ---\n");  cmd_phy(0,NULL);
    printf("\n--- power ---\n");cmd_power(0,NULL);
    printf("\n--- poe ---\n");  cmd_poe(0,NULL);
    printf("\n--- halo ---\n"); cmd_halo(2,(char*[]){"halo","test"});
    printf("\n===== END =====\n");
    return 0;
}

// ─── init ────────────────────────────────────────────────────────────────────
static void on_eth_evt(void *a, esp_event_base_t b, int32_t id, void *d)
{
    (void)a;(void)b;(void)d;
    if (id == ETHERNET_EVENT_CONNECTED)    { s_link = true;  ESP_LOGI(TAG,"*** LINK UP ***"); }
    if (id == ETHERNET_EVENT_DISCONNECTED) { s_link = false; ESP_LOGW(TAG,"link down"); }
}
static void on_ip_evt(void *a, esp_event_base_t b, int32_t id, void *d)
{
    (void)a;(void)b;(void)id;
    ip_event_got_ip_t *e = d;
    ESP_LOGI(TAG, "*** DHCP " IPSTR " ***", IP2STR(&e->ip_info.ip));
}

static void eth_init(void)
{
    gpio_config_t g = { .pin_bit_mask = 1ULL<<PIN_PHY_RST, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&g);
    gpio_set_level(PIN_PHY_RST, 0); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_PHY_RST, 1); vTaskDelay(pdMS_TO_TICKS(50));

    esp_netif_config_t nc = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&nc);
    eth_mac_config_t mc = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t ec = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&ec, &mc);
    eth_phy_config_t pc = ETH_PHY_DEFAULT_CONFIG();
    pc.phy_addr = ESP_ETH_PHY_ADDR_AUTO;     // SCAN — do not assume addr 1
    pc.reset_gpio_num = PIN_PHY_RST;
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&pc);
    esp_eth_config_t cfg = ETH_DEFAULT_CONFIG(mac, phy);

    esp_err_t e = esp_eth_driver_install(&cfg, &s_eth);
    if (e != ESP_OK) {
        s_eth = NULL;
        ESP_LOGE(TAG, "PHY did not answer on MDIO: %s", esp_err_to_name(e));
        ESP_LOGE(TAG, "  suspect U1 power, Y1 25MHz crystal, PHY_nRST, or MDC/MDIO through J1");
        return;
    }
    // Clear IP101 Rx->Tx loopback immediately, before autoneg can complete.
    // The PHY can latch into Rx-to-Tx loopback at reset and reflect frames, which
    // makes managed switches block the port for a "network loop". Doing it here
    // means the PHY is never linked while reflecting.
    {
        uint32_t page = 1, v = 0, restore = 0x10;
        esp_eth_phy_reg_rw_data_t sel = { .reg_addr = 20, .reg_value_p = &page };
        esp_eth_phy_reg_rw_data_t r23 = { .reg_addr = 23, .reg_value_p = &v };
        esp_eth_phy_reg_rw_data_t rst = { .reg_addr = 20, .reg_value_p = &restore };
        if (esp_eth_ioctl(s_eth, ETH_CMD_WRITE_PHY_REG, &sel) == ESP_OK &&
            esp_eth_ioctl(s_eth, ETH_CMD_READ_PHY_REG,  &r23) == ESP_OK) {
            if (v & (1u << 13)) {
                uint32_t nv = v & ~(1u << 13);
                esp_eth_phy_reg_rw_data_t w = { .reg_addr = 23, .reg_value_p = &nv };
                esp_eth_ioctl(s_eth, ETH_CMD_WRITE_PHY_REG, &w);
                ESP_LOGW(TAG, "IP101 was in Rx->Tx LOOPBACK (P1R23=0x%04" PRIx32 "); cleared", v);
            }
        }
        esp_eth_ioctl(s_eth, ETH_CMD_WRITE_PHY_REG, &rst);
    }

    esp_netif_attach(s_netif, esp_eth_new_netif_glue(s_eth));
    esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_evt, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_ip_evt, NULL);
    esp_eth_start(s_eth);
}

void app_main(void)
{
    esp_err_t nv = nvs_flash_init();
    if (nv == ESP_ERR_NVS_NO_FREE_PAGES || nv == ESP_ERR_NVS_NEW_VERSION_FOUND) { nvs_flash_erase(); nvs_flash_init(); }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // GPIO39-48 on the P4 are NOT on the main 3.3V IO rail — they sit on an
    // internal LDO domain (VDDPST, ESP_LDO_VO4 on the ws43 schematic, where it
    // also feeds the microSD pull-ups). Unconfigured it sits ~1.2V, so GPIO48
    // (HALO_DIN, J1.39) cannot reach the 2.0V TTL threshold of U3's 74AHCT1G125
    // and the halo never receives data. Acquire it at 3.3V before using GPIO48.
    // Same mechanism bsp_ws43.c already uses for the MIPI D-PHY on channel 3.
    static esp_ldo_channel_handle_t io_ldo;
    esp_ldo_channel_config_t io_ldo_cfg = { .chan_id = 4, .voltage_mv = 3300 };
    esp_err_t le = esp_ldo_acquire_channel(&io_ldo_cfg, &io_ldo);
    ESP_LOGI(TAG, "GPIO39-48 IO LDO (chan 4 @ 3.3V): %s", esp_err_to_name(le));

    temperature_sensor_config_t tc = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&tc, &s_tsens) == ESP_OK) temperature_sensor_enable(s_tsens);

    led_strip_config_t sc = { .strip_gpio_num = PIN_HALO_DIN, .max_leds = HALO_COUNT,
                              .led_pixel_format = LED_PIXEL_FORMAT_GRB,
                              .led_model = LED_MODEL_SK6812, .flags.invert_out = false };
    // with_dma=false makes led_strip stream 576 symbols (24 px x 24 bits) through
    // the RMT's 48-64 symbol buffer, refilling from an ISR mid-transmission. If
    // that refill stalls, pixel 0 goes out clean and everything after it is
    // garbage — exactly the "only the first LED lights" symptom. DMA sends the
    // whole frame without refills.
    led_strip_rmt_config_t rc = { .clk_src = RMT_CLK_SRC_DEFAULT,
                                  .resolution_hz = 10*1000*1000,
                                  .mem_block_symbols = 1024,
                                  .flags.with_dma = true };
    esp_err_t se = led_strip_new_rmt_device(&sc, &rc, &s_strip);
    if (se != ESP_OK) {
        ESP_LOGE(TAG, "halo RMT init FAILED: %s (handle=%p)", esp_err_to_name(se), s_strip);
        s_strip = NULL;
    } else {
        ESP_LOGI(TAG, "halo RMT ok: gpio%d, %d px, dma=on, handle=%p",
                 PIN_HALO_DIN, HALO_COUNT, s_strip);
        halo_all(0,0,0);
    }

    eth_init();

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rp = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rp.prompt = "carrier>";
    rp.max_cmdline_length = 128;
    esp_console_dev_uart_config_t uc = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uc, &rp, &repl));

    const esp_console_cmd_t cmds[] = {
        { .command="diag",  .help="run every check once",                 .func=&cmd_diag },
        { .command="chip",  .help="chip rev, MAC, flash, heap, die temp",  .func=&cmd_chip },
        { .command="halo",  .help="test|set r g b|px i r g b|bright n|off",.func=&cmd_halo },
        { .command="i2c",   .help="scan both buses | dump <bus> <addr>",   .func=&cmd_i2c  },
        { .command="eth",   .help="link, speed, duplex, IP",              .func=&cmd_eth  },
        { .command="phy",   .help="dump all PHY regs, or phy <reg>",      .func=&cmd_phy  },
        { .command="gpio",  .help="read the J7 breakout pins",            .func=&cmd_gpio },
        { .command="power", .help="halo current ESTIMATE (no sense hw)",  .func=&cmd_power},
        { .command="poe",   .help="why PoE state is invisible on rev-C",  .func=&cmd_poe  },
        { .command="refclk",.help="is the PHY's 50MHz REF_CLK alive?",     .func=&cmd_refclk},
        { .command="nrst",  .help="nrst <0|1> drive PHY reset (meter TP14)",.func=&cmd_nrst },
        { .command="retry", .help="re-attempt PHY detect (scope TP12/MDC)",.func=&cmd_retry},
        { .command="wig",   .help="wig <gpio> [s] square-wave a pin for DMM tracing",.func=&cmd_wig  },
        { .command="pinset",.help="pinset <gpio> <0|1> drive a pin to a static level",.func=&cmd_pinset},
        { .command="pinpu", .help="pinpu <gpio> input + internal pull-up",  .func=&cmd_pinpu},
        { .command="pinod", .help="pinod <gpio> drive hi/lo with readback",  .func=&cmd_pinod},
        { .command="halobb",.help="halobb [r g b] bit-bang the halo, RMT bypassed",.func=&cmd_halobb},
        { .command="mdio",  .help="bit-bang MDIO scan, bypasses the EMAC",   .func=&cmd_mdio },
        { .command="phyw",  .help="phyw <reg> <val> write a PHY register",   .func=&cmd_phyw },
        { .command="lpbk",  .help="lpbk [0|1] read/set RX2TX_LPBK (P1R23[13])",.func=&cmd_lpbk },
    };
    for (unsigned i=0;i<sizeof(cmds)/sizeof(cmds[0]);i++) ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));

    printf("\n\n======== backbox-poe CARRIER CONSOLE ========\n");
    printf("type 'help' for commands, 'diag' to run everything\n\n");
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
