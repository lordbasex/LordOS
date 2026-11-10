/* apps.h — installed Linux apps (.desktop files) and their launcher (see apps.c). */
#ifndef LORD_APPS_H
#define LORD_APPS_H

#include <limits.h>
#include <stddef.h>

typedef struct {
    char id[160];                       /* "org.featherpad.FeatherPad" (.desktop name without the extension) */
    char name[160], generic[160], comment[320], keywords[320], categories[256];
    char name_c[160];                   /* the untranslated Name ("Chromium" next to "Navegador web Chromium") */
    char wmclass[160];                  /* StartupWMClass: to recognize its Wayland window */
    char mime[1024];                    /* MimeType: file types it opens ("text/plain;…") */
    char icon[PATH_MAX];                /* PNG found, or "" */
    char file[PATH_MAX];                /* the .desktop file */
    char bin[128];                      /* program name from Exec (for the Monitor: process → app) */
    int user;                           /* from ~/.local/share/applications */
} DesktopApp;

/* Lists the apps visible to LordOS (Type=Application, no NoDisplay/Hidden, OnlyShowIn and
 * NotShowIn, TryExec present, not terminal apps). lang = "es-AR": translated names.
 * Returns the count; *out is an internal array valid until the next call. */
int apps_list(const char *lang, const DesktopApp **out);

/* The PNG for a tray icon's name (IconName of a StatusNotifierItem): hicolor's status and apps folders, the user's
 * and the system's, at the bar's sizes first, then /usr/share/pixmaps. A name with a '/' or "..", or other characters
 * than [A-Za-z0-9._+-], finds nothing. out = "" when there is none. */
void apps_status_icon(const char *name, char *out, size_t n);

/* Launches the app with that id: only one from the list above, with its Exec line, without arguments
 * from the caller and without a shell. 0 if the process started. */
int apps_launch(const char *id);
/* Same, but opening a file or folder (see apps.c: only apps with MimeType and paths in the
 * home folder or the user's mounts, as a single argument). */
int apps_launch_with(const char *id, const char *path);
int apps_launch_uri(const char *id, const char *uri);    /* vnc://, rdp://, http(s):// for the app that declares the scheme */
#define APPS_URI_WEB_MAX 3000                                /* an http(s) address at most (the others: 300) */
/* The app that opens files of a MIME type, as apps_launch_with(id = "") picks it: out gets its desktop
 * id ("firefox-esr.desktop", or the user's "lordos-<app>.desktop"). Order: [Default Applications] of
 * the mimeapps.list files (the user's first, then the system's: /etc/xdg,
 * /usr/local/share/applications, /usr/share/applications, then defaults.list), then their [Added
 * Associations], then the installed apps whose MimeType declares the type, in mimeinfo.cache order
 * (~/.local/share/applications, then /usr/share/applications) and then by desktop id. Only apps this
 * launcher lists and can open a file with count; [Removed Associations] are honoured.
 * Returns APPS_DEFAULT_USER (the user's own [Default Applications] choice: "explicit"),
 * APPS_DEFAULT_LISTED (a system default or an added association: "automatic") or
 * APPS_DEFAULT_DECLARED (the first installed app that declares the type: "automatic"), or 0 if no
 * app opens it; n must be at least 160 (DESKTOP_ID_MAX in hw/mimeapps.h). */
#define APPS_DEFAULT_USER 1
#define APPS_DEFAULT_LISTED 2
#define APPS_DEFAULT_DECLARED 3
int apps_default_for(const char *mime, char *out, size_t n);
/* Desktop actions ("Actions=" + "[Desktop Action <name>]", e.g. a browser's "New Private Window"):
 * only those listed in Actions=, with a [A-Za-z0-9-] name, a Name and an Exec line. */
#define APPS_MAX_ACTIONS 16
#define APPS_ACTION_NAME_MAX 64
typedef struct {
    char name[APPS_ACTION_NAME_MAX];    /* the action's identifier */
    char label[160];                    /* its Name, translated for lang */
} AppAction;
/* Lists the desktop actions of the listed app with that id (up to max). Returns the count. */
int apps_actions(const char *id, const char *lang, AppAction *out, int max);
/* Launches one of them: the same launcher as apps_launch (same id rules, scope, restrictions, no
 * shell, no caller arguments), with that action's Exec line instead of the main one. The action
 * must be listed by apps_actions. 0 if the process started. */
int apps_launch_action(const char *id, const char *action);
/* The Name of the listed app whose .desktop entry runs this program, only to show it (the keyboard shortcuts
 * prompt and Settings, wl/kbinhibit.h): an entry counts only if its Exec line's program, found and resolved as
 * the launcher would, is exe (a resolved path); or, with flatpak_id, if it is "flatpak run" for that app id. hint
 * (a window's app_id, or NULL) only says which entries to look at first. out = "" if none runs it. */
void apps_name_of_program(const char *exe, const char *flatpak_id, const char *hint, char *out, size_t n);
/* Why the last apps_launch/apps_launch_with refused, as an error code ("" = no specific reason). */
const char *apps_last_error(void);

#endif
