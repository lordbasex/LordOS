/* tsecret.c — a program asking for a password on a Terminal session (term_asks_secret, term.h): read from
 * the pty itself, not guessed from the text. getpass(3) and its kin (sudo, ssh, git, gpg) turn echo off and
 * keep line input; the prompt on the cursor's line must also name a password, so a program that hides input
 * for another reason isn't offered one (a full-screen program keeps line input off anyway). Its own file so
 * build/term-secret-test can check it on a real pty. */
#include "term.h"
#include "tcapture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>

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

/* The program asking on the session's pty: its executable and arguments (the foreground process group's
 * leader, from /proc: what it really runs, not what it prints) and what it asks for. 0 if it could be read. */
int term_secret_target(Session *s, const char *prompt, TcapTarget *t, char *exe, size_t exen) {
    memset(t, 0, sizeof *t);
    if (exe && exen) exe[0] = 0;
    if (!s || !s->alive || s->pid <= 0) return -1;
    pid_t pg = tcgetpgrp(s->fd);
    if (pg <= 0) return -1;
    char path[64], buf[4096];
    snprintf(path, sizeof path, "/proc/%d/cmdline", (int)pg);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t r = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (r <= 0) return -1;
    buf[r] = 0;
    const char *argv[64];
    int argc = 0;
    for (ssize_t i = 0; i < r && argc < 64; i += (ssize_t)strlen(buf + i) + 1) argv[argc++] = buf + i;
    if (exe && exen) {
        snprintf(path, sizeof path, "/proc/%d/exe", (int)pg);
        ssize_t n = readlink(path, exe, exen - 1);
        exe[n > 0 ? n : 0] = 0;
    }
    return tcap_target(argv, argc, prompt, t);
}

/* Whether something is tracing this process (a debugger reading its memory): then no password is captured,
 * filled or saved. lord-server is not dumpable, so only root could; this refuses to help it. */
int term_traced(void) {
    char buf[2048];
    int fd = open("/proc/self/status", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 1;
    ssize_t r = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (r <= 0) return 1;
    buf[r] = 0;
    const char *tp = strstr(buf, "\nTracerPid:");
    return !tp || atoi(tp + 11) != 0;
}
