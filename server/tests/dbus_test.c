/* dbus_test.c — our own D-Bus and Secret Service (src/vault/dbus.c, secretsvc.c):
 *   - valid and invalid signatures, paths and names;
 *   - round trip of every type (write → build → validate → read) and alignment cases;
 *   - real messages captured with dbus-monitor --binary (secret-tool + libsecret), if present;
 *   - rejection of truncated and huge messages, and of invalid signatures, booleans, paths or padding;
 *   - "fuzz": flipped bytes and random cuts over valid messages (it never crashes);
 *   - the 1024-bit arithmetic against Python's pow() and RFC 2409 group 2;
 *   - the Secret Service end to end with a fake bus (socketpair + fork): plain and DH sessions
 *     (our own client: modexp + HKDF + AES-128-CBC), create, search, read, change, delete,
 *     properties, introspection, lock and unlock with a prompt (Prompt + Completed);
 *   - fds (issue #36, logind's Inhibit): a reply carrying one is delivered with it only on a connection
 *     that negotiated fds, refused everywhere else; an 'h' with no fds is refused. */
#include "vault/dbus.h"
#include "vault/secretsvc.h"
#include "vault/smem.h"
#include "vault/vcrypto.h"
#include "ss_memback.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails, total;
static void check(int ok, const char *what) { total++; if (!ok) { fails++; printf("FAIL: %s\n", what); } }

static void unhex(const char *h, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) { unsigned v; sscanf(h + 2 * i, "%2x", &v); out[i] = (uint8_t)v; }
}

/* ---------- signatures ---------- */
static void t_sigs(void) {
    const char *good[] = { "", "s", "a{sv}", "(oayays)", "a{o(oayays)}", "aoao", "a{ss}", "vo", "a(yv)", "aaaai", "(i(s(b)))", "sa{sv}as", NULL };
    const char *bad[] = { "a", "a{", "a{sv", "{sv}", "()", "(s", "a{vs}", "a{(s)s}", "z", "ss)", "a{s}", "a{sss}", NULL };
    for (int i = 0; good[i]; i++) check(dbus_sig_valid(good[i]), good[i]);
    for (int i = 0; bad[i]; i++) check(!dbus_sig_valid(bad[i]), bad[i]);
    char deep[80];
    memset(deep, 'a', 40);
    strcpy(deep + 40, "i");
    check(!dbus_sig_valid(deep), "signature nested too deep");
    check(dbus_path_valid("/") && dbus_path_valid("/org/freedesktop/secrets") && dbus_path_valid("/a_b/C9"), "valid paths");
    check(!dbus_path_valid("") && !dbus_path_valid("org") && !dbus_path_valid("/a/") && !dbus_path_valid("//a") && !dbus_path_valid("/a-b"), "invalid paths");
    check(dbus_name_valid("org.freedesktop.Secret.Service") && !dbus_name_valid("org") && !dbus_name_valid(".a.b") && !dbus_name_valid("a..b") && !dbus_name_valid("1a.b"), "names");
}

/* ---------- fds (logind's Inhibit reply, hw/sleeplock.c) ---------- */

/* A method return to serial 1 whose body is one 'h' (index 0); unix_fds 1, or 0 to leave the field out. */
static void fd_reply(DBuf *w, int unix_fds) {
    dw_init(w);
    dw_u8(w, 'l'); dw_u8(w, DBUS_RETURN); dw_u8(w, 0); dw_u8(w, 1);
    dw_u32(w, 4);                       /* body length */
    dw_u32(w, 7);                       /* serial */
    size_t mark = dw_arr_begin(w, 8);
    dw_struct(w); dw_u8(w, 5); dw_variant(w, "u"); dw_u32(w, 1);
    dw_struct(w); dw_u8(w, 8); dw_variant(w, "g"); dw_sig(w, "h");
    if (unix_fds) { dw_struct(w); dw_u8(w, 9); dw_variant(w, "u"); dw_u32(w, (uint32_t)unix_fds); }
    dw_arr_end(w, mark);
    dw_align(w, 8);
    dw_u32(w, 0);                       /* the fd's index */
}

static int send_with_fd(int sock, const DBuf *w, int fd) {
    struct iovec io = { w->p, w->n };
    union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int))]; } cb;
    memset(&cb, 0, sizeof cb);
    struct msghdr mh = { .msg_iov = &io, .msg_iovlen = 1 };
    if (fd >= 0) {
        mh.msg_control = cb.b;
        mh.msg_controllen = sizeof cb.b;
        struct cmsghdr *h = CMSG_FIRSTHDR(&mh);
        h->cmsg_level = SOL_SOCKET;
        h->cmsg_type = SCM_RIGHTS;
        h->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(h), &fd, sizeof fd);
    }
    return sendmsg(sock, &mh, 0) == (ssize_t)w->n ? 0 : -1;
}

static void t_fds(void) {
    check(dbus_sig_valid("h") && dbus_sig_valid("a{sh}"), "'h' is a valid signature");
    DBuf w;
    fd_reply(&w, 1);
    DMsg m;
    int declared = -1;
    check(dbus_msg_parse(w.p, w.n, &m) != 0, "a message with fds refused by dbus_msg_parse");
    check(dbus_msg_parse_fds(w.p, w.n, &m, 4, &declared) == 0 && declared == 1 && !strcmp(m.sig, "h") && m.n_fds == 0,
          "dbus_msg_parse_fds takes it (the fds are attached by the connection)");
    dbus_msg_free(&m);
    DBuf w0;
    fd_reply(&w0, 0);
    check(dbus_msg_parse_fds(w0.p, w0.n, &m, 4, &declared) != 0, "an 'h' with no fds is refused");
    dw_free(&w0);

    for (int negotiated = 0; negotiated < 2; negotiated++) {
        int sv[2], pp[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) || pipe(pp)) { check(0, "socketpair/pipe"); continue; }
        DConn c;
        memset(&c, 0, sizeof c);
        c.fd = sv[0];
        c.unix_fds = negotiated;
        check(send_with_fd(sv[1], &w, pp[1]) == 0, "sendmsg with an fd");
        close(pp[1]);
        int r = dbus_next(&c, &m);
        if (negotiated) {
            check(r == 1 && m.n_fds == 1 && m.fds[0] >= 0 && m.type == DBUS_RETURN && m.reply_serial == 1,
                  "fds negotiated: the reply comes with its fd");
            int fd = m.n_fds == 1 ? m.fds[0] : -1;
            check(fd >= 0 && (fcntl(fd, F_GETFD) & FD_CLOEXEC), "the fd received is close-on-exec");
            if (fd >= 0) check(write(fd, "x", 1) == 1, "the fd is the pipe's write end");
            dbus_msg_free(&m);
            check(fd < 0 || fcntl(fd, F_GETFD) < 0, "dbus_msg_free closes it");
            char ch;
            check(read(pp[0], &ch, 1) == 1 && read(pp[0], &ch, 1) == 0, "no copy left open");
        } else {
            check(r == -1, "fds not negotiated: the message is refused");
            if (r == 1) dbus_msg_free(&m);
        }
        close(pp[0]);
        close(sv[1]);
        dbus_close(&c);
    }
    dw_free(&w);
}

