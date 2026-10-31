/* settings_hw_test.c — tests for the pure parts of Settings' hardware pages (src/hw):
 * EDID parsing, KMS mode selection, the Night Light math (temperature, gamma ramp, sunrise and
 * sunset, schedule), mimeapps.list editing and globs2, and the power_supply reader over a fake
 * sysfs tree. Run by `make test` (mk/settings.mk). */
#include "hw/edid.h"
#include "hw/hw_admin.h"
#include "hw/mimeapps.h"
#include "hw/modes.h"
#include "hw/night.h"
#include "hw/power.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int unhex(const char *h, unsigned char *out, size_t max) {
    size_t n = 0;
    for (; h[0] && h[1] && n < max; h += 2) {
        unsigned v;
        if (sscanf(h, "%2x", &v) != 1) return -1;
        out[n++] = (unsigned char)v;
    }
    return (int)n;
}

/* The EDID QEMU's virtio-gpu sends (VM 118, /sys/class/drm/card0-Virtual-1/edid), base block. */
static const char *qemu_edid =
    "00ffffffffffff0049143412000000002a180104a520147806ee91a3544c99260f5054210800e1c0d1c0d100a940b300950081808140ea2900c051201c304026444045cb10000018000000f7000a004082002820000000000000000000fd00327d1ea0ff010a202020202020000000fc0051454d55204d6f6e69746f720a013a";

static void put_desc(unsigned char *d, int slot, unsigned char tag, const char *text) {
    unsigned char *x = d + 54 + 18 * slot;
    memset(x, 0, 18);
    x[3] = tag;
    size_t n = strlen(text);
    for (int i = 0; i < 13; i++) x[5 + i] = i < (int)n ? (unsigned char)text[i] : i == (int)n ? 0x0a : 0x20;
}

static void fix_sum(unsigned char *d) {
    unsigned char s = 0;
    for (int i = 0; i < 127; i++) s = (unsigned char)(s + d[i]);
    d[127] = (unsigned char)(256 - s);
}

static void test_edid(void) {
    unsigned char d[256];
    int n = unhex(qemu_edid, d, sizeof d);
    CHECK(n == 128);
    Edid e;
    CHECK(edid_parse(d, (size_t)n, &e) == 0);
    CHECK(!strcmp(e.vendor, "RHT"));
    CHECK(e.product == 0x1234);
    CHECK(!strcmp(e.name, "QEMU Monitor"));
    CHECK(e.pref_w == 1280 && e.pref_h == 800);
    CHECK(e.width_cm == 32 && e.height_cm == 20);
    CHECK(e.year == 2014 && e.version == 1 && e.revision == 4);
    char id[64], name[64];
    edid_identity(&e, id, sizeof id);
    CHECK(!strcmp(id, "RHT-1234-QEMU_Monitor"));
    edid_display_name(&e, name, sizeof name);
    CHECK(!strcmp(name, "QEMU Monitor"));
    CHECK(edid_diagonal_tenths(&e) == 149);           /* √(32²+20²)/2.54 = 14.86" */

    /* a monitor with a text serial and a binary serial: the text one wins */
    unsigned char m[128];
    memcpy(m, d, 128);
    m[8] = 0x10; m[9] = 0xac;                          /* "DEL" */
    m[10] = 0xc4; m[11] = 0xa0;
    m[12] = 0x78; m[13] = 0x56; m[14] = 0x34; m[15] = 0x12;
    put_desc(m, 2, 0xff, "7HQ2C93");
    put_desc(m, 3, 0xfc, "DELL U2720Q");
    fix_sum(m);
    CHECK(edid_parse(m, 128, &e) == 0);
    CHECK(!strcmp(e.vendor, "DEL") && e.product == 0xa0c4 && e.serial == 0x12345678);
    edid_identity(&e, id, sizeof id);
    CHECK(!strcmp(id, "DEL-a0c4-7HQ2C93"));
    put_desc(m, 2, 0x10, "");                          /* no text serial: the binary one */
    fix_sum(m);
    CHECK(edid_parse(m, 128, &e) == 0);
    edid_identity(&e, id, sizeof id);
    CHECK(!strcmp(id, "DEL-a0c4-12345678"));

    /* broken: checksum, header, short */
    m[60] ^= 1;
    CHECK(edid_parse(m, 128, &e) != 0);
    CHECK(edid_parse(d, 100, &e) != 0);
    d[0] = 1;
    CHECK(edid_parse(d, 128, &e) != 0);
    CHECK(edid_parse(NULL, 0, &e) != 0);
}

