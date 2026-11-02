/* ime.c — input methods for scripts that are not typed letter by letter. Without ibus, fcitx
 * or D-Bus: the base keyboard is Latin (us) and the text is composed here.
 *
 *   hangul  Korean, 2-set keyboard (dubeolsik, the Korean standard): jamo are combined into
 *           syllables (ㅎ+ㅏ+ㄴ → 한) with compound vowels and finals; Backspace undoes the last one.
 *   kana    Japanese: romaji → hiragana (Hepburn and kunrei: shi/si, tsu/tu, kya, っ for a double
 *           consonant, nn/n' → ん). Space converts with the SKK dictionary (skkdic package,
 *           EUC-JP, binary search over the mapped file) and always offers hiragana and
 *           katakana. It converts the whole reading: it does not split phrases like Mozc or Anthy.
 *   pinyin  Simplified Chinese: letters → candidates from the Rime dictionary
 *           (rime-data-pinyin-simp: 65,000 characters and phrases with frequency). If the whole
 *           phrase is not there, those covering the beginning are offered and the rest stays in
 *           composition. 1–9 choose, Space takes the first, - and = page.
 *
 * Dictionaries are only loaded when the method is activated and are also looked up in
 * ~/.local/share/axia/ime/. */
#include "ime.h"

#include <ctype.h>
#include <fcntl.h>
#include <iconv.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

enum { E_NONE, E_HANGUL, E_KANA, E_PINYIN };

static int engine, context, changed;
static ImeState st;
static char dict_path[512];

/* ---- UTF-8 utilities ---- */

static size_t put_cp(char *out, uint32_t cp) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xc0 | cp >> 6); out[1] = (char)(0x80 | (cp & 0x3f)); return 2; }
    if (cp < 0x10000) { out[0] = (char)(0xe0 | cp >> 12); out[1] = (char)(0x80 | (cp >> 6 & 0x3f)); out[2] = (char)(0x80 | (cp & 0x3f)); return 3; }
    out[0] = (char)(0xf0 | cp >> 18); out[1] = (char)(0x80 | (cp >> 12 & 0x3f)); out[2] = (char)(0x80 | (cp >> 6 & 0x3f)); out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

static void append(char *buf, size_t size, const char *s) {
    size_t n = strlen(buf), m = strlen(s);
    if (n + m + 1 > size) return;
    memcpy(buf + n, s, m + 1);
}

static void append_cp(char *buf, size_t size, uint32_t cp) {
    char tmp[5];
    tmp[put_cp(tmp, cp)] = 0;
    append(buf, size, tmp);
}

static void drop_last_char(char *s) {
    size_t n = strlen(s);
    while (n > 0) {
        n--;
        if (((unsigned char)s[n] & 0xc0) != 0x80) break;
    }
    s[n] = 0;
}

static int is_letter(const KeyInfo *k) { return k->chr[0] && !k->chr[1] && isalpha((unsigned char)k->chr[0]); }

static void set_preedit(const char *s) {
    size_t n = strlen(s);
    if (n >= sizeof st.preedit) n = sizeof st.preedit - 1;
    memmove(st.preedit, s, n);              /* s may be a candidate from st */
    st.preedit[n] = 0;
    changed = 1;
}

static void clear_cand(void) {
    if (st.ncand || st.converting) changed = 1;
    st.ncand = st.sel = st.converting = 0;
}

