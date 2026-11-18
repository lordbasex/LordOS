/* startlogin_test.c — who may ask for the login screen in text mode (src/remote/startlogin.h). */
#include "../src/remote/startlogin.h"
#include <stdio.h>

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)
#define TTY(n) ((4u << 8) | (n))

int main(void) {
    CHECK(startlogin_check(1000, TTY(1), 1, 1000) == 1);          /* logged in on tty1, typing there */
    CHECK(startlogin_check(1001, TTY(3), 3, 1001) == 3);          /* another console, the one in front */
    CHECK(startlogin_check(1000, TTY(2), 1, 1000) == SL_NOT_IN_FRONT);
    CHECK(startlogin_check(1000, TTY(1), 1, 1001) == SL_NOT_ITS_OWNER);   /* someone else's console */
    CHECK(startlogin_check(1000, TTY(1), 1, 0) == SL_NOT_ITS_OWNER);      /* nobody logged in there */
    CHECK(startlogin_check(1000, (136u << 8) | 0, 1, 1000) == SL_NOT_A_CONSOLE);   /* /dev/pts/0: a terminal window */
    CHECK(startlogin_check(1000, 0, 1, 1000) == SL_NOT_A_CONSOLE);        /* no terminal: a cron job, an app */
    CHECK(startlogin_check(1000, (4u << 8) | 64, 1, 1000) == SL_NOT_A_CONSOLE);   /* ttyS0: a serial line */
    CHECK(startlogin_check(1000, TTY(0), 0, 1000) == SL_NOT_A_CONSOLE);   /* tty0 is "the current one", not a console */
    CHECK(startlogin_check(0, TTY(1), 1, 0) == SL_NOT_A_PERSON);
    CHECK(startlogin_check(999, TTY(1), 1, 999) == SL_NOT_A_PERSON);
    CHECK(startlogin_check(65534, TTY(1), 1, 65534) == SL_NOT_A_PERSON);
    puts(fails ? "startlogin: FAILED" : "startlogin: ok");
    return fails != 0;
}
