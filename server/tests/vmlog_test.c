/* vmlog_test.c — lord-vm's log: whole lines, repeats counted, rotation when full (src/vm/vmlog.c) */
#include "../src/vm/vmlog.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static char got[16][VMLOG_LINE_MAX + 1];
static int ngot;
static void take(void *ctx, const char *line, size_t n) {
    (void)ctx;
    if (ngot < 16) { memcpy(got[ngot], line, n); got[ngot][n] = 0; }
    ngot++;
}

static void lines(void) {
    VmLineBuf b = { .n = 0 };
    ngot = 0;
    /* QEMU's message in two writes (its timestamp, then the text), as the capped log of 2026-11-22 showed cut */
    vmlog_lines(&b, "2026-11-22T14:04:03.431181Z ", 28, take, NULL);
    CHECK(ngot == 0, "a line without its end was given: %d", ngot);
    vmlog_lines(&b, "qemu-system-x86_64: Failed\nsecond\n\nthi", 39, take, NULL);
    CHECK(ngot == 2, "two lines expected, %d", ngot);
    CHECK(!strcmp(got[0], "2026-11-22T14:04:03.431181Z qemu-system-x86_64: Failed"), "joined line: [%s]", got[0]);
    CHECK(!strcmp(got[1], "second"), "second: [%s]", got[1]);
    vmlog_lines_flush(&b, take, NULL);
    CHECK(ngot == 3 && !strcmp(got[2], "thi"), "flush: %d [%s]", ngot, got[2]);
    vmlog_lines_flush(&b, take, NULL);
    CHECK(ngot == 3, "an empty flush gave a line");
    /* longer than the buffer: in pieces, nothing lost */
    char *big = malloc(VMLOG_LINE_MAX * 2 + 10);
    memset(big, 'x', VMLOG_LINE_MAX * 2 + 9);
    big[VMLOG_LINE_MAX * 2 + 9] = '\n';
    ngot = 0;
    vmlog_lines(&b, big, VMLOG_LINE_MAX * 2 + 10, take, NULL);
    CHECK(ngot == 3 && strlen(got[0]) == VMLOG_LINE_MAX && strlen(got[2]) == 9, "long line: %d pieces", ngot);
    free(big);
}

static void key(void) {
    size_t kn;
    const char *l = "2026-11-22T14:04:50.906182Z qemu-system-x86_64: Failed to call update: The connection is closed";
    const char *k = vmlog_key(l, strlen(l), &kn);
    CHECK(kn == strlen("qemu-system-x86_64: Failed to call update: The connection is closed") && !strncmp(k, "qemu-system", 11), "key [%.*s]", (int)kn, k);
    l = "2026-11-22T14:04:50 x";
    k = vmlog_key(l, strlen(l), &kn);
    CHECK(kn == 1 && *k == 'x', "no fraction, no Z: [%.*s]", (int)kn, k);
    l = "2026-11-22T14:04:50.123Zx";
    k = vmlog_key(l, strlen(l), &kn);
    CHECK(kn == strlen(l), "no space after: not a prefix");
    l = "credential firstboot.locale";
    k = vmlog_key(l, strlen(l), &kn);
    CHECK(k == l && kn == strlen(l), "no timestamp kept whole");
}