static int map_file(const char *path, const uint8_t **data, size_t *size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat sb;
    void *m = MAP_FAILED;
    if (!fstat(fd, &sb) && S_ISREG(sb.st_mode) && sb.st_size > 0 && sb.st_size < (64 << 20))
        m = mmap(NULL, (size_t)sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return -1;
    *data = m;
    *size = (size_t)sb.st_size;
    return 0;
}

/* User dictionary (~/.local/share/axia/ime/<name>) or the system one. */
static int find_dict(const char *const *names, int n, char *out, size_t size) {
    const char *home = getenv("HOME");
    for (int i = 0; i < n; i++) {
        if (home && names[i][0] != '/') {
            snprintf(out, size, "%s/.local/share/axia/ime/%s", home, names[i]);
            if (!access(out, R_OK)) return 0;
        }
        if (names[i][0] == '/') {
            snprintf(out, size, "%s", names[i]);
            if (!access(out, R_OK)) return 0;
        }
    }
    out[0] = 0;
    return -1;
}

/* ================= Korean ================= */

/* Compatibility jamo (U+3131…) for the 2-set keyboard key. */
static uint32_t hangul_jamo(char c) {
    switch (c) {
    case 'q': return 0x3142; case 'Q': return 0x3143; case 'w': return 0x3148; case 'W': return 0x3149;
    case 'e': return 0x3137; case 'E': return 0x3138; case 'r': return 0x3131; case 'R': return 0x3132;
    case 't': return 0x3145; case 'T': return 0x3146; case 'o': return 0x3150; case 'O': return 0x3152;
    case 'p': return 0x3154; case 'P': return 0x3156;
    default: break;
    }
    static const char keys[] = "yuiasdfghjklzxcvbnm";
    static const uint16_t jamo[] = { 0x315B, 0x3155, 0x3151, 0x3141, 0x3134, 0x3147, 0x3139, 0x314E, 0x3157, 0x3153,
                                     0x314F, 0x3163, 0x314B, 0x314C, 0x314A, 0x314D, 0x3160, 0x315C, 0x3161 };
    const char *p = strchr(keys, tolower((unsigned char)c));
    return p && c ? jamo[p - keys] : 0;
}

static const uint16_t cho[19] = { 0x3131, 0x3132, 0x3134, 0x3137, 0x3138, 0x3139, 0x3141, 0x3142, 0x3143, 0x3145,
                                  0x3146, 0x3147, 0x3148, 0x3149, 0x314A, 0x314B, 0x314C, 0x314D, 0x314E };
static const uint16_t jong[28] = { 0, 0x3131, 0x3132, 0x3133, 0x3134, 0x3135, 0x3136, 0x3137, 0x3139, 0x313A, 0x313B,
                                   0x313C, 0x313D, 0x313E, 0x313F, 0x3140, 0x3141, 0x3142, 0x3144, 0x3145, 0x3146,
                                   0x3147, 0x3148, 0x314A, 0x314B, 0x314C, 0x314D, 0x314E };
static const uint16_t vowel_pairs[][3] = { { 0x3157, 0x314F, 0x3158 }, { 0x3157, 0x3150, 0x3159 }, { 0x3157, 0x3163, 0x315A },
                                           { 0x315C, 0x3153, 0x315D }, { 0x315C, 0x3154, 0x315E }, { 0x315C, 0x3163, 0x315F },
                                           { 0x3161, 0x3163, 0x3162 } };
static const uint16_t final_pairs[][3] = { { 0x3131, 0x3145, 0x3133 }, { 0x3134, 0x3148, 0x3135 }, { 0x3134, 0x314E, 0x3136 },
                                           { 0x3139, 0x3131, 0x313A }, { 0x3139, 0x3141, 0x313B }, { 0x3139, 0x3142, 0x313C },
                                           { 0x3139, 0x3145, 0x313D }, { 0x3139, 0x314C, 0x313E }, { 0x3139, 0x314D, 0x313F },
                                           { 0x3139, 0x314E, 0x3140 }, { 0x3142, 0x3145, 0x3144 } };

static int is_vowel(uint32_t j) { return j >= 0x314F && j <= 0x3163; }
static int idx(const uint16_t *t, int n, uint32_t j) { for (int i = 0; i < n; i++) if (t[i] == j) return i; return -1; }
static uint32_t combine(const uint16_t (*t)[3], int n, uint32_t a, uint32_t b) {
    for (int i = 0; i < n; i++) if (t[i][0] == a && t[i][1] == b) return t[i][2];
    return 0;
}

typedef struct { uint32_t l, v, t; } Syl;
static Syl syl, hist[12];
static int nhist;

static void syl_text(const Syl *s, char *out, size_t size) {
    out[0] = 0;
    int li = idx(cho, 19, s->l), ti = s->t ? idx(jong, 28, s->t) : 0;
    if (s->l && s->v && li >= 0 && ti >= 0) append_cp(out, size, 0xAC00 + ((uint32_t)li * 21 + (s->v - 0x314F)) * 28 + (uint32_t)ti);
    else {
        if (s->l) append_cp(out, size, s->l);
        if (s->v) append_cp(out, size, s->v);
        if (s->t) append_cp(out, size, s->t);
    }
}

static void hangul_show(void) {
    char tmp[16];
    syl_text(&syl, tmp, sizeof tmp);
    set_preedit(tmp);
}

static void hangul_commit(char *out, size_t size) {
    char tmp[16];
    syl_text(&syl, tmp, sizeof tmp);
    append(out, size, tmp);
    memset(&syl, 0, sizeof syl);
    nhist = 0;
    set_preedit("");
}

static void hangul_push(void) {
    if (nhist == 12) { memmove(hist, hist + 1, sizeof hist[0] * 11); nhist--; }
    hist[nhist++] = syl;
}

static int hangul_key(const KeyInfo *k, char *out, size_t size) {
    int empty = !syl.l && !syl.v && !syl.t;
    if (!strcmp(k->key, "Backspace")) {
        if (empty) return 0;
        if (nhist > 0) syl = hist[--nhist]; else memset(&syl, 0, sizeof syl);
        hangul_show();
        return 1;
    }
    uint32_t j = is_letter(k) ? hangul_jamo(k->chr[0]) : 0;
    if (!j) {                                   /* space, digits, Enter…: ends the syllable */
        if (!empty) hangul_commit(out, size);
        return 0;
    }
    if (is_vowel(j)) {
        if (syl.t) {                            /* the final becomes the initial of the new syllable */
            uint32_t keep = 0, move = syl.t;
            for (size_t i = 0; i < sizeof final_pairs / sizeof *final_pairs; i++)
                if (final_pairs[i][2] == syl.t) { keep = final_pairs[i][0]; move = final_pairs[i][1]; }
            if (idx(cho, 19, move) >= 0) {
                syl.t = keep;
                hangul_commit(out, size);
                syl.l = move;
                hangul_push();
                syl.v = j;
                hangul_show();
                return 1;
            }
        }
        uint32_t cv = syl.v && !syl.t ? combine(vowel_pairs, 7, syl.v, j) : 0;
        if (cv) { hangul_push(); syl.v = cv; }
        else if (!syl.v && !syl.t) { hangul_push(); syl.v = j; }
        else { hangul_commit(out, size); syl.v = j; }
    } else {
        if (syl.l && syl.v && !syl.t && idx(jong, 28, j) > 0) { hangul_push(); syl.t = j; }
        else if (syl.t && combine(final_pairs, 11, syl.t, j)) { hangul_push(); syl.t = combine(final_pairs, 11, syl.t, j); }
        else if (!syl.l && !syl.v) { hangul_push(); syl.l = j; }
        else { hangul_commit(out, size); syl.l = j; }
    }
    hangul_show();
    return 1;
}

/* ================= Japanese ================= */

static const char *const romaji[][2] = {
    { "a", "あ" }, { "i", "い" }, { "u", "う" }, { "e", "え" }, { "o", "お" },
    { "ka", "か" }, { "ki", "き" }, { "ku", "く" }, { "ke", "け" }, { "ko", "こ" },
    { "sa", "さ" }, { "si", "し" }, { "shi", "し" }, { "su", "す" }, { "se", "せ" }, { "so", "そ" },
    { "ta", "た" }, { "ti", "ち" }, { "chi", "ち" }, { "tu", "つ" }, { "tsu", "つ" }, { "te", "て" }, { "to", "と" },
    { "na", "な" }, { "ni", "に" }, { "nu", "ぬ" }, { "ne", "ね" }, { "no", "の" },
    { "ha", "は" }, { "hi", "ひ" }, { "hu", "ふ" }, { "fu", "ふ" }, { "he", "へ" }, { "ho", "ほ" },
    { "ma", "ま" }, { "mi", "み" }, { "mu", "む" }, { "me", "め" }, { "mo", "も" },
    { "ya", "や" }, { "yu", "ゆ" }, { "yo", "よ" },
    { "ra", "ら" }, { "ri", "り" }, { "ru", "る" }, { "re", "れ" }, { "ro", "ろ" },
    { "wa", "わ" }, { "wi", "うぃ" }, { "we", "うぇ" }, { "wo", "を" }, { "nn", "ん" }, { "n'", "ん" },
    { "ga", "が" }, { "gi", "ぎ" }, { "gu", "ぐ" }, { "ge", "げ" }, { "go", "ご" },
    { "za", "ざ" }, { "zi", "じ" }, { "ji", "じ" }, { "zu", "ず" }, { "ze", "ぜ" }, { "zo", "ぞ" },
    { "da", "だ" }, { "di", "ぢ" }, { "du", "づ" }, { "de", "で" }, { "do", "ど" },
    { "ba", "ば" }, { "bi", "び" }, { "bu", "ぶ" }, { "be", "べ" }, { "bo", "ぼ" },
    { "pa", "ぱ" }, { "pi", "ぴ" }, { "pu", "ぷ" }, { "pe", "ぺ" }, { "po", "ぽ" },
    { "kya", "きゃ" }, { "kyu", "きゅ" }, { "kyo", "きょ" },
    { "sya", "しゃ" }, { "syu", "しゅ" }, { "syo", "しょ" }, { "sha", "しゃ" }, { "shu", "しゅ" }, { "sho", "しょ" }, { "she", "しぇ" },
    { "tya", "ちゃ" }, { "tyu", "ちゅ" }, { "tyo", "ちょ" }, { "cha", "ちゃ" }, { "chu", "ちゅ" }, { "cho", "ちょ" }, { "che", "ちぇ" },
    { "nya", "にゃ" }, { "nyu", "にゅ" }, { "nyo", "にょ" }, { "hya", "ひゃ" }, { "hyu", "ひゅ" }, { "hyo", "ひょ" },
    { "mya", "みゃ" }, { "myu", "みゅ" }, { "myo", "みょ" }, { "rya", "りゃ" }, { "ryu", "りゅ" }, { "ryo", "りょ" },
    { "gya", "ぎゃ" }, { "gyu", "ぎゅ" }, { "gyo", "ぎょ" },
    { "zya", "じゃ" }, { "zyu", "じゅ" }, { "zyo", "じょ" }, { "ja", "じゃ" }, { "ju", "じゅ" }, { "jo", "じょ" }, { "je", "じぇ" },
    { "jya", "じゃ" }, { "jyu", "じゅ" }, { "jyo", "じょ" }, { "dya", "ぢゃ" }, { "dyu", "ぢゅ" }, { "dyo", "ぢょ" },
    { "bya", "びゃ" }, { "byu", "びゅ" }, { "byo", "びょ" }, { "pya", "ぴゃ" }, { "pyu", "ぴゅ" }, { "pyo", "ぴょ" },
    { "fa", "ふぁ" }, { "fi", "ふぃ" }, { "fe", "ふぇ" }, { "fo", "ふぉ" }, { "thi", "てぃ" }, { "dhi", "でぃ" },
    { "va", "ゔぁ" }, { "vi", "ゔぃ" }, { "vu", "ゔ" }, { "ve", "ゔぇ" }, { "vo", "ゔぉ" },
    { "xa", "ぁ" }, { "xi", "ぃ" }, { "xu", "ぅ" }, { "xe", "ぇ" }, { "xo", "ぉ" },
    { "la", "ぁ" }, { "li", "ぃ" }, { "lu", "ぅ" }, { "le", "ぇ" }, { "lo", "ぉ" },
    { "xya", "ゃ" }, { "xyu", "ゅ" }, { "xyo", "ょ" }, { "lya", "ゃ" }, { "lyu", "ゅ" }, { "lyo", "ょ" },
    { "xtu", "っ" }, { "ltu", "っ" }, { "xtsu", "っ" }, { "xwa", "ゎ" },
    { "-", "ー" }, { ",", "、" }, { ".", "。" }, { "[", "「" }, { "]", "」" }, { "/", "・" }, { "?", "？" }, { "!", "！" },
};
#define NROMAJI (int)(sizeof romaji / sizeof *romaji)

static char kana[384], pend[8];

static int romaji_exact(const char *s) { for (int i = 0; i < NROMAJI; i++) if (!strcmp(romaji[i][0], s)) return i; return -1; }
static int romaji_prefix(const char *s) {
    size_t n = strlen(s);
    for (int i = 0; i < NROMAJI; i++) if (!strncmp(romaji[i][0], s, n) && strlen(romaji[i][0]) > n) return 1;
    return 0;
}

/* Resolves pending romaji into kana. final = nothing else is coming (n → ん). */
static void kana_flush(int final) {
    while (pend[0]) {
        size_t n = strlen(pend);
        if (n >= 2 && pend[0] == pend[1] && pend[0] != 'n' && isalpha((unsigned char)pend[0]) && !strchr("aeiou", pend[0])) {
            append(kana, sizeof kana, "っ");            /* kk, tt, ss… → っ + consonant */
            memmove(pend, pend + 1, n);
            continue;
        }
        if (pend[0] == 'n' && n >= 2 && !strchr("aeiouyn'", pend[1])) {
            append(kana, sizeof kana, "ん");            /* n before another consonant */
            memmove(pend, pend + 1, n);
            continue;
        }
        int e = romaji_exact(pend), more = romaji_prefix(pend);
        if (e >= 0 && !more) { append(kana, sizeof kana, romaji[e][1]); pend[0] = 0; return; }
        if (more && !final) return;                     /* may continue: "s" → "sh" → "shi" */
        if (e >= 0) { append(kana, sizeof kana, romaji[e][1]); pend[0] = 0; return; }
        if (pend[0] == 'n') append(kana, sizeof kana, "ん");
        else { char one[2] = { pend[0], 0 }; append(kana, sizeof kana, one); }   /* forms nothing */
        memmove(pend, pend + 1, n);
    }
}

static void kana_show(void) {
    char tmp[sizeof kana + sizeof pend];
    snprintf(tmp, sizeof tmp, "%s%s", kana, pend);
    set_preedit(tmp);
}

static void kana_clear(void) { kana[0] = pend[0] = 0; clear_cand(); set_preedit(""); }

/* hiragana → katakana (U+3041…U+3096 → +0x60) */
static void to_katakana(const char *in, char *out, size_t size) {
    out[0] = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p;) {
        uint32_t c = *p;
        int n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        if (n == 3) c = (uint32_t)(p[0] & 0x0f) << 12 | (uint32_t)(p[1] & 0x3f) << 6 | (p[2] & 0x3f);
        else if (n == 2) c = (uint32_t)(p[0] & 0x1f) << 6 | (p[1] & 0x3f);
        else if (n == 4) c = (uint32_t)(p[0] & 0x07) << 18 | (uint32_t)(p[1] & 0x3f) << 12 | (uint32_t)(p[2] & 0x3f) << 6 | (p[3] & 0x3f);
        if (c >= 0x3041 && c <= 0x3096) c += 0x60;
        append_cp(out, size, c);
        p += n;
    }
}

