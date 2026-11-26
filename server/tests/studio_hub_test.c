/* studio_hub_test.c — LordOS Studio's camera for every app (docs/CAMERA-STUDIO-PLAN.md 2.2 and 8.2; phase 2), the parts
 * that need no v4l2loopback: what the loopback shows and when its lord-studio runs (media/mstudiohub.c), finding the
 * processes that hold a device open and naming them for the capture indicator, the idle and off cards and the YUYV
 * conversion lord-studio writes (media/studio/stout.c), and the client-usage event's payload.
 *
 * The real device (Chromium, Chrome, Firefox and OBS seeing "LordOS Studio", the camera's light) is checked on a VM:
 * tests/fwlinux/devices.sh and docs/CAMERA-STUDIO-PLAN.md 11.2. */
#include "media/mstudiohub.h"
#include "media/studio/stout.h"

#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_show(void) {
    /* on: the camera only with a reader or the preview; nothing reads → the idle card, camera closed */
    CHECK(studio_hub_show(1, 0, 0, 0) == STUDIO_SHOW_IDLE, "on, nobody");
    CHECK(studio_hub_show(1, 1, 0, 0) == STUDIO_SHOW_CAMERA, "on, a reader");
    CHECK(studio_hub_show(1, 0, 0, 1) == STUDIO_SHOW_CAMERA, "on, the preview");
    CHECK(studio_hub_show(1, 0, 3, 0) == STUDIO_SHOW_IDLE, "the event says nobody streams: openers that only listed it don't count");
    CHECK(studio_hub_show(1, -1, 1, 0) == STUDIO_SHOW_CAMERA, "no event: the openers count");
    CHECK(studio_hub_show(1, -1, 0, 0) == STUDIO_SHOW_IDLE, "no event, no openers");
    /* off: never the camera; the off card while an app still reads, even with the preview asked */
    CHECK(studio_hub_show(0, 1, 0, 1) == STUDIO_SHOW_OFF, "off with a reader");
    CHECK(studio_hub_show(0, 0, 0, 1) == STUDIO_SHOW_IDLE, "off, only the preview");
    CHECK(studio_hub_show(0, -1, 2, 0) == STUDIO_SHOW_OFF, "off, no event, openers");
    /* the hub runs: on with the device and the model; off only while somebody still reads */
    CHECK(studio_hub_wanted(1, 1, 1, 0, 0), "on");
    CHECK(!studio_hub_wanted(1, 1, 0, 0, 0), "no device (the package isn't installed)");
    CHECK(!studio_hub_wanted(1, 0, 1, 0, 0), "no model or no lord-studio");
    CHECK(!studio_hub_wanted(0, 1, 1, 0, 0), "off, nobody");
    CHECK(studio_hub_wanted(0, 1, 1, 1, 0), "off, a reader: the off card until it lets go");
    CHECK(studio_hub_wanted(0, 1, 1, -1, 1), "off, no event, an opener");
    /* the device's card: ours, or the label 0.8.9.5's package left (its quotes split at the comma); never OBS's, either
     * way it came up, nor a near miss. Not recognising it kept lord-studio from running at all, so no app listed it. */
    CHECK(studio_card_is_ours("LordOS Studio"), "the label");
    CHECK(studio_card_is_ours("LordOS Studio\""), "0.8.9.5's label until the module is loaded again");
    CHECK(!studio_card_is_ours("OBS Virtual Camera") && !studio_card_is_ours("\"OBS Virtual Camera"), "OBS's device, either way");
    CHECK(!studio_card_is_ours("\"LordOS Studio") && !studio_card_is_ours("LordOS Studio\"\"") && !studio_card_is_ours("LordOS Studi"),
          "near misses");
    CHECK(!studio_card_is_ours("LordOS Studio 2") && !studio_card_is_ours("") && !studio_card_is_ours(NULL), "others, empty, none");
}

