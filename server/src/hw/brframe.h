/* brframe.h — Settings → Appearance → "Browsers: tabs in the title bar" (docs/WAYLAND.md, "The
 * switch in Settings"). Writes ONE preference of the preinstalled browsers in the user's own
 * profiles, and nothing else:
 *
 *   Chromium  ~/.config/chromium/{Default,Profile N}/Preferences, and the same in Brave's
 *             ~/.config/BraveSoftware/Brave-Browser (0.8.8.4, #39; "chromium" below means both):
 *             browser.custom_chrome_frame = on, and with on extensions.theme.system_theme = 1
 *             (the GTK theme, for LordOS's window buttons) unless the profile uses a theme
 *             extension. Only while that browser is not running (it rewrites the file when it quits);
 *             a surgical edit of those keys, the rest of the file byte for byte, validated as JSON
 *             before and after, a backup (Preferences.lordos-backup) and an atomic rename.
 *   Firefox   ~/.config/mozilla/firefox and ~/.mozilla/firefox, the relative profiles listed in
 *             profiles.ini: one marked line user_pref("browser.tabs.inTitlebar", 1|0) in user.js
 *             (Firefox reads it at start, so it can be written while Firefox runs). brf_firefox_tidy
 *             removes the line again once Firefox has started with it (its "lock" link or .parentlock
 *             newer than user.js, the profile not open), so later choices in Firefox's own «Title Bar» stick. A
 *             user.js line the user wrote themselves is never touched ("kept").
 *
 * No program is run, no path comes from the caller: the folders are fixed under the user's home.
 * Every file is opened with O_NOFOLLOW, must be a regular file owned by the user, and is size-capped. */
#ifndef LORD_HW_BRFRAME_H
#define LORD_HW_BRFRAME_H

#include <stddef.h>

typedef struct {
    int found;      /* the browser's data folder exists */
    int profiles;   /* profiles looked at */
    int changed;    /* written now */
    int unchanged;  /* already as asked */
    int running;    /* Chromium/Brave: one is open and its profiles weren't written (try again after it quits);
                       Firefox: profiles that are open (the line applies when Firefox restarts) */
    int kept;       /* Firefox: the user's own user.js sets the preference; left alone */
    int failed;     /* unreadable, malformed or unwritable: left exactly as it was */
} BrfResult;

enum { BRF_CHANGED = 0, BRF_SAME = 1, BRF_BAD = -1 };

/* Pure edits (tests/brframe_test.c). *out is malloc'd (NUL-terminated) only with BRF_CHANGED. */
int brf_chrome_edit(const char *in, size_t n, int on, char **out, size_t *outn);
/* user.js: value 0/1 sets LordOS's marked line, -1 removes it. *own = 1 if a line the user wrote
 * sets the preference (then nothing is changed: BRF_SAME). */
int brf_userjs_edit(const char *in, size_t n, int value, char **out, size_t *outn, int *own);
/* The value prefs.js holds for browser.tabs.inTitlebar, or -1 if it isn't set there. */
int brf_prefs_value(const char *in, size_t n);
/* The value of LordOS's marked line in user.js, or -1. */
int brf_userjs_ours(const char *in, size_t n);

/* On disk. cfg: the user's config folder (~/.config); home: the home folder (legacy ~/.mozilla). */
void brf_chromium_apply(const char *cfg, int on, BrfResult *r);
void brf_firefox_apply(const char *cfg, const char *home, int on, BrfResult *r);
int  brf_firefox_tidy(const char *cfg, const char *home);         /* marked lines removed */
int  brf_chromium_running(const char *cfg);
/* Chromium and Brave profiles that switching off (*on) or on (*off) would change; malformed ones aren't counted. */
void brf_chromium_state(const char *cfg, int *on, int *off);

/* {"chromium":{…},"firefox":{…}} for lord.browserFrame.apply (malloc, NULL on OOM). */
char *brf_result_json(const BrfResult *chromium, const BrfResult *firefox);

#endif
