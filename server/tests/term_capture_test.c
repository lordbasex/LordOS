/* term_capture_test.c — Save to Passwords from the Terminal (src/term/tcapture.c): what program asked, read from
 * its own arguments, and when a typed password becomes an offer (accepted) or is wiped (rejected, abandoned,
 * filled from Passwords, expired, excluded). */
#include "term/tcapture.h"

#include <stdio.h>
#include <string.h>

static int fails, checks;
static void check(int ok, const char *what) { checks++; if (!ok) { fails++; printf("FAIL %s\n", what); } }

static TcapTarget T(const char *const *argv, int argc, const char *prompt) {
    TcapTarget t;
    tcap_target(argv, argc, prompt, &t);
    return t;
}
#define ARGV(...) ((const char *const[]){ __VA_ARGS__ }), (int)(sizeof((const char *const[]){ __VA_ARGS__ }) / sizeof(char *))

static int offers;
static unsigned last_token;
static int last_sid;
static void on_offer(int sid, unsigned token, const TcapTarget *t) { (void)t; offers++; last_token = token; last_sid = sid; }

static void type(int sid, const char *s, double now) { tcap_keys(sid, s, strlen(s), now); }

int main(void) {
    TcapTarget t;

    /* ---- what asked ---- */
    t = T(ARGV("/usr/bin/ssh", "-o", "StrictHostKeyChecking=no", "-i", "/home/a/key.pem", "ec2-user@78.13.253.112"), "");
    check(t.kind == TK_SSH && !strcmp(t.host, "78.13.253.112") && !strcmp(t.user, "ec2-user") && !t.port[0], "ssh: options with values are skipped");
    t = T(ARGV("ssh", "-p", "2222", "-l", "ana", "build.example.org"), "");
    check(!strcmp(t.host, "build.example.org") && !strcmp(t.user, "ana") && !strcmp(t.port, "2222"), "ssh: -p and -l");
    t = T(ARGV("ssh", "-p2222", "ssh://bob@srv:2200"), "");
    check(!strcmp(t.host, "srv") && !strcmp(t.user, "bob") && !strcmp(t.port, "2200"), "ssh: an ssh:// URL and an attached -p");
    t = T(ARGV("scp", "-P", "22", "notes.txt", "ana@files.lan:/tmp/"), "");
    check(t.kind == TK_SSH && !strcmp(t.program, "scp") && !strcmp(t.host, "files.lan") && !strcmp(t.user, "ana") && !strcmp(t.port, "22"), "scp: the remote side");
    t = T(ARGV("sftp", "ana@files.lan"), "");
    check(!strcmp(t.host, "files.lan") && !strcmp(t.user, "ana"), "sftp");
    t = T(ARGV("rsync", "-av", "src/", "ana@backup.lan:dst/"), "");
    check(!strcmp(t.host, "backup.lan") && !strcmp(t.user, "ana"), "rsync over ssh");
    t = T(ARGV("mysql", "-h", "db.lan", "-uroot", "--port=3307"), "");
    check(t.kind == TK_DB && !strcmp(t.engine, "MySQL") && !strcmp(t.host, "db.lan") && !strcmp(t.user, "root") && !strcmp(t.port, "3307"), "mysql");
    t = T(ARGV("psql", "postgresql://app@pg.lan:5433/shop"), "");
    check(!strcmp(t.engine, "PostgreSQL") && !strcmp(t.host, "pg.lan") && !strcmp(t.user, "app") && !strcmp(t.port, "5433"), "psql URI");
    t = T(ARGV("redis-cli", "-p", "6380"), "");
    check(!strcmp(t.engine, "Redis") && !strcmp(t.host, "localhost") && !strcmp(t.port, "6380"), "redis-cli: local, its port");
    t = T(ARGV("git", "push"), "Password for 'https://ana@github.com': ");
    check(t.kind == TK_GIT && !strcmp(t.host, "github.com") && !strcmp(t.user, "ana"), "git: the URL in its prompt");
    t = T(ARGV("ssh", "ana@host;rm -rf ~"), "");
    check(!t.host[0], "a host with shell characters is not kept");
    check(tcap_excluded("sudo") && tcap_excluded("su") && tcap_excluded("passwd") && !tcap_excluded("ssh"), "the user's own password is never kept");

    TcapTarget ssh = T(ARGV("ssh", "ana@srv"), "");
    double now = 1000;

    /* ---- accepted: typed, Enter, the program stops asking and doesn't fail ---- */
    tcap_prompt(1, 1, &ssh, now);
    type(1, "s3cret", now += 10);
    type(1, "\r", now += 10);
    tcap_prompt(1, 0, NULL, now += 50);
    tcap_output(1, (const uint8_t *)"Last failed login: Mon\r\nWelcome\r\n", 33);
    tcap_tick(now += 500, on_offer);
    check(offers == 0, "not offered before the program has been quiet for a while");
    tcap_tick(now += CAP_VERDICT_MS, on_offer);
    check(offers == 1 && last_sid == 1 && last_token, "accepted: offered (a MOTD's \"failed login\" is not a rejection)");
    size_t n = 0;
    TcapTarget got;
    const char *v = tcap_value(1, last_token, &n, &got);
    check(v && n == 6 && !memcmp(v, "s3cret", 6) && !strcmp(got.host, "srv"), "the value, for saving");
    check(!tcap_value(1, last_token + 1, &n, NULL), "a wrong token gets nothing");
    tcap_drop(1);
    check(!tcap_value(1, last_token, &n, NULL) && !tcap_pending(), "dropped: gone");

    /* ---- rejected: asked again after Enter ---- */
    offers = 0;
    tcap_prompt(2, 1, &ssh, now);
    type(2, "wrong\r", now += 10);
    tcap_output(2, (const uint8_t *)"Permission denied, please try again.\r\n", 38);
    tcap_prompt(2, 0, NULL, now += 5);
    tcap_tick(now += CAP_VERDICT_MS + 10, on_offer);
    check(offers == 0 && !tcap_pending(), "rejected (its words): wiped, not offered");
    tcap_prompt(3, 1, &ssh, now);
    type(3, "wrong\r", now += 10);
    tcap_prompt(3, 0, NULL, now += 5);
    tcap_prompt(3, 1, &ssh, now += 5);              /* the same program asks again */
    tcap_prompt(3, 0, NULL, now += 5);
    tcap_tick(now += CAP_VERDICT_MS + 10, on_offer);
    check(offers == 0 && !tcap_pending(), "asked again: the first one is wiped, the new prompt starts empty");

    /* ---- editing keys ---- */
    tcap_prompt(4, 1, &ssh, now);
    type(4, "abX\x7f" "c\xc3\xb1\x7f" "d\r", now += 10);    /* backspace removes a whole UTF-8 character */
    tcap_prompt(4, 0, NULL, now += 5);
    tcap_tick(now += CAP_VERDICT_MS + 10, on_offer);
    v = tcap_value(4, last_token, &n, NULL);
    check(offers == 1 && v && n == 4 && !memcmp(v, "abcd", 4), "backspace, also over UTF-8");
    tcap_drop(4);
    tcap_prompt(5, 1, &ssh, now);
    type(5, "abc\x15xyz\r", now += 10);              /* Ctrl+U clears the line */
    tcap_prompt(5, 0, NULL, now += 5);
    tcap_tick(now += CAP_VERDICT_MS + 10, on_offer);
    v = tcap_value(5, last_token, &n, NULL);
    check(v && n == 3 && !memcmp(v, "xyz", 3), "Ctrl+U");
    tcap_drop(5);

    /* ---- never offered ---- */
    offers = 0;
    tcap_prompt(6, 1, &ssh, now);
    type(6, "abc\x03", now += 10);                   /* Ctrl+C */
    check(!tcap_pending(), "Ctrl+C wipes it");
    tcap_prompt(7, 1, &ssh, now);
    type(7, "abc", now += 10);
    tcap_prompt(7, 0, NULL, now += 5);               /* stopped asking without Enter: filled from Passwords, or gone */
    check(!tcap_pending(), "no Enter typed (a fill from Passwords): nothing kept");
    TcapTarget sudo = T(ARGV("sudo", "true"), "");
    tcap_prompt(8, 1, &sudo, now);
    type(8, "mine\r", now += 10);
    check(!tcap_pending(), "sudo: not even captured");
    tcap_prompt(9, 1, &ssh, now);
    type(9, "\r", now += 10);
    check(!tcap_pending(), "an empty password is not kept");
    tcap_prompt(10, 1, &ssh, now);
    type(10, "x\r", now += 10);
    tcap_tick(now += CAP_SENT_MS + 10, on_offer);
    check(offers == 0 && !tcap_pending(), "still asking long after Enter: dropped");

    /* ---- the offer expires ---- */
    tcap_prompt(11, 1, &ssh, now);
    type(11, "pw\r", now += 10);
    tcap_prompt(11, 0, NULL, now += 5);
    tcap_tick(now += CAP_VERDICT_MS + 10, on_offer);
    unsigned tok = last_token;
    tcap_tick(now += SAVE_OFFER_MS + 10, on_offer);
    check(!tcap_value(11, tok, &n, NULL) && !tcap_pending(), "an offer nobody answered is wiped after a minute");

    /* ---- too long ---- */
    char big[CAP_MAX + 20];
    memset(big, 'a', sizeof big - 2);
    big[sizeof big - 2] = '\r';
    big[sizeof big - 1] = 0;
    offers = 0;
    tcap_prompt(12, 1, &ssh, now);
    type(12, big, now += 10);
    check(!tcap_pending() && offers == 0, "a password longer than the buffer is not kept");

    printf("term-capture-test: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
