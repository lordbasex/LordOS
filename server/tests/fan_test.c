/* fan_test.c — axia-fand without hardware (docs/FANS.md):
 *   - curve evaluation and hysteresis, presets, output clamping;
 *   - fan.json parser: valid files, round trip, and many broken ones (fuzz-ish);
 *   - socket request parser;
 *   - the sysfs driver against a fake tree: a Raspberry Pi 5 fan header with kernel trips and a PC
 *     Super I/O chip, take/set/release and the crash restore file.
 * Usage: build/fan-test           runs everything
 *        build/fan-test mkfake DIR VARIANT   builds a fake tree (pi5 | pc | none) for manual runs */
#include "fan/fancurve.h"
#include "fan/fanhw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- curve ---- */

static void test_curve(void) {
    FanConfig cfg;
    fan_config_default(&cfg, 1);
    CHECK(cfg.profile == FP_BALANCED);
    FanCurve c;
    fan_profile_curve(&cfg, &c);
    CHECK(c.n == 3 && c.p[0].temp == 55 && c.p[0].pct == 30 && c.p[2].pct == 100);
    CHECK(fan_curve_valid(&c) == 0);
    /* going up */
    int s = -1;
    s = fan_curve_step(&c, 3, s, 40000); CHECK(s == -1); CHECK(fan_curve_pct(&c, s) == 0);
    s = fan_curve_step(&c, 3, s, 54999); CHECK(s == -1);
    s = fan_curve_step(&c, 3, s, 55000); CHECK(s == 0); CHECK(fan_curve_pct(&c, s) == 30);
    s = fan_curve_step(&c, 3, s, 61000); CHECK(s == 1); CHECK(fan_curve_pct(&c, s) == 55);
    s = fan_curve_step(&c, 3, s, 70000); CHECK(s == 2); CHECK(fan_curve_pct(&c, s) == 100);
    /* jump straight from off to the top */
    CHECK(fan_curve_step(&c, 3, -1, 80000) == 2);
    /* hysteresis on the way down: 65 → stays until below 62 */
    s = fan_curve_step(&c, 3, 2, 64000); CHECK(s == 2);
    s = fan_curve_step(&c, 3, s, 62000); CHECK(s == 2);
    s = fan_curve_step(&c, 3, s, 61999); CHECK(s == 1);
    s = fan_curve_step(&c, 3, s, 57500); CHECK(s == 1);
    s = fan_curve_step(&c, 3, s, 56999); CHECK(s == 0);
    s = fan_curve_step(&c, 3, s, 52000); CHECK(s == 0);
    s = fan_curve_step(&c, 3, s, 51999); CHECK(s == -1);
    /* big drop leaves several steps at once */
    CHECK(fan_curve_step(&c, 3, 2, 30000) == -1);
    /* oscillating around a threshold does not flap */
    s = 0;
    for (int i = 0; i < 20; i++) { s = fan_curve_step(&c, 3, s, i & 1 ? 54500 : 55500); CHECK(s == 0); }
    /* bad previous steps are clamped */
    CHECK(fan_curve_step(&c, 3, 99, 70000) == 2);
    CHECK(fan_curve_step(&c, 3, -7, 20000) == -1);
    /* auto has no curve */
    cfg.profile = FP_AUTO;
    fan_profile_curve(&cfg, &c);
    CHECK(c.n == 0 && fan_curve_step(&c, 3, 1, 90000) == -1);
    /* always on: floor */
    cfg.profile = FP_ALWAYS;
    fan_profile_curve(&cfg, &c);
    CHECK(fan_curve_pct(&c, -1) == 40);
    CHECK(fan_curve_pct(&c, fan_curve_step(&c, 3, -1, 66000)) == 100);
    /* output clamp */
    CHECK(fan_output_pct(0) == 0 && fan_output_pct(-5) == 0 && fan_output_pct(1) == FAN_MIN_PCT);
    CHECK(fan_output_pct(60) == 60 && fan_output_pct(150) == 100);
    /* every preset is valid */
    for (int p = FP_QUIET; p <= FP_ALWAYS; p++) { cfg.profile = (FanProfile)p; fan_profile_curve(&cfg, &c); CHECK(fan_curve_valid(&c) == 0); }
    /* validity rules */
    FanCurve v = { 2, { { 50, 40 }, { 60, 30 } }, 0 };
    CHECK(fan_curve_valid(&v) != 0);                         /* % going down */
    v = (FanCurve){ 2, { { 60, 40 }, { 60, 50 } }, 0 };
    CHECK(fan_curve_valid(&v) != 0);                         /* same temperature */
    v = (FanCurve){ 1, { { 29, 40 } }, 0 };
    CHECK(fan_curve_valid(&v) != 0);
    v = (FanCurve){ 1, { { 91, 40 } }, 0 };
    CHECK(fan_curve_valid(&v) != 0);
    v = (FanCurve){ 1, { { 90, 101 } }, 0 };
    CHECK(fan_curve_valid(&v) != 0);
    v = (FanCurve){ 0, { { 0, 0 } }, 0 };
    CHECK(fan_curve_valid(&v) != 0);
    v = (FanCurve){ 9, { { 0, 0 } }, 0 };
    CHECK(fan_curve_valid(&v) != 0);
    v = (FanCurve){ 2, { { 30, 0 }, { 90, 100 } }, 0 };
    CHECK(fan_curve_valid(&v) == 0);
}

