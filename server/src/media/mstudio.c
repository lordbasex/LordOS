/* mstudio.c — LordOS Studio from lord-server (see mstudio.h and docs/CAMERA-STUDIO-PLAN.md). */
#include "mstudio.h"
#include "mstudiohub.h"
#include "media_int.h"
#include "proto.h"
#include "studio/models.h"
#include "../fsops.h"
#include "../pkg/crypto.h"
#include "../sec/appid.h"
#include "../sec/restrict.h"
#include "../fw/rules.h"
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
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#define BG_MAX (32 << 20)            /* bytes of a background picture */
#define BG_VIDEO_MAX (100 << 20)     /* bytes of a background video (the catalog's own limit per file) */
#define BG_STORE "/var/lib/lordos/studio"   /* the catalog's downloads (studio-bg-get, root's) */

static struct {
    int on;                          /* LordOS Studio's camera for every app (the loopback's hub) */
    char camera[32];                 /* "/dev/videoN" or "" (the first camera) */
    int mode, blur, mirror;          /* mode 0 none, 1 blur, 2 picture, 3 video (2 and 3: whatever the file is) */
    int key, similarity, smoothness, spill, framing, light, match, filter, filter_amt, retouch;   /* phase 3 (st.h) */
    char bg[PATH_MAX];               /* the checked real path, or "" */
    char bg_name[PATH_MAX];          /* as JS gave it (status) */
} cfg = { .on = 1, .mode = 1, .blur = 50, .similarity = 40, .smoothness = 20, .spill = 50, .filter_amt = 100 };

static struct { long long frames, seg_us, fx_us, frame_us, vid_us; int gpu; double fps; double at; long long at_frames; } stats;
static int model_state;              /* 0 unknown, 1 good, -1 refused */
static int face_state;               /* the same for BlazeFace (optional: without it, no Auto Framing, face light or Retouch) */

/* the hub: the session's one lord-studio writing the loopback */
#define HUB_MAX_READERS 32
static struct {
    int id;                          /* its MediaSrc, 0 none */
    int ready;                       /* it set the format and wrote the first card (its first MM_STUDIO_READERS) */
    int events;                      /* the module sends the client-usage event */
    int readers;                     /* streaming readers (-1 unknown) */
    int scanned;                     /* openers lord-server found (the attribution, and the count without events) */
    int cam_open, shows;             /* what the hub says it does */
    int sent_show, sent_preview;     /* the last MM_STUDIO_CAM */
    double sent_at;
    int preview;                     /* the popover's preview wants its frames */
    dev_t rdev;                      /* the loopback's device number */
    int device;                      /* the loopback is there (last look) */
    double device_at, tick_at, scan_at;
    double spawns[3];                /* the last starts: at most 3 a minute */
    int spawn_i, gave_up;
    char camera_name[64];            /* the real camera it uses (the indicator) */
    char use_last[2048];
} hub = { .readers = -1, .sent_show = -1, .use_last = "" };
static void (*use_emit)(const char *kind, const char *json);

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static int dev_ok(const char *d) {
    if (!d || strncmp(d, "/dev/video", 10)) return 0;
    size_t n = strlen(d + 10);
    if (n < 1 || n > 2) return 0;
    for (size_t i = 0; i < n; i++) if (!isdigit((unsigned char)d[10 + i])) return 0;
    return 1;
}

int mstudio_is_loopback_card(const char *card) { return studio_card_is_ours(card); }

/* a V4L2 device that captures and streams, and isn't LordOS Studio's own loopback; name: its card (or NULL) */
static int is_camera_named(const char *dev, char *name, size_t n) {
    int fd = open(dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return 0;
    struct v4l2_capability cap;
    memset(&cap, 0, sizeof cap);
    int ok = 0;
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
        uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
        ok = (caps & V4L2_CAP_VIDEO_CAPTURE) && (caps & V4L2_CAP_STREAMING) && !mstudio_is_loopback_card((const char *)cap.card);
        if (ok && name) snprintf(name, n, "%s", (const char *)cap.card);
    }
    close(fd);
    return ok;
}
static int is_camera(const char *dev) { return is_camera_named(dev, NULL, 0); }

