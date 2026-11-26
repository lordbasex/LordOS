/* spell_proto_test.c — build/spell-test (make test; docs/SPELLING.md "Tests"): lord-spell's frames and the person's
 * dictionary, then the service end to end with the real helper, Hunspell and small dictionaries made here.
 *
 *   ./build/spell-test build/lord-spell build/lord-spell-probe build/lord-spell-evil
 *
 * 1. Words and tags: what may be asked about and what may come back (control characters, spaces, punctuation, bad
 *    UTF-8, overlong forms, surrogates, too long).
 * 2. Every builder's frame parsed back; every parser fed truncated, extended and random payloads (ASan and UBSan watch).
 * 3. The person's dictionary file: comments, blank lines, duplicates, invalid words dropped; written and read back.
 * 4. The service with lord-spell: a UTF-8 and a Latin-1 dictionary, words checked in two languages, suggestions, words
 *    that are not words never sent, the person's dictionary written 0600; the sandbox refusing files, sockets and exec
 *    (lord-spell-probe answers only when all of it was refused); a helper whose answer is malformed is killed and the
 *    job fails. Without libhunspell the end-to-end part prints "skipped: needs libhunspell". */
#include "../src/spell/spellproto.h"
#include "../src/spell/spellsvc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static unsigned rng = 12345;
static unsigned rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

static void words(void) {
    CHECK(spell_word_ok("hello", 5));
    CHECK(spell_word_ok("can't", 5));
    CHECK(spell_word_ok("e-mail", 6));
    CHECK(spell_word_ok("cañón", strlen("cañón")));
    CHECK(spell_word_ok("Ελλάδα", strlen("Ελλάδα")));
    CHECK(spell_word_ok("d\xe2\x80\x99" "accord", 9));          /* U+2019 */
    CHECK(!spell_word_ok("", 0));
    CHECK(!spell_word_ok("two words", 9));
    CHECK(!spell_word_ok("tab\there", 8));
    CHECK(!spell_word_ok("new\nline", 8));
    CHECK(!spell_word_ok("a/b", 3));
    CHECK(!spell_word_ok("<b>", 3));
    CHECK(!spell_word_ok("x\"y", 3));
    CHECK(!spell_word_ok("\xc2\x85", 2));                        /* U+0085 (NEL) */
    CHECK(!spell_word_ok("\xc0\xaf", 2));                        /* overlong '/' */
    CHECK(!spell_word_ok("\xed\xa0\x80", 3));                    /* a surrogate */
    CHECK(!spell_word_ok("\xf4\x90\x80\x80", 4));                /* past U+10FFFF */
    CHECK(!spell_word_ok("caf\xe9", 4));                         /* Latin-1, not UTF-8 */
    CHECK(!spell_word_ok("a\0b", 3));
    char long_w[SPELL_WORD_MAX + 2];
    memset(long_w, 'a', sizeof long_w);
    CHECK(spell_word_ok(long_w, SPELL_WORD_MAX));
    CHECK(!spell_word_ok(long_w, SPELL_WORD_MAX + 1));
    CHECK(spell_sugg_ok("a lot", 5));
    CHECK(!spell_sugg_ok("a  lot", 6));
    CHECK(!spell_sugg_ok(" lot", 4));
    CHECK(!spell_sugg_ok("lot ", 4));
    CHECK(spell_tag_ok("en_US") && spell_tag_ok("es") && spell_tag_ok("fil_PH") && spell_tag_ok("pt_BR"));
    CHECK(!spell_tag_ok("EN_us") && !spell_tag_ok("e") && !spell_tag_ok("en_") && !spell_tag_ok("../en") && !spell_tag_ok("en_US.dic"));
    CHECK(!spell_tag_ok("en-US") && !spell_tag_ok("abcd") && !spell_tag_ok("en_USAX") && !spell_tag_ok(""));
}

