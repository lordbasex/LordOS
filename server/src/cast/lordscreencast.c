/* lordscreencast.c — lord-screencast: the PipeWire video source of a shared screen or window, sandboxed (see
 * castproto.h and docs/WAYLAND.md, "Sharing a screen or a window"). The only binary that links libpipewire-0.3.
 *
 * lord-server starts one per ScreenCast session, from a fixed path next to itself, with no arguments and only LANG,
 * the channel on fd 3. CM_HELLO brings the PipeWire connection lord-server opened to the user's daemon: PipeWire is
 * set up with no real-time module and no D-Bus, then sandbox_apply_helper locks the process in (no socket, connect,
 * bind or exec; only /usr to read) before the connection says a word. From then on:
 *   - each CM_STREAM is a Video/Source node of that size (BGRx or RGBx, its own memfd buffers), driven by us: a frame
 *     goes out only when lord-server sends one, which it does only when the source changed;
 *   - once its nodes exist, this connection lowers its own permissions to reading, but for its nodes: whatever the
 *     consumers send later can't make anything new in the daemon (a client can only lower its own permissions);
 *   - CM_CLIENT is a second connection, the app's: restricted the same way, to the streams only, and handed back.
 * Nothing here decides what is shared, and nothing from PipeWire goes back to lord-server but node ids, rates and
 * freed slots, which it checks (cast_check_helper). */
#include "castproto.h"
#include "../sec/sandbox.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/buffers.h>
#include <spa/buffer/meta.h>

#define CHAN 3

typedef struct {
    struct pw_stream *st;
    struct spa_hook hook;
    int idx, kind;
    uint32_t w, h, fps;
    const uint8_t *ring;
    size_t ring_len;
    CastLayout lay;
    uint32_t fmt;                       /* negotiated SPA_VIDEO_FORMAT_*, 0 none */
    uint32_t fw, fh;                    /* negotiated size */
    int node_sent, streaming;
    uint64_t seq;
} Stream;

typedef struct {
    struct pw_core *core;
    struct spa_hook hook;
    uint32_t req;
    int seq;
} Remote;

static struct pw_main_loop *loop;
static struct pw_context *ctx;
static struct pw_core *core;
static struct spa_hook core_hook;
static Stream S[CAST_MAX_STREAMS];
static Remote R[CAST_MAX_REMOTES];
static CastSide side;
static int restricted;

static void say(int type, int stream, uint32_t a, uint32_t b, uint32_t c) {
    CastMsg m = { .type = (uint16_t)type, .stream = (uint16_t)stream, .a = a, .b = b, .c = c };
    if (cast_send(CHAN, &m, -1)) pw_main_loop_quit(loop);
}

static void fail(int code) {
    say(CM_FAIL, 0, (uint32_t)code, 0, 0);
    pw_main_loop_quit(loop);
}

/* The client-node factory, found in the registry before this connection restricts itself: the app needs to read it
 * to make its own stream there. */
static struct pw_registry *registry;
static struct spa_hook reg_hook;
static uint32_t node_factory = PW_ID_ANY;

static void reg_global(void *data, uint32_t id, uint32_t perms, const char *type, uint32_t version, const struct spa_dict *props) {
    (void)data; (void)perms; (void)version;
    const char *name = props ? spa_dict_lookup(props, PW_KEY_FACTORY_NAME) : NULL;
    if (type && !strcmp(type, PW_TYPE_INTERFACE_Factory) && name && !strcmp(name, "client-node")) node_factory = id;
}
static const struct pw_registry_events reg_events = { PW_VERSION_REGISTRY_EVENTS, .global = reg_global };

/* What a client keeps. The app's connection: the core, the factory and the streams' nodes, and nothing else, now or
 * later (the same set the desktop portal gives a screen cast's app). Ours, once the nodes exist: read-only for
 * everything (a link is only made between nodes whose owners see each other) and full for our nodes, so with no factory
 * to execute nothing new can be made from here: no node of its own to record another stream with, no link, no module.
 * The named entries go first and PW_ID_ANY last: PW_ID_ANY applies at once to what has no entry yet, and a core hidden
 * for a moment is a connection gone. Applied by the client to itself: PipeWire only lets a client lower its own
 * permissions, never raise them. */
