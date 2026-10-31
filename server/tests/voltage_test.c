/* voltage_test.c — Monitor → Voltage's sysfs reader (src/monitor/voltage.c) on fake trees: a desktop board with
 * labelled and raw inputs, a laptop, the layout measured on a Raspberry Pi 5, a virtual machine with nothing, and
 * hostile values. */
#include "../src/monitor/voltage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static char base[256];
static int tree_n;

static void mkparents(char *p) {
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
}
static void put(const char *root, const char *path, const char *text) {
    char p[768];
    snprintf(p, sizeof p, "%s/%s", root, path);
    mkparents(p);
    FILE *f = fopen(p, "w");
    if (!f) { perror(p); exit(1); }
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}
static const char *tree(void) {
    static char r[300];
    snprintf(r, sizeof r, "%s/t%d", base, tree_n++);
    mkdir(r, 0755);
    return r;
}

static void test_parse(void) {
    long v;
    CHECK(!volt_parse_int("1234\n", &v) && v == 1234);
    CHECK(!volt_parse_int("0", &v) && v == 0);
    CHECK(!volt_parse_int("-12000\n", &v) && v == -12000);
    const char *bad[] = { "", "\n", "-", "12a", " 12", "12 ", "12\n3", "1.2", "1234567890", "0x10", NULL };
    for (int i = 0; bad[i]; i++) CHECK(volt_parse_int(bad[i], &v));
}

static void test_nothing(void) {
    VoltState v;
    const char *r = tree();
    volt_read(r, &v);
    CHECK(!volt_any(&v) && !v.board && v.alarm == -1 && v.since_boot == -1 && !v.nchips && !v.nsup);
    char *j = volt_json(&v);
    CHECK(j && !strcmp(j, "{\"board\":false,\"alarm\":-1,\"sinceBoot\":-1,\"chips\":[],\"supplies\":[]}"));
    free(j);
    /* temperature-only chips, as on a chip with no voltage driver: nothing */
    r = tree();
    put(r, "sys/class/hwmon/hwmon0/name", "k10temp\n");
    put(r, "sys/class/hwmon/hwmon0/temp1_input", "45000\n");
    volt_read(r, &v);
    CHECK(!volt_any(&v));
}

static void test_desktop(void) {
    VoltState v;
    const char *r = tree();
    put(r, "sys/class/hwmon/hwmon2/name", "nct6798\n");
    put(r, "sys/class/hwmon/hwmon2/in0_input", "1032\n");
    put(r, "sys/class/hwmon/hwmon2/in0_label", "Vcore\n");
    put(r, "sys/class/hwmon/hwmon2/in1_input", "1016\n");           /* raw: no label */
    put(r, "sys/class/hwmon/hwmon2/in2_input", "0\n");              /* raw zero: nothing there */
    put(r, "sys/class/hwmon/hwmon2/in3_input", "0\n");              /* labelled zero: the sensor says so */
    put(r, "sys/class/hwmon/hwmon2/in3_label", "+5V\n");
    put(r, "sys/class/hwmon/hwmon2/in10_input", "3344\n");
    put(r, "sys/class/hwmon/hwmon2/in10_label", "bad\"label\n");    /* bad label: no label, the value stays raw */
    put(r, "sys/class/hwmon/hwmon2/in4_input", "garbage\n");
    put(r, "sys/class/hwmon/hwmon2/in5_input", "999999999\n");      /* out of range */
    put(r, "sys/class/hwmon/hwmon2/in1_min", "900\n");
    put(r, "sys/class/hwmon/hwmon10/name", "amdgpu\n");
    put(r, "sys/class/hwmon/hwmon10/in0_input", "850\n");
    put(r, "sys/class/hwmon/hwmon10/in0_label", "vddgfx\n");
    put(r, "sys/class/hwmon/hwmon3/name", "bad name\n");            /* refused */
    put(r, "sys/class/hwmon/hwmon3/in0_input", "850\n");
    put(r, "sys/class/hwmon/notachip/name", "x\n");
    volt_read(r, &v);
    CHECK(volt_any(&v) && !v.board && v.alarm == -1 && v.nchips == 2);
    CHECK(!strcmp(v.chips[0].dev, "hwmon2") && !strcmp(v.chips[0].name, "nct6798") && !strcmp(v.chips[1].name, "amdgpu"));
    CHECK(v.chips[0].n == 4);
    CHECK(v.chips[0].in[0].index == 0 && v.chips[0].in[0].mv == 1032 && !strcmp(v.chips[0].in[0].label, "Vcore"));
    CHECK(v.chips[0].in[1].index == 1 && !v.chips[0].in[1].label[0]);
    CHECK(v.chips[0].in[2].index == 3 && !strcmp(v.chips[0].in[2].label, "+5V") && v.chips[0].in[2].mv == 0);
    CHECK(v.chips[0].in[3].index == 10 && !v.chips[0].in[3].label[0]);
    char *j = volt_json(&v);
    CHECK(j && strstr(j, "{\"i\":0,\"v\":1.032,\"label\":\"Vcore\"}") && !strstr(j, "\\"));
    free(j);
}