static void fuzz_one(const uint8_t *p, size_t n) {
    SpellTag tags[SPELL_LANGS_MAX];
    SpellWord w[SPELL_BATCH_MAX], one;
    uint8_t v[SPELL_BATCH_MAX];
    unsigned mask;
    int k;
    if (!spell_parse_load(p, n, tags, &k)) { CHECK(k >= 1 && k <= SPELL_LANGS_MAX); for (int i = 0; i < k; i++) CHECK(spell_tag_ok(tags[i])); }
    if (!spell_parse_check(p, n, &mask, w, &k)) { CHECK(k >= 1 && k <= SPELL_BATCH_MAX && mask >= 1 && mask < 16); for (int i = 0; i < k; i++) CHECK(spell_word_ok(w[i], strlen(w[i]))); }
    if (!spell_parse_suggest(p, n, &mask, one)) CHECK(spell_word_ok(one, strlen(one)));
    if (!spell_parse_suggestions(p, n, w, &k)) { CHECK(k >= 0 && k <= SPELL_SUGG_MAX); for (int i = 0; i < k; i++) CHECK(spell_sugg_ok(w[i], strlen(w[i]))); }
    for (int m = 1; m <= SPELL_BATCH_MAX; m++) if (!spell_parse_checked(p, n, m, v)) for (int i = 0; i < m; i++) CHECK(v[i] <= 1);
    if (n >= SPELL_HEAD) { int op; size_t l; if (!spell_head(p, &op, &l)) CHECK(l <= SPELL_PAYLOAD_MAX); }
}

