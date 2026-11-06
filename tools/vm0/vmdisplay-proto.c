/*
 * vmdisplay-proto: Phase 0 prototype of lord-vmdisplay (docs/VIRTUALIZATION.md section 3.2).
 *
 * Build: cc -O2 -Wall -Wextra -o vmdisplay-proto vmdisplay-proto.c -I../../third_party/stb $(pkg-config --cflags --libs libsystemd) -lm
 *
 * It starts nothing. Given the QMP socket of a running QEMU started with -display dbus,p2p=yes it:
 *   1. creates a socketpair, hands one end to QEMU (QMP getfd + add_client protocol=@dbus-display) and
 *      speaks QEMU's D-Bus display protocol peer to peer on the other end with sd-bus (client role, no bus
 *      daemon, no Hello: QEMU is the authentication server on both connections);
 *   2. registers a listener on Console_0 over a second socketpair and serves org.qemu.Display1.Listener
 *      (+ org.qemu.Display1.Listener.Unix.Map unless --no-map) on it;
 *   3. validates every picture, damage rectangle, buffer descriptor and cursor before using it;
 *   4. on stdin commands: writes PNGs, prints stats, types keys through org.qemu.Display1.Keyboard.
 *
 * Usage: vmdisplay-proto <qmp.sock> [--no-map] [--coalesce HZ] [--introspect FILE] [--trace FILE]
 *   --coalesce HZ   with Unix.Map, merge damage and copy it at most HZ times a second (0 = copy on every UpdateMap)
 * stdin commands (one per line):
 *   png PATH        save the current picture
 *   stats           print counters since the last "reset" as one JSON line
 *   reset           zero the counters
 *   type TEXT       type ASCII text; "\n" in TEXT means Enter
 *   key CODE        press and release one QEMU key number (decimal or 0x..)
 *   ctrl CODE       the same, with Left Ctrl held
 *   quit
 * stdout: one JSON line per event or command result. stderr: rejects and diagnostics.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <systemd/sd-bus.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_ONLY_PNG
#include "stb_image_write.h"

/* Limits applied before anything from QEMU is used. */
#define MAX_DIM        8192u                 /* width or height of a scanout */
#define MAX_BYTES      (256u * 1024 * 1024)  /* one picture */
#define MAX_CURSOR     256                   /* cursor width or height */

/* pixman format codes: PIXMAN_FORMAT(bpp, type, a, r, g, b) */
#define PIXMAN_x8r8g8b8 0x20020888u
#define PIXMAN_a8r8g8b8 0x20028888u

#define LISTENER_PATH "/org/qemu/Display1/Listener"
#define CONSOLE_PATH  "/org/qemu/Display1/Console_0"

static sd_bus *bus;   /* our connection to QEMU's display server */
static sd_bus *lbus;  /* the listener connection QEMU calls into */
static bool use_map = true;
static FILE *trace_f;

/* Current picture, always kept as 32 bpp x8r8g8b8 (BGRX bytes on little endian). */
static uint8_t *fb;
static uint32_t fb_w, fb_h, fb_stride;
static enum { SRC_NONE, SRC_SCANOUT, SRC_MAP, SRC_DMABUF } fb_src;
static uint8_t *map_ptr;
static size_t map_len;
static uint32_t map_off, map_stride;

static uint8_t cursor[MAX_CURSOR * MAX_CURSOR * 4];
static int cur_w, cur_h, cur_hx, cur_hy;

struct stats {
    double t0;
    uint64_t scanout, update, scanout_map, update_map, scanout_dmabuf, update_dmabuf, disable, cursor_define, mouse_set;
    uint64_t rejects, bytes, pixels, copies;
};
static double coalesce_hz;
static bool dirty;
static int dx0, dy0, dx1, dy1;
static double next_flush;
static struct stats st;
static double first_scanout_rt, first_update_rt;

static double now_rt(void) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
static double now_mono(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void out(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stdout, fmt, ap); va_end(ap);
    fputc('\n', stdout); fflush(stdout);
}

static void reject(const char *what, const char *fmt, ...) {
    va_list ap;
    st.rejects++;
    fprintf(stderr, "reject %s: ", what);
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
    out("{\"ev\":\"reject\",\"what\":\"%s\"}", what);
}

static bool fmt_ok(uint32_t fmt) {
    return fmt == PIXMAN_x8r8g8b8 || fmt == PIXMAN_a8r8g8b8;
}

