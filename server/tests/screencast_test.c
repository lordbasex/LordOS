/* screencast_test.c — lord-screencast against a real PipeWire daemon (a private one, tests/screencast_test.sh), with
 * this program as lord-server and as the app:
 *   - the helper starts from its path with only LANG and fd 3, gets its connection and a ring, and makes a node, one
 *     the PipeWire guard tells from a camera (media.role Screen: hidden from apps with no screen capture);
 *   - the app's connection comes back restricted: from it the registry shows the stream's node and nothing else of
 *     the daemon, and the app can still make its own stream there and receive;
 *   - frames written into the ring reach the app pixel for pixel, in BGRx, each slot given back (CM_DONE), and each in a
 *     memfd buffer of its own that the app maps itself, as Chromium and OBS do (they drop any other kind);
 *   - a resize: the stream again at another size, frames at the new size;
 *   - CM_STOP ends it; a message lord-server may not send ends it too.
 * With "bench": 1920x1080 at 30 frames a second for 5 seconds, every frame different, and what that costs: the copy
 * into the ring here (lord-server's part after painting) and the helper's CPU time. */
#include "cast/castproto.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/buffers.h>
#include <spa/buffer/meta.h>

static int ok, bad;
#define CHECK(c) do { if (c) ok++; else { bad++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const char *sock_path;
static int chan = -1;
static pid_t helper = -1;
static struct pw_main_loop *ml;
static struct pw_loop *lp;
static struct pw_context *ctx;

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static int pw_connect(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", sock_path);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a)) { if (fd >= 0) close(fd); return -1; }
    return fd;
}

static pid_t spawn(const char *path, int fd) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fd, 3);
    posix_spawn_file_actions_addclosefrom_np(&fa, 4);
    char *argv[] = { "lord-screencast", NULL }, *envp[] = { "LANG=C.UTF-8", NULL };
    pid_t pid = -1;
    if (posix_spawn(&pid, path, &fa, NULL, argv, envp)) pid = -1;
    posix_spawn_file_actions_destroy(&fa);
    return pid;
}

/* ---- the ring, as lord-server keeps it ---- */
typedef struct { int fd, ro; uint8_t *map; CastLayout l; uint32_t w, h; } Ring;