static void restrict_to_nodes(struct pw_core *c, int app) {
    struct pw_permission p[3 + CAST_MAX_STREAMS];
    int n = 0;
    p[n++] = PW_PERMISSION_INIT(PW_ID_CORE, app ? PW_PERM_RWX : PW_PERM_R | PW_PERM_X);
    if (app && node_factory != PW_ID_ANY) p[n++] = PW_PERMISSION_INIT(node_factory, PW_PERM_R);
    for (int i = 0; i < side.nstreams; i++) {
        uint32_t id = S[i].st ? pw_stream_get_node_id(S[i].st) : PW_ID_ANY;
        if (id != PW_ID_ANY) p[n++] = PW_PERMISSION_INIT(id, PW_PERM_RWX);
    }
    p[n++] = PW_PERMISSION_INIT(PW_ID_ANY, app ? 0 : PW_PERM_R);
    pw_client_update_permissions(pw_core_get_client(c), (uint32_t)n, p);
}

/* ---- the streams ---- */

static int enum_format(Stream *s, struct spa_pod_builder *b, const struct spa_pod **out) {
    struct spa_rectangle size = SPA_RECTANGLE(s->w, s->h);
    struct spa_fraction none = SPA_FRACTION(0, 1), max = SPA_FRACTION(s->fps, 1), min = SPA_FRACTION(1, 1);
    out[0] = spa_pod_builder_add_object(b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(3, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBx),
        SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&none),
        SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_CHOICE_RANGE_Fraction(&max, &min, &max));
    return 1;
}

static void want(Stream *s, int on, struct spa_fraction rate) {
    say(CM_WANT, s->idx, (uint32_t)on, rate.num, rate.denom);
}

static struct spa_fraction rate_of[CAST_MAX_STREAMS];

static void on_state(void *data, enum pw_stream_state old, enum pw_stream_state now, const char *err) {
    Stream *s = data;
    (void)old;
    if (now == PW_STREAM_STATE_ERROR) { fprintf(stderr, "lord-screencast: stream %d: %s\n", s->idx, err ? err : "error"); fail(CAST_E_STREAM); return; }
    uint32_t id = pw_stream_get_node_id(s->st);
    if (!s->node_sent && id != PW_ID_ANY && (now == PW_STREAM_STATE_PAUSED || now == PW_STREAM_STATE_STREAMING)) {
        s->node_sent = 1;
        int all = 1;
        for (int i = 0; i < side.nstreams; i++) all &= S[i].node_sent;
        /* every node exists: from here on this connection reaches only them */
        if (all && !restricted) { restricted = 1; restrict_to_nodes(core, 0); }
        say(CM_NODE, s->idx, id, 0, 0);
    }
    int on = now == PW_STREAM_STATE_STREAMING;
    if (on != s->streaming) { s->streaming = on; want(s, on, rate_of[s->idx]); }
}

static void on_param(void *data, uint32_t id, const struct spa_pod *param) {
    Stream *s = data;
    if (id != SPA_PARAM_Format || !param) return;
    struct spa_video_info_raw info;
    memset(&info, 0, sizeof info);
    if (spa_format_video_raw_parse(param, &info) < 0) return;
    if ((info.format != SPA_VIDEO_FORMAT_BGRx && info.format != SPA_VIDEO_FORMAT_RGBx) || info.size.width != s->w || info.size.height != s->h) {
        s->fmt = 0;                     /* not what we offered: no frames until it is */
        return;
    }
    s->fmt = info.format;
    s->fw = info.size.width;
    s->fh = info.size.height;
    rate_of[s->idx] = info.max_framerate.num ? info.max_framerate : info.framerate;
    if (s->streaming) want(s, 1, rate_of[s->idx]);
    uint8_t buf[512];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
    const struct spa_pod *p[2];
    p[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
        SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
        SPA_PARAM_BUFFERS_size, SPA_POD_Int((int)(s->w * 4 * s->h)),
        SPA_PARAM_BUFFERS_stride, SPA_POD_Int((int)(s->w * 4)),
        SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_MemFd));
    p[1] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
        SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
    pw_stream_update_params(s->st, p, 2);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state,
    .param_changed = on_param,
};

static int map_ring(Stream *s, int fd, uint32_t w, uint32_t h) {
    CastLayout l;
    struct stat st;
    if (cast_layout(w, h, &l) || fstat(fd, &st) || (uint64_t)st.st_size < l.total) return -1;
    /* sealed against shrinking: a ring that got shorter under us would fault */
    int seals = fcntl(fd, F_GET_SEALS);
    if (seals < 0 || !(seals & F_SEAL_SHRINK)) return -1;
    void *m = mmap(NULL, (size_t)l.total, PROT_READ, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) return -1;
    if (cast_ring_ok(m, l.total, w, h)) { munmap(m, (size_t)l.total); return -1; }
    if (s->ring) munmap((void *)s->ring, s->ring_len);
    s->ring = m;
    s->ring_len = (size_t)l.total;
    s->lay = l;
    return 0;
}