/* width, height, stride against limits; returns the picture size or 0. */
static uint64_t check_geometry(const char *what, uint32_t w, uint32_t h, uint32_t stride, uint32_t fmt) {
    if (w == 0 || h == 0 || w > MAX_DIM || h > MAX_DIM) { reject(what, "size %ux%u out of limits", w, h); return 0; }
    if (!fmt_ok(fmt)) { reject(what, "pixman format 0x%08x not accepted", fmt); return 0; }
    if ((uint64_t)stride < (uint64_t)w * 4 || stride % 4) { reject(what, "stride %u too small for width %u", stride, w); return 0; }
    uint64_t bytes = (uint64_t)stride * h;
    if (bytes > MAX_BYTES) { reject(what, "picture %" PRIu64 " bytes over limit", bytes); return 0; }
    return bytes;
}

static bool fb_alloc(uint32_t w, uint32_t h) {
    free(fb); fb = NULL; fb_w = fb_h = fb_stride = 0;
    fb = calloc((size_t)w * h, 4);
    if (!fb) return false;
    fb_w = w; fb_h = h; fb_stride = w * 4;
    return true;
}

static void unmap_shared(void) {
    dirty = false;
    if (map_ptr) munmap(map_ptr, map_len);
    map_ptr = NULL; map_len = 0;
}

static void trace_update(const char *kind, int x, int y, int w, int h) {
    if (trace_f) fprintf(trace_f, "%.6f %s %d %d %d %d\n", now_rt(), kind, x, y, w, h);
}

static void note_first_update(void) {
    if (!first_update_rt) { first_update_rt = now_rt(); out("{\"ev\":\"first_update\",\"rt\":%.6f}", first_update_rt); }
}
static void note_first_scanout(const char *kind, uint32_t w, uint32_t h) {
    if (!first_scanout_rt) first_scanout_rt = now_rt();
    out("{\"ev\":\"scanout\",\"kind\":\"%s\",\"w\":%u,\"h\":%u,\"rt\":%.6f,\"first_rt\":%.6f}", kind, w, h, now_rt(), first_scanout_rt);
}

/* rect inside the current picture? */
static bool rect_ok(const char *what, int x, int y, int w, int h) {
    if (!fb) { reject(what, "update without a scanout"); return false; }
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || (uint64_t)x + (uint64_t)w > fb_w || (uint64_t)y + (uint64_t)h > fb_h) {
        reject(what, "rect %d,%d %dx%d outside %ux%u", x, y, w, h, fb_w, fb_h); return false;
    }
    return true;
}

/* ---- org.qemu.Display1.Listener ---- */

static int on_scanout(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    uint32_t w, h, stride, fmt; const void *data; size_t len;
    int r = sd_bus_message_read(m, "uuuu", &w, &h, &stride, &fmt);
    if (r >= 0) r = sd_bus_message_read_array(m, 'y', &data, &len);
    if (r < 0) { reject("Scanout", "bad message: %s", strerror(-r)); return sd_bus_reply_method_return(m, NULL); }
    st.scanout++;
    uint64_t bytes = check_geometry("Scanout", w, h, stride, fmt);
    if (!bytes) return sd_bus_reply_method_return(m, NULL);
    if (len != bytes) { reject("Scanout", "array %zu bytes, stride*height %" PRIu64, len, bytes); return sd_bus_reply_method_return(m, NULL); }
    unmap_shared();
    if (!fb_alloc(w, h)) { reject("Scanout", "out of memory"); return sd_bus_reply_method_return(m, NULL); }
    for (uint32_t row = 0; row < h; row++) memcpy(fb + (size_t)row * fb_stride, (const uint8_t *)data + (size_t)row * stride, fb_stride);
    fb_src = SRC_SCANOUT;
    st.bytes += len; st.pixels += (uint64_t)w * h;
    note_first_scanout("shm-array", w, h);
    return sd_bus_reply_method_return(m, NULL);
}

static int on_update(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    int32_t x, y, w, h; uint32_t stride, fmt; const void *data; size_t len;
    int r = sd_bus_message_read(m, "iiiiuu", &x, &y, &w, &h, &stride, &fmt);
    if (r >= 0) r = sd_bus_message_read_array(m, 'y', &data, &len);
    if (r < 0) { reject("Update", "bad message: %s", strerror(-r)); return sd_bus_reply_method_return(m, NULL); }
    st.update++;
    trace_update("Update", x, y, w, h);
    note_first_update();
    if (!rect_ok("Update", x, y, w, h)) return sd_bus_reply_method_return(m, NULL);
    if (!fmt_ok(fmt) || (uint64_t)stride < (uint64_t)w * 4) { reject("Update", "format 0x%08x stride %u", fmt, stride); return sd_bus_reply_method_return(m, NULL); }
    if ((uint64_t)stride * (uint64_t)(h - 1) + (uint64_t)w * 4 > len) { reject("Update", "array %zu bytes too short for %dx%d stride %u", len, w, h, stride); return sd_bus_reply_method_return(m, NULL); }
    for (int row = 0; row < h; row++)
        memcpy(fb + (size_t)(y + row) * fb_stride + (size_t)x * 4, (const uint8_t *)data + (size_t)row * stride, (size_t)w * 4);
    st.bytes += len; st.pixels += (uint64_t)w * h; st.copies++;
    return sd_bus_reply_method_return(m, NULL);
}

