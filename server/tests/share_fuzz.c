/* share_fuzz.c — the screen-sharing API's parsers under ASan+UBSan with random and mutated inputs: byte streams
 * through sw_next and sw_parse, as read_conn does, the UTF-8 walk and the grants' file. Built by `make -C server
 * test` with -fsanitize=address,undefined; any memory error or undefined behaviour aborts.
 * Usage: share-fuzz [iterations] [seed]. */
#include "remote/sharewire.h"
#include "remote/sharegrant.h"
#include "remote/proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long st = 88172645463325252ULL;
static unsigned rnd(void) { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return (unsigned)(st >> 11); }

static const int types[] = { SHARE_START, SHARE_QUERY, SHARE_STOP, SHARE_AWAKE, SHARE_LOCK, SHARE_TEXT, SHARE_HEAR, RP_KEYFRAME, RP_RATE, RP_RESIZE, RP_PTR,
                             RP_BTN, RP_WHEEL, RP_SCROLL, RP_KEY, RP_SETCLIP, RP_SEAT, RP_DISPLAY, RP_KEYRING, RP_HELLO, RP_VIDEO };
static const char *const seeds[] = {
    "{\"v\":1,\"grants\":[]}",
    "{\"v\":1,\"grants\":[{\"id\":\"desktop:a\",\"exe\":\"/bin/a\",\"perm\":\"screen-share\",\"state\":\"allow\"},\n"
    "{\"id\":\"lpk:b\",\"exe\":\"\",\"perm\":\"remote-input\",\"state\":\"deny\"}]}\n",
};

/* A stream of plausible and broken messages. */
static size_t stream(uint8_t *b, size_t cap) {
    size_t o = 0;
    int k = 1 + (int)(rnd() % 12);
    while (k-- && o + 64 < cap) {
        uint32_t n = rnd() % 4 == 0 ? rnd() % 48 : (const uint32_t[]){ 0, 1, 2, 3, 4, 8, 9 }[rnd() % 7];
        uint32_t len = n + 1;
        if (rnd() % 40 == 0) len = rnd() % 16 == 0 ? rnd() : rnd() % (SW_MAX_MSG + 4);
        memcpy(b + o, &len, 4);
        b[o + 4] = rnd() % 8 ? (uint8_t)types[rnd() % (sizeof types / sizeof types[0])] : (uint8_t)rnd();
        o += 5;
        for (uint32_t i = 0; i < n; i++) b[o++] = rnd() % 3 ? (uint8_t)rnd() : (uint8_t)"\0\1\2\x7f\x80\xc3\xa9\xed\xf4\xff"[rnd() % 10];
    }
    return o;
}

static size_t mutate(char *b, size_t n, size_t cap) {
    int k = 1 + (int)(rnd() % 8);
    while (k--) {
        unsigned op = rnd() % 4;
        if (op == 0 && n) b[rnd() % n] = (char)rnd();
        else if (op == 1 && n) b[rnd() % n] ^= (char)(1 << (rnd() % 8));
        else if (op == 2 && n + 1 < cap) { size_t p = n ? rnd() % n : 0; memmove(b + p + 1, b + p, n - p); b[p] = (char)rnd(); n++; }
        else if (op == 3 && n > 1) { size_t p = rnd() % n; memmove(b + p, b + p + 1, n - p - 1); n--; }
    }
    return n;
}

int main(int argc, char **argv) {
    long iters = argc > 1 ? atol(argv[1]) : 200000;
    if (argc > 2) st ^= strtoull(argv[2], NULL, 10);
    static SgStore s;
    long parsed = 0, ok = 0;
    for (long i = 0; i < iters; i++) {
        /* an exact-size heap copy, so a read one byte past the end is caught */
        uint8_t tmp[1024];
        size_t n = stream(tmp, sizeof tmp);
        uint8_t *b = malloc(n ? n : 1);
        memcpy(b, tmp, n);
        size_t off = 0;
        for (;;) {
            SwFrame f;
            SwMsg m;
            int r = sw_next(b + off, n - off, &f);
            if (r <= 0) break;
            parsed++;
            if (!sw_parse(&f, &m)) {
                ok++;
                if (m.text) {
                    uint32_t cp;
                    for (uint32_t p = 0; p < m.tlen;) { int l = sw_utf8_next(m.text + p, m.tlen - p, &cp); if (l <= 0) abort(); p += (uint32_t)l; }
                }
            }
            off += f.used;
        }
        free(b);
        if (!(i % 8)) {
            char t[2048];
            const char *seed = seeds[rnd() % 2];
            size_t l = strlen(seed);
            memcpy(t, seed, l);
            l = mutate(t, l, sizeof t);
            char *h = malloc(l ? l : 1);
            memcpy(h, t, l);
            if (!sg_parse(h, l, &s)) {
                for (int k = 0; k < s.n; k++) {
                    if (!sg_id_valid(s.e[k].id)) abort();      /* what parses is valid */
                    sg_get(&s, s.e[k].id, s.e[k].exe, s.e[k].perm);
                }
                static char out[SG_FILE_MAX];
                int w = sg_write(&s, out, sizeof out);
                static SgStore again;
                if (w <= 0 || sg_parse(out, (size_t)w, &again) || again.n != s.n) abort();   /* survives its own file */
            }
            free(h);
        }
    }
    printf("share-fuzz: %ld iterations, %ld messages parsed, %ld valid, no memory errors\n", iters, parsed, ok);
    return 0;
}
