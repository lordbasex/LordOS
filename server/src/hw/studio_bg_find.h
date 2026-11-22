/* studio_bg_find.h — one file of LordOS Studio's background catalog (assets/studio/backgrounds.json; docs/CAMERA-STUDIO-
 * PLAN.md 6.2), apart for its test. The twin of wallpaper_find.h. */
#ifndef LORD_HW_STUDIO_BG_FIND_H
#define LORD_HW_STUDIO_BG_FIND_H
#include <stddef.h>

#define STUDIO_BG_MAX_FILE (100ull * 1024 * 1024)   /* the host's own limit per file */

/* The catalog entry of "<id> <variant>": 0 and the outputs if it exists and is well formed. url: base + "/" + file;
 * sha: 64 lowercase hex digits; ext: "jpg" for a picture's variant, "mp4" for a video's (the file's own extension
 * must agree with the item's kind). The base must be the project's GitHub owner on raw.githubusercontent.com. */
int hw_studio_bg_find(const char *json, size_t n, const char *arg, char *url, size_t un, char *sha, size_t sn,
                      unsigned long long *size, const char **ext);

#endif