static int stream_set(const CastMsg *m, int fd) {
    Stream *s = &S[m->stream];
    if (map_ring(s, fd, m->w, m->h)) return -1;
    s->idx = m->stream;
    s->w = m->w;
    s->h = m->h;
    s->fps = m->a;
    s->kind = (int)m->b;
    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
    const struct spa_pod *p[1];
    enum_format(s, &b, p);
    if (s->st) {                        /* a resize: the consumer renegotiates; frames wait for the new size */
        s->fmt = 0;
        return pw_stream_update_params(s->st, p, 1) < 0 ? -1 : 0;
    }
    char name[32];
    snprintf(name, sizeof name, "lordos-screencast-%d", m->stream);
    s->st = pw_stream_new(core, name, pw_properties_new(PW_KEY_MEDIA_CLASS, "Video/Source", PW_KEY_MEDIA_ROLE, "Screen", PW_KEY_NODE_NAME, name,
                                                        PW_KEY_NODE_DESCRIPTION, s->kind == CAST_SRC_WINDOW ? "LordOS window" : "LordOS screen",
                                                        NULL));
    if (!s->st) return -1;
    pw_stream_add_listener(s->st, &s->hook, &stream_events, s);
    return pw_stream_connect(s->st, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                             PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS, p, 1) < 0 ? -1 : 0;
}

/* A frame from slot m->a into the next free buffer: rows of w * 4 bytes, swapped to RGBx when that was asked for.
 * The slot goes back at once: its pixels are copied. */