static int ring_new(Ring *r, uint32_t w, uint32_t h) {
    memset(r, 0, sizeof *r);
    if (cast_layout(w, h, &r->l)) return -1;
    r->fd = memfd_create("lordos-cast", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (r->fd < 0 || ftruncate(r->fd, (off_t)r->l.total)) return -1;
    fcntl(r->fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL);
    r->map = mmap(NULL, (size_t)r->l.total, PROT_READ | PROT_WRITE, MAP_SHARED, r->fd, 0);
    if (r->map == MAP_FAILED) return -1;
    CastRingHdr hd = { CAST_RING_MAGIC, w, h, r->l.stride, CAST_SLOTS, 0, r->l.slot_bytes };
    memcpy(r->map, &hd, sizeof hd);
    char p[64];
    snprintf(p, sizeof p, "/proc/self/fd/%d", r->fd);
    r->ro = open(p, O_RDONLY | O_CLOEXEC);      /* the helper's copy can't write */
    r->w = w;
    r->h = h;
    return r->ro < 0 ? -1 : 0;
}
static void ring_free(Ring *r) {
    if (r->map && r->map != MAP_FAILED) munmap(r->map, (size_t)r->l.total);
    if (r->fd >= 0) close(r->fd);
    if (r->ro >= 0) close(r->ro);
    memset(r, 0, sizeof *r);
}

/* pixel (x, y) of frame k: what both the slot and the app's buffer must hold */
static uint32_t px_of(uint32_t k, uint32_t x, uint32_t y) { return 0xff000000u | ((x * 7 + k * 13) & 0xff) << 16 | ((y * 3 + k) & 0xff) << 8 | (k * 29 & 0xff); }

/* the frame is made in a canvas of its own, as lord-server paints the source; only the copy into the slot is timed
 * (the part sharing adds after the painting) */
static double copy_ms;
static uint32_t *canvas;
static size_t canvas_n;
static void fill(Ring *r, int slot, uint32_t k) {
    size_t n = (size_t)r->w * r->h;
    if (n > canvas_n) { free(canvas); canvas = malloc(n * 4); canvas_n = canvas ? n : 0; }
    if (!canvas) return;
    for (uint32_t y = 0; y < r->h; y++) for (uint32_t x = 0; x < r->w; x++) canvas[(size_t)y * r->w + x] = px_of(k, x, y);
    double t0 = now_ms();
    memcpy(r->map + cast_slot_off(&r->l, (uint32_t)slot), canvas, n * 4);
    copy_ms += now_ms() - t0;
}

/* ---- lord-server's side of the channel ---- */
static CastPeer peer;
static uint32_t node_id, remote_req;
static int want_on, remote_fd = -1, ended, got_fail, bad_msgs;
static uint32_t want_num, want_den;

static void on_chan(void *data, int fd, uint32_t mask) {
    (void)data; (void)fd;
    if (mask & (SPA_IO_ERR | SPA_IO_HUP)) { ended = 1; return; }
    for (;;) {
        CastMsg m;
        int f = -1, r = cast_recv(chan, &m, &f);
        if (r == 0) return;
        if (r < 0) { ended = 1; return; }
        if (cast_check_helper(&peer, &m, f >= 0)) { bad_msgs++; if (f >= 0) close(f); continue; }
        if (m.type == CM_NODE) node_id = m.a;
        else if (m.type == CM_WANT) { want_on = (int)m.a; want_num = m.b; want_den = m.c; }
        else if (m.type == CM_REMOTE) { remote_req = m.a; remote_fd = f; }
        else if (m.type == CM_FAIL) got_fail = (int)m.a;
    }
}

static int run_until(int *flag, double ms) {
    double end = now_ms() + ms;
    while (!*flag && now_ms() < end) pw_loop_iterate(lp, 20);
    return *flag;
}
static int node_seen(void) { return node_id != 0; }
static int remote_seen(void) { return remote_req != 0; }

/* ---- the app ---- */
typedef struct {
    struct pw_core *core;
    struct pw_registry *reg;
    struct spa_hook ch, rh, sh;
    struct pw_stream *st;
    int globals, others, factories, saw_node, synced, sync_seq, frames, wrong, not_memfd, fmt_ok;
    uint32_t w, h, last_k, node;
} App;
static App app;

static void reg_global(void *data, uint32_t id, uint32_t perms, const char *type, uint32_t version, const struct spa_dict *props) {
    App *a = data;
    (void)perms; (void)version; (void)props;
    a->globals++;
    if (!strcmp(type, PW_TYPE_INTERFACE_Factory)) a->factories++;
    if (id == node_id) a->saw_node = 1;
    else if (id != PW_ID_CORE && strcmp(type, PW_TYPE_INTERFACE_Factory) && !(a->st && id == pw_stream_get_node_id(a->st))) {
        a->others++;
        printf("  the app sees %u %s\n", id, type);
    }
}
static const struct pw_registry_events reg_events = { PW_VERSION_REGISTRY_EVENTS, .global = reg_global };
static void app_done(void *data, uint32_t id, int seq) { App *a = data; if (id == PW_ID_CORE && seq == a->sync_seq) a->synced = 1; }
static const struct pw_core_events app_core_events = { PW_VERSION_CORE_EVENTS, .done = app_done };

/* The app takes buffers the way the screen cast consumers do (Chromium's WebRTC capturer, OBS): it asks for MemFd (or
 * DmaBuf) buffers, maps each buffer's fd itself (no PW_STREAM_FLAG_MAP_BUFFERS) and has no use for any other kind. A
 * buffer that comes without a memfd counts as not received (not_memfd): WebRTC drops those without a word. */
static void app_param(void *data, uint32_t id, const struct spa_pod *param) {
    App *a = data;
    if (id != SPA_PARAM_Format || !param) return;
    struct spa_video_info_raw info;
    if (spa_format_video_raw_parse(param, &info) < 0) return;
    a->fmt_ok = info.format == SPA_VIDEO_FORMAT_BGRx;
    a->w = info.size.width;
    a->h = info.size.height;
    uint8_t buf[512];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
    const struct spa_pod *p[2];
    p[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_DmaBuf)));
    p[1] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header), SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
    pw_stream_update_params(a->st, p, 2);
}
static void app_process(void *data) {
    App *a = data;
    struct pw_buffer *b;
    while ((b = pw_stream_dequeue_buffer(a->st))) {
        struct spa_data *d = &b->buffer->datas[0];
        if (d->type != SPA_DATA_MemFd || d->fd < 0) { a->not_memfd++; pw_stream_queue_buffer(a->st, b); continue; }
        size_t len = (size_t)d->mapoffset + d->maxsize;
        uint8_t *map = mmap(NULL, len, PROT_READ, MAP_SHARED, (int)d->fd, 0);
        if (map == MAP_FAILED) { a->wrong++; pw_stream_queue_buffer(a->st, b); continue; }
        if (a->w && d->chunk->size >= a->w * 4 * a->h && (size_t)d->chunk->offset + d->chunk->size <= d->maxsize) {
            const uint32_t *p = (const uint32_t *)(map + d->mapoffset + d->chunk->offset);
            /* which frame: from pixel (0, 0)'s blue byte, then every pixel against it */
            uint32_t k = 0;
            while (k < 256 && (px_of(k, 0, 0) & 0xff) != (p[0] & 0xff)) k++;
            int same = 1;
            for (uint32_t y = 0; y < a->h && same; y += 7) for (uint32_t x = 0; x < a->w; x += 5)
                if ((p[(size_t)y * (d->chunk->stride / 4) + x] & 0xffffff) != (px_of(k, x, y) & 0xffffff)) { same = 0; break; }
            if (same) { a->frames++; a->last_k = k; } else a->wrong++;
        }
        munmap(map, len);
        pw_stream_queue_buffer(a->st, b);
    }
}
static const struct pw_stream_events app_stream_events = { PW_VERSION_STREAM_EVENTS, .param_changed = app_param, .process = app_process };

