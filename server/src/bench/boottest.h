/* boottest.h — LordBench's boot time test (docs/BENCHMARK.md, "Boot"): with an administrator's consent,
 * the computer restarts N times in a row (2 to 10) and lord-sessiond records each boot's time to the first
 * frame, marked as part of the test, in /var/lib/lordos/boot-times.jsonl.
 *
 * The state is one small root-owned file, /var/lib/lordos/boot-test, written only by the admin helper's
 * closed actions `boot-test <n>` and `boot-test-cancel` (hw/hw_admin.c) and by lord-sessiond, always in this
 * fixed format, never with user text:
 *
 *   lordos-boot-test 1
 *   total <n>
 *   left <boots still to measure, this one included>
 *   at <unix time it was written>
 *
 * At each boot's first frame lord-sessiond asks bt_next() what to do: record this boot as number k of the
 * test and, if more are left, write the file again and restart BT_SETTLE_S seconds later. The test always
 * ends: `left` only goes down, a file older than BT_STALE_S (or from the future) is discarded, a login at
 * the login screen or `boot-test-cancel` removes it, and lord-sessiond checks the file is still there right
 * before restarting. Used by lord-sessiond (remote/sessiond.c), the admin helper and lord.sysinfo.bench. */
#ifndef LORD_BENCH_BOOTTEST_H
#define LORD_BENCH_BOOTTEST_H

#define BT_PATH     "/var/lib/lordos/boot-test"
#define BT_MIN      2
#define BT_MAX      10
#define BT_SETTLE_S 30                  /* from the first frame to the next restart: time to cancel */
#define BT_STALE_S  1800

typedef struct { int total, left; long long at; } BootTest;

/* the file's text; 0, or -1 if the values are out of range */
int bt_format(const BootTest *b, char *out, unsigned n);
/* strict parse of that text; 0, or -1 */
int bt_parse(const char *text, BootTest *b);
/* reads path: a regular root-owned file, not writable by group or others, at most 256 bytes; 0 or -1 */
int bt_read(const char *path, BootTest *b);
/* writes path whole (a temporary file, then rename), 0644; 0 or -1 */
int bt_write(const char *path, const BootTest *b);

enum { BT_NONE = 0, BT_STALE, BT_LAST, BT_AGAIN };
/* what a boot's first frame does with the state read (or none: have = 0) at time now: BT_NONE (no test),
 * BT_STALE (discard the file), BT_LAST (this is boot k, the last: remove the file) or BT_AGAIN (boot k:
 * write *after, restart later) */
int bt_next(int have, const BootTest *b, long long now, int *k, BootTest *after);

#endif
