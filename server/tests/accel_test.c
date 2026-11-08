/* accel_test.c — automatic rotation's pure parts (src/hw/accel.c, issue #60): the mount matrix
 * parser, the orientation from one reading (with hysteresis), the debounce, the chassis type, and
 * finding the screen's accelerometer in a fake sysfs and udev tree (scale, offset, the hwdb's mount
 * matrix over the driver's, a sensor in the keyboard base skipped).
 *
 *   build/accel-test           all tests
 *   build/accel-test --live    reads this machine's accelerometer for 10 s and prints what it sees */
#include "../src/hw/accel.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("  FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define G 9.80665

static void t_matrix(void) {
    double m[9];
    CHECK(!accel_parse_matrix("1, 0, 0; 0, -1, 0; 0, 0, 1", m) && m[0] == 1 && m[4] == -1 && m[8] == 1, "hwdb form");
    CHECK(!accel_parse_matrix("0, -1, 0; -1, 0, 0; 0, 0, 1\n", m) && m[1] == -1 && m[3] == -1, "sysfs form with a newline");
    CHECK(!accel_parse_matrix("0.5,0,0;0,0.5,0;0,0,1", m) && m[0] == 0.5, "no spaces, fractions");
    CHECK(accel_parse_matrix("1, 0, 0; 0, 1, 0", m), "two rows");
    CHECK(accel_parse_matrix("1, 0, 0; 0, 1, 0; 0, 0, 1; 1", m), "trailing junk");
    CHECK(accel_parse_matrix("1; 0, 0, 0; 1, 0, 0; 0, 1", m), "separators in the wrong places");
    CHECK(accel_parse_matrix("0, 0, 0; 0, 0, 0; 0, 0, 0", m), "all zeros");
    CHECK(accel_parse_matrix("", m) && accel_parse_matrix(NULL, m), "empty");
    CHECK(accel_parse_matrix("nan, 0, 0; 0, 1, 0; 0, 0, 1", m), "not finite");
}

/* a reading for the device tilted so that `deg` from upright, around the screen's normal (clockwise
 * as the user sees the device turn), and `flat` toward lying on its back */
static void pose(double deg, double flat, double v[3]) {
    double a = deg * M_PI / 180, f = flat * M_PI / 180;
    /* upright: gravity along -y; turning the device clockwise moves gravity toward +x in its frame */
    v[0] = G * cos(f) * sin(a);
    v[1] = -G * cos(f) * cos(a);
    v[2] = -G * sin(f);
}

static void t_orientation(void) {
    double v[3];
    pose(0, 0, v);
    CHECK(accel_orientation(-1, v[0], v[1], v[2]) == 0, "upright: 0");
    pose(90, 0, v);
    CHECK(accel_orientation(-1, v[0], v[1], v[2]) == 3, "turned clockwise (left edge up): the picture turns 270°");
    pose(-90, 0, v);
    CHECK(accel_orientation(-1, v[0], v[1], v[2]) == 1, "turned counterclockwise (right edge up): 90°");
    pose(180, 0, v);
    CHECK(accel_orientation(-1, v[0], v[1], v[2]) == 2, "upside down: 180°");
    pose(0, 85, v);
    CHECK(accel_orientation(-1, v[0], v[1], v[2]) == -1, "flat on a table, nothing yet: none");
    CHECK(accel_orientation(2, v[0], v[1], v[2]) == 2, "flat on a table: keeps what it had");
    pose(90, 60, v);
    CHECK(accel_orientation(0, v[0], v[1], v[2]) == 0, "leaning far back while turned: not sure, keeps 0");
    pose(90, 40, v);
    CHECK(accel_orientation(0, v[0], v[1], v[2]) == 3, "on a lap, turned: 270°");
    pose(45, 0, v);
    CHECK(accel_orientation(0, v[0], v[1], v[2]) == 0, "on the corner: keeps 0");
    CHECK(accel_orientation(3, v[0], v[1], v[2]) == 3, "on the corner: keeps 270°");
    /* hysteresis: going back from 270° needs the bottom edge clearly down again */
    pose(60, 0, v);
    CHECK(accel_orientation(3, v[0], v[1], v[2]) == 3, "60°: still 270°");
    pose(25, 0, v);
    CHECK(accel_orientation(3, v[0], v[1], v[2]) == 3, "25° (its tilt still over the keep threshold): still 270°");
    pose(10, 0, v);
    CHECK(accel_orientation(3, v[0], v[1], v[2]) == 0, "10°: back to 0");
    CHECK(accel_orientation(1, 0, 0, 0) == 1, "no reading: keeps it");
}

