/* mstudio.c — LordOS Studio from lord-server (see mstudio.h and docs/CAMERA-STUDIO-PLAN.md). */
#include "mstudio.h"
#include "media_int.h"
#include "proto.h"
#include "studio/models.h"
#include "../fsops.h"
#include "../pkg/crypto.h"
#include "../ui/js_sys.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define BG_MAX (32 << 20)            /* bytes of a background picture */

static struct {
    char camera[32];                 /* "/dev/videoN" or "" (the first camera) */
    int mode, blur, mirror;
    char bg[PATH_MAX];               /* the checked real path, or "" */
    char bg_name[PATH_MAX];          /* as JS gave it (status) */
} cfg = { "", 1, 50, 0, "", "" };

static struct { long long frames, seg_us, fx_us; int gpu; double fps; double at; long long at_frames; } stats;
static int model_state;              /* 0 unknown, 1 good, -1 refused */

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static int dev_ok(const char *d) {
    if (!d || strncmp(d, "/dev/video", 10)) return 0;
    size_t n = strlen(d + 10);
    if (n < 1 || n > 2) return 0;
    for (size_t i = 0; i < n; i++) if (!isdigit((unsigned char)d[10 + i])) return 0;
    return 1;
}

/* a V4L2 device that captures and streams (not LordOS Studio's own loopback, later) */
static int is_camera(const char *dev) {
    int fd = open(dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return 0;
    struct v4l2_capability cap;
    memset(&cap, 0, sizeof cap);
    int ok = 0;
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
        uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
        ok = (caps & V4L2_CAP_VIDEO_CAPTURE) && (caps & V4L2_CAP_STREAMING) && strcmp((const char *)cap.card, "LordOS Studio");
    }
    close(fd);
    return ok;
}

static int first_camera(char *out, size_t n) {
    for (int i = 0; i < 64; i++) {
        char dev[32];
        snprintf(dev, sizeof dev, "/dev/video%d", i);
        if (is_camera(dev)) { snprintf(out, n, "%s", dev); return 0; }
    }
    return -1;
}

/* The model: opened read-only, its size and SHA-256 checked against models.h on every open (the helper reads the
 * same fd). -1 if it is missing or differs. */