/* the session manager's job here: a link from the screen's node to the app's */
static struct pw_core *mgr;
static struct spa_hook mgr_hook;
static void mgr_error(void *data, uint32_t id, int seq, int res, const char *msg) {
    (void)data; (void)seq;
    /* "unknown input port" while the app's ports are still coming: the link is tried again */
    if (getenv("CAST_DEBUG")) printf("  the manager's connection: %u: %s (%d)\n", id, msg ? msg : "", res);
}
static const struct pw_core_events mgr_events = { PW_VERSION_CORE_EVENTS, .error = mgr_error };
/* what the PipeWire guard reads of the node (sec/pwguard.c, pwguard_node_flag): a screen, hidden from no-screen apps */
static struct pw_registry *mgr_reg;
static struct spa_hook mgr_reg_hook;
static int node_is_screen;
static void mgr_global(void *data, uint32_t id, uint32_t perms, const char *type, uint32_t version, const struct spa_dict *props) {
    (void)data; (void)perms; (void)version;
    if (strcmp(type, PW_TYPE_INTERFACE_Node) || !props) return;
    if (getenv("CAST_DEBUG")) for (uint32_t k = 0; k < props->n_items; k++) printf("  node %u: %s = %s\n", id, props->items[k].key, props->items[k].value);
    const char *mc = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS), *role = spa_dict_lookup(props, PW_KEY_MEDIA_ROLE),
               *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (mc && !strcmp(mc, "Video/Source") && role && !strcmp(role, "Screen") && name && !strncmp(name, "lordos-screencast-", 18)) node_is_screen = (int)id;
}
static const struct pw_registry_events mgr_reg_events = { PW_VERSION_REGISTRY_EVENTS, .global = mgr_global };
static void link_nodes(uint32_t out, uint32_t in) {
    char o[16], i[16];
    snprintf(o, sizeof o, "%u", out);
    snprintf(i, sizeof i, "%u", in);
    struct pw_properties *p = pw_properties_new(PW_KEY_LINK_OUTPUT_NODE, o, PW_KEY_LINK_INPUT_NODE, i, PW_KEY_OBJECT_LINGER, "true", NULL);
    pw_core_create_object(mgr, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &p->dict, 0);
    pw_properties_free(p);
}