static int first_camera(char *out, size_t n) {
    for (int i = 0; i < 64; i++) {
        char dev[32];
        snprintf(dev, sizeof dev, "/dev/video%d", i);
        if (is_camera(dev)) { snprintf(out, n, "%s", dev); return 0; }
    }
    return -1;
}

/* the chosen real camera (or the first one), opened for a helper; name: its card. -1 with code */
static int open_real_camera(char *name, size_t n, const char **code) {
    char dev[32];
    if (cfg.camera[0] && is_camera_named(cfg.camera, name, n)) snprintf(dev, sizeof dev, "%s", cfg.camera);
    else if (first_camera(dev, sizeof dev) || !is_camera_named(dev, name, n)) { *code = "studio.noCamera"; return -1; }
    int fd = open(dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) *code = "camera.openFailed";
    return fd;
}

/* The loopback: /dev/video50, a V4L2 character device (major 81) whose driver is v4l2loopback and whose card is
 * "LordOS Studio" (the package's modprobe.d file). Opened read-write for its one writer or a reader; -1 otherwise. */
static int loopback_open(void) {
    int fd = open(STUDIO_LOOPBACK_DEV, O_RDWR | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    struct v4l2_capability cap;
    memset(&cap, 0, sizeof cap);
    if (fstat(fd, &st) || !S_ISCHR(st.st_mode) || major(st.st_rdev) != STUDIO_V4L2_MAJOR || ioctl(fd, VIDIOC_QUERYCAP, &cap) ||
        strcmp((const char *)cap.driver, STUDIO_LOOPBACK_DRIVER) || !mstudio_is_loopback_card((const char *)cap.card)) {
        close(fd);
        return -1;
    }
    hub.rdev = st.st_rdev;
    return fd;
}

/* The model: opened read-only, its size and SHA-256 checked against models.h on every open (the helper reads the
 * same fd). -1 if it is missing or differs. */
static int open_pinned(const char *file, const char *sha, int *state) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/studio/models/%s", js_sys_assets(), file);
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > STUDIO_MODEL_MAX) {
        if (fd >= 0) close(fd);
        if (*state != -1) fprintf(stderr, "studio: the model %s is missing\n", path);
        *state = -1;
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
    if (strcmp(hex, sha)) {
        if (*state != -1) fprintf(stderr, "studio: the model %s does not match its pinned hash; it isn't used\n", path);
        *state = -1;
        close(fd);
        return -1;
    }
    *state = 1;
    return fd;
}

static int open_model(void) { return open_pinned(STUDIO_SEG_FILE, STUDIO_SEG_SHA256, &model_state); }
static int open_face(void) { return open_pinned(STUDIO_FACE_FILE, STUDIO_FACE_SHA256, &face_state); }

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

/* r (a real path) is inside the real directory dir */
static int inside(const char *r, const char *dir) {
    char base[PATH_MAX];
    size_t bl;
    return realpath(dir, base) && (bl = strlen(base), !strncmp(r, base, bl)) && r[bl] == '/';
}

static int is_video_ext(const char *ext) {
    return !strcmp(ext, "mp4") || !strcmp(ext, "webm") || !strcmp(ext, "mkv") || !strcmp(ext, "mov") || !strcmp(ext, "m4v");
}

/* The background (studio_bg_path_ok in mstudiohub.c says which names are acceptable at all):
 *   "asset:wallpapers/…"            one of LordOS's wallpapers
 *   "asset:studio/backgrounds/…"    one of the catalog's pictures shipped in the package
 *   "/var/lib/lordos/studio/<sha256>.jpg|.mp4"   one the catalog downloaded (studio-bg-get), owned by root
 *   any other absolute path         a picture or a short video inside lord.fs's roots
 * A regular file with a picture's (or a video's) extension. real: the resolved path; *video: 1 for a video. */
