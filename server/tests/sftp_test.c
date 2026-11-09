/* sftp_test.c — the Files mode's wire (term/sshmux.c, term/sftp.c) against hostile bytes, and lord-sftp
 * end to end: against OpenSSH's own sftp-server over pipes (no ssh needed; skipped when it isn't
 * installed) and against a fake server that sends names with '/', control characters and odd owners.
 *   ./build/sftp-test build/lord-sftp */
#include "../src/term/sftp.h"
#include "../src/term/sftpproto.h"
#include "../src/doc/wire.h"
#include "../src/sec/json.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int pass, failn;
#define CHECK(c, ...) do { if (c) pass++; else { failn++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the pure part ---- */

static void test_wire(void) {
    char q[256];
    const char *a1[] = { "file", "-b", "--", "/srv/it's here", NULL };
    CHECK(!mux_quote_args(a1, q, sizeof q) && !strcmp(q, "'file' '-b' '--' '/srv/it'\\''s here'"), "quote: %s", q);
    const char *a2[] = { "file", "/a\nrm -rf ~", NULL };
    CHECK(mux_quote_args(a2, q, sizeof q) == -1, "quote: a newline is refused");
    const char *a3[] = { "x", NULL };
    CHECK(mux_quote_args(a3, q, 3) == -1, "quote: too small");

    /* a string whose length runs past the packet */
    uint8_t bad[] = { 0, 0, 0, 9, 'a', 'b' };
    MuxRd r = { bad, sizeof bad, 0 };
    const char *s;
    size_t n;
    CHECK(mxr_str(&r, &s, &n) == -1, "a string past the end is refused");
    uint8_t big[] = { 0xff, 0xff, 0xff, 0xff };
    r = (MuxRd){ big, sizeof big, 0 };
    CHECK(mxr_str(&r, &s, &n) == -1, "a 4 GB length is refused");

    /* attributes with too many extended pairs */
    MuxBuf b = { 0 };
    mxb_u32(&b, SSH_FILEXFER_ATTR_EXTENDED);
    mxb_u32(&b, 1000);
    r = (MuxRd){ b.p, b.n, 0 };
    SfAttrs at;
    CHECK(sf_get_attrs(&r, &at) == -1, "1000 extended pairs are refused");
    /* a NAME entry: good, with '/', with a newline, odd owner */
    SfEntry e;
    const char *names[] = { "notes.txt", "../evil", "a\nb", "ok" };
    const char *longs[] = { "-rw-r--r--    1 pi       www-data     12 Nov  9 17:31 notes.txt", "x", "x",
                            "-rw-r--r--    1 pi;rm    g$r          12 Nov  9 17:31 ok" };
    int want[] = { 0, -2, -2, 0 };
    for (int i = 0; i < 4; i++) {
        mxb_reset(&b);
        mxb_str(&b, names[i], strlen(names[i]));
        mxb_str(&b, longs[i], strlen(longs[i]));
        mxb_u32(&b, SSH_FILEXFER_ATTR_PERMISSIONS | SSH_FILEXFER_ATTR_SIZE);
        mxb_u64(&b, 12);
        mxb_u32(&b, 0100644);
        r = (MuxRd){ b.p, b.n, 0 };
        int k = sf_name_entry(&r, &e);
        CHECK(k == want[i], "entry %d: %d", i, k);
    }
    CHECK(!strcmp(e.owner, "") && !strcmp(e.group, ""), "odd owner and group names are left out: '%s' '%s'", e.owner, e.group);
    mxb_reset(&b);
    mxb_str(&b, "notes.txt", 9);
    mxb_str(&b, longs[0], strlen(longs[0]));
    mxb_u32(&b, SSH_FILEXFER_ATTR_PERMISSIONS);
    mxb_u32(&b, 0100644);
    r = (MuxRd){ b.p, b.n, 0 };
    CHECK(!sf_name_entry(&r, &e) && !strcmp(e.owner, "pi") && !strcmp(e.group, "www-data") && e.kind == '-', "owner pi, group www-data");
    mxb_free(&b);
    CHECK(sf_path_ok("/srv/x") && !sf_path_ok("srv/x") && !sf_path_ok("/a\nb") && !sf_path_ok(NULL), "paths");
}

/* ---- lord-sftp ---- */

typedef struct { pid_t helper, server; int chan; } Run;

