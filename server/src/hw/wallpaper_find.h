/* wallpaper_find.h — one entry of the moving wallpapers catalog. */
#ifndef LORD_HW_WALLPAPER_FIND_H
#define LORD_HW_WALLPAPER_FIND_H
#include <stddef.h>
/* The catalog entry of "<id> <variant>": 0 and the three outputs if it exists. */
int hw_wallpaper_find(const char *json, size_t n, const char *arg, char *url, size_t un, char *sha, size_t sn, unsigned long long *size);
#endif
