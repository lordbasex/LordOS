/* unicode.c — the only place where the unidata.h tables are compiled. */
#define UNIDATA_TABLES
#include "unicode.h"

int uc_bidi(uint32_t cp) { return uni_lookup(uni_bidi, UNI_N(uni_bidi), cp); }
int uc_join(uint32_t cp) { return uni_lookup(uni_join, UNI_N(uni_join), cp); }
int uc_script(uint32_t cp) { return uni_lookup(uni_script, UNI_N(uni_script), cp); }
int uc_linebreak(uint32_t cp) { return uni_lookup(uni_linebreak, UNI_N(uni_linebreak), cp); }
int uc_ccc(uint32_t cp) { return uni_lookup(uni_ccc, UNI_N(uni_ccc), cp); }
int uc_ignorable(uint32_t cp) { return uni_lookup(uni_ignorable, UNI_N(uni_ignorable), cp); }
const char *uc_script_name(int s) { return s >= 0 && s < SCRIPT_COUNT ? uni_script_name[s] : "Common"; }
const char *uc_script_tag(int s) { return s >= 0 && s < SCRIPT_COUNT ? uni_script_tag[s] : "DFLT"; }

uint32_t uc_mirror(uint32_t cp) {
    int lo = 0, hi = UNI_N(uni_mirror) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (uni_mirror[mid].a == cp) return uni_mirror[mid].b;
        if (uni_mirror[mid].a < cp) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

uint32_t uc_bracket(uint32_t cp, int *open) {
    int lo = 0, hi = UNI_N(uni_brackets) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (uni_brackets[mid].a == cp) { *open = uni_brackets[mid].open; return uni_brackets[mid].b; }
        if (uni_brackets[mid].a < cp) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

int uc_indic_cat(uint32_t cp) { return uni_lookup(uni_indic_cat, UNI_N(uni_indic_cat), cp); }
int uc_indic_pos(uint32_t cp) { return uni_lookup(uni_indic_pos, UNI_N(uni_indic_pos), cp); }

int uc_mark(uint32_t cp) { return uni_lookup(uni_mark, UNI_N(uni_mark), cp); }
int uc_sea_cat(uint32_t cp) { return uni_lookup(uni_sea, UNI_N(uni_sea), cp); }
int uc_use_cat(uint32_t cp) { return uni_lookup(uni_use, UNI_N(uni_use), cp); }
int uc_gcb(uint32_t cp) { return uni_lookup(uni_gcb, UNI_N(uni_gcb), cp); }
int uc_extpict(uint32_t cp) { return uni_lookup(uni_extpict, UNI_N(uni_extpict), cp); }
int uc_incb(uint32_t cp) { return uni_lookup(uni_incb, UNI_N(uni_incb), cp); }

int uc_decompose(uint32_t cp, uint32_t *a, uint32_t *b, int *excluded) {
    int lo = 0, hi = UNI_N(uni_decomp) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (uni_decomp[mid].cp == cp) { *a = uni_decomp[mid].a; *b = uni_decomp[mid].b; *excluded = uni_decomp[mid].excluded; return 1; }
        if (uni_decomp[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

uint32_t uc_compose(uint32_t a, uint32_t b) {
    for (int i = 0; i < UNI_N(uni_decomp); i++)
        if (uni_decomp[i].a == a && uni_decomp[i].b == b && !uni_decomp[i].excluded) return uni_decomp[i].cp;
    return 0;
}