/* the helper on fd 3/4/5: server_out → its 4, its 5 → server_in */
static int start_helper(const char *bin, int server_out, int server_in, Run *rn) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) return -1;
    pid_t p = fork();
    if (!p) {
        int a = fcntl(sv[1], F_DUPFD, 10), b = fcntl(server_out, F_DUPFD, 10), c = fcntl(server_in, F_DUPFD, 10);
        dup2(a, 3); dup2(b, 4); dup2(c, 5);
        for (int fd = 6; fd < 64; fd++) close(fd);
        execl(bin, "lord-sftp", (char *)NULL);
        _exit(127);
    }
    close(sv[1]);
    rn->helper = p;
    rn->chan = sv[0];
    return 0;
}

static char *ask(Run *rn, int op, const char *json, int fd, int *type) {
    static uint32_t id = 10;
    id++;
    if (doc_send(rn->chan, (uint32_t)op, id, json, strlen(json), fd >= 0 ? &fd : NULL, fd >= 0 ? 1 : 0)) return NULL;
    for (;;) {
        DocMsg m;
        if (doc_recv(rn->chan, &m)) return NULL;
        if (m.h.op == SE_PROGRESS) { doc_msg_free(&m); continue; }
        *type = (int)m.h.op;
        char *j = m.json;
        if (m.nfds == 1) {                       /* a big listing: the JSON in a memfd */
            struct stat st;
            fstat(m.fds[0], &st);
            free(j);
            j = calloc(1, (size_t)st.st_size + 1);
            if (j && pread(m.fds[0], j, (size_t)st.st_size, 0) != st.st_size) { free(j); j = NULL; }
        }
        m.json = NULL;
        doc_msg_free(&m);
        return j;
    }
}

