/* homedirs.h — the common home folders in the user's language (Escritorio, Documentos, Descargas,
 * Música, Imágenes, Videos in Spanish) and ~/.config/user-dirs.dirs (xdg-user-dirs). One list for
 * the installer (install.c) and Settings → Users (users_admin.c); it matches lordos-live-prep in
 * the live ISO. Called as root. */
#ifndef LORD_HOMEDIRS_H
#define LORD_HOMEDIRS_H

#include <sys/types.h>

/* UI language for a locale ("es_AR.UTF-8" → "es"): es, en, pt or fr (English otherwise). */
const char *homedirs_ui_lang(const char *locale);
/* Visible name of each folder (0..5: desktop, documents, downloads, music, pictures, videos). */
const char *homedirs_name(const char *ui_lang, int i);
#define HOMEDIRS_COUNT 6
/* Creates the missing folders in home (owner uid:gid, 0755) and writes ~/.config/user-dirs.dirs and
 * ~/.config/user-dirs.locale if they don't exist. Never follows symlinks. 0 on success. */
int homedirs_create(const char *home, uid_t uid, gid_t gid, const char *locale);

#endif
