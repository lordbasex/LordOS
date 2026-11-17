/* hidpp_test.c — lord-hidpp's protocol (src/hw/hidpp.c) against a fake device on a socketpair, and lord-server's
 * reading of its output (src/hw/hidppsvc.c): only the three battery features are ever asked, answers to anyone
 * else are ignored, and a malformed line or an unknown hidraw is dropped. */
#include "../src/hw/hidpp.h"
#include "../src/hw/hidppsvc.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(x) do { checks++; if (!(x)) { fails++; fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)

/* the fake device: which features it has and what it answers */
typedef struct {
    int fd;
    int has1004, soc, has1000, has1001;
    int noise;                       /* a notification and another program's answer before each of ours */
    uint16_t asked[32];              /* every feature id looked up */
    int nasked;
    int writes_other;                /* any request that isn't a lookup or one of the battery functions */
} Dev;

static void *device(void *arg) {
    Dev *d = arg;
    uint8_t q[64];
    for (;;) {
        ssize_t n = read(d->fd, q, sizeof q);
        if (n <= 0) return NULL;
        if (n != 20 || q[0] != 0x11) { d->writes_other++; continue; }
        uint8_t feat = q[2], fn = q[3] >> 4, sw = q[3] & 0x0F;
        uint8_t r[20] = { 0x11, 0x01, feat, q[3] };
        if (d->noise) {
            uint8_t note[20] = { 0x11, 0x01, 0x05, 0x00, 1, 2, 3 };          /* a notification */
            write(d->fd, note, 20);
            uint8_t other[20] = { 0x11, 0x01, feat, (uint8_t)(fn << 4 | 0x01), 99 };   /* the kernel's swid */
            write(d->fd, other, 20);
        }
        if (sw != HIDPP_SWID) d->writes_other++;
        if (feat == 0x00 && fn == 0) {                                       /* root: getFeature */
            uint16_t id = (uint16_t)(q[4] << 8 | q[5]);
            if (d->nasked < 32) d->asked[d->nasked++] = id;
            r[4] = id == 0x1004 && d->has1004 ? 7 : id == 0x1000 && d->has1000 ? 8 : id == 0x1001 && d->has1001 ? 9 : 0;
        } else if (feat == 7 && fn == 0) { r[4] = 0x0F; r[5] = d->soc ? 0x02 : 0x00; }   /* capabilities */
        else if (feat == 7 && fn == 1) { r[4] = 64; r[5] = 0x04; r[6] = 1; }           /* 64 %, charging */
        else if (feat == 8 && fn == 0) { r[4] = 85; r[5] = 50; r[6] = 0; }             /* 85 %, discharging */
        else if (feat == 9 && fn == 0) { r[4] = 3922 >> 8; r[5] = 3922 & 0xFF; r[6] = 0x80; }  /* 3.922 V, charging */
        else { d->writes_other++; uint8_t e[20] = { 0x11, 0x01, 0xFF, feat, q[3], 0x02 }; write(d->fd, e, 20); continue; }
        write(d->fd, r, 20);
    }
}

static int ask_battery(Dev *d, HidppBattery *b) {
    int sv[2];
    socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv);
    d->fd = sv[1];
    pthread_t th;
    pthread_create(&th, NULL, device, d);
    int r = hidpp_battery(sv[0], 300, b);
    close(sv[0]);
    pthread_join(th, NULL);
    close(sv[1]);
    return r;
}

static void wfile(const char *dir, const char *rel) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, rel);
    for (char *s = p + strlen(dir) + 1; (s = strchr(s, '/')); s++) { *s = 0; mkdir(p, 0755); *s = '/'; }
    mkdir(p, 0755);
}

