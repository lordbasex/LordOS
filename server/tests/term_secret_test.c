/* term_secret_test.c — Passwords in the Terminal (src/term/tsecret.c): a program asking for a password is
 * read from a real pty's settings (echo off, line input) and the prompt on the cursor's line. */
#include "term/term.h"

#include <locale.h>
#include <pty.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

void term_set_clipboard(const char *s, size_t n) { (void)s; (void)n; }

static int fails, checks;
static void check(int ok, const char *what) { checks++; if (!ok) { fails++; printf("FAIL %s\n", what); } }

static void tty(int slave, int echo, int icanon) {
    struct termios t;
    tcgetattr(slave, &t);
    if (echo) t.c_lflag |= ECHO; else t.c_lflag &= ~(tcflag_t)ECHO;
    if (icanon) t.c_lflag |= ICANON; else t.c_lflag &= ~(tcflag_t)ICANON;
    tcsetattr(slave, TCSANOW, &t);
}

int main(void) {
    setlocale(LC_CTYPE, "C.UTF-8");
    int master, slave;
    if (openpty(&master, &slave, NULL, NULL, NULL)) { printf("term-secret-test: skipped: needs a pty\n"); return 0; }
    static Session s;
    TermProfile p;
    memset(&p, 0, sizeof p);
    s.used = s.alive = 1;
    s.pid = getpid();
    s.fd = master;
    if (vt_init(&s.t, 60, 5, 10, &p)) return 1;
    char prompt[200];

    const char *ssh = "Last login: yesterday\r\nana@build.example.org's password: ";
    vt_write(&s.t, (const uint8_t *)ssh, strlen(ssh));
    tty(slave, 0, 1);
    check(term_asks_secret(&s, prompt, sizeof prompt) == 1, "ssh asks: echo off, line input, a password prompt");
    check(!strcmp(prompt, "ana@build.example.org's password:"), "the prompt is the cursor's line, trimmed");
    tty(slave, 1, 1);
    check(term_asks_secret(&s, NULL, 0) == 0, "echo back on: no longer asking");
    tty(slave, 0, 0);
    check(term_asks_secret(&s, NULL, 0) == 0, "a full-screen program (no line input) is not asking");

    vt_write(&s.t, (const uint8_t *)"\r\nName: ", 8);
    tty(slave, 0, 1);
    check(term_asks_secret(&s, NULL, 0) == 0, "echo off but the prompt names no password");
    vt_write(&s.t, (const uint8_t *)"\r\n[sudo] contraseña para ana: ", 31);
    check(term_asks_secret(&s, NULL, 0) == 1, "sudo in Spanish");
    vt_write(&s.t, (const uint8_t *)"\r\nEnter passphrase for key '/home/ana/.ssh/id_ed25519': ", 56);
    check(term_asks_secret(&s, NULL, 0) == 1, "an SSH key's passphrase");

    s.pid = 0;
    check(term_asks_secret(&s, NULL, 0) == 0, "a session without a process (a serial console) never asks");
    s.pid = getpid();
    s.alive = 0;
    check(term_asks_secret(&s, NULL, 0) == 0, "an ended session never asks");

    check(term_prompt_names_secret("Password for 'https://ana@github.com': ") && term_prompt_names_secret("Senha: ")
          && term_prompt_names_secret("PIN: ") && !term_prompt_names_secret("Username: "), "the words");

    vt_free(&s.t);
    close(slave);
    close(master);
    printf("term-secret-test: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