/* ---------- round trip ---------- */
static int build_parse(DHdr *h, DBuf *body, DMsg *m) {
    DBuf out;
    if (dbus_build(h, body, &out)) { dw_free(&out); return -1; }
    long sz = dbus_msg_size(out.p, out.n);
    int r = sz == (long)out.n ? dbus_msg_parse(out.p, out.n, m) : -1;
    dw_free(&out);
    return r;
}

static void t_roundtrip(void) {
    DBuf b;
    dw_init(&b);
    dw_u8(&b, 0xab);
    dw_bool(&b, 1);
    dw_i32(&b, -5);
    dw_u32(&b, 0xdeadbeef);
    dw_u64(&b, 0x0123456789abcdefULL);
    dw_str(&b, "hola, ñandú");
    dw_str(&b, "/org/freedesktop/secrets");
    dw_sig(&b, "a{sv}");
    uint8_t raw[300];
    for (int i = 0; i < 300; i++) raw[i] = (uint8_t)i;
    dw_bytes(&b, raw, 300);
    size_t mk = dw_arr_begin(&b, 8);            /* a{ss} */
    for (int i = 0; i < 3; i++) { dw_struct(&b); dw_str(&b, i ? "k" : "service"); dw_str(&b, "v"); }
    dw_arr_end(&b, mk);
    mk = dw_arr_begin(&b, 8);                   /* a{sv} */
    dw_struct(&b); dw_str(&b, "Label"); dw_variant(&b, "s"); dw_str(&b, "x");
    dw_struct(&b); dw_str(&b, "Num"); dw_variant(&b, "t"); dw_u64(&b, 7);
    dw_struct(&b); dw_str(&b, "Arr"); dw_variant(&b, "ao");
    size_t mk2 = dw_arr_begin(&b, 4); dw_str(&b, "/a"); dw_str(&b, "/b"); dw_arr_end(&b, mk2);
    dw_arr_end(&b, mk);
    dw_struct(&b);                               /* (oayays) */
    dw_str(&b, "/s/1"); dw_bytes(&b, raw, 16); dw_bytes(&b, raw, 5); dw_str(&b, "text/plain");
    mk = dw_arr_begin(&b, 8);                   /* empty ax: still aligned to 8 */
    dw_arr_end(&b, mk);
    const char *sig = "ybiuts" "og" "aya{ss}a{sv}(oayays)ax";
    DHdr h = { .type = DBUS_CALL, .serial = 9, .path = "/test", .iface = "a.b", .member = "M", .dest = "x.y", .sig = sig, .sender = ":1.1" };
    DMsg m;
    int r = build_parse(&h, &b, &m);
    check(r == 0, "round trip: build and validate");
    if (r) { dw_free(&b); return; }
    check(!strcmp(m.path, "/test") && !strcmp(m.iface, "a.b") && !strcmp(m.member, "M") && !strcmp(m.dest, "x.y") && !strcmp(m.sender, ":1.1") && !strcmp(m.sig, sig) && m.serial == 9, "header fields");
    DRd d;
    dr_init(&d, &m);
    check(dr_u8(&d) == 0xab && dr_bool(&d) == 1 && dr_i32(&d) == -5 && dr_u32(&d) == 0xdeadbeef && dr_u64(&d) == 0x0123456789abcdefULL, "numbers");
    check(!strcmp(dr_str(&d, NULL), "hola, ñandú") && !strcmp(dr_str(&d, NULL), "/org/freedesktop/secrets") && !strcmp(dr_sig(&d), "a{sv}"), "strings");
    uint32_t n;
    const uint8_t *p = dr_bytes(&d, &n);
    check(n == 300 && !memcmp(p, raw, 300), "ay");
    size_t end = dr_arr(&d, 8);
    int k = 0;
    while (dr_more(&d, end)) { dr_struct(&d); dr_str(&d, NULL); dr_str(&d, NULL); k++; }
    check(k == 3, "a{ss}");
    end = dr_arr(&d, 8);
    k = 0;
    while (dr_more(&d, end)) { dr_struct(&d); dr_str(&d, NULL); const char *vs = dr_variant(&d); dr_skip(&d, vs); k++; }
    check(k == 3, "a{sv} with dr_skip");
    dr_struct(&d);
    check(!strcmp(dr_str(&d, NULL), "/s/1"), "(oayays) o");
    dr_bytes(&d, &n); check(n == 16, "(oayays) params");
    dr_bytes(&d, &n); check(n == 5, "(oayays) value");
    check(!strcmp(dr_str(&d, NULL), "text/plain"), "(oayays) s");
    end = dr_arr(&d, 8);
    check(!dr_more(&d, end) && !d.err && d.pos == d.len, "empty ax and exact end");
    dbus_msg_free(&m);
    dw_free(&b);

    /* alignment: y followed by t (7 bytes of padding), empty body without a signature */
    dw_init(&b);
    dw_u8(&b, 1);
    dw_u64(&b, 2);
    check(b.n == 16 && b.p[1] == 0 && b.p[7] == 0 && b.p[8] == 2, "y→t padding");
    DHdr h2 = { .type = DBUS_RETURN, .serial = 3, .reply_serial = 2, .sig = "yt" };
    check(build_parse(&h2, &b, &m) == 0, "valid yt");
    dbus_msg_free(&m);
    dw_free(&b);
    DHdr h3 = { .type = DBUS_SIGNAL, .serial = 4, .path = "/", .iface = "a.b", .member = "S" };
    check(build_parse(&h3, NULL, &m) == 0 && m.body_len == 0 && !strcmp(m.sig, ""), "signal without a body");
    dbus_msg_free(&m);
}

