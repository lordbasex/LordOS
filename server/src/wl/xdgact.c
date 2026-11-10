/* xdgact.c — xdg-activation tokens: who may bring a window to the front.
 *
 * A window comes forward only when the person asked for it: LordOS launched the app (launcher, Open
 * with, a link, a notification's button: apps.c and notifyd.c put a token in what they start or tell),
 * or an app that had the keyboard or a recent click or key asked for one to hand to another app (a
 * link opened in the browser already running). Any other token, a stale one or one used twice only
 * marks the window as wanting attention (wl.c). The rules are KWin's and Mutter's: tokens are random,
 * short-lived and work once, and one an app asked for remembers that app and stops granting as soon as
 * the keyboard leaves it, so an app cannot ask while in front and use it later to steal the front. No Wayland types here, so the launcher and its tests link it alone. */
#include "xdgact.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char token[XACT_TOKEN_LEN + 1];     /* "" = free slot */
    double expires, issued;
    int granted;
    const void *owner;                  /* the app that asked; NULL = issued by LordOS */
    char app_id[128];
} Tok;
static Tok toks[XACT_MAX];

#define SERIALS 32
static struct { uint32_t serial; const void *client; double at; } serials[SERIALS];
static int next_serial;

double xact_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static int random_bytes(unsigned char *b, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = getrandom(b + got, n - got, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    if (got == n) return 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);     /* a kernel without getrandom */
    if (fd < 0) return -1;
    ssize_t r = read(fd, b, n);
    close(fd);
    return r == (ssize_t)n ? 0 : -1;
}

const char *xact_issue_for(double now_ms, int ttl_ms, int granted, const char *app_id, const void *client) {
    if (client && (ttl_ms <= 0 || ttl_ms > XACT_CLIENT_MS)) ttl_ms = XACT_CLIENT_MS;
    unsigned char rnd[XACT_TOKEN_LEN / 2];
    if (random_bytes(rnd, sizeof rnd)) return NULL;
    /* a free or expired slot, else the oldest */
    Tok *t = &toks[0];
    for (int i = 0; i < XACT_MAX; i++) {
        if (!toks[i].token[0] || toks[i].expires <= now_ms) { t = &toks[i]; break; }
        if (toks[i].issued < t->issued) t = &toks[i];
    }
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof rnd; i++) { t->token[2 * i] = hex[rnd[i] >> 4]; t->token[2 * i + 1] = hex[rnd[i] & 15]; }
    t->token[XACT_TOKEN_LEN] = 0;
    t->issued = now_ms;
    t->expires = now_ms + (ttl_ms > 0 ? ttl_ms : 0);
    t->granted = !!granted;
    t->owner = client;
    snprintf(t->app_id, sizeof t->app_id, "%s", app_id ? app_id : "");
    return t->token;
}

const char *xact_issue(double now_ms, int ttl_ms, int granted, const char *app_id) {
    return xact_issue_for(now_ms, ttl_ms, granted, app_id, NULL);
}

int xact_focus_moved(const void *client) {
    int n = 0;
    for (int i = 0; i < XACT_MAX; i++) {
        Tok *t = &toks[i];
        if (!t->token[0] || !t->owner || t->owner == client) continue;
        n += t->granted;
        t->granted = 0;                 /* kept, so using it still marks attention and uses it up */
    }
    return n;
}

int xact_take(const char *token, double now_ms, char *app_id, size_t n) {
    if (app_id && n) app_id[0] = 0;
    if (!token || strlen(token) != XACT_TOKEN_LEN) return XACT_UNKNOWN;
    for (int i = 0; i < XACT_MAX; i++) {
        Tok *t = &toks[i];
        if (!t->token[0] || strcmp(t->token, token)) continue;
        int r = t->expires <= now_ms ? XACT_STALE : t->granted ? XACT_OK : XACT_DENIED;
        if (app_id && n) snprintf(app_id, n, "%s", t->app_id);
        memset(t, 0, sizeof *t);        /* once */
        return r;
    }
    return XACT_UNKNOWN;
}

void xact_note_serial(uint32_t serial, const void *client, double now_ms) {
    if (!client) return;
    serials[next_serial].serial = serial;
    serials[next_serial].client = client;
    serials[next_serial].at = now_ms;
    next_serial = (next_serial + 1) % SERIALS;
}

int xact_serial_recent(uint32_t serial, const void *client, double now_ms) {
    if (!client) return 0;
    for (int i = 0; i < SERIALS; i++)
        if (serials[i].client == client && serials[i].serial == serial && now_ms - serials[i].at <= XACT_SERIAL_MS) return 1;
    return 0;
}

void xact_client_gone(const void *client) {
    if (!client) return;
    for (int i = 0; i < SERIALS; i++) if (serials[i].client == client) serials[i].client = NULL;
    for (int i = 0; i < XACT_MAX; i++)
        if (toks[i].token[0] && toks[i].owner == client) { toks[i].granted = 0; toks[i].owner = NULL; }
}

void xact_reset(void) {
    memset(toks, 0, sizeof toks);
    memset(serials, 0, sizeof serials);
    next_serial = 0;
}