static struct drm_mode_modeinfo mk(int w, int h, int clock, int htot, int vtot, int pref, int flags) {
    struct drm_mode_modeinfo m;
    memset(&m, 0, sizeof m);
    m.hdisplay = (uint16_t)w; m.vdisplay = (uint16_t)h;
    m.clock = (uint32_t)clock; m.htotal = (uint16_t)htot; m.vtotal = (uint16_t)vtot;
    m.type = pref ? DRM_MODE_TYPE_PREFERRED : 0;
    m.flags = (uint32_t)flags;
    return m;
}

static void test_modes(void) {
    struct drm_mode_modeinfo modes[] = {
        mk(1920, 1080, 148500, 2200, 1125, 1, 0),   /* 60.000 */
        mk(1920, 1080, 148352, 2200, 1125, 0, 0),   /* 59.940 */
        mk(1920, 1080, 74250, 2200, 1125, 0, 0),    /* 30 */
        mk(1920, 1080, 74250, 2200, 1125, 0, DRM_MODE_FLAG_INTERLACE),   /* 60i */
        mk(2560, 1440, 241500, 2720, 1481, 0, 0),   /* 59.951 */
        mk(1280, 720, 74250, 1650, 750, 0, 0),      /* 60 */
        mk(1280, 720, 74250, 1980, 750, 0, 0),      /* 50 */
        mk(1920, 1080, 297000, 2200, 1125, 0, 0),   /* 120 */
    };
    int n = (int)(sizeof modes / sizeof modes[0]);
    CHECK(mode_refresh_mhz(&modes[0]) == 60000);
    CHECK(mode_refresh_mhz(&modes[1]) == 59940);
    CHECK(mode_refresh_mhz(&modes[3]) == 60000);
    CHECK(mode_refresh_mhz(&modes[4]) == 59951);
    struct drm_mode_modeinfo z = { 0 };
    CHECK(mode_refresh_mhz(&z) == 0);
    CHECK(mode_preferred(modes, n) == 0);
    CHECK(mode_pick(modes, n, 0, 0, 0) == 0);
    CHECK(mode_pick(modes, n, 1920, 1080, 59940) == 1);
    CHECK(mode_pick(modes, n, 1920, 1080, 60000) == 0);  /* progressive, not 60i */
    CHECK(mode_pick(modes, n, 1920, 1080, 0) == 7);      /* highest */
    CHECK(mode_pick(modes, n, 1920, 1080, 75000) == 0);  /* closest to 75 */
    CHECK(mode_pick(modes, n, 1280, 720, 50000) == 6);
    CHECK(mode_pick(modes, n, 800, 600, 0) == -1);
    int res[8];
    int k = mode_resolutions(modes, n, res, 8);
    CHECK(k == 3);
    CHECK(modes[res[0]].hdisplay == 2560 && modes[res[1]].hdisplay == 1920 && modes[res[2]].hdisplay == 1280);
    CHECK(res[1] == 7);                                  /* the 120 Hz one of that size */
    int rates[8];
    k = mode_rates(modes, n, 1920, 1080, rates, 8);
    CHECK(k == 4 && rates[0] == 120000 && rates[1] == 60000 && rates[2] == 59940 && rates[3] == 30000);
    CHECK(mode_preferred(modes, 0) == -1);
    modes[0].type = 0;
    CHECK(mode_preferred(modes, n) == 4);                /* largest without a preferred one */
}

