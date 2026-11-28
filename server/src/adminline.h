/* adminline.h — the privileged helper's reply lines (admin.c): building one and splitting a stream of them.
 *
 * A reply is one line, "<id> <KIND> <text>\n", at most ADMIN_LINE_MAX bytes (admin.h). build_line() cuts a longer
 * text to fit and turns its \n and \r into spaces, so a line never becomes two. The readers on every hop (an A_FORK
 * child's pipe, lord-netd's socket, lord-server's end) keep whole lines in a buffer of that size; a line that doesn't
 * fit is dropped whole, up to its newline, instead of being split (before #280 the tail of a cut line was read as a
 * line of its own, with whatever number it started with as its id). Header-only: admin.c and
 * tests/adminline_test.c. */
#ifndef LORD_ADMINLINE_H
#define LORD_ADMINLINE_H

#include "admin.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* "<id> <kind> <text>\n" in line (cap bytes, at least 4): the line's length, or 0 if the header itself doesn't fit */
static inline size_t adminline_vbuild(char *line, size_t cap, int id, const char *kind, const char *fmt, va_list ap) {
    if (cap < 4) return 0;
    int h = snprintf(line, cap, "%d %s ", id, kind);
    if (h < 0 || (size_t)h >= cap - 1) return 0;
    size_t room = cap - 1 - (size_t)h;                  /* the text and its 0, leaving a byte for the \n */
    int m = vsnprintf(line + h, room, fmt, ap);
    if (m < 0) return 0;
    size_t n = (size_t)h + ((size_t)m < room ? (size_t)m : room - 1);
    for (char *p = line + h; p < line + n; p++) if (*p == '\n' || *p == '\r' || !*p) *p = ' ';
    line[n++] = '\n';
    return n;
}

__attribute__((format(printf, 5, 6)))
static inline size_t adminline_build(char *line, size_t cap, int id, const char *kind, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    size_t n = adminline_vbuild(line, cap, id, kind, fmt, ap);
    va_end(ap);
    return n;
}

/* A reader's buffer: the caller reads into buf + len (at most cap - 1 - len bytes), adds what it read to len and
 * calls adminline_scan. */
typedef struct {
    char *buf;
    size_t cap, len;
    int skip;                           /* inside a line too long for buf: dropped up to its newline */
} AdminLineRd;

/* Calls fn for every whole line in the buffer, its \n replaced by a 0 (fn may put the \n back at line[n] while it
 * runs), and keeps the rest. A full buffer without a newline is dropped and the line's remainder after it too. */
static inline void adminline_scan(AdminLineRd *r, void (*fn)(char *line, size_t n, void *arg), void *arg) {
    size_t off = 0;
    char *nl;
    while ((nl = memchr(r->buf + off, '\n', r->len - off))) {
        size_t n = (size_t)(nl - (r->buf + off));
        if (r->skip) r->skip = 0;
        else { *nl = 0; fn(r->buf + off, n, arg); }
        off += n + 1;
    }
    if (off) { memmove(r->buf, r->buf + off, r->len - off); r->len -= off; }
    if (r->len >= r->cap - 1) { r->len = 0; r->skip = 1; }
}

#endif
