/* vmexec_test.c — VM_EXEC's request and answer and the guest's printout (vm/vmexec.c, docs/VIRTUALIZATION.md 6.15):
 * round trips, every limit, every malformed form, the memfd seals, guest paths, and a fuzz pass of the three parsers
 * (built with ASan and UBSan). The scripts themselves run under /bin/sh with fake runuser, loginctl and getent. */
#include "vm/vmexec.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); } } while (0)

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static void test_req(void) {
    const char *av[] = { "uname", "-m", "a b\nc" };
    const uint8_t in[] = "hello";
    size_t n;
    uint8_t *b = vmx_req_encode(30, 3, av, in, 5, &n);
    CHECK(b && n == 12 + 4 * 3 + 5 + 2 + 5 + 4 + 5);
    VmxReq *r = malloc(sizeof *r);
    CHECK(!vmx_req_parse(b, n, r) && r->timeout == 30 && r->argc == 3 && !strcmp(r->argv[0], "uname") && !strcmp(r->argv[1], "-m") &&
          !strcmp(r->argv[2], "a b\nc") && !r->argv[3] && r->inlen == 5 && !memcmp(r->in, "hello", 5));
    /* every shorter prefix and one byte more are refused */
    for (size_t k = 0; k < n; k++) CHECK(vmx_req_parse(b, k, r));
    uint8_t *b2 = malloc(n + 1);
    memcpy(b2, b, n); b2[n] = 'x';
    CHECK(vmx_req_parse(b2, n + 1, r));
    /* a NUL inside an argument */
    memcpy(b2, b, n); b2[12 + 4 + 2] = 0;
    CHECK(vmx_req_parse(b2, n, r));
    /* bad magic, timeouts, counts */
    memcpy(b2, b, n); b2[3] = '2'; CHECK(vmx_req_parse(b2, n, r));
    memcpy(b2, b, n); put32(b2 + 4, 0); CHECK(vmx_req_parse(b2, n, r));
    memcpy(b2, b, n); put32(b2 + 4, VMX_TIMEOUT_MAX + 1); CHECK(vmx_req_parse(b2, n, r));
    memcpy(b2, b, n); put32(b2 + 8, 0); CHECK(vmx_req_parse(b2, n, r));
    memcpy(b2, b, n); put32(b2 + 8, 4); CHECK(vmx_req_parse(b2, n, r));
    memcpy(b2, b, n); put32(b2 + 8, 0xffffffffu); CHECK(vmx_req_parse(b2, n, r));
    memcpy(b2, b, n); put32(b2 + 12, 0); CHECK(vmx_req_parse(b2, n, r));                  /* an empty argument */
    memcpy(b2, b, n); put32(b2 + 12, 0xfffffff0u); CHECK(vmx_req_parse(b2, n, r));        /* a length past the end */
    memcpy(b2, b, n); put32(b2 + n - 9, 6); CHECK(vmx_req_parse(b2, n, r));               /* stdin's length off by one */
    free(b2);
    free(b);
    /* encoder limits */
    CHECK(!vmx_req_encode(0, 1, av, NULL, 0, &n) && !vmx_req_encode(VMX_TIMEOUT_MAX + 1, 1, av, NULL, 0, &n));
    CHECK(!vmx_req_encode(5, 0, av, NULL, 0, &n) && !vmx_req_encode(5, VMX_ARGC_MAX + 1, av, NULL, 0, &n));
    const char *empty[] = { "" };
    CHECK(!vmx_req_encode(5, 1, empty, NULL, 0, &n));
    CHECK(!vmx_req_encode(5, 1, av, in, VMX_STDIN_MAX + 1, &n));
    /* the largest request: 64 arguments of 1024 bytes, 1 MB of input */
    {
        static char big[VMX_ARGC_MAX][1025];
        const char *bv[VMX_ARGC_MAX];
        for (int i = 0; i < VMX_ARGC_MAX; i++) { memset(big[i], 'a' + i % 26, 1024); big[i][1024] = 0; bv[i] = big[i]; }
        uint8_t *inb = malloc(VMX_STDIN_MAX);
        memset(inb, 0xff, VMX_STDIN_MAX);
        uint8_t *bb = vmx_req_encode(VMX_TIMEOUT_MAX, VMX_ARGC_MAX, bv, inb, VMX_STDIN_MAX, &n);
        CHECK(bb && n <= VMX_REQ_MAX && !vmx_req_parse(bb, n, r) && r->argc == VMX_ARGC_MAX && strlen(r->argv[63]) == 1024 && r->inlen == VMX_STDIN_MAX);
        free(bb);
        /* one byte more of arguments: refused both ways */
        big[0][1024] = 'x'; big[0][1025 - 1] = 'x';
        static char one[VMX_ARG_MAX + 2];
        memset(one, 'z', VMX_ARG_MAX + 1); one[VMX_ARG_MAX + 1] = 0;
        const char *ov[] = { one };
        CHECK(!vmx_req_encode(5, 1, ov, NULL, 0, &n));
        free(inb);
    }
    free(r);
}

