/* mcli_test.c — the machines command's pure parts (src/cli/mcli.c) and its session channel's lines (src/vm/vmcli.c):
 * sizes, name resolution, set's options as lord-vm's, folder names, run.json states, placeholders, JSON strings, and the
 * request parser (strict: every accepted line must format back to itself; a fuzz pass of mutated and random lines). */
#include "../src/cli/mcli.h"
#include "../src/vm/vmcli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void test_sizes(void) {
    CHECK(mcli_size_mb("8G", 1) == 8192);
    CHECK(mcli_size_mb("8 GB", 1) == 8192);
    CHECK(mcli_size_mb("8gib", 1) == 8192);
    CHECK(mcli_size_mb("1.5G", 1) == 1536);
    CHECK(mcli_size_mb("512M", 1) == 512);
    CHECK(mcli_size_mb("512MiB", 1) == 512);
    CHECK(mcli_size_mb("4096", 1) == 4096);
    CHECK(mcli_size_mb("64", 1024) == 65536);
    CHECK(mcli_size_mb("2T", 1024) == 2 * 1024 * 1024);
    CHECK(mcli_size_mb("0", 1) == -1);
    CHECK(mcli_size_mb("", 1) == -1);
    CHECK(mcli_size_mb("G", 1) == -1);
    CHECK(mcli_size_mb("-1G", 1) == -1);
    CHECK(mcli_size_mb("1.G", 1) == -1);
    CHECK(mcli_size_mb("1.0001M", 1) == -1);   /* not a whole MB */
    CHECK(mcli_size_mb("8X", 1) == -1);
    CHECK(mcli_size_mb("8GBB", 1) == -1);
    CHECK(mcli_size_mb("99999999999999G", 1) == -1);
    CHECK(mcli_size_mb(NULL, 1) == -1);
}

static void test_resolve(void) {
    const char *names[] = { "Ubuntu 26.04", "Ubuntu 24.04", "Windows 11", "win", "Debian" };
    int idx[8];
    CHECK(mcli_resolve(names, 5, "debian", idx, 8) == 1 && idx[0] == 4);
    CHECK(mcli_resolve(names, 5, "DEB", idx, 8) == 1 && idx[0] == 4);
    CHECK(mcli_resolve(names, 5, "ubuntu", idx, 8) == 2);                  /* ambiguous: both listed */
    CHECK(idx[0] == 0 && idx[1] == 1);
    CHECK(mcli_resolve(names, 5, "ubuntu 26", idx, 8) == 1 && idx[0] == 0);
    CHECK(mcli_resolve(names, 5, "WIN", idx, 8) == 1 && idx[0] == 3);       /* an exact name beats a longer one it starts */
    CHECK(mcli_resolve(names, 5, "wind", idx, 8) == 1 && idx[0] == 2);
    CHECK(mcli_resolve(names, 5, "mac", idx, 8) == 0);
    CHECK(mcli_resolve(names, 5, "", idx, 8) == 0);
    CHECK(mcli_resolve(names, 5, "Ubuntu 26.04 LTS", idx, 8) == 0);
    CHECK(mcli_resolve(names, 5, "u", idx, 1) == 2 && idx[0] == 0);         /* more than max: counted, not written */
}

static int argv_is(char **out, const char *const *want) {
    int i = 0;
    for (; want[i]; i++) if (!out[i] || strcmp(out[i], want[i])) return 0;
    return out[i] == NULL;
}

static void test_set(void) {
    char *out[64];
    McliStore st;
    const char *bo, *bv;
    char *a1[] = { "--memory", "8G", "--cpus", "6", "--disk=64G", "--clipboard", "both", "--network", "host-only" };
    CHECK(!mcli_set_args(9, a1, out, 64, &st, &bo, &bv));
    const char *w1[] = { "--memory", "8192", "--vcpus", "6", "--disk", "64", "--clipboard", "both", "--network", "hostonly", NULL };
    CHECK(argv_is(out, w1));
    char *a2[] = { "--port", "tcp:2222:22", "--port", "udp:5000:5000:lan", "--share=rw:Docs:/home/a/Docs", "--name", "New name" };
    CHECK(!mcli_set_args(7, a2, out, 64, &st, &bo, &bv));
    const char *w2[] = { "--port", "tcp:2222:22", "--port", "udp:5000:5000:lan", "--share", "rw:Docs:/home/a/Docs", "--name", "New name", NULL };
    CHECK(argv_is(out, w2));
    /* the performance options of 3.7.1, 7.1.4 and 6.25 pass to lord-vm, which checks their values */
    char *a2b[] = { "--cpu-pin", "auto", "--perf-profile=on", "--capture-fullscreen", "off" };
    CHECK(!mcli_set_args(5, a2b, out, 64, &st, &bo, &bv));
    const char *w2b[] = { "--cpu-pin", "auto", "--perf-profile", "on", "--capture-fullscreen", "off", NULL };
    CHECK(argv_is(out, w2b));
    char *a3[] = { "--memory", "lots" };
    CHECK(mcli_set_args(2, a3, out, 64, &st, &bo, &bv) && !strcmp(bo, "--memory") && bv && !strcmp(bv, "lots"));
    char *a4[] = { "--disk", "1500M" };                                      /* not whole GB */
    CHECK(mcli_set_args(2, a4, out, 64, &st, &bo, &bv) && bv);
    char *a5[] = { "--cpus", "0" };
    CHECK(mcli_set_args(2, a5, out, 64, &st, &bo, &bv));
    char *a6[] = { "--cpus", "65" };
    CHECK(mcli_set_args(2, a6, out, 64, &st, &bo, &bv));
    char *a7[] = { "--qmp", "x" };                                           /* not one of --set's */
    CHECK(mcli_set_args(2, a7, out, 64, &st, &bo, &bv) && !bv && !strcmp(bo, "--qmp"));
    char *a8[] = { "--memory" };                                             /* no value */
    CHECK(mcli_set_args(1, a8, out, 64, &st, &bo, &bv) && !bv);
    char *a9[] = { "memory", "8G" };
    CHECK(mcli_set_args(2, a9, out, 64, &st, &bo, &bv));
    char *a10[] = { "--network", "bridge" };
    CHECK(mcli_set_args(2, a10, out, 64, &st, &bo, &bv) && bv);
    CHECK(mcli_set_args(0, a10, out, 64, &st, &bo, &bv));                   /* nothing to set */
    char *a11[] = { "--start", "x" };                                        /* never another lord-vm command */
    CHECK(mcli_set_args(2, a11, out, 64, &st, &bo, &bv));
    char *a12[] = { "--=x" };
    CHECK(mcli_set_args(1, a12, out, 64, &st, &bo, &bv));
    /* out's room: never past max */
    char *many[40];
    for (int i = 0; i < 40; i += 2) { many[i] = "--sound"; many[i + 1] = "on"; }
    CHECK(mcli_set_args(40, many, out, 8, &st, &bo, &bv));
}