static void repeats(void) {
    VmLogRep r;
    memset(&r, 0, sizeof r);
    char sum[160];
    char line[160];
    long long t = 1000000;
    int logged = 0, sums = 0;
    /* the incident: the same error twice every 30 ms for 15 minutes, each with its own timestamp */
    for (int i = 0; i < 2 * 30000; i++) {
        long long now = t + (i / 2) * 30;
        snprintf(line, sizeof line, "2026-11-22T14:%02d:%02d.%06dZ qemu-system-x86_64: Failed to call update: The connection is closed",
                 (int)(now / 60000 % 60), (int)(now / 1000 % 60), i);
        logged += vmlog_repeat(&r, line, strlen(line), now, sum, sizeof sum);
        if (sum[0]) { sums++; CHECK(strstr(sum, "more times in"), "summary text [%s]", sum); }
    }
    CHECK(logged == 1, "the repeated line logged %d times", logged);
    /* 900 s of it: one summary each 10 s */
    CHECK(sums >= 88 && sums <= 90, "summaries: %d", sums);
    /* a different line: the count so far first, then it is logged */
    long long now = t + 30000 * 30 + 100;
    int say = vmlog_repeat(&r, "agent: connected", 16, now, sum, sizeof sum);
    CHECK(say == 1, "a new line wasn't logged");
    CHECK(sum[0] && strstr(sum, "came"), "no summary before a new line");
    say = vmlog_repeat(&r, "agent: connected", 16, now + 5, sum, sizeof sum);
    CHECK(say == 0 && !sum[0], "a repeat within 10 s logged or summarized");
    CHECK(!vmlog_repeat_tick(&r, now + 9000, sum, sizeof sum), "tick before 10 s");
    CHECK(vmlog_repeat_tick(&r, now + 10000, sum, sizeof sum) && !strcmp(sum, "(the line before came 1 more time in 10 s)"), "tick: [%s]", sum);
    CHECK(!vmlog_repeat_tick(&r, now + 30000, sum, sizeof sum), "tick with nothing to say");
    /* different lines all pass */
    say = vmlog_repeat(&r, "a", 1, now, sum, sizeof sum) + vmlog_repeat(&r, "b", 1, now, sum, sizeof sum) + vmlog_repeat(&r, "a", 1, now, sum, sizeof sum);
    CHECK(say == 3, "alternating lines: %d logged", say);
}

static long fsize(const char *p) { struct stat st; return stat(p, &st) ? -1 : (long)st.st_size; }

static void rotation(void) {
    char dir[] = "/tmp/vmlog-test-XXXXXX";
    CHECK(mkdtemp(dir) != NULL, "mkdtemp");
    char p[256];
    for (int round = 0; round < 5; round++) {
        int fd = vmlog_rotate(dir, "lord-vm", 3);
        CHECK(fd >= 0, "rotate %d: no fd", round);
        char b[32];
        int n = snprintf(b, sizeof b, "round %d\n", round);
        CHECK(write(fd, b, (size_t)n) == n, "write");
        close(fd);
    }
    /* the newest three kept, the older dropped: the folder stays within 3 files */
    snprintf(p, sizeof p, "%s/lord-vm.log", dir);
    char b[32] = { 0 };
    int fd = open(p, O_RDONLY);
    CHECK(fd >= 0 && read(fd, b, sizeof b - 1) > 0 && !strcmp(b, "round 4\n"), "current: [%s]", b);
    if (fd >= 0) close(fd);
    snprintf(p, sizeof p, "%s/lord-vm.1.log", dir);
    memset(b, 0, sizeof b);
    fd = open(p, O_RDONLY);
    CHECK(fd >= 0 && read(fd, b, sizeof b - 1) > 0 && !strcmp(b, "round 3\n"), ".1: [%s]", b);
    if (fd >= 0) close(fd);
    snprintf(p, sizeof p, "%s/lord-vm.2.log", dir);
    CHECK(fsize(p) == 8, ".2 missing");
    snprintf(p, sizeof p, "%s/lord-vm.3.log", dir);
    CHECK(fsize(p) < 0, "a fourth file was kept");
    CHECK(vmlog_rotate(dir, "lord-vm", 0) < 0 && vmlog_rotate(dir, "lord-vm", 10) < 0, "keep out of range accepted");
    const char *names[] = { "lord-vm.log", "lord-vm.1.log", "lord-vm.2.log" };
    for (int i = 0; i < 3; i++) { snprintf(p, sizeof p, "%s/%s", dir, names[i]); unlink(p); }
    rmdir(dir);
}

/* QEMU's 3D abort at a guest's virtio-gpu-gl reset (QEMU issue 4918): the line as lord-vm logged it on 2026-11-23, and
 * the same message with QEMU's timestamp; nothing else of QEMU's */
