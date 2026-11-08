/* accel.h — the accelerometer of a tablet or a convertible, for automatic rotation of the built-in
 * screen (Settings → Displays, "Rotate automatically"; docs/DISPLAYS.md, issue #60).
 *
 *   - reads an IIO accelerometer through sysfs only (/sys/bus/iio/devices/iio:deviceN/in_accel_*_raw),
 *     no library: its scale and offset, and the mount matrix (udev's ACCEL_MOUNT_MATRIX from the
 *     systemd hwdb first, then the driver's in_mount_matrix), so the reading is in the frame of the
 *     screen as the user sees it: x to the right, y up, z out of the screen, and the vector points
 *     where gravity pulls (held upright, y ≈ -9.8 m/s²) — the convention of the hwdb and of the
 *     usual desktop sensor services;
 *   - a sensor in the keyboard base of a convertible (ACCEL_LOCATION=base, label "accel-base") is skipped;
 *   - the reads run on a small thread every ACCEL_PERIOD_MS while someone wants them: a sensor behind
 *     a slow bus never stalls the main loop;
 *   - pure parts (tests/accel_test.c): the mount matrix parser, the orientation from one reading with
 *     hysteresis, and the debounce. */
#ifndef LORD_HW_ACCEL_H
#define LORD_HW_ACCEL_H

#define ACCEL_PERIOD_MS 400         /* one reading every 0.4 s while automatic rotation is on */
#define ACCEL_SETTLE_MS 900         /* a new orientation must hold this long before the screen turns */
#define ACCEL_ENTER_DEG 35.0        /* tilt past which an edge counts as "up" */
#define ACCEL_KEEP_DEG  20.0        /* the current orientation holds until its tilt falls under this */

/* Parses "a, b, c; d, e, f; g, h, i" (rows). 0 if it is a valid 3×3 matrix. */
int accel_parse_matrix(const char *s, double m[9]);

/* The rotation (0..3, ×90° clockwise, as in DisplayOut.rot) that keeps the picture upright for a
 * gravity vector in the screen's frame, or prev (−1 = none yet) when the device lies flat or sits
 * between two orientations. */
int accel_orientation(int prev, double x, double y, double z);

/* Debounce: an orientation becomes current once it held for ACCEL_SETTLE_MS (the first one at once). */
typedef struct { int cur, cand; double since; } AccelFilter;
void accel_filter_init(AccelFilter *f);
/* Feeds one reading (m/s²); 1 if f->cur changed. Readings far from 1 g (shaking, a fall) are ignored. */
int  accel_filter_feed(AccelFilter *f, double now_ms, double x, double y, double z);

/* chassis_type (SMBIOS): 30 tablet, 31 convertible, 32 detachable. path NULL = the machine's. */
int accel_chassis_tablet(const char *path);

typedef struct {
    char dir[256];                  /* …/iio:deviceN */
    char name[64];
    int fd[3];
    double scale, offset, m[9];
    int matrix_from;                /* 0 identity, 1 udev hwdb, 2 driver */
} AccelDev;

/* Finds the screen's accelerometer under iio_root (NULL = /sys/bus/iio/devices), with udev's
 * properties from udev_root (NULL = /run/udev/data). 0 and a with its files open, or -1. */
int  accel_find(const char *iio_root, const char *udev_root, AccelDev *a);
/* One reading in m/s², in the screen's frame. 0 if read. */
int  accel_read(AccelDev *a, double v[3]);
void accel_close(AccelDev *a);

/* The runtime: the device found once, the thread started on the first accel_want(1). */
int  accel_present(void);           /* is there a screen accelerometer (looked up once) */
void accel_want(int on);            /* read it (on) or let the thread sleep */
/* The latest reading since the last call: 1 and v when there is a new one. */
int  accel_latest(double v[3]);

#endif