static void test_folder(void) {
    char f[64];
    mcli_folder_name("Ubuntu 26.04", f, sizeof f); CHECK(!strcmp(f, "Ubuntu 26.04"));
    mcli_folder_name("Win/11: Pro", f, sizeof f); CHECK(!strcmp(f, "Win-11- Pro"));
    mcli_folder_name("Débian", f, sizeof f); CHECK(!strcmp(f, "D-bian"));
    mcli_folder_name(".hidden", f, sizeof f); CHECK(!strcmp(f, "-hidden"));
    mcli_folder_name("///", f, sizeof f); CHECK(!strcmp(f, "Machine"));
    mcli_folder_name("..", f, sizeof f); CHECK(!strcmp(f, "Machine"));
}

static void test_state(void) {
    McliState s;
    const char *r1 = "{\"state\":\"running\",\"reason\":\"user\",\"pid\":1234,\"qemu_pid\":99,\"since\":\"2026-11-21T00:00:00Z\"}";
    CHECK(mcli_run_pid(r1, strlen(r1)) == 1234);
    mcli_state(r1, strlen(r1), 1, &s);
    CHECK(!strcmp(s.state, "running") && !strcmp(s.view, "running") && s.active && s.qemu_pid == 99);
    mcli_state(r1, strlen(r1), 0, &s);                                       /* its lord-vm is gone */
    CHECK(!strcmp(s.state, "stopped") && !strcmp(s.reason, "supervisor-lost") && !strcmp(s.view, "recovered") && !s.active && !s.qemu_pid);
    const char *r2 = "{\"state\":\"stopped\",\"reason\":\"suspended\"}";
    mcli_state(r2, strlen(r2), 0, &s);
    CHECK(!strcmp(s.view, "suspended"));
    const char *r3 = "{\"state\":\"stopping\",\"reason\":\"suspended\",\"pid\":5}";
    mcli_state(r3, strlen(r3), 1, &s);
    CHECK(!strcmp(s.view, "suspending"));
    const char *r4 = "{\"state\":\"exploded\",\"reason\":\"why\"}";
    mcli_state(r4, strlen(r4), 1, &s);
    CHECK(!strcmp(s.state, "stopped") && !strcmp(s.reason, "none") && !strcmp(s.view, "stopped"));
    mcli_state(NULL, 0, 0, &s);
    CHECK(!strcmp(s.view, "stopped"));
    mcli_state("not json", 8, 1, &s);
    CHECK(!strcmp(s.view, "stopped"));
    const char *r5 = "{\"state\":\"crashed\",\"reason\":\"qemu-failed\"}";
    mcli_state(r5, strlen(r5), 0, &s);
    CHECK(!strcmp(s.view, "crashed") && !s.active);
}

static void test_fill(void) {
    char o[64];
    const char *kv[] = { "name", "Debian", "why", "{name}", NULL };
    mcli_fill("{name} didn't start: {why}", kv, o, sizeof o);
    CHECK(!strcmp(o, "Debian didn't start: {name}"));                      /* values aren't filled again */
    mcli_fill("{unknown} {name", kv, o, sizeof o);
    CHECK(!strcmp(o, "{unknown} {name"));
    char small[8];
    mcli_fill("ééééé", NULL, small, sizeof small);                          /* cut on a character's boundary */
    CHECK(!strcmp(small, "ééé"));
    FILE *f = tmpfile();
    mcli_json_str(f, "a\"b\\c\n\x01");
    rewind(f);
    char b[64] = "";
    CHECK(fgets(b, sizeof b, f) && !strcmp(b, "\"a\\\"b\\\\c\\n\\u0001\""));
    fclose(f);
}