static void test_night(void) {
    double r, g, b;
    night_temp_rgb(6500, &r, &g, &b);
    CHECK(r == 1 && g == 1 && b == 1);
    night_temp_rgb(3400, &r, &g, &b);
    CHECK(r == 1 && g < 1 && b < g && b > 0.4);         /* warm: blue cut the most */
    double g1 = g, b1 = b;
    night_temp_rgb(2700, &r, &g, &b);
    CHECK(g < g1 && b < b1);                            /* monotonic */
    night_temp_rgb(100, &r, &g, &b);                    /* clamped to 1000 K */
    double r2, g2, b2;
    night_temp_rgb(1000, &r2, &g2, &b2);
    CHECK(r == r2 && g == g2 && b == b2 && b == 0);

    uint16_t R[256], G[256], B[256];
    night_ramp(256, 1, 0.5, 0.25, 1, R, G, B);
    CHECK(R[0] == 0 && G[0] == 0 && B[0] == 0);
    CHECK(R[255] == 65535 && G[255] == 32768 && B[255] == 16384);
    CHECK(R[128] == (uint16_t)(128.0 / 255 * 65535 + 0.5));
    night_ramp(256, 1, 1, 1, 0.5, R, G, B);
    CHECK(R[255] == 32768);
    night_ramp(1, 1, 1, 1, 1, R, G, B);
    CHECK(R[0] == 65535);

    /* sunrise/sunset (minutes UTC) against NOAA's calculator, ±4 minutes */
    double rise, set;
    CHECK(night_sun_times(2026, 6, 21, 51.5074, -0.1278, &rise, &set) == 0);       /* London */
    CHECK(fabs(rise - (3 * 60 + 43)) < 4 && fabs(set - (20 * 60 + 21)) < 4);
    CHECK(night_sun_times(2026, 6, 21, -34.6037, -58.3816, &rise, &set) == 0);     /* Buenos Aires */
    CHECK(fabs(rise - (11 * 60 + 0)) < 4 && fabs(set - (20 * 60 + 50)) < 4);
    CHECK(night_sun_times(2026, 12, 21, 35.6762, 139.6503, &rise, &set) == 0);     /* Tokyo, UTC */
    CHECK(fabs(rise - (21 * 60 + 47)) < 4 && fabs(set - (7 * 60 + 32)) < 4);
    CHECK(night_sun_times(2026, 6, 21, 78.22, 15.65, &rise, &set) == -1);          /* Svalbard: midnight sun */

    /* schedule 22:00 → 07:00 with a 30-minute fade */
    int from = 22 * 60, to = 7 * 60;
    CHECK(night_amount(12 * 60, from, to, 30) == 0);
    CHECK(night_amount(22 * 60, from, to, 30) == 0);
    CHECK(fabs(night_amount(22 * 60 + 15, from, to, 30) - 0.5) < 1e-9);
    CHECK(night_amount(23 * 60, from, to, 30) == 1);
    CHECK(night_amount(3 * 60, from, to, 30) == 1);
    CHECK(fabs(night_amount(6 * 60 + 45, from, to, 30) - 0.5) < 1e-9);
    CHECK(night_amount(7 * 60, from, to, 30) == 0);
    CHECK(night_amount(3 * 60, from, to, 0) == 1);
    CHECK(night_amount(3 * 60, 600, 600, 30) == 0);
    CHECK(night_amount(-60, from, to, 0) == 1);          /* 23:00 written as -60 */

    double lat, lon;
    CHECK(night_parse_coords("-3436-05827", &lat, &lon) == 0 && fabs(lat + 34.6) < 1e-9 && fabs(lon + 58.45) < 1e-9);
    CHECK(night_parse_coords("+404251-0740023", &lat, &lon) == 0 && fabs(lat - 40.714167) < 1e-5 && fabs(lon + 74.006389) < 1e-5);
    CHECK(night_parse_coords("+4230+00131", &lat, &lon) == 0 && fabs(lon - (1 + 31 / 60.0)) < 1e-9);
    CHECK(night_parse_coords("+4290+00131", &lat, &lon) != 0);
    CHECK(night_parse_coords("4230+00131", &lat, &lon) != 0);
    CHECK(night_parse_coords("+4230", &lat, &lon) != 0);
    const char *tab =
        "# comment\n"
        "AD\t+4230+00131\tEurope/Andorra\n"
        "AR\t-3436-05827\tAmerica/Argentina/Buenos_Aires\tBuenos Aires (BA, CF)\n"
        "US\t+404251-0740023\tAmerica/New_York\tEastern (most areas)\n";
    CHECK(night_zone_coords(tab, "America/New_York", &lat, &lon) == 0 && lat > 40.7 && lon < -74);
    CHECK(night_zone_coords(tab, "America/Argentina/Buenos_Aires", &lat, &lon) == 0 && lat < -34);
    CHECK(night_zone_coords(tab, "America/New", &lat, &lon) != 0);
    CHECK(night_zone_coords(tab, "Mars/Olympus", &lat, &lon) != 0);
}