static int app_connect(int fd) {
    memset(&app, 0, sizeof app);
    app.core = pw_context_connect_fd(ctx, fd, NULL, 0);
    if (!app.core) return -1;
    pw_core_add_listener(app.core, &app.ch, &app_core_events, &app);
    app.reg = pw_core_get_registry(app.core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(app.reg, &app.rh, &reg_events, &app);
    app.sync_seq = pw_core_sync(app.core, PW_ID_CORE, 0);
    run_until(&app.synced, 3000);
    return 0;
}

static int app_stream(void) {
    if (!app.core) return -1;
    app.st = pw_stream_new(app.core, "app", pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", NULL));
    if (!app.st) return -1;
    pw_stream_add_listener(app.st, &app.sh, &app_stream_events, &app);
    uint8_t buf[512];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
    const struct spa_pod *p[1];
    p[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_BGRx));
    if (pw_stream_connect(app.st, PW_DIRECTION_INPUT, node_id, 0, p, 1) < 0) return -1;
    double end = now_ms() + 3000;
    while (pw_stream_get_state(app.st, NULL) != PW_STREAM_STATE_PAUSED && now_ms() < end) pw_loop_iterate(lp, 20);
    printf("  the app's stream: %s\n", pw_stream_state_as_string(pw_stream_get_state(app.st, NULL)));
    app.node = pw_stream_get_node_id(app.st);
    return app.node == PW_ID_ANY ? -1 : 0;
}

static void send_msg(int type, int stream, uint32_t a, uint32_t w, uint32_t h, uint64_t seq, int fd) {
    CastMsg m = { .type = (uint16_t)type, .stream = (uint16_t)stream, .a = a, .b = CAST_SRC_MONITOR, .w = w, .h = h, .seq = seq };
    if (cast_send(chan, &m, fd)) { bad++; printf("FAIL: send %d: %s\n", type, strerror(errno)); }
}

/* frames for ms: one whenever a slot is free and the period passed; how many went */
static int frames_for(Ring *r, double ms, uint32_t *k) {
    double end = now_ms() + ms, next = now_ms();
    int sent = 0;
    while (now_ms() < end && !ended) {
        if (want_on && now_ms() >= next) {
            int s = cast_slot_take(&peer, 0);
            if (s >= 0) {
                fill(r, s, *k);
                send_msg(CM_FRAME, 0, (uint32_t)s, r->w, r->h, *k, -1);
                (*k)++;
                sent++;
                next += cast_period_ms(want_num, want_den);
                if (next < now_ms() - 100) next = now_ms();
            }
        }
        pw_loop_iterate(lp, 2);
    }
    return sent;
}

static double cpu_ms(pid_t pid) {
    char p[64], b[1024];
    snprintf(p, sizeof p, "/proc/%d/stat", (int)pid);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = 0;
    char *c = strrchr(b, ')');
    unsigned long ut = 0, st = 0;
    if (!c || sscanf(c + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) != 2) return -1;
    return (double)(ut + st) * 1000.0 / (double)sysconf(_SC_CLK_TCK);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: screencast-test <lord-screencast> <pipewire socket> [bench]\n"); return 2; }
    sock_path = argv[2];
    setvbuf(stdout, NULL, _IONBF, 0);
    int bench = argc > 3 && !strcmp(argv[3], "bench");
    signal(SIGPIPE, SIG_IGN);
    pw_init(NULL, NULL);
    ml = pw_main_loop_new(NULL);
    lp = pw_main_loop_get_loop(ml);
    ctx = pw_context_new(lp, pw_properties_new("module.rt", "false", "support.dbus", "false", NULL), 0);
    mgr = pw_context_connect(ctx, pw_properties_new(PW_KEY_REMOTE_NAME, sock_path, NULL), 0);
    CHECK(ctx && mgr);
    if (mgr) {
        pw_core_add_listener(mgr, &mgr_hook, &mgr_events, NULL);
        mgr_reg = pw_core_get_registry(mgr, PW_VERSION_REGISTRY, 0);
        pw_registry_add_listener(mgr_reg, &mgr_reg_hook, &mgr_reg_events, NULL);
    }

    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) == 0);
    chan = sv[0];
    helper = spawn(argv[1], sv[1]);
    close(sv[1]);
    CHECK(helper > 0);
    pw_loop_add_io(lp, chan, SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP, false, on_chan, NULL);

    uint32_t W = bench ? 1920 : 320, H = bench ? 1080 : 240;
    int pwfd = pw_connect();
    CHECK(pwfd >= 0);
    peer.nstreams = 1;
    send_msg(CM_HELLO, 0, 1, 0, 0, 0, pwfd);
    close(pwfd);
    Ring ring;
    CHECK(ring_new(&ring, W, H) == 0);
    CastMsg sm = { .type = CM_STREAM, .stream = 0, .a = 30, .b = CAST_SRC_MONITOR, .w = W, .h = H };
    CHECK(cast_send(chan, &sm, ring.ro) == 0);
    CHECK(run_until(&ended, 0) == 0);
    {
        double end = now_ms() + 5000;
        while (!node_seen() && !ended && now_ms() < end) pw_loop_iterate(lp, 20);
    }
    CHECK(node_id != 0 && !got_fail);
    printf("  node %u\n", node_id);
    for (int i = 0; i < 20 && node_is_screen != (int)node_id; i++) pw_loop_iterate(lp, 20);
    CHECK(node_is_screen == (int)node_id);      /* a Video/Source with media.role Screen, named lordos-screencast-0 */

    /* the app's connection, restricted to the stream */
    int cfd = pw_connect();
    CHECK(cast_remote_add(&peer, 7) == 0);
    send_msg(CM_CLIENT, 0, 7, 0, 0, 0, cfd);
    close(cfd);
    {
        double end = now_ms() + 5000;
        while (!remote_seen() && !ended && now_ms() < end) pw_loop_iterate(lp, 20);
    }
    CHECK(remote_req == 7 && remote_fd >= 0);
    if (remote_fd >= 0) CHECK(app_connect(remote_fd) == 0);
    remote_fd = -1;
    CHECK(app.synced && app.saw_node);
    CHECK(app.others == 0 && app.factories == 1);  /* nothing of the daemon's but the stream and the one factory */
    if (app_stream()) { CHECK(0); goto out; }
    printf("  the app's stream: node %u\n", app.node);
    /* the ports come a moment after the node: the manager tries again until the stream flows */
    for (int tries = 0; tries < 10 && !want_on && !ended; tries++) {
        link_nodes(node_id, app.node);
        double end = now_ms() + 500;
        while (!want_on && !ended && now_ms() < end) pw_loop_iterate(lp, 20);
    }
    CHECK(want_on == 1);
    CHECK(want_den != 0 || want_num == 0);
    uint32_t k = 1;
    if (!bench) {
        int sent = frames_for(&ring, 1500, &k);
        CHECK(app.fmt_ok && app.w == W && app.h == H);
        printf("  %d frames sent, %d received whole in their own memfd, %d wrong, %d without a memfd, last %u\n", sent, app.frames,
               app.wrong, app.not_memfd, app.last_k);
        /* in 1.5 s at 30 a second: most of them reach the app, every one in a memfd buffer of the right size */
        CHECK(sent > 10 && app.frames >= 10 && app.wrong == 0 && app.not_memfd == 0);
        for (int i = 0; i < CAST_SLOTS; i++) if (peer.busy[0][i]) { pw_loop_iterate(lp, 50); }
        int held = 0;
        for (int i = 0; i < CAST_SLOTS; i++) held += peer.busy[0][i];
        CHECK(held == 0);                       /* every slot came back */
        char tp[48];
        snprintf(tp, sizeof tp, "/proc/%d/task", (int)helper);
        int threads = 0;
        DIR *td = opendir(tp);
        for (struct dirent *e; td && (e = readdir(td));) threads += e->d_name[0] != '.';
        if (td) closedir(td);
        printf("  the helper runs %d thread%s while streaming\n", threads, threads == 1 ? "" : "s");
        CHECK(threads == 1);                    /* PipeWire's work in its one sandboxed thread */

        /* a resize: another ring, the stream renegotiates, frames at the new size */
        Ring r2;
        CHECK(ring_new(&r2, 400, 300) == 0);
        CastMsg rs = { .type = CM_STREAM, .stream = 0, .a = 30, .b = CAST_SRC_WINDOW, .w = 400, .h = 300 };
        CHECK(cast_send(chan, &rs, r2.ro) == 0);
        int before = app.frames;
        frames_for(&r2, 1500, &k);
        printf("  after the resize: %ux%u, %d more frames, %d wrong, %d without a memfd\n", app.w, app.h, app.frames - before, app.wrong,
               app.not_memfd);
        CHECK(app.w == 400 && app.h == 300 && app.frames > before + 3 && app.wrong == 0 && app.not_memfd == 0);
        ring_free(&r2);
        CHECK(bad_msgs == 0);

        /* a frame of a size lord-server never gave: the helper ends */
        CastMsg wrong = { .type = CM_FRAME, .stream = 0, .a = 0, .w = 401, .h = 300 };
        cast_send(chan, &wrong, -1);
        run_until(&ended, 3000);
        CHECK(ended);
    } else {
        double c0 = cpu_ms(helper);
        copy_ms = 0;
        int before = app.frames;
        int sent = frames_for(&ring, 5000, &k);
        double c1 = cpu_ms(helper);
        printf("bench: 1920x1080, %d frames in 5 s (%.1f a second), %d received\n", sent, sent / 5.0, app.frames - before);
        printf("bench: lord-server's copy into the ring %.2f ms a frame; lord-screencast %.1f %% of one core (%.2f ms a frame)\n",
               copy_ms / (sent ? sent : 1), (c1 - c0) / 50.0, (c1 - c0) / (sent ? sent : 1));
        CHECK(sent >= 140 && app.frames - before >= 120 && app.wrong == 0 && app.not_memfd == 0);
        send_msg(CM_STOP, 0, 0, 0, 0, 0, -1);
        run_until(&ended, 3000);
    }
out:;
    int st = 0;
    kill(helper, SIGTERM);
    for (int i = 0; i < 100 && waitpid(helper, &st, WNOHANG) == 0; i++) usleep(20000);
    if (waitpid(helper, &st, WNOHANG) == 0) { kill(helper, SIGKILL); waitpid(helper, &st, 0); CHECK(0); }
    ring_free(&ring);
    free(canvas);
    printf("screencast-test: %d/%d\n", ok, ok + bad);
    return bad ? 1 : 0;
}