int main(void) {
    HidppBattery b;
    /* a device with Unified Battery giving a state of charge */
    Dev a = { .has1004 = 1, .soc = 1, .has1000 = 1 };
    CHECK(ask_battery(&a, &b) == 0 && b.percent == 64 && b.status == 1 && b.source == 0x1004);
    /* Unified Battery with levels only (the MX Master 3 in the kernel): Battery Status gives the percentage */
    Dev l = { .has1004 = 1, .soc = 0, .has1000 = 1, .noise = 1 };
    CHECK(ask_battery(&l, &b) == 0 && b.percent == 85 && b.status == 0 && b.source == 0x1000);
    /* only the voltage */
    Dev v = { .has1001 = 1 };
    CHECK(ask_battery(&v, &b) == 0 && b.source == 0x1001 && b.percent == 70 && b.status == 1);
    /* levels only and nothing else: no number made up */
    Dev none = { .has1004 = 1, .soc = 0 };
    CHECK(ask_battery(&none, &b) == -1);
    /* only the three features are ever looked up, and nothing but their read functions asked */
    Dev *all[] = { &a, &l, &v, &none };
    for (int i = 0; i < 4; i++) {
        CHECK(all[i]->writes_other == 0);
        for (int k = 0; k < all[i]->nasked; k++) CHECK(all[i]->asked[k] == 0x1004 || all[i]->asked[k] == 0x1000 || all[i]->asked[k] == 0x1001);
    }

    /* the request and the match */
    uint8_t q[20], p[16];
    hidpp_request(q, 3, 1, (uint8_t[]){ 0xAB }, 1);
    CHECK(q[0] == 0x11 && q[1] == 0xFF && q[2] == 3 && q[3] == (0x10 | HIDPP_SWID) && q[4] == 0xAB && q[19] == 0);
    uint8_t ok[20] = { 0x11, 1, 3, 0x10 | HIDPP_SWID, 42 }, err[20] = { 0x11, 1, 0xFF, 3, 0x10 | HIDPP_SWID, 5 }, other[20] = { 0x11, 1, 3, 0x11, 42 };
    CHECK(hidpp_match(ok, 20, 3, 1, p) == 1 && p[0] == 42);
    CHECK(hidpp_match(err, 20, 3, 1, p) == -1);
    CHECK(hidpp_match(other, 20, 3, 1, p) == 0);
    CHECK(hidpp_match(ok, 5, 3, 1, p) == 0);
    /* the voltage curve */
    CHECK(hidpp_voltage_percent(4300) == 100 && hidpp_voltage_percent(3400) == 0 && hidpp_voltage_percent(3800) == 50);
    CHECK(hidpp_voltage_percent(3830) > 50 && hidpp_voltage_percent(3830) < 60);

    /* lord-server reading lord-hidpp's lines: numbers in range only */
    int h, pc, st;
    CHECK(hidpp_parse_line("4 85 0", &h, &pc, &st) == 0 && h == 4 && pc == 85 && st == 0);
    CHECK(hidpp_parse_line("2 100 2\n", &h, &pc, &st) == 0 && st == 2);
    const char *bad[] = { "", "4 85", "4 101 0", "4 85 3", "-1 5 0", "4 85 0 x", "4 8a 0", "1000 5 0", "4 85 0; rm", NULL };
    for (int i = 0; bad[i]; i++) CHECK(hidpp_parse_line(bad[i], &h, &pc, &st) == -1);
    /* and finding the device's power supply in a fake sysfs, hidpp_battery_<n> only */
    char dir[] = "/tmp/hidpp-test-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    wfile(dir, "hidraw4/device/power_supply/hidpp_battery_0");
    wfile(dir, "hidraw5/device/power_supply/BAT0");
    wfile(dir, "hidraw6/device/power_supply/hidpp_battery_../x");
    char s[32];
    CHECK(hidpp_supply_of(dir, 4, s, sizeof s) == 0 && !strcmp(s, "hidpp_battery_0"));
    CHECK(hidpp_supply_of(dir, 5, s, sizeof s) == -1);
    CHECK(hidpp_supply_of(dir, 6, s, sizeof s) == -1);
    CHECK(hidpp_supply_of(dir, 7, s, sizeof s) == -1);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd)) { /* a leftover in /tmp */ }

    printf("hidpp-test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