static void frame(const CastMsg *m) {
    Stream *s = &S[m->stream];
    struct pw_buffer *pb = s->fmt && s->streaming && s->fw == m->w && s->fh == m->h ? pw_stream_dequeue_buffer(s->st) : NULL;
    if (pb) {
        struct spa_buffer *buf = pb->buffer;
        struct spa_data *d = &buf->datas[0];
        size_t stride = (size_t)s->w * 4, need = stride * s->h;
        if (d->data && d->maxsize >= need) {
            const uint8_t *src = s->ring + cast_slot_off(&s->lay, m->a);
            uint8_t *dst = d->data;
            if (s->fmt == SPA_VIDEO_FORMAT_BGRx) memcpy(dst, src, need);
            else {
                const uint32_t *a = (const uint32_t *)src;
                uint32_t *o = (uint32_t *)dst;
                for (size_t i = 0, n = (size_t)s->w * s->h; i < n; i++) {
                    uint32_t v = a[i];
                    o[i] = (v & 0xff00ff00u) | (v & 0xffu) << 16 | (v >> 16 & 0xffu);
                }
            }
            d->chunk->offset = 0;
            d->chunk->size = (uint32_t)need;
            d->chunk->stride = (int32_t)stride;
            d->chunk->flags = 0;
            struct spa_meta_header *h = spa_buffer_find_meta_data(buf, SPA_META_Header, sizeof *h);
            if (h) {
                struct timespec t;
                clock_gettime(CLOCK_MONOTONIC, &t);
                h->pts = (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
                h->flags = 0;
                h->seq = s->seq++;
                h->dts_offset = 0;
            }
        } else d->chunk->size = 0;
        pw_stream_queue_buffer(s->st, pb);
    }
    say(CM_DONE, m->stream, m->a, 0, 0);
}

/* ---- the app's connection ---- */

static void remote_done(void *data, uint32_t id, int seq) {
    Remote *r = data;
    if (id != PW_ID_CORE || seq != r->seq) return;
    /* the daemon has applied the restriction before this sync's answer: the fd may go */
    int fd = pw_core_steal_fd(r->core);
    CastMsg m = { .type = CM_REMOTE, .a = r->req };
    if (cast_send(CHAN, &m, fd)) pw_main_loop_quit(loop);
    if (fd >= 0) close(fd);
    spa_hook_remove(&r->hook);
    pw_core_disconnect(r->core);
    memset(r, 0, sizeof *r);
}

static void remote_error(void *data, uint32_t id, int seq, int res, const char *msg) {
    Remote *r = data;
    (void)seq;
    if (id != PW_ID_CORE) return;
    fprintf(stderr, "lord-screencast: the app's connection: %s (%d)\n", msg ? msg : "", res);
    say(CM_REMOTE, 0, r->req, 0, 0);
    spa_hook_remove(&r->hook);
    pw_core_disconnect(r->core);
    memset(r, 0, sizeof *r);
}

static const struct pw_core_events remote_events = { PW_VERSION_CORE_EVENTS, .done = remote_done, .error = remote_error };

static void client(const CastMsg *m, int fd) {
    Remote *r = NULL;
    for (int i = 0; i < CAST_MAX_REMOTES && !r; i++) if (!R[i].core) r = &R[i];
    struct pw_core *c = r ? pw_context_connect_fd(ctx, fd, NULL, 0) : NULL;
    if (!c) {
        if (!r) close(fd);              /* pw_context_connect_fd owns fd once called */
        say(CM_REMOTE, 0, m->a, 0, 0);
        return;
    }
    r->core = c;
    r->req = m->a;
    pw_core_add_listener(c, &r->hook, &remote_events, r);
    /* the app may use the streams' nodes and make its own stream, and nothing else; the session manager links them */
    restrict_to_nodes(c, 1);
    r->seq = pw_core_sync(c, PW_ID_CORE, 0);
}

/* ---- the channel ---- */

static void on_chan(void *data, int fd, uint32_t mask) {
    (void)data;
    (void)fd;
    if (mask & (SPA_IO_ERR | SPA_IO_HUP)) { pw_main_loop_quit(loop); return; }
    for (int k = 0; k < 64; k++) {
        CastMsg m;
        int mfd = -1;
        int r = cast_recv(CHAN, &m, &mfd);
        if (r == 0) return;
        if (r < 0 || cast_check_server(&side, &m, mfd >= 0)) {
            if (mfd >= 0) close(mfd);
            pw_main_loop_quit(loop);
            return;
        }
        switch (m.type) {
        case CM_STREAM: {
            int bad = stream_set(&m, mfd);
            close(mfd);
            if (bad) { fail(CAST_E_RING); return; }
            break;
        }
        case CM_FRAME: frame(&m); break;
        case CM_CLIENT: client(&m, mfd); break;
        case CM_STOP: pw_main_loop_quit(loop); return;
        default: if (mfd >= 0) close(mfd); pw_main_loop_quit(loop); return;
        }
    }
}

static void core_error(void *data, uint32_t id, int seq, int res, const char *msg) {
    (void)data;
    (void)seq;
    fprintf(stderr, "lord-screencast: PipeWire: %s (%d)\n", msg ? msg : "", res);
    if (id == PW_ID_CORE) fail(CAST_E_PIPEWIRE);
}

static const struct pw_core_events core_events = { PW_VERSION_CORE_EVENTS, .error = core_error };

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    /* the first message, the PipeWire connection: blocking, it comes right after the start */
    struct pollfd p = { CHAN, POLLIN, 0 };
    CastMsg m;
    int pwfd = -1, r = 0;
    while (r == 0) {
        if (poll(&p, 1, 10000) <= 0) return 1;
        r = cast_recv(CHAN, &m, &pwfd);
    }
    if (r < 0 || m.type != CM_HELLO || cast_check_server(&side, &m, pwfd >= 0)) { if (pwfd >= 0) close(pwfd); return 1; }
    pw_init(NULL, NULL);
    loop = pw_main_loop_new(NULL);
    /* no real-time module (it would ask RTKit on the bus), no D-Bus, no devices: a stream and nothing else */
    ctx = loop ? pw_context_new(pw_main_loop_get_loop(loop),
                                pw_properties_new("module.rt", "false", "module.client-device", "false", "module.metadata", "false",
                                                  "support.dbus", "false", "context.num-data-loops", "1", NULL), 0) : NULL;
    if (!ctx) { close(pwfd); return 1; }
    static const char *const ro[] = { "/usr", NULL };
    static const int extra[] = { SYS_timerfd_create, SYS_timerfd_settime, SYS_timerfd_gettime, -1 };
    SandboxHelperCfg cfg = {
        .name = "lordos-screencast",    /* cgroup scope lordos-screencast.scope */
        .ro_paths = ro,
        .extra_syscalls = extra,
        .allow_ioctl_classes = 0,
        .mem_max_mb = 768,
        .ro_dir_fds = NULL,
        .rw_dir_fds = NULL,
    };
    sandbox_apply_helper(&cfg);
    /* from here on, the daemon's bytes and the consumers' */
    core = pw_context_connect_fd(ctx, pwfd, NULL, 0);
    if (!core) { CastMsg f = { .type = CM_FAIL, .a = CAST_E_PIPEWIRE }; cast_send(CHAN, &f, -1); return 1; }
    pw_core_add_listener(core, &core_hook, &core_events, NULL);
    registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    if (registry) pw_registry_add_listener(registry, &reg_hook, &reg_events, NULL);
    pw_loop_add_io(pw_main_loop_get_loop(loop), CHAN, SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP, false, on_chan, NULL);
    pw_main_loop_run(loop);
    for (int i = 0; i < CAST_MAX_STREAMS; i++) if (S[i].st) pw_stream_destroy(S[i].st);
    pw_core_disconnect(core);
    return 0;
}
