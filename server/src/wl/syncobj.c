/* syncobj.c — the kernel side of explicit sync (syncobj.h, docs/SYNCOBJ-PLAN.md): DRM syncobj ioctls on a render node
 * fd of lord-server's own, with the kernel's uapi headers and no libdrm (as kms.c). Every ioctl here is
 * DRM_RENDER_ALLOW: no DRM master, no privilege. The fd is lord-server's alone and lives as long as it does, so a GPU
 * context re-created (gpu.c) never invalidates a client's imported timelines. Syncobjs belong to no device: a timeline
 * an app made on its GPU imports on any render node. */
#include "syncobj.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <drm/drm.h>
#include <linux/dma-buf.h>

static int sfd = -1;            /* the render node; -1 = explicit sync not offered */
static int tried;
static char node[64];

static int xioctl(unsigned long req, void *arg) {
    int r;
    do r = ioctl(sfd, req, arg);
    while (r < 0 && (errno == EINTR || errno == EAGAIN));
    return r;
}

/* timelines, and SYNCOBJ_EVENTFD (a handle that doesn't exist: ENOENT if the ioctl is there, EINVAL/ENOTTY if not) */
static int usable(int fd) {
    struct drm_get_cap c = { .capability = DRM_CAP_SYNCOBJ_TIMELINE };
    if (ioctl(fd, DRM_IOCTL_GET_CAP, &c) || !c.value) return 0;
    struct drm_syncobj_eventfd e = { .handle = 0xffffffffu, .fd = -1 };
    return ioctl(fd, DRM_IOCTL_SYNCOBJ_EVENTFD, &e) < 0 && errno == ENOENT;
}

static int try_node(const char *path) {
    int fd = open(path, O_RDWR | O_CLOEXEC | O_NOCTTY);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) || !S_ISCHR(st.st_mode) || !usable(fd)) { close(fd); return 0; }
    sfd = fd;
    snprintf(node, sizeof node, "%s", path);
    return 1;
}

int sobj_init(dev_t dev) {
    if (tried) return sfd >= 0 ? 0 : -1;
    tried = 1;
    const char *off = getenv("LORD_SYNCOBJ");          /* LORD_SYNCOBJ=0: not offered (a test, or a driver at fault) */
    if (off && !strcmp(off, "0")) return -1;
    char path[64];
    DIR *d = opendir("/dev/dri");
    struct dirent *e;
    /* the GPU context's render node first, then any other */
    for (int pass = 0; pass < 2 && sfd < 0 && d; pass++) {
        rewinddir(d);
        while (sfd < 0 && (e = readdir(d))) {
            if (strncmp(e->d_name, "renderD", 7) || strlen(e->d_name) > 11) continue;
            snprintf(path, sizeof path, "/dev/dri/%.16s", e->d_name);
            struct stat st;
            if (stat(path, &st) || !S_ISCHR(st.st_mode)) continue;
            if (pass == 0 && (!dev || st.st_rdev != dev)) continue;
            try_node(path);
        }
    }
    if (d) closedir(d);
    if (sfd >= 0) fprintf(stderr, "wayland: explicit sync (linux-drm-syncobj-v1) on %s\n", node);
    else fprintf(stderr, "wayland: explicit sync not offered (no render node with timeline syncobjs and SYNCOBJ_EVENTFD)\n");
    return sfd >= 0 ? 0 : -1;
}

int sobj_available(void) { return sfd >= 0; }

SobjTimeline *sobj_import(int fd) {
    if (sfd < 0 || fd < 0) return NULL;
    struct drm_syncobj_handle h = { .fd = fd };
    if (xioctl(DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &h)) return NULL;
    SobjTimeline *t = calloc(1, sizeof *t);
    if (!t) {
        struct drm_syncobj_destroy dd = { .handle = h.handle };
        xioctl(DRM_IOCTL_SYNCOBJ_DESTROY, &dd);
        return NULL;
    }
    t->handle = h.handle;
    t->refs = 1;
    return t;
}

SobjTimeline *sobj_ref(SobjTimeline *t) {
    if (t) t->refs++;
    return t;
}

void sobj_unref(SobjTimeline *t) {
    if (!t || --t->refs > 0) return;
    struct drm_syncobj_destroy dd = { .handle = t->handle };
    if (sfd >= 0) xioctl(DRM_IOCTL_SYNCOBJ_DESTROY, &dd);
    free(t);
}

/* A wait that never blocks: its deadline (CLOCK_MONOTONIC ns) is 0, already past. WAIT_FOR_SUBMIT so a point with no
 * fence yet is "not signalled" (ETIME), not an error. */
int sobj_signalled(SobjPoint p) {
    if (sfd < 0 || !p.tl) return -1;
    uint32_t h = p.tl->handle;
    uint64_t pt = p.point;
    struct drm_syncobj_timeline_wait w = {
        .handles = (uintptr_t)&h, .points = (uintptr_t)&pt, .timeout_nsec = 0, .count_handles = 1,
        .flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
    };
    if (!xioctl(DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &w)) return 1;
    return errno == ETIME ? 0 : -1;
}

int sobj_eventfd(SobjPoint p) {
    if (sfd < 0 || !p.tl) return -1;
    int efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (efd < 0) return -1;
    /* flags 0: when the point is signalled, not only available (the CPU path maps the buffer right after) */
    struct drm_syncobj_eventfd e = { .handle = p.tl->handle, .flags = 0, .point = p.point, .fd = efd };
    if (xioctl(DRM_IOCTL_SYNCOBJ_EVENTFD, &e)) { close(efd); return -1; }
    return efd;
}

static void signal_now(uint32_t handle, uint64_t point) {
    struct drm_syncobj_timeline_array a = { .handles = (uintptr_t)&handle, .points = (uintptr_t)&point, .count_handles = 1 };
    if (xioctl(DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &a))
        fprintf(stderr, "wayland: explicit sync: signalling a release point failed (%s)\n", strerror(errno));
}

void sobj_release(SobjPoint *p) { (void)sobj_release_after(p, -1); }

int sobj_release_after(SobjPoint *p, int dmabuf_fd) {
    if (!p->tl) return 0;
    int done = 0;
    if (sfd >= 0) {
        if (dmabuf_fd >= 0) {
            /* the buffer's fences (DMA_BUF_SYNC_WRITE: every reader's and writer's, so the GPU's last sampling of it)
             * as a sync_file, into a temporary binary syncobj, transferred onto the release point */
            struct dma_buf_export_sync_file ex = { .flags = DMA_BUF_SYNC_WRITE, .fd = -1 };
            int r;
            do r = ioctl(dmabuf_fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &ex);
            while (r < 0 && errno == EINTR);
            if (!r && ex.fd >= 0) {
                struct drm_syncobj_create c = { 0 };
                if (!xioctl(DRM_IOCTL_SYNCOBJ_CREATE, &c)) {
                    struct drm_syncobj_handle im = { .handle = c.handle, .flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE, .fd = ex.fd };
                    struct drm_syncobj_transfer tr = { .src_handle = c.handle, .dst_handle = p->tl->handle, .src_point = 0, .dst_point = p->point };
                    done = !xioctl(DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &im) && !xioctl(DRM_IOCTL_SYNCOBJ_TRANSFER, &tr);
                    struct drm_syncobj_destroy dd = { .handle = c.handle };
                    xioctl(DRM_IOCTL_SYNCOBJ_DESTROY, &dd);
                }
                close(ex.fd);
            }
        }
        if (!done) signal_now(p->tl->handle, p->point);
    }
    sobj_unref(p->tl);
    p->tl = NULL;
    return done;
}
