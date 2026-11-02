/* wallpaper_image.h — a picture chosen by the user, made safe and quick to show (docs/WALLPAPERS.md). */
#ifndef LORD_HW_WALLPAPER_IMAGE_H
#define LORD_HW_WALLPAPER_IMAGE_H
#include <stddef.h>

#define WP_MAX_FILE   (64u << 20)                   /* the file the user picks */
#define WP_MAX_PIXELS 100000000LL                   /* checked in its header, before any memory is taken */
#define WP_MAX_SIDE   3840                          /* what is kept: enough for a 4K display, and quick to load */
#define WP_MAX_JPEG   (32u << 20)                   /* what is written */

enum { WP_OK = 0, WP_BAD, WP_TOOBIG, WP_READ, WP_WRITE };

/* A regular file, never a link, up to WP_MAX_FILE. Reading a pipe or a device can't hang. Free with free(). */
int wp_read_file(const char *path, unsigned char **data, size_t *n);
/* EXIF orientation of a JPEG, 1 to 8; 1 if it has none. */
int wp_orientation(const unsigned char *d, size_t n);
/* Only PNG and JPEG, by content. Output: a baseline JPEG, upright, no metadata, with no side over WP_MAX_SIDE, and if
 * thumb isn't NULL a 320×180 crop for the pickers. Free both with free(). */
int wp_encode(const unsigned char *in, size_t n, unsigned char **full, size_t *fn, unsigned char **thumb, size_t *tn);
/* Is this a finished JPEG of an allowed size? What root checks before keeping bytes it didn't make. */
int wp_is_jpeg(const unsigned char *d, size_t n);
#endif
