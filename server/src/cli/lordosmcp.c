/* lordosmcp.c — lordos-mcp: an MCP server (stdio) that lets an AI agent (Claude Code, later Lumo's own agent) use the
 * LordOS desktop, as Claude in Chrome lets it use a browser (docs/HANDS.md). Approved by the maintainer on 2026-11-26.
 * Installed as /usr/lib/lordos/bin/lordos-mcp, /usr/bin/lordos-mcp a link to it.
 *
 *   lordos-mcp                  the person's running session, over its session bus (org.lordos.Hands1): off until they
 *                               turn on Settings → AI → Hands, and LordOS asks them before the first action
 *   lordos-mcp --headless APP   a private desktop of its own for QA: a headless lord-server --hands-qa on a private bus,
 *                               a demo home with sample files, APP opened; nobody sees it and the person's desktop is
 *                               never touched. Ends (and is deleted) with this program.
 *   claude mcp add -s user lordos-mcp -- lordos-mcp
 *
 * It runs as the person and links only libc: it never reads the screen, injects input or opens anything itself. Every
 * tool is one or two calls to lord-server, which checks everything (hands/hands.c). Tools: windows_context, open_app,
 * open_path, close_window, read_window, find, computer, set_value, read_app_log, record, hands_mode (src/hands/mcp.c has
 * their schemas). There is no tool that runs code.
 *
 * How the agent's cursor moves (docs/HANDS.md "The agent's cursor"): human or machine, the session's (hands_mode,
 * --mode) or one call's (computer's and set_value's mode), set on lord-server with SetMode before an action when it
 * differs from what lord-server has for this connection. */
#include "../hands/hands.h"
#include "../hands/mcp.h"
#include "../hands/mcpimg.h"
#include "../sec/json.h"
#include "../vault/dbus.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LINE_MAX_BYTES (4u << 20)
#define CALL_MS 30000
#define ASK_MS 120000                   /* how long a first call waits for the person's answer */
#define REC_MAX 200

/* ---- text ---- */

typedef struct { char *p; size_t n, cap; int oom; } Str;
static void s_put(Str *s, const char *d, size_t n) {
    if (s->oom) return;
    if (s->n + n + 1 > s->cap) {
        size_t c = s->cap ? s->cap * 2 : 1024;
        while (c < s->n + n + 1) c *= 2;
        char *q = realloc(s->p, c);
        if (!q) { s->oom = 1; return; }
        s->p = q;
        s->cap = c;
    }
    memcpy(s->p + s->n, d, n);
    s->n += n;
    s->p[s->n] = 0;
}
static void s_add(Str *s, const char *d) { s_put(s, d, strlen(d)); }
static void s_fmt(Str *s, const char *f, ...) {
    char b[1024];
    va_list ap;
    va_start(ap, f);
    int k = vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    if (k > 0) s_put(s, b, (size_t)k < sizeof b ? (size_t)k : sizeof b - 1);
}
static void s_q(Str *s, const char *t) { char *q = mcp_quote(t); if (q) { s_add(s, q); free(q); } else s->oom = 1; }