/* ---------- rejections ---------- */
static uint8_t *valid_msg(size_t *len) {
    DBuf b;
    dw_init(&b);
    dw_str(&b, "hola");
    dw_bool(&b, 0);
    size_t mk = dw_arr_begin(&b, 8);
    dw_struct(&b); dw_str(&b, "a"); dw_variant(&b, "s"); dw_str(&b, "b");
    dw_arr_end(&b, mk);
    DHdr h = { .type = DBUS_CALL, .serial = 1, .path = "/x", .member = "M", .sig = "sba{sv}" };
    DBuf out;
    dbus_build(&h, &b, &out);
    dw_free(&b);
    uint8_t *p = malloc(out.n);
    memcpy(p, out.p, out.n);
    *len = out.n;
    dw_free(&out);
    return p;
}

static void t_reject(void) {
    size_t len;
    uint8_t *v = valid_msg(&len);
    DMsg m;
    check(dbus_msg_parse(v, len, &m) == 0, "valid base");
    dbus_msg_free(&m);
    int ok = 1;
    for (size_t i = 0; i < len; i++) {
        long s = dbus_msg_size(v, i);
        if (s > 0 && s == (long)i) ok = 0;
        if (dbus_msg_parse(v, i, &m) == 0) { ok = 0; dbus_msg_free(&m); }
    }
    check(ok, "every cut is rejected");
    uint8_t *w = malloc(len);
    /* huge body length */
    memcpy(w, v, len); w[4] = 0xff; w[5] = 0xff; w[6] = 0xff; w[7] = 0x7f;
    check(dbus_msg_size(w, len) == -1, "2 GB body");
    /* bad version / bad byte order / bad type / serial 0 */
    memcpy(w, v, len); w[3] = 2; check(dbus_msg_parse(w, len, &m) != 0, "version 2");
    memcpy(w, v, len); w[0] = 'x'; check(dbus_msg_parse(w, len, &m) != 0, "byte order");
    memcpy(w, v, len); w[1] = 9; check(dbus_msg_parse(w, len, &m) != 0, "type 9");
    memcpy(w, v, len); memset(w + 8, 0, 4); check(dbus_msg_parse(w, len, &m) != 0, "serial 0");
    /* find things in the message and break them: the boolean (last u32 before the array), the signature */
    uint8_t *sg = memmem(w, len, "sba{sv}", 7);
    memcpy(w, v, len); sg = memmem(w, len, "sba{sv}", 7);
    if (sg) { sg[3] = '('; check(dbus_msg_parse(w, len, &m) != 0, "broken signature"); }
    memcpy(w, v, len); sg = memmem(w, len, "hola", 4);
    if (sg) {
        sg[1] = 0; check(dbus_msg_parse(w, len, &m) != 0, "NUL inside a string");
        memcpy(w, v, len);
        size_t bo = (size_t)(sg - w) + 5;           /* after "hola\0" comes the aligned boolean */
        bo = (bo + 3) & ~(size_t)3;
        w[bo] = 2; check(dbus_msg_parse(w, len, &m) != 0, "boolean 2");
        memcpy(w, v, len);
        w[(size_t)(sg - w) + 5] = 7;                /* non-zero padding */
        check(dbus_msg_parse(w, len, &m) != 0, "non-zero padding");
    }
    memcpy(w, v, len); sg = memmem(w, len, "/x", 2);
    if (sg) { sg[1] = '-'; check(dbus_msg_parse(w, len, &m) != 0, "invalid path"); }
    /* call without a member */
    DBuf out;
    DHdr h = { .type = DBUS_CALL, .serial = 1, .path = "/x" };
    dbus_build(&h, NULL, &out);
    check(dbus_msg_parse(out.p, out.n, &m) != 0, "call without a member");
    dw_free(&out);
    /* error without reply_serial */
    DHdr he = { .type = DBUS_ERROR, .serial = 1, .error = "a.b" };
    dbus_build(&he, NULL, &out);
    check(dbus_msg_parse(out.p, out.n, &m) != 0, "error without reply_serial");
    dw_free(&out);
    /* body without a signature */
    DBuf b;
    dw_init(&b);
    dw_u32(&b, 1);
    DHdr hn = { .type = DBUS_RETURN, .serial = 1, .reply_serial = 1 };
    dbus_build(&hn, &b, &out);
    check(dbus_msg_parse(out.p, out.n, &m) != 0, "body without a signature");
    dw_free(&out);
    /* array whose length runs past the end, and a variant with two types */
    dw_free(&b); dw_init(&b);
    dw_u32(&b, 1000);
    DHdr ha = { .type = DBUS_RETURN, .serial = 1, .reply_serial = 1, .sig = "ay" };
    dbus_build(&ha, &b, &out);
    check(dbus_msg_parse(out.p, out.n, &m) != 0, "array longer than the body");
    dw_free(&out);
    dw_free(&b); dw_init(&b);
    dw_sig(&b, "ss"); dw_str(&b, "a"); dw_str(&b, "b");
    DHdr hv = { .type = DBUS_RETURN, .serial = 1, .reply_serial = 1, .sig = "v" };
    dbus_build(&hv, &b, &out);
    check(dbus_msg_parse(out.p, out.n, &m) != 0, "variant with two types");
    dw_free(&out);
    /* endlessly nested variants */
    dw_free(&b); dw_init(&b);
    for (int i = 0; i < 40; i++) dw_sig(&b, "v");
    dw_sig(&b, "y"); dw_u8(&b, 1);
    dbus_build(&hv, &b, &out);
    check(dbus_msg_parse(out.p, out.n, &m) != 0, "too many nested variants");
    dw_free(&out);
    dw_free(&b);
    free(w);
    free(v);
}