/* ---- fan.json ---- */

static int parse(const char *s, FanConfig *c) {
    fan_config_default(c, 1);
    char err[96];
    return fan_config_parse(s, strlen(s), c, err, sizeof err);
}

static void test_config(void) {
    FanConfig c;
    CHECK(parse("{}", &c) == 0 && c.profile == FP_BALANCED && c.hyst == 3);
    CHECK(parse("{\"version\":1,\"profile\":\"quiet\",\"hyst\":5}", &c) == 0 && c.profile == FP_QUIET && c.hyst == 5);
    CHECK(parse("{\"profile\":\"custom\",\"custom\":[[40,20],[50,50],[70,100]]}", &c) == 0 && c.profile == FP_CUSTOM && c.custom.n == 3 && c.custom.p[1].pct == 50);
    CHECK(parse("{\"force\":\"timed\",\"forceUntil\":1900000000}", &c) == 0 && c.force == FF_TIMED && c.force_until == 1900000000LL);
    CHECK(parse("{\"force\":\"on\",\"forceUntil\":5}", &c) == 0 && c.force == FF_ON && c.force_until == 0);
    /* round trip */
    FanConfig a;
    fan_config_default(&a, 0);
    a.profile = FP_CUSTOM; a.hyst = 4; a.force = FF_TIMED; a.force_until = 1800000000LL;
    a.custom = (FanCurve){ 4, { { 35, 0 }, { 45, 25 }, { 60, 60 }, { 85, 100 } }, 0 };
    char buf[1024];
    size_t n = fan_config_write(&a, buf, sizeof buf);
    CHECK(n > 0);
    FanConfig b;
    fan_config_default(&b, 0);
    CHECK(fan_config_parse(buf, n, &b, NULL, 0) == 0);
    CHECK(b.profile == a.profile && b.hyst == a.hyst && b.force == a.force && b.force_until == a.force_until);
    CHECK(b.custom.n == 4 && b.custom.p[3].temp == 85 && b.custom.p[2].pct == 60);
    /* broken files: each must fail and leave the defaults */
    static const char *const bad[] = {
        "", " ", "null", "[]", "1", "\"x\"", "{", "{}}", "{\"profile\":\"turbo\"}", "{\"profile\":1}",
        "{\"profile\":\"quiet\",\"profile\":\"quiet\"}", "{\"hyst\":0}", "{\"hyst\":11}", "{\"hyst\":2.5}", "{\"hyst\":\"3\"}",
        "{\"version\":2}", "{\"extra\":1}", "{\"custom\":[]}", "{\"custom\":[[50]]}", "{\"custom\":[[50,30,1]]}",
        "{\"custom\":[[50,30],[45,40]]}", "{\"custom\":[[50,30],[60,20]]}", "{\"custom\":[[20,30]]}", "{\"custom\":[[95,30]]}",
        "{\"custom\":[[50,-1]]}", "{\"custom\":[[50,101]]}", "{\"custom\":[[50.5,30]]}", "{\"custom\":[[\"50\",30]]}",
        "{\"custom\":[[30,1],[31,2],[32,3],[33,4],[34,5],[35,6],[36,7],[37,8],[38,9]]}", "{\"custom\":{}}",
        "{\"force\":\"maybe\"}", "{\"force\":\"timed\"}", "{\"force\":\"timed\",\"forceUntil\":-1}", "{\"forceUntil\":1e30}",
        "{\"forceUntil\":1.5}", "{\"profile\":\"quiet\"} x", "{\"profile\":\"qu\\u0000iet\"}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int r = parse(bad[i], &c);
        if (r == 0) fprintf(stderr, "accepted: %s\n", bad[i]);
        CHECK(r != 0 && c.profile == FP_BALANCED && c.custom.n == 3);
    }
    /* random bytes and truncations of a valid file never crash and never half-apply */
    unsigned seed = 12345;
    for (int i = 0; i < 4000; i++) {
        char s[160];
        size_t l = strlen(buf);
        memcpy(s, buf, l < sizeof s ? l : sizeof s - 1);
        l = l < sizeof s ? l : sizeof s - 1;
        seed = seed * 1103515245u + 12345u;
        size_t cut = (seed >> 8) % (l + 1);
        seed = seed * 1103515245u + 12345u;
        if (cut < l) s[(seed >> 8) % (cut + 1 > l ? l : cut + 1)] = (char)(seed >> 20);
        fan_config_default(&c, 1);
        if (fan_config_parse(s, cut, &c, NULL, 0) == 0) {
            FanCurve cv;
            fan_profile_curve(&c, &cv);
            CHECK(c.hyst >= FAN_HYST_MIN && c.hyst <= FAN_HYST_MAX && fan_curve_valid(&c.custom) == 0);
        } else CHECK(c.profile == FP_BALANCED);
    }
}

