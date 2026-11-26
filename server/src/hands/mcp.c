/* mcp.c — lordos-mcp's pure parts (see mcp.h) */
#include "mcp.h"
#include "hands.h"
#include "../sec/json.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

static const char TOOLS[] =
"{\"tools\":["
"{\"name\":\"windows_context\",\"description\":\"List the open windows of the LordOS desktop: id, app, title, display, focused, minimized, kind (native, linux, package) and frame. Start here, as tabs_context in a browser.\","
"\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
"{\"name\":\"open_app\",\"description\":\"Open an app by its id or its name (\\\"notes\\\", \\\"Files\\\"). Returns the new window's id.\","
"\"inputSchema\":{\"type\":\"object\",\"properties\":{\"app\":{\"type\":\"string\"}},\"required\":[\"app\"]}},"
"{\"name\":\"open_path\",\"description\":\"Open a file or folder of the home folder or a disk in its default app, through LordOS's own safe open path (a file that would run code is only shown in Files).\","
"\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"absolute path\"}},\"required\":[\"path\"]}},"
"{\"name\":\"close_window\",\"description\":\"Close a window (the app may ask about unsaved changes).\","
"\"inputSchema\":{\"type\":\"object\",\"properties\":{\"window_id\":{\"type\":\"string\"}},\"required\":[\"window_id\"]}},"
"{\"name\":\"read_window\",\"description\":\"Read a window's UI as a tree: one line per button, field, menu, list row, checkbox, tab or text, with its role, name, state, value (never a password's) and box (@x,y wxh in desktop points), and a ref_N to act on. window_id \\\"\\\" is the window in front (with the menu bar and open menus), \\\"desktop\\\" every window. A Linux app's window has no tree: take a screenshot of it.\","
"\"inputSchema\":{\"type\":\"object\",\"properties\":{\"window_id\":{\"type\":\"string\"},\"filter\":{\"type\":\"string\",\"enum\":[\"all\",\"interactive\"]},\"depth\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":60}}}},"
"{\"name\":\"find\",\"description\":\"Find UI elements by words (\\\"save button\\\", \\\"search field\\\", \\\"Import\\\") in a window or the desktop; returns matching lines with their refs.\","
"\"inputSchema\":{\"type\":\"object\",\"properties\":{\"window_id\":{\"type\":\"string\"},\"query\":{\"type\":\"string\"}},\"required\":[\"query\"]}},"
"{\"name\":\"computer\",\"description\":\"Use the mouse and keyboard and see the screen, through LordOS's own input path. Actions: screenshot (target \\\"display\\\", \\\"display:<id>\\\" or \\\"window:<id>\\\"), left_click, right_click, middle_click, double_click, triple_click, hover (at ref or coordinate), left_click_drag (start_coordinate to coordinate), type (text), key (text: chords such as \\\"ctrl+s\\\", \\\"Return\\\", \\\"ctrl+a Delete\\\"; repeat), scroll (ref or coordinate, scroll_direction, scroll_amount), zoom (region [x0, y0, x1, y1]: a closer screenshot), wait (duration seconds, at most 10). Coordinates are pixels of the last screenshot (desktop points before any). Password fields, permission and password prompts and the lock screen are never reachable; the person's own input pauses you for a few seconds.\","
"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
"\"action\":{\"type\":\"string\",\"enum\":[\"screenshot\",\"left_click\",\"right_click\",\"middle_click\",\"double_click\",\"triple_click\",\"hover\",\"left_click_drag\",\"type\",\"key\",\"scroll\",\"zoom\",\"wait\"]},"
"\"ref\":{\"type\":\"string\"},\"coordinate\":{\"type\":\"array\",\"items\":{\"type\":\"number\"},\"minItems\":2,\"maxItems\":2},"
"\"start_coordinate\":{\"type\":\"array\",\"items\":{\"type\":\"number\"},\"minItems\":2,\"maxItems\":2},"
"\"text\":{\"type\":\"string\"},\"repeat\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":20},"
"\"scroll_direction\":{\"type\":\"string\",\"enum\":[\"up\",\"down\",\"left\",\"right\"]},\"scroll_amount\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":30},"
"\"region\":{\"type\":\"array\",\"items\":{\"type\":\"number\"},\"minItems\":4,\"maxItems\":4},"
"\"duration\":{\"type\":\"number\",\"minimum\":0,\"maximum\":10},\"target\":{\"type\":\"string\"}},\"required\":[\"action\"]}},"
"{\"name\":\"set_value\",\"description\":\"Set a control's value directly: a text field or text area (the text), a checkbox or switch (true/false), a pop-up or option list (an option's value or label), a slider (a number). Never a password field.\","
"\"inputSchema\":{\"type\":\"object\",\"properties\":{\"ref\":{\"type\":\"string\"},\"value\":{\"type\":[\"string\",\"boolean\",\"number\"]}},\"required\":[\"ref\",\"value\"]}},"
"{\"name\":\"read_app_log\",\"description\":\"Read an app's own log lines (its log file, a web app's, or its lines of the shell's log), optionally only those holding a pattern.\","
"\"inputSchema\":{\"type\":\"object\",\"properties\":{\"app\":{\"type\":\"string\"},\"pattern\":{\"type\":\"string\"},\"max_lines\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":400}},\"required\":[\"app\"]}},"
"{\"name\":\"record\",\"description\":\"Record what you do as an animated GIF for a QA report: start, then every action adds a frame; stop; export returns the GIF, or with name (a bare file name ending in .gif, no folders) saves it as a new file in ~/Pictures/LordOS QA, the only place lordos-mcp writes.\","
"\"inputSchema\":{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"start\",\"stop\",\"export\"]},\"name\":{\"type\":\"string\"}},\"required\":[\"action\"]}}"
"]}";

