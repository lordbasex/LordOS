/* gshort.h — the rules behind the global shortcuts portal (portal.c) that need no D-Bus: a trigger's
 * text, which combinations an app may have, the states of a session and the bindings remembered per app
 * (see gshort.c and docs/SHORTCUTS.md, "Linux apps' global shortcuts"). */
#ifndef LORD_GSHORT_H
#define LORD_GSHORT_H

#include <stddef.h>

/* A combination: KG_* modifiers (wl/keygrab.h) and a letter 'a'..'z' (by what the layout types, as
 * keygrab_letter) or, for any other key, its evdev code (by position). */
typedef struct { unsigned char mods; char letter; unsigned short code; } GsKey;

/* A trigger as the spec writes it ("CTRL+SHIFT+a", "LOGO+space", "ALT+F9") or as sdk/shortcuts.js does
 * ("Ctrl+Shift+A"): modifiers and one key, joined by '+', in any case. 0 ok, -1 not one LordOS takes
 * (unknown key or modifier, a modifier twice, NUM, nothing after the last '+', over 63 bytes). */
int  gs_parse(const char *text, GsKey *k);
/* The form sdk/shortcuts.js reads, modifiers in the order Ctrl, Alt, Shift, Meta: "Ctrl+Shift+A", "F9" */
void gs_format(const GsKey *k, char *out, size_t n);
int  gs_same(const GsKey *a, const GsKey *b);
/* This key, pressed with exactly these modifiers, is k */
int  gs_match(const GsKey *k, int code, char letter, unsigned mods);

/* Why LordOS keeps a combination from apps, or GS_FREE. GS_LOGO: every ❖ combination is the desktop's.
 * GS_SYSTEM: the media row, Print Screen, Tab and Esc (the switcher, menus, ending a share), ⌃⌥ with
 * F1..F12, Backspace or Delete. GS_TYPING: with neither Ctrl nor Alt only F1..F24, Pause and Scroll Lock,
 * so a binding never takes a key used to type or move. GS_THEME: in the theme's table of kept keys
 * (keygrab_match). The shell's own shortcuts (sdk/shortcuts.js) are checked by the shell. */
enum { GS_FREE, GS_LOGO, GS_SYSTEM, GS_TYPING, GS_THEME };
int  gs_kept(const GsKey *k);

/* A session's life. NEW after CreateSession; BindShortcuts once (ASKING while the sheet is up; cancelled,
 * it may ask again); BOUND once answered, the only state that gets key events; CLOSED by Session.Close or
 * when its app leaves the bus, after which nothing is accepted or sent. */
enum { GSS_NONE, GSS_NEW, GSS_ASKING, GSS_BOUND, GSS_CLOSED };
enum { GSO_CREATE, GSO_BIND, GSO_ANSWER, GSO_CANCEL, GSO_LIST, GSO_KEY, GSO_CLOSE };
/* The state after op, or -1 when op is not allowed in state (the state does not change) */
int  gs_step(int state, int op);

/* An id an app gives a shortcut: 1 to 63 printable ASCII bytes. 1 yes. */
int  gs_id_valid(const char *id);
/* A description cut to n - 1 bytes on a character boundary, control characters left out */
void gs_text(char *out, size_t n, const char *in);

/* ---- what the person decided, per app: ~/.config/lordos/global-shortcuts.json ----
 * {"apps":{"desktop:<id>":{"<shortcut id>":{"description":"…","trigger":"Ctrl+Shift+R"},…},…}}
 * An empty trigger is a shortcut the person left unassigned: decided too, so the app is not asked again. */
#define GS_MAX_ENTRIES 256
#define GS_APP_MAX     128
#define GS_ID_MAX      64
#define GS_DESC_MAX    128
typedef struct { char app[GS_APP_MAX], id[GS_ID_MAX], desc[GS_DESC_MAX]; GsKey key; int bound; } GsEntry;
typedef struct { GsEntry e[GS_MAX_ENTRIES]; int n; } GsStore;

/* Reads the file's text into s (emptied first). Entries that are not valid are left out, and so is a
 * trigger LordOS keeps (gs_kept) or a second app's binding of the same combination. 0 ok, -1 not JSON. */
int  gs_store_load(GsStore *s, const char *text, size_t len);
/* The file's text (malloc), or NULL on out-of-memory */
char *gs_store_json(const GsStore *s);
GsEntry *gs_store_find(GsStore *s, const char *app, const char *id);
/* Sets app's decision for id (bound with key, or unassigned when key is NULL); a new entry when there was
 * none. NULL when the store is full. The caller has checked the combination (gs_kept, gs_store_owner). */
GsEntry *gs_store_set(GsStore *s, const char *app, const char *id, const char *desc, const GsKey *key);
/* Forgets every decision for app; how many there were */
int  gs_store_forget(GsStore *s, const char *app);
/* The entry of another app, or of another shortcut of the same app, bound to k; NULL when none */
const GsEntry *gs_store_owner(const GsStore *s, const GsKey *k, const char *app, const char *id);

#endif