static void test_laptop(void) {
    VoltState v;
    const char *r = tree();
    put(r, "sys/class/power_supply/BAT0/type", "Battery\n");
    put(r, "sys/class/power_supply/BAT0/present", "1\n");
    put(r, "sys/class/power_supply/BAT0/voltage_now", "11824000\n");
    put(r, "sys/class/power_supply/BAT0/voltage_min_design", "11400000\n");
    put(r, "sys/class/power_supply/BAT1/type", "Battery\n");
    put(r, "sys/class/power_supply/BAT1/present", "0\n");           /* the bay is empty */
    put(r, "sys/class/power_supply/BAT1/voltage_now", "1000\n");
    put(r, "sys/class/power_supply/AC/type", "Mains\n");            /* online, no voltage: nothing to show */
    put(r, "sys/class/power_supply/AC/online", "1\n");
    put(r, "sys/class/power_supply/ucsi-source-psy-USBC000:001/type", "USB\n");
    put(r, "sys/class/power_supply/ucsi-source-psy-USBC000:001/online", "1\n");
    put(r, "sys/class/power_supply/ucsi-source-psy-USBC000:001/voltage_now", "20000000\n");
    put(r, "sys/class/power_supply/ucsi-source-psy-USBC000:002/type", "USB\n");
    put(r, "sys/class/power_supply/ucsi-source-psy-USBC000:002/online", "0\n");   /* unplugged */
    put(r, "sys/class/power_supply/ucsi-source-psy-USBC000:002/voltage_now", "5000000\n");
    put(r, "sys/class/power_supply/hidpp_battery_0/type", "Battery\n");           /* a mouse */
    put(r, "sys/class/power_supply/hidpp_battery_0/scope", "Device\n");
    put(r, "sys/class/power_supply/hidpp_battery_0/voltage_now", "3900000\n");
    /* hwmon mirrors of the same batteries are not shown twice */
    put(r, "sys/class/hwmon/hwmon1/name", "BAT0\n");
    put(r, "sys/class/hwmon/hwmon1/in0_input", "11824\n");
    put(r, "sys/class/hwmon/hwmon2/name", "ucsi_source_psy_USBC000:001\n");
    put(r, "sys/class/hwmon/hwmon2/in0_input", "20000\n");
    volt_read(r, &v);
    CHECK(volt_any(&v) && v.nsup == 2 && !v.nchips);
    CHECK(!strcmp(v.sup[0].name, "BAT0") && v.sup[0].battery && v.sup[0].mv == 11824 && v.sup[0].min_mv == 11400);
    CHECK(!strcmp(v.sup[1].name, "ucsi-source-psy-USBC000:001") && !v.sup[1].battery && v.sup[1].mv == 20000 && v.sup[1].min_mv == -1);
    /* a bad reading is left out */
    r = tree();
    put(r, "sys/class/power_supply/BAT0/type", "Battery\n");
    put(r, "sys/class/power_supply/BAT0/voltage_now", "-5\n");
    volt_read(r, &v);
    CHECK(!v.nsup && !volt_any(&v));
}