static int on_scanout_dmabuf(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    int fd; uint32_t w, h, stride, fourcc; uint64_t modifier; int y0_top;
    int r = sd_bus_message_read(m, "huuuutb", &fd, &w, &h, &stride, &fourcc, &modifier, &y0_top);
    if (r < 0) { reject("ScanoutDMABUF", "bad message: %s", strerror(-r)); return sd_bus_reply_method_return(m, NULL); }
    st.scanout_dmabuf++;
    off_t real = lseek(fd, 0, SEEK_END);
    char fcc[5] = { fourcc & 0xff, (fourcc >> 8) & 0xff, (fourcc >> 16) & 0xff, fourcc >> 24, 0 };
    for (int i = 0; i < 4; i++) if (fcc[i] < 32 || fcc[i] > 126) fcc[i] = '?';
    out("{\"ev\":\"scanout_dmabuf\",\"w\":%u,\"h\":%u,\"stride\":%u,\"fourcc\":\"%s\",\"fourcc_hex\":\"0x%08x\",\"modifier\":\"0x%016" PRIx64 "\",\"y0_top\":%d,\"fd_len\":%lld}",
        w, h, stride, fcc, fourcc, modifier, y0_top, (long long)real);
    if (w == 0 || h == 0 || w > MAX_DIM || h > MAX_DIM) reject("ScanoutDMABUF", "size %ux%u out of limits", w, h);
    else if (real < 0) reject("ScanoutDMABUF", "lseek failed: %s", strerror(errno));
    else if ((uint64_t)stride * h > (uint64_t)real) reject("ScanoutDMABUF", "stride*height %" PRIu64 " > fd length %lld", (uint64_t)stride * h, (long long)real);
    else { note_first_scanout("dmabuf", w, h); fb_src = SRC_DMABUF; unmap_shared(); free(fb); fb = NULL; fb_w = w; fb_h = h; }
    /* Not imported: the prototype only records what QEMU offers. sd-bus closes the fd with the message. */
    return sd_bus_reply_method_return(m, NULL);
}

static int on_update_dmabuf(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    int32_t x, y, w, h;
    if (sd_bus_message_read(m, "iiii", &x, &y, &w, &h) < 0) return sd_bus_reply_method_return(m, NULL);
    st.update_dmabuf++;
    trace_update("UpdateDMABUF", x, y, w, h);
    note_first_update();
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || (uint64_t)x + w > fb_w || (uint64_t)y + h > fb_h) reject("UpdateDMABUF", "rect %d,%d %dx%d outside %ux%u", x, y, w, h, fb_w, fb_h);
    else st.pixels += (uint64_t)w * h;
    return sd_bus_reply_method_return(m, NULL);
}

static int on_disable(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    st.disable++;
    unmap_shared(); free(fb); fb = NULL; fb_w = fb_h = 0; fb_src = SRC_NONE;
    out("{\"ev\":\"disable\",\"rt\":%.6f}", now_rt());
    return sd_bus_reply_method_return(m, NULL);
}

static int on_mouse_set(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    int32_t x, y, on;
    if (sd_bus_message_read(m, "iii", &x, &y, &on) >= 0) st.mouse_set++;
    return sd_bus_reply_method_return(m, NULL);
}

static int on_cursor_define(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    int32_t w, h, hx, hy; const void *data; size_t len;
    int r = sd_bus_message_read(m, "iiii", &w, &h, &hx, &hy);
    if (r >= 0) r = sd_bus_message_read_array(m, 'y', &data, &len);
    if (r < 0) { reject("CursorDefine", "bad message"); return sd_bus_reply_method_return(m, NULL); }
    st.cursor_define++;
    if (w <= 0 || h <= 0 || w > MAX_CURSOR || h > MAX_CURSOR) reject("CursorDefine", "size %dx%d out of limits", w, h);
    else if (len != (size_t)w * h * 4) reject("CursorDefine", "array %zu bytes, expected %d", len, w * h * 4);
    else if (hx < 0 || hy < 0 || hx >= w || hy >= h) reject("CursorDefine", "hotspot %d,%d outside %dx%d", hx, hy, w, h);
    else {
        memcpy(cursor, data, len); cur_w = w; cur_h = h; cur_hx = hx; cur_hy = hy;
        out("{\"ev\":\"cursor\",\"w\":%d,\"h\":%d,\"hot_x\":%d,\"hot_y\":%d}", w, h, hx, hy);
    }
    return sd_bus_reply_method_return(m, NULL);
}