static void test_scan(void) {
    /* a child holds /dev/zero; scanned as if lord-server were another process (self = 1), it is found; as its parent
     * (self = this process) it is skipped like lord-server's own helpers */
    struct stat st;
    CHECK(stat("/dev/zero", &st) == 0, "stat /dev/zero");
    int pfd[2];
    CHECK(pipe(pfd) == 0, "pipe");
    pid_t c = fork();
    if (c == 0) {
        for (int f = 0; f < 3; f++) if (f != pfd[1]) close(f);     /* /dev/zero its only device (stdin may be /dev/null) */
        int z = open("/dev/zero", O_RDONLY);
        char b = z >= 0 ? 'y' : 'n';
        if (write(pfd[1], &b, 1) != 1) _exit(1);
        pause();
        _exit(0);
    }
    char got = 0;
    CHECK(read(pfd[0], &got, 1) == 1 && got == 'y', "child ready");
    static pid_t pids[8192];
    int n = studio_scan_readers("/proc", st.st_rdev, 1, getuid(), pids, 8192);
    int seen = 0;
    for (int i = 0; i < n && i < 8192; i++) if (pids[i] == c) seen = 1;
    CHECK(seen, "the child holding /dev/zero is found (%d processes)", n);
    n = studio_scan_readers("/proc", st.st_rdev, getpid(), getuid(), pids, 8192);
    seen = 0;
    for (int i = 0; i < n && i < 8192; i++) if (pids[i] == c || pids[i] == getpid()) seen = 1;
    CHECK(!seen, "lord-server's own children and itself are skipped");
    n = studio_scan_readers("/proc", st.st_rdev, 1, getuid() + 4242, pids, 8192);
    CHECK(n == 0, "another uid's processes are never looked at (%d)", n);
    CHECK(studio_scan_readers("/nonexistent", st.st_rdev, 1, getuid(), pids, 8) == 0, "no /proc: nobody");
    /* the camera rows (camera_scan_holders): any device of the major, here /dev/zero's (1) */
    n = camera_scan_holders("/proc", major(st.st_rdev), 0, 1, getuid(), pids, 8192);
    seen = 0;
    for (int i = 0; i < n && i < 8192; i++) if (pids[i] == c) seen = 1;
    CHECK(seen, "camera_scan_holders: the child holding a device of the major is found (%d)", n);
    n = camera_scan_holders("/proc", major(st.st_rdev), st.st_rdev, 1, getuid(), pids, 8192);
    seen = 0;
    for (int i = 0; i < n && i < 8192; i++) if (pids[i] == c) seen = 1;
    CHECK(!seen, "camera_scan_holders: the skipped device (Studio's loopback) isn't counted");
    n = camera_scan_holders("/proc", major(st.st_rdev), 0, getpid(), getuid(), pids, 8192);
    seen = 0;
    for (int i = 0; i < n && i < 8192; i++) if (pids[i] == c || pids[i] == getpid()) seen = 1;
    CHECK(!seen, "camera_scan_holders: lord-server's own children and itself are skipped");
    CHECK(camera_scan_holders("/proc", major(st.st_rdev), 0, 1, getuid() + 4242, pids, 8192) == 0, "camera_scan_holders: another uid: nobody");
    CHECK(camera_scan_holders("/proc", 81, 0, 1, getuid(), NULL, 0) >= 0, "no room: only counted");
    CHECK(camera_scan_holders("/nonexistent", 81, 0, 1, getuid(), pids, 8) == 0, "camera_scan_holders: no /proc: nobody");
    kill(c, SIGKILL);
    waitpid(c, NULL, 0);
    close(pfd[0]);
    close(pfd[1]);
}

static void test_use(void) {
    char apps[4][64] = { "desktop:chromium", "flatpak:com.obsproject.Studio", "desktop:chromium", "bad\"id" };
    char out[512];
    CHECK(studio_use_format(&apps[0][0], 64, 4, "Logitech \"C930e\"", 1, out, sizeof out) == 0, "format");
    CHECK(!strcmp(out, "{\"apps\":[\"desktop:chromium\",\"flatpak:com.obsproject.Studio\",\"\"],\"camera\":\"Logitech  C930e \",\"effects\":true}"),
          "the event: %s", out);
    CHECK(studio_use_format(&apps[0][0], 64, 0, "", 0, out, sizeof out) == 0 && !strcmp(out, "{\"apps\":[],\"camera\":\"\",\"effects\":false}"),
          "nobody: %s", out);
    char br[1][64] = { "x]y" };
    CHECK(studio_use_format(&br[0][0], 64, 1, "c", 0, out, sizeof out) == 0 && strstr(out, "[\"\"]"), "a bracket isn't plain: %s", out);
    CHECK(studio_use_format(&apps[0][0], 64, 4, "cam", 1, out, 40) == -1, "too long: refused, not cut");
}