/* ---------- fuzz ---------- */
static uint64_t rs = 88172645463325252ULL;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)rs; }

static void walk(const DMsg *m) {
    DRd r;
    dr_init(&r, m);
    size_t p = 0, n = strlen(m->sig);
    while (p < n && !r.err) {
        size_t k;
        if (dbus_sig_single(m->sig + p, &k)) break;
        char one[256];
        memcpy(one, m->sig + p, k);
        one[k] = 0;
        dr_skip(&r, one);
        p += k;
    }
}

static int fuzz_one(const uint8_t *base, size_t len, int rounds) {
    uint8_t *w = malloc(len + 64);
    int accepted = 0;
    for (int it = 0; it < rounds; it++) {
        size_t l = len;
        memcpy(w, base, len);
        int flips = 1 + (int)(rnd() % 4);
        for (int f = 0; f < flips; f++) {
            size_t at = rnd() % len;
            switch (rnd() % 4) {
            case 0: w[at] ^= (uint8_t)(1u << (rnd() % 8)); break;
            case 1: w[at] = (uint8_t)rnd(); break;
            case 2: w[at] = (uint8_t)(rnd() % 2 ? 0xff : 0); break;
            case 3: l = rnd() % (len + 1); break;
            }
        }
        long sz = dbus_msg_size(w, l);
        DMsg m;
        if (sz > 0 && (size_t)sz <= l && dbus_msg_parse(w, (size_t)sz, &m) == 0) { accepted++; walk(&m); dbus_msg_free(&m); }
    }
    free(w);
    return accepted;
}

/* ---------- real captures ---------- */
static int t_capture(const char *file, uint8_t ***msgs, size_t **lens) {
    FILE *f = fopen(file, "rb");
    if (!f) return -1;
    static uint8_t buf[1 << 20];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    int count = 0, bad = 0;
    *msgs = NULL;
    *lens = NULL;
    for (size_t p = 0; p + 16 <= n;) {
        long s = dbus_msg_size(buf + p, n - p);
        if (s <= 0 || (size_t)s > n - p) { bad++; break; }
        DMsg m;
        if (dbus_msg_parse(buf + p, (size_t)s, &m)) bad++;
        else {
            walk(&m);
            dbus_msg_free(&m);
            *msgs = realloc(*msgs, sizeof(uint8_t *) * (size_t)(count + 1));
            *lens = realloc(*lens, sizeof(size_t) * (size_t)(count + 1));
            (*msgs)[count] = malloc((size_t)s);
            memcpy((*msgs)[count], buf + p, (size_t)s);
            (*lens)[count] = (size_t)s;
            count++;
        }
        p += (size_t)s;
    }
    check(!bad, "captures: every message valid");
    return count;
}

/* ---------- 1024 bits ---------- */
static const char *const MODEXP[][4] = {
#include "secretsvc/modexp_vectors.h"
};

static void t_modexp(void) {
    int n = (int)(sizeof MODEXP / sizeof MODEXP[0]), ok = 1;
    for (int i = 0; i < n; i++) {
        uint8_t b[128], e[128], m[128], want[128], got[128];
        unhex(MODEXP[i][0], b, 128); unhex(MODEXP[i][1], e, 128); unhex(MODEXP[i][2], m, 128); unhex(MODEXP[i][3], want, 128);
        if (ss_dh_modexp(b, e, m, got) || memcmp(got, want, 128)) { ok = 0; printf("  modexp vector %d\n", i); }
    }
    check(ok, "modexp against Python's pow()");
    check(!memcmp(ss_dh_prime(), (const uint8_t *)"\xff\xff\xff\xff\xff\xff\xff\xff\xc9\x0f\xda\xa2", 12) && ss_dh_prime()[127] == 0xff, "group 2 prime");
    uint8_t even[128] = { 0 }, one[128] = { 0 }, out[128];
    even[127] = 4; one[127] = 1;
    check(ss_dh_modexp(one, one, even, out) != 0, "even modulus rejected");
    double t0 = (double)clock();
    for (int i = 0; i < 10; i++) ss_dh_modexp(one, (const uint8_t *)MODEXP[0][1], ss_dh_prime(), out);
    printf("1024-bit modexp: %.2f ms\n", ((double)clock() - t0) / CLOCKS_PER_SEC * 100.0);
}

/* ---------- Secret Service with a fake bus ---------- */
static DConn bus;
static uint32_t bserial = 100;
static int completed_seen, completed_dismissed, item_created_seen;

static int bus_read_line(int fd, char *out, size_t cap) {
    size_t n = 0;
    while (n + 1 < cap) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 5000) <= 0) return -1;
        if (read(fd, out + n, 1) != 1) return -1;
        n++;
        if (n >= 2 && out[n - 2] == '\r' && out[n - 1] == '\n') { out[n - 2] = 0; return 0; }
    }
    return -1;
}

static void bus_send(DHdr *h, DBuf *b) { h->serial = ++bserial; dbus_send(&bus, h, b); }

/* waits for the reply to serial (answers the service's calls to the bus with an error, takes note of
 * the signals). 1 RETURN, 0 ERROR, -1 nothing */