static int prop_interfaces(sd_bus *b, const char *path, const char *iface, const char *prop,
                           sd_bus_message *reply, void *u, sd_bus_error *e) {
    (void)b; (void)path; (void)iface; (void)prop; (void)u; (void)e;
    int r = sd_bus_message_open_container(reply, 'a', "s");
    if (r < 0) return r;
    if (use_map) { r = sd_bus_message_append(reply, "s", "org.qemu.Display1.Listener.Unix.Map"); if (r < 0) return r; }
    return sd_bus_message_close_container(reply);
}

/* ---- org.qemu.Display1.Listener.Unix.Map ---- */

static int on_scanout_map(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    int fd; uint32_t off, w, h, stride, fmt;
    int r = sd_bus_message_read(m, "huuuuu", &fd, &off, &w, &h, &stride, &fmt);
    if (r < 0) { reject("ScanoutMap", "bad message: %s", strerror(-r)); return sd_bus_reply_method_return(m, NULL); }
    st.scanout_map++;
    uint64_t bytes = check_geometry("ScanoutMap", w, h, stride, fmt);
    if (!bytes) return sd_bus_reply_method_return(m, NULL);
    off_t real = lseek(fd, 0, SEEK_END);
    int seals = fcntl(fd, F_GET_SEALS);
    if (real < 0) { reject("ScanoutMap", "lseek failed: %s", strerror(errno)); return sd_bus_reply_method_return(m, NULL); }
    if ((uint64_t)off + bytes > (uint64_t)real) {
        reject("ScanoutMap", "offset %u + stride*height %" PRIu64 " > fd length %lld", off, bytes, (long long)real);
        return sd_bus_reply_method_return(m, NULL);
    }
    unmap_shared();
    /* Read-only, private to us. A peer that can still shrink the file could make reads fault (SIGBUS);
     * the real helper must require F_SEAL_SHRINK or copy under a SIGBUS guard. Recorded below. */
    void *p = mmap(NULL, (size_t)real, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { reject("ScanoutMap", "mmap failed: %s", strerror(errno)); return sd_bus_reply_method_return(m, NULL); }
    map_ptr = p; map_len = (size_t)real; map_off = off; map_stride = stride;
    if (!fb_alloc(w, h)) { reject("ScanoutMap", "out of memory"); unmap_shared(); return sd_bus_reply_method_return(m, NULL); }
    for (uint32_t row = 0; row < h; row++) memcpy(fb + (size_t)row * fb_stride, map_ptr + off + (size_t)row * stride, fb_stride);
    fb_src = SRC_MAP;
    st.bytes += (uint64_t)w * h * 4; st.pixels += (uint64_t)w * h;
    out("{\"ev\":\"scanout_map_fd\",\"fd_len\":%lld,\"offset\":%u,\"stride\":%u,\"seals\":%d,\"seal_shrink\":%s}",
        (long long)real, off, stride, seals, (seals >= 0 && (seals & F_SEAL_SHRINK)) ? "true" : "false");
    note_first_scanout("shm-map", w, h);
    return sd_bus_reply_method_return(m, NULL);
}

/* Copy damage out of QEMU's memory, as the helper must before forwarding (QEMU keeps writing it). */
static void copy_from_map(int x, int y, int w, int h) {
    for (int row = 0; row < h; row++)
        memcpy(fb + (size_t)(y + row) * fb_stride + (size_t)x * 4, map_ptr + map_off + (size_t)(y + row) * map_stride + (size_t)x * 4, (size_t)w * 4);
    st.bytes += (uint64_t)w * h * 4; st.pixels += (uint64_t)w * h; st.copies++;
}

static void flush_coalesced(void) {
    if (!dirty || now_mono() < next_flush) return;
    dirty = false;
    next_flush = now_mono() + 1.0 / coalesce_hz;
    /* the rect may be stale after a new ScanoutMap: clip again */
    if (!map_ptr || dx1 > (int)fb_w || dy1 > (int)fb_h) return;
    copy_from_map(dx0, dy0, dx1 - dx0, dy1 - dy0);
}

static int on_update_map(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    int32_t x, y, w, h;
    if (sd_bus_message_read(m, "iiii", &x, &y, &w, &h) < 0) return sd_bus_reply_method_return(m, NULL);
    st.update_map++;
    trace_update("UpdateMap", x, y, w, h);
    note_first_update();
    if (!map_ptr) { reject("UpdateMap", "no mapping"); return sd_bus_reply_method_return(m, NULL); }
    if (!rect_ok("UpdateMap", x, y, w, h)) return sd_bus_reply_method_return(m, NULL);
    if (coalesce_hz > 0) {
        if (!dirty) { dx0 = x; dy0 = y; dx1 = x + w; dy1 = y + h; dirty = true; }
        else { if (x < dx0) dx0 = x; if (y < dy0) dy0 = y; if (x + w > dx1) dx1 = x + w; if (y + h > dy1) dy1 = y + h; }
    } else copy_from_map(x, y, w, h);
    return sd_bus_reply_method_return(m, NULL);
}

static const sd_bus_vtable listener_vt[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Scanout", "uuuuay", "", on_scanout, 0),
    SD_BUS_METHOD("Update", "iiiiuuay", "", on_update, 0),
    SD_BUS_METHOD("ScanoutDMABUF", "huuuutb", "", on_scanout_dmabuf, 0),
    SD_BUS_METHOD("UpdateDMABUF", "iiii", "", on_update_dmabuf, 0),
    SD_BUS_METHOD("Disable", "", "", on_disable, 0),
    SD_BUS_METHOD("MouseSet", "iii", "", on_mouse_set, 0),
    SD_BUS_METHOD("CursorDefine", "iiiiay", "", on_cursor_define, 0),
    SD_BUS_PROPERTY("Interfaces", "as", prop_interfaces, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END
};
static const sd_bus_vtable map_vt[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("ScanoutMap", "huuuuu", "", on_scanout_map, 0),
    SD_BUS_METHOD("UpdateMap", "iiii", "", on_update_map, 0),
    SD_BUS_VTABLE_END
};

/* ---- QMP ---- */

static int qmp_read_reply(int fd, char *buf, size_t cap) {
    /* Reads lines until one carries "return" or "error"; events are skipped. */
    size_t n = 0;
    for (;;) {
        ssize_t r = read(fd, buf + n, cap - 1 - n);
        if (r <= 0) return -1;
        n += (size_t)r; buf[n] = 0;
        char *line = buf, *nl;
        while ((nl = strchr(line, '\n'))) {
            *nl = 0;
            if (strstr(line, "\"return\"") || strstr(line, "\"error\"") || strstr(line, "\"QMP\"")) {
                int err = strstr(line, "\"error\"") != NULL;
                if (err) fprintf(stderr, "qmp: %s\n", line);
                return err ? -1 : 0;
            }
            line = nl + 1;
        }
        memmove(buf, line, strlen(line) + 1); n = strlen(buf);
        if (n >= cap - 1) return -1;
    }
}

static int qmp_send(int fd, const char *json, int pass_fd) {
    struct iovec iov = { (void *)json, strlen(json) };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
    char cbuf[CMSG_SPACE(sizeof(int))];
    if (pass_fd >= 0) {
        memset(cbuf, 0, sizeof cbuf);
        msg.msg_control = cbuf; msg.msg_controllen = sizeof cbuf;
        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &pass_fd, sizeof(int));
    }
    return sendmsg(fd, &msg, MSG_NOSIGNAL) == (ssize_t)iov.iov_len ? 0 : -1;
}

/* Returns our end of a D-Bus socketpair whose other end QEMU now owns as a display client. */
static int qmp_add_display_client(const char *path) {
    int q = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    char buf[65536];
    if (strlen(path) >= sizeof a.sun_path) return -1;
    strcpy(a.sun_path, path);
    if (q < 0 || connect(q, (struct sockaddr *)&a, sizeof a) < 0) { perror("qmp connect"); return -1; }
    if (qmp_read_reply(q, buf, sizeof buf) < 0) return -1; /* greeting */
    if (qmp_send(q, "{\"execute\":\"qmp_capabilities\"}\n", -1) < 0 || qmp_read_reply(q, buf, sizeof buf) < 0) return -1;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) return -1;
    if (qmp_send(q, "{\"execute\":\"getfd\",\"arguments\":{\"fdname\":\"vmdisplay\"}}\n", sv[1]) < 0 || qmp_read_reply(q, buf, sizeof buf) < 0) return -1;
    close(sv[1]);
    if (qmp_send(q, "{\"execute\":\"add_client\",\"arguments\":{\"protocol\":\"@dbus-display\",\"fdname\":\"vmdisplay\"}}\n", -1) < 0 ||
        qmp_read_reply(q, buf, sizeof buf) < 0) return -1;
    close(q); /* the D-Bus connection outlives the QMP session */
    return sv[0];
}