static void out_line(const char *json) {
    fputs(json, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

/* ---- the bus ---- */

static DConn bus;
static int bus_up, granted, hello_done;
static int session_mode = HANDS_MODE_DEFAULT;   /* hands_mode, --mode */
static int srv_mode = HANDS_MODE_DEFAULT;       /* what lord-server has for this connection */
static char client_name[64] = "MCP client";
static McpShot last_shot;

static int bus_open(void) {
    if (bus_up) return 0;
    int fd = dbus_connect_session();
    if (fd < 0 || dbus_open(&bus, fd, 3000)) { if (fd >= 0) close(fd); return -1; }
    bus_up = 1;
    return 0;
}

/* 1 a reply (m), 0 a D-Bus error (m, m->error), -1 no answer */
static int call(const char *member, const char *sig, const DBuf *body, DMsg *m) {
    if (bus_open()) return -1;
    DHdr h = { .type = DBUS_CALL, .path = HANDS_BUS_PATH, .iface = HANDS_BUS_IFACE, .member = member, .dest = HANDS_BUS_NAME,
               .sig = sig, .flags = DBUS_NO_AUTOSTART };
    uint32_t s = dbus_send(&bus, &h, body);
    if (!s) return -1;
    memset(m, 0, sizeof *m);
    int r = dbus_wait_reply(&bus, s, m, CALL_MS);
    if (r < 0) { dbus_close(&bus); bus_up = 0; hello_done = granted = 0; srv_mode = HANDS_MODE_DEFAULT; }
    return r;
}

static void sleep_ms(int ms) { struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }

/* the error's sentence for the agent; the message of a D-Bus error from Hands1 is one closed word */
static const char *err_of(const DMsg *m, char *buf, size_t n) {
    const char *word = NULL;
    if (m->sig && !strcmp(m->sig, "s")) { DRd d; dr_init(&d, m); word = dr_str(&d, NULL); if (d.err) word = NULL; }
    return mcp_error_text(m->error, word, buf, n);
}

/* Hello, then wait for the person's answer: 0 granted, or the reason in why */
static int ready(char *why, size_t n) {
    if (granted && bus_up) return 0;
    DMsg m;
    DBuf b;
    dw_init(&b);
    dw_str(&b, client_name);
    int r = call("Hello", "s", &b, &m);
    dw_free(&b);
    if (r < 0) { mcp_error_text("org.freedesktop.DBus.Error.ServiceUnknown", NULL, why, n); return -1; }
    if (r == 0) { err_of(&m, why, n); dbus_msg_free(&m); return -1; }
    DRd d;
    dr_init(&d, &m);
    const char *st = dr_str(&d, NULL);
    int asking = !d.err && !strcmp(st, "asking");
    dbus_msg_free(&m);
    hello_done = 1;
    for (int waited = 0; asking && waited < ASK_MS; waited += 500) {
        sleep_ms(500);
        r = call("Status", NULL, NULL, &m);
        if (r == 1) { dbus_msg_free(&m); asking = 0; break; }
        if (r < 0) { mcp_error_text("org.freedesktop.DBus.Error.ServiceUnknown", NULL, why, n); return -1; }
        int still = m.error && !strcmp(m.error, HANDS_ERR_ASKING);
        if (!still) { err_of(&m, why, n); dbus_msg_free(&m); return -1; }
        dbus_msg_free(&m);
    }
    if (asking) { mcp_error_text(HANDS_ERR_ASKING, NULL, why, n); return -1; }
    granted = 1;
    return 0;
}

/* a call whose answer is one string: 1 with it in *out (malloc'd), 0 an error (its sentence in why) */
static int call_s(const char *member, const char *sig, const DBuf *body, char **out, char *why, size_t n) {
    *out = NULL;
    DMsg m;
    int r = call(member, sig, body, &m);
    if (r < 0) { mcp_error_text("org.freedesktop.DBus.Error.ServiceUnknown", NULL, why, n); return 0; }
    if (r == 0) {
        err_of(&m, why, n);
        if (m.error && (!strcmp(m.error, HANDS_ERR_STOPPED) || !strcmp(m.error, HANDS_ERR_DENIED))) granted = 0;
        dbus_msg_free(&m);
        return 0;
    }
    if (m.sig && !strcmp(m.sig, "s")) { DRd d; dr_init(&d, &m); const char *s = dr_str(&d, NULL); if (!d.err) *out = strdup(s); }
    if (!*out) *out = strdup("");
    dbus_msg_free(&m);
    return 1;
}

/* the connection's mode on lord-server: want, set with SetMode when it differs (1 done, 0 an error in why) */
static int use_mode(int want, char *why, size_t n) {
    if (want == srv_mode) return 1;
    DBuf b;
    dw_init(&b);
    dw_str(&b, hands_mode_word(want));
    char *out = NULL;
    int ok = call_s("SetMode", "s", &b, &out, why, n);
    dw_free(&b);
    free(out);
    if (ok) srv_mode = want;
    return ok;
}
/* a call's mode argument: its mode, the session's when absent; -1 not a mode */
static int mode_arg(const JVal *a) {
    const char *m = a ? json_str(a, "mode") : NULL;
    if (!m) return session_mode;
    int k = hands_mode_of(m);
    return k < 0 ? -1 : k == HANDS_MODE_DEFAULT ? session_mode : k;
}

/* ---- the recording ---- */

static int recording;
static McpFrame frames[REC_MAX];
static int nframes;

static void rec_clear(void) { for (int i = 0; i < nframes; i++) free(frames[i].rgb); nframes = 0; }

/* the display now, small, as one more frame */
static void rec_frame(void) {
    if (!recording || nframes >= REC_MAX) return;
    DBuf b;
    dw_init(&b);
    dw_str(&b, "display");
    for (int i = 0; i < 4; i++) dw_i32(&b, 0);
    dw_u32(&b, 640);
    DMsg m;
    int r = call("Screenshot", "siiiiu", &b, &m);
    dw_free(&b);
    if (r != 1) { if (r == 0) dbus_msg_free(&m); return; }
    DRd d;
    dr_init(&d, &m);
    uint32_t n = 0;
    const uint8_t *png = dr_bytes(&d, &n);
    int w = 0, h = 0;
    uint8_t *rgb = !d.err ? mcp_png_rgb(png, n, &w, &h) : NULL;
    if (rgb && w <= 1024 && h <= 1024) {
        frames[nframes].rgb = malloc((size_t)w * h * 3);
        if (frames[nframes].rgb) { memcpy(frames[nframes].rgb, rgb, (size_t)w * h * 3); frames[nframes].w = w; frames[nframes].h = h; nframes++; }
    }
    if (rgb) mcp_png_free(rgb);
    dbus_msg_free(&m);
}

/* ---- results ---- */

static char *text_result(const char *text, int error) {
    Str s = { 0 };
    s_add(&s, "{\"content\":[{\"type\":\"text\",\"text\":");
    s_q(&s, text);
    s_add(&s, error ? "}],\"isError\":true}" : "}]}");
    return s.oom ? NULL : s.p;
}

static const char *str_arg(const JVal *a, const char *k) { return a ? json_str(a, k) : NULL; }
static int point_arg(const JVal *a, const char *k, double *x, double *y) {
    const JVal *v = a ? json_get(a, k) : NULL;
    if (!v || v->type != J_ARR || v->len != 2 || !v->kid || v->kid->type != J_NUM || !v->kid->next || v->kid->next->type != J_NUM) return 0;
    mcp_map_point(&last_shot, v->kid->num, v->kid->next->num, x, y);
    return 1;
}

/* empty: the tool's name for its answer when lord-server sends back nothing (mcp_empty_text), and empty_arg what to echo */
static char *tool_simple_as(const char *empty, const char *empty_arg, const char *member, const char *arg, const char *sig2, const char *arg2, unsigned u) {
    char why[256], none[192];
    if (ready(why, sizeof why)) return text_result(why, 1);
    DBuf b;
    dw_init(&b);
    char sig[8] = "";
    if (arg) { dw_str(&b, arg); strcat(sig, "s"); }
    if (sig2 && !strcmp(sig2, "su")) { dw_str(&b, arg2 ? arg2 : ""); dw_u32(&b, u); strcat(sig, "su"); }
    else if (sig2 && !strcmp(sig2, "s")) { dw_str(&b, arg2 ? arg2 : ""); strcat(sig, "s"); }
    else if (sig2 && !strcmp(sig2, "u")) { dw_u32(&b, u); strcat(sig, "u"); }
    char *out = NULL;
    int ok = call_s(member, sig[0] ? sig : NULL, &b, &out, why, sizeof why);
    dw_free(&b);
    char *r = text_result(ok ? (out && out[0] ? out : mcp_empty_text(empty, empty_arg, none, sizeof none)) : why, !ok);
    free(out);
    return r;
}
static char *tool_simple(const char *member, const char *arg, const char *sig2, const char *arg2, unsigned u) {
    return tool_simple_as(NULL, NULL, member, arg, sig2, arg2, u);
}

/* one Screenshot call: 1 with the reply in *m (its PNG and meta point into it), 0 with why */
static int shot_call(const char *target, double x, double y, double w, double h, uint32_t maxdim, DMsg *m,
                     const uint8_t **png, uint32_t *n, const char **meta, char *why, size_t wn) {
    DBuf b;
    dw_init(&b);
    dw_str(&b, target);
    dw_i32(&b, (int32_t)x);
    dw_i32(&b, (int32_t)y);
    dw_i32(&b, (int32_t)w);
    dw_i32(&b, (int32_t)h);
    dw_u32(&b, maxdim);
    int r = call("Screenshot", "siiiiu", &b, m);
    dw_free(&b);
    if (r < 0) { mcp_error_text("org.freedesktop.DBus.Error.ServiceUnknown", NULL, why, wn); return 0; }
    if (r == 0) { err_of(m, why, wn); dbus_msg_free(m); return 0; }
    DRd d;
    dr_init(&d, m);
    *n = 0;
    *png = dr_bytes(&d, n);
    *meta = dr_str(&d, NULL);
    if (d.err) { dbus_msg_free(m); snprintf(why, wn, "The screenshot failed."); return 0; }
    return 1;
}

/* save: the same capture again at its own resolution (lord-server's same path: the same black areas, never more of the
 * screen than the agent's image shows), written as a new file in ~/Pictures/LordOS QA; the sentence for the result */
static void shot_save(const char *target, double x, double y, double w, double h, const char *file, char *say, size_t sn) {
    char why[256];
    DMsg m;
    const uint8_t *png = NULL;
    const char *meta = NULL;
    uint32_t n = 0;
    if (!shot_call(target, x, y, w, h, HANDS_SHOT_NATIVE, &m, &png, &n, &meta, why, sizeof why)) {
        snprintf(say, sn, " Not saved: %s", why);
        return;
    }
    McpShot s;
    mcp_shot_from_json(&s, meta);
    char where[PATH_MAX + 128];
    int ok = !mcp_write_export(getenv("HOME"), file, png, n, where, sizeof where);
    int e = errno;
    dbus_msg_free(&m);
    if (ok) snprintf(say, sn, " Saved at full resolution (%dx%d) to %s.", s.width, s.height, where);
    else snprintf(say, sn, " Not saved: could not write ~/%s/%s: %s.", MCP_EXPORT_DIR, file, e == EEXIST ? "a file with that name exists" : strerror(e));
}

static char *screenshot(const char *target, double x, double y, double w, double h, int maxdim, int keep, const char *save) {
    char why[256], file[128];
    if (save && mcp_shot_save_name(save, file, sizeof file))
        return text_result("save must be a bare file name (letters, digits, spaces, . _ -), no folders.", 1);
    if (ready(why, sizeof why)) return text_result(why, 1);
    DMsg m;
    const uint8_t *png = NULL;
    const char *meta = NULL;
    uint32_t n = 0;
    if (!shot_call(target, x, y, w, h, (uint32_t)maxdim, &m, &png, &n, &meta, why, sizeof why)) return text_result(why, 1);
    McpShot s;
    mcp_shot_from_json(&s, meta);
    if (keep && s.valid) last_shot = s;
    char *b64 = mcp_base64(png, n);
    dbus_msg_free(&m);
    char saved[PATH_MAX + 256] = "";
    if (save) shot_save(target, x, y, w, h, file, saved, sizeof saved);
    Str o = { 0 };
    s_add(&o, "{\"content\":[{\"type\":\"image\",\"mimeType\":\"image/png\",\"data\":\"");
    s_add(&o, b64 ? b64 : "");
    s_add(&o, "\"},{\"type\":\"text\",\"text\":");
    char t[PATH_MAX + 700];
    snprintf(t, sizeof t, "%dx%d image of the desktop area at (%.0f, %.0f), %.0fx%.0f points.%s System prompts and protected or "
             "password areas show as black.%s", s.width, s.height, s.x, s.y, s.w, s.h,
             keep ? " Coordinates you give now are pixels of this image." : " (Zoom: coordinates still refer to the last screenshot.)", saved);
    s_q(&o, t);
    s_add(&o, "}]}");
    free(b64);
    return o.oom ? NULL : o.p;
}

static char *computer(const JVal *a) {
    const char *act = str_arg(a, "action");
    if (!act) return text_result("action is required.", 1);
    const char *ref = str_arg(a, "ref");
    if (ref && !hands_ref_ok(ref)) return text_result("ref must look like ref_12 (from read_window or find).", 1);
    char why[256];
    if (!strcmp(act, "wait")) {
        double s = a ? json_num(a, "duration", 1) : 1;
        if (s < 0) s = 0;
        if (s > 10) s = 10;
        sleep_ms((int)(s * 1000));
        rec_frame();
        return text_result("waited", 0);
    }
    if (!strcmp(act, "screenshot")) {
        const char *target = str_arg(a, "target");
        char *r = screenshot(target ? target : "display", 0, 0, 0, 0, HANDS_SHOT_MAXDIM, 1, str_arg(a, "save"));
        return r;
    }
    if (!strcmp(act, "zoom")) {
        const JVal *v = a ? json_get(a, "region") : NULL;
        double c[4];
        int k = 0;
        for (const JVal *e = v && v->type == J_ARR ? v->kid : NULL; e && k < 4; e = e->next) if (e->type == J_NUM) c[k++] = e->num;
        if (k != 4) return text_result("region must be [x0, y0, x1, y1].", 1);
        double x0, y0, x1, y1;
        mcp_map_point(&last_shot, c[0], c[1], &x0, &y0);
        mcp_map_point(&last_shot, c[2], c[3], &x1, &y1);
        if (x1 <= x0 || y1 <= y0) return text_result("region must be [x0, y0, x1, y1] with x1 > x0 and y1 > y0.", 1);
        return screenshot("region", x0, y0, x1 - x0, y1 - y0, 1024, 0, str_arg(a, "save"));
    }
    if (ready(why, sizeof why)) return text_result(why, 1);
    int mode = mode_arg(a);
    if (mode < 0) return text_result("mode must be human or machine.", 1);
    if (!use_mode(mode, why, sizeof why)) return text_result(why, 1);
    DBuf b;
    dw_init(&b);
    const char *member = NULL, *sig = NULL;
    char *res = NULL;
    double x = -1, y = -1, x2 = -1, y2 = -1;
    if (!strcmp(act, "type")) {
        const char *t = str_arg(a, "text");
        if (!t || !t[0]) { dw_free(&b); return text_result("text is required.", 1); }
        dw_str(&b, t);
        member = "Type"; sig = "s";
    } else if (!strcmp(act, "key")) {
        const char *t = str_arg(a, "text");
        if (!t || !t[0]) { dw_free(&b); return text_result("text is required (a chord such as ctrl+s).", 1); }
        double rep = a ? json_num(a, "repeat", 1) : 1;
        dw_str(&b, t);
        dw_u32(&b, rep >= 1 && rep <= 20 ? (uint32_t)rep : 1);
        member = "Key"; sig = "su";
    } else if (!strcmp(act, "scroll")) {
        if (!ref && !point_arg(a, "coordinate", &x, &y)) { dw_free(&b); return text_result("ref or coordinate is required.", 1); }
        const char *dir = str_arg(a, "scroll_direction");
        double amt = a ? json_num(a, "scroll_amount", 3) : 3;
        dw_str(&b, ref ? ref : "");
        dw_i32(&b, ref ? -1 : (int32_t)x);
        dw_i32(&b, ref ? -1 : (int32_t)y);
        dw_str(&b, dir ? dir : "down");
        dw_u32(&b, amt >= 1 && amt <= 30 ? (uint32_t)amt : 3);
        member = "Scroll"; sig = "siisu";
    } else {
        int p = hands_pointer_action(act);
        if (!p) { dw_free(&b); return text_result("unknown action.", 1); }
        if (p == HANDS_P_DRAG) {
            if (!point_arg(a, "start_coordinate", &x, &y) || !point_arg(a, "coordinate", &x2, &y2)) { dw_free(&b); return text_result("left_click_drag needs start_coordinate and coordinate.", 1); }
            ref = NULL;
        } else if (!ref && !point_arg(a, "coordinate", &x, &y)) { dw_free(&b); return text_result("ref or coordinate is required.", 1); }
        dw_str(&b, act);
        dw_str(&b, ref ? ref : "");
        dw_i32(&b, ref ? -1 : (int32_t)x);
        dw_i32(&b, ref ? -1 : (int32_t)y);
        dw_i32(&b, (int32_t)x2);
        dw_i32(&b, (int32_t)y2);
        member = "Pointer"; sig = "ssiiii";
    }
    char *out = NULL;
    int ok = call_s(member, sig, &b, &out, why, sizeof why);
    dw_free(&b);
    if (ok) rec_frame();
    res = text_result(ok ? (out && out[0] ? out : "done") : why, !ok);
    free(out);
    return res;
}

static char *set_value(const JVal *a) {
    const char *ref = str_arg(a, "ref");
    const JVal *v = a ? json_get(a, "value") : NULL;
    if (!ref || !hands_ref_ok(ref) || !ref[0] || !v) return text_result("ref and value are required.", 1);
    char num[64];
    const char *val = v->type == J_STR ? v->str : v->type == J_BOOL ? (v->num ? "true" : "false") : NULL;
    if (v->type == J_NUM) { snprintf(num, sizeof num, "%g", v->num); val = num; }
    if (!val) return text_result("value must be a string, a boolean or a number.", 1);
    int mode = mode_arg(a);
    if (mode < 0) return text_result("mode must be human or machine.", 1);
    char why[256];
    if (ready(why, sizeof why) || !use_mode(mode, why, sizeof why)) return text_result(why, 1);
    char *r = tool_simple("SetValue", ref, "s", val, 0);
    rec_frame();
    return r;
}

static char *record(const JVal *a) {
    const char *act = str_arg(a, "action");
    if (!act) return text_result("action is required.", 1);
    if (!strcmp(act, "start")) { rec_clear(); recording = 1; rec_frame(); return text_result("recording: every action adds a frame", 0); }
    if (!strcmp(act, "stop")) {
        recording = 0;
        char t[64];
        snprintf(t, sizeof t, "stopped: %d frames", nframes);
        return text_result(t, 0);
    }
    if (!strcmp(act, "export")) {
        /* the GIF back as content, or saved under one name in ~/Pictures/LordOS QA: never a path the agent picks */
        const char *name = str_arg(a, "name");
        if (name && !mcp_export_name_ok(name)) return text_result("name must be a bare file name ending in .gif (letters, digits, spaces, . _ -), no folders.", 1);
        if (!nframes) return text_result("nothing recorded.", 1);
        size_t n = 0;
        uint8_t *gif = mcp_gif(frames, nframes, 60, &n);
        if (!gif) return text_result("the GIF could not be made.", 1);
        if (!name) {
            char *b64 = mcp_base64(gif, n);
            free(gif);
            Str o = { 0 };
            s_add(&o, "{\"content\":[{\"type\":\"image\",\"mimeType\":\"image/gif\",\"data\":\"");
            s_add(&o, b64 ? b64 : "");
            s_fmt(&o, "\"},{\"type\":\"text\",\"text\":\"%d frames\"}]}", nframes);
            free(b64);
            return o.oom ? NULL : o.p;
        }
        const char *home = getenv("HOME");
        char where[PATH_MAX + 128], t[PATH_MAX + 192];
        int ok = !mcp_write_export(home, name, gif, n, where, sizeof where);
        int e = errno;
        free(gif);
        if (ok) snprintf(t, sizeof t, "wrote %s (%d frames)", where, nframes);
        else snprintf(t, sizeof t, "could not write ~/%s/%s: %s", MCP_EXPORT_DIR, name, e == EEXIST ? "a file with that name exists" : strerror(e));
        return text_result(t, !ok);
    }
    return text_result("action must be start, stop or export.", 1);
}

static char *tools_call(const JVal *params) {
    const char *name = params ? json_str(params, "name") : NULL;
    const JVal *a = params ? json_get(params, "arguments") : NULL;
    if (a && a->type != J_OBJ) a = NULL;
    if (!name) return text_result("tool name missing.", 1);
    if (!strcmp(name, "windows_context")) return tool_simple("Windows", NULL, NULL, NULL, 0);
    if (!strcmp(name, "open_app")) {
        const char *app = str_arg(a, "app");
        if (!app || !hands_name_ok(app)) return text_result("app is required.", 1);
        char *r = tool_simple("OpenApp", app, NULL, NULL, 0);
        rec_frame();
        return r;
    }
    if (!strcmp(name, "open_path")) {
        const char *p = str_arg(a, "path");
        if (!p || !hands_path_ok(p)) return text_result("path must be an absolute path.", 1);
        char *r = tool_simple("OpenPath", p, NULL, NULL, 0);
        rec_frame();
        return r;
    }
    if (!strcmp(name, "close_window")) {
        const char *w = str_arg(a, "window_id");
        if (!w || !hands_id_ok(w)) return text_result("window_id is required.", 1);
        return tool_simple("CloseWindow", w, NULL, NULL, 0);
    }
    if (!strcmp(name, "read_window")) {
        const char *w = str_arg(a, "window_id"), *f = str_arg(a, "filter");
        double depth = a ? json_num(a, "depth", 0) : 0;
        char why[256];
        if (ready(why, sizeof why)) return text_result(why, 1);
        DBuf b;
        dw_init(&b);
        dw_str(&b, w ? w : "");
        dw_str(&b, f ? f : "all");
        dw_u32(&b, depth >= 1 && depth <= HANDS_DEPTH_MAX ? (uint32_t)depth : 0);
        char *out = NULL;
        int ok = call_s("ReadWindow", "ssu", &b, &out, why, sizeof why);
        dw_free(&b);
        char *r = text_result(ok ? (out && out[0] ? out : "(nothing to read)") : why, !ok);
        free(out);
        return r;
    }
    if (!strcmp(name, "find")) {
        const char *q = str_arg(a, "query"), *w = str_arg(a, "window_id");
        if (!q || !q[0]) return text_result("query is required.", 1);
        char *r = tool_simple_as("find", q, "Find", w ? w : "", "s", q, 0);
        return r;
    }
    if (!strcmp(name, "computer")) return computer(a);
    if (!strcmp(name, "set_value")) return set_value(a);
    if (!strcmp(name, "read_app_log")) {
        const char *app = str_arg(a, "app"), *p = str_arg(a, "pattern");
        double mx = a ? json_num(a, "max_lines", 100) : 100;
        if (!app || !hands_name_ok(app)) return text_result("app is required.", 1);
        return tool_simple_as("read_app_log", p, "AppLog", app, "su", p ? p : "", mx >= 1 && mx <= HANDS_LOG_MAX ? (unsigned)mx : 100);
    }
    if (!strcmp(name, "record")) return record(a);
    if (!strcmp(name, "hands_mode")) {
        const char *m = str_arg(a, "mode");
        int k = m ? hands_mode_of(m) : -1;
        if (k < 0 || !m[0]) return text_result("mode must be human, machine or default.", 1);
        char why[256];
        if (ready(why, sizeof why)) return text_result(why, 1);
        session_mode = k;
        DBuf b;
        dw_init(&b);
        dw_str(&b, hands_mode_word(k));
        char *out = NULL;
        int ok = call_s("SetMode", "s", &b, &out, why, sizeof why);
        dw_free(&b);
        if (ok) srv_mode = k;
        char *r = text_result(ok ? (out && out[0] ? out : "done") : why, !ok);
        free(out);
        return r;
    }
    return text_result("unknown tool.", 1);
}

/* ---- JSON-RPC ---- */

static void reply(const JVal *id, const char *result) {
    Str s = { 0 };
    s_add(&s, "{\"jsonrpc\":\"2.0\",\"id\":");
    if (id && id->type == J_NUM) s_fmt(&s, "%.0f", id->num);
    else if (id && id->type == J_STR) s_q(&s, id->str);
    else s_add(&s, "null");
    s_add(&s, ",\"result\":");
    s_add(&s, result ? result : "{}");
    s_add(&s, "}");
    if (!s.oom) out_line(s.p);
    free(s.p);
}

static void reply_error(const JVal *id, int code, const char *msg) {
    Str s = { 0 };
    s_add(&s, "{\"jsonrpc\":\"2.0\",\"id\":");
    if (id && id->type == J_NUM) s_fmt(&s, "%.0f", id->num);
    else if (id && id->type == J_STR) s_q(&s, id->str);
    else s_add(&s, "null");
    s_fmt(&s, ",\"error\":{\"code\":%d,\"message\":", code);
    s_q(&s, msg);
    s_add(&s, "}}");
    if (!s.oom) out_line(s.p);
    free(s.p);
}

static void handle(const char *line, size_t n) {
    JDoc d;
    if (json_parse_depth(line, n, &d, NULL, 0, 64) || !d.root || d.root->type != J_OBJ) { reply_error(NULL, -32700, "parse error"); return; }
    const char *method = json_str(d.root, "method");
    const JVal *id = json_get(d.root, "id"), *params = json_get(d.root, "params");
    if (!method) { json_free(&d); return; }                  /* a response to us: nothing asks anything */
    if (!id) { json_free(&d); return; }                      /* notifications (initialized, cancelled) */
    if (!strcmp(method, "initialize")) {
        const JVal *ci = params ? json_get(params, "clientInfo") : NULL;
        const char *cn = ci ? json_str(ci, "name") : NULL;
        char clean[HANDS_NAME_MAX + 1];
        /* "claude-code" → "Claude Code": the name LordOS's prompt shows (reported by the client, never verified) */
        if (cn && !strcmp(cn, "claude-code")) snprintf(client_name, sizeof client_name, "Claude Code");
        else if (cn && !hands_clean_name(cn, clean, sizeof clean)) snprintf(client_name, sizeof client_name, "%s", clean);
        const char *pv = params ? json_str(params, "protocolVersion") : NULL;
        Str s = { 0 };
        s_add(&s, "{\"protocolVersion\":");
        s_q(&s, pv && strlen(pv) < 32 ? pv : MCP_PROTOCOL);
        s_add(&s, ",\"capabilities\":{\"tools\":{\"listChanged\":false}},\"serverInfo\":{\"name\":\"lordos-mcp\",\"version\":\"1.0\"},"
                  "\"instructions\":\"LordOS desktop tools (Hands). Start with windows_context, then read_window for refs, act with computer or set_value, check with read_window or a screenshot. The person can stop you at any time; respect refusals.\"}");
        reply(id, s.p);
        free(s.p);
    } else if (!strcmp(method, "ping")) reply(id, "{}");
    else if (!strcmp(method, "tools/list")) reply(id, mcp_tools_json());
    else if (!strcmp(method, "tools/call")) {
        char *r = tools_call(params && params->type == J_OBJ ? params : NULL);
        if (r) reply(id, r);
        else reply_error(id, -32603, "out of memory");
        free(r);
    } else reply_error(id, -32601, "method not found");
    json_free(&d);
}

/* ---- --headless: a private desktop for QA ---- */

static char qa_dir[1024];
static pid_t qa_daemon, qa_server;

static int rm_one(const char *p, const struct stat *st, int flag, struct FTW *f) { (void)st; (void)flag; (void)f; remove(p); return 0; }
static void qa_end(void) {
    if (qa_server > 0) { kill(qa_server, SIGTERM); for (int i = 0; i < 30 && waitpid(qa_server, NULL, WNOHANG) == 0; i++) sleep_ms(100); kill(qa_server, SIGKILL); waitpid(qa_server, NULL, 0); }
    if (qa_daemon > 0) { kill(qa_daemon, SIGTERM); waitpid(qa_daemon, NULL, 0); }
    qa_server = qa_daemon = 0;
    if (qa_dir[0] && !getenv("LORDOS_MCP_KEEP")) nftw(qa_dir, rm_one, 16, FTW_DEPTH | FTW_PHYS);
}

static int write_file(const char *dir, const char *name, const char *text) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "we");
    if (!f) return -1;
    fputs(text, f);
    return fclose(f);
}

