/* pkfield_test.c — the agent's pure parts (hw/pkfield.c). The field's contents here are made-up text. */
#include "../src/hw/pkfield.h"

#include <stdio.h>
#include <string.h>

static int pass, fail;
#define CK(c) do { if (c) pass++; else { fail++; printf("FAILED line %d: %s\n", __LINE__, #c); } } while (0)

int main(void) {
    PkField f = { 0 };
    CK(pkf_open(&f) == 0);
    CK(pkf_add(&f, "a") && pkf_add(&f, "ñ") && pkf_add(&f, "€") && pkf_add(&f, "😀"));
    CK(f.len == 10 && pkf_chars(&f) == 4);
    pkf_back(&f); CK(f.len == 6 && pkf_chars(&f) == 3);
    pkf_back(&f); CK(f.len == 3);
    pkf_back(&f); CK(f.len == 1);
    pkf_back(&f); CK(f.len == 0);
    pkf_back(&f); CK(f.len == 0);
    CK(!pkf_add(&f, "\n") && !pkf_add(&f, "a\rb") && !pkf_add(&f, "\x7f") && !pkf_add(&f, "") && f.len == 0);
    CK(!pkf_add(&f, "\xc3") && !pkf_add(&f, "\xc0\x80") && !pkf_add(&f, "\xed\xa0\x80") && !pkf_add(&f, "\xf4\x90\x80\x80"));
    int n = 0;
    while (pkf_add(&f, "x")) n++;
    CK(n == PKF_MAX - 1 && f.len == PKF_MAX - 1);       /* room for the newline */
    pkf_wipe(&f);
    CK(f.len == 0);
    int zero = 1;
    for (int i = 0; i < PKF_MAX; i++) if (f.buf[i]) zero = 0;
    CK(zero);

    CK(pk_line_kind("PAM_PROMPT_ECHO_OFF Password: ") == PKL_ASK_HIDDEN);
    CK(pk_line_kind("PAM_PROMPT_ECHO_ON login:") == PKL_ASK_SHOWN);
    CK(pk_line_kind("PAM_ERROR_MSG x") == PKL_ERROR && pk_line_kind("PAM_TEXT_INFO x") == PKL_INFO);
    CK(pk_line_kind("SUCCESS") == PKL_SUCCESS && pk_line_kind("FAILURE") == PKL_FAILURE);
    CK(pk_line_kind("") == PKL_OTHER && pk_line_kind("SUCCES") == PKL_OTHER);

    char o[32];
    pk_clean(o, sizeof o, "run\n\ttool"); CK(!strcmp(o, "runtool"));
    pk_clean(o, sizeof o, "a\xe2\x80\xae" "b\xe2\x80\x8b" "c\xe2\x81\xa6" "d"); CK(!strcmp(o, "abcd"));    /* RLO, ZWSP, LRI */
    pk_clean(o, sizeof o, "a\xff" "b\xc3"); CK(!strcmp(o, "ab"));
    pk_clean(o, sizeof o, NULL); CK(!strcmp(o, ""));
    pk_clean(o, 5, "abcñ€"); CK(!strcmp(o, "abc"));          /* never half a character */
    pk_clean(o, 6, "abcñ€"); CK(!strcmp(o, "abcñ"));

    CK(pk_stat_start("12 (a b) c) S 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 987654 0 0") == 987654);
    CK(pk_stat_start("12 (x) S 1 2 3") == 0);
    CK(pk_stat_start("nothing") == 0);

    printf("pkfield-test: %d ok, %d failed\n", pass, fail);
    return fail != 0;
}