static void frames(void) {
    uint8_t buf[SPELL_PAYLOAD_MAX], fr[SPELL_HEAD + SPELL_PAYLOAD_MAX];
    SpellTag tags[2] = { "en_US", "es_ES" }, gt[SPELL_LANGS_MAX];
    int n = spell_build_load(buf, sizeof buf, tags, 2), k = 0;
    CHECK(n > 0 && !spell_parse_load(buf, (size_t)n, gt, &k) && k == 2 && !strcmp(gt[1], "es_ES"));
    CHECK(spell_parse_load(buf, (size_t)n - 1, gt, &k));                    /* short */
    buf[n] = 0;
    CHECK(spell_parse_load(buf, (size_t)n + 1, gt, &k));                    /* a byte too many */
    SpellTag bad[1] = { "../x" };
    CHECK(spell_build_load(buf, sizeof buf, bad, 1) < 0);

    SpellWord w[3] = { "hello", "wrld", "cañón" }, gw[SPELL_BATCH_MAX];
    unsigned mask;
    n = spell_build_check(buf, sizeof buf, 3, w, 3);
    CHECK(n > 0 && !spell_parse_check(buf, (size_t)n, &mask, gw, &k) && k == 3 && mask == 3 && !strcmp(gw[2], "cañón"));
    for (int cut = 0; cut < n; cut++) CHECK(spell_parse_check(buf, (size_t)cut, &mask, gw, &k));
    buf[3] = 0;                                                             /* a word of length 0 */
    CHECK(spell_parse_check(buf, (size_t)n, &mask, gw, &k));
    n = spell_build_check(buf, sizeof buf, 3, w, 3);
    buf[0] = 0;                                                             /* no language */
    CHECK(spell_parse_check(buf, (size_t)n, &mask, gw, &k));
    buf[0] = 16;                                                            /* a fifth language */
    CHECK(spell_parse_check(buf, (size_t)n, &mask, gw, &k));
    SpellWord spaced[1] = { "two words" };
    CHECK(spell_build_check(buf, sizeof buf, 1, spaced, 1) < 0);
    SpellWord many[SPELL_BATCH_MAX + 1];
    for (int i = 0; i <= SPELL_BATCH_MAX; i++) snprintf(many[i], sizeof many[i], "w%d", i);
    CHECK(spell_build_check(buf, sizeof buf, 1, many, SPELL_BATCH_MAX) > 0);
    CHECK(spell_build_check(buf, sizeof buf, 1, many, SPELL_BATCH_MAX + 1) < 0);

    uint8_t good[3] = { 1, 0, 1 }, gg[3];
    n = spell_build_checked(buf, sizeof buf, good, 3);
    CHECK(n == 4 && !spell_parse_checked(buf, 4, 3, gg) && gg[1] == 0 && gg[2] == 1);
    CHECK(spell_parse_checked(buf, 4, 2, gg));                              /* answers for another count */
    buf[2] = 2;
    CHECK(spell_parse_checked(buf, 4, 3, gg));                              /* not 0 or 1 */

    SpellWord sg[2] = { "world", "a lot" }, gs[SPELL_SUGG_MAX], one;
    n = spell_build_suggestions(buf, sizeof buf, sg, 2);
    CHECK(n > 0 && !spell_parse_suggestions(buf, (size_t)n, gs, &k) && k == 2 && !strcmp(gs[1], "a lot"));
    n = spell_build_suggestions(buf, sizeof buf, sg, 0);
    CHECK(n == 1 && !spell_parse_suggestions(buf, 1, gs, &k) && k == 0);
    buf[0] = SPELL_SUGG_MAX + 1;
    CHECK(spell_parse_suggestions(buf, 1, gs, &k));
    SpellWord evil[1] = { "x\x1b[2J" };
    CHECK(spell_build_suggestions(buf, sizeof buf, evil, 1) < 0);
    n = spell_build_suggest(buf, sizeof buf, 1, "wrld");
    CHECK(n > 0 && !spell_parse_suggest(buf, (size_t)n, &mask, one) && !strcmp(one, "wrld"));

    /* frames: magic, operation, length cap */
    n = spell_frame(fr, sizeof fr, 'C', buf, 10);
    int op;
    size_t l;
    CHECK(n == 14 && !spell_head(fr, &op, &l) && op == 'C' && l == 10);
    fr[0] = 'X';
    CHECK(spell_head(fr, &op, &l));
    fr[0] = 'S'; fr[1] = 'Z';
    CHECK(spell_head(fr, &op, &l));
    fr[1] = 'C'; fr[2] = 0x01; fr[3] = 0x21;                                /* 8449 > SPELL_PAYLOAD_MAX */
    CHECK(spell_head(fr, &op, &l));
    CHECK(spell_frame(fr, sizeof fr, 'C', buf, SPELL_PAYLOAD_MAX + 1) < 0);
    CHECK(spell_frame(fr, 8, 'C', buf, 10) < 0);

    /* random and mutated payloads: never a crash, and what parses is valid */
    static uint8_t fz[SPELL_PAYLOAD_MAX];
    for (int it = 0; it < 200000; it++) {
        size_t len = rnd() % 300;
        for (size_t i = 0; i < len; i++) fz[i] = (uint8_t)rnd();
        if (len && rnd() % 3 == 0) fz[0] = (uint8_t)(rnd() % 6);
        if (len > 1 && rnd() % 3 == 0) fz[1] = (uint8_t)(rnd() % 8);
        fuzz_one(fz, len);
    }
    n = spell_build_check(buf, sizeof buf, 3, w, 3);
    for (int it = 0; it < 200000; it++) {
        memcpy(fz, buf, (size_t)n);
        int flips = 1 + (int)(rnd() % 3);
        for (int f = 0; f < flips; f++) fz[rnd() % (unsigned)n] = (uint8_t)rnd();
        fuzz_one(fz, (size_t)n - (rnd() % 4 == 0 ? rnd() % (unsigned)n : 0));
    }
}

static void dictfile(void) {
    const char *text = "# my words\nLordOS\n\n  Lumo  \r\nLordOS\nbad word\nx\x1b\nñandú\n";
    SpellWord w[8];
    int n = spelldict_parse(text, strlen(text), w, 8);
    CHECK(n == 3 && !strcmp(w[0], "LordOS") && !strcmp(w[1], "Lumo") && !strcmp(w[2], "ñandú"));
    char out[256];
    CHECK(spelldict_format(w, n, out, sizeof out) > 0 && !strcmp(out, "LordOS\nLumo\nñandú\n"));
    CHECK(spelldict_format(w, n, out, 8) < 0);
    CHECK(spelldict_parse(text, strlen(text), w, 1) == 1);
}

