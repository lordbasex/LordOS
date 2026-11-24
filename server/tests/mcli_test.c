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

int main(void) {
    test_sizes();
    test_resolve();
    test_set();
    test_folder();
    test_state();
    test_fill();
    test_channel();
    fuzz_channel();
    if (fails) { fprintf(stderr, "mcli-test: %d failed\n", fails); return 1; }
    printf("mcli-test: ok\n");
    return 0;
}
