/* webwin.h — lord-web's Wayland window (see web.h). */
#ifndef LORD_WEB_WEBWIN_H
#define LORD_WEB_WEBWIN_H

#include "web.h"

/* Connects to the session's compositor and reads its globals. NULL when there is no display or no xdg-shell. */
WebWin *webwin_new(Web *w);
/* After webwin_new, before any view exists: starts the backend for this window (webback.h; dmabuf with EGL when the
 * compositor offers linux-dmabuf and EGL works, shared memory otherwise: the CPU path) and sets w->back. */
int webwin_init_backend(WebWin *win);
/* Once WebKit's view exists: input, the input method, the clipboard */
void webwin_attach(WebWin *win);
void webwin_show(WebWin *win, const char *activation_token);
void webwin_hide(WebWin *win);
int webwin_visible(WebWin *win);
void webwin_set_title(WebWin *win, const char *title);
void webwin_set_cursor_hint(WebWin *win, int link, int text);
/* the person pressed a key, clicked or touched the window less than a few seconds ago and it has the keyboard: what a
 * page may write to the clipboard needs it */
int webwin_user_active(WebWin *win);
/* the page copied: offer this text (mime NULL) or image (image/png, image/jpeg) to the session; data taken over */
void webwin_offer(WebWin *win, const char *mime, GBytes *data);
/* the page copied text (and its HTML, may be NULL) through a copy event it handled itself: offer it to the session;
 * both taken over */
void webwin_offer_text(WebWin *win, char *text, char *html);
/* a short diagnostic line in lord-web's log (stderr), at most a few a second: never what was copied or dropped */
void web_note(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
/* an editing command was run on the page (copy, cut): what it put in WebKit's pasteboard goes to the session */
void webwin_after_edit(WebWin *win);
/* Ctrl+<letter> (Shift too when shift) to the page as from the keyboard; 0 when the keymap has no such key */
int webwin_send_shortcut(WebWin *win, uint32_t keysym, int shift);
/* the person pastes (the Edit menu, the context menu; the keys are webwin.c's own): the session's file list, its image
 * when it has no text, or a text WebKit would garble, pasted into the page by lord-web (webxfer.c); 1 when it was,
 * 0 for WebKit's own Paste */
int webwin_paste(WebWin *win);
/* Paste as Plain Text (the context menu, Ctrl+Shift+V): the session's text only, as text/plain, never its HTML, files
 * or image */
void webwin_paste_plain(WebWin *win);
/* the page's selection (UTF-8, the person's own: after a click or key in the window) as the session's primary
 * selection, as a browser on Linux does: what LordOS's Speech (❖Esc, Edit → Speech, the context menu's Speech) reads
 * aloud, and a middle click pastes. Never a password field's (webselect.c doesn't report those). */
void webwin_set_primary(WebWin *win, const char *text);
void webwin_free(WebWin *win);

#endif