/* ---- requests ---- */

static int req(const char *s, FanReq *r) { return fan_req_parse(s, strlen(s), r); }

static void test_requests(void) {
    FanReq r;
    CHECK(req("STATUS", &r) == 0 && r.type == FR_STATUS);
    CHECK(req("PROFILE quiet", &r) == 0 && r.type == FR_PROFILE && r.profile == FP_QUIET);
    CHECK(req("PROFILE auto", &r) == 0 && r.profile == FP_AUTO);
    CHECK(req("CURVE 55:30,60:55,65:100", &r) == 0 && r.type == FR_CURVE && r.curve.n == 3 && r.curve.p[2].pct == 100);
    CHECK(req("FORCE on", &r) == 0 && r.force == FF_ON);
    CHECK(req("FORCE off", &r) == 0 && r.force == FF_OFF);
    CHECK(req("FORCE 30", &r) == 0 && r.force == FF_TIMED && r.minutes == 30);
    CHECK(req("FORCE 1440", &r) == 0);
    static const char *const bad[] = {
        "", "status", "STATUS ", " STATUS", "PROFILE", "PROFILE ", "PROFILE turbo", "PROFILE quiet x", "CURVE", "CURVE ",
        "CURVE 55:30,", "CURVE 55:30,,60:50", "CURVE 55-30", "CURVE 55:30,50:40", "CURVE 55:30,60:20", "CURVE 10:30",
        "CURVE 55:300", "CURVE 00055:30", "CURVE 55:30\n", "FORCE", "FORCE 0", "FORCE 1441", "FORCE -1", "FORCE 30m",
        "FORCE on ", "SET /sys/class/hwmon/hwmon0/pwm1 255", "PROFILE quiet\tx",
        "CURVE 30:1,31:2,32:3,33:4,34:5,35:6,36:7,37:8,38:9",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int rr = req(bad[i], &r);
        if (rr == 0) fprintf(stderr, "accepted request: %s\n", bad[i]);
        CHECK(rr != 0);
    }
    char longline[FAN_REQ_MAX + 8];
    memset(longline, 'A', sizeof longline - 1);
    longline[sizeof longline - 1] = 0;
    CHECK(fan_req_parse(longline, strlen(longline), &r) != 0);
    CHECK(fan_req_parse("STATUS\0x", 8, &r) != 0);
}