const char *mcp_tools_json(void) { return TOOLS; }

const char *mcp_error_text(const char *error, const char *word, char *buf, size_t n) {
    const char *e = error ? error : "";
    const char *w = word && hands_id_ok(word) ? word : "";
    if (!strcmp(e, HANDS_ERR_OFF)) snprintf(buf, n, "Hands is off. Ask the person to turn on Settings > AI > Hands for this session, then try again.");
    else if (!strcmp(e, HANDS_ERR_ASKING)) snprintf(buf, n, "Waiting for the person to answer LordOS's prompt (This session / Deny) on their screen. Try again in a moment.");
    else if (!strcmp(e, HANDS_ERR_DENIED)) snprintf(buf, n, "The person denied access to this computer. Do not retry; ask them.");
    else if (!strcmp(e, HANDS_ERR_STOPPED)) snprintf(buf, n, "The person stopped the agent (Stop or Ctrl+Alt+Esc). Do not retry; ask them what to do.");
    else if (!strcmp(e, HANDS_ERR_ACTIVE)) snprintf(buf, n, "user is active: the person is using the computer right now. Wait a few seconds and try again.");
    else if (!strcmp(e, HANDS_ERR_REFUSED) && !strcmp(w, "protected-dialog"))
        snprintf(buf, n, "Refused (protected-dialog): a protected dialog is open; only the person answers it. Ask them.");
    else if (!strcmp(e, HANDS_ERR_REFUSED)) snprintf(buf, n, "Refused (%s): LordOS never lets an agent reach that place or field.", w[0] ? w : "refused");
    else if (!strcmp(e, HANDS_ERR_RATE)) snprintf(buf, n, "Too many actions: slow down and try again in a second.");
    else if (!strcmp(e, HANDS_ERR_NOTFOUND)) snprintf(buf, n, "Not found: the window or ref is gone. Read the window again for fresh refs.");
    else if (!strcmp(e, HANDS_ERR_ACCESS)) snprintf(buf, n, "Access denied: LordOS serves only programs of the person's own account.");
    else if (!strcmp(e, HANDS_ERR_ARGS)) snprintf(buf, n, "Invalid arguments.");
    else if (!strcmp(e, HANDS_ERR_BUSY)) snprintf(buf, n, "Too many agents are connected.");
    else if (!strcmp(e, "org.freedesktop.DBus.Error.ServiceUnknown") || !strcmp(e, "org.freedesktop.DBus.Error.NameHasNoOwner") ||
             !strcmp(e, "org.freedesktop.DBus.Error.UnknownObject") || !strcmp(e, "org.freedesktop.DBus.Error.UnknownMethod"))
        snprintf(buf, n, "No LordOS session answers on this bus (is this LordOS, and is the desktop running?).");
    else snprintf(buf, n, "The action failed.");
    return buf;
}

void mcp_shot_from_json(McpShot *s, const char *meta) {
    JDoc d;
    s->valid = 0;
    if (!meta || json_parse(meta, strlen(meta), &d, NULL, 0) || !d.root) return;
    s->x = json_num(d.root, "x", 0);
    s->y = json_num(d.root, "y", 0);
    s->w = json_num(d.root, "w", 0);
    s->h = json_num(d.root, "h", 0);
    s->width = (int)json_num(d.root, "width", 0);
    s->height = (int)json_num(d.root, "height", 0);
    s->valid = s->w > 0 && s->h > 0 && s->width > 0 && s->height > 0;
    json_free(&d);
}

