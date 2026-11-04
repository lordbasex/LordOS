/* homedirs_test.c — the user's common folders (src/users/homedirs.c): each language's names,
 * user-dirs.dirs and user-dirs.locale, a second run overwrites nothing, and symlinks are not
 * followed. Runs without root: the owner is the current user. */
#include "users/homedirs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int is_dir(const char *base, const char *name) {
    char p[512];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s", base, name);
    return !lstat(p, &st) && S_ISDIR(st.st_mode);
}

static char *slurp(const char *base, const char *name) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", base, name);
    FILE *f = fopen(p, "r");
    if (!f) return NULL;
    static char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    return buf;
}

static void lang(const char *locale, const char *const want[6], const char *xdg_doc, const char *loc) {
    char home[] = "/tmp/homedirs-test-XXXXXX";
    if (!mkdtemp(home)) { perror("mkdtemp"); exit(1); }
    CHECK(homedirs_create(home, getuid(), getgid(), locale) == 0, "%s: homedirs_create", locale);
    for (int i = 0; i < 6; i++) {
        CHECK(is_dir(home, want[i]), "%s: missing %s", locale, want[i]);
        CHECK(!strcmp(homedirs_name(homedirs_ui_lang(locale), i), want[i]), "%s: name %d", locale, i);
    }
    char *d = slurp(home, ".config/user-dirs.dirs");
    CHECK(d && strstr(d, xdg_doc), "%s: user-dirs.dirs lacks %s", locale, xdg_doc);
    CHECK(d && strstr(d, "XDG_TEMPLATES_DIR=\"$HOME/\""), "%s: templates", locale);
    char *l = slurp(home, ".config/user-dirs.locale");
    CHECK(l && !strncmp(l, loc, strlen(loc)), "%s: user-dirs.locale = %s", locale, l ? l : "(none)");
    /* a second run: succeeds and keeps what the user changed */
    char p[512];
    snprintf(p, sizeof p, "%s/.config/user-dirs.dirs", home);
    FILE *f = fopen(p, "w");
    if (f) { fputs("XDG_DOCUMENTS_DIR=\"$HOME/Mis cosas\"\n", f); fclose(f); }
    CHECK(homedirs_create(home, getuid(), getgid(), locale) == 0, "%s: second run", locale);
    d = slurp(home, ".config/user-dirs.dirs");
    CHECK(d && strstr(d, "Mis cosas"), "%s: overwrote user-dirs.dirs", locale);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", home);
    if (system(cmd)) { /* ignored */ }
}

int main(void) {
    static const char *const es[6] = { "Escritorio", "Documentos", "Descargas", "Música", "Imágenes", "Videos" };
    static const char *const en[6] = { "Desktop", "Documents", "Downloads", "Music", "Pictures", "Videos" };
    static const char *const pt[6] = { "Área de trabalho", "Documentos", "Downloads", "Música", "Imagens", "Vídeos" };
    static const char *const fr[6] = { "Bureau", "Documents", "Téléchargements", "Musique", "Images", "Vidéos" };
    static const char *const it[6] = { "Scrivania", "Documenti", "Scaricati", "Musica", "Immagini", "Video" };
    lang("es_AR.UTF-8", es, "XDG_DOCUMENTS_DIR=\"$HOME/Documentos\"", "es_AR");
    lang("en_US.UTF-8", en, "XDG_DOWNLOAD_DIR=\"$HOME/Downloads\"", "en_US");
    lang("pt_BR.UTF-8", pt, "XDG_DESKTOP_DIR=\"$HOME/Área de trabalho\"", "pt_BR");
    lang("fr_FR.UTF-8", fr, "XDG_DESKTOP_DIR=\"$HOME/Bureau\"", "fr_FR");
    lang("it_IT.UTF-8", it, "XDG_DOWNLOAD_DIR=\"$HOME/Scaricati\"", "it_IT");
    lang("C.UTF-8", en, "XDG_PICTURES_DIR=\"$HOME/Pictures\"", "en_US");    /* no language: English */

    /* ~/.config is a symlink elsewhere: nothing is written through it */
    char home[] = "/tmp/homedirs-test-XXXXXX", other[] = "/tmp/homedirs-other-XXXXXX";
    if (!mkdtemp(home) || !mkdtemp(other)) return 1;
    char link[512];
    snprintf(link, sizeof link, "%s/.config", home);
    if (symlink(other, link)) return 1;
    CHECK(homedirs_create(home, getuid(), getgid(), "es_ES.UTF-8") != 0, "symlink: should fail");
    CHECK(!slurp(other, "user-dirs.dirs"), "symlink: wrote through ~/.config");
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' '%s'", home, other);
    if (system(cmd)) { /* ignored */ }

    printf("homedirs: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
