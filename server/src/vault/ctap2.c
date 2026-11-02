/* ctap2.c — our own FIDO2 client: CTAPHID over hidraw + CTAP2 with hmac-secret (see ctap2.h).
 *
 * Only read/write/poll on the fd that arrives already open (runs sandboxed in axia-vault). An
 * HID report is 64 bytes; when writing it is prefixed by report number 0 (65 bytes),
 * as hidraw requires for devices without report ids. */
#include "ctap2.h"
#include "cbor.h"
#include "p256.h"
#include "vcrypto.h"
#include "../pkg/crypto.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PKT 64
#define HID_PING 0x81
#define HID_INIT 0x86
#define HID_CBOR 0x90
#define HID_CANCEL 0x91
#define HID_KEEPALIVE 0xbb
#define HID_ERROR 0xbf
#define CAP_CBOR 0x04
#define MSG_MAX 7609

#define CMD_MAKE_CRED 0x01
#define CMD_GET_ASSERT 0x02
#define CMD_GET_INFO 0x04
#define CMD_CLIENT_PIN 0x06

/* channel in use (for fido_cancel from another thread) */
static volatile int cur_fd = -1;
static volatile uint32_t cur_cid;
static volatile int cancel_flag;

static long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* ---- reports ---- */

static int pkt_write(int fd, const uint8_t pkt[PKT]) {
    uint8_t b[PKT + 1];
    b[0] = 0;
    memcpy(b + 1, pkt, PKT);
    for (int tries = 0; tries < 50; tries++) {
        ssize_t w = write(fd, b, sizeof b);
        if (w == (ssize_t)sizeof b) return 0;
        if (w < 0 && (errno == EINTR)) continue;
        if (w < 0 && errno == EAGAIN) { struct pollfd p = { fd, POLLOUT, 0 }; poll(&p, 1, 100); continue; }
        return FIDO_ERR_IO;
    }
    return FIDO_ERR_IO;
}

/* 0 = ok, FIDO_ERR_TIMEOUT if nothing arrived within ms, FIDO_ERR_IO */
static int pkt_read(int fd, uint8_t pkt[PKT], int ms) {
    struct pollfd p = { fd, POLLIN, 0 };
    int r = poll(&p, 1, ms < 0 ? 0 : ms);
    if (r < 0) return errno == EINTR ? FIDO_ERR_TIMEOUT : FIDO_ERR_IO;
    if (r == 0) return FIDO_ERR_TIMEOUT;
    if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) return FIDO_ERR_IO;
    uint8_t b[PKT + 1];
    ssize_t n = read(fd, b, sizeof b);
    if (n < 0) return errno == EAGAIN || errno == EINTR ? FIDO_ERR_TIMEOUT : FIDO_ERR_IO;
    if (n == PKT) { memcpy(pkt, b, PKT); return 0; }
    if (n == PKT + 1 && b[0] == 0) { memcpy(pkt, b + 1, PKT); return 0; }   /* with report number */
    return FIDO_ERR_IO;
}