static sd_bus *p2p_client(int fd) {
    sd_bus *b = NULL;
    int r = sd_bus_new(&b);
    if (r >= 0) r = sd_bus_set_fd(b, fd, fd);
    if (r >= 0) r = sd_bus_negotiate_fds(b, 1);
    if (r >= 0) r = sd_bus_start(b);
    if (r < 0) { fprintf(stderr, "sd-bus p2p setup: %s\n", strerror(-r)); sd_bus_unref(b); return NULL; }
    return b;
}

/* ---- introspection dump ---- */

static void introspect(FILE *f, const char *path, int depth) {
    sd_bus_error e = SD_BUS_ERROR_NULL; sd_bus_message *rep = NULL; const char *xml;
    if (depth > 4) return;
    if (sd_bus_call_method(bus, NULL, path, "org.freedesktop.DBus.Introspectable", "Introspect", &e, &rep, "") < 0) {
        fprintf(f, "<!-- %s: %s -->\n", path, e.message); sd_bus_error_free(&e); return;
    }
    if (sd_bus_message_read(rep, "s", &xml) >= 0) {
        fprintf(f, "<!-- object %s -->\n%s\n", path, xml);
        const char *p = xml;
        while ((p = strstr(p, "<node name=\""))) {
            p += 12; const char *q = strchr(p, '"'); if (!q) break;
            char child[512];
            snprintf(child, sizeof child, "%s%s%.*s", path, strcmp(path, "/") ? "/" : "", (int)(q - p), p);
            introspect(f, child, depth + 1);
            p = q;
        }
    }
    sd_bus_message_unref(rep);
}

