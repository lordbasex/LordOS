/* tsecret.c — a program asking for a password on a Terminal session (term_asks_secret, term.h): read from
 * the pty itself, not guessed from the text. getpass(3) and its kin (sudo, ssh, git, gpg) turn echo off and
 * keep line input; the prompt on the cursor's line must also name a password, so a program that hides input
 * for another reason isn't offered one (a full-screen program keeps line input off anyway). Its own file so
 * build/term-secret-test can check it on a real pty. */
#include "term.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>

int term_prompt_names_secret(const char *line) {
    /* what programs print in the languages LordOS speaks (data matched against their output, not text shown) */
    static const char *const words[] = { "password", "passphrase", "passcode", "pin:", "contraseña", "clave", "senha", /* i18n-ok */
                                         "mot de passe", "passwort", "parola d", NULL };
    char low[256];
    size_t i = 0;
    for (; line[i] && i + 1 < sizeof low; i++) low[i] = (char)(line[i] >= 'A' && line[i] <= 'Z' ? line[i] + 32 : line[i]);
    low[i] = 0;
    for (int w = 0; words[w]; w++) if (strstr(low, words[w])) return 1;
    return 0;
}

int term_asks_secret(Session *s, char *prompt, size_t n) {
    if (prompt && n) prompt[0] = 0;
    struct termios tio;
    if (!s || !s->alive || s->pid <= 0 || s->gateway || tcgetattr(s->fd, &tio)) return 0;
    if ((tio.c_lflag & ECHO) || !(tio.c_lflag & ICANON)) return 0;
    long long a = vt_cursor_abs(&s->t);
    char *line = vt_text_range(&s->t, a, a);
    int yes = line && term_prompt_names_secret(line);
    if (yes && prompt && n) {
        const char *p = line;
        while (*p == ' ') p++;
        snprintf(prompt, n, "%s", p);
        for (size_t k = strlen(prompt); k > 0 && prompt[k - 1] == ' '; k--) prompt[k - 1] = 0;
    }
    free(line);
    return yes;
}