void mcp_map_point(const McpShot *s, double px, double py, double *lx, double *ly) {
    if (!s || !s->valid) { *lx = px; *ly = py; return; }
    *lx = s->x + px * s->w / s->width;
    *ly = s->y + py * s->h / s->height;
}

char *mcp_base64(const uint8_t *p, size_t n) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *o = malloc((n + 2) / 3 * 4 + 1);
    if (!o) return NULL;
    size_t k = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        o[k++] = T[v >> 18 & 63];
        o[k++] = T[v >> 12 & 63];
        o[k++] = i + 1 < n ? T[v >> 6 & 63] : '=';
        o[k++] = i + 2 < n ? T[v & 63] : '=';
    }
    o[k] = 0;
    return o;
}

char *mcp_quote(const char *s) {
    size_t n = strlen(s ? s : "");
    char *o = malloc(n * 6 + 3);
    if (!o) return NULL;
    size_t k = 0;
    o[k++] = '"';
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        if (*p == '"' || *p == '\\') { o[k++] = '\\'; o[k++] = (char)*p; }
        else if (*p == '\n') { o[k++] = '\\'; o[k++] = 'n'; }
        else if (*p == '\t') { o[k++] = '\\'; o[k++] = 't'; }
        else if (*p < 0x20 || *p == 0x7f) k += (size_t)sprintf(o + k, "\\u%04x", *p);
        else o[k++] = (char)*p;
    }
    o[k++] = '"';
    o[k] = 0;
    return o;
}

/* ---- the export ---- */

int mcp_export_name_ok(const char *name) {
    size_t n = name ? strlen(name) : 0;
    if (n < 5 || n > 100 || name[0] == '.' || name[0] == ' ' || strcmp(name + n - 4, ".gif")) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '.' || c == '_' || c == '-')) return 0;
    }
    return !strstr(name, "..");
}

