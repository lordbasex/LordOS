/* thumb.c — the picture of a remote computer that Remote Desk shows on its card: the shared picture of a session, scaled
 * down and kept as a PNG under ~/.local/share/lordos/remote-desk/thumbs/<device>.png (folder 0700, file 0600). Runs in
 * lord-server, as the person; lord-rdesk never writes files. */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "stb_image_write.h"
#pragma GCC diagnostic pop
#pragma GCC diagnostic ignored "-Wunused-function"      /* the writer's other formats, reported at the end of the file */

#include "thumb.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int rd_thumb_id_ok(const char *id) {
    size_t n = id ? strlen(id) : 0;
    if (!n || n > 64) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_')) return 0;
    }
    return 1;
}

/* BGRA (0xAARRGGBB) w×h → RGB at most maxw wide, each output pixel the average of the source box it covers */
unsigned char *rd_thumb_scale(const uint32_t *px, int w, int h, int maxw, int *ow, int *oh) {
    if (!px || w < 1 || h < 1 || maxw < 1) return NULL;
    int tw = w > maxw ? maxw : w, th = (int)((long long)h * tw / w);
    if (th < 1) th = 1;
    unsigned char *rgb = malloc((size_t)tw * th * 3);
    if (!rgb) return NULL;
    for (int y = 0; y < th; y++) {
        int y0 = (int)((long long)y * h / th), y1 = (int)((long long)(y + 1) * h / th);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < tw; x++) {
            int x0 = (int)((long long)x * w / tw), x1 = (int)((long long)(x + 1) * w / tw);
            if (x1 <= x0) x1 = x0 + 1;
            unsigned long r = 0, g = 0, b = 0, k = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    uint32_t v = px[(size_t)yy * w + xx];
                    r += (v >> 16) & 255; g += (v >> 8) & 255; b += v & 255; k++;
                }
            unsigned char *o = rgb + ((size_t)y * tw + x) * 3;
            o[0] = (unsigned char)(r / k); o[1] = (unsigned char)(g / k); o[2] = (unsigned char)(b / k);
        }
    }
    *ow = tw; *oh = th;
    return rgb;
}

/* the folder, made 0700 where it is missing (each level that already exists is left as it is, but the last one must be
 * a real directory of this user). An open directory descriptor, or -1. */
static int thumbs_dir(const char *home) {
    static const char *const parts[] = { ".local", "share", "lordos", "remote-desk", "thumbs" };
    int fd = open(home, O_PATH | O_DIRECTORY | O_CLOEXEC);
    size_t n = sizeof parts / sizeof parts[0];
    for (size_t i = 0; fd >= 0 && i < n; i++) {
        if (mkdirat(fd, parts[i], 0700) && errno != EEXIST) { close(fd); return -1; }
        int next = openat(fd, parts[i], (i + 1 < n ? O_PATH : O_RDONLY) | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(fd);
        fd = next;
    }
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || st.st_uid != geteuid() || ((st.st_mode & 077) && fchmod(fd, 0700))) { close(fd); return -1; }
    return fd;
}

typedef struct { int fd, failed; } Out;
static void put(void *ud, void *data, int n) {
    Out *o = ud;
    const char *p = data;
    while (!o->failed && n > 0) {
        ssize_t r = write(o->fd, p, (size_t)n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { o->failed = 1; break; }
        p += r; n -= (int)r;
    }
}

int rd_thumb_save(const char *home, const char *id, const uint32_t *px, int w, int h, int maxw) {
    if (!home || home[0] != '/' || !rd_thumb_id_ok(id)) return -1;
    int ow = 0, oh = 0;
    unsigned char *rgb = rd_thumb_scale(px, w, h, maxw, &ow, &oh);
    if (!rgb) return -1;
    int dir = thumbs_dir(home);
    if (dir < 0) { free(rgb); return -1; }
    char tmp[96], name[80];
    snprintf(name, sizeof name, "%s.png", id);
    snprintf(tmp, sizeof tmp, ".%s.%ld.tmp", id, (long)getpid());
    int rc = -1;
    int fd = openat(dir, tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd >= 0) {
        Out o = { fd, 0 };
        stbi_write_png_compression_level = 4;          /* a quick write: it runs once a connection, on the UI thread */
        int ok = stbi_write_png_to_func(put, &o, ow, oh, 3, rgb, ow * 3);
        if (fchmod(fd, 0600)) o.failed = 1;
        if (close(fd)) o.failed = 1;
        if (ok && !o.failed && !renameat(dir, tmp, dir, name)) rc = 0;
        else unlinkat(dir, tmp, 0);
    }
    close(dir);
    free(rgb);
    return rc;
}

int rd_thumb_remove(const char *home, const char *id) {
    if (!home || home[0] != '/' || !rd_thumb_id_ok(id)) return -1;
    char path[4096];
    snprintf(path, sizeof path, "%s/.local/share/lordos/remote-desk/thumbs/%s.png", home, id);
    return unlink(path) && errno != ENOENT ? -1 : 0;
}