/* --- SKK dictionary (EUC-JP): okuri-ari entries (descending order) and okuri-nasi (ascending) --- */
static const uint8_t *skk;
static size_t skk_size;
static uint32_t *skk_lines;
static int skk_n, skk_ari, skk_tried;      /* [0, skk_ari): okuri-ari; [skk_ari, skk_n): okuri-nasi */

static void skk_load(void) {
    if (skk_tried) return;
    skk_tried = 1;
    static const char *const names[] = { "SKK-JISYO.L", "SKK-JISYO", "/usr/share/skk/SKK-JISYO.L", "/usr/share/skk/SKK-JISYO" };
    char path[512];
    if (find_dict(names, 4, path, sizeof path) || map_file(path, &skk, &skk_size)) { skk = NULL; return; }
    int cap = 1 << 18, section = -1;
    skk_lines = malloc(sizeof *skk_lines * (size_t)cap);
    if (!skk_lines) return;
    for (size_t i = 0; i < skk_size;) {
        size_t e = i;
        while (e < skk_size && skk[e] != '\n') e++;
        if (e - i >= 20 && !memcmp(skk + i, ";; okuri-ari entries.", 21)) section = 0;
        else if (e - i >= 21 && !memcmp(skk + i, ";; okuri-nasi entries.", 22)) { section = 1; skk_ari = skk_n; }
        else if (section >= 0 && e > i && skk[i] != ';' && skk_n < cap) skk_lines[skk_n++] = (uint32_t)i;
        i = e + 1;
    }
    if (section < 1) skk_ari = 0;
    snprintf(dict_path, sizeof dict_path, "%s", path);
}