/* ---- keyboard ---- */

/* ASCII to QEMU key number (PC set-1 scancodes), with shift flag in bit 8. */
static int ascii_key(char c) {
    static const char *row1 = "1234567890-=", *row1s = "!@#$%^&*()_+";
    static const char *q = "qwertyuiop[]", *a = "asdfghjkl;'", *z = "zxcvbnm,./";
    static const char *qs = "QWERTYUIOP{}", *as = "ASDFGHJKL:\"", *zs = "ZXCVBNM<>?";
    const char *p;
    if (c == '\n') return 0x1c;
    if (c == ' ') return 0x39;
    if (c == '\t') return 0x0f;
    if (c == '`') return 0x29;
    if (c == '~') return 0x29 | 0x100;
    if (c == '\\') return 0x2b;
    if (c == '|') return 0x2b | 0x100;
    if ((p = strchr(row1, c))) return 0x02 + (int)(p - row1);
    if ((p = strchr(row1s, c))) return (0x02 + (int)(p - row1s)) | 0x100;
    if ((p = strchr(q, c))) return 0x10 + (int)(p - q);
    if ((p = strchr(qs, c))) return (0x10 + (int)(p - qs)) | 0x100;
    if ((p = strchr(a, c))) return 0x1e + (int)(p - a);
    if ((p = strchr(as, c))) return (0x1e + (int)(p - as)) | 0x100;
    if ((p = strchr(z, c))) return 0x2c + (int)(p - z);
    if ((p = strchr(zs, c))) return (0x2c + (int)(p - zs)) | 0x100;
    return -1;
}

/* Queue of key events, sent asynchronously so we never block while QEMU waits on us. */
struct kev { uint32_t code; bool press; };
static struct kev kq[4096];
static size_t kq_head, kq_tail;
static double kq_next;
#define KEY_GAP 0.015

static void kq_push(uint32_t code, bool press) {
    if ((kq_tail + 1) % 4096 == kq_head) return;
    kq[kq_tail] = (struct kev){ code, press }; kq_tail = (kq_tail + 1) % 4096;
}
static void kq_stroke(uint32_t code, bool shift, bool ctrl) {
    if (ctrl) kq_push(0x1d, true);
    if (shift) kq_push(0x2a, true);
    kq_push(code, true); kq_push(code, false);
    if (shift) kq_push(0x2a, false);
    if (ctrl) kq_push(0x1d, false);
}
static uint64_t key_errors, keys_sent;
static int key_reply(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    if (sd_bus_message_is_method_error(m, NULL)) { key_errors++; fprintf(stderr, "keyboard: %s\n", sd_bus_message_get_error(m)->message); }
    return 0;
}
static void kq_pump(void) {
    if (kq_head == kq_tail || now_mono() < kq_next) return;
    struct kev k = kq[kq_head]; kq_head = (kq_head + 1) % 4096;
    sd_bus_call_method_async(bus, NULL, NULL, CONSOLE_PATH, "org.qemu.Display1.Keyboard",
                             k.press ? "Press" : "Release", key_reply, NULL, "u", k.code);
    keys_sent++;
    kq_next = now_mono() + KEY_GAP;
}

/* ---- commands ---- */