static void test_channel(void) {
    VmCliReq r;
    char line[VMCLI_LINE_MAX + 8];
#define OK(s) CHECK(!vmcli_req_parse(s, strlen(s), &r))
#define NO(s) CHECK(vmcli_req_parse(s, strlen(s), &r))
    OK("open /home/a/Machines/Debian.lordvm\n");
    CHECK(r.verb == VMCLI_OPEN && !strcmp(r.dir, "/home/a/Machines/Debian.lordvm"));
    OK("start /home/a/Machines/Windows 11 Pro (prueba).lordvm\n");
    CHECK(r.verb == VMCLI_START);
    OK("start-detached /m/x.lordvm\n");
    CHECK(r.verb == VMCLI_START_DETACHED);
    OK("release /m/x.lordvm\n");
    CHECK(r.verb == VMCLI_RELEASE);
    OK("open /home/a/Machines/Débian ✓.lordvm\n");
    NO("open /home/a/Machines/Debian.lordvm");                 /* no newline */
    NO("open /home/a/Machines/Debian.lordvm\n\n");
    NO("open  /home/a/Machines/Debian.lordvm\n");               /* two spaces: the path would start with ' ' */
    NO("OPEN /m/x.lordvm\n");
    NO("run /m/x.lordvm\n");
    NO("open\n");
    NO("open \n");
    NO("open x.lordvm\n");                                      /* relative */
    NO("open /m/x\n");                                          /* not a machine folder */
    NO("open /m/.lordvm\n");
    NO("open /m/x.lordvm/\n");
    NO("open /m//x.lordvm\n");
    NO("open /m/../x.lordvm\n");
    NO("open /m/./x.lordvm\n");
    NO("open /m/x\t.lordvm\n");
    NO("open /m/x\x7f.lordvm\n");
    NO("open /m/x.lordvm --no-window\n");                       /* nothing after the path: its name would end otherwise */
    NO("start-detached\t/m/x.lordvm\n");
    /* a machine group's folder (docs/VIRTUALIZATION.md 19.9) */
    OK("start-detached /home/a/Machines/Whonix.lordgroup\n");
    CHECK(r.verb == VMCLI_START_DETACHED && !strcmp(r.dir, "/home/a/Machines/Whonix.lordgroup"));
    NO("open /m/.lordgroup\n");
    NO("open /m/x.lordgroup/\n");
    NO("open /m/../x.lordgroup\n");
    NO("open /m/x.lordgroups\n");
    NO("open /m/x.LORDGROUP\n");
    CHECK(vmcli_req_parse("open /m/x\0.lordvm\n", 17, &r));
    /* the longest path, and one byte more */
    memcpy(line, "open /", 6);
    memset(line + 6, 'a', VMCLI_DIR_MAX - 8);
    memcpy(line + 6 + VMCLI_DIR_MAX - 8, ".lordvm\n", 9);
    CHECK(!vmcli_req_parse(line, strlen(line), &r) && strlen(r.dir) == VMCLI_DIR_MAX);
    memcpy(line + 6, "aa", 2);
    memset(line + 6, 'a', VMCLI_DIR_MAX - 6);
    memcpy(line + 6 + VMCLI_DIR_MAX - 6, ".lordvm\n", 9);
    CHECK(vmcli_req_parse(line, strlen(line), &r));
    /* format → parse round trip, and refusals */
    char out[VMCLI_LINE_MAX + 1];
    size_t n = vmcli_req_format(VMCLI_START, "/home/a/Machines/Debian.lordvm", out, sizeof out);
    CHECK(n && !strcmp(out, "start /home/a/Machines/Debian.lordvm\n"));
    CHECK(!vmcli_req_format(VMCLI_START, "/home/a/Machines/../x.lordvm", out, sizeof out));
    CHECK(!vmcli_req_format(9, "/m/x.lordvm", out, sizeof out));
    CHECK(!vmcli_req_format(VMCLI_OPEN, "/m/x.lordvm", out, 8));
    /* answers */
    char ans[VMCLI_ANSWER_MAX + 1], text[VMCLI_TEXT_MAX + 1];
    int code;
    n = vmcli_answer_format(0, "Debian is starting in its window.", ans, sizeof ans);
    CHECK(!vmcli_answer_parse(ans, n, &code, text, sizeof text) && code == 0 && !strcmp(text, "Debian is starting in its window."));
    n = vmcli_answer_format(3, "line\nbreak\x01 and \xff bad", ans, sizeof ans);
    CHECK(!vmcli_answer_parse(ans, n, &code, text, sizeof text) && code == 3 && !strcmp(text, "line break  and ? bad"));
    n = vmcli_answer_format(42, NULL, ans, sizeof ans);
    CHECK(!vmcli_answer_parse(ans, n, &code, text, sizeof text) && code == VMCLI_FAILED && !text[0]);
    char big[3000];
    for (int i = 0; i < 1000; i++) memcpy(big + i * 2, "é", 2);
    big[2000] = 0;
    n = vmcli_answer_format(1, big, ans, sizeof ans);
    CHECK(n <= VMCLI_ANSWER_MAX && !vmcli_answer_parse(ans, n, &code, text, sizeof text) && strlen(text) == VMCLI_TEXT_MAX);
    CHECK(vmcli_answer_parse("0 x", 3, &code, text, sizeof text));
    CHECK(vmcli_answer_parse("x ok\n", 5, &code, text, sizeof text));
    CHECK(vmcli_answer_parse("0 a\x01\n", 5, &code, text, sizeof text));
    CHECK(vmcli_answer_parse("0 \xc3\n", 4, &code, text, sizeof text));
    CHECK(!vmcli_answer_parse("0 \n", 3, &code, text, sizeof text) && code == 0 && !text[0]);
}

/* random and mutated lines: the parser never reads past n, and whatever it accepts is a clean request that formats back
 * to the same line */
static void fuzz_channel(void) {
    static const char *seeds[] = { "open /home/a/Machines/Debian.lordvm\n", "start-detached /m/x.lordvm\n", "release /a/b c/d.lordvm\n" };
    unsigned s = 12345;
    int accepted = 0;
    for (int it = 0; it < 200000; it++) {
        char buf[VMCLI_LINE_MAX + 16];
        size_t n;
        s = s * 1103515245u + 12345u;
        if (it & 1) {
            const char *seed = seeds[(s >> 8) % 3];
            n = strlen(seed);
            memcpy(buf, seed, n);
            int muts = 1 + (int)((s >> 4) % 4);
            for (int m = 0; m < muts; m++) {
                s = s * 1103515245u + 12345u;
                size_t at = (s >> 8) % n;
                switch ((s >> 20) % 4) {
                case 0: buf[at] = (char)(s >> 12); break;
                case 1: if (n < sizeof buf - 1) { memmove(buf + at + 1, buf + at, n - at); buf[at] = "/.\n \0"[(s >> 16) % 5]; n++; } break;
                case 2: if (n > 1) { memmove(buf + at, buf + at + 1, n - at - 1); n--; } break;
                default: buf[at] = '/'; break;
                }
            }
        } else {
            n = (s >> 8) % 64;
            for (size_t i = 0; i < n; i++) { s = s * 1103515245u + 12345u; buf[i] = "openstartdlrcv-/ .x\n"[(s >> 16) % 21]; }
        }
        char *heap = malloc(n ? n : 1);              /* exact size: ASan catches a read past it */
        memcpy(heap, buf, n);
        VmCliReq r;
        if (!vmcli_req_parse(heap, n, &r)) {
            accepted++;
            char again[VMCLI_LINE_MAX + 1];
            size_t k = vmcli_req_format(r.verb, r.dir, again, sizeof again);
            if (k != n || memcmp(again, heap, n)) { fprintf(stderr, "FAIL fuzz: accepted %.*s", (int)n, heap); fails++; }
            if (strstr(r.dir, "/../") || strstr(r.dir, "/./") || strstr(r.dir, "//") || strchr(r.dir, '\n')) { fprintf(stderr, "FAIL fuzz: dir %s\n", r.dir); fails++; }
        }
        int code;
        char text[VMCLI_TEXT_MAX + 1];
        (void)vmcli_answer_parse(heap, n, &code, text, sizeof text);
        free(heap);
    }
    CHECK(accepted > 0);
}