static void qemu_3d_abort(void) {
    const char *l = "qemu-system-x86_64: ui/dbus-listener.c:511: dbus_scanout_texture: Assertion `tex_id' failed.";
    CHECK(vmlog_qemu_3d_abort(l, strlen(l)), "the abort line not recognised");
    l = "2026-11-23T10:01:02.123456Z qemu-system-x86_64: ../ui/dbus-listener.c:600: dbus_scanout_texture: Assertion `tex_id' failed.";
    CHECK(vmlog_qemu_3d_abort(l, strlen(l)), "with a timestamp, QEMU 11's line number");
    CHECK(!vmlog_qemu_3d_abort(l, 40), "cut before the words");
    const char *other[] = {
        "qemu-system-x86_64: Failed to call update: The connection is closed",
        "qemu-system-x86_64: dbus_scanout_texture: failed to get fd for texture",
        "qemu-system-x86_64: ui/console.c:1: foo: Assertion `tex_id' failed.",
        "",
    };
    for (int i = 0; i < 4; i++) CHECK(!vmlog_qemu_3d_abort(other[i], strlen(other[i])), "[%s] taken for the 3D abort", other[i]);
}

/* how a run that ended in that abort is recorded (vmlog_3d_abort_end): asked, what the guest's tools said (0, 1 power
 * off, 2 restart), whether tools that say were there, how long it ran */
static void qemu_3d_end(void) {
    const long long up = 5LL * 60 * 1000, early = 10000;
    /* a stop asked from LordOS: a shutdown, whatever else, even right after the start */
    CHECK(vmlog_3d_abort_end(1, 0, 0, up) == VMLOG_3D_SHUTDOWN, "asked, no tools");
    CHECK(vmlog_3d_abort_end(1, 2, 1, up) == VMLOG_3D_SHUTDOWN, "asked wins over the guest's restart");
    CHECK(vmlog_3d_abort_end(1, 0, 0, 0) == VMLOG_3D_SHUTDOWN, "asked before it ran");
    /* the guest's tools said */
    CHECK(vmlog_3d_abort_end(0, 1, 1, up) == VMLOG_3D_SHUTDOWN, "the guest powers off");
    CHECK(vmlog_3d_abort_end(0, 1, 0, early) == VMLOG_3D_SHUTDOWN, "a power off, early, the hello never came");
    CHECK(vmlog_3d_abort_end(0, 2, 1, up) == VMLOG_3D_RESTART, "the guest restarts");
    CHECK(vmlog_3d_abort_end(0, 2, 0, VMLOG_3D_MIN_RUN_MS) == VMLOG_3D_RESTART, "a restart said by the hook only (no one signed in)");
    /* a restart that aborts again right away (a restart loop): the error screen, never another start */
    CHECK(vmlog_3d_abort_end(0, 2, 1, early) == VMLOG_3D_CRASH, "a restart right after the start");
    CHECK(vmlog_3d_abort_end(0, 2, 1, VMLOG_3D_MIN_RUN_MS - 1) == VMLOG_3D_CRASH, "just under the minimum");
    /* nothing said */
    CHECK(vmlog_3d_abort_end(0, 0, 1, up) == VMLOG_3D_CRASH, "tools that would say said nothing: not a stop");
    CHECK(vmlog_3d_abort_end(0, 0, 0, up) == VMLOG_3D_GUEST_STOP, "no tools, up a while: stopped by the guest");
    CHECK(vmlog_3d_abort_end(0, 0, 0, early) == VMLOG_3D_CRASH, "no tools, right after the start");
    CHECK(vmlog_3d_abort_end(0, 0, 0, 0) == VMLOG_3D_CRASH, "never ran");
    /* only an explicit restart restarts */
    for (int asked = 0; asked < 2; asked++)
        for (int notice = 0; notice < 3; notice++)
            for (int ann = 0; ann < 2; ann++)
                for (long long t = 0; t <= up; t += 5000)
                    if (vmlog_3d_abort_end(asked, notice, ann, t) == VMLOG_3D_RESTART)
                        CHECK(!asked && notice == 2 && t >= VMLOG_3D_MIN_RUN_MS, "a restart from asked %d notice %d", asked, notice);
}

int main(void) {
    lines();
    key();
    qemu_3d_abort();
    qemu_3d_end();
    repeats();
    rotation();
    if (fails) { fprintf(stderr, "vmlog-test: %d failure(s)\n", fails); return 1; }
    printf("vmlog-test: ok\n");
    return 0;
}