static void put_cid(uint8_t *p, uint32_t cid) { p[0] = (uint8_t)(cid >> 24); p[1] = (uint8_t)(cid >> 16); p[2] = (uint8_t)(cid >> 8); p[3] = (uint8_t)cid; }
static uint32_t get_cid(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int hid_send(int fd, uint32_t cid, uint8_t cmd, const uint8_t *data, size_t len) {
    if (len > MSG_MAX) return FIDO_ERR_PROTOCOL;
    uint8_t pkt[PKT];
    memset(pkt, 0, PKT);
    put_cid(pkt, cid);
    pkt[4] = cmd;
    pkt[5] = (uint8_t)(len >> 8);
    pkt[6] = (uint8_t)len;
    size_t k = len < PKT - 7 ? len : PKT - 7;
    if (k) memcpy(pkt + 7, data, k);
    int rc = pkt_write(fd, pkt);
    size_t off = k;
    for (uint8_t seq = 0; rc == 0 && off < len; seq++) {
        memset(pkt, 0, PKT);
        put_cid(pkt, cid);
        pkt[4] = seq;
        k = len - off < PKT - 5 ? len - off : PKT - 5;
        memcpy(pkt + 5, data + off, k);
        off += k;
        rc = pkt_write(fd, pkt);
    }
    explicit_bzero(pkt, sizeof pkt);
    return rc;
}

static int hid_err(uint8_t code) {
    switch (code) {
    case 0x01: return FIDO_ERR_NO_HMAC;        /* ERR_INVALID_CMD: does not speak CBOR */
    case 0x05: return FIDO_ERR_TIMEOUT;        /* ERR_MSG_TIMEOUT */
    case 0x06: return FIDO_ERR_IO;             /* ERR_CHANNEL_BUSY */
    default: return FIDO_ERR_PROTOCOL;
    }
}

/* Receives the response on channel cid. Ignores other channels; KEEPALIVE notifies via cb. */
static int hid_recv(int fd, uint32_t cid, uint8_t *cmd, uint8_t *buf, size_t cap, size_t *len,
                    long long deadline, FidoStatusCb cb, void *ud) {
    uint8_t pkt[PKT];
    size_t total = 0, got = 0;
    int seq = -1;
    for (;;) {
        long long left = deadline - now_ms();
        if (left <= 0) return FIDO_ERR_TIMEOUT;
        if (cancel_flag && seq < 0) return FIDO_ERR_CANCELLED;
        int rc = pkt_read(fd, pkt, left > 100 ? 100 : (int)left);
        if (rc == FIDO_ERR_TIMEOUT) continue;
        if (rc) return rc;
        if (get_cid(pkt) != cid) continue;
        if (pkt[4] & 0x80) {
            if (pkt[4] == HID_KEEPALIVE) {
                if (cb) cb(ud, pkt[7] == 2 ? FIDO_ST_TOUCH : FIDO_ST_PROCESSING);
                continue;
            }
            if (pkt[4] == HID_ERROR) return hid_err(pkt[7]);
            *cmd = pkt[4];
            total = (size_t)pkt[5] << 8 | pkt[6];
            if (total > cap || total > MSG_MAX) return FIDO_ERR_PROTOCOL;
            size_t k = total < PKT - 7 ? total : PKT - 7;
            memcpy(buf, pkt + 7, k);
            got = k;
            seq = 0;
        } else {
            if (seq < 0) continue;                 /* stray continuation: discarded */
            if (pkt[4] != seq) return FIDO_ERR_PROTOCOL;
            size_t k = total - got < PKT - 5 ? total - got : PKT - 5;
            memcpy(buf + got, pkt + 5, k);
            got += k;
            seq++;
        }
        if (seq >= 0 && got >= total) { *len = total; return 0; }
    }
}

/* CTAPHID_INIT on the broadcast channel: own channel and capabilities. */
static int hid_init(int fd, uint32_t *cid, uint8_t *caps) {
    uint8_t nonce[8], resp[64];
    if (fd != cur_fd) cancel_flag = 0;          /* a stale cancel does not abort a new request */
    if (vc_random(nonce, sizeof nonce)) return FIDO_ERR_IO;
    /* discard whatever is left over from before */
    uint8_t junk[PKT];
    for (int i = 0; i < 32 && pkt_read(fd, junk, 0) == 0; i++) {}
    int rc = hid_send(fd, 0xffffffffu, HID_INIT, nonce, sizeof nonce);
    if (rc) return rc;
    long long deadline = now_ms() + 2000;
    for (;;) {
        uint8_t cmd = 0;
        size_t len = 0;
        rc = hid_recv(fd, 0xffffffffu, &cmd, resp, sizeof resp, &len, deadline, NULL, NULL);
        if (rc) return rc;
        if (cmd != HID_INIT || len < 17) continue;
        if (memcmp(resp, nonce, 8)) continue;          /* response to another INIT */
        *cid = get_cid(resp + 8);
        if (caps) *caps = resp[16];
        if (*cid == 0 || *cid == 0xffffffffu) return FIDO_ERR_PROTOCOL;
        return 0;
    }
}

int ctaphid_command(int fd, uint8_t cmd, const uint8_t *data, size_t len,
                    uint8_t *resp, size_t cap, size_t *resp_len, int timeout_ms) {
    uint32_t cid;
    int rc = hid_init(fd, &cid, NULL);
    if (rc) return rc;
    rc = hid_send(fd, cid, cmd, data, len);
    if (rc) return rc;
    uint8_t rcmd = 0;
    rc = hid_recv(fd, cid, &rcmd, resp, cap, resp_len, now_ms() + (timeout_ms > 0 ? timeout_ms : 3000), NULL, NULL);
    if (rc == 0 && rcmd != cmd) rc = FIDO_ERR_PROTOCOL;
    return rc;
}

static int ctap_status(uint8_t s) {
    switch (s) {
    case 0x00: return FIDO_OK;
    case 0x01: return FIDO_ERR_NO_HMAC;            /* invalid command: not CTAP2 */
    case 0x2b: return FIDO_ERR_NO_HMAC;            /* unsupported extension */
    case 0x2d: return FIDO_ERR_CANCELLED;          /* KEEPALIVE_CANCEL */
    case 0x2e: return FIDO_ERR_NO_CREDENTIAL;
    case 0x2f: return FIDO_ERR_TIMEOUT;            /* USER_ACTION_TIMEOUT */
    case 0x27: return FIDO_ERR_DENIED;             /* OPERATION_DENIED */
    case 0x30: return FIDO_ERR_DENIED;             /* NOT_ALLOWED */
    case 0x31: return FIDO_ERR_PIN_INVALID;
    case 0x32: return FIDO_ERR_PIN_BLOCKED;
    case 0x33: return FIDO_ERR_PIN_INVALID;        /* PIN_AUTH_INVALID */
    case 0x34: return FIDO_ERR_PIN_BLOCKED;        /* PIN_AUTH_BLOCKED (must reconnect) */
    case 0x35: return FIDO_ERR_PIN_REQUIRED;       /* PIN_NOT_SET */
    case 0x36: return FIDO_ERR_PIN_REQUIRED;
    case 0x37: return FIDO_ERR_PIN_INVALID;        /* PIN_POLICY_VIOLATION */
    default: return FIDO_ERR_PROTOCOL;
    }
}

typedef struct { int fd; uint32_t cid; uint8_t caps; } Dev;

static int dev_open(Dev *d, int fd) {
    d->fd = fd;
    int rc = hid_init(fd, &d->cid, &d->caps);
    if (rc) return rc;
    if (!(d->caps & CAP_CBOR)) return FIDO_ERR_NO_HMAC;   /* U2F only */
    return 0;
}

/* CTAP2 command: req (CBOR) → resp (CBOR, without the status byte). */
static int ctap_cmd(Dev *d, uint8_t c, const uint8_t *req, size_t reqlen, uint8_t *resp, size_t cap, size_t *resplen,
                    int timeout_ms, FidoStatusCb cb, void *ud) {
    uint8_t msg[MSG_MAX];
    if (reqlen + 1 > sizeof msg) return FIDO_ERR_PROTOCOL;
    msg[0] = c;
    if (reqlen) memcpy(msg + 1, req, reqlen);
    cur_cid = d->cid;
    cur_fd = d->fd;
    cancel_flag = 0;
    int rc = hid_send(d->fd, d->cid, HID_CBOR, msg, reqlen + 1);
    explicit_bzero(msg, reqlen + 1);
    if (rc) { cur_fd = -1; return rc; }
    uint8_t cmd = 0;
    size_t len = 0;
    rc = hid_recv(d->fd, d->cid, &cmd, msg, sizeof msg, &len, now_ms() + timeout_ms, cb, ud);
    if (rc == FIDO_ERR_TIMEOUT || rc == FIDO_ERR_CANCELLED) {
        /* abort the wait on the key and listen to its response for a while */
        uint8_t pkt[PKT];
        memset(pkt, 0, PKT);
        put_cid(pkt, d->cid);
        pkt[4] = HID_CANCEL;
        pkt_write(d->fd, pkt);
        int was = rc;
        cancel_flag = 0;
        rc = hid_recv(d->fd, d->cid, &cmd, msg, sizeof msg, &len, now_ms() + 700, NULL, NULL);
        if (rc == 0 && len >= 1 && msg[0] != 0x2d && msg[0] != 0) { cur_fd = -1; rc = ctap_status(msg[0]); explicit_bzero(msg, len); return rc; }
        cur_fd = -1;
        explicit_bzero(msg, sizeof msg);
        return was;
    }
    cur_fd = -1;
    cancel_flag = 0;
    if (rc) return rc;
    if (cmd != HID_CBOR || len < 1) { explicit_bzero(msg, len); return FIDO_ERR_PROTOCOL; }
    rc = ctap_status(msg[0]);
    if (rc == 0) {
        if (len - 1 > cap) rc = FIDO_ERR_PROTOCOL;
        else { memcpy(resp, msg + 1, len - 1); *resplen = len - 1; }
    }
    explicit_bzero(msg, len);
    return rc;
}

void fido_cancel(int fd) {
    if (fd < 0 || fd != cur_fd) return;
    cancel_flag = 1;
    uint8_t pkt[PKT];
    memset(pkt, 0, PKT);
    put_cid(pkt, cur_cid);
    pkt[4] = HID_CANCEL;
    pkt_write(fd, pkt);
}

/* ---- getInfo ---- */

static int get_info(Dev *d, FidoInfo *info) {
    uint8_t resp[2048];
    size_t len = 0;
    memset(info, 0, sizeof *info);
    info->client_pin = -1;
    info->pin_retries = -1;
    int rc = ctap_cmd(d, CMD_GET_INFO, NULL, 0, resp, sizeof resp, &len, 3000, NULL, NULL);
    if (rc) return rc;
    CbItem it[160];
    Cb c;
    if (cb_parse(&c, resp, len, it, 160, 0) || cb_type(&c, 0) != CB_MAP) return FIDO_ERR_PROTOCOL;
    int v = cb_map_int(&c, 0, 1);
    size_t o = 0;
    for (int k = 0; cb_at(&c, v, k) >= 0; k++) {
        int s = cb_at(&c, v, k);
        if (cb_type(&c, s) != CB_TEXT) continue;
        size_t n = c.it[s].len;
        if (o + n + 2 > sizeof info->versions) break;
        if (o) info->versions[o++] = ',';
        memcpy(info->versions + o, c.it[s].ptr, n);
        o += n;
    }
    info->versions[o] = 0;
    int e = cb_map_int(&c, 0, 2);
    for (int k = 0; cb_at(&c, e, k) >= 0; k++) if (cb_text_eq(&c, cb_at(&c, e, k), "hmac-secret")) info->hmac_secret = 1;
    int a = cb_map_int(&c, 0, 3);
    if (cb_type(&c, a) == CB_BYTES && c.it[a].len == 16) memcpy(info->aaguid, c.it[a].ptr, 16);
    int op = cb_map_int(&c, 0, 4);
    int cp = cb_map_text(&c, op, "clientPin");
    if (cp >= 0) info->client_pin = cb_type(&c, cp) == CB_TRUE ? 1 : 0;
    int uv = cb_map_text(&c, op, "uv");
    info->uv = cb_type(&c, uv) == CB_TRUE;
    return 0;
}

static int pin_retries(Dev *d) {
    uint8_t req[16], resp[64];
    CbW w;
    cbw_init(&w, req, sizeof req);
    cbw_map(&w, 2);
    cbw_uint(&w, 1); cbw_uint(&w, 1);          /* pinProtocol 1 */
    cbw_uint(&w, 2); cbw_uint(&w, 1);          /* getRetries */
    size_t len = 0;
    if (ctap_cmd(d, CMD_CLIENT_PIN, req, w.n, resp, sizeof resp, &len, 3000, NULL, NULL)) return -1;
    CbItem it[16];
    Cb c;
    int64_t r;
    if (cb_parse(&c, resp, len, it, 16, 0) || cb_int(&c, cb_map_int(&c, 0, 3), &r)) return -1;
    return (int)r;
}

int fido_get_info(int fd, FidoInfo *info) {
    Dev d;
    int rc = dev_open(&d, fd);
    if (rc == FIDO_ERR_NO_HMAC) { memset(info, 0, sizeof *info); info->client_pin = -1; info->pin_retries = -1; return 0; }
    if (rc) return rc;
    rc = get_info(&d, info);
    if (rc == FIDO_ERR_NO_HMAC) return 0;      /* U2F key: hmac_secret stays 0 */
    if (rc == 0 && info->client_pin == 1) info->pin_retries = pin_retries(&d);
    return rc;
}

/* ---- PIN protocol 1 ---- */

static void cose_key(CbW *w, const uint8_t x[32], const uint8_t y[32]) {
    cbw_map(w, 5);
    cbw_int(w, 1); cbw_int(w, 2);              /* kty: EC2 */
    cbw_int(w, 3); cbw_int(w, -25);            /* alg: ECDH-ES+HKDF-256 */
    cbw_int(w, -1); cbw_int(w, 1);             /* crv: P-256 */
    cbw_int(w, -2); cbw_bytes(w, x, 32);
    cbw_int(w, -3); cbw_bytes(w, y, 32);
}

/* Key agreement with the key: shared = SHA-256(x of ECDH); mx/my = our public key. */
static int key_agreement(Dev *d, uint8_t shared[32], uint8_t mx[32], uint8_t my[32]) {
    uint8_t req[16], resp[256];
    CbW w;
    cbw_init(&w, req, sizeof req);
    cbw_map(&w, 2);
    cbw_uint(&w, 1); cbw_uint(&w, 1);
    cbw_uint(&w, 2); cbw_uint(&w, 2);          /* getKeyAgreement */
    size_t len = 0;
    int rc = ctap_cmd(d, CMD_CLIENT_PIN, req, w.n, resp, sizeof resp, &len, 3000, NULL, NULL);
    if (rc) return rc;
    CbItem it[32];
    Cb c;
    if (cb_parse(&c, resp, len, it, 32, 0)) return FIDO_ERR_PROTOCOL;
    int k = cb_map_int(&c, 0, 1);
    int x = cb_map_int(&c, k, -2), y = cb_map_int(&c, k, -3);
    if (cb_type(&c, x) != CB_BYTES || c.it[x].len != 32 || cb_type(&c, y) != CB_BYTES || c.it[y].len != 32) return FIDO_ERR_PROTOCOL;
    uint8_t priv[32], z[32];
    if (p256_keypair(priv, mx, my)) return FIDO_ERR_IO;
    rc = p256_ecdh(priv, c.it[x].ptr, c.it[y].ptr, z) ? FIDO_ERR_PROTOCOL : 0;
    if (rc == 0) sha256(z, 32, shared);
    explicit_bzero(priv, sizeof priv);
    explicit_bzero(z, sizeof z);
    return rc;
}

static int aes_iv0(const uint8_t key[32], const uint8_t *in, uint8_t *out, size_t len, int enc) {
    Aes a;
    uint8_t iv[16] = { 0 };
    aes_init(&a, key, 256);
    int rc = enc ? aes_cbc_encrypt(&a, iv, in, out, len) : aes_cbc_decrypt(&a, iv, in, out, len);
    aes_wipe(&a);
    return rc;
}

static int pin_token(Dev *d, const char *pin, uint8_t token[32], size_t *toklen) {
    uint8_t shared[32], mx[32], my[32], h[32], enc[16], req[200], resp[128];
    int rc = key_agreement(d, shared, mx, my);
    if (rc) return rc;
    sha256(pin, strlen(pin), h);
    aes_iv0(shared, h, enc, 16, 1);
    CbW w;
    cbw_init(&w, req, sizeof req);
    cbw_map(&w, 4);
    cbw_uint(&w, 1); cbw_uint(&w, 1);
    cbw_uint(&w, 2); cbw_uint(&w, 5);          /* getPINToken */
    cbw_uint(&w, 3); cose_key(&w, mx, my);
    cbw_uint(&w, 6); cbw_bytes(&w, enc, 16);
    size_t len = 0;
    rc = ctap_cmd(d, CMD_CLIENT_PIN, req, w.n, resp, sizeof resp, &len, 5000, NULL, NULL);
    explicit_bzero(h, sizeof h);
    if (rc == 0) {
        CbItem it[8];
        Cb c;
        int t;
        if (cb_parse(&c, resp, len, it, 8, 0) || cb_type(&c, (t = cb_map_int(&c, 0, 2))) != CB_BYTES ||
            (c.it[t].len != 16 && c.it[t].len != 32)) rc = FIDO_ERR_PROTOCOL;
        else { *toklen = c.it[t].len; aes_iv0(shared, c.it[t].ptr, token, *toklen, 0); }
    }
    explicit_bzero(shared, sizeof shared);
    explicit_bzero(resp, sizeof resp);
    return rc;
}

static int pin_auth(Dev *d, const char *pin, const uint8_t cdh[32], uint8_t auth[16]) {
    uint8_t tok[32], mac[32];
    size_t tl = 0;
    int rc = pin_token(d, pin, tok, &tl);
    if (rc) return rc;
    hmac_sha256(tok, tl, cdh, 32, mac);
    memcpy(auth, mac, 16);
    explicit_bzero(tok, sizeof tok);
    explicit_bzero(mac, sizeof mac);
    return 0;
}

int fido_set_pin(int fd, const char *pin) {
    size_t pl = strlen(pin);
    if (pl < 4 || pl > 63) return FIDO_ERR_PIN_INVALID;
    Dev d;
    int rc = dev_open(&d, fd);
    if (rc) return rc;
    uint8_t shared[32], mx[32], my[32], padded[64] = { 0 }, enc[64], mac[32], req[300], resp[64];
    rc = key_agreement(&d, shared, mx, my);
    if (rc) return rc;
    memcpy(padded, pin, pl);
    aes_iv0(shared, padded, enc, 64, 1);
    hmac_sha256(shared, 32, enc, 64, mac);
    CbW w;
    cbw_init(&w, req, sizeof req);
    cbw_map(&w, 5);
    cbw_uint(&w, 1); cbw_uint(&w, 1);
    cbw_uint(&w, 2); cbw_uint(&w, 3);          /* setPIN */
    cbw_uint(&w, 3); cose_key(&w, mx, my);
    cbw_uint(&w, 4); cbw_bytes(&w, mac, 16);
    cbw_uint(&w, 5); cbw_bytes(&w, enc, 64);
    size_t len = 0;
    rc = ctap_cmd(&d, CMD_CLIENT_PIN, req, w.n, resp, sizeof resp, &len, 5000, NULL, NULL);
    explicit_bzero(shared, sizeof shared);
    explicit_bzero(padded, sizeof padded);
    explicit_bzero(req, sizeof req);
    return rc;
}

/* ---- authData ---- */

typedef struct {
    uint8_t flags;
    const uint8_t *cred; size_t cred_len;
    const uint8_t *ext; size_t ext_len;
} AuthData;

static int parse_authdata(const uint8_t *a, size_t n, AuthData *o) {
    memset(o, 0, sizeof *o);
    if (n < 37) return FIDO_ERR_PROTOCOL;
    uint8_t rph[32];
    sha256(FIDO_RP_ID, strlen(FIDO_RP_ID), rph);
    if (memcmp(a, rph, 32)) return FIDO_ERR_PROTOCOL;   /* not from our rp.id */
    o->flags = a[32];
    size_t p = 37;
    if (o->flags & 0x40) {                               /* AT: credential data */
        if (n < p + 18) return FIDO_ERR_PROTOCOL;
        p += 16;                                         /* aaguid */
        size_t cl = (size_t)a[p] << 8 | a[p + 1];
        p += 2;
        if (cl == 0 || cl > FIDO_CRED_MAX || n - p < cl) return FIDO_ERR_PROTOCOL;
        o->cred = a + p;
        o->cred_len = cl;
        p += cl;
        CbItem it[32];
        Cb c;
        if (cb_parse(&c, a + p, n - p, it, 32, 1) || cb_type(&c, 0) != CB_MAP) return FIDO_ERR_PROTOCOL;   /* COSE key */
        p += c.used;
    }
    if (o->flags & 0x80) {                               /* ED: extensions */
        CbItem it[16];
        Cb c;
        if (cb_parse(&c, a + p, n - p, it, 16, 1) || cb_type(&c, 0) != CB_MAP) return FIDO_ERR_PROTOCOL;
        o->ext = a + p;
        o->ext_len = c.used;
        p += c.used;
    }
    return p == n ? 0 : FIDO_ERR_PROTOCOL;
}

static int prepare(int fd, Dev *d, const char *pin, int need_pin_for_make) {
    int rc = dev_open(d, fd);
    if (rc) return rc;
    FidoInfo info;
    rc = get_info(d, &info);
    if (rc) return rc;
    if (!info.hmac_secret) return FIDO_ERR_NO_HMAC;
    if (pin && info.client_pin != 1) return FIDO_ERR_PIN_INVALID;   /* extra PIN: the key has none */
    if (need_pin_for_make && info.client_pin == 1 && !pin) return FIDO_ERR_PIN_REQUIRED;
    return 0;
}

int fido_make_credential(int fd, const char *pin, uint8_t *cred, size_t *cred_len,
                         int timeout_ms, FidoStatusCb cb, void *ud) {
    Dev d;
    int rc = prepare(fd, &d, pin, 1);
    if (rc) return rc;
    uint8_t cdh[32], uid[16], auth[16];
    if (vc_random(cdh, 32) || vc_random(uid, 16)) return FIDO_ERR_IO;
    if (pin && (rc = pin_auth(&d, pin, cdh, auth))) return rc;
    uint8_t req[512];
    CbW w;
    cbw_init(&w, req, sizeof req);
    cbw_map(&w, pin ? 7 : 5);
    cbw_uint(&w, 1); cbw_bytes(&w, cdh, 32);
    cbw_uint(&w, 2); cbw_map(&w, 2);
    cbw_text(&w, "id"); cbw_text(&w, FIDO_RP_ID);
    cbw_text(&w, "name"); cbw_text(&w, "LordOS");
    cbw_uint(&w, 3); cbw_map(&w, 3);
    cbw_text(&w, "id"); cbw_bytes(&w, uid, 16);
    cbw_text(&w, "name"); cbw_text(&w, "vault");
    cbw_text(&w, "displayName"); cbw_text(&w, "Contrasenas");
    cbw_uint(&w, 4); cbw_array(&w, 1); cbw_map(&w, 2);
    cbw_text(&w, "alg"); cbw_int(&w, -7);
    cbw_text(&w, "type"); cbw_text(&w, "public-key");
    cbw_uint(&w, 6); cbw_map(&w, 1);
    cbw_text(&w, "hmac-secret"); cbw_bool(&w, 1);
    if (pin) {
        cbw_uint(&w, 8); cbw_bytes(&w, auth, 16);
        cbw_uint(&w, 9); cbw_uint(&w, 1);
    }
    if (w.err) return FIDO_ERR_PROTOCOL;
    uint8_t resp[2048];
    size_t len = 0;
    rc = ctap_cmd(&d, CMD_MAKE_CRED, req, w.n, resp, sizeof resp, &len, timeout_ms, cb, ud);
    if (rc) return rc;
    CbItem it[64];
    Cb c;
    if (cb_parse(&c, resp, len, it, 64, 0)) return FIDO_ERR_PROTOCOL;
    int ad = cb_map_int(&c, 0, 2);
    if (cb_type(&c, ad) != CB_BYTES) return FIDO_ERR_PROTOCOL;
    AuthData a;
    if ((rc = parse_authdata(c.it[ad].ptr, c.it[ad].len, &a))) return rc;
    if (!a.cred) return FIDO_ERR_PROTOCOL;
    /* the key must confirm hmac-secret in the extensions */
    int ok = 0;
    if (a.ext) {
        CbItem e[16];
        Cb ec;
        if (cb_parse(&ec, a.ext, a.ext_len, e, 16, 0) == 0) ok = cb_type(&ec, cb_map_text(&ec, 0, "hmac-secret")) == CB_TRUE;
    }
    if (!ok) return FIDO_ERR_NO_HMAC;
    memcpy(cred, a.cred, a.cred_len);
    *cred_len = a.cred_len;
    return 0;
}

int fido_hmac_secret(int fd, const char *pin, const uint8_t *cred, size_t cred_len,
                     const uint8_t salt[32], uint8_t out[32], int timeout_ms, FidoStatusCb cb, void *ud) {
    if (!cred_len || cred_len > FIDO_CRED_MAX) return FIDO_ERR_NO_CREDENTIAL;
    Dev d;
    int rc = prepare(fd, &d, pin, 0);
    if (rc) return rc;
    uint8_t shared[32], mx[32], my[32], senc[32], mac[32], cdh[32], auth[16];
    if ((rc = key_agreement(&d, shared, mx, my))) return rc;
    aes_iv0(shared, salt, senc, 32, 1);
    hmac_sha256(shared, 32, senc, 32, mac);
    if (vc_random(cdh, 32)) { rc = FIDO_ERR_IO; goto out; }
    if (pin && (rc = pin_auth(&d, pin, cdh, auth))) goto out;
    uint8_t req[1024];
    CbW w;
    cbw_init(&w, req, sizeof req);
    cbw_map(&w, pin ? 6 : 4);
    cbw_uint(&w, 1); cbw_text(&w, FIDO_RP_ID);
    cbw_uint(&w, 2); cbw_bytes(&w, cdh, 32);
    cbw_uint(&w, 3); cbw_array(&w, 1); cbw_map(&w, 2);
    cbw_text(&w, "id"); cbw_bytes(&w, cred, cred_len);
    cbw_text(&w, "type"); cbw_text(&w, "public-key");
    cbw_uint(&w, 4); cbw_map(&w, 1);
    cbw_text(&w, "hmac-secret"); cbw_map(&w, 3);
    cbw_uint(&w, 1); cose_key(&w, mx, my);
    cbw_uint(&w, 2); cbw_bytes(&w, senc, 32);
    cbw_uint(&w, 3); cbw_bytes(&w, mac, 16);
    if (pin) {
        cbw_uint(&w, 6); cbw_bytes(&w, auth, 16);
        cbw_uint(&w, 7); cbw_uint(&w, 1);
    }
    if (w.err) { rc = FIDO_ERR_PROTOCOL; goto out; }
    uint8_t resp[2048];
    size_t len = 0;
    rc = ctap_cmd(&d, CMD_GET_ASSERT, req, w.n, resp, sizeof resp, &len, timeout_ms, cb, ud);
    if (rc) goto out;
    CbItem it[64];
    Cb c;
    int ad;
    AuthData a;
    if (cb_parse(&c, resp, len, it, 64, 0) || cb_type(&c, (ad = cb_map_int(&c, 0, 2))) != CB_BYTES) { rc = FIDO_ERR_PROTOCOL; goto out; }
    if ((rc = parse_authdata(c.it[ad].ptr, c.it[ad].len, &a))) goto out;
    rc = FIDO_ERR_NO_HMAC;
    if (a.ext) {
        CbItem e[16];
        Cb ec;
        int h;
        if (cb_parse(&ec, a.ext, a.ext_len, e, 16, 0) == 0 && cb_type(&ec, (h = cb_map_text(&ec, 0, "hmac-secret"))) == CB_BYTES) {
            if (ec.it[h].len == 32) { aes_iv0(shared, ec.it[h].ptr, out, 32, 0); rc = 0; }
            else rc = FIDO_ERR_PROTOCOL;
        }
    }
    explicit_bzero(resp, sizeof resp);
out:
    explicit_bzero(shared, sizeof shared);
    explicit_bzero(auth, sizeof auth);
    return rc;
}

const char *fido_strerror(int err) {
    switch (err) {
    case FIDO_OK: return "ok";
    case FIDO_ERR_IO: return "io";
    case FIDO_ERR_TIMEOUT: return "timeout";
    case FIDO_ERR_PROTOCOL: return "protocol";
    case FIDO_ERR_NO_HMAC: return "no-hmac-secret";
    case FIDO_ERR_PIN_REQUIRED: return "pin-required";
    case FIDO_ERR_PIN_INVALID: return "pin-invalid";
    case FIDO_ERR_PIN_BLOCKED: return "pin-blocked";
    case FIDO_ERR_NO_CREDENTIAL: return "no-credential";
    case FIDO_ERR_CANCELLED: return "cancelled";
    case FIDO_ERR_DENIED: return "denied";
    default: return "error";
    }
}