/* ---- exec, cp, logs, mcp and apply (6.15, 6.24) ---- */

static void test_cp_side(void) {
    char n[65];
    const char *p;
    CHECK(mcli_cp_side("Debian Dev:~/build", n, sizeof n, &p) && !strcmp(n, "Debian Dev") && !strcmp(p, "~/build"));
    CHECK(mcli_cp_side("deb:", n, sizeof n, &p) && !strcmp(n, "deb") && !strcmp(p, "~"));
    CHECK(mcli_cp_side("deb:/etc/hosts", n, sizeof n, &p) && !strcmp(p, "/etc/hosts"));
    CHECK(!mcli_cp_side("./a:b", n, sizeof n, &p) && !mcli_cp_side("/tmp/x:y", n, sizeof n, &p) && !mcli_cp_side(":x", n, sizeof n, &p) &&
          !mcli_cp_side("plain", n, sizeof n, &p) && !mcli_cp_side(NULL, n, sizeof n, &p));
    char small[4];
    CHECK(!mcli_cp_side("toolong:x", small, sizeof small, &p));
}

/* cp of a folder: the relative paths and the guest's listing, both hostile */
static void test_tree(void) {
    CHECK(mcli_rel_path_ok("a") && mcli_rel_path_ok("src/main.c") && mcli_rel_path_ok(".hidden/x") && mcli_rel_path_ok("a b/\xc3\xa9t\xc3\xa9") && mcli_rel_path_ok("..x/y.."));
    CHECK(!mcli_rel_path_ok("") && !mcli_rel_path_ok(NULL) && !mcli_rel_path_ok("/etc/passwd") && !mcli_rel_path_ok("a/") && !mcli_rel_path_ok("a//b"));
    CHECK(!mcli_rel_path_ok("..") && !mcli_rel_path_ok("../x") && !mcli_rel_path_ok("a/../../x") && !mcli_rel_path_ok("a/./b") && !mcli_rel_path_ok("."));
    CHECK(!mcli_rel_path_ok("a\nb") && !mcli_rel_path_ok("a\x1b[2Jb") && !mcli_rel_path_ok("del\x7f"));
    CHECK(!mcli_rel_path_ok("x\xe2\x80\xaetxt.exe") && !mcli_rel_path_ok("a/\xe2\x81\xa6" "b") && !mcli_rel_path_ok("ls\xe2\x80\xa8x") && mcli_rel_path_ok("dash\xe2\x80\x94ok"));
    {
        /* a hostile listing: "..", an absolute path, too deep, too many entries: each refused by the parser */
        static const char H1[] = "d a\0d a/..\0";
        size_t o = 0; McliTreeEnt h;
        CHECK(mcli_tree_next(H1, sizeof H1 - 1, &o, &h) == 1 && mcli_tree_next(H1, sizeof H1 - 1, &o, &h) == -1);
        char deep[256] = "d ";
        for (int i = 0; i <= MCLI_TREE_DEPTH; i++) strcat(deep, i ? "/x" : "x");
        o = 0;
        CHECK(mcli_tree_next(deep, strlen(deep) + 1, &o, &h) == -1);
    }
    {
        char deep[200] = "";
        for (int i = 0; i < MCLI_TREE_DEPTH; i++) strcat(deep, i ? "/d" : "d");
        CHECK(mcli_rel_path_ok(deep));
        strcat(deep, "/d");
        CHECK(!mcli_rel_path_ok(deep));
        char longc[300];
        memset(longc, 'x', 256); longc[256] = 0;
        CHECK(!mcli_rel_path_ok(longc));
        longc[255] = 0;
        CHECK(mcli_rel_path_ok(longc));
    }
    static const char L[] = "d src\0f 12 src/a.c\0f 0 empty\0d src/sub\0";
    size_t off = 0;
    McliTreeEnt e;
    CHECK(mcli_tree_next(L, sizeof L - 1, &off, &e) == 1 && e.dir && !strcmp(e.path, "src"));
    CHECK(mcli_tree_next(L, sizeof L - 1, &off, &e) == 1 && !e.dir && e.size == 12 && !strcmp(e.path, "src/a.c"));
    CHECK(mcli_tree_next(L, sizeof L - 1, &off, &e) == 1 && !e.dir && e.size == 0 && !strcmp(e.path, "empty"));
    CHECK(mcli_tree_next(L, sizeof L - 1, &off, &e) == 1 && e.dir && !strcmp(e.path, "src/sub"));
    CHECK(mcli_tree_next(L, sizeof L - 1, &off, &e) == 0);
    static const char *const BAD[] = { "d ../x", "f 1 /etc/shadow", "f x a", "f 1a b", "l a", "d ", "f 12345678901234567 a", "f  a", "d a/../b", "fx 1 a" };
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) { off = 0; CHECK(mcli_tree_next(BAD[i], strlen(BAD[i]) + 1, &off, &e) == -1); }
    off = 0;
    CHECK(mcli_tree_next("f 1 cut", 7, &off, &e) == -1);       /* no NUL: the listing was cut */
    /* fuzz: random bytes never read past the buffer (ASan) and every entry taken is a safe relative path */
    unsigned s = 77;
    for (int r = 0; r < 20000; r++) {
        char b[64];
        size_t n = 1 + (s = s * 1103515245u + 12345u) % sizeof b;
        for (size_t i = 0; i < n; i++) { s = s * 1103515245u + 12345u; b[i] = (char)((s >> 16) % 5 == 0 ? 0 : "df ./.a1\n9"[(s >> 8) % 10]); }
        off = 0;
        for (int k = 0; k < 64; k++) { int g = mcli_tree_next(b, n, &off, &e); if (g <= 0) break; CHECK(mcli_rel_path_ok(e.path) && e.path >= b && e.path < b + n); }
    }
}