static int open_model(void) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/studio/models/%s", js_sys_assets(), STUDIO_SEG_FILE);
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > STUDIO_MODEL_MAX) {
        if (fd >= 0) close(fd);
        if (model_state != -1) fprintf(stderr, "studio: the model %s is missing\n", path);
        model_state = -1;
        return -1;
    }
    unsigned char *b = malloc((size_t)st.st_size);
    size_t got = 0;
    while (b && got < (size_t)st.st_size) {
        ssize_t r = pread(fd, b + got, (size_t)st.st_size - got, (off_t)got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    uint8_t h[32];
    char hex[65] = "";
    if (b && got == (size_t)st.st_size) {
        sha256(b, got, h);
        for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", h[i]);
    }
    free(b);
    if (strcmp(hex, STUDIO_SEG_SHA256)) {
        if (model_state != -1) fprintf(stderr, "studio: the model %s does not match its pinned hash; LordOS Studio stays off\n", path);
        model_state = -1;
        close(fd);
        return -1;
    }
    model_state = 1;
    return fd;
}

int mstudio_available(void) {
    if (!media_has_studio()) return 0;
    if (model_state == 0) { int fd = open_model(); if (fd >= 0) close(fd); }
    return model_state == 1;
}

void mstudio_camera_entry(char *json, size_t n, int first) {
    if (!n) return;
    json[0] = 0;
    if (!mstudio_available()) return;
    snprintf(json, n, "%s{\"device\":\"" STUDIO_DEVICE "\",\"name\":\"LordOS Studio\",\"bus\":\"studio\",\"studio\":true}", first ? "" : ",");
}

/* the background: one of LordOS's wallpapers ("asset:wallpapers/…") or a picture inside lord.fs's roots; a regular
 * file of at most BG_MAX with a picture's extension. real: the resolved path */
static int check_background(const char *path, char *real, size_t n) {
    char full[PATH_MAX * 2], r[PATH_MAX];
    if (!path || !*path || strlen(path) >= PATH_MAX) return -1;
    for (const char *p = path; *p; p++) if ((unsigned char)*p < 0x20) return -1;
    int asset = !strncmp(path, "asset:wallpapers/", 17);
    if (asset) {
        if (strstr(path, "..")) return -1;
        snprintf(full, sizeof full, "%s/wallpapers/%s", js_sys_assets(), path + 17);
    } else if (path[0] == '/') snprintf(full, sizeof full, "%s", path);
    else return -1;
    if (!realpath(full, r)) return -1;
    if (asset) {
        char base[PATH_MAX], dir[PATH_MAX * 2];
        snprintf(dir, sizeof dir, "%s/wallpapers", js_sys_assets());
        size_t bl;
        if (!realpath(dir, base) || (bl = strlen(base), strncmp(r, base, bl)) || r[bl] != '/') return -1;
    } else if (!fsops_allowed(r)) return -1;
    const char *dot = strrchr(r, '.');
    char ext[8] = "";
    for (int i = 0; dot && dot[1 + i] && i < 7; i++) { ext[i] = (char)tolower((unsigned char)dot[1 + i]); ext[i + 1] = 0; }
    if (strcmp(ext, "jpg") && strcmp(ext, "jpeg") && strcmp(ext, "png") && strcmp(ext, "webp")) return -1;
    snprintf(real, n, "%s", r);
    return 0;
}

static int open_background(const char *real) {
    int fd = open(real, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > BG_MAX) { if (fd >= 0) close(fd); return -1; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    return fd;
}

static void send_config(int sock) {
    mp_sendv(sock, MM_STUDIO, 0, cfg.mode, cfg.blur, cfg.mirror, 0);
}

static void send_background(int sock) {
    int fd = cfg.bg[0] ? open_background(cfg.bg) : -1;
    mp_send(sock, MM_STUDIO_BG, 0, NULL, NULL, 0, fd >= 0 ? &fd : NULL, fd >= 0 ? 1 : 0);
    if (fd >= 0) close(fd);
}

void mstudio_attach(int sock) {
    if (sock < 0) return;
    send_config(sock);
    if (cfg.bg[0]) send_background(sock);
}

int mstudio_spawn(int w, int h, pid_t *pid, const char **code) {
    *code = "studio.unavailable";
    if (!mstudio_available()) return -1;
    char dev[32];
    if (cfg.camera[0] && is_camera(cfg.camera)) snprintf(dev, sizeof dev, "%s", cfg.camera);
    else if (first_camera(dev, sizeof dev)) { *code = "studio.noCamera"; return -1; }
    int cam = open(dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (cam < 0) { *code = "camera.openFailed"; return -1; }
    int model = open_model();
    if (model < 0) { close(cam); return -1; }
    int render = media_render_fd();
    if (w < 160 || w > 3840) w = 1280;
    if (h < 120 || h > 2160) h = 720;
    char params[128];
    snprintf(params, sizeof params, "width=%d\nheight=%d\nhw=%d\nhwaccel=%s\nrender=%d\n", w, h, render >= 0, media_hwaccel(), render >= 0);
    int fds[3], n = 0;
    fds[n++] = cam;
    if (render >= 0) fds[n++] = render;
    fds[n++] = model;
    int sock = media_spawn_studio(params, fds, n, pid);
    close(cam);
    close(model);
    if (render >= 0) close(render);
    if (sock < 0) { *code = "media.noHelper"; return -1; }
    *code = NULL;
    return sock;
}

static void each_source(void (*fn)(int sock)) {
    for (int i = 0; i < MEDIA_MAX_SRC; i++) {
        MediaSrc *s = media_src_at(i);
        if (s && s->studio && s->sock >= 0) fn(s->sock);
    }
}

int mstudio_set(const char *camera, int mode, int blur, int mirror) {
    if (camera && *camera && !dev_ok(camera)) return -1;
    if (camera) snprintf(cfg.camera, sizeof cfg.camera, "%s", camera);
    cfg.mode = mode < 0 || mode > 2 ? 0 : mode;
    cfg.blur = blur < 0 ? 0 : blur > 100 ? 100 : blur;
    cfg.mirror = mirror != 0;
    each_source(send_config);
    return 0;
}

int mstudio_set_background(const char *path) {
    if (!path || !*path) {
        cfg.bg[0] = cfg.bg_name[0] = 0;
        each_source(send_background);
        return 0;
    }
    char real[PATH_MAX];
    if (check_background(path, real, sizeof real)) return -1;
    int fd = open_background(real);
    if (fd < 0) return -1;
    close(fd);
    snprintf(cfg.bg, sizeof cfg.bg, "%s", real);
    snprintf(cfg.bg_name, sizeof cfg.bg_name, "%s", path);
    each_source(send_background);
    return 0;
}

void mstudio_stats(int src_id, long long frames, long long seg_us, long long fx_us, long long gpu) {
    if (frames < 0 || frames > (1LL << 40) || seg_us < 0 || seg_us > 10000000 || fx_us < 0 || fx_us > 10000000 || (gpu != 0 && gpu != 1)) return;
    double t = now_s();
    if (stats.at > 0 && t > stats.at && frames >= stats.at_frames) stats.fps = (double)(frames - stats.at_frames) / (t - stats.at);
    stats.at = t;
    stats.at_frames = frames;
    stats.frames = frames;
    stats.seg_us = seg_us;
    stats.fx_us = fx_us;
    stats.gpu = (int)gpu;
    media_event(src_id, "studio", "{\"fps\":%.1f,\"segMs\":%.2f,\"fxMs\":%.2f,\"gpu\":%s}", stats.fps, seg_us / 1000.0, fx_us / 1000.0, gpu ? "true" : "false");
}

void mstudio_status(char *json, size_t n) {
    static const char *const modes[] = { "none", "blur", "picture" };
    char bg[PATH_MAX * 2 + 8], cam[80];
    media_json_str(bg, sizeof bg, cfg.bg_name);
    media_json_str(cam, sizeof cam, cfg.camera);
    size_t o = (size_t)snprintf(json, n, "{\"available\":%s,\"camera\":%s,\"mode\":\"%s\",\"blur\":%d,\"mirror\":%s,\"background\":%s,"
                                "\"fps\":%.1f,\"segMs\":%.2f,\"fxMs\":%.2f,\"gpu\":%s,\"sources\":[",
                                mstudio_available() ? "true" : "false", cam, modes[cfg.mode], cfg.blur, cfg.mirror ? "true" : "false", bg,
                                stats.fps, stats.seg_us / 1000.0, stats.fx_us / 1000.0, stats.gpu ? "true" : "false");
    int first = 1;
    for (int i = 0; i < MEDIA_MAX_SRC && o + 160 < n; i++) {
        MediaSrc *s = media_src_at(i);
        if (!s || !s->studio || s->sock < 0) continue;
        char app[MEDIA_APP_MAX * 2 + 8];
        media_json_str(app, sizeof app, s->app);
        o += (size_t)snprintf(json + o, n - o, "%s{\"id\":%d,\"app\":%s,\"isolated\":%s}", first ? "" : ",", s->id, app, s->bk ? "true" : "false");
        first = 0;
    }
    if (o + 3 < n) snprintf(json + o, n - o, "]}");
}
