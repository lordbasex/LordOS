/* ime_test.c — headless test of the input methods (ime.c): simulates keys and
 * prints the committed text, the preedit and the candidates.
 *   build/ime-test hangul|kana|pinyin "keys"
 * In the keys: _ = Space, ^ = Enter, < = Backspace, ~ = Escape, + = Shift pressed alone (as a real keyboard
 * sends it before a capital), everything else letter by letter. */
#include <stdio.h>
#include <string.h>

#include "ime.h"

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    int r = ime_set(argv[1]);
    ime_context(1);
    printf("motor %s (%d, diccionario «%s»)\n", argv[1], r, ime_dict_path());
    char out[2048] = "";
    for (const char *p = argv[2]; *p; p++) {
        KeyInfo k;
        memset(&k, 0, sizeof k);
        switch (*p) {
        case '_': strcpy(k.key, " "); strcpy(k.chr, " "); break;
        case '^': strcpy(k.key, "Enter"); break;
        case '<': strcpy(k.key, "Backspace"); break;
        case '~': strcpy(k.key, "Escape"); break;
        case '+': k.shift = 1; break;                       /* no key name and no text, like keyboard.c's Shift */
        default: k.key[0] = k.chr[0] = *p; k.shift = *p >= 'A' && *p <= 'Z'; break;
        }
        char commit[512];
        int used = ime_key(&k, commit, sizeof commit);
        if (strlen(out) + strlen(commit) + 8 >= sizeof out) break;
        strcat(out, commit);
        if (!used && k.chr[0]) strcat(out, k.chr);       /* the key passed through: type its own text */
    }
    const ImeState *s = ime_state();
    printf("committed: %s\npreedit: %s\ncandidates (%d):", out, s->preedit, s->ncand);
    for (int i = 0; i < s->ncand && i < 12; i++) printf(" %s%s", i == s->sel ? "*" : "", s->cand[i]);
    printf("\n");
    return 0;
}