static void test_json_bytes(void) {
    char buf[256];
    FILE *f = fmemopen(buf, sizeof buf, "w");
    const uint8_t b[] = { 'a', '"', '\\', '\n', 0x01, 0xc3, 0xb1, 0xff, 0xe2, 0x82, 0xc0, 0xaf, 0xed, 0xa0, 0x80, 'z' };
    mcli_json_bytes(f, b, sizeof b);
    fclose(f);
    CHECK(!strcmp(buf, "\"a\\\"\\\\\\n\\u0001ñ\\ufffd\\ufffd\\ufffd\\ufffd\\ufffd\\ufffd\\ufffd\\ufffdz\""));
    /* whatever comes in, the result parses as JSON */
    unsigned s = 7;
    for (int it = 0; it < 20000; it++) {
        uint8_t r[64];
        for (int i = 0; i < 64; i++) { s = s * 1103515245u + 12345u; r[i] = (uint8_t)(s >> 16); }
        char out[2048];
        FILE *g = fmemopen(out, sizeof out, "w");
        mcli_json_bytes(g, r, (size_t)(s % 64));
        long len = ftell(g);
        fclose(g);
        JDoc d;
        int ok = len > 0 && !json_parse(out, (size_t)len, &d, NULL, 0);
        CHECK(ok);
        if (ok) json_free(&d);
    }
}

static void test_term_clean(void) {
    char o[64];
    const uint8_t b[] = { 'o', 'k', '\n', 0x1b, '[', '2', 'J', 0xc2, 0x9b, '1', 0xc3, 0xa9, 0x7f, '\t', 0xff };
    size_t n = mcli_term_clean(b, sizeof b, o, sizeof o);
    CHECK(n == strlen(o) && !strcmp(o, "ok\n?[2J?1é?\t?"));
    for (size_t i = 0; i < n; i++) CHECK((unsigned char)o[i] != 0x1b);
    CHECK(mcli_term_clean(b, sizeof b, o, 5) <= 4);
}

static int apply_ok(const char *t, McliApply *a) {
    int line;
    const char *why;
    return !mcli_apply_parse(t, strlen(t), a, &line, &why);
}

static void test_apply(void) {
    McliApply *a = malloc(sizeof *a);
    const char good[] =
        "# dev machine\n[machine]\nname = \"Debian Dev\"   # its name\nmemory = \"8G\"\ncpus = 6\ndisk = \"64G\"\nnetwork = \"shared\"\n"
        "agent = true\nports = [\"tcp:2222:22\", \"tcp:8080:80:lan\",]\nshares = [ \"ro:src:/home/a/src\" ]\niso = \"/home/a/Descargas/debian.iso\"\r\n";
    CHECK(apply_ok(good, a) && !strcmp(a->name, "Debian Dev") && !strcmp(a->memory, "8G") && a->cpus == 6 && !strcmp(a->disk, "64G") && a->agent == 1 &&
          a->n_ports == 2 && !strcmp(a->ports[1], "tcp:8080:80:lan") && a->ports_set && a->n_shares == 1 && !strcmp(a->shares[0], "ro:src:/home/a/src") &&
          !strcmp(a->iso, "/home/a/Descargas/debian.iso") && !a->graphics[0]);
    CHECK(apply_ok("name = \"x\"\nports = []\n", a) && a->ports_set && !a->n_ports && a->cpus == -1 && a->agent == -1);
    CHECK(apply_ok("name = \"a\\\"b\\\\c\"", a) && !strcmp(a->name, "a\"b\\c"));
    CHECK(apply_ok("name = \"Año ñ\"", a) && !strcmp(a->name, "Año ñ"));
    const char *bad[] = {
        "", "memory = \"8G\"\n", "name = \"x\"\nname = \"y\"\n", "name = \"x\"\nautomation = true\n", "name = \"x\"\n[machine]\n",
        "[machine]\n[machine]\nname=\"x\"", "[other]\nname=\"x\"", "name = x\n", "name = \"x\" y\n", "name = \"x\ny\"\n", "name = \"\"\n",
        "name = \"x\"\ncpus = 0\n", "name = \"x\"\ncpus = 65\n", "name = \"x\"\ncpus = 07\n", "name = \"x\"\ncpus = 12345\n", "name = \"x\"\ncpus = \"4\"\n",
        "name = \"x\"\nagent = yes\n", "name = \"x\"\nports = [\"a\" \"b\"]\n", "name = \"x\"\nports = [\"a\",\n\"b\"]\n", "name = \"x\"\nports = \"a\"\n",
        "name = \"\\u0041\"\n", "name = \"x\\ty\x01\"\n", "name = \"\xff\"\n", "NAME = \"x\"\n", "name \"x\"\n", "[machine] x\nname = \"x\"\n",
        "name = \"0123456789012345678901234567890123456789012345678901234567890123456789\"\n",
        "name = \"x\"\nshares = [\"a\",\"b\",\"c\",\"d\",\"e\"]\n", "name = \"x\"\nmemory = \"8G\" # ok\ndisk = \"1\" \"2\"\n",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) { if (apply_ok(bad[i], a)) fprintf(stderr, "  apply accepted %zu\n", i); CHECK(!apply_ok(bad[i], a)); }
    {
        int line; const char *why;
        CHECK(mcli_apply_parse("name = \"x\"\n\ncpus = 99\n", 22, a, &line, &why) && line == 3 && why[0]);
        char nul[] = "name = \"x\"\0";
        CHECK(mcli_apply_parse(nul, sizeof nul - 1, a, &line, &why));
        char *big = malloc(MCLI_APPLY_MAX + 2);
        memset(big, '#', MCLI_APPLY_MAX + 1);
        CHECK(mcli_apply_parse(big, MCLI_APPLY_MAX + 1, a, &line, &why));
        free(big);
    }
    /* fuzz: flipped bytes of the good file never crash and never give a value past its buffer */
    unsigned s = 99;
    char t[sizeof good];
    for (int it = 0; it < 100000; it++) {
        memcpy(t, good, sizeof good);
        for (int k = 0; k < 3; k++) { s = s * 1103515245u + 12345u; t[(s >> 8) % (sizeof good - 1)] = (char)(s >> 20); }
        int line; const char *why;
        size_t n = strlen(t);
        if (!mcli_apply_parse(t, n, a, &line, &why)) CHECK(strlen(a->name) < sizeof a->name && a->n_ports <= MCLI_APPLY_PORTS && a->n_shares <= MCLI_APPLY_SHARES);
    }
    free(a);
}