static int bus_wait(uint32_t serial, DMsg *m, int ms) {
    double end = (double)time(NULL) + ms / 1000.0 + 1;
    while ((double)time(NULL) < end) {
        dbus_flush(&bus);
        int r = dbus_next(&bus, m);
        if (r < 0) return -1;
        if (r == 0) { struct pollfd p = { bus.fd, POLLIN, 0 }; poll(&p, 1, 100); continue; }
        if (m->type == DBUS_SIGNAL) {
            if (!strcmp(m->member, "Completed")) { completed_seen++; DRd d; dr_init(&d, m); completed_dismissed = dr_bool(&d); }
            if (!strcmp(m->member, "ItemCreated")) item_created_seen++;
            dbus_msg_free(m);
            if (!serial) return 2;
            continue;
        }
        if (m->type == DBUS_CALL) {      /* GetConnectionUnixProcessID: answer 4242 */
            if (!strcmp(m->member, "GetConnectionUnixProcessID")) {
                DBuf b; dw_init(&b); dw_u32(&b, 4242);
                DHdr h = { .type = DBUS_RETURN, .reply_serial = m->serial, .dest = ":1.7", .sig = "u", .sender = "org.freedesktop.DBus" };
                bus_send(&h, &b);
                dw_free(&b);
            }
            dbus_msg_free(m);
            continue;
        }
        if (m->reply_serial == serial) return m->type == DBUS_RETURN ? 1 : 0;
        dbus_msg_free(m);
    }
    return -1;
}

static int call(const char *sender, const char *path, const char *iface, const char *member, const char *sig, DBuf *body, DMsg *m) {
    DHdr h = { .type = DBUS_CALL, .path = path, .iface = iface, .member = member, .dest = ":1.7", .sig = sig, .sender = sender };
    bus_send(&h, body);
    return bus_wait(h.serial, m, 5000);
}

static void serve_child(int fd) {
    MemBack *mb = calloc(1, sizeof *mb);
    memback_init(mb, 0);
    if (ss_start(fd, &memback_ops, mb)) _exit(2);
    for (;;) {
        struct pollfd p = { ss_fd(), POLLIN, 0 };
        poll(&p, 1, 50);
        if (ss_dispatch() < 0) break;
        memback_tick(mb);
    }
    _exit(0);
}

typedef struct { int dh; uint8_t key[16]; char path[64]; } Cli;

static int open_session(Cli *c, int dh) {
    DBuf b;
    dw_init(&b);
    uint8_t priv[128], pub[128], g[128] = { 0 };
    g[127] = 2;
    if (dh) {
        vc_random(priv, 128);
        priv[0] &= 0x3f;
        ss_dh_modexp(g, priv, ss_dh_prime(), pub);
        dw_str(&b, "dh-ietf1024-sha256-aes128-cbc-pkcs7");
        dw_variant(&b, "ay");
        dw_bytes(&b, pub, 128);
    } else { dw_str(&b, "plain"); dw_variant(&b, "s"); dw_str(&b, ""); }
    DMsg m;
    int r = call(":1.9", "/org/freedesktop/secrets", "org.freedesktop.Secret.Service", "OpenSession", "sv", &b, &m);
    dw_free(&b);
    if (r != 1 || strcmp(m.sig, "vo")) { if (r >= 0) dbus_msg_free(&m); return -1; }
    DRd d;
    dr_init(&d, &m);
    const char *vs = dr_variant(&d);
    c->dh = dh;
    if (dh) {
        uint32_t n;
        const uint8_t *sp = dr_bytes(&d, &n);
        uint8_t y[128] = { 0 }, sh[128];
        if (strcmp(vs, "ay") || n != 128) { dbus_msg_free(&m); return -1; }
        memcpy(y, sp, 128);
        ss_dh_modexp(y, priv, ss_dh_prime(), sh);
        hkdf_sha256(NULL, 0, sh, 128, NULL, 0, c->key, 16);
    } else dr_str(&d, NULL);
    snprintf(c->path, sizeof c->path, "%s", dr_str(&d, NULL));
    dbus_msg_free(&m);
    return 0;
}

static void put_secret(DBuf *b, const Cli *c, const char *val) {
    dw_struct(b);
    dw_str(b, c->path);
    size_t n = strlen(val);
    if (c->dh) {
        uint8_t iv[16], iv2[16], buf[256];
        vc_random(iv, 16);
        memcpy(iv2, iv, 16);
        size_t pl = n / 16 * 16 + 16;
        memcpy(buf, val, n);
        memset(buf + n, (int)(pl - n), pl - n);
        Aes a;
        aes_init(&a, c->key, 128);
        aes_cbc_encrypt(&a, iv2, buf, buf, pl);
        dw_bytes(b, iv, 16);
        dw_bytes(b, buf, pl);
    } else { dw_bytes(b, NULL, 0); dw_bytes(b, (const uint8_t *)val, n); }
    dw_str(b, "text/plain");
}

static int get_secret_val(DRd *d, const Cli *c, char *out, size_t cap) {
    dr_struct(d);
    dr_str(d, NULL);
    uint32_t pl, vl;
    const uint8_t *params = dr_bytes(d, &pl), *val = dr_bytes(d, &vl);
    dr_str(d, NULL);
    if (d->err || vl >= cap) return -1;
    uint8_t buf[512];
    memcpy(buf, val, vl);
    if (c->dh) {
        if (pl != 16 || vl % 16) return -1;
        uint8_t iv[16];
        memcpy(iv, params, 16);
        Aes a;
        aes_init(&a, c->key, 128);
        aes_cbc_decrypt(&a, iv, buf, buf, vl);
        long n = pkcs7_unpad(buf, vl);
        if (n < 0) return -1;
        vl = (uint32_t)n;
    }
    memcpy(out, buf, vl);
    out[vl] = 0;
    return 0;
}

