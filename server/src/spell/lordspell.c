/* lordspell.c — lord-spell: the spell checker's sandboxed helper (docs/SPELLING.md; AGENTS.md "lord-spell").
 *
 * The only LordOS program that loads Hunspell: libhunspell-1.7.so.0, dlopen'd (RTLD_NOW | RTLD_LOCAL) from Debian's
 * fixed library folders only, after checking that the folder, its parents and the real file are root's and not group-
 * or other-writable. lord-server (spell/spellsvc.c) starts it from the fixed path next to itself, with no arguments and
 * an empty environment, and gives it:
 *   fd 0       the channel (a socketpair; spellproto.h's frames, both ways)
 *   fd 1, 2    /dev/null
 *   fd 3 + 2i  language i's .aff, fd 4 + 2i its .dic: files of /usr/share/hunspell lord-server opened read-only
 * The first frame ('L') names the languages. It reads each pair through /proc/self/fd, closes every descriptor but the
 * channel, and applies the helpers' sandbox (sandbox_apply_helper: no files, no network, no exec, no new sockets, a
 * syscall allowlist, a memory cap) before it answers; only then does it read a word. Words in, verdicts and at most
 * SPELL_SUGG_MAX suggestions out, each checked by spellproto.c on the way in and out. A malformed frame ends it. */
#include "spellproto.h"
#include "../sec/sandbox.h"

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#ifdef SPELL_PROBE
#include <fcntl.h>
#include <netinet/in.h>
#endif

#define DICT_MAX (64u << 20)            /* a .dic or .aff larger than this isn't a dictionary we load */

static const char *const LIB_DIRS[] = { "/usr/lib/x86_64-linux-gnu", "/usr/lib/aarch64-linux-gnu", "/usr/lib64", "/usr/lib", NULL };
#ifndef SPELL_LIB_NAME
#define SPELL_LIB_NAME "libhunspell-1.7.so.0"
#endif

typedef struct Hunhandle Hunhandle;
static struct {
    Hunhandle *(*create)(const char *aff, const char *dic);
    void (*destroy)(Hunhandle *);
    int (*spell)(Hunhandle *, const char *);
    int (*suggest)(Hunhandle *, char ***, const char *);
    void (*free_list)(Hunhandle *, char ***, int);
    char *(*encoding)(Hunhandle *);
} H;

static struct { Hunhandle *h; int latin1; const char *marks; } L[SPELL_LANGS_MAX];
static int nlang;

/* a folder of the system: root's, not writable by group or others, every parent too */
static int root_dir_ok(const char *dir) {
    char p[PATH_MAX];
    if (snprintf(p, sizeof p, "%s", dir) >= (int)sizeof p || p[0] != '/') return 0;
    for (;;) {
        struct stat st;
        if (lstat(p, &st) || !S_ISDIR(st.st_mode) || st.st_uid != 0 || (st.st_mode & 022)) return 0;
        char *sl = strrchr(p, '/');
        if (!sl) return 0;
        if (sl == p) { if (p[1]) { p[1] = 0; continue; } return 1; }
        *sl = 0;
    }
}

/* the library from the fixed folders only; its real file in one of them too, root's and not writable by others */
static void *load_hunspell(void) {
    for (int i = 0; LIB_DIRS[i]; i++) {
        if (!root_dir_ok(LIB_DIRS[i])) continue;
        char p[PATH_MAX], real[PATH_MAX], dir[PATH_MAX];
        snprintf(p, sizeof p, "%s/%s", LIB_DIRS[i], SPELL_LIB_NAME);
        if (!realpath(p, real)) continue;
        snprintf(dir, sizeof dir, "%s", real);
        char *sl = strrchr(dir, '/');
        if (!sl || sl == dir) continue;
        *sl = 0;
        int fixed = 0;
        for (int j = 0; LIB_DIRS[j]; j++) if (!strcmp(dir, LIB_DIRS[j])) fixed = 1;
        struct stat st;
        if (!fixed || !root_dir_ok(dir) || lstat(real, &st) || !S_ISREG(st.st_mode) || st.st_uid != 0 || (st.st_mode & 022)) continue;
        void *h = dlopen(real, RTLD_NOW | RTLD_LOCAL);
        if (h) return h;
    }
    return NULL;
}