static int conv(const char *to, const char *from, const char *in, size_t inlen, char *out, size_t size) {
    iconv_t cd = iconv_open(to, from);
    if (cd == (iconv_t)-1) return -1;
    char *ip = (char *)in, *op = out;
    size_t il = inlen, ol = size - 1;
    size_t r = iconv(cd, &ip, &il, &op, &ol);
    iconv_close(cd);
    *op = 0;
    return r == (size_t)-1 ? -1 : 0;
}

static void add_to(char (*list)[96], int *n, int max, const char *s) {
    if (!*s || *n >= max) return;
    for (int i = 0; i < *n; i++) if (!strcmp(list[i], s)) return;
    snprintf(list[(*n)++], 96, "%s", s);
}

static void add_cand(const char *s) { add_to(st.cand, &st.ncand, IME_MAX_CAND, s); }

/* Looks up the reading (UTF-8; okuri = the table with okurigana, "かk"). Returns the line or NULL. */
static const uint8_t *skk_line(const char *reading, int okuri, size_t *keylen) {
    skk_load();
    if (!skk || !skk_n) return NULL;
    char key[256];
    if (conv("EUC-JP", "UTF-8", reading, strlen(reading), key, sizeof key)) return NULL;
    size_t kl = strlen(key);
    int lo = okuri ? 0 : skk_ari, hi = okuri ? skk_ari - 1 : skk_n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const uint8_t *l = skk + skk_lines[mid];
        size_t avail = skk_size - skk_lines[mid], ll = 0;
        while (ll < avail && l[ll] != ' ' && l[ll] != '\n') ll++;
        int c = memcmp(l, key, ll < kl ? ll : kl);
        if (!c) c = ll < kl ? -1 : ll > kl ? 1 : 0;
        if (!c) { *keylen = kl; return l; }
        if (okuri) c = -c;                      /* okuri-ari is in descending order */
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

/* Candidates from a line " /候補;注釈/…/" (+ suffix, for the okurigana). */
static int skk_cands(const uint8_t *l, size_t kl, const char *suffix, char (*list)[96], int *n, int max) {
    const uint8_t *end = memchr(l, '\n', skk_size - (size_t)(l - skk));
    if (!end) end = skk + skk_size;
    l += kl;
    const uint8_t *p = memchr(l, '/', (size_t)(end - l));
    int before = *n;
    while (p && p < end && *n < max) {
        const uint8_t *q = memchr(p + 1, '/', (size_t)(end - p - 1));
        if (!q) break;
        const uint8_t *semi = memchr(p + 1, ';', (size_t)(q - p - 1));
        const uint8_t *ce = semi ? semi : q;
        if (ce > p + 1 && p[1] != '(' && ce - p - 1 < 80) {
            char utf[96];
            if (!conv("UTF-8", "EUC-JP", (const char *)p + 1, (size_t)(ce - p - 1), utf, sizeof utf)) {
                append(utf, sizeof utf, suffix);
                add_to(list, n, max, utf);
            }
        }
        p = q;
    }
    return *n - before;
}

/* --- phrase conversion: the reading is split into words --- */

#define MAX_SEG 24
#define SEG_CAND 24
typedef struct { int start, len, okuri, sel, ncand; char cand[SEG_CAND][96]; } Seg;
static Seg segs[MAX_SEG];
static int nseg, fseg;
static uint32_t kcp[160];
static int nk;

static void kana_sub(int i, int n, char *out, size_t size) {
    out[0] = 0;
    for (int k = i; k < i + n && k < nk; k++) append_cp(out, size, kcp[k]);
}

/* Consonant SKK uses to write the okurigana: か→k, し→s, ち/つ/っ→t, あ→a… */
static char okuri_letter(uint32_t cp) {
    if (cp == 0x3063) return 't';
    if (cp == 0x3093) return 0;                 /* ん does not start an okurigana */
    char s[5];
    s[put_cp(s, cp)] = 0;
    for (int i = 0; i < NROMAJI; i++) if (!strcmp(romaji[i][1], s) && isalpha((unsigned char)romaji[i][0][0])) return romaji[i][0][0];
    return 0;
}

static int is_particle(uint32_t cp) {
    static const uint16_t p[] = { 0x306F, 0x304C, 0x3092, 0x306B, 0x3067, 0x3068, 0x306E, 0x3078, 0x3082, 0x3084, 0x306D, 0x3088 };   /* は が を に で と の へ も や ね よ */
    for (size_t i = 0; i < sizeof p / sizeof *p; i++) if (p[i] == cp) return 1;
    return 0;
}

/* Endings, auxiliaries and particles that stay in hiragana (without this, "います" would be
 * "converted" to 今す). */
static const char *const function_words[] = {
    "です", "でした", "ですね", "ですか", "ます", "ました", "ません", "ませんでした", "ましょう", "でしょう",
    "だ", "だった", "である", "ない", "なかった", "たい", "たかった", "ている", "ています", "ていました",
    "ていた", "てください", "ください", "います", "いました", "から", "まで", "より", "けど", "けれど",
    "ので", "のに", "には", "では", "とは", "への", "での", "との", "からの", "する", "します", "しました",
    "して", "した", "しない", "される", "させる", "られる", "れる", "こと", "もの", "という", "など", "よね",
    "いい", "よい", "ある", "あった", "いる", "いた", "なる", "なった", "くる", "きた", "その", "この", "あの",
    "どの", "もう", "まだ", "それ", "これ", "あれ", "どれ", "ここ", "そこ", "どこ", "とても", "ちょっと", "みんな",
    "そう", "よく", "すごく", "そして", "でも", "しかし", "また", "さん", "くん", "ちゃん", "さま",
    "しています", "していました", "している", "していた", "いません", "いませんでした", "ありません", "ありませんでした", "あります", "ありました",
};
enum { W_PLAIN = 2 };

static int is_function_word(const char *r) {
    for (size_t k = 0; k < sizeof function_words / sizeof *function_words; k++) if (!strcmp(function_words[k], r)) return 1;
    return 0;
}

/* Cost of the word [i, i+n): dictionary or ending 10 (single kana 14), with okurigana 10,
 * particle 7, lone kana 15, minus the square of the length (with the same number of words the
 * partition with longer words wins); -1 if it is not a word. kind: 1 okurigana, W_PLAIN
 * ending that stays in hiragana. */
static int word_cost_(int i, int n, int *kind);
static int word_cost(int i, int n, int *kind) {
    int c = word_cost_(i, n, kind);
    if (c >= 0 && n >= 2 && kcp[i + n - 1] == 0x3092) c += 1200;     /* を almost never ends a word */
    return c;
}

static int word_cost_(int i, int n, int *kind) {
    char r[256];
    size_t kl;
    *kind = 0;
    kana_sub(i, n, r, sizeof r);
    int sq = n * n;
    if (is_function_word(r)) { *kind = W_PLAIN; return 1000 - sq; }
    if (n == 1 && is_particle(kcp[i])) return 700 - sq;          /* particle: cheaper than a word */
    /* with okurigana (書く, 行き): preferred before ます/ました/ません ("いきます" → 行きます) */
    int okuri = 0;
    if (n >= 2) {
        char stem[256];
        kana_sub(i, n - 1, stem, sizeof stem);
        char l = okuri_letter(kcp[i + n - 1]);
        size_t sl = strlen(stem);
        if (l && sl + 2 < sizeof stem) { stem[sl] = l; stem[sl + 1] = 0; okuri = skk_line(stem, 1, &kl) != NULL; }
    }
    uint32_t n1 = i + n < nk ? kcp[i + n] : 0, n2 = i + n + 1 < nk ? kcp[i + n + 1] : 0;
    int verb_follows = n1 == 0x307E && (n2 == 0x3059 || n2 == 0x3057 || n2 == 0x305B);     /* ます まし ませ */
    /* mid-phrase, a word that starts with a particle and whose rest is a verb or an
     * ending ("はいい" = は + いい, "へいき" = へ + 行き) is usually particle + word;
     * "にほん" (日本: the rest is a noun) is not */
    int starts_particle = 0, tie = 0;
    if (i > 0 && n >= 2 && n <= 4 && is_particle(kcp[i])) {
        char rem[256], rk[256];
        kana_sub(i + 1, n - 1, rem, sizeof rem);
        starts_particle = is_function_word(rem);
        tie = 5;                                /* at equal cost, particle + word wins */
        uint32_t p0 = kcp[i];                   /* を へ に で が + verb (は, と, の… start many words) */
        int verbish = p0 == 0x3092 || p0 == 0x3078 || p0 == 0x306B || p0 == 0x3067 || p0 == 0x304C;
        if (!starts_particle && verbish && n >= 3) {
            kana_sub(i + 1, n - 2, rk, sizeof rk);
            char l = okuri_letter(kcp[i + n - 1]);
            size_t rl = strlen(rk);
            if (l && rl + 2 < sizeof rk) { rk[rl] = l; rk[rl + 1] = 0; starts_particle = skk_line(rk, 1, &kl) != NULL; }
        }
    }
    int pen = starts_particle ? 1100 : tie;
    if (okuri && verb_follows) { *kind = 1; return 1000 - sq - 30 + pen; }
    /* ichidan verbs before ます: "み" + ます → 見ます (in SKK: みr /見/) */
    if (verb_follows && !okuri) {
        char ich[256];
        kana_sub(i, n, ich, sizeof ich);
        size_t il = strlen(ich);
        if (il + 2 < sizeof ich) { ich[il] = 'r'; ich[il + 1] = 0; if (skk_line(ich, 1, &kl)) { *kind = 3; return 1000 - sq - 30 + pen; } }
    }
    if (skk_line(r, 0, &kl)) {
        int c = (n == 1 ? 1400 : 1000) - sq + pen;
        /* "きょうは" (教派) loses against きょう + は: word ending in は or を (not with が:
         * 映画, 漫画) */
        if (n >= 3 && kcp[i + n - 1] == 0x306F) {
            char pre[256];
            kana_sub(i, n - 1, pre, sizeof pre);
            if (skk_line(pre, 0, &kl)) c += 1200;
        }
        return c;
    }
    if (okuri) { *kind = 1; return 1000 - sq + pen; }
    return n == 1 ? 1500 - sq : -1;
}

/* Dynamic programming: lowest total cost of [from, nk) split into words. */
static void segment_from(int from) {
    int best[161], prev[161], ok[161];
    for (int i = from; i <= nk; i++) best[i] = 1 << 28;
    best[from] = 0;
    for (int i = from; i < nk; i++) {
        if (best[i] >= 1 << 28) continue;
        for (int n = 1; n <= 10 && i + n <= nk; n++) {
            int okr, c = word_cost(i, n, &okr);
            if (c < 0) continue;
            /* at equal cost the one that leaves the previous word longer wins (やまだ|さん) */
            if (best[i] + c < best[i + n] || (best[i] + c == best[i + n] && i > prev[i + n])) {
                best[i + n] = best[i] + c;
                prev[i + n] = i;
                ok[i + n] = okr;
            }
        }
    }
    int stack[161], ns = 0;
    for (int j = nk; j > from && ns < 160; j = prev[j]) stack[ns++] = j;
    for (int start = from; ns-- > 0 && nseg < MAX_SEG;) {
        int j = stack[ns];
        Seg *sg = &segs[nseg++];
        sg->start = start;
        sg->len = j - start;
        sg->okuri = ok[j];
        sg->sel = sg->ncand = 0;
        start = j;
    }
    if (nseg == MAX_SEG && segs[nseg - 1].start + segs[nseg - 1].len < nk) segs[nseg - 1].len = nk - segs[nseg - 1].start;
}

/* Candidates: endings and particles stay first in hiragana; the rest, with the
 * dictionary's first candidate. */
static void seg_cands(Seg *s) {
    char r[384], kata[768];
    size_t kl;
    kana_sub(s->start, s->len, r, sizeof r);
    to_katakana(r, kata, sizeof kata);
    s->ncand = 0;
    int plain_first = s->okuri == W_PLAIN || (s->len == 1 && is_particle(kcp[s->start]));
    if (plain_first) add_to(s->cand, &s->ncand, SEG_CAND, r);
    const uint8_t *l = s->okuri == 1 || s->okuri == 3 ? NULL : skk_line(r, 0, &kl);
    if (l) skk_cands(l, kl, "", s->cand, &s->ncand, SEG_CAND - 2);
    if (s->okuri == 3) {                        /* ichidan: candidates of "<reading>r" without suffix */
        char key[384];
        kana_sub(s->start, s->len, key, sizeof key);
        size_t kl2 = strlen(key);
        key[kl2] = 'r';
        key[kl2 + 1] = 0;
        if ((l = skk_line(key, 1, &kl))) skk_cands(l, kl, "", s->cand, &s->ncand, SEG_CAND - 2);
    }
    if (s->okuri == 1) {
        char stem[384], suf[8];
        kana_sub(s->start, s->len - 1, stem, sizeof stem);
        kana_sub(s->start + s->len - 1, 1, suf, sizeof suf);
        size_t sl = strlen(stem);
        stem[sl] = okuri_letter(kcp[s->start + s->len - 1]);
        stem[sl + 1] = 0;
        if ((l = skk_line(stem, 1, &kl))) skk_cands(l, kl, suf, s->cand, &s->ncand, SEG_CAND - 2);
    }
    add_to(s->cand, &s->ncand, SEG_CAND, r);
    add_to(s->cand, &s->ncand, SEG_CAND, kata);
}

static void conv_show(void) {
#ifdef IME_DEBUG
    for (int i = 0; i < nseg; i++) { char r[256]; int k; kana_sub(segs[i].start, segs[i].len, r, sizeof r); fprintf(stderr, "[%s %d] ", r, word_cost(segs[i].start, segs[i].len, &k)); }
    fprintf(stderr, "\n");
#endif
    char buf[sizeof st.preedit];
    buf[0] = 0;
    st.focus_begin = st.focus_end = 0;
    for (int i = 0; i < nseg; i++) {
        if (i == fseg) st.focus_begin = (int)strlen(buf);
        append(buf, sizeof buf, segs[i].cand[segs[i].sel]);
        if (i == fseg) st.focus_end = (int)strlen(buf);
    }
    set_preedit(buf);
    Seg *f = &segs[fseg];
    st.ncand = f->ncand;
    for (int i = 0; i < f->ncand; i++) memcpy(st.cand[i], f->cand[i], sizeof st.cand[i]);
    st.sel = f->sel;
    st.converting = 1;
    changed = 1;
}

static void kana_convert(void) {
    kana_flush(1);
    if (!kana[0]) return;
    clear_cand();
    nk = 0;
    for (const unsigned char *p = (const unsigned char *)kana; *p && nk < 160;) {
        uint32_t c = *p;
        int n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        if (n == 3) c = (uint32_t)(p[0] & 0x0f) << 12 | (uint32_t)(p[1] & 0x3f) << 6 | (p[2] & 0x3f);
        else if (n == 2) c = (uint32_t)(p[0] & 0x1f) << 6 | (p[1] & 0x3f);
        kcp[nk++] = c;
        p += n;
    }
    nseg = fseg = 0;
    segment_from(0);
    for (int i = 0; i < nseg; i++) seg_cands(&segs[i]);
    conv_show();
}

/* Shift+→/←: the focused segment takes or gives back a kana; what follows is split again. */
static void conv_resize(int delta) {
    Seg *f = &segs[fseg];
    int len = f->len + delta;
    if (len < 1 || f->start + len > nk) return;
    f->len = len;
    f->okuri = 0;
    int w;
    word_cost(f->start, len, &w);
    f->okuri = w;
    f->sel = 0;
    nseg = fseg + 1;
    if (f->start + len < nk) segment_from(f->start + len);
    for (int i = fseg; i < nseg; i++) seg_cands(&segs[i]);
    conv_show();
}

static void kana_commit(char *out, size_t size) {
    if (st.converting && nseg) for (int i = 0; i < nseg; i++) append(out, size, segs[i].cand[segs[i].sel]);
    else { kana_flush(1); append(out, size, kana); }
    nseg = 0;
    st.focus_begin = st.focus_end = 0;
    kana_clear();
}

static int digit_pick(const KeyInfo *k) {
    if (k->chr[0] >= '1' && k->chr[0] <= '9' && !k->chr[1]) {
        int i = (st.sel / IME_PAGE) * IME_PAGE + (k->chr[0] - '1');
        return i < st.ncand ? i : -1;
    }
    return -1;
}

static int kana_key(const KeyInfo *k, char *out, size_t size) {
    int empty = !kana[0] && !pend[0];
    if (st.converting) {
        Seg *f = &segs[fseg];
        int d = digit_pick(k);
        if (d >= 0) {                           /* chooses and moves to the next segment */
            f->sel = d;
            if (fseg + 1 < nseg) { fseg++; conv_show(); } else kana_commit(out, size);
            return 1;
        }
        if (!strcmp(k->key, " ") || !strcmp(k->key, "ArrowDown")) { f->sel = (f->sel + 1) % f->ncand; conv_show(); return 1; }
        if (!strcmp(k->key, "ArrowUp")) { f->sel = (f->sel + f->ncand - 1) % f->ncand; conv_show(); return 1; }
        if (!strcmp(k->key, "ArrowRight")) { if (k->shift) conv_resize(1); else if (fseg + 1 < nseg) { fseg++; conv_show(); } return 1; }
        if (!strcmp(k->key, "ArrowLeft")) { if (k->shift) conv_resize(-1); else if (fseg > 0) { fseg--; conv_show(); } return 1; }
        if (!strcmp(k->key, "Enter")) { kana_commit(out, size); return 1; }
        if (!strcmp(k->key, "Escape") || !strcmp(k->key, "Backspace")) {
            clear_cand(); nseg = 0; st.focus_begin = st.focus_end = 0; kana_show(); return 1;
        }
        kana_commit(out, size);                 /* typing on confirms the conversion */
        empty = 1;
    }
    if (!strcmp(k->key, "Backspace")) {
        if (empty) return 0;
        if (pend[0]) pend[strlen(pend) - 1] = 0; else drop_last_char(kana);
        kana_show();
        return 1;
    }
    if (!strcmp(k->key, "Enter")) { if (empty) return 0; kana_commit(out, size); return 1; }
    if (!strcmp(k->key, "Escape")) { if (empty) return 0; kana_clear(); return 1; }
    if (!strcmp(k->key, " ")) { if (empty) return 0; kana_convert(); return 1; }
    char c = k->chr[0] && !k->chr[1] ? k->chr[0] : 0;
    int roman = c && (isalpha((unsigned char)c) || c == '\'' || (strchr("-,.[]/?!", c) && c));
    if (!roman) {                               /* digits, arrows, Tab…: confirms and passes through */
        if (!empty) kana_commit(out, size);
        return 0;
    }
    size_t n = strlen(pend);
    if (n + 1 < sizeof pend) { pend[n] = (char)tolower((unsigned char)c); pend[n + 1] = 0; }
    kana_flush(0);
    kana_show();
    return 1;
}

/* ================= Chinese (pinyin) ================= */

typedef struct { uint32_t text, key, weight, pin; uint16_t tlen, klen, plen; } PyEntry;
/* Abbreviations: initials of each syllable ("zg" → zhong guo) of phrases of 2 or more. */
typedef struct { char ini[12]; int ent; } PyAbbr;
static PyAbbr *py_abbr;
static int py_nabbr;
static const uint8_t *py;
static size_t py_size;
static PyEntry *py_ent;
static char *py_keys;
static int py_n, py_tried;
static char raw[64], acc[256];
static int cand_len[IME_MAX_CAND];

static int py_cmp(const void *a, const void *b) {
    const PyEntry *x = a, *y = b;
    int c = memcmp(py_keys + x->key, py_keys + y->key, x->klen < y->klen ? x->klen : y->klen);
    if (c) return c;
    if (x->klen != y->klen) return x->klen < y->klen ? -1 : 1;
    return x->weight > y->weight ? -1 : x->weight < y->weight ? 1 : 0;
}

static int abbr_cmp(const void *a, const void *b) {
    const PyAbbr *x = a, *y = b;
    int c = strcmp(x->ini, y->ini);
    if (c) return c;
    uint32_t wx = py_ent[x->ent].weight, wy = py_ent[y->ent].weight;
    return wx > wy ? -1 : wx < wy ? 1 : 0;
}

static void py_load(void) {
    if (py_tried) return;
    py_tried = 1;
    static const char *const names[] = { "pinyin_simp.dict.yaml", "/usr/share/rime-data/pinyin_simp.dict.yaml" };
    char path[512];
    if (find_dict(names, 2, path, sizeof path) || map_file(path, &py, &py_size)) { py = NULL; return; }
    size_t i = 0;
    const char *mark = "\n...";
    for (; i + 4 < py_size; i++) if (!memcmp(py + i, mark, 4)) break;
    int cap = 1 << 17;
    py_ent = malloc(sizeof *py_ent * (size_t)cap);
    py_keys = malloc(1 << 20);
    size_t kused = 0;
    if (!py_ent || !py_keys) return;
    while (i < py_size) {
        size_t e = i;
        while (e < py_size && py[e] != '\n') e++;
        const uint8_t *l = py + i;
        const uint8_t *t1 = memchr(l, '\t', e - i);
        if (t1 && l[0] != '#' && py_n < cap) {
            const uint8_t *t2 = memchr(t1 + 1, '\t', (size_t)(py + e - t1 - 1));
            const uint8_t *ke = t2 ? t2 : py + e;
            size_t start = kused;
            for (const uint8_t *p = t1 + 1; p < ke && kused < (1 << 20) - 1; p++) if (*p != ' ') py_keys[kused++] = (char)*p;
            PyEntry *en = &py_ent[py_n];
            en->text = (uint32_t)i;
            en->tlen = (uint16_t)(t1 - l);
            en->key = (uint32_t)start;
            en->klen = (uint16_t)(kused - start);
            en->weight = t2 ? (uint32_t)strtoul((const char *)t2 + 1, NULL, 10) : 0;
            en->pin = (uint32_t)(t1 + 1 - py);
            en->plen = (uint16_t)(ke - t1 - 1);
            if (en->klen && en->tlen < 90) py_n++;
        }
        i = e + 1;
    }
    qsort(py_ent, (size_t)py_n, sizeof *py_ent, py_cmp);
    py_abbr = malloc(sizeof *py_abbr * (size_t)py_n);
    for (int k = 0; py_abbr && k < py_n; k++) {
        const PyEntry *en = &py_ent[k];
        char ini[12];
        int ni = 0;
        for (int j = 0; j < en->plen && ni < 11; j++)
            if ((j == 0 || py[en->pin + j - 1] == ' ') && py[en->pin + j] != ' ') ini[ni++] = (char)py[en->pin + j];
        if (ni < 2 || ni >= 11) continue;
        ini[ni] = 0;
        memcpy(py_abbr[py_nabbr].ini, ini, (size_t)ni + 1);
        py_abbr[py_nabbr++].ent = k;
    }
    if (py_abbr) qsort(py_abbr, (size_t)py_nabbr, sizeof *py_abbr, abbr_cmp);
    snprintf(dict_path, sizeof dict_path, "%s", path);
}

/* First entry with that exact key (binary search), or -1. */
static int py_find(const char *key, size_t kl) {
    int lo = 0, hi = py_n - 1, found = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const PyEntry *e = &py_ent[mid];
        int c = memcmp(py_keys + e->key, key, e->klen < kl ? e->klen : kl);
        if (!c) c = e->klen < kl ? -1 : e->klen > kl ? 1 : 0;
        if (!c) { found = mid; hi = mid - 1; }
        else if (c < 0) lo = mid + 1;
        else hi = mid - 1;
    }
    return found;
}