static void test_camera_use(void) {
    char apps[5][64] = { "desktop:lordos-web-whatsapp", "", "desktop:lordos-web-whatsapp", "bad\"id", "desktop:cheese" };
    char out[512];
    CHECK(camera_use_format(&apps[0][0], 64, 5, out, sizeof out) == 0, "format");
    CHECK(!strcmp(out, "{\"apps\":[\"desktop:lordos-web-whatsapp\",\"\",\"desktop:cheese\"]}"), "each app once, unknowns one \"\": %s", out);
    CHECK(camera_use_format(&apps[0][0], 64, 0, out, sizeof out) == 0 && !strcmp(out, "{\"apps\":[]}"), "nobody: %s", out);
    char full[1][8];
    memset(full[0], 'a', sizeof full[0]);           /* no terminator inside stride: never read past it */
    CHECK(camera_use_format(&full[0][0], sizeof full[0], 1, out, sizeof out) == 0 && !strcmp(out, "{\"apps\":[\"\"]}"), "unterminated: %s", out);
    CHECK(camera_use_format(&apps[0][0], 64, 5, out, 30) == -1, "too long: refused, not cut");
}

static void test_cards(void) {
    int w = 1280, h = 720;
    st_out_size(&w, &h);
    CHECK(w == 1280 && h == 720, "1280x720 stays");
    w = 641; h = 361;
    st_out_size(&w, &h);
    CHECK(w == 640 && h == 360, "even: %dx%d", w, h);
    w = 4000; h = 3000;
    st_out_size(&w, &h);
    CHECK(w == 1280 && h == 720, "out of range: the default");
    CHECK(st_yuyv_size(640, 360) == 640 * 360 * 2, "YUYV size");
    w = 640; h = 360;
    uint8_t *a = malloc((size_t)w * h * 4), *b = malloc((size_t)w * h * 4), *y = malloc(st_yuyv_size(w, h));
    st_card_bgra(ST_CARD_IDLE, a, w, h);
    st_card_bgra(ST_CARD_OFF, b, w, h);
    /* dark edges, a light glyph in the middle; the off card differs from the idle one (the slash) */
    CHECK(a[3] == 255 && a[0] < 60 && a[1] < 60 && a[2] < 60, "a dark corner");
    size_t mid = ((size_t)(h / 2) * w + (w / 2 - 25)) * 4;
    CHECK(a[mid] > 150 && a[mid + 1] > 150 && a[mid + 2] > 150, "the glyph's body in the middle (%d %d %d)", a[mid], a[mid + 1], a[mid + 2]);
    long diff = 0;
    for (size_t i = 0; i < (size_t)w * h * 4; i++) diff += a[i] != b[i];
    CHECK(diff > 1000, "the off card has its slash (%ld bytes differ)", diff);
    /* the conversion: limited range, white → Y 235, black → Y 16, grey's chroma 128 */
    uint8_t px[16] = { 255, 255, 255, 255, 0, 0, 0, 255, 128, 128, 128, 255, 128, 128, 128, 255 };
    uint8_t yy[8];
    st_bgra_to_yuyv(px, 16, 4, 1, yy);
    CHECK(yy[0] == 235 && yy[2] == 16, "white and black: Y %d and %d", yy[0], yy[2]);
    CHECK(yy[4] >= 125 && yy[4] <= 128 && yy[5] == 128 && yy[7] == 128, "grey: Y %d Cb %d Cr %d", yy[4], yy[5], yy[7]);
    uint8_t red[8] = { 0, 0, 255, 255, 0, 0, 255, 255 }, ry[4];
    st_bgra_to_yuyv(red, 8, 2, 1, ry);
    CHECK(ry[0] == 82 && ry[3] == 240 && ry[1] < 100, "red: Y %d Cb %d Cr %d (BT.601)", ry[0], ry[1], ry[3]);
    st_bgra_to_yuyv(a, w * 4, w, h, y);
    int ok = 1;
    for (size_t i = 0; i < st_yuyv_size(w, h); i += 2) if (y[i] < 16 || y[i] > 235 || y[i + 1] < 16 || y[i + 1] > 240) ok = 0;
    CHECK(ok, "the card in YUYV stays in limited range");
    /* how long the cards take (once, at the hub's start) */
    struct timespec t0, t1;
    w = 1920; h = 1080;
    uint8_t *big = malloc((size_t)w * h * 4);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    st_card_bgra(ST_CARD_OFF, big, w, h);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("  the off card at 1920x1080: %.1f ms\n", (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
    free(big);
    free(a);
    free(b);
    free(y);
}

static void test_event(void) {
    uint8_t d[64] = { 0 };
    uint32_t two = 2;
    memcpy(d, &two, 4);
    CHECK(st_client_usage(ST_EVENT_CLIENT_USAGE, d, sizeof d) == 2, "two readers");
    CHECK(st_client_usage(ST_EVENT_CLIENT_USAGE + 1, d, sizeof d) == -1, "another event");
    CHECK(st_client_usage(ST_EVENT_CLIENT_USAGE, d, 2) == -1, "short");
    uint32_t huge = 0xffffffffu;
    memcpy(d, &huge, 4);
    CHECK(st_client_usage(ST_EVENT_CLIENT_USAGE, d, sizeof d) == 1024, "clamped");
}

/* phase 3: the filters' names, MM_STUDIO's bytes and the background's name */
static void test_phase3(void) {
    CHECK(!strcmp(studio_filter_name(0), "none") && !strcmp(studio_filter_name(8), "sepia") && !strcmp(studio_filter_name(9), "none") &&
          !strcmp(studio_filter_name(-1), "none"), "filter names");
    CHECK(studio_filter_id("film") == 6 && studio_filter_id("Film") == -1 && studio_filter_id(NULL) == -1 && studio_filter_id("") == -1, "filter ids");
    int64_t v[6];
    studio_pack_config(3, 1, 6, 1, 70, 40, 20, 50, 60, 55, 30, 80, 25, v);
    CHECK(v[0] == (3 | 1 << 8 | 6 << 16 | 1 << 24) && v[1] == (70 | 40 << 8 | 20 << 16 | 50LL << 24) && v[2] == (60 | 55 << 8 | 30 << 16 | 80LL << 24) &&
          v[3] == 25 && v[4] == 0 && v[5] == 0, "packed config");
    studio_pack_config(9, 7, 99, 5, 300, -4, 101, 1000, -1, 200, 99, 101, 500, v);
    CHECK(v[0] == (3 | 2 << 8 | 8 << 16 | 1 << 24) && v[1] == (100 | 0 << 8 | 100 << 16 | 100LL << 24) && v[2] == (0 | 100 << 8 | 60 << 16 | 100LL << 24) &&
          v[3] == 100, "clamped config");
    static const char *const good[] = {
        "asset:wallpapers/lumo/lordos-lumo-aurora.jpg", "asset:wallpapers/x.jpg", "asset:studio/backgrounds/office-loft.jpg",
        "/var/lib/lordos/studio/0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef.jpg",
        "/var/lib/lordos/studio/0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef.mp4", "/home/u/Pictures/me.png",
    };
    for (size_t i = 0; i < sizeof good / sizeof good[0]; i++) CHECK(studio_bg_path_ok(good[i]) == 0, "refused %s", good[i]);
    static const char *const badp[] = {
        "", "relative.jpg", "asset:wallpapers/../../etc/passwd", "asset:wallpapers/a/b/c.jpg", "asset:wallpapers/.hidden.jpg",
        "asset:studio/backgrounds/sub/x.jpg", "asset:studio/models/x.lnn", "asset:fonts/x.jpg", "/var/lib/lordos/studio/abc.jpg",
        "/var/lib/lordos/studio/0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef.png",
        "/var/lib/lordos/studio/0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef.jpg", "/home/u/a\nb.jpg",
        "/home/u/../../etc/shadow", "asset:studio/backgrounds/a b.jpg",
    };
    for (size_t i = 0; i < sizeof badp / sizeof badp[0]; i++) CHECK(studio_bg_path_ok(badp[i]) == -1, "accepted %s", badp[i]);
    CHECK(studio_bg_path_ok(NULL) == -1, "NULL");
}

int main(void) {
    test_show();
    test_scan();
    test_use();
    test_camera_use();
    test_cards();
    test_event();
    test_phase3();
    if (fails) { printf("studio-hub-test: %d failures\n", fails); return 1; }
    printf("studio-hub-test: ok\n");
    return 0;
}