static void t_filter(void) {
    AccelFilter f;
    accel_filter_init(&f);
    double v[3];
    pose(0, 85, v);
    CHECK(!accel_filter_feed(&f, 0, v[0], v[1], v[2]) && f.cur == -1, "flat at start: nothing");
    pose(0, 0, v);
    CHECK(accel_filter_feed(&f, 400, v[0], v[1], v[2]) && f.cur == 0, "the first orientation counts at once");
    pose(90, 0, v);
    double t = 800;
    CHECK(!accel_filter_feed(&f, t, v[0], v[1], v[2]) && f.cur == 0, "turned: not yet");
    CHECK(!accel_filter_feed(&f, t + 400, v[0], v[1], v[2]) && f.cur == 0, "0.4 s later: not yet");
    CHECK(accel_filter_feed(&f, t + ACCEL_SETTLE_MS, v[0], v[1], v[2]) && f.cur == 3, "held: 270°");
    /* a quick wobble does not turn it */
    pose(-90, 0, v);
    t = 5000;
    accel_filter_feed(&f, t, v[0], v[1], v[2]);
    pose(90, 0, v);
    accel_filter_feed(&f, t + 400, v[0], v[1], v[2]);
    pose(-90, 0, v);
    CHECK(!accel_filter_feed(&f, t + 800, v[0], v[1], v[2]) && f.cur == 3, "a wobble restarts the wait");
    CHECK(!accel_filter_feed(&f, t + 800 + ACCEL_SETTLE_MS - 1, v[0], v[1], v[2]), "still waiting");
    CHECK(accel_filter_feed(&f, t + 800 + ACCEL_SETTLE_MS, v[0], v[1], v[2]) && f.cur == 1, "then 90°");
    /* shaking (far from 1 g) is not gravity */
    pose(180, 0, v);
    for (int i = 0; i < 3; i++) v[i] *= 2.2;
    t = 20000;
    accel_filter_feed(&f, t, v[0], v[1], v[2]);
    CHECK(!accel_filter_feed(&f, t + 5000, v[0], v[1], v[2]) && f.cur == 1, "2.2 g: ignored");
}

/* ---- a fake sysfs and udev tree ---- */

static char root[256];

static void put(const char *rel, const char *text) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    for (char *s = p + strlen(root) + 1; (s = strchr(s, '/')); s++) { *s = 0; mkdir(p, 0700); *s = '/'; }
    FILE *f = fopen(p, "w");
    if (!f) { perror(p); exit(2); }
    fputs(text, f);
    fclose(f);
}