static void test_real(const char *bin, const char *server) {
    if (access(server, X_OK)) { printf("skipped: needs %s (openssh-sftp-server)\n", server); return; }
    char dir[] = "/tmp/sftp-test.XXXXXX";
    if (!mkdtemp(dir)) return;
    int to_srv[2], from_srv[2];
    if (pipe2(to_srv, O_CLOEXEC) || pipe2(from_srv, O_CLOEXEC)) return;
    pid_t s = fork();
    if (!s) {
        dup2(to_srv[0], 0); dup2(from_srv[1], 1);
        execl(server, "sftp-server", (char *)NULL);
        _exit(127);
    }
    close(to_srv[0]);
    close(from_srv[1]);
    Run rn = { 0 };
    CHECK(!start_helper(bin, from_srv[0], to_srv[1], &rn), "helper started");
    close(from_srv[0]);
    close(to_srv[1]);
    DocMsg h;
    CHECK(!doc_recv(rn.chan, &h) && h.h.op == SE_OK && h.json && strstr(h.json, "\"version\":3"), "hello: %s", h.json ? h.json : "-");
    doc_msg_free(&h);
    int t;
    char q[1024], *a;
    snprintf(q, sizeof q, "{\"path\":\"%s/sub\"}", dir);
    a = ask(&rn, SP_MKDIR, q, -1, &t);
    CHECK(a && t == SE_OK, "mkdir: %s", a ? a : "-");
    free(a);
    /* upload 200 KB + 1 from a local file */
    char src[600], dst[600];
    snprintf(src, sizeof src, "%s.src", dir);
    snprintf(dst, sizeof dst, "%s.dst", dir);
    int f = open(src, O_RDWR | O_CREAT | O_TRUNC, 0600);
    static char data[200 * 1024 + 1];
    for (size_t i = 0; i < sizeof data; i++) data[i] = (char)(i * 31 + 7);
    CHECK(write(f, data, sizeof data) == (ssize_t)sizeof data, "source written");
    lseek(f, 0, SEEK_SET);
    snprintf(q, sizeof q, "{\"path\":\"%s/sub/up.bin\",\"size\":%zu,\"perm\":416}", dir, sizeof data);
    a = ask(&rn, SP_PUT, q, f, &t);
    CHECK(a && t == SE_OK && strstr(a, "\"bytes\":204801"), "put: %s", a ? a : "-");
    free(a);
    close(f);
    char up[700];
    snprintf(up, sizeof up, "%s/sub/up.bin", dir);
    struct stat st;
    CHECK(!stat(up, &st) && st.st_size == (off_t)sizeof data && (st.st_mode & 0777) == 0640, "uploaded: %ld bytes, mode %o", (long)st.st_size, st.st_mode & 0777);
    /* listing */
    snprintf(q, sizeof q, "{\"path\":\"%s/sub\"}", dir);
    a = ask(&rn, SP_LIST, q, -1, &t);
    CHECK(a && t == SE_OK && strstr(a, "\"name\":\"up.bin\"") && strstr(a, "\"size\":204801") && strstr(a, "\"perm\":416"), "list: %.300s", a ? a : "-");
    free(a);
    /* download into another file, compared byte for byte */
    f = open(dst, O_RDWR | O_CREAT | O_TRUNC, 0600);
    snprintf(q, sizeof q, "{\"path\":\"%s/sub/up.bin\"}", dir);
    a = ask(&rn, SP_GET, q, f, &t);
    CHECK(a && t == SE_OK && strstr(a, "\"bytes\":204801"), "get: %s", a ? a : "-");
    free(a);
    static char back[sizeof data];
    CHECK(pread(f, back, sizeof back, 0) == (ssize_t)sizeof back && !memcmp(back, data, sizeof data), "downloaded bytes match");
    close(f);
    /* rename, chmod, stat */
    snprintf(q, sizeof q, "{\"from\":\"%s/sub/up.bin\",\"to\":\"%s/sub/moved.bin\"}", dir, dir);
    a = ask(&rn, SP_RENAME, q, -1, &t);
    CHECK(a && t == SE_OK, "rename: %s", a ? a : "-");
    free(a);
    snprintf(q, sizeof q, "{\"path\":\"%s/sub/moved.bin\",\"perm\":384}", dir);
    a = ask(&rn, SP_CHMOD, q, -1, &t);
    CHECK(a && t == SE_OK, "chmod: %s", a ? a : "-");
    free(a);
    snprintf(q, sizeof q, "{\"path\":\"%s/sub/moved.bin\"}", dir);
    a = ask(&rn, SP_STAT, q, -1, &t);
    CHECK(a && t == SE_OK && strstr(a, "\"perm\":384") && strstr(a, "\"kind\":\"-\""), "stat: %s", a ? a : "-");
    free(a);
    /* errors */
    snprintf(q, sizeof q, "{\"path\":\"%s/none\"}", dir);
    a = ask(&rn, SP_STAT, q, -1, &t);
    CHECK(a && t == SE_ERR && strstr(a, "\"nofile\""), "missing: %s", a ? a : "-");
    free(a);
    a = ask(&rn, SP_STAT, "{\"path\":\"relative\"}", -1, &t);
    CHECK(a && t == SE_ERR && strstr(a, "\"bad\""), "relative path refused: %s", a ? a : "-");
    free(a);
    /* removing: the file, then the empty folder */
    snprintf(q, sizeof q, "{\"path\":\"%s/sub/moved.bin\"}", dir);
    a = ask(&rn, SP_REMOVE, q, -1, &t);
    CHECK(a && t == SE_OK, "remove: %s", a ? a : "-");
    free(a);
    snprintf(q, sizeof q, "{\"path\":\"%s/sub\",\"dir\":true}", dir);
    a = ask(&rn, SP_REMOVE, q, -1, &t);
    CHECK(a && t == SE_OK, "rmdir: %s", a ? a : "-");
    free(a);
    CHECK(stat(dir, &st) == 0, "the test folder is still there");
    doc_send(rn.chan, SP_END, 0, "{}", 2, NULL, 0);
    int status;
    waitpid(rn.helper, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "lord-sftp ended: %d", status);
    close(rn.chan);
    kill(s, SIGTERM);
    waitpid(s, NULL, 0);
    unlink(src);
    unlink(dst);
    rmdir(dir);
}

/* a fake server: reads requests, answers each from a script; names with '/' and control characters */
static int fake_reply(int fd, const MuxBuf *b) {
    uint8_t h[4] = { (uint8_t)(b->n >> 24), (uint8_t)(b->n >> 16), (uint8_t)(b->n >> 8), (uint8_t)b->n };
    return write(fd, h, 4) == 4 && write(fd, b->p, b->n) == (ssize_t)b->n ? 0 : -1;
}
static int fake_read(int fd, uint8_t *buf, size_t cap, int *type, uint32_t *id) {
    uint8_t h[4];
    if (read(fd, h, 4) != 4) return -1;
    uint32_t n = (uint32_t)h[0] << 24 | (uint32_t)h[1] << 16 | (uint32_t)h[2] << 8 | h[3];
    if (n > cap || n < 1) return -1;
    size_t got = 0;
    while (got < n) { ssize_t k = read(fd, buf + got, n - got); if (k <= 0) return -1; got += (size_t)k; }
    *type = buf[0];
    *id = n >= 5 ? (uint32_t)buf[1] << 24 | (uint32_t)buf[2] << 16 | (uint32_t)buf[3] << 8 | buf[4] : 0;
    return 0;
}

