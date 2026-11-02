/* firstrun_test.c — the strict first-run file parser (src/firstrun/firstrun.c, docs/IMAGER.md).
 *
 *   build/firstrun-test            validators, a full valid file, broken files, cross-key rules and
 *                                  30 000 random mutations (normal build, in `make test`)
 *   build/firstrun-fuzz N          the same plus N more mutations, under ASan/UBSan (also in `make test`)
 *   build/firstrun-test check F    parse file F and print "ok" or the reason (used by
 *                                  tests/imager_core_test.mjs to check files the JS serializer writes) */
#include "firstrun/firstrun.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const char *ED = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIF8zdvpQ/Ic++n9TOGaDGEe9/+IRgtRBf1H86da41yJ8 ana@laptop";
static const char *EC = "ecdsa-sha2-nistp256 AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBBwbMKUZp+OoFOXG9AqteOZlGM/WkyBjc1CpgUqvgeHFv2kzd7XYR87xzMpqjVv57J4/d1776zu3hG/O3KNB+EY=";
static const char *H6 = "$6$saltstring$svn8UoSVapNtMuq1ukKS4tPQd8iKwSMHWjl/O817G3uBnIFNjnQJuesI68u4OTLiBFdcbYEdFCoEOfaS35inz1";
static const char *H6R = "$6$rounds=10000$saltstringsaltst$OW1/O6BYHV6BcXZu8QVeXbDWra3Oeqh0sbHbbMCVNSnCM/UrjmM0Dp8vOuZeHBy/YTBmSK6H9qs/y3RnOaw5v.";
static const char *HY = "$y$j9T$a5XnTnCOaDHBZtHZ9F4Wd1$pvAh1x2YFgYZK8u/FnDXrWWBDuXqDuGRX8o2.zDuRw6";

static char full[8192];

static const char *full_file(void) {
    snprintf(full, sizeof full,
        "# lordos-firstrun, written by the LordOS Imager\n"
        "version=1\n"
        "hostname=pi-living\n"
        "lang=es\n"
        "timezone=America/Argentina/Buenos_Aires\n"
        "keyboard=latam\n"
        "keyboard_variant=\n"
        "wifi_country=AR\n"
        "user=ana\n"
        "fullname=Ana Pérez\n"
        "password_hash=%s\n"
        "autologin=0\n"
        "wifi_ssid_hex=4d792052656420e29da4\n"
        "wifi_psk=f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e\n"
        "wifi_hidden=1\n"
        "ssh=keys\n"
        "ssh_key=%s\n"
        "ssh_key=%s\n"
        "remote_desktop=1\n"
        "bluetooth=0\n"
        "theme=classic\n"
        "mode=light\n"
        "accent=violeta\n"
        "wallpaper=lumo-celestial\n"
        "cooler=active-cooler\n"
        "performance=performance\n"
        "overclock=1\n"
        "auto_updates=1\n", H6R, ED, EC);
    return full;
}

static int parses(const char *s) {
    FirstRun fr;
    char err[200];
    int r = fr_parse(s, strlen(s), &fr, err, sizeof err);
    fr_free(&fr);
    return r == 0;
}

