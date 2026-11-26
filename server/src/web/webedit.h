/* webedit.h — the two closed lists lord-server and lord-web share for editing a web app's page (docs/WEBAPPS.md,
 * "Editing"), both pure (tests/webapp_test.c):
 *
 *   - the commands of the shell's Edit menu, which lord-server asks of the lord-web it started:
 *       org.lordos.WebApp1.Edit(s command)   on lord-web, only from lord-server's own process (pid checked)
 *     undo, redo, cut, copy, paste, selectAll → WebKit's editing commands Undo, Redo, Cut, Copy, Paste, SelectAll;
 *   - the items of a page's context menu, which lord-web asks lord-server to show at the pointer:
 *       org.lordos.WebApps1.Menu(s app, s profile, as items, u checked, s text) → s item   on lord-server, only from
 *       that lord-web (the older Menu(s, s, as) is still taken: no check marks, no text)
 *     The items are names from this list, never text from the page; checked is a mask of the items' positions that
 *     show a check mark; text is the selection's few words for "Search the web for “…”" (websearch.h, cleaned on both
 *     sides). lord-server shows its own words for the items and answers the one the person chose, or "" when the
 *     menu closed without a choice. */
#ifndef LORD_WEB_WEBEDIT_H
#define LORD_WEB_WEBEDIT_H

#include <stddef.h>

enum { WEBEDIT_UNDO, WEBEDIT_REDO, WEBEDIT_CUT, WEBEDIT_COPY, WEBEDIT_PASTE, WEBEDIT_SELECT_ALL, WEBEDIT_N };

int         webedit_command(const char *name);      /* the command named so, or -1 */
const char *webedit_name(int cmd);                  /* its name, or NULL */
const char *webedit_webkit(int cmd);                /* WebKit's editing command for it ("SelectAll"), or NULL */

/* The context menu's items, a closed list. Its words are LordOS's: the one piece of the page a menu shows is the few
 * cleaned words of the selection in "Search the web for “…”" (websearch.h). Items the shell runs itself, never sent
 * back to lord-web: emoji (LordOS's emoji picker), speech (LordOS's Speech submenu, which reads the selection lord-web
 * made the session's primary selection) and inspect (developer tools, only while developer mode is on). */
enum {
    WEBMENU_SPELL_1, WEBMENU_SPELL_2, WEBMENU_SPELL_3, WEBMENU_SPELL_4, WEBMENU_SPELL_5, WEBMENU_IGNORE_SPELLING, WEBMENU_LEARN_SPELLING,
    WEBMENU_UNDO, WEBMENU_REDO, WEBMENU_CUT, WEBMENU_COPY, WEBMENU_PASTE, WEBMENU_PASTE_PLAIN, WEBMENU_SELECT_ALL, WEBMENU_EMOJI,
    WEBMENU_COPY_HIGHLIGHT, WEBMENU_SEARCH, WEBMENU_SPEECH,
    WEBMENU_OPEN_LINK, WEBMENU_COPY_LINK,
    WEBMENU_OPEN_IMAGE, WEBMENU_SAVE_IMAGE, WEBMENU_COPY_IMAGE, WEBMENU_COPY_IMAGE_ADDRESS,
    WEBMENU_PLAY, WEBMENU_PAUSE, WEBMENU_MUTE, WEBMENU_UNMUTE, WEBMENU_LOOP, WEBMENU_CONTROLS, WEBMENU_SAVE_VIDEO, WEBMENU_SAVE_AUDIO,
    WEBMENU_BACK, WEBMENU_FORWARD, WEBMENU_RELOAD,
    WEBMENU_INSPECT,
    WEBMENU_N
};

