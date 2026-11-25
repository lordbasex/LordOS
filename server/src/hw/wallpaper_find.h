/* wallpaper_find.h — one entry of the wallpapers catalog (moving wallpapers and, since 0.8.9.6.2, still pictures). */
#ifndef LORD_HW_WALLPAPER_FIND_H
#define LORD_HW_WALLPAPER_FIND_H
#include <stddef.h>
/* The catalog entry of "<id> <variant>": 0 and the four outputs if it exists. *ext is the extension the file is kept
 * with, decided here from the variant's codec and never from the caller: "mp4" for hevc and h264 (the file must be an
 * .mp4), "jpg" for jpeg (the file must be a .jpg). Any other codec, or a file of another kind, is no entry. */
int hw_wallpaper_find(const char *json, size_t n, const char *arg, char *url, size_t un, char *sha, size_t sn, unsigned long long *size,
                      const char **ext);
#endif