static int read_all(int fd, void *b, size_t n) {
    for (size_t o = 0; o < n;) {
        ssize_t r = read(fd, (char *)b + o, n - o);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        o += (size_t)r;
    }
    return 0;
}
static int write_all(int fd, const void *b, size_t n) {
    for (size_t o = 0; o < n;) {
        ssize_t w = write(fd, (const char *)b + o, n - o);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        o += (size_t)w;
    }
    return 0;
}

static uint8_t inbuf[SPELL_HEAD + SPELL_PAYLOAD_MAX], outbuf[SPELL_HEAD + SPELL_PAYLOAD_MAX], pay[SPELL_PAYLOAD_MAX];

/* the next frame from lord-server: its operation and payload (in inbuf + SPELL_HEAD) */
static int next_frame(int *op, size_t *len) {
    if (read_all(0, inbuf, SPELL_HEAD) || spell_head(inbuf, op, len)) return -1;
    return *len ? read_all(0, inbuf + SPELL_HEAD, *len) : 0;
}
static int answer(int op, size_t len) {
    int n = spell_frame(outbuf, sizeof outbuf, op, pay, len);
    return n < 0 ? -1 : write_all(0, outbuf, (size_t)n);
}

/* UTF-8 ↔ ISO-8859-1, for the older dictionaries that are in Latin-1 (SET ISO8859-1); -1 when it doesn't fit */
static int to_latin1(const char *s, char *out, size_t cap) {
    size_t o = 0;
    for (const uint8_t *p = (const uint8_t *)s; *p; p++) {
        unsigned c = *p;
        if (c >= 0x80) {
            if ((c != 0xc2 && c != 0xc3) || (p[1] & 0xc0) != 0x80) return -1;
            c = ((c & 0x1f) << 6) | (p[1] & 0x3f);
            p++;
        }
        if (o + 1 >= cap) return -1;
        out[o++] = (char)c;
    }
    out[o] = 0;
    return 0;
}
static int from_latin1(const char *s, char *out, size_t cap) {
    size_t o = 0;
    for (const uint8_t *p = (const uint8_t *)s; *p; p++) {
        if (*p < 0x80) { if (o + 1 >= cap) return -1; out[o++] = (char)*p; continue; }
        if (o + 2 >= cap) return -1;
        out[o++] = (char)(0xc0 | (*p >> 6));
        out[o++] = (char)(0x80 | (*p & 0x3f));
    }
    out[o] = 0;
    return 0;
}

static int known(int i, const char *w) {
    char l1[SPELL_WORD_MAX + 1];
    if (L[i].latin1) return to_latin1(w, l1, sizeof l1) ? 0 : H.spell(L[i].h, l1) != 0;
    return H.spell(L[i].h, w) != 0;
}

/* The letters a language writes with a mark, each after the plain one it stands for (pairs of UTF-8: plain, marked; the
 * lowercase ones, then the capitals: a, a-acute, e, e-acute… for Spanish),
 * for the words typed without their accents. Hunspell's own suggestions find most of them (MAP), but not always: in
 * the 0.8.9.6.2 QA "cancion" got none in a session where "pelicula" got "película" (BUG-7). */