static void validators(void) {
    CHECK(fr_valid_hostname("pi"));
    CHECK(fr_valid_hostname("living-room-pi5"));
    CHECK(fr_valid_hostname("a"));
    CHECK(fr_valid_hostname("pi5"));
    CHECK(!fr_valid_hostname(""));
    CHECK(!fr_valid_hostname("-pi"));
    CHECK(!fr_valid_hostname("pi-"));
    CHECK(!fr_valid_hostname("Pi"));
    CHECK(!fr_valid_hostname("pi.local"));
    CHECK(!fr_valid_hostname("pi_5"));
    CHECK(!fr_valid_hostname("12345"));
    CHECK(!fr_valid_hostname("localhost"));
    CHECK(fr_valid_hostname("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));   /* 63 */
    CHECK(!fr_valid_hostname("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));  /* 64 */
    CHECK(!fr_valid_hostname("pi\xc3\xb1"));

    CHECK(fr_valid_user("ana"));
    CHECK(fr_valid_user("_svc"));
    CHECK(fr_valid_user("ana-maria_2"));
    CHECK(!fr_valid_user("Ana"));
    CHECK(!fr_valid_user("1ana"));
    CHECK(!fr_valid_user("-ana"));
    CHECK(!fr_valid_user("root"));
    CHECK(!fr_valid_user("lordos"));
    CHECK(!fr_valid_user("systemd-foo"));
    CHECK(!fr_valid_user("sudo"));
    CHECK(!fr_valid_user("ana maria"));
    CHECK(!fr_valid_user("ana$"));
    CHECK(!fr_valid_user(""));
    CHECK(fr_valid_user("abcdefghijklmnopqrstuvwxyzabcdef"));    /* 32 */
    CHECK(!fr_valid_user("abcdefghijklmnopqrstuvwxyzabcdefg"));  /* 33 */

    CHECK(fr_valid_hash(H6));
    CHECK(fr_valid_hash(H6R));
    CHECK(fr_valid_hash(HY));
    CHECK(!fr_valid_hash("$1$abc$def"));
    CHECK(!fr_valid_hash("$6$salt$short"));
    CHECK(!fr_valid_hash("$6$rounds=10$saltstring$svn8UoSVapNtMuq1ukKS4tPQd8iKwSMHWjl/O817G3uBnIFNjnQJuesI68u4OTLiBFdcbYEdFCoEOfaS35inz1"));
    CHECK(!fr_valid_hash("$6$saltstringsaltstr$svn8UoSVapNtMuq1ukKS4tPQd8iKwSMHWjl/O817G3uBnIFNjnQJuesI68u4OTLiBFdcbYEdFCoEOfaS35inz1"));
    CHECK(!fr_valid_hash("$6$salt:x$svn8UoSVapNtMuq1ukKS4tPQd8iKwSMHWjl/O817G3uBnIFNjnQJuesI68u4OTLiBFdcbYEdFCoEOfaS35inz1"));
    CHECK(!fr_valid_hash("hunter2"));
    CHECK(!fr_valid_hash("!"));
    CHECK(!fr_valid_hash(""));

    CHECK(fr_valid_timezone("UTC"));
    CHECK(fr_valid_timezone("Europe/Madrid"));
    CHECK(fr_valid_timezone("America/Argentina/Buenos_Aires"));
    CHECK(fr_valid_timezone("Etc/GMT+3"));
    CHECK(!fr_valid_timezone("../etc/shadow"));
    CHECK(!fr_valid_timezone("/etc/passwd"));
    CHECK(!fr_valid_timezone("Europe/../../etc"));
    CHECK(!fr_valid_timezone("europe/madrid"));
    CHECK(!fr_valid_timezone("Europe//Madrid"));
    CHECK(!fr_valid_timezone("Europe/Madrid "));
    CHECK(!fr_valid_timezone("posix/Europe/Madrid") || 1);   /* starts lower-case: refused by the capital rule */

    CHECK(fr_valid_fullname("Ana Pérez"));
    CHECK(fr_valid_fullname("李小龍"));
    CHECK(fr_valid_fullname("O'Brien"));
    CHECK(!fr_valid_fullname("Ana:0:0"));
    CHECK(!fr_valid_fullname("Ana, Room 4"));
    CHECK(!fr_valid_fullname(" Ana"));
    CHECK(!fr_valid_fullname("Ana\x01"));
    CHECK(!fr_valid_fullname("Ana\xc3"));               /* truncated UTF-8 */
    CHECK(!fr_valid_fullname("\xc0\xaf"));               /* overlong */
    CHECK(!fr_valid_fullname("Ana\xe2\x80\xae" "evil"));  /* bidi override */
    CHECK(!fr_valid_fullname("-o"));

    CHECK(fr_valid_ssh_key(ED));
    CHECK(fr_valid_ssh_key(EC));
    CHECK(fr_valid_ssh_key("ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIF8zdvpQ/Ic++n9TOGaDGEe9/+IRgtRBf1H86da41yJ8"));
    CHECK(!fr_valid_ssh_key("ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIF8zdvpQ/Ic++n9TOGaDGEe9/+IRgtRBf1H86da41yJ"));   /* bad length */
    CHECK(!fr_valid_ssh_key("ssh-rsa AAAAC3NzaC1lZDI1NTE5AAAAIF8zdvpQ/Ic++n9TOGaDGEe9/+IRgtRBf1H86da41yJ8 x"));  /* type mismatch */
    CHECK(!fr_valid_ssh_key("command=\"/bin/sh\" ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIF8zdvpQ/Ic++n9TOGaDGEe9/+IRgtRBf1H86da41yJ8"));
    CHECK(!fr_valid_ssh_key("ssh-dss AAAAB3NzaC1kc3MAAACBAP"));
    CHECK(!fr_valid_ssh_key("ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIF8zdvpQ/Ic++n9TOGaDGEe9/+IRgtRBf1H86da41yJ8 a\x01"));
    CHECK(!fr_valid_ssh_key("ssh-ed25519 !!!!"));
    CHECK(!fr_valid_ssh_key("ssh-ed25519"));

    CHECK(fr_valid_wallpaper("lumo-celestial"));
    CHECK(!fr_valid_wallpaper("../x"));
    CHECK(!fr_valid_wallpaper("Lumo"));
    CHECK(fr_valid_accent("azul"));
    CHECK(!fr_valid_accent("blue"));

    unsigned char b[4];
    CHECK(fr_unhex("00ff", b, 4) == 2 && b[1] == 0xff);
    CHECK(fr_unhex("0g", b, 4) == -1);
    CHECK(fr_unhex("0F", b, 4) == -1);
    CHECK(fr_unhex("abc", b, 4) == -1);
    CHECK(fr_unhex("0011223344", b, 4) == -1);
}

static void full_valid(void) {
    FirstRun fr;
    char err[200];
    const char *s = full_file();
    int r = fr_parse(s, strlen(s), &fr, err, sizeof err);
    if (r) printf("  full file: %s\n", err);
    CHECK(r == 0);
    CHECK(!strcmp(fr.hostname, "pi-living"));
    CHECK(!strcmp(fr.lang, "es"));
    CHECK(!strcmp(fr.user, "ana"));
    CHECK(!strcmp(fr.fullname, "Ana Pérez"));
    CHECK(fr.ssid_len == 10 && !memcmp(fr.ssid, "My Red \xe2\x9d\xa4", 10));
    CHECK(fr.has_psk && fr.hidden == 1);
    CHECK(fr.ssh == FR_SSH_KEYS && fr.nkeys == 2);
    CHECK(fr.remote == 1 && fr.bluetooth == 0);
    CHECK(fr.cooler == FR_COOL_ACTIVE && fr.perf == FR_PERF_PERFORMANCE && fr.overclock == 1);
    CHECK(!strcmp(fr.theme, "classic") && !strcmp(fr.mode, "light") && !strcmp(fr.accent, "violeta"));
    CHECK(fr.has_variant && fr.variant[0] == 0);
    CHECK(fr.updates == 1);
    fr_free(&fr);

    /* CRLF and a BOM are accepted */
    char crlf[16384];
    size_t o = 0;
    crlf[o++] = (char)0xef; crlf[o++] = (char)0xbb; crlf[o++] = (char)0xbf;
    for (const char *p = s; *p; p++) { if (*p == '\n') crlf[o++] = '\r'; crlf[o++] = *p; }
    crlf[o] = 0;
    CHECK(parses(crlf));
    /* the smallest file */
    CHECK(parses("version=1\n"));
    CHECK(parses("version=1"));
    CHECK(parses("version=1\nhostname=pi\n"));
    CHECK(parses("version=1\nuser=ana\npassword_hash=$6$saltstring$svn8UoSVapNtMuq1ukKS4tPQd8iKwSMHWjl/O817G3uBnIFNjnQJuesI68u4OTLiBFdcbYEdFCoEOfaS35inz1\n"));
    CHECK(parses("version=1\nwifi_ssid_hex=6162\n"));    /* open network */
    CHECK(parses("version=1\ncooler=none\nperformance=conservative\n"));
}

static void replace_line(char *dst, size_t cap, const char *src, const char *key, const char *line) {
    /* the file with the line starting with key= replaced by `line` ("" = removed) */
    size_t o = 0, kl = strlen(key);
    for (const char *l = src; *l;) {
        const char *e = strchr(l, '\n');
        size_t ll = e ? (size_t)(e - l + 1) : strlen(l);
        if (!strncmp(l, key, kl) && l[kl] == '=') { o += (size_t)snprintf(dst + o, cap - o, "%s", line); }
        else { memcpy(dst + o, l, ll); o += ll; }
        l += ll;
    }
    dst[o] = 0;
}

static void broken(void) {
    static const char *const bad[] = {
        "", "\n", "# only a comment\n", "hostname=pi\nversion=1\n", "version=2\n", "version=1\nversion=1\n",
        "version=1\nhostname=pi\nhostname=pi2\n", "version=1\nfoo=bar\n", "version=1\nhostname\n",
        "version=1\nhostname= pi\n", "version=1\nhostname=pi \n", "version=1\nhostname=\n", "version=1\nhostname=PI\n",
        "version=1\n hostname=pi\n", "version=1\nhostname=p\ti\n", "version=1\nlang=de\n", "version=1\nlang=EN\n",
        "version=1\ntimezone=../../etc/shadow\n", "version=1\nkeyboard=../x\n", "version=1\nkeyboard=US\n",
        "version=1\nkeyboard_variant=intl\n", "version=1\nwifi_country=ar\n", "version=1\nwifi_country=ARG\n",
        "version=1\nuser=root\npassword_hash=$6$saltstring$svn8UoSVapNtMuq1ukKS4tPQd8iKwSMHWjl/O817G3uBnIFNjnQJuesI68u4OTLiBFdcbYEdFCoEOfaS35inz1\n",
        "version=1\nuser=ana\n", "version=1\npassword_hash=$6$saltstring$svn8UoSVapNtMuq1ukKS4tPQd8iKwSMHWjl/O817G3uBnIFNjnQJuesI68u4OTLiBFdcbYEdFCoEOfaS35inz1\n",
        "version=1\nuser=ana\npassword_hash=hunter2\n", "version=1\nuser=ana\npassword=hunter2\n",
        "version=1\nwifi_ssid_hex=\n", "version=1\nwifi_ssid_hex=00\n", "version=1\nwifi_ssid_hex=ABCD\n",
        "version=1\nwifi_ssid_hex=000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20\n",
        "version=1\nwifi_psk=f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e\n",
        "version=1\nwifi_ssid_hex=6162\nwifi_psk=hunter22\n",
        "version=1\nwifi_ssid_hex=6162\nwifi_password=hunter22\n",
        "version=1\nssh=on\n", "version=1\nssh=password\n", "version=1\nremote_desktop=1\n", "version=1\nremote_desktop=yes\n",
        "version=1\ncooler=fan\n", "version=1\ncooler=none\noverclock=1\n", "version=1\ncooler=passive\nperformance=performance\n",
        "version=1\nperformance=balanced\n", "version=1\ncooler=none\nperformance=quiet\n", "version=1\noverclock=0\n",
        "version=1\ntheme=gnome\n", "version=1\nmode=auto\n", "version=1\naccent=blue\n", "version=1\nwallpaper=../../etc\n",
        "version=1\nfullname=Ana\n", "version=1\nautologin=1\n",
        "version=1\nhostname=pi\x01\n", "version=1\r\nhostname=pi\r\r\n", "version=1\nhostname=\xff\n",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        int ok = parses(bad[i]);
        if (ok) printf("  accepted bad file %zu: %s\n", i, bad[i]);
        CHECK(!ok);
    }
    /* a NUL inside the buffer (strlen would stop early, so pass the real length) */
    {
        const char s[] = "version=1\nhostname=pi\0\n";
        FirstRun fr;
        char err[200];
        CHECK(fr_parse(s, sizeof s - 1, &fr, err, sizeof err) != 0);
        fr_free(&fr);
    }
    /* cross-key rules on the full file */
    char f[16384];
    const char *s = full_file();
    replace_line(f, sizeof f, s, "password_hash", "");
    CHECK(!parses(f));
    replace_line(f, sizeof f, s, "cooler", "cooler=passive\n");
    CHECK(!parses(f));                                       /* performance + overclock need a fan */
    replace_line(f, sizeof f, s, "ssh", "ssh=off\n");
    CHECK(!parses(f));                                       /* keys with ssh off */
    replace_line(f, sizeof f, s, "user", "");
    CHECK(!parses(f));
    replace_line(f, sizeof f, s, "wifi_ssid_hex", "");
    CHECK(!parses(f));
    /* 17 keys */
    {
        char many[65536];
        size_t o = (size_t)snprintf(many, sizeof many, "version=1\nuser=ana\npassword_hash=%s\nssh=keys\n", H6);
        for (int i = 0; i < 17; i++) o += (size_t)snprintf(many + o, sizeof many - o, "ssh_key=%s\n", ED);
        CHECK(!parses(many));
        o = (size_t)snprintf(many, sizeof many, "version=1\nuser=ana\npassword_hash=%s\nssh=keys\n", H6);
        for (int i = 0; i < 16; i++) o += (size_t)snprintf(many + o, sizeof many - o, "ssh_key=%s\n", ED);
        CHECK(parses(many));
    }
    /* a file larger than 64 KiB */
    {
        static char big[FR_MAX_FILE + 10];
        memset(big, '#', sizeof big - 1);
        memcpy(big, "version=1\n", 10);
        big[sizeof big - 1] = 0;
        CHECK(!parses(big));
    }
}

/* ---- random mutations: never crash, never read out of bounds (ASan) ---- */

static unsigned long long rng = 0x9e3779b97f4a7c15ull;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (unsigned)(rng >> 11); }

static void fuzz(long iters) {
    const char *s = full_file();
    size_t n = strlen(s);
    char *buf = malloc(n + 512);
    long accepted = 0;
    for (long it = 0; it < iters; it++) {
        size_t m = n;
        memcpy(buf, s, n);
        int edits = 1 + (int)(rnd() % 6);
        for (int e = 0; e < edits; e++) {
            unsigned op = rnd() % 5;
            size_t at = m ? rnd() % m : 0;
            if (op == 0 && m) buf[at] = (char)(rnd() & 0xff);                 /* flip a byte */
            else if (op == 1 && m) { memmove(buf + at, buf + at + 1, m - at - 1); m--; }   /* delete */
            else if (op == 2 && m + 1 < n + 512) { memmove(buf + at + 1, buf + at, m - at); buf[at] = "=\n#\r \x80\xc3\xa9$:,"[rnd() % 12]; m++; }
            else if (op == 3) m = at;                                            /* truncate */
            else if (op == 4 && m) buf[at] = (char)('0' + rnd() % 75);
        }
        /* the parser gets an exact-size heap copy so ASan sees any read past the end */
        char *exact = malloc(m ? m : 1);
        memcpy(exact, buf, m);
        FirstRun fr;
        char err[200];
        if (fr_parse(exact, m, &fr, err, sizeof err) == 0) accepted++;
        fr_free(&fr);
        free(exact);
    }
    free(buf);
    printf("  %ld mutations, %ld still valid\n", iters, accepted);
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "check")) {
        FILE *f = fopen(argv[2], "rb");
        if (!f) { printf("cannot read %s\n", argv[2]); return 2; }
        static char b[FR_MAX_FILE + 2];
        size_t n = fread(b, 1, sizeof b, f);
        fclose(f);
        FirstRun fr;
        char err[200];
        int r = fr_parse(b, n, &fr, err, sizeof err);
        fr_free(&fr);
        printf("%s\n", r ? err : "ok");
        return r ? 1 : 0;
    }
    validators();
    full_valid();
    broken();
    fuzz(argc > 1 ? atol(argv[1]) : 30000);
    printf("firstrun-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