static void test_rpc(void) {
    McliRpc r;
    const char *ok1 = "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/list\"}";
    CHECK(!mcli_rpc_parse(ok1, strlen(ok1), &r) && !strcmp(r.method, "tools/list") && r.has_id && !strcmp(r.id, "7") && !r.params);
    mcli_rpc_free(&r);
    const char *ok2 = "{\"jsonrpc\":\"2.0\",\"id\":\"a\\\"b\",\"method\":\"tools/call\",\"params\":{\"name\":\"x\"}}";
    CHECK(!mcli_rpc_parse(ok2, strlen(ok2), &r) && !strcmp(r.id, "\"a\\\"b\"") && r.params && r.params->type == J_OBJ);
    mcli_rpc_free(&r);
    const char *ok3 = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
    CHECK(!mcli_rpc_parse(ok3, strlen(ok3), &r) && !r.has_id);
    mcli_rpc_free(&r);
    const char *bad[] = {
        "", "[]", "{}", "{\"jsonrpc\":\"1.0\",\"id\":1,\"method\":\"x\"}", "{\"jsonrpc\":\"2.0\",\"id\":1}", "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":1.5,\"method\":\"x\"}", "{\"jsonrpc\":\"2.0\",\"id\":null,\"method\":\"x\"}", "{\"jsonrpc\":\"2.0\",\"id\":{},\"method\":\"x\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"x\",\"params\":3}", "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"x\",\"extra\":1}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"x\"} trailing", "{\"jsonrpc\":\"2.0\",\"id\":1e300,\"method\":\"x\"}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int got = !mcli_rpc_parse(bad[i], strlen(bad[i]), &r);
        if (got) { fprintf(stderr, "  rpc accepted %zu\n", i); mcli_rpc_free(&r); }
        CHECK(!got);
    }
    /* the longest string id still fits escaped */
    char line[1200], id[129];
    memset(id, '"', 128); id[128] = 0;
    char esc[400];
    size_t o = 0;
    for (int i = 0; i < 128; i++) { esc[o++] = '\\'; esc[o++] = '"'; }
    esc[o] = 0;
    snprintf(line, sizeof line, "{\"jsonrpc\":\"2.0\",\"id\":\"%s\",\"method\":\"ping\"}", esc);
    CHECK(!mcli_rpc_parse(line, strlen(line), &r) && strlen(r.id) == 2 + 256);
    mcli_rpc_free(&r);
    (void)id;
}

/* the security review of 2026-11-24: an MCP name never a path; an apply file's boundary settings listed; its ISO placed */
static void test_review(void) {
    CHECK(mcli_mcp_name_ok("Debian Dev") && mcli_mcp_name_ok("Año") && mcli_mcp_name_ok("ceb6f9df-1627-4006-b3bb-0043f9938883"));
    CHECK(!mcli_mcp_name_ok(NULL) && !mcli_mcp_name_ok("") && !mcli_mcp_name_ok("/tmp/evil.lordvm") && !mcli_mcp_name_ok("evil.lordvm") &&
          !mcli_mcp_name_ok("evil.LORDVM") && !mcli_mcp_name_ok("../Machines/x") && !mcli_mcp_name_ok("w.lordgroup") && !mcli_mcp_name_ok("..") &&
          !mcli_mcp_name_ok("a\nb") && !mcli_mcp_name_ok("x/"));
    McliApply *a = malloc(sizeof *a);
    int line;
    const char *why;
    char out[2000];
    /* a hostile file: every boundary setting at once */
    const char hostile[] = "name = \"Dev\"\niso = \"/home/a/Downloads/x.iso\"\nports = [\"tcp:2222:22:lan\"]\nshares = [\"rw:h:/home/a\"]\n"
                           "network = \"bridged\"\nclipboard = \"both\"\nagent = true\nmemory = \"2G\"\n";
    CHECK(!mcli_apply_parse(hostile, strlen(hostile), a, &line, &why));
    CHECK(mcli_apply_risks(a, out, sizeof out) == 6 && strstr(out, "tcp:2222:22:lan") && strstr(out, "rw:h:/home/a") && strstr(out, "bridged") &&
          strstr(out, "both") && strstr(out, "agent") && strstr(out, "x.iso"));
    const char host_only[] = "name = \"Dev\"\nnetwork = \"host-only\"\n";
    CHECK(!mcli_apply_parse(host_only, strlen(host_only), a, &line, &why) && mcli_apply_risks(a, out, sizeof out) == 1);
    const char calm[] = "name = \"Dev\"\nmemory = \"2G\"\ncpus = 2\nnetwork = \"isolated\"\nclipboard = \"off\"\nagent = false\nports = []\n";
    CHECK(!mcli_apply_parse(calm, strlen(calm), a, &line, &why) && mcli_apply_risks(a, out, sizeof out) == 0 && !out[0]);
    char tiny[8];
    CHECK(!mcli_apply_parse(hostile, strlen(hostile), a, &line, &why) && mcli_apply_risks(a, tiny, sizeof tiny) == 6 && strlen(tiny) < sizeof tiny);
    free(a);
    const char *roots[] = { "/home/a/Downloads", "/home/a/Machines", NULL };
    CHECK(mcli_iso_place_ok("/home/a/Downloads/debian.iso", roots) && mcli_iso_place_ok("/home/a/Machines/.images/x.IMG", roots));
    CHECK(!mcli_iso_place_ok("/etc/passwd", roots) && !mcli_iso_place_ok("/home/a/Downloads/x.qcow2", roots) && !mcli_iso_place_ok("/home/a/x.iso", roots) &&
          !mcli_iso_place_ok("/home/a/Downloads2/x.iso", roots) && !mcli_iso_place_ok("/home/a/Downloads/../.ssh/x.iso", roots) &&
          !mcli_iso_place_ok("x.iso", roots) && !mcli_iso_place_ok("/home/a/Downloads/x.iso", NULL) && !mcli_iso_place_ok(NULL, roots));
}