/* what was under the pointer (the page's right click, webselect.c) */
#define WEBMENU_AT_EDITABLE    (1u << 0)
#define WEBMENU_AT_SELECTION   (1u << 1)
#define WEBMENU_AT_LINK        (1u << 2)    /* a web, mail or phone link */
#define WEBMENU_AT_IMAGE       (1u << 3)    /* an image the page can read again (Copy Image) */
#define WEBMENU_AT_MEDIA       (1u << 4)    /* a video or audio element (in the top frame) */
#define WEBMENU_AT_PAUSED      (1u << 5)    /*   … paused */
#define WEBMENU_AT_MUTED       (1u << 6)    /*   … muted */
#define WEBMENU_AT_LOOP        (1u << 7)    /*   … looping */
#define WEBMENU_AT_CONTROLS    (1u << 8)    /*   … showing its controls */
#define WEBMENU_AT_MEDIA_SAVE  (1u << 9)    /*   … whose source is an http(s) address (not a stream the page feeds it) */
#define WEBMENU_AT_SEARCH      (1u << 10)   /* the selection has words to search for and to show */
#define WEBMENU_AT_HIGHLIGHT   (1u << 11)   /* the selection, outside a field, on an http(s) page: a link to it */
#define WEBMENU_AT_BACK        (1u << 12)   /* the view can go back */
#define WEBMENU_AT_FORWARD     (1u << 13)   /*   … forward */
#define WEBMENU_AT_IMAGE_WEB   (1u << 14)   /* the image's address is http(s): it can be opened in the browser */
#define WEBMENU_AT_IMAGE_SAVE  (1u << 15)   /* the image can be saved (an http(s) or data: address, or the page's own blob) */
#define WEBMENU_AT_VIDEO       (1u << 16)
#define WEBMENU_AT_GUESSES(n)  ((unsigned)((n) & 7) << 17)   /* WebKit's spelling suggestions for the word (0..5) */
#define WEBMENU_GUESSES_OF(at) ((int)(((at) >> 17) & 7) > WEBMENU_GUESSES_MAX ? WEBMENU_GUESSES_MAX : (int)(((at) >> 17) & 7))
#define WEBMENU_AT_IGNORE      (1u << 20)   /* WebKit offers Ignore Spelling */
#define WEBMENU_AT_LEARN       (1u << 21)   /* WebKit offers Learn Spelling (Add to Dictionary) */
#define WEBMENU_GUESSES_MAX    5   /* the media element is a video (Save Video As…), else audio (Save Audio As…) */

int         webmenu_item(const char *name);         /* the item named so, or -1 */
const char *webmenu_name(int item);                 /* its name, or NULL */
/* Items the shell runs itself (emoji, speech, inspect): lord-web never acts on them. */
int         webmenu_shell_only(int item);
/* The items for what is under the pointer, in the menu's order (Chrome's):
 *   an editable field: WebKit's spelling suggestions for the word, Ignore Spelling, Add to Dictionary, then Undo, Redo, Cut and Copy (with a selection), Paste, Paste as Plain Text, Select All, Emoji;
 *   a selection: Copy (outside a field), Copy Link to Highlight, Search the web, Speech;
 *   a link: Open Link in Browser, Copy Link Address;
 *   an image: Open Image in Browser, Save Image As…, Copy Image, Copy Image Address;
 *   a video or audio: Play or Pause, Mute or Unmute, Loop, Show Controls, Save Video As… (Save Audio As…);
 *   none of these (the page itself): Back, Forward (when the view can), Reload, Select All;
 *   and Inspect last, always (the shell shows it only while developer mode is on).
 * The count (at most WEBMENU_N). */
int         webmenu_items(unsigned at, int out[WEBMENU_N]);
/* Which of items (n of them) show a check mark: Loop and Show Controls when they are on. A mask of the positions. */
unsigned    webmenu_checked(unsigned at, const int *items, int n);
/* A Menu call's item list, checked: 1..WEBMENU_N known names, none twice. The count and the items in out, or -1. */
int         webmenu_parse(const char *const *names, int n, int out[WEBMENU_N]);

/* The selection a page's menu carries for the shell's own items (docs/AI.md "Writing", docs/VOICE.md): ✨ AI (which takes
 * at most 4,000 characters of it) and Speech (which reads it aloud without the primary selection), as both lord-web
 * and lord-server clean it. in (at most WEBAI_MAX bytes of well-formed UTF-8, at most WEBAI_CHARS characters) into out
 * (WEBAI_MAX + 1 bytes) without C0 controls but line breaks and tabs, DEL, C1 controls or bidi controls (U+200E/F,
 * U+202A..E, U+2066..9); 0, or -1 (empty, too long, not UTF-8: nothing is offered). */
#define WEBAI_CHARS 16384          /* the selection lord-web keeps for a menu (websearch.h WEBSEARCH_SELECTION_MAX) */
#define WEBAI_MAX   65536
int webai_clean(const char *in, char *out);

#endif
