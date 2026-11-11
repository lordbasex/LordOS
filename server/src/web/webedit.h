/* webedit.h — the two closed lists lord-server and lord-web share for editing a web app's page (docs/WEBAPPS.md,
 * "Editing"), both pure (tests/webapp_test.c):
 *
 *   - the commands of the shell's Edit menu, which lord-server asks of the lord-web it started:
 *       org.lordos.WebApp1.Edit(s command)   on lord-web, only from lord-server's own process (pid checked)
 *     undo, redo, cut, copy, paste, selectAll → WebKit's editing commands Undo, Redo, Cut, Copy, Paste, SelectAll;
 *   - the items of a page's context menu, which lord-web asks lord-server to show at the pointer:
 *       org.lordos.WebApps1.Menu(s app, s profile, as items) → s item   on lord-server, only from that lord-web
 *     The items are names from this list, never text from the page; lord-server shows its own words for them and
 *     answers the one the person chose, or "" when the menu closed without a choice. */
#ifndef LORD_WEB_WEBEDIT_H
#define LORD_WEB_WEBEDIT_H

#include <stddef.h>

enum { WEBEDIT_UNDO, WEBEDIT_REDO, WEBEDIT_CUT, WEBEDIT_COPY, WEBEDIT_PASTE, WEBEDIT_SELECT_ALL, WEBEDIT_N };

int         webedit_command(const char *name);      /* the command named so, or -1 */
const char *webedit_name(int cmd);                  /* its name, or NULL */
const char *webedit_webkit(int cmd);                /* WebKit's editing command for it ("SelectAll"), or NULL */

enum { WEBMENU_CUT, WEBMENU_COPY, WEBMENU_PASTE, WEBMENU_SELECT_ALL, WEBMENU_COPY_LINK, WEBMENU_OPEN_LINK, WEBMENU_COPY_IMAGE, WEBMENU_N };

/* what was under the pointer (WebKit's hit test) */
#define WEBMENU_AT_EDITABLE  1u
#define WEBMENU_AT_SELECTION 2u
#define WEBMENU_AT_LINK      4u
#define WEBMENU_AT_IMAGE     8u

int         webmenu_item(const char *name);         /* the item named so, or -1 */
const char *webmenu_name(int item);                 /* its name, or NULL */
/* The items for what is under the pointer, in the menu's order: Cut and Copy with a selection in an editable field,
 * Copy with a selection elsewhere, Paste in an editable field, Select All always; Copy Link Address and Open Link in
 * Browser on a link; Copy Image on an image. The count (at most WEBMENU_N). */
int         webmenu_items(unsigned at, int out[WEBMENU_N]);
/* A Menu call's item list, checked: 1..WEBMENU_N known names, none twice. The count and the items in out, or -1. */
int         webmenu_parse(const char *const *names, int n, int out[WEBMENU_N]);

#endif
