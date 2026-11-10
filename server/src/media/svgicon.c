/* svgicon.c — SVG icons drawn by lord-media for lord-server's images (see svgicon.h). */
#include "svgicon.h"
#include "media_int.h"
#include "helper/lm_svg.h"
#include "../iconfind.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int enabled;

void svgicon_enable(void) { enabled = 1; }

Image *svgicon_get(const char *path, int side, long color) {
    char real[PATH_MAX];
    struct stat st;
    if (!enabled || !path || path[0] != '/' || !realpath(path, real) || !iconfind_svg_allowed(real, getenv("HOME"))) return NULL;
    if (stat(real, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > LM_SVG_MAX_BYTES) return NULL;
    if (side < LM_SVG_MIN_SIDE) side = LM_SVG_MIN_SIDE;
    if (side > LM_SVG_MAX_SIDE) side = LM_SVG_MAX_SIDE;
    if (color > 0xffffff) color = -1;
    char key[PATH_MAX + 96], hex[8] = "-";
    if (color >= 0) snprintf(hex, sizeof hex, "%06lx", color);
    snprintf(key, sizeof key, "svg:%d:%s:%lld.%09ld:%lld:%s", side, hex, (long long)st.st_mtim.tv_sec, st.st_mtim.tv_nsec,
             (long long)st.st_size, real);
    int created = 0;
    Image *img = image_get_ext(key, &created);
    if (!img || !created) return img;
    /* the file itself, checked again once open: it may have changed since the stat */
    int fd = open(real, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd >= 0 && (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > LM_SVG_MAX_BYTES)) { close(fd); fd = -1; }
    if (fd < 0) { image_ext_done(key, 0, 0, NULL); return img; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    /* the key goes in params for lord-server's side only (media.c doesn't send it to the helper) */
    if (media_job(0, MW_SVG, JP_SVG, &fd, 1, side, color >= 0 ? color : -1, key, NULL, NULL, 0) < 0) image_ext_done(key, 0, 0, NULL);
    return img;
}

Image *svgicon_get_spec(const char *spec) {
    if (strncmp(spec, "svg:", 4)) return NULL;
    const char *p = spec + 4;
    char *end;
    long side = strtol(p, &end, 10);
    if (end == p || end - p > 3 || *end != ':') return NULL;
    p = end + 1;
    long color = -1;
    if (*p == '-') p++;
    else {
        if (strspn(p, "0123456789abcdefABCDEF") != 6) return NULL;
        color = strtol(p, NULL, 16);
        p += 6;
    }
    if (*p != ':' || p[1] != '/') return NULL;
    return svgicon_get(p + 1, (int)side, color);
}

void svgicon_done(const char *key, int w, int h, const uint32_t *argb) {
    if (!key) return;
    if (argb && (w < 1 || h < 1 || w > LM_SVG_MAX_SIDE || h > LM_SVG_MAX_SIDE)) argb = NULL;
    image_ext_done(key, w, h, argb);
}
