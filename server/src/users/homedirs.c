/* homedirs.c — the user's common folders and xdg-user-dirs (see homedirs.h). */
#include "homedirs.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The same list as tools/iso/rootfs/usr/lib/lordos/lordos-live-prep (there with _ for spaces). */
static const char *const names[5][HOMEDIRS_COUNT] = {
    { "Escritorio", "Documentos", "Descargas", "Música", "Imágenes", "Videos" },   /* i18n-ok */
    { "Desktop", "Documents", "Downloads", "Music", "Pictures", "Videos" },
    { "Área de trabalho", "Documentos", "Downloads", "Música", "Imagens", "Vídeos" },   /* i18n-ok */
    { "Bureau", "Documents", "Téléchargements", "Musique", "Images", "Vidéos" },   /* i18n-ok */
    { "Scrivania", "Documenti", "Scaricati", "Musica", "Immagini", "Video" },   /* i18n-ok */
};
static const char *const xdg_keys[HOMEDIRS_COUNT] = { "DESKTOP", "DOCUMENTS", "DOWNLOAD", "MUSIC", "PICTURES", "VIDEOS" };

const char *homedirs_ui_lang(const char *locale) {
    if (locale && !strncmp(locale, "es", 2)) return "es";
    if (locale && !strncmp(locale, "pt", 2)) return "pt";
    if (locale && !strncmp(locale, "fr", 2)) return "fr";
    if (locale && !strncmp(locale, "it", 2)) return "it";
    return "en";                        /* English is the default language */
}

static int lang_index(const char *ui) { return !strcmp(ui, "es") ? 0 : !strcmp(ui, "pt") ? 2 : !strcmp(ui, "fr") ? 3 : !strcmp(ui, "it") ? 4 : 1; }

const char *homedirs_name(const char *ui_lang, int i) {
    if (i < 0 || i >= HOMEDIRS_COUNT) return NULL;
    return names[lang_index(ui_lang ? ui_lang : "en")][i];
}

/* New file (O_EXCL, no symlinks followed) relative to the open directory dfd. */
static int write_new(int dfd, const char *name, const char *text, uid_t uid, gid_t gid) {
    int fd = openat(dfd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return errno == EEXIST ? 0 : -1;
    size_t n = strlen(text);
    int ok = write(fd, text, n) == (ssize_t)n;
    if (fchown(fd, uid, gid)) ok = 0;
    if (close(fd)) ok = 0;
    return ok ? 0 : -1;
}

/* New owned subdirectory of dfd; returns it opened (no symlinks followed) or -1. */
static int mkdir_owned(int dfd, const char *name, uid_t uid, gid_t gid) {
    if (mkdirat(dfd, name, 0755) == 0) {
        if (fchownat(dfd, name, uid, gid, AT_SYMLINK_NOFOLLOW)) return -1;
    } else if (errno != EEXIST) return -1;
    return openat(dfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

int homedirs_create(const char *home, uid_t uid, gid_t gid, const char *locale) {
    int bad = 0;
    int hfd = open(home, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (hfd < 0) return -1;
    const char *ui = homedirs_ui_lang(locale);
    int li = lang_index(ui);
    for (int i = 0; i < HOMEDIRS_COUNT; i++) {
        int d = mkdir_owned(hfd, names[li][i], uid, gid);
        if (d < 0) bad = -1; else close(d);
    }
    int cfd = mkdir_owned(hfd, ".config", uid, gid);
    if (cfd >= 0) {
        char text[1024];
        size_t o = (size_t)snprintf(text, sizeof text,
            "# User folders (xdg-user-dirs). Written by LordOS when the account was created.\n");
        for (int i = 0; i < HOMEDIRS_COUNT && o < sizeof text; i++)
            o += (size_t)snprintf(text + o, sizeof text - o, "XDG_%s_DIR=\"$HOME/%s\"\n", xdg_keys[i], names[li][i]);
        if (o < sizeof text) snprintf(text + o, sizeof text - o, "XDG_TEMPLATES_DIR=\"$HOME/\"\nXDG_PUBLICSHARE_DIR=\"$HOME/\"\n");
        if (write_new(cfd, "user-dirs.dirs", text, uid, gid)) bad = -1;
        /* the language they were named in (xdg-user-dirs-update compares it when the language changes) */
        char loc[64];
        snprintf(loc, sizeof loc, "%s", li == 1 ? "en_US" : li == 2 ? "pt_BR" : li == 3 ? "fr_FR" : li == 4 ? "it_IT" : "es_ES");
        if (locale && !strncmp(locale, ui, 2)) { snprintf(loc, sizeof loc, "%s", locale); loc[strcspn(loc, ".@")] = 0; }
        snprintf(text, sizeof text, "%s\n", loc);
        if (write_new(cfd, "user-dirs.locale", text, uid, gid)) bad = -1;
        close(cfd);
    } else bad = -1;
    close(hfd);
    return bad;
}