/* F8 of the security review of 2026-11-24: apply's worst case (every scalar, 32 ports, 4 shares) fits its argv exactly,
 * a smaller array is refused without a write past it, and set's conversion takes all of it */
static void test_apply_argv(void) {
    static char text[MCLI_APPLY_MAX];
    size_t o = 0;
    o += (size_t)snprintf(text + o, sizeof text - o, "name = \"dev\"\nmemory = \"8G\"\ncpus = 4\ndisk = \"64G\"\nnetwork = \"shared\"\n"
                          "iso = \"/x.iso\"\ngraphics = \"2d\"\nclipboard = \"off\"\nagent = true\nports = [");
    for (int i = 0; i < MCLI_APPLY_PORTS; i++) o += (size_t)snprintf(text + o, sizeof text - o, "%s\"tcp:%d:%d\"", i ? ", " : "", 20000 + i, 22);
    o += (size_t)snprintf(text + o, sizeof text - o, "]\nshares = [");
    for (int i = 0; i < MCLI_APPLY_SHARES; i++) o += (size_t)snprintf(text + o, sizeof text - o, "%s\"ro:t%d:/srv/%d\"", i ? ", " : "", i, i);
    o += (size_t)snprintf(text + o, sizeof text - o, "]\n");
    McliApply *a = malloc(sizeof *a);
    int line;
    const char *why;
    CHECK(!mcli_apply_parse(text, o, a, &line, &why) && a->n_ports == MCLI_APPLY_PORTS && a->n_shares == MCLI_APPLY_SHARES);
    /* guarded on both sides: a canary past the end must never be touched */
    char *so[MCLI_APPLY_ARGV + 4];
    char canary[] = "canary";
    for (size_t i = 0; i < sizeof so / sizeof so[0]; i++) so[i] = canary;
    int k = mcli_apply_set_argv(a, 0, "4", so, MCLI_APPLY_ARGV);
    CHECK(k == MCLI_APPLY_ARGV - 1 && k == 2 * (8 + MCLI_APPLY_PORTS + MCLI_APPLY_SHARES) && so[k] == NULL);
    for (size_t i = MCLI_APPLY_ARGV; i < sizeof so / sizeof so[0]; i++) CHECK(so[i] == canary);
    CHECK(!strcmp(so[0], "--memory") && !strcmp(so[k - 2], "--share") && !strcmp(so[k - 1], "ro:t3:/srv/3"));
    /* one entry short (the old so[80] is far shorter): refused, nothing written at or past cap */
    for (int cap = MCLI_APPLY_ARGV - 1; cap >= 0; cap -= cap > 70 ? 1 : 13) {
        for (size_t i = 0; i < sizeof so / sizeof so[0]; i++) so[i] = canary;
        CHECK(mcli_apply_set_argv(a, 0, "4", so, cap) == -1);
        for (size_t i = (size_t)cap; i < sizeof so / sizeof so[0]; i++) CHECK(so[i] == canary);
    }
    /* a machine just made: the settings --create took are left out */
    k = mcli_apply_set_argv(a, 1, "4", so, MCLI_APPLY_ARGV);
    CHECK(k == 2 * (3 + MCLI_APPLY_PORTS + MCLI_APPLY_SHARES));   /* memory, cpus, disk, iso and a shared network: --create's */
    /* and `machines set` converts the worst case whole (its store and its out[] have room) */
    k = mcli_apply_set_argv(a, 0, "4", so, MCLI_APPLY_ARGV);
    char *out[96];
    McliStore st;
    const char *bo, *bv;
    int sr = mcli_set_args(k, so, out + 3, 96 - 3, &st, &bo, &bv);
    if (sr) fprintf(stderr, "  set_args refused %s %s\n", bo ? bo : "-", bv ? bv : "-");
    CHECK(!sr);
    free(a);
}

/* F11 of the security review of 2026-11-24: a guest's console stream keeps colors and the cursor, loses what reaches
 * past the terminal; every split of the input gives the same output */