static void test_mimeapps(void) {
    CHECK(mime_valid("text/plain") && mime_valid("x-scheme-handler/https") && mime_valid("application/vnd.oasis.opendocument.text"));
    CHECK(mime_valid("image/svg+xml"));
    CHECK(!mime_valid("text") && !mime_valid("/plain") && !mime_valid("text/") && !mime_valid("a/b/c") && !mime_valid("Text/plain"));
    CHECK(!mime_valid("text/pl ain") && !mime_valid("text/plain;"));
    CHECK(desktop_id_valid("org.gnome.Evince.desktop") && desktop_id_valid("lordos-photos.desktop"));
    CHECK(!desktop_id_valid("../x.desktop") && !desktop_id_valid("a/b.desktop") && !desktop_id_valid("x.desk") && !desktop_id_valid(".desktop"));
    CHECK(!desktop_id_valid("-x.desktop") && !desktop_id_valid("a b.desktop"));

    const char *text =
        "# mine\n"
        "[Added Associations]\n"
        "text/plain=mousepad.desktop;\n"
        "\n"
        "[Default Applications]\n"
        "text/plain = org.gnome.TextEditor.desktop;mousepad.desktop;\n"
        "application/pdf=bad/../id;org.gnome.Evince.desktop;\n"
        "\n"
        "[Removed Associations]\n"
        "image/png=gimp.desktop;\n";
    char id[DESKTOP_ID_MAX];
    CHECK(mimeapps_default(text, "text/plain", id, sizeof id) == 0 && !strcmp(id, "org.gnome.TextEditor.desktop"));
    CHECK(mimeapps_default(text, "application/pdf", id, sizeof id) == 0 && !strcmp(id, "org.gnome.Evince.desktop"));
    CHECK(mimeapps_default(text, "image/png", id, sizeof id) != 0);
    CHECK(mimeapps_default(text, "text/plai", id, sizeof id) != 0);
    CHECK(mimeapps_removed(text, "image/png", "gimp.desktop") && !mimeapps_removed(text, "image/png", "eog.desktop"));

    char *t2 = mimeapps_set_default(text, "text/plain", "mousepad.desktop");
    CHECK(t2 != NULL);
    CHECK(mimeapps_default(t2, "text/plain", id, sizeof id) == 0 && !strcmp(id, "mousepad.desktop"));
    CHECK(strstr(t2, "# mine\n") == t2);                         /* comments kept */
    CHECK(strstr(t2, "[Default Applications]\ntext/plain=mousepad.desktop;\napplication/pdf=bad/../id;org.gnome.Evince.desktop;\n\n[Removed"));
    CHECK(strstr(t2, "[Added Associations]\ntext/plain=mousepad.desktop;\n\n"));
    CHECK(strstr(t2, "image/png=gimp.desktop;\n"));
    char *t3 = mimeapps_set_default(t2, "image/png", "lordos-photos.desktop");
    CHECK(t3 && mimeapps_default(t3, "image/png", id, sizeof id) == 0 && !strcmp(id, "lordos-photos.desktop"));
    /* new key goes at the end of its group, before the blank line */
    CHECK(t3 && strstr(t3, "application/pdf=bad/../id;org.gnome.Evince.desktop;\nimage/png=lordos-photos.desktop;\n\n[Removed"));
    CHECK(t3 && strstr(t3, "[Added Associations]\ntext/plain=mousepad.desktop;\nimage/png=lordos-photos.desktop;\n"));
    char *t4 = mimeapps_set_default(t3, "text/plain", "org.gnome.TextEditor.desktop");
    CHECK(t4 && strstr(t4, "[Added Associations]\ntext/plain=org.gnome.TextEditor.desktop;mousepad.desktop;\n"));
    free(t2); free(t3); free(t4);

    char *e = mimeapps_set_default(NULL, "x-scheme-handler/https", "firefox-esr.desktop");
    CHECK(e && !strcmp(e, "[Default Applications]\nx-scheme-handler/https=firefox-esr.desktop;\n\n[Added Associations]\nx-scheme-handler/https=firefox-esr.desktop;\n"));
    free(e);
    e = mimeapps_set_default("[Default Applications]\ntext/plain=a.desktop;", "text/plain", "b.desktop");   /* no final newline */
    CHECK(e && mimeapps_default(e, "text/plain", id, sizeof id) == 0 && !strcmp(id, "b.desktop"));
    free(e);
    CHECK(mimeapps_set_default(text, "text/plain", "../evil.desktop") == NULL);
    CHECK(mimeapps_set_default(text, "text/plain\n[x]", "a.desktop") == NULL);

    const char *globs =
        "# glob\n"
        "50:text/plain:*.txt\n"
        "50:application/pdf:*.pdf\n"
        "50:image/jpeg:*.jpg\n"
        "50:image/jpeg:*.jpeg\n"
        "50:text/x-csrc:*.c\n"
        "50:text/x-c++src:*.C:cs\n"
        "55:application/x-compressed-tar:*.tar.gz\n"
        "50:application/gzip:*.gz\n"
        "10:text/x-readme:README*\n";
    char mt[MIME_MAX];
    CHECK(mime_guess(globs, "/home/u/Doc.PDF", mt, sizeof mt) == 0 && !strcmp(mt, "application/pdf"));
    CHECK(mime_guess(globs, "a.tar.gz", mt, sizeof mt) == 0 && !strcmp(mt, "application/x-compressed-tar"));
    CHECK(mime_guess(globs, "a.gz", mt, sizeof mt) == 0 && !strcmp(mt, "application/gzip"));
    CHECK(mime_guess(globs, "x.c", mt, sizeof mt) == 0 && !strcmp(mt, "text/x-csrc"));
    CHECK(mime_guess(globs, "x.C", mt, sizeof mt) == 0 && !strcmp(mt, "text/x-c++src"));
    CHECK(mime_guess(globs, "README.md", mt, sizeof mt) == 0 && !strcmp(mt, "text/x-readme"));
    CHECK(mime_guess(globs, "noext", mt, sizeof mt) != 0);
}

