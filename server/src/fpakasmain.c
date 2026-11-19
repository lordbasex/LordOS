/* lord-appstream — Flathub's AppStream catalog to the compact index (docs/FLATPAK.md, "The catalog"; AGENTS.md E7).
 * Started only by lord-server (ui/js_flatpak.c): the AppStream XML it opened on stdin, a memfd on stdout, the language
 * and the arch as arguments. It reads the file whole, then sandbox_apply_helper (no network, no file opened by path, no
 * exec, a memory cap) before a single byte of it is parsed, writes the index and exits. lord-server checks every line
 * again (fpakas_line). It links only the C library. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "fpakas.h"
#include "sec/sandbox.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#define XML_MAX (256L << 20)

static char outbuf[1 << 16];
static size_t outn;
static int flush_out(void) {
    size_t o = 0;
    while (o < outn) {
        ssize_t w = write(1, outbuf + o, outn - o);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        o += (size_t)w;
    }
    outn = 0;
    return 0;
}
static int out(void *ud, const char *line, size_t n) {
    (void)ud;
    if (outn + n > sizeof outbuf && flush_out()) return -1;
    memcpy(outbuf + outn, line, n);
    outn += n;
    return 0;
}

static int word_ok(const char *s, size_t max) {
    size_t n = strlen(s);
    if (!n || n > max) return 0;
    for (; *s; s++) if (!isalnum((unsigned char)*s) && *s != '_' && *s != '-') return 0;
    return 1;
}

int main(int argc, char **argv) {
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    prctl(PR_SET_DUMPABLE, 0);
    struct rlimit nocore = { 0, 0 };
    setrlimit(RLIMIT_CORE, &nocore);
    alarm(60);
    if (argc != 3 || !word_ok(argv[1], 16) || !word_ok(argv[2], 16)) return 2;
    struct stat st;
    if (fstat(0, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > XML_MAX) return 3;
    char *xml = malloc((size_t)st.st_size);
    size_t have = 0;
    while (xml && have < (size_t)st.st_size) {
        ssize_t r = read(0, xml + have, (size_t)st.st_size - have);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        have += (size_t)r;
    }
    if (!xml || !have) return 3;
    close(0);
    SandboxHelperCfg cfg = {
        .name = "lordos-appstream",
        .ro_paths = NULL,
        .extra_syscalls = (int[]){ -1 },
        .allow_ioctl_classes = 0,
        .mem_max_mb = 512,
        .ro_dir_fds = NULL,
        .rw_dir_fds = NULL,
    };
    sandbox_apply_helper(&cfg);
    int n = fpakas_index(xml, have, argv[1], argv[2], out, NULL);
    if (flush_out()) return 4;
    return n < 0 ? 5 : 0;
}