static void py_add(int i, int consumed) {
    const PyEntry *e = &py_ent[i];
    char text[96];
    memcpy(text, py + e->text, e->tlen);
    text[e->tlen] = 0;
    int before = st.ncand;
    add_cand(text);
    if (st.ncand > before) cand_len[st.ncand - 1] = consumed;
}

static void py_exact(size_t len) {
    int f = py_find(raw, len);
    for (int i = f; f >= 0 && i < py_n && st.ncand < IME_MAX_CAND; i++) {
        const PyEntry *e = &py_ent[i];
        if (e->klen != len || memcmp(py_keys + e->key, raw, len)) break;
        py_add(i, (int)len);
    }
}

/* Each syllable written as its initial (zh, ch, sh or one letter) or in full. */
static int abbr_match(const PyEntry *e, const char *const *tok, const int *tl, int nt) {
    int t = 0;
    for (int j = 0; j < e->plen && t < nt; j++) {
        if (py[e->pin + j] == ' ' || (j > 0 && py[e->pin + j - 1] != ' ')) continue;
        int sl = 0;
        while (j + sl < e->plen && py[e->pin + j + sl] != ' ') sl++;
        if (tl[t] > sl || memcmp(py + e->pin + j, tok[t], (size_t)tl[t])) return 0;
        t++;
    }
    return t == nt;
}