static void test_res(void) {
    VmxRes a = { .exit = 3, .flags = VMX_F_OUT_CUT, .out = (const uint8_t *)"out", .outlen = 3, .err = (const uint8_t *)"e", .errlen = 1 }, b;
    size_t n;
    uint8_t *p = vmx_res_encode(&a, &n);
    CHECK(p && n == 24 && !vmx_res_parse(p, n, &b) && b.exit == 3 && b.flags == VMX_F_OUT_CUT && b.outlen == 3 && !memcmp(b.out, "out", 3) &&
          b.errlen == 1 && b.err[0] == 'e');
    for (size_t k = 0; k < n; k++) CHECK(vmx_res_parse(p, k, &b));
    p[4] = 0; p[5] = 1; CHECK(vmx_res_parse(p, n, &b));            /* 256 */
    p[5] = 0; p[8] = 8; CHECK(vmx_res_parse(p, n, &b));            /* an unknown flag */
    free(p);
    a.exit = 256; CHECK(!vmx_res_encode(&a, &n));
    a.exit = 0; a.outlen = VMX_OUT_MAX + 1; CHECK(!vmx_res_encode(&a, &n));
}

static void test_guest(void) {
    VmxRes r;
    const char ok[] = "LVXG 0 3 2\nabcxy";
    CHECK(!vmx_guest_parse((const uint8_t *)ok, sizeof ok - 1, &r) && r.exit == 0 && r.outlen == 3 && !memcmp(r.out, "abc", 3) && r.errlen == 2 && !r.flags);
    const char cut[] = "LVXG 124 999999999 0\n";
    {
        size_t n = sizeof cut - 1 + VMX_OUT_MAX;
        uint8_t *b = malloc(n);
        memcpy(b, cut, sizeof cut - 1);
        memset(b + sizeof cut - 1, 'o', VMX_OUT_MAX);
        CHECK(!vmx_guest_parse(b, n, &r) && r.exit == 124 && r.outlen == VMX_OUT_MAX && r.flags == (VMX_F_OUT_CUT | VMX_F_TIMEOUT));
        CHECK(vmx_guest_parse(b, n - 1, &r));
        free(b);
    }
    const char *bad[] = {
        "LVXG 0 3 2\nabcx", "LVXG 0 3 2\nabcxyz", "LVXG 256 0 0\n", "LVXG -1 0 0\n", "LVXG 0 03 0\nabc", "LVXG 0 0 0", "LVXG  0 0 0\n",
        "LVXG 0 0 0 \n", "lvxg 0 0 0\n", "LVXG 0 99999999999 0\n", "LVXG 0 0\n", "LVXG 0 0 0\r\n", "",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) CHECK(vmx_guest_parse((const uint8_t *)bad[i], strlen(bad[i]), &r));
    CHECK(!vmx_guest_parse((const uint8_t *)"LVXG 0 0 0\n", 11, &r) && !r.outlen && !r.errlen);
}