static size_t vtf_all(const char *in, size_t n, size_t step, char *out) {
    McliVtf *f = malloc(sizeof *f);
    mcli_vtf_init(f);
    size_t o = 0;
    for (size_t i = 0; i < n; i += step) {
        size_t c = n - i < step ? n - i : step;
        o += mcli_vtf_feed(f, (const uint8_t *)in + i, c, (uint8_t *)out + o, c + MCLI_VTF_SLACK);
    }
    out[o] = 0;
    free(f);
    return o;
}
static int vtf_is(const char *in, const char *want) {
    static char out[8192];
    size_t n = strlen(in);
    for (size_t step = 1; step <= n + 1; step++) {
        vtf_all(in, n, step, out);
        if (strcmp(out, want)) { fprintf(stderr, "  vtf step %zu: got \"", step); for (char *p = out; *p; p++) fprintf(stderr, (unsigned char)*p < 0x20 || (unsigned char)*p >= 0x7f ? "\\x%02x" : "%c", (unsigned char)*p); fprintf(stderr, "\"\n"); return 0; }
    }
    return 1;
}
static void test_vtf(void) {
    /* what an interactive console needs passes */
    CHECK(vtf_is("plain text\r\n", "plain text\r\n"));
    CHECK(vtf_is("\033[1;31mred\033[0m \033[2J\033[H\033[?1049h\033[?2004h\033[10;5H", "\033[1;31mred\033[0m \033[2J\033[H\033[?1049h\033[?2004h\033[10;5H"));
    CHECK(vtf_is("\033(B\0337\0338\033M\033=\033>", "\033(B\0337\0338\033M\033=\033>"));
    CHECK(vtf_is("a\bb\tc\a", "a\bb\tc\a"));
    CHECK(vtf_is("año €😀", "año €😀"));
    CHECK(vtf_is("\033]0;guest: ~\a", "\033]0;guest: ~\a"));
    CHECK(vtf_is("\033]2;título\033\\x", "\033]2;título\ax"));
    CHECK(vtf_is("\033]11;?\a\033]4;1;rgb:ff/00/00\a\033]104\a", "\033]11;?\a\033]4;1;rgb:ff/00/00\a\033]104\a"));
    /* the clipboard, in every form, is dropped and the text around it kept */
    CHECK(vtf_is("a\033]52;c;Y3VybCBldmlsfHNoCg==\ab", "ab"));
    CHECK(vtf_is("a\033]52;c;Y3VybA==\033\\b", "ab"));
    CHECK(vtf_is("a\xc2\x9d" "52;c;Y3VybA==\xc2\x9c" "b", "ab"));
    CHECK(vtf_is("a\xc2\x9d" "52;c;Y3VybA==\ab", "ab"));
    CHECK(vtf_is("a\033]1337;Copy=:Y3VybA==\ab\033]1337;File=inline=1:AAAA\a", "ab"));
    /* links, notifications, cwd, prompt marks, unknown OSC */
    CHECK(vtf_is("\033]8;;http://evil/\afake\033]8;;\a", "fake"));
    CHECK(vtf_is("\033]9;pwned\a\033]777;notify;x;y\a\033]7;file://h/etc\a\033]133;A\a\033]99999;x\a.", "."));
    /* DCS (DECRQSS, sixel, tmux pass-through), SOS, PM, APC: dropped whole */
    CHECK(vtf_is("x\033P$qm\033\\y\033Ptmux;\033\033]52;c;QQ==\a\033\\z", "xyz"));
    CHECK(vtf_is("x\033P$qm\033\\y", "xy"));
    CHECK(vtf_is("x\033Pq#0;2;0;0;0~~\033\\y\033X sos \033\\\033^pm\033\\\033_apc\033\\z", "xyz"));
    CHECK(vtf_is("x\xc2\x90$qm\xc2\x9cy", "xy"));
    /* an ESC inside a string ends it unfinished (dropped) and starts its own escape: no OSC 52 slips through after it */
    CHECK(vtf_is("\033]0;t\033]52;c;QQ==\ax", "x"));
    CHECK(vtf_is("\033P1\033[31mx", "\033[31mx"));
    /* window operations dropped, C1 CSI rebuilt as 7-bit, other C1 dropped, broken UTF-8 replaced */
    CHECK(vtf_is("\033[8;100;300t\033[21tok", "ok"));
    CHECK(vtf_is("\xc2\x9b" "31mr", "\033[31mr"));
    CHECK(vtf_is("a\xc2\x85" "b\xc2\x84" "c", "abc"));
    CHECK(vtf_is("a\x9d" "52;c;QQ==\x07" "b", "a?52;c;QQ==\ab"));
    CHECK(vtf_is("\xff\xc3(\xe2\x82", "?" "?("));
    CHECK(vtf_is("\033[31\033]52;c;QQ==\ax", "x"));
    /* CAN and SUB abort a sequence; an over-long or bad-character OSC is dropped, never cut */
    CHECK(vtf_is("\033]52;c;QQ\x18" "after", "after"));
    CHECK(vtf_is("\033]0;a\x01" "b\ac", "c"));
    {
        static char big[MCLI_VTF_BUF + 64];
        size_t o = (size_t)snprintf(big, sizeof big, "\033]0;");
        while (o < MCLI_VTF_BUF + 10) big[o++] = 'A';
        big[o++] = '\a'; big[o++] = 'z'; big[o] = 0;
        char out[sizeof big + MCLI_VTF_SLACK];
        CHECK(vtf_all(big, o, 1, out) == 1 && !strcmp(out, "z"));
        CHECK(vtf_all(big, o, 4096, out) == 1 && !strcmp(out, "z"));
    }
    /* fuzz: whatever the guest sends, the output has no OSC but the allowed ones, no DCS/SOS/PM/APC, no C1, stays in
     * bounds, and is the same however the input is split */
    unsigned s = 12345;
    static const char *const bits[] = { "\033", "]", "52;c;", "0;", "P", "[", "\\", "\a", "\x18", "\xc2", "\x9d", "\x9c", "\x9b",
                                        "\x90", "A", ";", "1337;Copy=", "31m", "t", "\xe2\x82", "\xac", "_", "^", "X", "8;;" };
    for (int it = 0; it < 4000; it++) {
        char in[256];
        size_t n = 0;
        int parts = 1 + (int)((s = s * 1103515245u + 12345u) >> 16) % 30;
        for (int p = 0; p < parts; p++) {
            const char *b = bits[((s = s * 1103515245u + 12345u) >> 16) % (sizeof bits / sizeof bits[0])];
            size_t l = strlen(b);
            if (n + l >= sizeof in) break;
            memcpy(in + n, b, l);
            n += l;
        }
        char o1[sizeof in + MCLI_VTF_SLACK + 1], o2[sizeof in + MCLI_VTF_SLACK + 1];
        size_t l1 = vtf_all(in, n, n ? n : 1, o1), l2 = vtf_all(in, n, 1 + it % 7, o2);
        CHECK(l1 == l2 && !memcmp(o1, o2, l1));
        for (size_t i = 0; i < l1; i++) {
            unsigned char c = (unsigned char)o1[i];
            if (c == 0x1b && i + 1 < l1) {
                char nx = o1[i + 1];
                CHECK(nx != 'P' && nx != 'X' && nx != '^' && nx != '_');
                if (nx == ']') {
                    int num = atoi(o1 + i + 2);
                    CHECK(num == 0 || num == 1 || num == 2 || num == 4 || (num >= 10 && num <= 12) || num == 104 || (num >= 110 && num <= 112));
                }
            }
            if (c == 0xc2 && i + 1 < l1) CHECK((unsigned char)o1[i + 1] >= 0xa0);
        }
    }
}

int main(void) {
    test_sizes();
    test_resolve();
    test_set();
    test_folder();
    test_state();
    test_fill();
    test_channel();
    test_cp_side();
    test_tree();
    test_json_bytes();
    test_term_clean();
    test_apply();
    test_apply_argv();
    test_vtf();
    test_rpc();
    test_review();
    fuzz_channel();
    if (fails) { fprintf(stderr, "mcli-test: %d failed\n", fails); return 1; }
    printf("mcli-test: ok\n");
    return 0;
}