static void test_hostile(const char *bin) {
    int to_srv[2], from_srv[2];
    if (pipe2(to_srv, O_CLOEXEC) || pipe2(from_srv, O_CLOEXEC)) return;
    pid_t s = fork();
    if (!s) {
        static uint8_t buf[65536];
        MuxBuf b = { 0 };
        int type;
        uint32_t id;
        int readdirs = 0;
        while (!fake_read(to_srv[0], buf, sizeof buf, &type, &id)) {
            mxb_reset(&b);
            if (type == SSH_FXP_INIT) { mxb_u8(&b, SSH_FXP_VERSION); mxb_u32(&b, 3); }
            else if (type == SSH_FXP_REALPATH) {
                mxb_u8(&b, SSH_FXP_NAME); mxb_u32(&b, id); mxb_u32(&b, 1);
                mxb_str(&b, "/srv", 4); mxb_str(&b, "", 0); mxb_u32(&b, 0);
            } else if (type == SSH_FXP_OPENDIR) { mxb_u8(&b, SSH_FXP_HANDLE); mxb_u32(&b, id); mxb_str(&b, "H", 1); }
            else if (type == SSH_FXP_READDIR && !readdirs++) {
                const char *nm[] = { "good.txt", "../../etc", "x\x1b[31m", ".", "..", "fine" };
                mxb_u8(&b, SSH_FXP_NAME); mxb_u32(&b, id); mxb_u32(&b, 6);
                for (int i = 0; i < 6; i++) {
                    mxb_str(&b, nm[i], strlen(nm[i]));
                    const char *ln = "-rw-r--r-- 1 pi\x1b]0;x pi 1 Nov 9 17:31 n";
                    mxb_str(&b, ln, strlen(ln));
                    mxb_u32(&b, SSH_FILEXFER_ATTR_PERMISSIONS);
                    mxb_u32(&b, 0100644);
                }
            } else if (type == SSH_FXP_READDIR) { mxb_u8(&b, SSH_FXP_STATUS); mxb_u32(&b, id); mxb_u32(&b, SSH_FX_EOF); mxb_str(&b, "", 0); mxb_str(&b, "", 0); }
            else { mxb_u8(&b, SSH_FXP_STATUS); mxb_u32(&b, id); mxb_u32(&b, SSH_FX_OK); mxb_str(&b, "", 0); mxb_str(&b, "", 0); }
            if (fake_reply(from_srv[1], &b)) break;
        }
        _exit(0);
    }
    close(to_srv[0]);
    close(from_srv[1]);
    Run rn = { 0 };
    CHECK(!start_helper(bin, from_srv[0], to_srv[1], &rn), "helper started (fake)");
    close(from_srv[0]);
    close(to_srv[1]);
    DocMsg h;
    CHECK(!doc_recv(rn.chan, &h) && h.h.op == SE_OK, "hello (fake)");
    doc_msg_free(&h);
    int t;
    char *a = ask(&rn, SP_LIST, "{\"path\":\"/srv\"}", -1, &t);
    CHECK(a && t == SE_OK && strstr(a, "\"good.txt\"") && strstr(a, "\"fine\""), "the plain names are listed: %s", a ? a : "-");
    CHECK(a && !strstr(a, "etc") && !strstr(a, "\x1b") && !strstr(a, "\\u001b") && !strstr(a, "\"..\""), "names with '/', escapes, . and .. are left out");
    CHECK(a && !strstr(a, "\"owner\""), "an owner with an escape is left out");
    free(a);
    doc_send(rn.chan, SP_END, 0, "{}", 2, NULL, 0);
    waitpid(rn.helper, NULL, 0);
    close(rn.chan);
    kill(s, SIGKILL);
    waitpid(s, NULL, 0);
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    test_wire();
    if (argc > 1) {
        test_hostile(argv[1]);
        test_real(argv[1], "/usr/lib/openssh/sftp-server");
    }
    printf("sftp-test: %d passed, %d failed\n", pass, failn);
    return failn ? 1 : 0;
}