/* ---- fake sysfs ---- */

static void put(const char *root, const char *rel, const char *val) {
    char p[512], d[512];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    /* mkdir -p of the parent */
    snprintf(d, sizeof d, "%s", p);
    for (char *s = d + strlen(root) + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(d, 0755); *s = '/'; }
    FILE *f = fopen(p, "w");
    if (f) { fputs(val, f); fputc('\n', f); fclose(f); }
}

static void link_rel(const char *root, const char *rel, const char *target) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    unlink(p);
    if (symlink(target, p)) perror(p);
}

static int get(const char *root, const char *rel) {
    char p[512], b[32] = "";
    snprintf(p, sizeof p, "%s/%s", root, rel);
    FILE *f = fopen(p, "r");
    if (!f) return -99999;
    if (!fgets(b, sizeof b, f)) b[0] = 0;
    fclose(f);
    return atoi(b);
}

/* A Raspberry Pi 5 with the fan header: cpu-thermal zone, pwm-fan cooling device bound to four
 * active trips, hwmon pwmfan with tachometer; plus an unrelated hwmon. */
static void make_pi5(const char *r, int temp_mc) {
    char t[16];
    put(r, "proc/device-tree/model", "Raspberry Pi 5 Model B Rev 1.1");
    snprintf(t, sizeof t, "%d", temp_mc);
    put(r, "sys/class/thermal/thermal_zone0/type", "cpu-thermal");
    put(r, "sys/class/thermal/thermal_zone0/temp", t);
    put(r, "sys/class/thermal/thermal_zone0/trip_point_0_type", "critical");
    put(r, "sys/class/thermal/thermal_zone0/trip_point_0_temp", "110000");
    static const int trips[] = { 50000, 60000, 67500, 75000 };
    for (int i = 0; i < 4; i++) {
        char rel[96], v[16];
        snprintf(rel, sizeof rel, "sys/class/thermal/thermal_zone0/trip_point_%d_type", i + 1);
        put(r, rel, "active");
        snprintf(rel, sizeof rel, "sys/class/thermal/thermal_zone0/trip_point_%d_temp", i + 1);
        snprintf(v, sizeof v, "%d", trips[i]);
        put(r, rel, v);
        snprintf(rel, sizeof rel, "sys/class/thermal/thermal_zone0/cdev%d_trip_point", i + 1);
        snprintf(v, sizeof v, "%d", i + 1);
        put(r, rel, v);
        snprintf(rel, sizeof rel, "sys/class/thermal/thermal_zone0/cdev%d", i + 1);
        link_rel(r, rel, "../cooling_device0");
    }
    put(r, "sys/class/thermal/cooling_device0/type", "pwm-fan");
    put(r, "sys/class/thermal/cooling_device0/max_state", "4");
    put(r, "sys/class/thermal/cooling_device0/cur_state", "0");
    put(r, "sys/class/hwmon/hwmon0/name", "cpu_thermal");
    put(r, "sys/class/hwmon/hwmon0/temp1_input", t);
    put(r, "sys/class/hwmon/hwmon2/name", "pwmfan");
    put(r, "sys/class/hwmon/hwmon2/pwm1", "0");
    put(r, "sys/class/hwmon/hwmon2/pwm1_enable", "1");
    put(r, "sys/class/hwmon/hwmon2/fan1_input", "0");
}