/* The real flow with the fake bus: Hello and RequestName have to be answered first. */
static int fake_bus_start(pid_t *pid_out) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) return -1;
    pid_t pid = fork();
    if (pid == 0) { close(sv[0]); serve_child(sv[1]); }
    close(sv[1]);
    *pid_out = pid;
    int fd = sv[0];
    char line[256], nul;
    if (read(fd, &nul, 1) != 1 || bus_read_line(fd, line, sizeof line)) return -1;
    if (write(fd, "OK 0123456789abcdef0123456789abcdef\r\n", 37) != 37) return -1;
    if (bus_read_line(fd, line, sizeof line) || strcmp(line, "BEGIN")) return -1;
    memset(&bus, 0, sizeof bus);
    bus.fd = fd;
    for (int step = 0; step < 2; step++) {
        DMsg m;
        int got = 0;
        for (int i = 0; i < 100 && !got; i++) {
            int r = dbus_next(&bus, &m);
            if (r < 0) return -1;
            if (r == 0) { struct pollfd p = { fd, POLLIN, 0 }; poll(&p, 1, 50); continue; }
            got = 1;
        }
        if (!got || m.type != DBUS_CALL) return -1;
        DBuf b;
        dw_init(&b);
        const char *sig;
        if (!strcmp(m.member, "Hello")) { dw_str(&b, ":1.7"); sig = "s"; }
        else if (!strcmp(m.member, "RequestName")) {
            DRd d; dr_init(&d, &m);
            check(!strcmp(dr_str(&d, NULL), "org.freedesktop.secrets") && dr_u32(&d) == 4, "RequestName with DO_NOT_QUEUE");
            dw_u32(&b, 1); sig = "u";
        } else { dbus_msg_free(&m); dw_free(&b); return -1; }
        DHdr h = { .type = DBUS_RETURN, .reply_serial = m.serial, .dest = ":1.7", .sig = sig, .sender = "org.freedesktop.DBus" };
        bus_send(&h, &b);
        dw_free(&b);
        dbus_msg_free(&m);
    }
    return 0;
}

static void attrs2(DBuf *b, const char *k1, const char *v1, const char *k2, const char *v2) {
    size_t mk = dw_arr_begin(b, 8);
    if (k1) { dw_struct(b); dw_str(b, k1); dw_str(b, v1); }
    if (k2) { dw_struct(b); dw_str(b, k2); dw_str(b, v2); }
    dw_arr_end(b, mk);
}

static char *create(const Cli *c, const char *label, const char *user, const char *secret, int replace, int *err) {
    DBuf b;
    dw_init(&b);
    size_t mk = dw_arr_begin(&b, 8);
    dw_struct(&b); dw_str(&b, "org.freedesktop.Secret.Item.Label"); dw_variant(&b, "s"); dw_str(&b, label);
    dw_struct(&b); dw_str(&b, "org.freedesktop.Secret.Item.Attributes"); dw_variant(&b, "a{ss}");
    attrs2(&b, "service", "demo", "user", user);
    dw_struct(&b); dw_str(&b, "otra.cosa"); dw_variant(&b, "a{sv}");     /* unknown: skipped */
    size_t mk2 = dw_arr_begin(&b, 8); dw_struct(&b); dw_str(&b, "x"); dw_variant(&b, "u"); dw_u32(&b, 1); dw_arr_end(&b, mk2);
    dw_arr_end(&b, mk);
    put_secret(&b, c, secret);
    dw_bool(&b, replace);
    DMsg m;
    int r = call(":1.9", "/org/freedesktop/secrets/collection/login", "org.freedesktop.Secret.Collection", "CreateItem", "a{sv}(oayays)b", &b, &m);
    dw_free(&b);
    *err = r;
    if (r != 1) { if (r >= 0) dbus_msg_free(&m); return NULL; }
    DRd d;
    dr_init(&d, &m);
    char *p = strdup(dr_str(&d, NULL));
    dbus_msg_free(&m);
    return p;
}

static int read_item(const char *item, const Cli *c, const char *sender, char *out) {
    DBuf b;
    dw_init(&b);
    dw_str(&b, c->path);
    DMsg m;
    int r = call(sender, item, "org.freedesktop.Secret.Item", "GetSecret", "o", &b, &m);
    dw_free(&b);
    if (r != 1) { if (r == 0) { snprintf(out, 128, "%s", m.error); dbus_msg_free(&m); } return -1; }
    DRd d;
    dr_init(&d, &m);
    int ok = get_secret_val(&d, c, out, 128);
    dbus_msg_free(&m);
    return ok;
}

static int search_count(const char *user, int *nlocked) {
    DBuf b;
    dw_init(&b);
    attrs2(&b, "service", "demo", user ? "user" : NULL, user);
    DMsg m;
    int r = call(":1.9", "/org/freedesktop/secrets", "org.freedesktop.Secret.Service", "SearchItems", "a{ss}", &b, &m);
    dw_free(&b);
    if (r != 1) { if (r >= 0) dbus_msg_free(&m); return -1; }
    DRd d;
    dr_init(&d, &m);
    int n = 0, l = 0;
    size_t end = dr_arr(&d, 4);
    while (dr_more(&d, end)) { dr_str(&d, NULL); n++; }
    end = dr_arr(&d, 4);
    while (dr_more(&d, end)) { dr_str(&d, NULL); l++; }
    dbus_msg_free(&m);
    if (nlocked) *nlocked = l;
    return n;
}