static void t_find(void) {
    snprintf(root, sizeof root, "/tmp/accel-test-%d", (int)getpid());
    mkdir(root, 0700);
    char iio[300], udev[300];
    snprintf(iio, sizeof iio, "%s/iio", root);
    snprintf(udev, sizeof udev, "%s/udev", root);
    AccelDev a;
    CHECK(accel_find(iio, udev, &a) < 0, "no iio folder: none");
    /* a light sensor (no accel files) and the keyboard base's accelerometer are skipped */
    put("iio/iio:device0/name", "als\n");
    put("iio/iio:device0/in_illuminance_raw", "12\n");
    put("iio/iio:device1/name", "accel-base-sensor\n");
    put("iio/iio:device1/label", "accel-base\n");
    for (int i = 0; i < 3; i++) { char r[64]; snprintf(r, sizeof r, "iio/iio:device1/in_accel_%c_raw", "xyz"[i]); put(r, "0\n"); }
    CHECK(accel_find(iio, udev, &a) < 0, "only a base sensor: none");
    /* the Chuwi Hi12 of issue #60: bmc150 with its scale; the hwdb's matrix flips y */
    put("iio/iio:device3/name", "bmc150_accel\n");
    put("iio/iio:device3/dev", "246:3\n");
    put("iio/iio:device3/in_accel_scale", "0.019163\n");
    put("iio/iio:device3/in_accel_x_raw", "0\n");
    put("iio/iio:device3/in_accel_y_raw", "512\n");
    put("iio/iio:device3/in_accel_z_raw", "0\n");
    put("iio/iio:device3/in_mount_matrix", "0, 1, 0; 1, 0, 0; 0, 0, 1\n");
    put("udev/c246:3", "I:123\nE:ID_PATH=platform-i2c\nE:ACCEL_MOUNT_MATRIX=1, 0, 0; 0, -1, 0; 0, 0, 1\nG:x\n");
    CHECK(accel_find(iio, udev, &a) == 0 && strstr(a.dir, "iio:device3") && a.matrix_from == 1, "found, matrix from the hwdb (%s, %d)", a.dir, a.matrix_from);
    double v[3];
    CHECK(accel_read(&a, v) == 0 && fabs(v[1] + 512 * 0.019163) < 1e-6 && fabs(v[0]) < 1e-9, "read: scale and the hwdb's matrix (%.3f, %.3f, %.3f)", v[0], v[1], v[2]);
    double rv[3] = { v[0], v[1], v[2] };
    CHECK(accel_orientation(-1, rv[0], rv[1], rv[2]) == 0, "y toward the bottom: upright");
    put("iio/iio:device3/in_accel_x_raw", "500\n");
    put("iio/iio:device3/in_accel_y_raw", "-20\n");
    CHECK(accel_read(&a, v) == 0 && accel_orientation(-1, v[0], v[1], v[2]) == 3, "re-read through the open files: left edge up");
    accel_close(&a);
    /* no hwdb entry: the driver's matrix; no scale: normalized to 1 g */
    put("udev/c246:3", "I:123\n");
    put("iio/iio:device3/in_accel_scale", "\n");
    CHECK(accel_find(iio, udev, &a) == 0 && a.matrix_from == 2 && a.scale == 0, "the driver's matrix, no scale");
    CHECK(accel_read(&a, v) == 0 && fabs(sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) - G) < 1e-6,
          "normalized to 1 g, axes swapped (%.2f, %.2f, %.2f)", v[0], v[1], v[2]);
    CHECK(v[1] > 9.7, "x of the sensor is y of the screen");
    accel_close(&a);
    /* the hwdb says this one is in the base */
    put("udev/c246:3", "E:ACCEL_LOCATION=base\n");
    CHECK(accel_find(iio, udev, &a) < 0, "ACCEL_LOCATION=base: skipped");
    /* chassis */
    put("ct-tablet", "30\n");
    put("ct-laptop", "10\n");
    char p[320];
    snprintf(p, sizeof p, "%s/ct-tablet", root);
    CHECK(accel_chassis_tablet(p) == 1, "chassis 30: tablet");
    snprintf(p, sizeof p, "%s/ct-laptop", root);
    CHECK(accel_chassis_tablet(p) == 0, "chassis 10: notebook");
    snprintf(p, sizeof p, "%s/none", root);
    CHECK(accel_chassis_tablet(p) == 0, "no chassis type: not a tablet");
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd)) fprintf(stderr, "could not remove %s\n", root);
}

static int live(void) {
    AccelDev a;
    if (accel_find(NULL, NULL, &a)) { printf("no screen accelerometer under /sys/bus/iio/devices\n"); return 1; }
    printf("%s (%s), scale %g, offset %g, matrix from %s: %g %g %g; %g %g %g; %g %g %g\n", a.name, a.dir, a.scale, a.offset,
           a.matrix_from == 1 ? "the hwdb" : a.matrix_from == 2 ? "the driver" : "nowhere", a.m[0], a.m[1], a.m[2], a.m[3], a.m[4], a.m[5],
           a.m[6], a.m[7], a.m[8]);
    AccelFilter f;
    accel_filter_init(&f);
    for (int i = 0; i < 25; i++) {
        double v[3];
        if (accel_read(&a, v)) { printf("read failed: %s\n", strerror(errno)); break; }
        accel_filter_feed(&f, i * ACCEL_PERIOD_MS, v[0], v[1], v[2]);
        printf("x %+7.2f  y %+7.2f  z %+7.2f m/s²  → rotation %d (%d°)\n", v[0], v[1], v[2], f.cur, f.cur < 0 ? 0 : f.cur * 90);
        fflush(stdout);
        struct timespec ts = { 0, ACCEL_PERIOD_MS * 1000000L };
        nanosleep(&ts, NULL);
    }
    accel_close(&a);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--live")) return live();
    t_matrix();
    t_orientation();
    t_filter();
    t_find();
    printf("accel-test: %d/%d passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