static const char *const MARKS[][2] = {
    { "es", "a" "\xc3\xa1" "e" "\xc3\xa9" "i" "\xc3\xad" "o" "\xc3\xb3" "u" "\xc3\xba" "n" "\xc3\xb1" "u" "\xc3\xbc"
      "A" "\xc3\x81" "E" "\xc3\x89" "I" "\xc3\x8d" "O" "\xc3\x93" "U" "\xc3\x9a" "N" "\xc3\x91" "U" "\xc3\x9c" },
    { "pt", "a" "\xc3\xa1" "a" "\xc3\xa2" "a" "\xc3\xa3" "a" "\xc3\xa0" "e" "\xc3\xa9" "e" "\xc3\xaa" "i" "\xc3\xad"
      "o" "\xc3\xb3" "o" "\xc3\xb4" "o" "\xc3\xb5" "u" "\xc3\xba" "c" "\xc3\xa7"
      "A" "\xc3\x81" "A" "\xc3\x82" "A" "\xc3\x83" "A" "\xc3\x80" "E" "\xc3\x89" "E" "\xc3\x8a" "I" "\xc3\x8d" "O"
      "\xc3\x93" "O" "\xc3\x94" "O" "\xc3\x95" "U" "\xc3\x9a" "C" "\xc3\x87" },
    { "fr", "e" "\xc3\xa9" "e" "\xc3\xa8" "e" "\xc3\xaa" "a" "\xc3\xa0" "a" "\xc3\xa2" "u" "\xc3\xb9" "u" "\xc3\xbb"
      "i" "\xc3\xae" "i" "\xc3\xaf" "o" "\xc3\xb4" "c" "\xc3\xa7" "E" "\xc3\x89" "E" "\xc3\x88" "E" "\xc3\x8a"
      "A" "\xc3\x80" "A" "\xc3\x82" "U" "\xc3\x99" "U" "\xc3\x9b" "I" "\xc3\x8e" "I" "\xc3\x8f" "O" "\xc3\x94" "C"
      "\xc3\x87" },
    { "it", "a" "\xc3\xa0" "e" "\xc3\xa8" "e" "\xc3\xa9" "i" "\xc3\xac" "o" "\xc3\xb2" "u" "\xc3\xb9"
      "A" "\xc3\x80" "E" "\xc3\x88" "E" "\xc3\x89" "I" "\xc3\x8c" "O" "\xc3\x92" "U" "\xc3\x99" },
    { "ca", "a" "\xc3\xa0" "e" "\xc3\xa8" "e" "\xc3\xa9" "i" "\xc3\xad" "o" "\xc3\xb2" "o" "\xc3\xb3" "u" "\xc3\xba"
      "i" "\xc3\xaf" "u" "\xc3\xbc" "c" "\xc3\xa7"
      "A" "\xc3\x80" "E" "\xc3\x88" "E" "\xc3\x89" "I" "\xc3\x8d" "O" "\xc3\x92" "O" "\xc3\x93" "U" "\xc3\x9a" "I"
      "\xc3\x8f" "U" "\xc3\x9c" "C" "\xc3\x87" },
    { "gl", "a" "\xc3\xa1" "e" "\xc3\xa9" "i" "\xc3\xad" "o" "\xc3\xb3" "u" "\xc3\xba" "n" "\xc3\xb1"
      "A" "\xc3\x81" "E" "\xc3\x89" "I" "\xc3\x8d" "O" "\xc3\x93" "U" "\xc3\x9a" "N" "\xc3\x91" },
    { "de", "a" "\xc3\xa4" "o" "\xc3\xb6" "u" "\xc3\xbc"
      "A" "\xc3\x84" "O" "\xc3\x96" "U" "\xc3\x9c" },
};
static const char *marks_of(const char *tag) {
    for (size_t i = 0; i < sizeof MARKS / sizeof MARKS[0]; i++)
        if (!strncmp(tag, MARKS[i][0], 2) && (tag[2] == 0 || tag[2] == '_')) return MARKS[i][1];
    return NULL;
}
static size_t u8len(const char *s) { unsigned c = (unsigned char)*s; return c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4; }
static int add_sugg(SpellWord *out, int n, const char *asked, const char *s) {
    size_t l = strlen(s);
    if (n >= SPELL_SUGG_MAX || !strcmp(s, asked) || !spell_sugg_ok(s, l)) return n;   /* the word itself is no suggestion */
    for (int m = 0; m < n; m++) if (!strcmp(out[m], s)) return n;
    memcpy(out[n], s, l + 1);
    return n + 1;
}
/* the word with one of its letters given its mark (each place, each mark), the ones the dictionary knows */
static int marked(int i, const char *w, SpellWord *out, int n) {
    const char *mk = L[i].marks;
    if (!mk) return n;
    size_t wl = strlen(w);
    for (size_t at = 0; at < wl && n < SPELL_SUGG_MAX; at += u8len(w + at)) {
        size_t cl = u8len(w + at);
        for (const char *p = mk; *p && n < SPELL_SUGG_MAX;) {
            size_t pl = u8len(p), ml = u8len(p + pl);
            if (pl == cl && !memcmp(p, w + at, cl) && wl - cl + ml <= SPELL_WORD_MAX) {
                char v[SPELL_WORD_MAX + 1];
                memcpy(v, w, at);
                memcpy(v + at, p + pl, ml);
                memcpy(v + at + ml, w + at + cl, wl - at - cl + 1);
                if (known(i, v)) n = add_sugg(out, n, w, v);
            }
            p += pl + ml;
        }
    }
    return n;
}