static void wfile(const char *dir, const char *dev, const char *name, const char *val) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, dev);
    mkdir(p, 0700);
    snprintf(p, sizeof p, "%s/%s/%s", dir, dev, name);
    FILE *f = fopen(p, "w");
    if (f) { fprintf(f, "%s\n", val); fclose(f); }
}

static void test_power(void) {
    char dir[] = "/tmp/lord-power-test-XXXXXX";
    if (!mkdtemp(dir)) { CHECK(0); return; }
    PowerState ps;
    CHECK(power_read(dir, &ps) == 0 && !ps.has_battery && ps.on_ac && ps.percent == -1);
    wfile(dir, "AC", "type", "Mains");
    wfile(dir, "AC", "online", "0");
    wfile(dir, "BAT0", "type", "Battery");
    wfile(dir, "BAT0", "status", "Discharging");
    wfile(dir, "BAT0", "capacity", "42");
    wfile(dir, "BAT0", "energy_now", "21000000");          /* 21 Wh */
    wfile(dir, "BAT0", "energy_full", "50000000");
    wfile(dir, "BAT0", "energy_full_design", "57000000");
    wfile(dir, "BAT0", "power_now", "7000000");            /* 7 W → 3 h */
    wfile(dir, "BAT0", "cycle_count", "312");
    wfile(dir, "BAT0", "model_name", "5B10W13930");
    wfile(dir, "BAT0", "technology", "Li-poly");
    wfile(dir, "hidpp_battery_0", "type", "Battery");      /* a mouse: not the system battery */
    wfile(dir, "hidpp_battery_0", "scope", "Device");
    wfile(dir, "hidpp_battery_0", "capacity", "90");
    wfile(dir, "hidpp_battery_0", "status", "Discharging");
    CHECK(power_read(dir, &ps) == 3);
    CHECK(ps.has_battery && !ps.on_ac && ps.percent == 42 && !ps.charging);
    CHECK(ps.minutes_left == 180);
    const PowerSupply *b = &ps.s[1];
    CHECK(!strcmp(b->name, "BAT0") && b->health == 87 && b->cycles == 312 && fabs(b->watts - 7) < 1e-9);
    CHECK(!strcmp(b->technology, "Li-poly") && !strcmp(b->model, "5B10W13930"));
    CHECK(ps.s[2].device_scope && ps.s[2].percent == 90);

    /* charging with charge_* / current_now, time to full */
    wfile(dir, "AC", "online", "1");
    wfile(dir, "BAT0", "status", "Charging");
    char p[512];
    const char *gone[] = { "energy_now", "energy_full", "energy_full_design", "power_now", "capacity" };
    for (int i = 0; i < 5; i++) { snprintf(p, sizeof p, "%s/BAT0/%s", dir, gone[i]); unlink(p); }
    wfile(dir, "BAT0", "charge_now", "2000000");           /* 2 Ah */
    wfile(dir, "BAT0", "charge_full", "4000000");
    wfile(dir, "BAT0", "charge_full_design", "4000000");
    wfile(dir, "BAT0", "current_now", "1000000");          /* 1 A → 2 h to full */
    wfile(dir, "BAT0", "voltage_now", "12000000");
    CHECK(power_read(dir, &ps) == 3);
    CHECK(ps.on_ac && ps.charging && ps.percent == 50 && ps.minutes_left == 120);
    CHECK(ps.s[1].health == 100 && fabs(ps.s[1].watts - 12) < 1e-9);
    char json[4096];
    size_t n = power_json(&ps, json, sizeof json);
    CHECK(n > 0 && n < sizeof json && json[n - 1] == '}');
    CHECK(strstr(json, "\"percent\":50") && strstr(json, "\"name\":\"BAT0\""));
    char small[40];
    n = power_json(&ps, small, sizeof small);
    CHECK(n < sizeof small);

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd)) { /* best effort */ }
}