/* A desktop PC: k10temp, a Super I/O with two PWM headers (one with tachometer), and a GPU whose
 * fan must never be touched. */
static void make_pc(const char *r) {
    put(r, "sys/class/hwmon/hwmon0/name", "k10temp");
    put(r, "sys/class/hwmon/hwmon0/temp1_input", "48250");
    put(r, "sys/class/hwmon/hwmon1/name", "nct6798");
    put(r, "sys/class/hwmon/hwmon1/pwm1", "100");
    put(r, "sys/class/hwmon/hwmon1/pwm1_enable", "5");
    put(r, "sys/class/hwmon/hwmon1/fan1_input", "820");
    put(r, "sys/class/hwmon/hwmon1/pwm2", "80");
    put(r, "sys/class/hwmon/hwmon1/pwm2_enable", "2");
    put(r, "sys/class/hwmon/hwmon3/name", "amdgpu");
    put(r, "sys/class/hwmon/hwmon3/pwm1", "60");
    put(r, "sys/class/hwmon/hwmon3/pwm1_enable", "2");
    put(r, "sys/class/hwmon/hwmon3/fan1_input", "1100");
}

static void make_none(const char *r) {
    put(r, "sys/class/thermal/thermal_zone0/type", "acpitz");
    put(r, "sys/class/thermal/thermal_zone0/temp", "41000");
}