/* suggestions from each language of the mask in turn, each valid and new and not the word itself, at most
 * SPELL_SUGG_MAX; a language that suggests nothing gives the word with one accent it lacks, when it knows that */
static int suggest(unsigned mask, const char *w, SpellWord *out) {
    int n = 0;
    for (int i = 0; i < nlang && n < SPELL_SUGG_MAX; i++) {
        if (!(mask & (1u << i)) || !L[i].h) continue;
        char l1[SPELL_WORD_MAX + 1];
        const char *q = w;
        if (L[i].latin1) { if (to_latin1(w, l1, sizeof l1)) continue; q = l1; }
        char **list = NULL;
#ifdef SPELL_NOSUGGEST
        int k = 0, before = n;                  /* tests only: Hunspell suggests nothing (the fallback alone) */
        (void)q;
#else
        int k = H.suggest(L[i].h, &list, q), before = n;
#endif
        for (int j = 0; j < k && n < SPELL_SUGG_MAX; j++) {
            char u[SPELL_WORD_MAX * 2 + 1];
            const char *s = list[j];
            if (!s) continue;
            if (L[i].latin1) { if (from_latin1(s, u, sizeof u)) continue; s = u; }
            n = add_sugg(out, n, w, s);
        }
        if (list) H.free_list(L[i].h, &list, k);
        if (n == before) n = marked(i, w, out, n);
    }
    return n;
}

static int dict_fd_ok(int fd) {
    struct stat st;
    return !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && (unsigned long long)st.st_size <= DICT_MAX;
}