static int check_background(const char *path, char *real, size_t n, int *video) {
    char full[PATH_MAX * 2], r[PATH_MAX], dir[PATH_MAX * 2];
    if (studio_bg_path_ok(path)) return -1;
    int wall = !strncmp(path, "asset:wallpapers/", 17), cat = !strncmp(path, "asset:studio/backgrounds/", 25);
    int store = !strncmp(path, BG_STORE "/", sizeof BG_STORE);
    if (wall) snprintf(full, sizeof full, "%s/wallpapers/%s", js_sys_assets(), path + 17);
    else if (cat) snprintf(full, sizeof full, "%s/studio/backgrounds/%s", js_sys_assets(), path + 25);
    else snprintf(full, sizeof full, "%s", path);
    if (!realpath(full, r)) return -1;
    if (wall || cat) {
        snprintf(dir, sizeof dir, wall ? "%s/wallpapers" : "%s/studio/backgrounds", js_sys_assets());
        if (!inside(r, dir)) return -1;
    } else if (store) {
        struct stat st;
        if (!inside(r, BG_STORE) || stat(r, &st) || !S_ISREG(st.st_mode) || st.st_uid != 0) return -1;
    } else if (!fsops_allowed(r)) return -1;
    const char *dot = strrchr(r, '.');
    char ext[8] = "";
    for (int i = 0; dot && dot[1 + i] && i < 7; i++) { ext[i] = (char)tolower((unsigned char)dot[1 + i]); ext[i + 1] = 0; }
    int vid = is_video_ext(ext);
    if (!vid && strcmp(ext, "jpg") && strcmp(ext, "jpeg") && strcmp(ext, "png") && strcmp(ext, "webp")) return -1;
    if (vid && (wall || cat)) return -1;
    snprintf(real, n, "%s", r);
    *video = vid;
    return 0;
}