static void py_abbrev(void) {
    const char *tok[11];
    int tl[11], nt = 0;
    char ini[12];
    for (const char *p = raw; *p && nt < 11;) {
        int l = (p[1] == 'h' && strchr("zcs", p[0])) ? 2 : 1;
        tok[nt] = p; tl[nt] = l; ini[nt] = p[0]; nt++;
        p += l;
    }
    if (nt < 2 || nt > 10 || !py_abbr) return;
    for (int t = 0; t < nt; t++) if (strchr("aeiouv", ini[t])) return;     /* with vowels it is not an abbreviation */
    ini[nt] = 0;
    int lo = 0, hi = py_nabbr - 1, f = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(py_abbr[mid].ini, ini);
        if (!c) { f = mid; hi = mid - 1; } else if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    int added = 0;
    for (int i = f; f >= 0 && i < py_nabbr && !strcmp(py_abbr[i].ini, ini) && added < 24 && st.ncand < IME_MAX_CAND; i++)
        if (abbr_match(&py_ent[py_abbr[i].ent], tok, tl, nt)) { py_add(py_abbr[i].ent, (int)strlen(raw)); added++; }
}

/* Longer phrases starting with what was typed: the 8 most frequent. */
static void py_complete(void) {
    size_t rl = strlen(raw);
    int lo = 0, hi = py_n - 1, f = py_n;
    while (lo <= hi) {                      /* first key >= raw */
        int mid = (lo + hi) / 2;
        const PyEntry *e = &py_ent[mid];
        int c = memcmp(py_keys + e->key, raw, e->klen < rl ? e->klen : rl);
        if (!c && e->klen < rl) c = -1;
        if (c < 0) lo = mid + 1; else { f = mid; hi = mid - 1; }
    }
    int best[8], nb = 0;
    for (int i = f, scanned = 0; i < py_n && scanned < 20000; i++, scanned++) {
        const PyEntry *e = &py_ent[i];
        if (e->klen < rl || memcmp(py_keys + e->key, raw, rl)) break;
        if (e->klen == rl) continue;
        int k;
        if (nb < 8) k = nb++;
        else if (e->weight > py_ent[best[7]].weight) k = 7;
        else continue;
        best[k] = i;
        for (; k > 0 && py_ent[best[k - 1]].weight < py_ent[best[k]].weight; k--) { int tmp = best[k]; best[k] = best[k - 1]; best[k - 1] = tmp; }
    }
    for (int k = 0; k < nb && st.ncand < IME_MAX_CAND; k++) py_add(best[k], (int)rl);
}

