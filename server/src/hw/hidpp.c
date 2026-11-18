/* hidpp.c — HID++ 2.0 battery questions (see hidpp.h). */
#include "hidpp.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void hidpp_request(uint8_t out[HIDPP_LONG_LEN], uint8_t feat, uint8_t fn, const uint8_t *params, int nparams) {
    memset(out, 0, HIDPP_LONG_LEN);
    out[0] = HIDPP_LONG;
    out[1] = 0xFF;                       /* the device itself (the kernel puts in a receiver's slot) */
    out[2] = feat;
    out[3] = (uint8_t)((fn & 0x0F) << 4 | HIDPP_SWID);
    if (params && nparams > 0) memcpy(out + 4, params, (size_t)(nparams > 16 ? 16 : nparams));
}

int hidpp_match(const uint8_t *rep, int len, uint8_t feat, uint8_t fn, uint8_t params[16]) {
    if (!rep || len < 7 || (rep[0] != HIDPP_SHORT && rep[0] != HIDPP_LONG)) return 0;
    const uint8_t fnsw = (uint8_t)((fn & 0x0F) << 4 | HIDPP_SWID);
    if (rep[2] == 0xFF && rep[3] == feat && rep[4] == fnsw) return -1;       /* HID++ 2.0 error */
    if (rep[2] == 0x8F && rep[3] == feat) return -1;                         /* HID++ 1.0 error */
    if (rep[2] != feat || rep[3] != fnsw) return 0;
    int n = len - 4;
    if (n > 16) n = 16;
    memset(params, 0, 16);
    memcpy(params, rep + 4, (size_t)n);
    return 1;
}

int hidpp_voltage_percent(int mv) {
    /* millivolts at each 10 % from 100 down to 0 (Solaar's li-ion curve) */
    static const int curve[11] = { 4186, 4067, 3989, 3922, 3859, 3800, 3734, 3690, 3651, 3606, 3500 };
    if (mv >= curve[0]) return 100;
    if (mv <= curve[10]) return 0;
    for (int i = 1; i < 11; i++) {
        if (mv >= curve[i]) {
            int hi = 100 - (i - 1) * 10, span = curve[i - 1] - curve[i];
            return hi - (int)((long)(curve[i - 1] - mv) * 10 / (span ? span : 1));
        }
    }
    return 0;
}

int hidpp_status_1004(uint8_t c) { return c == 0 ? 0 : c == 1 || c == 2 ? 1 : c == 3 ? 2 : -1; }
int hidpp_status_1000(uint8_t s) { return s == 0 ? 0 : s == 1 || s == 2 || s == 4 ? 1 : s == 3 ? 2 : -1; }

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* one question and its answer: 1 with params, -1 the device said no, 0 no answer in time */
static int ask(int fd, int timeout_ms, uint8_t feat, uint8_t fn, const uint8_t *p, int np, uint8_t params[16]) {
    uint8_t req[HIDPP_LONG_LEN];
    hidpp_request(req, feat, fn, p, np);
    ssize_t w = write(fd, req, sizeof req);
    if (w != (ssize_t)sizeof req) return 0;
    long end = now_ms() + timeout_ms;
    for (;;) {
        long left = end - now_ms();
        if (left <= 0) return 0;
        struct pollfd pf = { fd, POLLIN, 0 };
        int r = poll(&pf, 1, (int)left);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return 0;
        if (pf.revents & (POLLERR | POLLHUP | POLLNVAL)) return 0;
        uint8_t rep[64];
        ssize_t n = read(fd, rep, sizeof rep);
        if (n <= 0) return 0;
        int m = hidpp_match(rep, (int)n, feat, fn, params);
        if (m) return m;
    }
}

/* the index of a feature through the root feature; 0 when the device doesn't have it */
static int feature_index(int fd, int timeout_ms, uint16_t id) {
    uint8_t p[2] = { (uint8_t)(id >> 8), (uint8_t)id }, r[16];
    int got = ask(fd, timeout_ms, 0x00, 0, p, 2, r) == 1 ? r[0] : 0;
    return got;
}

int hidpp_battery(int fd, int timeout_ms, HidppBattery *b) {
    uint8_t r[16];
    int ix = feature_index(fd, timeout_ms, 0x1004);
    if (ix > 0 && ask(fd, timeout_ms, (uint8_t)ix, 0, NULL, 0, r) == 1 && (r[1] & 0x02)) {   /* state of charge given */
        if (ask(fd, timeout_ms, (uint8_t)ix, 1, NULL, 0, r) == 1 && r[0] <= 100) {
            *b = (HidppBattery){ r[0], hidpp_status_1004(r[2]), 0x1004 };
            return 0;
        }
    }
    ix = feature_index(fd, timeout_ms, 0x1000);
    if (ix > 0 && ask(fd, timeout_ms, (uint8_t)ix, 0, NULL, 0, r) == 1 && r[0] <= 100) {
        int st = hidpp_status_1000(r[2]);
        if (r[0] > 0) { *b = (HidppBattery){ r[0], st, 0x1000 }; return 0; }
        if (st == 1 || st == 2) { *b = (HidppBattery){ -1, st, 0x1000 }; return 0; }   /* charging: no level given then */
    }
    ix = feature_index(fd, timeout_ms, 0x1001);
    if (ix > 0 && ask(fd, timeout_ms, (uint8_t)ix, 0, NULL, 0, r) == 1) {
        int mv = r[0] << 8 | r[1];
        if (mv >= 2500 && mv <= 5000) {
            *b = (HidppBattery){ hidpp_voltage_percent(mv), r[2] & 0x80 ? 1 : 0, 0x1001 };
            return 0;
        }
    }
    return -1;
}
