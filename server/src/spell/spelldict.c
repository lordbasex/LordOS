/* spelldict.c — the person's dictionary file, read and written as text (see spellsvc.h). Pure. */
#include "spellsvc.h"

#include <string.h>

int spelldict_parse(const char *text, size_t n, SpellWord *out, int max) {
    int k = 0;
    size_t i = 0;
    while (i < n && k < max) {
        size_t e = i;
        while (e < n && text[e] != '\n') e++;
        size_t a = i, b = e;
        if (b > a && text[b - 1] == '\r') b--;
        while (a < b && (text[a] == ' ' || text[a] == '\t')) a++;
        while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t')) b--;
        i = e + 1;
        if (b == a || text[a] == '#' || !spell_word_ok(text + a, b - a)) continue;
        int dup = 0;
        for (int j = 0; j < k && !dup; j++) dup = strlen(out[j]) == b - a && !memcmp(out[j], text + a, b - a);
        if (dup) continue;
        memcpy(out[k], text + a, b - a);
        out[k][b - a] = 0;
        k++;
    }
    return k;
}

int spelldict_format(const SpellWord *w, int n, char *out, size_t cap) {
    size_t at = 0;
    for (int i = 0; i < n; i++) {
        size_t l = strlen(w[i]);
        if (!spell_word_ok(w[i], l)) continue;
        if (at + l + 2 > cap) return -1;
        memcpy(out + at, w[i], l);
        at += l;
        out[at++] = '\n';
    }
    if (at + 1 > cap) return -1;
    out[at] = 0;
    return (int)at;
}
