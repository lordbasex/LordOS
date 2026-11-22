/* syncobj_kernel_test.c — explicit sync's kernel side (src/wl/syncobj.c) on this machine's render node: importing a
 * timeline, checking a point without blocking, the eventfd that wakes the loop, signalling a release point at once
 * and with a dmabuf's fences (a udmabuf), and the timeline's references. No GPU work is needed. Exit 77 without a render
 * node that has timeline syncobjs and SYNCOBJ_EVENTFD (Linux 6.6). Run by tests/run_suites.sh. */
#include "wl/syncobj.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/udmabuf.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <drm/drm.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "syncobj_kernel_test: line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static int rn = -1;         /* the test's own render node: it plays the app */

static int open_rnode(void) {
    DIR *d = opendir("/dev/dri");
    struct dirent *e;
    int fd = -1;
    while (d && fd < 0 && (e = readdir(d))) {
        if (strncmp(e->d_name, "renderD", 7)) continue;
        char p[300];
        snprintf(p, sizeof p, "/dev/dri/%s", e->d_name);
        fd = open(p, O_RDWR | O_CLOEXEC);
        struct drm_get_cap c = { .capability = DRM_CAP_SYNCOBJ_TIMELINE };
        if (fd >= 0 && (ioctl(fd, DRM_IOCTL_GET_CAP, &c) || !c.value)) { close(fd); fd = -1; }
    }
    if (d) closedir(d);
    return fd;
}

static void app_signal(uint32_t h, uint64_t pt) {
    struct drm_syncobj_timeline_array a = { .handles = (uintptr_t)&h, .points = (uintptr_t)&pt, .count_handles = 1 };
    if (ioctl(rn, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &a)) perror("TIMELINE_SIGNAL");
}

/* 1 when the point is signalled within ms */
static int app_wait(uint32_t h, uint64_t pt, int ms) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t deadline = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec + (int64_t)ms * 1000000LL;
    struct drm_syncobj_timeline_wait w = { .handles = (uintptr_t)&h, .points = (uintptr_t)&pt, .timeout_nsec = deadline,
                                           .count_handles = 1, .flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT };
    return ioctl(rn, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &w) == 0;
}

static int readable(int fd, int ms) {
    struct pollfd p = { fd, POLLIN, 0 };
    return poll(&p, 1, ms) == 1 && (p.revents & POLLIN);
}

int main(void) {
    if (sobj_init(0) || (rn = open_rnode()) < 0) {
        printf("skipped: needs a render node with timeline syncobjs and SYNCOBJ_EVENTFD\n");
        return 77;
    }
    CHECK(sobj_available());
    /* the app's timeline, handed over as an fd */
    struct drm_syncobj_create c = { 0 };
    CHECK(!ioctl(rn, DRM_IOCTL_SYNCOBJ_CREATE, &c));
    struct drm_syncobj_handle hf = { .handle = c.handle };
    CHECK(!ioctl(rn, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &hf));
    SobjTimeline *t = sobj_import(hf.fd);
    close(hf.fd);
    CHECK(t && t->refs == 1);
    /* an fd that is not a syncobj is refused */
    int mfd = memfd_create("not-a-syncobj", MFD_CLOEXEC);
    CHECK(sobj_import(mfd) == NULL);
    close(mfd);
    CHECK(sobj_import(-1) == NULL);
    if (!t) { fprintf(stderr, "syncobj_kernel_test: no timeline\n"); return 1; }

    /* an acquire point not submitted yet: not signalled, without blocking; its eventfd wakes when the app signals it */
    SobjPoint a = { sobj_ref(t), 1 };
    CHECK(t->refs == 2);
    CHECK(sobj_signalled(a) == 0);
    int efd = sobj_eventfd(a);
    CHECK(efd >= 0);
    CHECK(!readable(efd, 50));
    app_signal(c.handle, 1);
    CHECK(readable(efd, 1000));
    CHECK(sobj_signalled(a) == 1);
    close(efd);
    /* a point already signalled: its eventfd is readable at once (a commit queued behind is never stuck) */
    efd = sobj_eventfd(a);
    CHECK(efd >= 0 && readable(efd, 100));
    if (efd >= 0) close(efd);
    /* a later point: not yet (signalling 1 doesn't reach 2) */
    SobjPoint a2 = { t, 2 };
    CHECK(sobj_signalled(a2) == 0);

    /* release at once (after a CPU copy) */
    SobjPoint r = { sobj_ref(t), 5 };
    CHECK(sobj_release_after(&r, -1) == 0);
    CHECK(r.tl == NULL);
    CHECK(app_wait(c.handle, 5, 1000));
    /* release with a dmabuf's fences (DMA_BUF_IOCTL_EXPORT_SYNC_FILE → temporary syncobj → TRANSFER): a udmabuf has
     * none, so the point is signalled at once, but through the transfer */
    int ud = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (ud >= 0) {
        int m = memfd_create("syncobj-kernel-test", MFD_ALLOW_SEALING | MFD_CLOEXEC);
        CHECK(m >= 0 && !ftruncate(m, 4096) && !fcntl(m, F_ADD_SEALS, F_SEAL_SHRINK));
        struct udmabuf_create uc = { .memfd = (uint32_t)m, .flags = UDMABUF_FLAGS_CLOEXEC, .offset = 0, .size = 4096 };
        int dfd = ioctl(ud, UDMABUF_CREATE, &uc);
        CHECK(dfd >= 0);
        SobjPoint r2 = { sobj_ref(t), 7 };
        CHECK(sobj_release_after(&r2, dfd) == 1);
        CHECK(r2.tl == NULL);
        CHECK(app_wait(c.handle, 7, 1000));
        if (dfd >= 0) close(dfd);
        close(m);
        close(ud);
        printf("syncobj_kernel_test: a release point went with a udmabuf's fences (export, import, transfer)\n");
    } else {
        printf("syncobj_kernel_test: no /dev/udmabuf (%s): the release with a dmabuf's fences not checked\n", strerror(errno));
    }
    /* a release point that is not a dmabuf: an fd the export refuses falls back to signalling at once */
    SobjPoint r3 = { sobj_ref(t), 9 };
    int nfd = memfd_create("not-a-dmabuf", MFD_CLOEXEC);
    CHECK(sobj_release_after(&r3, nfd) == 0);
    close(nfd);
    CHECK(app_wait(c.handle, 9, 1000));
    CHECK(sobj_release_after(&r3, -1) == 0);          /* no point: nothing */

    /* references: the protocol object gone, a pending point keeps the timeline */
    CHECK(t->refs == 2);
    sobj_unref(t);                                    /* the wp_linux_drm_syncobj_timeline_v1 destroyed */
    SobjPoint a3 = { a.tl, 9 };
    CHECK(sobj_signalled(a3) == 1);
    sobj_unref(a.tl);                                 /* the last point: the handle is destroyed */

    if (fails) { fprintf(stderr, "syncobj_kernel_test: %d failed\n", fails); return 1; }
    printf("syncobj_kernel_test: ok\n");
    return 0;
}