static void test_sysfs(void) {
    char tmpl[] = "/tmp/fan-test.XXXXXX";
    char *r = mkdtemp(tmpl);
    CHECK(r != NULL);
    if (!r) return;
    char root[64], state[96];

    /* Raspberry Pi 5 */
    snprintf(root, sizeof root, "%s/pi5", r);
    mkdir(root, 0755);
    make_pi5(root, 63000);
    snprintf(state, sizeof state, "%s/fand.restore", r);
    FanHw h;
    CHECK(fanhw_scan(&h, root) == 1);
    CHECK(h.board == 1 && h.t[0].kind == FK_BOARD && h.t[0].has_rpm && !strcmp(h.temp_src, "cpu-thermal"));
    CHECK(fanhw_temp(&h) == 63000);
    CHECK(h.ntrips == 4 && h.trip[0] == 1 && h.trip_orig[0] == 50000 && h.trip_orig[3] == 75000);
    CHECK(h.nro == 0);
    CHECK(fanhw_take(&h, state) == 1);
    CHECK(access(state, F_OK) == 0);
    CHECK(get(root, "sys/class/thermal/thermal_zone0/trip_point_1_temp") == 78000);
    CHECK(get(root, "sys/class/thermal/thermal_zone0/trip_point_4_temp") == 84000);
    CHECK(get(root, "sys/class/thermal/thermal_zone0/trip_point_0_temp") == 110000);   /* critical untouched */
    /* starting a stopped fan at 30 %: kick at full speed first, then 30 % */
    CHECK(fanhw_set(&h, 0, 30) == 0 && get(root, "sys/class/hwmon/hwmon2/pwm1") == 255);
    CHECK(fanhw_set(&h, 0, 30) == 0 && get(root, "sys/class/hwmon/hwmon2/pwm1") == 77);
    CHECK(fanhw_pct(&h, 0) == 30);
    CHECK(fanhw_set(&h, 0, 100) == 0 && get(root, "sys/class/hwmon/hwmon2/pwm1") == 255);
    CHECK(fanhw_set(&h, 0, 0) == 0 && get(root, "sys/class/hwmon/hwmon2/pwm1") == 0);
    CHECK(fanhw_set(&h, 0, 60) == 0 && get(root, "sys/class/hwmon/hwmon2/pwm1") == 153);  /* ≥ 50 %: no kick */
    put(root, "sys/class/hwmon/hwmon2/pwm1", "125");               /* the kernel stepped in */
    CHECK(fanhw_set(&h, 0, 60) == 0 && get(root, "sys/class/hwmon/hwmon2/pwm1") == 153);
    put(root, "sys/class/hwmon/hwmon2/fan1_input", "4120");
    CHECK(fanhw_rpm(&h, 0) == 4120);
    CHECK(fanhw_set(&h, 5, 50) == -1);
    fanhw_release(&h);
    CHECK(get(root, "sys/class/thermal/thermal_zone0/trip_point_1_temp") == 50000);
    CHECK(get(root, "sys/class/thermal/thermal_zone0/trip_point_3_temp") == 67500);
    CHECK(get(root, "sys/class/thermal/cooling_device0/cur_state") == 2);   /* 63 °C: trips 50 and 60 */
    CHECK(fanhw_set(&h, 0, 50) == -1);                                     /* not ours any more */
    /* crash: take, then restore from the notes with a fresh process */
    FanHw h2;
    fanhw_scan(&h2, root);
    fanhw_take(&h2, state);
    CHECK(get(root, "sys/class/thermal/thermal_zone0/trip_point_2_temp") == 80000);
    CHECK(fanhw_restore_file(root, state) == 0);
    CHECK(get(root, "sys/class/thermal/thermal_zone0/trip_point_2_temp") == 60000);
    CHECK(access(state, F_OK) != 0);

    /* PC */
    snprintf(root, sizeof root, "%s/pc", r);
    mkdir(root, 0755);
    make_pc(root);
    CHECK(fanhw_scan(&h, root) == 2);
    CHECK(h.board == 0 && !strcmp(h.temp_src, "k10temp") && fanhw_temp(&h) == 48250);
    CHECK(h.t[0].kind == FK_HWMON && h.t[0].ch == 1 && h.t[0].has_rpm && h.t[1].ch == 2 && !h.t[1].has_rpm);
    CHECK(h.nro == 1 && !strcmp(h.ro[0].chip, "amdgpu") && fanhw_ro_rpm(&h, 0) == 1100);
    CHECK(fanhw_take(&h, state) == 2);
    CHECK(get(root, "sys/class/hwmon/hwmon1/pwm1_enable") == 1 && get(root, "sys/class/hwmon/hwmon1/pwm2_enable") == 1);
    CHECK(get(root, "sys/class/hwmon/hwmon3/pwm1_enable") == 2);   /* GPU untouched */
    fanhw_set(&h, 0, 70);
    CHECK(get(root, "sys/class/hwmon/hwmon1/pwm1") == 179);
    fanhw_release(&h);
    CHECK(get(root, "sys/class/hwmon/hwmon1/pwm1_enable") == 5 && get(root, "sys/class/hwmon/hwmon1/pwm2_enable") == 2);
    /* crash restore with notes; a tampered line is ignored */
    fanhw_scan(&h, root);
    fanhw_take(&h, state);
    FILE *f = fopen(state, "a");
    if (f) { fputs("enable 1 9 /etc\nexec /bin/sh\n", f); fclose(f); }
    fanhw_restore_file(root, state);
    CHECK(get(root, "sys/class/hwmon/hwmon1/pwm1_enable") == 5 && get(root, "sys/class/hwmon/hwmon1/pwm2_enable") == 2);

    /* nothing to drive */
    snprintf(root, sizeof root, "%s/none", r);
    mkdir(root, 0755);
    make_none(root);
    CHECK(fanhw_scan(&h, root) == 0 && h.nro == 0 && fanhw_temp(&h) == 41000 && !strcmp(h.temp_src, "thermal_zone0"));

    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", r);
    if (system(cmd)) fprintf(stderr, "cleanup failed\n");
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "mkfake")) {
        mkdir(argv[2], 0755);
        if (!strcmp(argv[3], "pi5")) make_pi5(argv[2], 58000);
        else if (!strcmp(argv[3], "pc")) make_pc(argv[2]);
        else make_none(argv[2]);
        return 0;
    }
    test_curve();
    test_config();
    test_requests();
    test_sysfs();
    printf("fan-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