static void t_service_flow(void) {
    pid_t pid;
    if (fake_bus_start(&pid)) { check(0, "fake bus: startup"); return; }
    check(1, "fake bus: SASL, Hello and RequestName");
    Cli plain, dh;
    check(open_session(&plain, 0) == 0, "OpenSession plain");
    check(open_session(&dh, 1) == 0, "OpenSession dh-ietf1024-sha256-aes128-cbc-pkcs7");
    int err;
    char *it1 = create(&plain, "Correo", "ana", "clave plana ñ", 0, &err);
    check(it1 != NULL, "CreateItem (plain)");
    char *it2 = create(&dh, "Banco", "beto", "secreto con DH: 0123456789abcdef", 0, &err);
    check(it2 != NULL, "CreateItem (DH)");
    char *it3 = create(&dh, "Banco", "beto", "reemplazado", 1, &err);
    check(it3 && it2 && !strcmp(it2, it3), "CreateItem replace reuses the item");
    char v[128];
    check(read_item(it1, &plain, ":1.9", v) == 0 && !strcmp(v, "clave plana ñ"), "GetSecret plain");
    check(read_item(it1, &dh, ":1.9", v) == 0 && !strcmp(v, "clave plana ñ"), "GetSecret DH (decrypted on the client side)");
    check(read_item(it2, &dh, ":1.9", v) == 0 && !strcmp(v, "reemplazado"), "secret replaced");
    check(read_item(it1, &plain, ":1.66", v) != 0 && !strcmp(v, "org.freedesktop.Secret.Error.NoSession"), "another client's session: NoSession");
    int nl = -1;
    check(search_count(NULL, &nl) == 2 && nl == 0, "SearchItems (service=demo)");
    check(search_count("ana", NULL) == 1, "SearchItems (user=ana)");
    check(search_count("nadie", NULL) == 0, "SearchItems with no results");
    /* GetSecrets of both */
    {
        DBuf b; dw_init(&b);
        size_t mk = dw_arr_begin(&b, 4); dw_str(&b, it1); dw_str(&b, it2); dw_str(&b, "/org/freedesktop/secrets/collection/login/nada"); dw_arr_end(&b, mk);
        dw_str(&b, dh.path);
        DMsg m;
        int r = call(":1.9", "/org/freedesktop/secrets", "org.freedesktop.Secret.Service", "GetSecrets", "aoo", &b, &m);
        dw_free(&b);
        int n = 0, good = 1;
        if (r == 1) {
            DRd d; dr_init(&d, &m);
            size_t end = dr_arr(&d, 8);
            while (dr_more(&d, end)) { dr_struct(&d); dr_str(&d, NULL); char s[128]; if (get_secret_val(&d, &dh, s, sizeof s)) good = 0; n++; }
            dbus_msg_free(&m);
        }
        check(r == 1 && n == 2 && good, "GetSecrets (2 of 3, DH)");
    }
    /* SetSecret over DH */
    {
        DBuf b; dw_init(&b);
        put_secret(&b, &dh, "nuevo valor");
        DMsg m;
        int r = call(":1.9", it1, "org.freedesktop.Secret.Item", "SetSecret", "(oayays)", &b, &m);
        dw_free(&b);
        if (r >= 0) dbus_msg_free(&m);
        check(r == 1 && read_item(it1, &plain, ":1.9", v) == 0 && !strcmp(v, "nuevo valor"), "SetSecret (DH)");
    }
    /* properties */
    {
        DBuf b; dw_init(&b); dw_str(&b, "org.freedesktop.Secret.Item");
        DMsg m;
        int r = call(":1.9", it1, "org.freedesktop.DBus.Properties", "GetAll", "s", &b, &m);
        dw_free(&b);
        int found = 0;
        if (r == 1) {
            DRd d; dr_init(&d, &m);
            size_t end = dr_arr(&d, 8);
            while (dr_more(&d, end)) {
                dr_struct(&d);
                const char *k = dr_str(&d, NULL), *vs = dr_variant(&d);
                if (!strcmp(k, "Label") && !strcmp(vs, "s") && !strcmp(dr_str(&d, NULL), "Correo")) found++;
                else if (!strcmp(k, "Attributes") && !strcmp(vs, "a{ss}")) { found++; dr_skip(&d, vs); }
                else dr_skip(&d, vs);
            }
            dbus_msg_free(&m);
        }
        check(r == 1 && found == 2, "Properties.GetAll of the item");
        dw_init(&b); dw_str(&b, "org.freedesktop.Secret.Item"); dw_str(&b, "Label"); dw_variant(&b, "s"); dw_str(&b, "Correo 2");
        r = call(":1.9", it1, "org.freedesktop.DBus.Properties", "Set", "ssv", &b, &m);
        dw_free(&b);
        if (r >= 0) dbus_msg_free(&m);
        dw_init(&b); dw_str(&b, "org.freedesktop.Secret.Item"); dw_str(&b, "Label");
        r = call(":1.9", it1, "org.freedesktop.DBus.Properties", "Get", "ss", &b, &m);
        dw_free(&b);
        int ok = 0;
        if (r == 1) { DRd d; dr_init(&d, &m); dr_variant(&d); ok = !strcmp(dr_str(&d, NULL), "Correo 2"); dbus_msg_free(&m); }
        check(ok, "Properties.Set/Get Label");
        dw_init(&b); dw_str(&b, "org.freedesktop.Secret.Service"); dw_str(&b, "Collections");
        r = call(":1.9", "/org/freedesktop/secrets", "org.freedesktop.DBus.Properties", "Get", "ss", &b, &m);
        dw_free(&b);
        ok = 0;
        if (r == 1) { DRd d; dr_init(&d, &m); ok = !strcmp(dr_variant(&d), "ao"); size_t e = dr_arr(&d, 4); ok = ok && dr_more(&d, e) && !strcmp(dr_str(&d, NULL), "/org/freedesktop/secrets/collection/login"); dbus_msg_free(&m); }
        check(ok, "Service.Collections");
    }
    /* alias, introspection, errors */
    {
        DBuf b; dw_init(&b); dw_str(&b, "default");
        DMsg m;
        int r = call(":1.9", "/org/freedesktop/secrets", "org.freedesktop.Secret.Service", "ReadAlias", "s", &b, &m);
        dw_free(&b);
        int ok = 0;
        if (r == 1) { DRd d; dr_init(&d, &m); ok = !strcmp(dr_str(&d, NULL), "/org/freedesktop/secrets/collection/login"); dbus_msg_free(&m); }
        check(ok, "ReadAlias default");
        r = call(":1.9", "/org/freedesktop/secrets", "org.freedesktop.DBus.Introspectable", "Introspect", "", NULL, &m);
        ok = 0;
        if (r == 1) { DRd d; dr_init(&d, &m); const char *x = dr_str(&d, NULL); ok = strstr(x, "OpenSession") && strstr(x, "<node name=\"collection\"/>"); dbus_msg_free(&m); }
        check(ok, "Introspect of the service");
        r = call(":1.9", "/org/freedesktop/secrets/collection/login", "org.freedesktop.DBus.Introspectable", "Introspect", "", NULL, &m);
        ok = 0;
        if (r == 1) { DRd d; dr_init(&d, &m); const char *x = dr_str(&d, NULL); ok = strstr(x, "CreateItem") && strstr(x, "<node name=\"i0001\"/>"); dbus_msg_free(&m); }
        check(ok, "Introspect of the collection with its items");
        r = call(":1.9", "/org/freedesktop/secrets/collection/login", "org.freedesktop.Secret.Collection", "Delete", "", NULL, &m);
        ok = r == 0 && !strcmp(m.error, "org.freedesktop.DBus.Error.NotSupported");
        if (r >= 0) dbus_msg_free(&m);
        check(ok, "Collection.Delete: NotSupported");
        r = call(":1.9", "/org/freedesktop/secrets", "org.freedesktop.Secret.Service", "Inventado", "", NULL, &m);
        ok = r == 0 && !strcmp(m.error, "org.freedesktop.DBus.Error.UnknownMethod");
        if (r >= 0) dbus_msg_free(&m);
        check(ok, "unknown method");
        r = call(":1.9", "/no/existe", NULL, "Ping", "", NULL, &m);
        ok = r == 0;
        if (r >= 0) dbus_msg_free(&m);
        check(ok, "unknown object");
        dw_init(&b); dw_str(&b, "plain");
        r = call(":1.9", "/org/freedesktop/secrets", "org.freedesktop.Secret.Service", "OpenSession", "s", &b, &m);
        dw_free(&b);
        ok = r == 0 && !strcmp(m.error, "org.freedesktop.DBus.Error.InvalidArgs");
        if (r >= 0) dbus_msg_free(&m);
        check(ok, "wrong signature: InvalidArgs");
        r = call(":1.9", "/org/freedesktop/secrets", "org.freedesktop.DBus.Peer", "Ping", "", NULL, &m);
        if (r >= 0) dbus_msg_free(&m);
        check(r == 1, "Peer.Ping");
    }
    /* lock → empty search, GetSecret IsLocked → Unlock with a prompt → Completed */
    {
        DBuf b; dw_init(&b);
        size_t mk = dw_arr_begin(&b, 4); dw_str(&b, "/org/freedesktop/secrets/collection/login"); dw_arr_end(&b, mk);
        DMsg m;
        int r = call(":1.9", "/org/freedesktop/secrets", "org.freedesktop.Secret.Service", "Lock", "ao", &b, &m);
        if (r >= 0) dbus_msg_free(&m);
        check(r == 1, "Lock");
        int nlk = -1;
        check(search_count(NULL, &nlk) == 0 && nlk == 2, "locked: the items go in the locked list");
        check(read_item(it1, &plain, ":1.9", v) != 0 && !strcmp(v, "org.freedesktop.Secret.Error.IsLocked"), "locked: IsLocked");
        r = call(":1.9", "/org/freedesktop/secrets", "org.freedesktop.Secret.Service", "Unlock", "ao", &b, &m);
        dw_free(&b);
        char prompt[96] = "";
        int nun = -1;
        if (r == 1) {
            DRd d; dr_init(&d, &m);
            size_t end = dr_arr(&d, 4);
            nun = 0;
            while (dr_more(&d, end)) { dr_str(&d, NULL); nun++; }
            snprintf(prompt, sizeof prompt, "%s", dr_str(&d, NULL));
            dbus_msg_free(&m);
        }
        check(r == 1 && nun == 0 && !strncmp(prompt, "/org/freedesktop/secrets/prompt/", 32), "Unlock returns a prompt");
        dw_init(&b); dw_str(&b, "");
        completed_seen = 0;
        r = call(":1.9", prompt, "org.freedesktop.Secret.Prompt", "Prompt", "s", &b, &m);
        dw_free(&b);
        if (r >= 0) dbus_msg_free(&m);
        check(r == 1, "Prompt.Prompt");
        for (int i = 0; i < 40 && !completed_seen; i++) { DMsg s; if (bus_wait(0, &s, 100) == 1) dbus_msg_free(&s); }
        check(completed_seen == 1 && completed_dismissed == 0, "Completed (not dismissed)");
        check(read_item(it1, &plain, ":1.9", v) == 0 && !strcmp(v, "nuevo valor"), "unlocked: GetSecret again");
    }
    /* delete */
    {
        DMsg m;
        int r = call(":1.9", it1, "org.freedesktop.Secret.Item", "Delete", "", NULL, &m);
        if (r >= 0) dbus_msg_free(&m);
        check(r == 1 && search_count(NULL, NULL) == 1, "Item.Delete");
        r = call(":1.9", plain.path, "org.freedesktop.Secret.Session", "Close", "", NULL, &m);
        if (r >= 0) dbus_msg_free(&m);
        check(r == 1 && read_item(it2, &plain, ":1.9", v) != 0, "Session.Close");
    }
    check(item_created_seen >= 2, "ItemCreated signals");
    free(it1); free(it2); free(it3);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    dbus_close(&bus);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    signal(SIGPIPE, SIG_IGN);
    t_sigs();
    t_roundtrip();
    t_reject();
    t_modexp();
    t_fds();
    /* fuzz over our own and captured messages */
    size_t len;
    uint8_t *v = valid_msg(&len);
    int acc = fuzz_one(v, len, 100000);
    free(v);
    uint8_t **caps = NULL;
    size_t *clens = NULL;
    int ncap = t_capture("tests/secretsvc/libsecret-capture.bin", &caps, &clens);
    int acc2 = 0;
    for (int i = 0; i < ncap; i++) { acc2 += fuzz_one(caps[i], clens[i], 4000); free(caps[i]); }
    free(caps); free(clens);
    if (ncap >= 0) printf("real captures: %d messages from libsecret\n", ncap);
    else printf("real captures: tests/secretsvc/libsecret-capture.bin is missing (skipped)\n");
    printf("fuzz: 100000 + %d mutations, accepted %d + %d, no crashes\n", ncap > 0 ? ncap * 4000 : 0, acc, acc2);
    check(1, "fuzz without crashes");
    t_service_flow();
    printf("dbus-test: %d/%d\n", total - fails, total);
    return fails ? 1 : 0;
}
