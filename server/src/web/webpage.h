/* webpage.h — what lord-web decides about the page without WebKit (tests/webapp_test.c): the unread count in its
 * title, the user agent, whether LordOS plays its own sound for the page's notification, and lord-web's own state file
 * of the profile. docs/WEBAPPS.md, "The page". */
#ifndef LORD_WEB_WEBPAGE_H
#define LORD_WEB_WEBPAGE_H

#include <stddef.h>

#define WEBPAGE_BADGE_MAX 9999
/* The unread count a title announces: "(3) Chat" → 3, "(99+) Chat" → 99. N is 1..9999 without a leading zero, the
 * ")" is followed by a space or ends the title; anything else (no count, "(0)", "(12345)", "(3)Chat") is 0. */
int  webpage_title_badge(const char *title);

/* lord-web's user agent: "Mozilla/5.0 (X11; Linux <machine>) AppleWebKit/605.1.15 (KHTML, like Gecko)
 * Version/18.0 Safari/605.1.15 LordOS/<version>". The AppleWebKit and Safari tokens are the ones WebKit itself sends
 * (frozen at 605.1.15); a machine that isn't [A-Za-z0-9_] is left out ("(X11; Linux)"), a version that isn't
 * [A-Za-z0-9.+~-] drops the LordOS token. 0, or -1 when out is too short. */
int  webpage_user_agent(const char *machine, const char *version, char *out, size_t n);

/* LordOS's sound for a page's notification ("notifySound" in the registry, webreg.h WEBREG_SOUND_*):
 *   auto    LordOS plays its sound unless the page plays its own: it started audio less than WEBPAGE_SOUND_WINDOW_MS
 *           before or after a notification. Once seen, lord-web remembers it for the profile (its state file,
 *           "ownSound") and later notifications go without LordOS's sound at once.
 *   app     never LordOS's sound (the page has its own);
 *   system  always LordOS's sound;
 *   none    never LordOS's sound; lord-web doesn't touch the page's audio either.
 * A notification the page marks silent never has LordOS's sound (WebKit 2.48 doesn't say, so lord-web passes 0). */
#define WEBPAGE_SOUND_WINDOW_MS 1500.0
/* 1 when the notification posted at now_ms goes with "suppress-sound"; last_audio_ms is when the page last started
 * audio (< 0: never) */
int  webpage_sound_suppress(int mode, int learned, int silent, double last_audio_ms, double now_ms);
/* 1 when audio started at audio_ms, next to a notification at note_ms, teaches "the page plays its own sound" */
int  webpage_sound_learns(int mode, double note_ms, double audio_ms);

/* The profile's state file (<profile>/state.json, lord-web's own: { "ownSound": true|false }). Unknown keys are
 * ignored, anything unreadable is the defaults. */
typedef struct { int own_sound; } WebPageState;
void   webpage_state_parse(const char *json, size_t len, WebPageState *s);
size_t webpage_state_write(const WebPageState *s, char *out, size_t n);

/* The window as WebKit should see it: visible (painting, requestAnimationFrame, CSS animations, timers at full rate)
 * only while it is mapped, configured and not suspended. */
int  webpage_view_visible(int shown, int configured, int suspended);
/* 1 when xdg_toplevel's configure states hold "suspended" (9, xdg-shell 6: lord-server sends it for a minimized
 * window and one on a hidden desktop) */
int  webpage_toplevel_suspended(const unsigned *states, size_t n);

#endif