static void save_png(const char *path) {
    if (!fb || !fb_w) { out("{\"ok\":false,\"cmd\":\"png\",\"why\":\"no shm picture (source %d)\"}", fb_src); return; }
    if (fb_src == SRC_MAP && map_ptr) /* take the freshest pixels */
        for (uint32_t row = 0; row < fb_h; row++) memcpy(fb + (size_t)row * fb_stride, map_ptr + map_off + (size_t)row * map_stride, fb_stride);
    uint8_t *rgb = malloc((size_t)fb_w * fb_h * 3);
    if (!rgb) return;
    for (uint32_t y = 0; y < fb_h; y++)
        for (uint32_t x = 0; x < fb_w; x++) {
            const uint8_t *s = fb + (size_t)y * fb_stride + (size_t)x * 4; uint8_t *d = rgb + ((size_t)y * fb_w + x) * 3;
            d[0] = s[2]; d[1] = s[1]; d[2] = s[0];
        }
    int ok = stbi_write_png(path, (int)fb_w, (int)fb_h, 3, rgb, (int)fb_w * 3);
    free(rgb);
    out("{\"ok\":%s,\"cmd\":\"png\",\"path\":\"%s\",\"w\":%u,\"h\":%u}", ok ? "true" : "false", path, fb_w, fb_h);
}

static double cpu_s(void) {
    struct rusage ru; getrusage(RUSAGE_SELF, &ru);
    return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}
static double st_cpu0;

static void print_stats(void) {
    double dt = now_mono() - st.t0, cpu = cpu_s() - st_cpu0;
    uint64_t upd = st.update + st.update_map + st.update_dmabuf;
    out("{\"cmd\":\"stats\",\"secs\":%.3f,\"scanout\":%" PRIu64 ",\"scanout_map\":%" PRIu64 ",\"scanout_dmabuf\":%" PRIu64
        ",\"update\":%" PRIu64 ",\"update_map\":%" PRIu64 ",\"update_dmabuf\":%" PRIu64 ",\"disable\":%" PRIu64
        ",\"cursor_define\":%" PRIu64 ",\"mouse_set\":%" PRIu64 ",\"rejects\":%" PRIu64
        ",\"updates_per_s\":%.1f,\"copies_per_s\":%.1f,\"bytes_per_s\":%.0f,\"pixels_per_s\":%.0f,\"cpu_pct\":%.2f,\"keys_sent\":%" PRIu64 ",\"key_errors\":%" PRIu64
        ",\"w\":%u,\"h\":%u,\"source\":%d}",
        dt, st.scanout, st.scanout_map, st.scanout_dmabuf, st.update, st.update_map, st.update_dmabuf, st.disable,
        st.cursor_define, st.mouse_set, st.rejects, upd / dt, st.copies / dt, st.bytes / dt, st.pixels / dt, 100.0 * cpu / dt, keys_sent, key_errors,
        fb_w, fb_h, fb_src);
}
static void reset_stats(void) { memset(&st, 0, sizeof st); st.t0 = now_mono(); st_cpu0 = cpu_s(); }

static bool handle_line(char *line) {
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (!strncmp(line, "png ", 4)) save_png(line + 4);
    else if (!strcmp(line, "stats")) print_stats();
    else if (!strcmp(line, "reset")) { reset_stats(); out("{\"ok\":true,\"cmd\":\"reset\"}"); }
    else if (!strncmp(line, "type ", 5)) {
        int bad = 0;
        for (const char *p = line + 5; *p; p++) {
            char c = *p;
            if (c == '\\' && p[1] == 'n') { c = '\n'; p++; }
            int k = ascii_key(c);
            if (k < 0) { bad++; continue; }
            kq_stroke(k & 0xff, k & 0x100, false);
        }
        out("{\"ok\":true,\"cmd\":\"type\",\"skipped\":%d}", bad);
    } else if (!strncmp(line, "key ", 4) || !strncmp(line, "ctrl ", 5)) {
        bool ctrl = line[0] == 'c';
        kq_stroke((uint32_t)strtoul(line + (ctrl ? 5 : 4), NULL, 0), false, ctrl);
        out("{\"ok\":true,\"cmd\":\"key\"}");
    } else if (!strcmp(line, "quit")) return false;
    else out("{\"ok\":false,\"why\":\"unknown command\"}");
    return true;
}

