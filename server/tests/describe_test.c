/* describe_test.c — monitor/describe.c: Debian's Description-md5, an installed package's description and its
 * translation (only the entry whose MD5 matches), the language codes apt and man use, a manual page's NAME summary (man
 * and mdoc, a translated page first), and a systemd unit's Description= (a template too). Made-up files in a temporary
 * folder; the machine's own pages and database only when present (read-only). */
#include "../src/monitor/describe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void put(const char *path, const char *s) {
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(1); }
    fputs(s, f);
    fclose(f);
    chmod(path, 0644); /* describe.c refuses group- or world-writable files; a umask of 002 would make them so */
}

int main(void) {
    char hex[33];
    desc_md5_hex("", 0, hex);
    CHECK(!strcmp(hex, "d41d8cd98f00b204e9800998ecf8427e"), "md5 of nothing %s", hex);
    desc_md5_hex("The quick brown fox jumps over the lazy dog", 43, hex);
    CHECK(!strcmp(hex, "9e107d9d372bb6826bd81d3542a419d6"), "md5 of the fox %s", hex);

    char codes[3][16];
    CHECK(desc_lang_codes("en", codes) == 0, "English needs no translation");
    CHECK(desc_lang_codes("es", codes) == 1 && !strcmp(codes[0], "es"), "es");
    CHECK(desc_lang_codes("pt", codes) == 2 && !strcmp(codes[0], "pt_BR") && !strcmp(codes[1], "pt"), "pt: Brazil first");
    CHECK(desc_lang_codes("zh-Hans", codes) == 1 && !strcmp(codes[0], "zh_CN"), "zh-Hans");
    CHECK(desc_lang_codes("zh-Hant", codes) == 2 && !strcmp(codes[0], "zh_TW"), "zh-Hant");
    CHECK(desc_lang_codes("../x", codes) == 0, "not a language");

    char dir[] = "/tmp/desc-test.XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    char status[256], lists[256], p[512];
    snprintf(status, sizeof status, "%s/status", dir);
    snprintf(lists, sizeof lists, "%s/lists", dir);
    mkdir(lists, 0755);
    /* bash's real description, whose MD5 is Debian's 3522aa7b4374048d6450e348a5bb45d9 */
    put(status,
        "Package: removed\nStatus: deinstall ok config-files\nDescription: an old one\n\n"
        "Package: bash\nEssential: yes\nStatus: install ok installed\nPriority: required\nVersion: 5.2.37-2\n"
        "Description: GNU Bourne Again SHell\n Bash is an sh-compatible command language interpreter that executes\n"
        " commands read from the standard input or from a file.  Bash also\n incorporates useful features from the Korn and C shells (ksh and csh).\n .\n"
        " Bash is ultimately intended to be a conformant implementation of the\n IEEE POSIX Shell and Tools specification (IEEE Working Group 1003.2).\n .\n"
        " The Programmable Completion Code, by Ian Macdonald, is now found in\n the bash-completion package.\nHomepage: http://tiswww.case.edu/php/chet/bash/bashtop.html\n\n"
        "Package: tool\nStatus: install ok installed\nDescription: a made-up tool\n It does things.\n");
    snprintf(p, sizeof p, "%s/deb.debian.org_debian_dists_trixie_main_i18n_Translation-es", lists);
    put(p, "Package: bash\nDescription-md5: 00000000000000000000000000000000\nDescription-es: una traducción vieja\n\n"
           "Package: bash\nDescription-md5: 3522aa7b4374048d6450e348a5bb45d9\nDescription-es: El intérprete de órdenes Bourne Again SHell de GNU\n"
           " Bash es un intérprete compatible con sh.\n .\n Segundo párrafo.\n\n"
           "Package: tool\nDescription-md5: 11111111111111111111111111111111\nDescription-es: no es esta versión\n");
    DescOut o;
    CHECK(!desc_pkg(status, lists, "bash", "en", &o) && !strcmp(o.text, "GNU Bourne Again SHell") && !strcmp(o.lang, "en"), "bash in English: %s", o.text);
    CHECK(strstr(o.lng, "sh-compatible command language interpreter") && strstr(o.lng, "\n\nBash is ultimately"), "its long description in paragraphs: %s", o.lng);
    CHECK(!desc_pkg(status, lists, "bash", "es", &o) && !strcmp(o.text, "El intérprete de órdenes Bourne Again SHell de GNU") && !strcmp(o.lang, "es"),
          "bash in Spanish, the entry whose MD5 matches: %s", o.text);
    CHECK(strstr(o.lng, "Bash es un intérprete") && strstr(o.lng, "\n\nSegundo párrafo."), "its long translation %s", o.lng);
    CHECK(!desc_pkg(status, lists, "tool", "es", &o) && !strcmp(o.text, "a made-up tool") && !strcmp(o.lang, "en"), "a translation of another version is not used: %s", o.text);
    CHECK(!desc_pkg(status, lists, "bash:amd64", "fr", &o) && !strcmp(o.lang, "en"), "no French list: English");
    CHECK(desc_pkg(status, lists, "removed", "en", &o) == -1, "a package that isn't installed");
    CHECK(desc_pkg(status, lists, "../etc", "en", &o) == -1, "not a package name");

    /* manual pages */
    char page[] = ".TH LS 1\n.SH NAME\nls \\- list directory contents\n.SH SYNOPSIS\n.B ls\n";
    char s[320];
    CHECK(!desc_man_summary(page, strlen(page), s, sizeof s) && !strcmp(s, "list directory contents"), "man: %s", s);
    char mdoc[] = ".Dd May 1\n.Dt CAT 1\n.Sh NAME\n.Nm cat\n.Nd concatenate and print files\n.Sh SYNOPSIS\n";
    CHECK(!desc_man_summary(mdoc, strlen(mdoc), s, sizeof s) && !strcmp(s, "concatenate and print files"), "mdoc: %s", s);
    char two[] = ".SH NOMBRE\nsleep \\- espera durante un\n.B tiempo\nespecificado\n.SH SINOPSIS\n";
    CHECK(!desc_man_summary(two, strlen(two), s, sizeof s) && !strcmp(s, "espera durante un tiempo especificado"), "a translated page, NAME on several lines: %s", s);
    char none[] = ".SH NAME\nfoo\n.SH SYNOPSIS\n";
    CHECK(desc_man_summary(none, strlen(none), s, sizeof s) == -1, "no summary");
    char man[256];
    snprintf(man, sizeof man, "%s/man", dir);
    mkdir(man, 0755);
    snprintf(p, sizeof p, "%s/man1", man); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/es", man); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/es/man1", man); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/man1/sleep.1", man); put(p, ".SH NAME\nsleep \\- delay for a specified amount of time\n");
    snprintf(p, sizeof p, "%s/es/man1/sleep.1", man); put(p, ".SH NOMBRE\nsleep \\- espera durante un tiempo especificado\n");
    snprintf(p, sizeof p, "%s/man1/vi.1", man); put(p, ".so man1/vim.1\n");
    snprintf(p, sizeof p, "%s/man1/vim.1", man); put(p, ".SH NAME\nvim \\- Vi IMproved, a programmer's text editor\n");
    desc_require_root = 0;
    CHECK(!desc_man(man, "sleep", "en", &o) && !strcmp(o.text, "delay for a specified amount of time") && !strcmp(o.lang, "en"), "sleep in English: %s", o.text);
    CHECK(!desc_man(man, "sleep", "es", &o) && !strcmp(o.text, "espera durante un tiempo especificado") && !strcmp(o.lang, "es"), "sleep in Spanish: %s", o.text);
    CHECK(!desc_man(man, "sleep", "de", &o) && !strcmp(o.lang, "en"), "no German page: English");
    CHECK(!desc_man(man, "vi", "en", &o) && !strcmp(o.text, "Vi IMproved, a programmer's text editor"), ".so followed once: %s", o.text);
    CHECK(desc_man(man, "../man1/sleep", "en", &o) == -1, "not a program name");
    desc_require_root = 1;
    CHECK(desc_man(man, "sleep", "en", &o) == -1, "a page not owned by root is not read");
    if (!access("/usr/share/man/man1/ls.1.gz", R_OK)) {
        CHECK(!desc_man("/usr/share/man", "ls", "en", &o) && strstr(o.text, "list directory contents"), "the system's ls page (gzip): %s", o.text);
    }

    /* systemd units */
    char units[256];
    snprintf(units, sizeof units, "%s/units", dir);
    mkdir(units, 0755);
    snprintf(p, sizeof p, "%s/demo.service", units); put(p, "[Unit]\nDescription=A made-up daemon\n\n[Service]\nDescription=not this one\n");
    snprintf(p, sizeof p, "%s/getty@.service", units); put(p, "# comment\n[Unit]\n  Description=Getty on %I\n");
    const char *dirs[] = { "/nonexistent", units, NULL };
    CHECK(!desc_unit(dirs, "demo.service", &o) && !strcmp(o.text, "A made-up daemon"), "unit: %s", o.text);
    CHECK(!desc_unit(dirs, "getty@tty1.service", &o) && !strcmp(o.text, "Getty on %I"), "template: %s", o.text);
    CHECK(desc_unit(dirs, "demo.conf", &o) == -1 && desc_unit(dirs, "../x.service", &o) == -1 && desc_unit(dirs, "none.service", &o) == -1, "refused or missing");

    /* the cache answers later, from its worker */
    char *json = NULL;
    desc_unit_dirs = dirs;
    int r = desc_lookup("unit", "demo.service", "en", &json);
    for (int i = 0; i < 200 && r < 0; i++) { usleep(10000); r = desc_lookup("unit", "demo.service", "en", &json); }
    CHECK(r == 1 && json && strstr(json, "\"text\":\"A made-up daemon\""), "lookup: %d %s", r, json ? json : "");
    free(json);
    CHECK(desc_lookup("exec", "x", "en", &json) == -1 && desc_lookup("man", "a/b", "en", &json) == -1, "kinds and arguments checked");

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd)) { /* left behind */ }
    printf("describe-test: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