static void py_update(void) {
    clear_cand();
    char tmp[sizeof acc + sizeof raw];
    snprintf(tmp, sizeof tmp, "%s%s", acc, raw);
    set_preedit(tmp);
    if (!raw[0] || !py) return;
    size_t rl = strlen(raw);
    /* 1. the whole phrase */
    py_exact(rl);
    /* 2. abbreviation by initials: "zg", "zhg", "bjdx" (北京大学) */
    py_abbrev();
    /* 3. phrases starting with what was typed ("zhongg" → 中国) */
    if (rl >= 3) py_complete();
    /* 4. those covering the beginning, longest to shortest (the rest remains) */
    for (size_t len = rl - 1; len > 0 && st.ncand < IME_MAX_CAND; len--) py_exact(len);
    changed = 1;
}

static void py_clear(void) { raw[0] = acc[0] = 0; clear_cand(); set_preedit(""); }

static void py_pick(int i, char *out, size_t size) {
    if (i < 0 || i >= st.ncand) return;
    append(acc, sizeof acc, st.cand[i]);
    size_t used = (size_t)cand_len[i], n = strlen(raw);
    memmove(raw, raw + (used < n ? used : n), n - (used < n ? used : n) + 1);
    if (!raw[0]) { append(out, size, acc); py_clear(); }
    else py_update();
}