/* What leaves the hw-serial-numbers action: real serials only, one safe line. */
static void test_serial_clean(void) {
    char o[64];
    CHECK(!hw_serial_clean("PF1ABCDE\n", 9, o, sizeof o) && !strcmp(o, "PF1ABCDE"));
    CHECK(!hw_serial_clean("  VMware-56 4d 12.ab/x:1_2\n", 27, o, sizeof o) && !strcmp(o, "VMware-56 4d 12.ab/x:1_2"));
    CHECK(!hw_serial_clean("10000000deadbeef\0", 17, o, sizeof o) && !strcmp(o, "10000000deadbeef"));     /* a device tree string */
    static const char *const no[] = { "", "\n", "   ", "To Be Filled By O.E.M.", "Default string", "Not Specified", "NONE", "unknown\n",
                                       "System Serial Number", "Base Board Serial Number", "0123456789", "0000000000", "XXXXXXXX", "0" };
    for (size_t i = 0; i < sizeof no / sizeof no[0]; i++) CHECK(hw_serial_clean(no[i], strlen(no[i]), o, sizeof o) == -1 && !o[0]);
    /* anything that could end the protocol line or carry text of another kind */
    static const char *const bad[] = { "AB\nLOG serial x", "A|B", "A;rm", "A$(x)", "A\tB", "A\"B", "A\xc3\xa9" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) CHECK(hw_serial_clean(bad[i], strlen(bad[i]), o, sizeof o) == -1 && !o[0]);
    CHECK(hw_serial_clean("A\0B", 3, o, sizeof o) == -1);                                    /* a NUL inside */
    char l[80];
    memset(l, 'a', sizeof l);
    l[0] = 'b';
    CHECK(hw_serial_clean(l, 64, o, sizeof o) == -1);                                        /* too long */
    CHECK(!hw_serial_clean(l, 63, o, sizeof o) && strlen(o) == 63);
}

/* the admin helper's functions hw_admin.c calls (not used by the validators) */
void admin_say(const char *kind, const char *fmt, ...) { (void)kind; (void)fmt; }
int admin_run(const char *const argv[]) { (void)argv; return -1; }
int admin_run_capture(const char *const argv[], char *out, size_t cap) { (void)argv; if (cap) out[0] = 0; return -1; }