static void test_memfd(void) {
    size_t n;
    int fd = vmx_memfd("t", (const uint8_t *)"abc", 3);
    uint8_t *b = fd >= 0 ? vmx_memfd_read(fd, 3, &n) : NULL;
    CHECK(b && n == 3 && !memcmp(b, "abc", 3));
    free(b);
    CHECK(fd >= 0 && !vmx_memfd_read(fd, 2, &n));                 /* larger than allowed */
    if (fd >= 0) close(fd);
    /* unsealed: refused (the sender could change it while lord-vm reads) */
    int u = memfd_create("u", MFD_CLOEXEC);
    CHECK(u >= 0 && write(u, "abc", 3) == 3 && !vmx_memfd_read(u, 10, &n));
    if (u >= 0) close(u);
    /* a file, a pipe: refused */
    int p[2];
    CHECK(!pipe(p) && !vmx_memfd_read(p[0], 10, &n));
    close(p[0]); close(p[1]);
}

static void test_paths(void) {
    CHECK(vmx_guest_path_ok("~/build") && vmx_guest_path_ok("/tmp/a b") && vmx_guest_path_ok("Documentos/año.txt") && vmx_guest_path_ok("日本"));
    CHECK(!vmx_guest_path_ok("") && !vmx_guest_path_ok(NULL) && !vmx_guest_path_ok("a\nb") && !vmx_guest_path_ok("a\x7f") && !vmx_guest_path_ok("\x1b[2J"));
    CHECK(!vmx_guest_path_ok("\xc2\x85") && !vmx_guest_path_ok("\xc0\xaf") && !vmx_guest_path_ok("\xe0\x80\xaf") && !vmx_guest_path_ok("\xed\xa0\x80") &&
          !vmx_guest_path_ok("\xf4\x90\x80\x80") && !vmx_guest_path_ok("\xe6\x97") && !vmx_guest_path_ok("a\xff"));
    static char longp[VMX_ARG_MAX + 2];
    memset(longp, 'a', VMX_ARG_MAX + 1);
    CHECK(!vmx_guest_path_ok(longp));
    longp[VMX_ARG_MAX] = 0;
    CHECK(vmx_guest_path_ok(longp));
}

/* the scripts under /bin/sh, with stub loginctl, getent and runuser (runuser just runs the command as this account) */
static void test_scripts(void) {
    char d[] = "/tmp/vmexec-test.XXXXXX";
    if (!mkdtemp(d)) { fails++; return; }
    char p[256];
    snprintf(p, sizeof p, "%s/loginctl", d);
    FILE *f = fopen(p, "w");
    fprintf(f, "#!/bin/sh\necho '   3 1000 alex seat0 tty2'\n"); fclose(f); chmod(p, 0755);
    snprintf(p, sizeof p, "%s/runuser", d);
    f = fopen(p, "w");
    /* runuser -u USER -- CMD...: checks the shape and runs CMD */
    fprintf(f, "#!/bin/sh\n[ \"$1\" = -u ] && [ \"$2\" = alex ] && [ \"$3\" = -- ] || exit 99\nshift 3\nexec \"$@\"\n"); fclose(f); chmod(p, 0755);
    char cmd[8192], path[512];
    snprintf(path, sizeof path, "PATH=%s:/usr/bin:/bin", d);
    struct { const char *args, *in; int exit; const char *out, *err; } C[] = {
        { "'printf' '%s|%s' 'a b' '$(id)'", "", 0, "a b|$(id)", "" },
        { "'sh' '-c' 'cat; echo err >&2; exit 7'", "input", 7, "input", "err\n" },
        { "'sleep' '5'", "", 124, "", "" },
        { "'no-such-program-here'", "", 127, "", NULL },
    };
    for (size_t i = 0; i < sizeof C / sizeof C[0]; i++) {
        int timeout = i == 2 ? 1 : 10;
        snprintf(cmd, sizeof cmd, "cd /; printf '%%s' '%s' | env -i %s HOME=%s /bin/sh -c \"$(cat %s/root.sh)\" lordos-exec \"$(cat %s/person.sh)\" %d %s > %s/out 2>%s/err",
                 C[i].in, path, d, d, d, timeout, C[i].args, d, d);
        snprintf(p, sizeof p, "%s/root.sh", d); f = fopen(p, "w"); fputs(VMX_ROOT_SH, f); fclose(f);
        snprintf(p, sizeof p, "%s/person.sh", d); f = fopen(p, "w"); fputs(VMX_PERSON_SH, f); fclose(f);
        int st = system(cmd);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
        static uint8_t buf[1 << 18];
        snprintf(p, sizeof p, "%s/out", d);
        f = fopen(p, "rb");
        size_t n = f ? fread(buf, 1, sizeof buf, f) : 0;
        if (f) fclose(f);
        VmxRes r;
        int ok = !vmx_guest_parse(buf, n, &r);
        if (!ok) fprintf(stderr, "  script case %zu: %.*s\n", i, (int)(n < 200 ? n : 200), buf);
        CHECK(ok && r.exit == C[i].exit && r.outlen == strlen(C[i].out) && !memcmp(r.out, C[i].out, r.outlen) &&
              (!C[i].err || (r.errlen == strlen(C[i].err) && !memcmp(r.err, C[i].err, r.errlen))));
    }
    /* nobody in the guest: exit 3 and its reason, no header */
    snprintf(p, sizeof p, "%s/loginctl", d); f = fopen(p, "w"); fprintf(f, "#!/bin/sh\nexit 0\n"); fclose(f);
    snprintf(p, sizeof p, "%s/getent", d); f = fopen(p, "w"); fprintf(f, "#!/bin/sh\necho 'root:x:0:0::/root:/bin/sh'\n"); fclose(f); chmod(p, 0755);
    snprintf(cmd, sizeof cmd, "env -i %s /bin/sh -c \"$(cat %s/root.sh)\" lordos-exec \"$(cat %s/person.sh)\" 5 true > %s/out 2>%s/err", path, d, d, d, d);
    int st = system(cmd);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 3);
    snprintf(cmd, sizeof cmd, "rm -rf -- %s", d);
    if (system(cmd)) {}
}