static int pinyin_key(const KeyInfo *k, char *out, size_t size) {
    int empty = !raw[0] && !acc[0];
    char c = k->chr[0] && !k->chr[1] ? k->chr[0] : 0;
    if (c >= 'a' && c <= 'z') {
        size_t n = strlen(raw);
        if (n + 1 < sizeof raw) { raw[n] = c; raw[n + 1] = 0; }
        py_update();
        return 1;
    }
    if (empty) return 0;
    int d = digit_pick(k);
    if (d >= 0) { py_pick(d, out, size); return 1; }
    if (!strcmp(k->key, " ")) { if (st.ncand) py_pick(st.sel, out, size); else { append(out, size, acc); append(out, size, raw); py_clear(); } return 1; }
    if (!strcmp(k->key, "Enter")) { append(out, size, acc); append(out, size, raw); py_clear(); return 1; }
    if (!strcmp(k->key, "Escape")) { py_clear(); return 1; }
    if (!strcmp(k->key, "Backspace")) {
        if (raw[0]) raw[strlen(raw) - 1] = 0; else drop_last_char(acc);
        if (!raw[0] && !acc[0]) py_clear(); else py_update();
        return 1;
    }
    if (c == '\'') { py_update(); return 1; }
    if (!strcmp(k->key, "ArrowDown") || !strcmp(k->key, "ArrowRight")) { if (st.ncand) { st.sel = (st.sel + 1) % st.ncand; changed = 1; } return 1; }
    if (!strcmp(k->key, "ArrowUp") || !strcmp(k->key, "ArrowLeft")) { if (st.ncand) { st.sel = (st.sel + st.ncand - 1) % st.ncand; changed = 1; } return 1; }
    if (c == '=' || !strcmp(k->key, "PageDown")) { if (st.sel + IME_PAGE < st.ncand) { st.sel = (st.sel / IME_PAGE + 1) * IME_PAGE; changed = 1; } return 1; }
    if (c == '-' || !strcmp(k->key, "PageUp")) { if (st.sel >= IME_PAGE) { st.sel = (st.sel / IME_PAGE - 1) * IME_PAGE; changed = 1; } return 1; }
    append(out, size, acc);                     /* something else: confirms what was typed and passes through */
    append(out, size, raw);
    py_clear();
    return 0;
}

/* ================= common ================= */

void ime_reset(void) {
    memset(&syl, 0, sizeof syl);
    nhist = 0;
    kana[0] = pend[0] = raw[0] = acc[0] = 0;
    if (st.preedit[0] || st.ncand) changed = 1;
    st.preedit[0] = 0;
    clear_cand();
}

int ime_set(const char *name) {
    int e = !name || !*name ? E_NONE : !strcmp(name, "hangul") ? E_HANGUL : !strcmp(name, "kana") ? E_KANA :
            !strcmp(name, "pinyin") ? E_PINYIN : -1;
    if (e < 0) return -1;
    ime_reset();
    engine = e;
    changed = 1;
    dict_path[0] = 0;
    if (e == E_KANA) { skk_load(); if (skk) snprintf(dict_path, sizeof dict_path, "%s", "skkdic"); return skk ? 0 : 1; }
    if (e == E_PINYIN) { py_load(); if (py) snprintf(dict_path, sizeof dict_path, "%s", "pinyin_simp"); return py && py_n ? 0 : 1; }
    return 0;
}

const char *ime_current(void) {
    return engine == E_HANGUL ? "hangul" : engine == E_KANA ? "kana" : engine == E_PINYIN ? "pinyin" : "";
}

void ime_context(int on) {
    if (context && !on) ime_reset();
    context = on;
}

int ime_key(const KeyInfo *k, char *out, size_t size) {
    out[0] = 0;
    if (!engine || !context) return 0;
    if (k->ctrl || k->alt || k->meta) {         /* shortcuts: confirms whatever there is and passes through */
        if (engine == E_HANGUL) { if (syl.l || syl.v || syl.t) hangul_commit(out, size); }
        else if (engine == E_KANA) { if (kana[0] || pend[0]) kana_commit(out, size); }
        else if (raw[0] || acc[0]) { append(out, size, acc); append(out, size, raw); py_clear(); }
        return 0;
    }
    if (engine == E_HANGUL) return hangul_key(k, out, size);
    if (engine == E_KANA) return kana_key(k, out, size);
    return pinyin_key(k, out, size);
}

const ImeState *ime_state(void) { return &st; }
int ime_changed(void) { int c = changed; changed = 0; return c; }
const char *ime_dict_path(void) { return dict_path; }