/* rpi-eeprom-update's report: the strict parser behind rpi-eeprom-status */
static void test_eeprom(void) {
    /* as printed on the Raspberry Pi 5 (rpi-eeprom 28.34-1, 2026-10-25) */
    const char *avail =
        "*** UPDATE AVAILABLE ***\n\nRun \"sudo rpi-eeprom-update -a\" to install this update without a prompt.\n\n"
        "To configure the bootloader update policy run \"sudo raspi-config\"\n\n"
        "BOOTLOADER: update available\n"
        "   CURRENT: Tue May 26 03:01:25 PM UTC 2026 (1779807685)\n"
        "    LATEST: Fri Sep 25 03:59:09 PM UTC 2026 (1790351949)\n"
        "   RELEASE: default (/usr/lib/firmware/raspberrypi/bootloader-2712/default)\n"
        "            Use raspi-config to change the release.\n";
    HwEeprom e;
    CHECK(!hw_eeprom_parse(avail, &e) && e.update && e.current == 1779807685LL && e.latest == 1790351949LL && !strcmp(e.release, "default"));
    const char *same =
        "BOOTLOADER: up to date\n   CURRENT: Fri Sep 25 03:59:09 PM UTC 2026 (1790351949)\n"
        "    LATEST: Fri Sep 25 03:59:09 PM UTC 2026 (1790351949)\n   RELEASE: latest (/usr/lib/firmware/raspberrypi/bootloader-2712/latest)\n"
        "            Use raspi-config to change the release.\n";
    CHECK(!hw_eeprom_parse(same, &e) && !e.update && e.current == e.latest && !strcmp(e.release, "latest"));
    /* the running version unknown (a chroot): never "available" */
    CHECK(!hw_eeprom_parse("BOOTLOADER: up to date\n   CURRENT: unknown\n    LATEST: x (1790351949)\n   RELEASE: default (/x)\n", &e) && !e.update && !e.current);
    CHECK(hw_eeprom_parse("BOOTLOADER: update available\n   CURRENT: unknown\n    LATEST: x (1790351949)\n   RELEASE: default (/x)\n", &e));
    /* the verdict must agree with the numbers */
    CHECK(hw_eeprom_parse("BOOTLOADER: up to date\n   CURRENT: x (1779807685)\n    LATEST: x (1790351949)\n   RELEASE: default (/x)\n", &e));
    CHECK(hw_eeprom_parse("BOOTLOADER: update available\n   CURRENT: x (1790351949)\n    LATEST: x (1790351949)\n   RELEASE: default (/x)\n", &e));
    /* the Pi 4's VL805 block after the bootloader's is not read */
    CHECK(!hw_eeprom_parse("BOOTLOADER: up to date\n   CURRENT: x (1790351949)\n    LATEST: x (1790351949)\n   RELEASE: default (/x)\n\n"
                           "  VL805_FW: Dedicated VL805 EEPROM\n     VL805: up to date\n   CURRENT: 000138c0\n    LATEST: 000138c0\n", &e) && !e.update);
    /* refused: missing lines, repeated lines, bad numbers, odd release names, nothing at all */
    CHECK(hw_eeprom_parse("", &e) && hw_eeprom_parse("garbage\n", &e));
    CHECK(hw_eeprom_parse("BOOTLOADER: up to date\n   CURRENT: x (1790351949)\n   RELEASE: default (/x)\n", &e));
    CHECK(hw_eeprom_parse("BOOTLOADER: up to date\n   CURRENT: x (1790351949)\n   CURRENT: x (1790351949)\n    LATEST: x (1790351949)\n   RELEASE: default (/x)\n", &e));
    CHECK(hw_eeprom_parse("BOOTLOADER: up to date\n   CURRENT: x (17903519490)\n    LATEST: x (1790351949)\n   RELEASE: default (/x)\n", &e));
    CHECK(hw_eeprom_parse("BOOTLOADER: up to date\n   CURRENT: x (1790351949) \n    LATEST: x (1790351949)\n   RELEASE: default (/x)\n", &e));
    CHECK(hw_eeprom_parse("BOOTLOADER: up to date\n   CURRENT: x (-1)\n    LATEST: x (1790351949)\n   RELEASE: default (/x)\n", &e));
    CHECK(hw_eeprom_parse("BOOTLOADER: up to date\n   CURRENT: x (1790351949)\n    LATEST: x (1790351949)\n   RELEASE: Default;rm (/x)\n", &e));
    CHECK(hw_eeprom_parse("BOOTLOADER: up to date\n   CURRENT: x (1790351949)\n    LATEST: x (1790351949)\n   RELEASE: averyveryverylongname (/x)\n", &e));
    CHECK(hw_eeprom_parse("BOOTLOADER: up to date\n   CURRENT: x (1790351949)\n    LATEST: x (1790351949)\n   RELEASE:  (/x)\n", &e));
    /* the board: BCM2712 in the device tree's compatible list */
    static const char pi5[] = "raspberrypi,5-model-b\0brcm,bcm2712\0";
    static const char pi4[] = "raspberrypi,4-model-b\0brcm,bcm2711\0";
    static const char trick[] = "brcm,bcm2712x\0xbrcm,bcm2712\0";
    CHECK(hw_is_bcm2712(pi5, sizeof pi5 - 1) && !hw_is_bcm2712(pi4, sizeof pi4 - 1) && !hw_is_bcm2712(trick, sizeof trick - 1));
    CHECK(!hw_is_bcm2712("brcm,bcm271", 11) && !hw_is_bcm2712(NULL, 0));
}