/* the layout of the real board (172.17.0.24, kernel 6.18): rp1_adc's four raw inputs and rpi_volt's alarm */
static void test_pi5(void) {
    VoltState v;
    const char *r = tree();
    static const char compat[] = "raspberrypi,5-model-b\0brcm,bcm2712\0";
    char p[512];
    snprintf(p, sizeof p, "%s/proc/device-tree/compatible", r);
    mkparents(p);
    FILE *f = fopen(p, "w");
    fwrite(compat, 1, sizeof compat - 1, f);
    fclose(f);
    put(r, "usr/bin/vcgencmd", "");
    snprintf(p, sizeof p, "%s/usr/bin/vcgencmd", r);
    chmod(p, 0755);
    put(r, "sys/class/hwmon/hwmon0/name", "cpu_thermal\n");
    put(r, "sys/class/hwmon/hwmon0/temp1_input", "45000\n");
    put(r, "sys/class/hwmon/hwmon1/name", "rp1_adc\n");
    put(r, "sys/class/hwmon/hwmon1/in1_input", "2017\n");
    put(r, "sys/class/hwmon/hwmon1/in2_input", "2565\n");
    put(r, "sys/class/hwmon/hwmon1/in3_input", "1454\n");
    put(r, "sys/class/hwmon/hwmon1/in4_input", "1451\n");
    put(r, "sys/class/hwmon/hwmon3/name", "rpi_volt\n");
    put(r, "sys/class/hwmon/hwmon3/in0_lcrit_alarm", "0\n");
    volt_read(r, &v);
    CHECK(v.board && v.alarm == 0 && v.since_boot == -1 && v.nchips == 1 && v.chips[0].n == 4 && v.chips[0].in[3].index == 4);
    put(r, "sys/class/hwmon/hwmon3/in0_lcrit_alarm", "1\n");
    volt_read(r, &v);
    CHECK(v.alarm == 1);
    /* with vcgencmd missing the board can't answer, and the alarm alone is still something */
    snprintf(p, sizeof p, "%s/usr/bin/vcgencmd", r);
    unlink(p);
    volt_read(r, &v);
    CHECK(!v.board && volt_any(&v));
}

/* an older kernel's get_throttled, without rpi_volt: its sticky bit does mean "since boot" */
static void test_throttled(void) {
    VoltState v;
    const char *r = tree();
    put(r, "sys/devices/platform/soc/soc:firmware/get_throttled", "50000\n");
    volt_read(r, &v);
    CHECK(v.alarm == 0 && v.since_boot == 1 && volt_any(&v));
    put(r, "sys/class/hwmon/hwmon3/name", "rpi_volt\n");
    put(r, "sys/class/hwmon/hwmon3/in0_lcrit_alarm", "1\n");
    volt_read(r, &v);
    CHECK(v.alarm == 1 && v.since_boot == -1);                      /* rpi_volt clears it every 2 s */
}

static void test_many(void) {
    VoltState v;
    const char *r = tree();
    for (int c = 0; c < 20; c++) {
        char a[96], b[96];
        snprintf(a, sizeof a, "sys/class/hwmon/hwmon%d/name", c);
        put(r, a, "chip\n");
        for (int i = 0; i < 40; i++) {
            snprintf(b, sizeof b, "sys/class/hwmon/hwmon%d/in%d_input", c, i);
            put(r, b, "-100000\n");
            snprintf(b, sizeof b, "sys/class/hwmon/hwmon%d/in%d_label", c, i);
            put(r, b, "A label thirty one chars long..\n");
        }
    }
    volt_read(r, &v);
    CHECK(v.nchips == VOLT_CHIPS && v.chips[0].n == VOLT_INPUTS);
    char *j = volt_json(&v);
    CHECK(j && strlen(j) > 100 && !strcmp(j + strlen(j) - 2, "]}"));
    free(j);
}

/* `build/voltage-test dump [root]`: the JSON of a tree, or of this machine */
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "dump")) {
        VoltState v;
        volt_read(argc > 2 ? argv[2] : "", &v);
        char *j = volt_json(&v);
        printf("any=%d %s\n", volt_any(&v), j ? j : "");
        free(j);
        return 0;
    }
    snprintf(base, sizeof base, "/tmp/voltage-test-%d", (int)getpid());
    mkdir(base, 0755);
    test_parse();
    test_nothing();
    test_desktop();
    test_laptop();
    test_pi5();
    test_throttled();
    test_many();
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf %s", base);
    if (system(cmd)) fails++;
    printf("voltage-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