/* ---- end to end ---- */

static char tmpdir[256];

static void put(const char *name, const char *text, size_t n) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", tmpdir, name);
    FILE *f = fopen(p, "wb");
    if (!f) { perror(p); exit(1); }
    fwrite(text, 1, n, f);
    fclose(f);
}

/* a job's result, waited for */
static char *wait_for(int id) {
    for (int i = 0; i < 1000 && id > 0; i++) {
        char *r = spell_take(id);
        if (r) return r;
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static int have_hunspell(void) {
    return !access("/usr/lib/x86_64-linux-gnu/libhunspell-1.7.so.0", F_OK) || !access("/usr/lib/aarch64-linux-gnu/libhunspell-1.7.so.0", F_OK);
}

static void end_to_end(const char *helper, const char *probe, const char *evil) {
    if (!have_hunspell()) { printf("spell end to end: skipped: needs libhunspell (libhunspell-1.7-0)\n"); return; }
    snprintf(tmpdir, sizeof tmpdir, "/tmp/spell-test-XXXXXX");
    if (!mkdtemp(tmpdir)) { perror("mkdtemp"); exit(1); }
    const char *aff = "SET UTF-8\nTRY esianrtolcdugmphbyfvkwz'\n";
    put("en_US.aff", aff, strlen(aff));
    { const char *d = "4\nhello\nworld\nwonder\nword\n"; put("en_US.dic", d, strlen(d)); }
    put("es_ES.aff", aff, strlen(aff));
    const char *esd = "3\nhola\nmundo\ncañón\n";
    put("es_ES.dic", esd, strlen(esd));
    const char *l1a = "SET ISO8859-1\nTRY esianrtolcdugmphbyfvkwz'\n";
    put("fr_FR.aff", l1a, strlen(l1a));
    { const char *d = "2\ncaf\xe9\nbonjour\n"; put("fr_FR.dic", d, strlen(d)); }
    put("xx_XX.dic", "1\norphan\n", strlen("1\norphan\n"));                                      /* no .aff: not installed */
    char words_dir[300], words_file[340];
    snprintf(words_dir, sizeof words_dir, "%s/cfg/enchant", tmpdir);
    spell_set_paths(tmpdir, helper, words_dir);

    SpellTag inst[8];
    int ni = spell_installed(inst, 8);
    CHECK(ni == 3 && !strcmp(inst[0], "en_US") && !strcmp(inst[1], "es_ES") && !strcmp(inst[2], "fr_FR"));

    /* no languages yet */
    const char *w1[] = { "hello" };
    char *r = wait_for(spell_submit_check(w1, 1));
    CHECK(r && strstr(r, "spell.nolang"));
    free(r);

    const char *langs[] = { "en_US", "es_ES", "../etc", "de_DE", "fr_FR" };
    CHECK(spell_set_langs(langs, 5) == 3);
    const char *ws[] = { "hello", "wrold", "mundo", "cañón", "two words", "café", "", NULL, "\x1b[31m" };
    r = wait_for(spell_submit_check(ws, 9));
    CHECK(r && !strcmp(r, "{\"ok\":true,\"good\":[1,0,1,1,1,1,1,1,1]}"));
    if (r) printf("check: %s\n", r);
    free(r);
    r = wait_for(spell_submit_suggest("wrold"));
    CHECK(r && strstr(r, "\"world\""));
    if (r) printf("suggest: %s\n", r);
    free(r);
    r = wait_for(spell_submit_suggest("cafe"));
    CHECK(r && strstr(r, "\"café\""));                                       /* from the Latin-1 dictionary, in UTF-8 */
    free(r);
    r = spell_state_json();
    CHECK(r && strstr(r, "\"state\":\"ready\"") && strstr(r, "\"loaded\":[1,1,1]"));
    free(r);

    /* spellang's defaults: the region first, then English */
    SpellTag def[2];
    CHECK(spell_defaults("es_AR.UTF-8", def) == 2 && !strcmp(def[0], "es_ES") && !strcmp(def[1], "en_US"));
    CHECK(spell_defaults("fr", def) == 2 && !strcmp(def[0], "fr_FR"));
    CHECK(spell_defaults("ja", def) == 1 && !strcmp(def[0], "en_US"));

    /* the person's dictionary (enchant's lists, one per language in use): created 0600, words valid only, kept once,
     * a word WebKit learned (already in one list) read too */
    {
        char d[340];
        snprintf(d, sizeof d, "mkdir -p '%s'", words_dir);
        if (system(d)) { /* checked below */ }
        snprintf(words_file, sizeof words_file, "%s/es_ES.dic", words_dir);
        FILE *f = fopen(words_file, "w");
        if (f) { fputs("\xef\xbb\xbfWebKitWord\n", f); fclose(f); }
    }
    CHECK(spell_add_word("LordOS") == 0 && spell_add_word("Lumo") == 0 && spell_add_word("LordOS") == 0);
    CHECK(spell_add_word("two words") < 0 && errno == EINVAL);
    CHECK(spell_add_word("x\n") < 0);
    r = spell_words_json();
    CHECK(r && !strcmp(r, "[\"LordOS\",\"Lumo\",\"WebKitWord\"]"));
    if (r) printf("words: %s\n", r);
    free(r);
    struct stat st;
    snprintf(words_file, sizeof words_file, "%s/fr_FR.dic", words_dir);
    CHECK(!stat(words_file, &st) && (st.st_mode & 0777) == 0600);
    CHECK(spell_remove_word("LordOS") == 0);
    r = spell_words_json();
    CHECK(r && !strcmp(r, "[\"Lumo\",\"WebKitWord\"]"));
    free(r);

    /* the sandbox: the probe answers only if files, sockets and exec were all refused */
    spell_set_paths(NULL, probe, NULL);
    const char *again[] = { "en_US", "es_ES" };
    spell_set_langs(again, 2);
    r = wait_for(spell_submit_check(w1, 1));
    CHECK(r && !strcmp(r, "{\"ok\":true,\"good\":[1]}"));
    if (!r || strcmp(r, "{\"ok\":true,\"good\":[1]}")) fprintf(stderr, "probe: %s\n", r ? r : "(nothing)");
    free(r);

    /* a malformed answer: the job fails and the helper is gone */
    spell_set_paths(NULL, evil, NULL);
    spell_set_langs(langs, 1);
    r = wait_for(spell_submit_check(w1, 1));
    CHECK(r && strstr(r, "spell.failed"));
    free(r);
    r = spell_state_json();
    CHECK(r && strstr(r, "\"state\":\"failed\""));
    free(r);
    spell_stop();

    /* the system's Spanish dictionary when it is installed (hunspell-es): a real one, through the real helper */
    if (!access("/usr/share/hunspell/es_ES.dic", R_OK)) {
        spell_set_paths(SPELL_DIR, helper, NULL);
        const char *es[] = { "es_ES" };
        CHECK(spell_set_langs(es, 1) == 1);
        const char *ew[] = { "canción", "cancion", "murciélago" };
        r = wait_for(spell_submit_check(ew, 3));
        CHECK(r && !strcmp(r, "{\"ok\":true,\"good\":[1,0,1]}"));
        free(r);
        r = wait_for(spell_submit_suggest("cancion"));
        CHECK(r && strstr(r, "\"canción\""));
        if (r) printf("es_ES suggest: %s\n", r);
        free(r);
        spell_stop();
    } else printf("es_ES: skipped: needs hunspell-es\n");

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmpdir);
    if (system(cmd)) { /* left behind in /tmp */ }
}

int main(int argc, char **argv) {
    words();
    frames();
    dictfile();
    if (argc >= 4) end_to_end(argv[1], argv[2], argv[3]);
    if (fails) { fprintf(stderr, "spell-test: %d failures\n", fails); return 1; }
    printf("spell-test: ok\n");
    return 0;
}