static int open_background(const char *real, int video) {
    int fd = open(real, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > (video ? BG_VIDEO_MAX : BG_MAX)) {
        if (fd >= 0) close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    return fd;
}

static int bg_video;                 /* the chosen background is a video */

/* any effect at all (the indicator's "effects on") */
static int effects_on(void) {
    return cfg.mode != 0 || cfg.framing || cfg.light || (cfg.filter && cfg.filter_amt) || cfg.retouch;
}

static void send_config(int sock) {
    int64_t v[6];
    studio_pack_config(cfg.mode, cfg.key, cfg.filter, cfg.mirror, cfg.blur, cfg.similarity, cfg.smoothness, cfg.spill, cfg.framing,
                       cfg.light, cfg.match, cfg.filter_amt, cfg.retouch, v);
    mp_send(sock, MM_STUDIO, 0, v, NULL, 0, NULL, 0);
}

static void send_background(int sock) {
    int fd = cfg.bg[0] ? open_background(cfg.bg, bg_video) : -1;
    mp_send(sock, MM_STUDIO_BG, 0, NULL, NULL, 0, fd >= 0 ? &fd : NULL, fd >= 0 ? 1 : 0);
    if (fd >= 0) close(fd);
}

static void attach(int sock) {
    if (sock < 0) return;
    send_config(sock);
    if (cfg.bg[0]) send_background(sock);
}

/* phase 1: an app's own lord-studio on the real camera */
static int spawn_own(int w, int h, pid_t *pid, const char **code) {
    *code = "studio.unavailable";
    if (!mstudio_available()) return -1;
    char name[64];
    int cam = open_real_camera(name, sizeof name, code);
    if (cam < 0) return -1;
    int model = open_model();
    if (model < 0) { close(cam); *code = "studio.unavailable"; return -1; }
    int face = open_face();
    int render = media_render_fd();
    if (w < 160 || w > 3840) w = 1280;
    if (h < 120 || h > 2160) h = 720;
    char params[160];
    snprintf(params, sizeof params, "width=%d\nheight=%d\nhw=%d\nhwaccel=%s\nrender=%d\nface=%d\n", w, h, render >= 0, media_hwaccel(),
             render >= 0, face >= 0);
    int fds[4], n = 0;
    fds[n++] = cam;
    if (render >= 0) fds[n++] = render;
    fds[n++] = model;
    if (face >= 0) fds[n++] = face;
    int sock = media_spawn_studio(params, fds, n, pid);
    close(cam);
    close(model);
    if (face >= 0) close(face);
    if (render >= 0) close(render);
    if (sock < 0) { *code = "media.noHelper"; return -1; }
    *code = NULL;
    return sock;
}

static MediaSrc *hub_src(void) {
    MediaSrc *s = hub.id ? media_src_get(hub.id) : NULL;
    if (s && s->studio != STUDIO_SRC_HUB) s = NULL;
    if (!s) hub.id = 0;
    return s;
}

/* the hub runs, wrote its first card and LordOS Studio is on: apps read the loopback */
static int hub_serving(void) {
    MediaSrc *s = hub_src();
    return s && s->sock >= 0 && hub.ready && cfg.on;
}

int mstudio_open(void *src, int w, int h, const char **code) {
    MediaSrc *s = src;
    if (hub_serving()) {
        /* phase 2: one more reader of the "LordOS Studio" device, through lord-media like any camera */
        int lfd = loopback_open();
        if (lfd >= 0) {
            int render = media_render_fd();
            char params[96];
            snprintf(params, sizeof params, "width=%d\nheight=%d\nhw=%d\nhwaccel=%s\n", w, h, render >= 0, media_hwaccel());
            int fds[2] = { lfd, render };
            s->sock = media_spawn(MK_CAMERA, params, fds, 2, &s->pid);
            close(lfd);
            if (render >= 0) close(render);
            if (s->sock < 0) { *code = "media.noHelper"; return -1; }
            s->studio = STUDIO_SRC_LOOP;
            hub.tick_at = 0;                        /* the hub hears the new reader; look again soon */
            *code = NULL;
            return 0;
        }
    }
    s->sock = spawn_own(w, h, &s->pid, code);
    if (s->sock < 0) return -1;
    s->studio = STUDIO_SRC_OWN;
    attach(s->sock);
    return 0;
}

/* the sources that run the effects: each own lord-studio and the hub (a loopback reader gets them through the hub) */
static void each_source(void (*fn)(int sock)) {
    for (int i = 0; i < MEDIA_MAX_SRC; i++) {
        MediaSrc *s = media_src_at(i);
        if (s && (s->studio == STUDIO_SRC_OWN || s->studio == STUDIO_SRC_HUB) && s->sock >= 0) fn(s->sock);
    }
}

static int c100(int v) { return v < 0 ? 0 : v > 100 ? 100 : v; }

int mstudio_set(const MStudioSet *c) {
    if (!c || (c->camera && *c->camera && !dev_ok(c->camera))) return -1;
    if (c->camera) snprintf(cfg.camera, sizeof cfg.camera, "%s", c->camera);
    if ((c->on != 0) != cfg.on) { hub.gave_up = 0; memset(hub.spawns, 0, sizeof hub.spawns); }   /* turned on again: tries again */
    cfg.on = c->on != 0;
    cfg.mode = c->mode < 0 || c->mode > 3 ? 0 : c->mode;
    cfg.blur = c100(c->blur);
    cfg.mirror = c->mirror != 0;
    cfg.key = c->key < 0 || c->key > 2 ? 0 : c->key;
    cfg.similarity = c100(c->similarity);
    cfg.smoothness = c100(c->smoothness);
    cfg.spill = c100(c->spill);
    cfg.framing = c100(c->framing);
    cfg.light = c100(c->light);
    cfg.match = c->match < 0 ? 0 : c->match > 60 ? 60 : c->match;
    cfg.filter = c->filter < 0 || c->filter > STUDIO_FILTERS ? 0 : c->filter;
    cfg.filter_amt = c100(c->filter_amt);
    cfg.retouch = c100(c->retouch);
    each_source(send_config);
    hub.tick_at = 0;                                /* on/off: the hub follows at the next turn of the loop */
    return 0;
}

int mstudio_set_background(const char *path) {
    if (!path || !*path) {
        cfg.bg[0] = cfg.bg_name[0] = 0;
        bg_video = 0;
        each_source(send_background);
        return 0;
    }
    char real[PATH_MAX];
    int video = 0;
    if (check_background(path, real, sizeof real, &video)) return -1;
    int fd = open_background(real, video);
    if (fd < 0) return -1;
    close(fd);
    snprintf(cfg.bg, sizeof cfg.bg, "%s", real);
    snprintf(cfg.bg_name, sizeof cfg.bg_name, "%s", path);
    bg_video = video;
    each_source(send_background);
    return 0;
}

void mstudio_stats(int src_id, const int64_t v[6]) {
    long long frames = v[0], seg_us = v[1], fx_us = v[2], gpu = v[3], frame_us = v[4], vid_us = v[5];
    if (frames < 0 || frames > (1LL << 40) || seg_us < 0 || seg_us > 10000000 || fx_us < 0 || fx_us > 10000000 || (gpu != 0 && gpu != 1) ||
        frame_us < 0 || frame_us > 10000000 || vid_us < 0 || vid_us > 10000000)
        return;
    double t = now_s();
    if (stats.at > 0 && t > stats.at && frames >= stats.at_frames) stats.fps = (double)(frames - stats.at_frames) / (t - stats.at);
    stats.at = t;
    stats.at_frames = frames;
    stats.frames = frames;
    stats.seg_us = seg_us;
    stats.fx_us = fx_us;
    stats.frame_us = frame_us;
    stats.vid_us = vid_us;
    stats.gpu = (int)gpu;
    media_event(src_id, "studio", "{\"fps\":%.1f,\"segMs\":%.2f,\"fxMs\":%.2f,\"frameMs\":%.2f,\"videoMs\":%.2f,\"gpu\":%s}", stats.fps,
                seg_us / 1000.0, fx_us / 1000.0, frame_us / 1000.0, vid_us / 1000.0, gpu ? "true" : "false");
}

/* ---- the hub -------------------------------------------------------------------------------------------------- */

void mstudio_on_use(void (*emit)(const char *kind, const char *json)) { use_emit = emit; }

static void use_send(const char *json) {
    if (!strcmp(json, hub.use_last)) return;
    snprintf(hub.use_last, sizeof hub.use_last, "%s", json);
    if (use_emit) use_emit("studio-use", json);
}

static void hub_reset(void) {
    hub.id = 0;
    hub.ready = 0;
    hub.events = 0;
    hub.readers = -1;
    hub.scanned = 0;
    hub.cam_open = hub.shows = 0;
    hub.sent_show = -1;
    hub.sent_preview = 0;
    if (hub.use_last[0]) use_send("{\"apps\":[],\"camera\":\"\",\"effects\":false}");
}

void mstudio_readers(int src_id, long long readers, long long cam_open, long long what) {
    MediaSrc *s = hub_src();
    if (!s || s->id != src_id) return;
    if (readers < -1 || readers > 1024 || (cam_open != 0 && cam_open != 1) || what < 0 || what > STUDIO_SHOW_OFF) return;
    if (!hub.ready) hub.events = readers >= 0;
    hub.ready = 1;
    hub.readers = (int)readers;
    hub.cam_open = (int)cam_open;
    hub.shows = (int)what;
    hub.tick_at = 0;                                /* decide now: a reader came or went */
}

static MediaSrc *hub_spawn(double t) {
    /* at most 3 starts a minute; after that LordOS Studio's camera for apps stays off until the settings change */
    if (hub.gave_up) return NULL;
    double oldest = hub.spawns[hub.spawn_i];
    if (oldest > 0 && t - oldest < 60) {
        hub.gave_up = 1;
        fprintf(stderr, "studio: lord-studio for the loopback stopped 3 times in a minute; LordOS Studio's camera for apps is off\n");
        return NULL;
    }
    int lfd = loopback_open();
    if (lfd < 0) return NULL;
    int model = open_model();
    if (model < 0) { close(lfd); return NULL; }
    MediaSrc *s = media_src_new(SK_CAMERA, STUDIO_DEVICE);
    if (!s) { close(lfd); close(model); return NULL; }
    int render = media_render_fd();
    int w = 1280, h = 720;
#if defined(__aarch64__)
    if (render < 0) { w = 640; h = 360; }           /* a Raspberry Pi compositing on its CPU (plan 3.3) */
#endif
    int face = open_face();
    char params[160];
    snprintf(params, sizeof params, "loopback=1\nwidth=%d\nheight=%d\nhw=0\nrender=%d\nface=%d\n", w, h, render >= 0, face >= 0);
    int fds[4], n = 0;
    if (render >= 0) fds[n++] = render;
    fds[n++] = model;
    if (face >= 0) fds[n++] = face;
    fds[n++] = lfd;
    s->sock = media_spawn_studio(params, fds, n, &s->pid);
    close(lfd);
    close(model);
    if (face >= 0) close(face);
    if (render >= 0) close(render);
    if (s->sock < 0) { media_src_close(s); return NULL; }
    s->studio = STUDIO_SRC_HUB;
    hub.spawns[hub.spawn_i] = t;
    hub.spawn_i = (hub.spawn_i + 1) % 3;
    hub_reset();
    hub.id = s->id;
    attach(s->sock);
    return s;
}

static void hub_stop(MediaSrc *s) {
    media_src_close(s);
    hub_reset();
}

/* what the loopback should show now, and the camera for it */
static void hub_drive(MediaSrc *s, double t) {
    int preview = hub.preview && !restrict_blocks("lordos", FW_R_NO_CAMERA);
    int show = studio_hub_show(cfg.on, hub.readers, hub.scanned, preview);
    /* asked for the camera but the hub couldn't open it (another app holds it): again every 3 s */
    int retry = show == STUDIO_SHOW_CAMERA && hub.sent_show == STUDIO_SHOW_CAMERA && !hub.cam_open && t - hub.sent_at > 3;
    if (show == hub.sent_show && preview == hub.sent_preview && !retry) return;
    int fd = -1;
    if (show == STUDIO_SHOW_CAMERA && (hub.sent_show != STUDIO_SHOW_CAMERA || retry)) {
        const char *code = NULL;
        fd = open_real_camera(hub.camera_name, sizeof hub.camera_name, &code);
        if (fd < 0) { show = STUDIO_SHOW_IDLE; hub.camera_name[0] = 0; }
    }
    int64_t v[6] = { show, preview, 0, 0, 0, 0 };
    mp_send(s->sock, MM_STUDIO_CAM, 0, v, NULL, 0, fd >= 0 ? &fd : NULL, fd >= 0 ? 1 : 0);
    if (fd >= 0) close(fd);
    hub.sent_show = show;
    hub.sent_preview = preview;
    hub.sent_at = t;
}

/* who has the loopback open, for the indicator (and the count when the module sends no event) */
static void hub_scan(void) {
    pid_t pids[HUB_MAX_READERS];
    int n = studio_scan_readers("/proc", hub.rdev, getpid(), getuid(), pids, HUB_MAX_READERS);
    hub.scanned = n;
    if (n > HUB_MAX_READERS) n = HUB_MAX_READERS;
    static char apps[HUB_MAX_READERS][FW_APP_MAX];
    for (int i = 0; i < n; i++) if (appid_of_pid((unsigned)pids[i], apps[i], sizeof apps[i])) apps[i][0] = 0;
    char json[sizeof hub.use_last];
    if (studio_use_format(&apps[0][0], FW_APP_MAX, n, hub.camera_name, effects_on(), json, sizeof json))
        snprintf(json, sizeof json, "{\"apps\":[\"\"],\"camera\":\"\",\"effects\":%s}", effects_on() ? "true" : "false");
    use_send(json);
}

static int hub_off;
void mstudio_hub_off(void) { hub_off = 1; }

void mstudio_tick(void) {
    if (hub_off) return;
    double t = now_s();
    if (t < hub.tick_at) return;
    hub.tick_at = t + 1.0;
    MediaSrc *s = hub_src();
    if (s && s->sock < 0) { media_src_close(s); hub_reset(); s = NULL; }        /* it ended: started again below */
    if (!s && t >= hub.device_at) {
        int fd = loopback_open();
        hub.device = fd >= 0;
        if (fd >= 0) close(fd);
        hub.device_at = t + 5;
    }
    int device = s ? 1 : hub.device;
    /* the readers: named (the indicator) while the hub says somebody streams, or always when it can't say */
    if (s && hub.ready && (hub.readers != 0 || !hub.events)) {
        if (t >= hub.scan_at) { hub_scan(); hub.scan_at = t + 2; }
    } else {
        hub.scanned = 0;
        if (hub.use_last[0]) use_send("{\"apps\":[],\"camera\":\"\",\"effects\":false}");
    }
    int want = device && studio_hub_wanted(cfg.on, mstudio_available(), device, hub.readers, hub.scanned);
    if (!s && want) s = hub_spawn(t);
    else if (s && !want) { hub_stop(s); s = NULL; }
    if (s && hub.ready && s->sock >= 0) hub_drive(s, t);
}

/* only while the hub runs: without it nothing here needs the loop to wake (a newly loaded module is seen at the next
 * turn of the loop for any other reason, at most every 5 s) */
double mstudio_timeout(void) {
    if (!hub_src()) return -1;
    double left = (hub.tick_at - now_s()) * 1000;
    return left < 0 ? 0 : left;
}

int mstudio_preview_hub(void) {
    if (!hub_serving()) return 0;
    hub.preview = 1;
    hub.tick_at = 0;
    mstudio_tick();
    return hub.id;
}

void mstudio_preview_end(void) {
    hub.preview = 0;
    hub.tick_at = 0;
}

int mstudio_is_hub(int src_id) { return src_id > 0 && hub.id == src_id && hub_src() != NULL; }

void mstudio_status(char *json, size_t n) {
    static const char *const modes[] = { "none", "blur", "picture", "video" };
    static const char *const keys[] = { "none", "green", "blue" };
    static const char *const shows[] = { "idle", "camera", "off" };
    char bg[PATH_MAX * 2 + 8], cam[80], camname[160];
    media_json_str(bg, sizeof bg, cfg.bg_name);
    media_json_str(cam, sizeof cam, cfg.camera);
    media_json_str(camname, sizeof camname, hub.camera_name);
    MediaSrc *h = hub_src();
    /* the apps reading the loopback, as the indicator names them (use_last's list, which studio_use_format made) */
    char apps[sizeof hub.use_last] = "[]";
    const char *a = strstr(hub.use_last, "\"apps\":["), *e = a ? strchr(a, ']') : NULL;
    if (a && e) snprintf(apps, sizeof apps, "%.*s", (int)(e - (a + 7)) + 1, a + 7);
    /* loopback: 'missing' (no device: the package isn't installed or its module isn't loaded), 'ready', 'running' */
    const char *lb = h && hub.ready ? "running" : (h || hub.device) ? "ready" : "missing";
    if (face_state == 0) { int fd = open_face(); if (fd >= 0) close(fd); }
    size_t o = (size_t)snprintf(json, n, "{\"available\":%s,\"on\":%s,\"camera\":%s,\"mode\":\"%s\",\"blur\":%d,\"mirror\":%s,\"background\":%s,"
                                "\"video\":%s,\"key\":\"%s\",\"similarity\":%d,\"smoothness\":%d,\"spill\":%d,\"framing\":%d,\"light\":%d,"
                                "\"match\":%d,\"filter\":\"%s\",\"filterStrength\":%d,\"retouch\":%d,\"faces\":%s,"
                                "\"fps\":%.1f,\"segMs\":%.2f,\"fxMs\":%.2f,\"frameMs\":%.2f,\"videoMs\":%.2f,\"gpu\":%s,"
                                "\"loopback\":\"%s\",\"hub\":%d,\"readers\":%d,\"cameraOpen\":%s,\"shows\":\"%s\",\"cameraName\":%s,\"stopped\":%s,"
                                "\"apps\":%s,\"sources\":[",
                                mstudio_available() ? "true" : "false", cfg.on ? "true" : "false", cam, modes[cfg.mode], cfg.blur,
                                cfg.mirror ? "true" : "false", bg, bg_video ? "true" : "false", keys[cfg.key], cfg.similarity, cfg.smoothness,
                                cfg.spill, cfg.framing, cfg.light, cfg.match, studio_filter_name(cfg.filter), cfg.filter_amt, cfg.retouch,
                                face_state == 1 ? "true" : "false",
                                stats.fps, stats.seg_us / 1000.0, stats.fx_us / 1000.0, stats.frame_us / 1000.0, stats.vid_us / 1000.0, stats.gpu ? "true" : "false",
                                lb, h ? h->id : 0, hub.readers < 0 ? hub.scanned : hub.readers, hub.cam_open ? "true" : "false",
                                shows[hub.shows < 0 || hub.shows > 2 ? 0 : hub.shows], camname, hub.gave_up ? "true" : "false", apps);
    int first = 1;
    for (int i = 0; i < MEDIA_MAX_SRC && o + 160 < n; i++) {
        MediaSrc *s = media_src_at(i);
        if (!s || !s->studio || s->studio == STUDIO_SRC_HUB || s->sock < 0) continue;
        char app[MEDIA_APP_MAX * 2 + 8];
        media_json_str(app, sizeof app, s->app);
        o += (size_t)snprintf(json + o, n - o, "%s{\"id\":%d,\"app\":%s,\"isolated\":%s}", first ? "" : ",", s->id, app, s->bk ? "true" : "false");
        first = 0;
    }
    if (o + 3 < n) snprintf(json + o, n - o, "]}");
}
