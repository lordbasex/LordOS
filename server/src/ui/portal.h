/* portal.h — our own org.freedesktop.portal.Settings, FileChooser, OpenURI, Inhibit, GlobalShortcuts and
 * ScreenCast, and org.freedesktop.ScreenSaver, on the session bus (see portal.c). */
#ifndef LORD_PORTAL_H
#define LORD_PORTAL_H

#include <stddef.h>
#include <stdint.h>
#include "../cast/cast.h"

int  portal_start(void);                /* after dropping privileges; 0 if it owns the name */
int  portal_fd(void);                   /* for poll; -1 if absent */
int  portal_wants_write(void);          /* there are unsent messages (POLLOUT) */
void portal_dispatch(void);
/* The answer to a file dialog: n paths, or 0 if it was cancelled. */
void portal_file_chosen(int id, const char *const *paths, int n);
/* The shell's answer to an OpenURI request (lord.portal.opened): 0 opened, 1 cancelled, 2 refused. */
void portal_open_done(int id, int code);
/* Right before the shell opens what request id named (lord.portal.still): 1 when a local file is still the
 * one checked when the app asked (or it is a web address), 0 otherwise. */
int  portal_open_still(int id);
/* An app holds idle or sleep off (Inhibit portal, ScreenSaver.Inhibit): counts as wl_idle_inhibited. */
int  portal_inhibited(void);
/* A key before anything else gets it (js_key): 1 when it is an app's global shortcut and went to that app
 * (Activated on press, Deactivated on release; repeats taken), so nothing else may have it. letter:
 * keygrab_letter; mods: KG_* (wl/keygrab.h). */
int  portal_shortcut_key(int code, int value, char letter, unsigned mods);
/* While the shell records a new combination (the sheet, Settings) no global shortcut fires. */
void portal_shortcuts_pause(int on);
/* The person's answer to session id's sheet (lord.portal.shortcutsChosen): n shortcut ids with a trigger
 * each ("" unassigned), or n < 0 cancelled. A trigger LordOS keeps or someone else has is left unassigned. */
void portal_shortcuts_chosen(int id, const char *const *ids, const char *const *triggers, int n);
/* Every kept decision as JSON, [{app, id, description, trigger}] (malloc, or NULL) */
char *portal_shortcuts_list(void);
/* What a trigger would be for app's (or session's) shortcut id: the formatted form in out, why LordOS
 * keeps it (GS_* of gshort.h) in kept and who else has it in owner. 0, or -1 when it is not a trigger. */
int  portal_shortcut_check(const char *trigger, int session, const char *app, const char *id, char *out, size_t n,
                           int *kept, char *owner, size_t on);
/* Settings: app's shortcut id gets trigger ("" leaves it unassigned). 0, or -1 when refused. */
int  portal_shortcut_set(const char *app, const char *id, const char *trigger);
/* Settings: every decision for app forgotten, its live sessions unbound. How many there were. */
int  portal_shortcuts_forget(const char *app);
/* ScreenCast. From the shell: the person's picks in the chooser (n = 0: Cancel), the indicator on screen, its Stop, and a
 * picture of a source for the chooser while it is up (a media id, 0 none). */
void portal_cast_chosen(int id, const CastSource *src, int n);
void portal_cast_shown(int id);
void portal_cast_stop(int id);
int  portal_cast_thumb(int id, const CastSource *src);
/* From cast.c: the streams are there, they couldn't be made, a running session ended (the source went, the helper
 * failed, a lock), and the app's restricted connection (fd, or -1; the portal closes it). */
void portal_cast_streams(int id, const CastStreamInfo *info, int n);
void portal_cast_failed(int id);
void portal_cast_lost(int id);
void portal_cast_remote(int id, uint32_t req, int fd);
/* From JS (lord.portal.set): mode, accent (0..1), GNOME accent name and GTK button order. */
void portal_set(int dark, const double rgb[3], const char *accent_name, const char *button_layout);

#endif