/* a folder under dirfd, made when missing, opened without following a link */
static int sub(int dirfd, const char *name) {
    if (mkdirat(dirfd, name, 0700) && errno != EEXIST) return -1;
    return openat(dirfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

int mcp_write_export(const char *home, const char *name, const uint8_t *d, size_t n, char *out, size_t on) {
    if (out && on) out[0] = 0;
    if (!home || home[0] != '/' || !mcp_export_name_ok(name)) { errno = EINVAL; return -1; }
    int h = open(home, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int pics = h >= 0 ? sub(h, "Pictures") : -1;
    int qa = pics >= 0 ? sub(pics, "LordOS QA") : -1;
    int e = errno;
    if (h >= 0) close(h);
    if (pics >= 0) close(pics);
    if (qa < 0) { errno = e; return -1; }
    int fd = openat(qa, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    e = errno;
    close(qa);
    if (fd < 0) { errno = e; return -1; }
    size_t done = 0;
    while (done < n) {
        ssize_t w = write(fd, d + done, n - done);
        if (w <= 0) { e = errno; close(fd); errno = e ? e : EIO; return -1; }
        done += (size_t)w;
    }
    if (close(fd)) return -1;
    if (out && on) snprintf(out, on, "%s/%s/%s", home, MCP_EXPORT_DIR, name);
    return 0;
}

/* ---- GIF89a ---- */

typedef struct { uint8_t *p; size_t n, cap; int oom; } Out;
static void put(Out *o, const void *d, size_t n) {
    if (o->oom) return;
    if (o->n + n > o->cap) {
        size_t c = o->cap ? o->cap * 2 : 65536;
        while (c < o->n + n) c *= 2;
        uint8_t *q = realloc(o->p, c);
        if (!q) { o->oom = 1; return; }
        o->p = q;
        o->cap = c;
    }
    memcpy(o->p + o->n, d, n);
    o->n += n;
}
static void put8(Out *o, int v) { uint8_t b = (uint8_t)v; put(o, &b, 1); }
static void put16(Out *o, int v) { put8(o, v & 255); put8(o, v >> 8 & 255); }

/* the bit writer of LZW codes, in sub-blocks of 255 bytes */
typedef struct { Out *o; uint8_t blk[255]; int nblk; uint32_t acc; int bits; } Bits;
static void bits_byte(Bits *b, uint8_t v) {
    b->blk[b->nblk++] = v;
    if (b->nblk == 255) { put8(b->o, 255); put(b->o, b->blk, 255); b->nblk = 0; }
}
static void bits_put(Bits *b, int code, int size) {
    b->acc |= (uint32_t)code << b->bits;
    b->bits += size;
    while (b->bits >= 8) { bits_byte(b, (uint8_t)b->acc); b->acc >>= 8; b->bits -= 8; }
}
static void bits_end(Bits *b) {
    if (b->bits > 0) bits_byte(b, (uint8_t)b->acc);
    if (b->nblk) { put8(b->o, b->nblk); put(b->o, b->blk, (size_t)b->nblk); }
    put8(b->o, 0);
}

/* 6 × 7 × 6 levels of red, green and blue */
static uint8_t pal_index(const uint8_t *rgb) { return (uint8_t)((rgb[0] * 6 / 256) * 42 + (rgb[1] * 7 / 256) * 6 + rgb[2] * 6 / 256); }

static void lzw(Out *o, const uint8_t *idx, size_t n) {
    enum { CLEAR = 256, END = 257, MAXC = 4096, HASH = 8192 };
    static int16_t hk[HASH];
    static int32_t hv[HASH];
    Bits b = { .o = o };
    put8(o, 8);
    int size = 9, next = 258;
    memset(hk, -1, sizeof hk);
    bits_put(&b, CLEAR, size);
    if (!n) { bits_put(&b, END, size); bits_end(&b); return; }
    int prefix = idx[0];
    for (size_t i = 1; i < n; i++) {
        int c = idx[i];
        int32_t key = prefix << 8 | c;
        unsigned h = ((unsigned)key * 2654435761u) >> 19 & (HASH - 1);
        int found = -1;
        while (hk[h] >= 0) { if (hv[h] == key) { found = hk[h]; break; } h = (h + 1) & (HASH - 1); }
        if (found >= 0) { prefix = found; continue; }
        bits_put(&b, prefix, size);
        if (next < MAXC) {
            hk[h] = (int16_t)next;
            hv[h] = key;
            if (next == (1 << size) && size < 12) size++;
            next++;
        } else {
            bits_put(&b, CLEAR, size);
            memset(hk, -1, sizeof hk);
            size = 9;
            next = 258;
        }
        prefix = c;
    }
    bits_put(&b, prefix, size);
    bits_put(&b, END, size);
    bits_end(&b);
}

uint8_t *mcp_gif(const McpFrame *f, int n, int delay_cs, size_t *len) {
    *len = 0;
    if (!f || n < 1 || f[0].w < 1 || f[0].h < 1 || f[0].w > 1024 || f[0].h > 1024) return NULL;
    int W = f[0].w, H = f[0].h;
    Out o = { 0 };
    put(&o, "GIF89a", 6);
    put16(&o, W);
    put16(&o, H);
    put8(&o, 0xf7);                         /* a global table of 256 colours */
    put8(&o, 0);
    put8(&o, 0);
    for (int i = 0; i < 256; i++) {
        int r = i / 42, g = i / 6 % 7, b = i % 6;
        uint8_t c[3] = { (uint8_t)(i < 252 ? r * 255 / 5 : 0), (uint8_t)(i < 252 ? g * 255 / 6 : 0), (uint8_t)(i < 252 ? b * 255 / 5 : 0) };
        put(&o, c, 3);
    }
    put(&o, "\x21\xff\x0bNETSCAPE2.0\x03\x01\x00\x00\x00", 19);   /* loop forever */
    uint8_t *idx = malloc((size_t)W * H);
    if (!idx) { free(o.p); return NULL; }
    for (int k = 0; k < n; k++) {
        if (!f[k].rgb || f[k].w < 1 || f[k].h < 1) continue;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                int sx = (int)((long)x * f[k].w / W), sy = (int)((long)y * f[k].h / H);
                idx[(size_t)y * W + x] = pal_index(f[k].rgb + ((size_t)sy * f[k].w + sx) * 3);
            }
        put(&o, "\x21\xf9\x04\x04", 4);     /* a graphic control: the delay, drawn over (disposal 1) */
        put16(&o, delay_cs);
        put8(&o, 0);
        put8(&o, 0);
        put8(&o, 0x2c);
        put16(&o, 0);
        put16(&o, 0);
        put16(&o, W);
        put16(&o, H);
        put8(&o, 0);
        lzw(&o, idx, (size_t)W * H);
    }
    free(idx);
    put8(&o, 0x3b);
    if (o.oom) { free(o.p); return NULL; }
    *len = o.n;
    return o.p;
}