int main(int argc, char **argv) {
    (void)argv;
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    prctl(PR_SET_DUMPABLE, 0);
    struct rlimit nocore = { 0, 0 };
    setrlimit(RLIMIT_CORE, &nocore);
    if (argc != 1) return 2;
    alarm(30);                                    /* the dictionaries load once, at most this long */

    int op;
    size_t len;
    SpellTag tags[SPELL_LANGS_MAX];
    if (next_frame(&op, &len) || op != 'L' || spell_parse_load(inbuf + SPELL_HEAD, len, tags, &nlang)) return 3;

    uint8_t ok[SPELL_LANGS_MAX] = { 0 };
    void *lib = load_hunspell();
    if (lib) {
        *(void **)&H.create = dlsym(lib, "Hunspell_create");
        *(void **)&H.destroy = dlsym(lib, "Hunspell_destroy");
        *(void **)&H.spell = dlsym(lib, "Hunspell_spell");
        *(void **)&H.suggest = dlsym(lib, "Hunspell_suggest");
        *(void **)&H.free_list = dlsym(lib, "Hunspell_free_list");
        *(void **)&H.encoding = dlsym(lib, "Hunspell_get_dic_encoding");
        if (!H.create || !H.destroy || !H.spell || !H.suggest || !H.free_list || !H.encoding) lib = NULL;
    }
    for (int i = 0; lib && i < nlang; i++) {
        int aff = 3 + 2 * i, dic = 4 + 2 * i;
        if (!dict_fd_ok(aff) || !dict_fd_ok(dic)) continue;
        char pa[64], pd[64];
        snprintf(pa, sizeof pa, "/proc/self/fd/%d", aff);
        snprintf(pd, sizeof pd, "/proc/self/fd/%d", dic);
        Hunhandle *h = H.create(pa, pd);
        if (!h) continue;
        const char *enc = H.encoding(h);
        if (enc && (!strcmp(enc, "UTF-8") || !strcmp(enc, "utf-8"))) L[i].h = h;
        else if (enc && (!strcmp(enc, "ISO8859-1") || !strcmp(enc, "ISO-8859-1"))) { L[i].h = h; L[i].latin1 = 1; }
        else { H.destroy(h); continue; }       /* another 8-bit encoding: that language stays off */
        L[i].marks = marks_of(tags[i]);
        ok[i] = 1;
    }
    /* nothing but the channel from here on, and then the sandbox: no files, no network, no exec */
    syscall(SYS_close_range, 1, ~0u, 0);
    SandboxHelperCfg cfg = {
        .name = "lordos-spell",
        .ro_paths = NULL,
        .extra_syscalls = (int[]){ -1 },
        .allow_ioctl_classes = 0,
        .mem_max_mb = 512,
        .ro_dir_fds = NULL,
        .rw_dir_fds = NULL,
    };
    sandbox_apply_helper(&cfg);
    alarm(0);
#ifdef SPELL_PROBE
    /* tests only (build/lord-spell-probe): what the sandbox must refuse; any of it allowed and there is no answer */
    {
        int bad = 0, f = open("/etc/passwd", O_RDONLY);
        if (f >= 0) { bad = 1; close(f); }
        f = open("/usr/share/hunspell", O_RDONLY | O_DIRECTORY);
        if (f >= 0) { bad = 1; close(f); }
        f = open("/tmp/lord-spell-probe", O_WRONLY | O_CREAT, 0600);
        if (f >= 0) { bad = 1; close(f); unlink("/tmp/lord-spell-probe"); }
        f = socket(AF_INET, SOCK_STREAM, 0);
        if (f >= 0) { bad = 1; close(f); }
        f = socket(AF_UNIX, SOCK_STREAM, 0);
        if (f >= 0) { bad = 1; close(f); }
        char *const a[] = { "sh", "-c", "exit 0", NULL }, *const e[] = { NULL };
        if (execve("/bin/sh", a, e) == 0) bad = 1;
        if (bad) return 9;
    }
#endif

    int n = spell_build_status(pay, sizeof pay, ok, nlang);
    if (n < 0 || answer('R', (size_t)n)) return 4;
    static SpellWord words[SPELL_BATCH_MAX], sugg[SPELL_SUGG_MAX];
    for (;;) {
        if (next_frame(&op, &len)) return 0;                  /* lord-server closed the channel (or sent garbage) */
        unsigned mask;
        int k;
        if (op == 'C') {
            if (spell_parse_check(inbuf + SPELL_HEAD, len, &mask, words, &k)) return 3;
            uint8_t good[SPELL_BATCH_MAX];
            for (int i = 0; i < k; i++) {
                good[i] = 0;
                for (int j = 0; j < nlang && !good[i]; j++) if ((mask & (1u << j)) && L[j].h && known(j, words[i])) good[i] = 1;
            }
            n = spell_build_checked(pay, sizeof pay, good, k);
#ifdef SPELL_EVIL
            pay[0] = (uint8_t)(k + 1);                 /* tests only: an answer for one word more than was asked */
            pay[n++] = 1;
#endif
            if (n < 0 || answer('c', (size_t)n)) return 4;
        } else if (op == 'G') {
            if (spell_parse_suggest(inbuf + SPELL_HEAD, len, &mask, words[0])) return 3;
            k = suggest(mask, words[0], sugg);
            n = spell_build_suggestions(pay, sizeof pay, sugg, k);
            if (n < 0 || answer('g', (size_t)n)) return 4;
        } else {
            return 3;
        }
    }
}