/* the first of a few places a file is, relative to the folder above lord-server's (installed: /usr/lib/lordos; built:
 * server/, whose ../ is the repository) */
static int find_in(const char *root, const char *rel, char *out, size_t n) {
    const char *tries[] = { "%s/%s", "%s/../%s" };
    for (int i = 0; i < 2; i++) { snprintf(out, n, tries[i], root, rel); if (!access(out, R_OK)) return 0; }
    return -1;
}

static int qa_start(const char *app) {
    char self[2048], bindir[2048], server[PATH_MAX], root[PATH_MAX];
    ssize_t l = readlink("/proc/self/exe", self, sizeof self - 1);
    if (l <= 0) return -1;
    self[l] = 0;
    snprintf(bindir, sizeof bindir, "%s", self);
    char *sl = strrchr(bindir, '/');
    if (sl) *sl = 0;
    const char *env_server = getenv("LORDOS_MCP_SERVER");
    if (env_server && env_server[0] == '/') snprintf(server, sizeof server, "%s", env_server);
    else snprintf(server, sizeof server, "%s/lord-server", bindir);
    if (access(server, X_OK)) { fprintf(stderr, "lordos-mcp: no lord-server at %s\n", server); return -1; }
    snprintf(root, sizeof root, "%s", server);
    for (int i = 0; i < 2; i++) { sl = strrchr(root, '/'); if (sl) *sl = 0; }
    char theme[PATH_MAX], fonts[PATH_MAX], shell[PATH_MAX];
    const char *env_theme = getenv("LORDOS_MCP_THEME");
    if (env_theme && env_theme[0] == '/') snprintf(theme, sizeof theme, "%s", env_theme);
    else if (find_in(root, "themes/default/theme.bundle.js", theme, sizeof theme)) { fprintf(stderr, "lordos-mcp: no theme next to %s\n", server); return -1; }
    if (find_in(root, "assets/fonts", fonts, sizeof fonts) || find_in(root, "assets/shell", shell, sizeof shell)) { fprintf(stderr, "lordos-mcp: no assets next to %s\n", server); return -1; }
    const char *rt = getenv("XDG_RUNTIME_DIR");
    snprintf(qa_dir, sizeof qa_dir, "%s/lordos-mcp-XXXXXX", rt && rt[0] == '/' ? rt : "/tmp");
    if (!mkdtemp(qa_dir)) { qa_dir[0] = 0; return -1; }
    atexit(qa_end);
    /* the demo identity: a home of its own with a few sample files, nothing of the person's */
    char home[2048], d[PATH_MAX];
    snprintf(home, sizeof home, "%s/home", qa_dir);
    const char *dirs[] = { "", "/.config", "/.config/lordos", "/Documents", "/Pictures", "/Desktop", "/Downloads" };
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) { snprintf(d, sizeof d, "%s%s", home, dirs[i]); mkdir(d, 0700); }
    snprintf(d, sizeof d, "%s/.config/lordos", home);
    const char *lang = getenv("LORDOS_MCP_LANG");
    char settings[256];
    snprintf(settings, sizeof settings, "{\"schema\": 9, \"desktopChosen\": true, \"welcomeShown\": true, \"language\": \"%s\", \"mode\": \"dark\"}\n",
             lang && strlen(lang) < 12 && hands_id_ok(lang) ? lang : "en");
    write_file(d, "settings.json", settings);
    snprintf(d, sizeof d, "%s/Documents", home);
    write_file(d, "Shopping list.md", "# Shopping list\n\n- [ ] Bread\n- [ ] Milk\n- [ ] Coffee\n");
    write_file(d, "Meeting notes.txt", "Meeting notes\n\nRelease checklist reviewed. Next meeting on Monday.\n");
    write_file(d, "Ideas.md", "# Ideas\n\nA LordOS theme with sea colours.\n");
    /* the demo identity in a terminal too: its prompt and what programs read from the environment */
    write_file(home, ".bashrc", "export USER=demo LOGNAME=demo HOSTNAME=demo-pc\nPS1='demo@demo-pc:\\w\\$ '\nexport PS1\n");
    /* a private bus */
    char conf[PATH_MAX], sock[PATH_MAX], addr[PATH_MAX + 16];
    snprintf(conf, sizeof conf, "%s/bus.conf", qa_dir);
    snprintf(sock, sizeof sock, "%s/bus", qa_dir);
    snprintf(addr, sizeof addr, "unix:path=%s", sock);
    FILE *f = fopen(conf, "we");
    if (!f) return -1;
    fprintf(f, "<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN\" "
               "\"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n"
               "<busconfig><type>session</type><listen>%s</listen><auth>EXTERNAL</auth>"
               "<policy context=\"default\"><allow send_destination=\"*\"/><allow receive_sender=\"*\"/><allow own=\"*\"/><allow user=\"*\"/></policy></busconfig>\n", addr);
    fclose(f);
    qa_daemon = fork();
    if (qa_daemon == 0) {
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0); dup2(null, 1); dup2(null, 2);
        char arg[PATH_MAX + 16];
        snprintf(arg, sizeof arg, "--config-file=%s", conf);
        execl("/usr/bin/dbus-daemon", "dbus-daemon", arg, "--nofork", "--nopidfile", (char *)NULL);
        _exit(127);
    }
    for (int i = 0; i < 100 && access(sock, F_OK); i++) sleep_ms(20);
    if (access(sock, F_OK)) { fprintf(stderr, "lordos-mcp: the private bus didn't start\n"); return -1; }
    const char *size = getenv("LORDOS_MCP_SIZE");
    int sw = 0, shh = 0;
    char sz[32] = "1280x800";
    if (size && sscanf(size, "%dx%d", &sw, &shh) == 2 && sw >= 640 && shh >= 480 && sw <= 3840 && shh <= 2400) snprintf(sz, sizeof sz, "%dx%d", sw, shh);
    qa_server = fork();
    if (qa_server == 0) {
        char log[PATH_MAX];
        snprintf(log, sizeof log, "%s/server.log", qa_dir);
        int lf = open(log, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600), null = open("/dev/null", O_RDONLY);
        dup2(null, 0);
        if (lf >= 0) { dup2(lf, 1); dup2(lf, 2); }
        if (chdir(bindir)) _exit(127);
        unsetenv("WAYLAND_DISPLAY"); unsetenv("DISPLAY"); unsetenv("LORD_SESSIOND"); unsetenv("LORDOS_SESSIOND");
        setenv("XDG_RUNTIME_DIR", qa_dir, 1);
        /* the demo identity (lord-server's --hands-qa answers "Alex Demo", "demo", "demo-pc" too): never the real account */
        setenv("USER", "demo", 1);
        setenv("LOGNAME", "demo", 1);
        setenv("HOSTNAME", "demo-pc", 1);
        setenv("HOME", home, 1);
        setenv("DBUS_SESSION_BUS_ADDRESS", addr, 1);
        setenv("LORD_GPU", "0", 1);
        setenv("LANG", "en_US.UTF-8", 1);
        /* LORDOS_MCP_TEST_HOOKS=1 with LORDOS_MCP_SERVER=…/build/lord-server-test: tests/hands_e2e.py plays the person
         * through the test hooks' socket (the installed lord-server has no such flag and refuses to start with it) */
        if (getenv("LORDOS_MCP_TEST_HOOKS"))
            execl(server, "lord-server", "--headless", sz, "--hands-qa", "--test-hooks", "--theme", theme, "--fonts", fonts, "--shell", shell, (char *)NULL);
        execl(server, "lord-server", "--headless", sz, "--hands-qa", "--theme", theme, "--fonts", fonts, "--shell", shell, (char *)NULL);
        _exit(127);
    }
    setenv("DBUS_SESSION_BUS_ADDRESS", addr, 1);
    /* the desktop is up when Hands1 answers Status (its shell answers too) */
    char why[256];
    for (int i = 0; i < 300; i++) {
        if (waitpid(qa_server, NULL, WNOHANG) == qa_server) { qa_server = 0; fprintf(stderr, "lordos-mcp: lord-server ended (see %s/server.log)\n", qa_dir); return -1; }
        if (!ready(why, sizeof why)) {
            DMsg m;
            if (call("Status", NULL, NULL, &m) == 1) { dbus_msg_free(&m); break; }
        } else if (bus_up) { dbus_close(&bus); bus_up = 0; }
        sleep_ms(200);
    }
    if (!granted) { fprintf(stderr, "lordos-mcp: the QA desktop didn't answer: %s\n", why); return -1; }
    if (app && app[0]) {
        char *r = tool_simple("OpenApp", app, NULL, NULL, 0);
        free(r);
        sleep_ms(800);
    }
    fprintf(stderr, "lordos-mcp: QA desktop ready (%s, demo home %s)\n", sz, home);
    return 0;
}