static void test_admin(void) {
    CHECK(hw_time_valid("2026-10-18 03:15:00") && hw_time_valid("2028-02-29 23:59:59"));
    CHECK(!hw_time_valid("2026-02-29 10:00:00") && !hw_time_valid("2026-13-01 10:00:00") && !hw_time_valid("2026-10-18 24:00:00"));
    CHECK(!hw_time_valid("2026-10-18T03:15:00") && !hw_time_valid("2026-10-18 03:15") && !hw_time_valid("1999-12-31 23:59:59"));
    CHECK(!hw_time_valid("2026-10-18 03:15:0;") && !hw_time_valid("") && !hw_time_valid(NULL));
    CHECK(hw_button_action_valid("suspend", 1) && hw_button_action_valid("poweroff", 0) && hw_button_action_valid("ignore", 1));
    CHECK(!hw_button_action_valid("lock", 1) && !hw_button_action_valid("kexec", 0) && !hw_button_action_valid("suspend\n", 0));
    /* rpi-case: only the known cases, fixed text with the host-mode USB line for the Argon ONE V5 */
    CHECK(hw_rpi_case_text("none") && !strstr(hw_rpi_case_text("none"), "dtoverlay"));
    CHECK(hw_rpi_case_text("argon-one-v5") && strstr(hw_rpi_case_text("argon-one-v5"), "\n[pi5]\ndtoverlay=dwc2,dr_mode=host\n[all]\n"));
    CHECK(!hw_rpi_case_text("") && !hw_rpi_case_text("argon") && !hw_rpi_case_text("none ") && !hw_rpi_case_text("../x"));
    CHECK(hw_backlight_name_valid("intel_backlight") && hw_backlight_name_valid("amdgpu_bl1") && hw_backlight_name_valid("acpi_video0"));
    CHECK(!hw_backlight_name_valid("../x") && !hw_backlight_name_valid(".x") && !hw_backlight_name_valid("a/b") && !hw_backlight_name_valid(""));
    /* video-optimize: closed values only, fixed text out */
    char cr[1024], ff[512];
    CHECK(!hw_video_files("auto off no no", cr, sizeof cr, ff, sizeof ff));
    CHECK(strstr(cr, "\"" HW_VIDEO_EXT_ID "\"") && strstr(cr, "\"mode\": \"auto\"") && strstr(cr, "\"hwVp9\": false") && strstr(cr, "\"autoLower\": false"));
    CHECK(strstr(ff, "pref(\"media.av1.enabled\", false);"));
    CHECK(!hw_video_files("auto on yes yes", cr, sizeof cr, ff, sizeof ff) && strstr(cr, "\"autoLower\": true") && !strstr(ff, "pref("));
    CHECK(!hw_video_files("never off no no", cr, sizeof cr, ff, sizeof ff) && !strstr(ff, "pref("));
    CHECK(!hw_video_files("always off yes yes", cr, sizeof cr, ff, sizeof ff) && strstr(ff, "media.av1.enabled"));
    CHECK(hw_video_files("auto off no", cr, sizeof cr, ff, sizeof ff) && hw_video_files("auto off no no x", cr, sizeof cr, ff, sizeof ff));
    CHECK(hw_video_files("sometimes off no no", cr, sizeof cr, ff, sizeof ff) && hw_video_files("auto maybe no no", cr, sizeof cr, ff, sizeof ff));
    CHECK(hw_video_files("auto off true no", cr, sizeof cr, ff, sizeof ff) && hw_video_files("auto\" off no no", cr, sizeof cr, ff, sizeof ff));
    CHECK(hw_video_files("", cr, sizeof cr, ff, sizeof ff) && hw_video_files(NULL, cr, sizeof cr, ff, sizeof ff));
}

int main(void) {
    test_admin();
    test_eeprom();
    test_edid();
    test_modes();
    test_night();
    test_mimeapps();
    test_power();
    test_serial_clean();
    printf("settings-hw: %d/%d passed\n", checks - fails, checks);
    return fails != 0;
}