static int bus_timeout_ms(sd_bus *b) {
    uint64_t usec;
    if (sd_bus_get_timeout(b, &usec) <= 0 || usec == UINT64_MAX) return -1;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t n = (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    return usec <= n ? 0 : (int)((usec - n + 999) / 1000);
}

int main(int argc, char **argv) {
    const char *qmp = NULL, *intro = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--no-map")) use_map = false;
        else if (!strcmp(argv[i], "--coalesce") && i + 1 < argc) coalesce_hz = atof(argv[++i]);
        else if (!strcmp(argv[i], "--introspect") && i + 1 < argc) intro = argv[++i];
        else if (!strcmp(argv[i], "--trace") && i + 1 < argc) { trace_f = fopen(argv[++i], "w"); }
        else qmp = argv[i];
    }
    if (!qmp) { fprintf(stderr, "usage: %s <qmp.sock> [--no-map] [--introspect FILE] [--trace FILE]\n", argv[0]); return 2; }
    reset_stats();
    double t_start = now_rt();

    int fd = qmp_add_display_client(qmp);
    if (fd < 0) { fprintf(stderr, "add_client failed\n"); return 1; }
    bus = p2p_client(fd);
    if (!bus) return 1;

    sd_bus_error e = SD_BUS_ERROR_NULL;
    char *label = NULL, *type = NULL; uint32_t cw = 0, ch = 0;
    if (sd_bus_get_property_string(bus, NULL, CONSOLE_PATH, "org.qemu.Display1.Console", "Label", &e, &label) < 0) {
        fprintf(stderr, "Console_0 not reachable: %s\n", e.message); return 1;
    }
    sd_bus_get_property_string(bus, NULL, CONSOLE_PATH, "org.qemu.Display1.Console", "Type", &e, &type);
    sd_bus_get_property_trivial(bus, NULL, CONSOLE_PATH, "org.qemu.Display1.Console", "Width", &e, 'u', &cw);
    sd_bus_get_property_trivial(bus, NULL, CONSOLE_PATH, "org.qemu.Display1.Console", "Height", &e, 'u', &ch);
    out("{\"ev\":\"connected\",\"console\":\"%s\",\"type\":\"%s\",\"w\":%u,\"h\":%u,\"connect_ms\":%.1f}",
        label, type ? type : "", cw, ch, (now_rt() - t_start) * 1000);
    free(label); free(type);
    if (intro) { FILE *f = fopen(intro, "w"); if (f) { introspect(f, "/", 0); fclose(f); } }

    int lv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, lv) < 0) return 1;
    lbus = p2p_client(lv[0]);
    if (!lbus) return 1;
    if (sd_bus_add_object_vtable(lbus, NULL, LISTENER_PATH, "org.qemu.Display1.Listener", listener_vt, NULL) < 0) return 1;
    if (use_map && sd_bus_add_object_vtable(lbus, NULL, LISTENER_PATH, "org.qemu.Display1.Listener.Unix.Map", map_vt, NULL) < 0) return 1;
    sd_bus_message *rep = NULL;
    if (sd_bus_call_method(bus, NULL, CONSOLE_PATH, "org.qemu.Display1.Console", "RegisterListener", &e, &rep, "h", lv[1]) < 0) {
        fprintf(stderr, "RegisterListener: %s\n", e.message); return 1;
    }
    sd_bus_message_unref(rep);
    close(lv[1]);
    out("{\"ev\":\"listener\",\"coalesce_hz\":%.0f,\"map\":%s,\"ms\":%.1f}", coalesce_hz, use_map ? "true" : "false", (now_rt() - t_start) * 1000);

    char line[8192]; size_t ln = 0;
    bool run = true;
    while (run) {
        int r;
        while ((r = sd_bus_process(lbus, NULL)) > 0) ;
        if (r < 0) { fprintf(stderr, "listener connection: %s\n", strerror(-r)); break; }
        while ((r = sd_bus_process(bus, NULL)) > 0) ;
        if (r < 0) { fprintf(stderr, "display connection: %s\n", strerror(-r)); break; }
        kq_pump();
        if (coalesce_hz > 0) flush_coalesced();
        if (trace_f) fflush(trace_f);
        struct pollfd p[3] = {
            { sd_bus_get_fd(lbus), (short)sd_bus_get_events(lbus), 0 },
            { sd_bus_get_fd(bus), (short)sd_bus_get_events(bus), 0 },
            { 0, POLLIN, 0 },
        };
        int to = bus_timeout_ms(lbus), t2 = bus_timeout_ms(bus);
        if (t2 >= 0 && (to < 0 || t2 < to)) to = t2;
        if (kq_head != kq_tail) { int tk = (int)((kq_next - now_mono()) * 1000) + 1; if (tk < 0) tk = 0; if (to < 0 || tk < to) to = tk; }
        if (dirty) { int tf = (int)((next_flush - now_mono()) * 1000) + 1; if (tf < 0) tf = 0; if (to < 0 || tf < to) to = tf; }
        if (poll(p, 3, to) < 0 && errno != EINTR) break;
        if (p[2].revents & (POLLIN | POLLHUP)) {
            ssize_t k = read(0, line + ln, sizeof line - 1 - ln);
            if (k <= 0) break;
            ln += (size_t)k; line[ln] = 0;
            char *s = line, *nl;
            while (run && (nl = strchr(s, '\n'))) { *nl = 0; run = handle_line(s); s = nl + 1; }
            memmove(line, s, strlen(s) + 1); ln = strlen(line);
            if (ln >= sizeof line - 1) ln = 0;
        }
    }
    print_stats();
    unmap_shared();
    sd_bus_flush_close_unref(lbus);
    sd_bus_flush_close_unref(bus);
    if (trace_f) fclose(trace_f);
    return 0;
}