/* ---- main ---- */

static volatile sig_atomic_t stop_now;
static void on_stop(int s) { (void)s; stop_now = 1; }

static void usage(FILE *f) {
    fprintf(f, "usage: lordos-mcp [--headless APP] [--mode human|machine]\n"
               "  An MCP server (stdio) for AI agents to use the LordOS desktop (docs/HANDS.md).\n"
               "  claude mcp add -s user lordos-mcp -- lordos-mcp\n"
               "  --headless APP   a private desktop for QA with APP open (notes, files, …); nothing of yours is touched\n"
               "  --mode MODE      how the agent's cursor moves this session: human (glides, hovers, types at a person's pace;\n"
               "                   the live desktop's default) or machine (at once; the --headless default)\n");
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa = { .sa_handler = on_stop };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    const char *headless = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless") && i + 1 < argc && hands_name_ok(argv[i + 1])) headless = argv[++i];
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc && hands_mode_of(argv[i + 1]) > 0) session_mode = hands_mode_of(argv[++i]);
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(stdout); return 0; }
        else if (!strcmp(argv[i], "--version")) { puts("lordos-mcp (LordOS)"); return 0; }
        else { usage(stderr); return 2; }
    }
    if (headless) {
        snprintf(client_name, sizeof client_name, "QA agent");
        if (qa_start(headless)) return 1;
    }
    static char buf[LINE_MAX_BYTES + 1];
    size_t len = 0;
    while (!stop_now) {
        struct pollfd p = { 0, POLLIN, 0 };
        int r = poll(&p, 1, 500);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) continue;
        ssize_t k = read(0, buf + len, LINE_MAX_BYTES - len);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        len += (size_t)k;
        size_t start = 0;
        for (size_t i = 0; i < len; i++) {
            if (buf[i] != '\n') continue;
            buf[i] = 0;
            if (i > start) handle(buf + start, i - start);
            start = i + 1;
        }
        memmove(buf, buf + start, len - start);
        len -= start;
        if (len >= LINE_MAX_BYTES) len = 0;             /* a line too long: dropped */
    }
    if (bus_up) dbus_close(&bus);
    rec_clear();
    return 0;
}