/* random requests, answers and guest printouts: never a crash, never a read past the buffer (ASan), and anything
 * accepted is within its limits */
static void fuzz(void) {
    unsigned s = 12345;
    uint8_t *b = malloc(70000);
    VmxReq *r = malloc(sizeof *r);
    VmxRes x;
    const char *av[] = { "a", "bb", "ccc" };
    size_t sn;
    uint8_t *seed = vmx_req_encode(9, 3, av, (const uint8_t *)"in", 2, &sn);
    VmxRes rs = { .exit = 1, .out = (const uint8_t *)"o", .outlen = 1, .err = (const uint8_t *)"e", .errlen = 1 };
    size_t rn;
    uint8_t *rseed = vmx_res_encode(&rs, &rn);
    for (int it = 0; it < 300000; it++) {
        s = s * 1103515245u + 12345u;
        int kind = (int)(s >> 16) % 3;
        size_t n;
        if (kind == 0) { memcpy(b, seed, sn); n = sn; }
        else if (kind == 1) { memcpy(b, rseed, rn); n = rn; }
        else { memcpy(b, "LVXG 0 2 1\nabc", 14); n = 14; }
        int flips = 1 + (int)((s >> 8) % 4);
        for (int k = 0; k < flips; k++) { s = s * 1103515245u + 12345u; b[(s >> 8) % n] = (uint8_t)(s >> 20); }
        s = s * 1103515245u + 12345u;
        if ((s >> 12) % 4 == 0) n = (s >> 16) % (n + 1);
        uint8_t *c = malloc(n ? n : 1);
        memcpy(c, b, n);
        if (kind == 0 && !vmx_req_parse(c, n, r)) { CHECK(r->argc >= 1 && r->argc <= VMX_ARGC_MAX && r->inlen <= VMX_STDIN_MAX); for (int i = 0; i < r->argc; i++) CHECK(strlen(r->argv[i]) >= 1); }
        if (kind == 1 && !vmx_res_parse(c, n, &x)) CHECK(x.outlen + x.errlen + 20 == n && x.exit >= 0 && x.exit <= 255);
        if (kind == 2 && !vmx_guest_parse(c, n, &x)) CHECK(x.outlen <= VMX_OUT_MAX && x.errlen <= VMX_ERR_MAX && x.exit <= 255);
        free(c);
    }
    free(seed); free(rseed); free(r); free(b);
}

int main(void) {
    test_req();
    test_res();
    test_guest();
    test_memfd();
    test_paths();
    test_scripts();
    fuzz();
    printf("vmexec-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
