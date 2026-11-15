/* webinspect.c — see webinspect.h. */
#include "webinspect.h"
#include "../sec/json.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

int webinspect_parse(const char *text, size_t len) {
    if (!text || !len || len > WEBINSPECT_FILE_MAX) return 0;
    JDoc d;
    if (json_parse(text, len, &d, NULL, 0)) return 0;
    const JVal *v = d.root && d.root->type == J_OBJ ? json_get(d.root, "developerMode") : NULL;
    int on = v && v->type == J_BOOL && v->num != 0;
    json_free(&d);
    return on;
}

size_t webinspect_write(int on, char *out, size_t n) {
    int w = snprintf(out, n, "{\n  \"developerMode\": %s\n}\n", on ? "true" : "false");
    return w < 0 || (size_t)w >= n ? 0 : (size_t)w;
}

int webinspect_port_ok(unsigned port) { return port >= 1024 && port <= 65535; }

static int fmt(unsigned port, char *out, size_t n, const char *f) {
    if (!out || !n) return -1;
    out[0] = 0;
    if (!webinspect_port_ok(port)) return -1;
    int w = snprintf(out, n, f, port);
    if (w < 0 || (size_t)w >= n) { out[0] = 0; return -1; }
    return 0;
}

int webinspect_address(unsigned port, char *out, size_t n) { return fmt(port, out, n, WEBINSPECT_HOST ":%u"); }

static int ours(unsigned long inode, const unsigned long *inodes, int n) {
    for (int i = 0; inode && i < n; i++) if (inodes[i] == inode) return 1;
    return 0;
}

int webinspect_listening(const char *tcp, size_t len, unsigned port, const unsigned long *inodes, int n) {
    if (!tcp || !webinspect_port_ok(port)) return -1;
    /* the kernel prints the address as the 32-bit word it holds (network order) read in this machine's order */
    unsigned long loopback = (unsigned long)htonl(0x7F000001u);
    int found = 0;
    const char *p = tcp, *end = tcp + len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t l = nl ? (size_t)(nl - p) : (size_t)(end - p);
        char line[512];
        if (l < sizeof line) {
            memcpy(line, p, l);
            line[l] = 0;
            unsigned long addr = 0, inode = 0;
            unsigned lport = 0, st = 0;
            /* "  sl  local_address rem_address   st tx_queue:rx_queue tr:tm->when retrnsmt   uid  timeout inode" */
            if (sscanf(line, " %*u: %8lX:%4X %*8X:%*4X %2X %*s %*s %*s %*u %*u %lu", &addr, &lport, &st, &inode) == 4
                && lport == port && st == 0x0A) {
                if (!ours(inode, inodes, n) || addr != loopback) return -1;
                found = 1;
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    return found;
}
