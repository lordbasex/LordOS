/* drmprobe.c — the display engine's facts for the newer Wayland protocols (drmprobe.h). Kernel uapi only. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "drmprobe.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_mode.h>

/* the "type" property's values (the kernel's enum drm_plane_type; not in the uapi headers) */
enum { PLANE_OVERLAY = 0, PLANE_PRIMARY = 1, PLANE_CURSOR = 2 };

#define MAX_PLANES 64
#define MAX_CONNECTORS 32
#define MAX_PROPS 96
#define MAX_BLOB (256 * 1024)

int drmprobe_in_formats(const void *blob, size_t len, int *formats, int *modifiers) {
    struct drm_format_modifier_blob h;
    if (!blob || len < sizeof h) return -1;
    memcpy(&h, blob, sizeof h);
    if (h.version != FORMAT_BLOB_CURRENT) return -1;
    if (h.count_formats > 4096 || h.count_modifiers > 4096) return -1;
    uint64_t fe = (uint64_t)h.formats_offset + (uint64_t)h.count_formats * sizeof(uint32_t);
    uint64_t me = (uint64_t)h.modifiers_offset + (uint64_t)h.count_modifiers * sizeof(struct drm_format_modifier);
    if (h.formats_offset < sizeof h || fe > len || (h.count_modifiers && (h.modifiers_offset < sizeof h || me > len))) return -1;
    /* distinct modifiers (each entry is one modifier for a window of 64 formats; at most 4096 entries) */
    int ns = 0;
    const unsigned char *p = (const unsigned char *)blob + h.modifiers_offset;
    for (uint32_t i = 0; i < h.count_modifiers; i++) {
        struct drm_format_modifier m, o;
        memcpy(&m, p + (size_t)i * sizeof m, sizeof m);
        int dup = 0;
        for (uint32_t k = 0; k < i && !dup; k++) { memcpy(&o, p + (size_t)k * sizeof o, sizeof o); dup = o.modifier == m.modifier; }
        ns += !dup;
    }
    *formats = (int)h.count_formats;
    *modifiers = ns;
    return 0;
}

static int xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do r = ioctl(fd, req, arg); while (r == -1 && (errno == EINTR || errno == EAGAIN));
    return r;
}

static int get_cap(int fd, uint64_t cap, uint64_t *v) {
    struct drm_get_cap c = { .capability = cap };
    if (xioctl(fd, DRM_IOCTL_GET_CAP, &c)) return -1;
    *v = c.value;
    return 0;
}

/* an object's property, by name: its value (and for a blob, the blob's id). 0 when found. */
static int prop_value(int fd, uint32_t obj, uint32_t type, const char *name, uint64_t *value) {
    uint32_t ids[MAX_PROPS];
    uint64_t vals[MAX_PROPS];
    struct drm_mode_obj_get_properties gp = { .obj_id = obj, .obj_type = type };
    if (xioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp)) return -1;
    if (gp.count_props > MAX_PROPS) gp.count_props = MAX_PROPS;
    uint32_t want = gp.count_props;
    gp.props_ptr = (uintptr_t)ids;
    gp.prop_values_ptr = (uintptr_t)vals;
    if (xioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp)) return -1;
    if (gp.count_props < want) want = gp.count_props;      /* the list may have shrunk meanwhile */
    for (uint32_t i = 0; i < want; i++) {
        struct drm_mode_get_property p = { .prop_id = ids[i] };   /* counts 0: the name and flags only */
        if (xioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &p)) continue;
        p.name[DRM_PROP_NAME_LEN - 1] = 0;
        if (!strcmp(p.name, name)) { *value = vals[i]; return 0; }
    }
    return -1;
}

static void in_formats(int fd, uint32_t blob_id, DrmFacts *f) {
    struct drm_mode_get_blob b = { .blob_id = blob_id };
    if (!blob_id || xioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &b) || !b.length || b.length > MAX_BLOB) return;
    void *data = malloc(b.length);
    if (!data) return;
    uint32_t len = b.length;
    b.data = (uintptr_t)data;
    if (!xioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &b) && b.length == len)
        drmprobe_in_formats(data, len, &f->scanout_formats, &f->scanout_modifiers);
    free(data);
}

static void planes(int fd, DrmFacts *f) {
    uint32_t ids[MAX_PLANES];
    struct drm_mode_get_plane_res pr = { 0 };
    if (xioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr)) return;
    if (pr.count_planes > MAX_PLANES) pr.count_planes = MAX_PLANES;
    uint32_t want = pr.count_planes;
    pr.plane_id_ptr = (uintptr_t)ids;
    if (xioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr)) return;
    if (pr.count_planes < want) want = pr.count_planes;
    f->planes = (int)want;
    f->primary = f->overlay = f->cursor = 0;
    int got_formats = 0;
    for (uint32_t i = 0; i < want; i++) {
        uint64_t t;
        if (prop_value(fd, ids[i], DRM_MODE_OBJECT_PLANE, "type", &t)) continue;
        if (t == PLANE_PRIMARY) {
            f->primary++;
            uint64_t blob;
            if (!got_formats && !prop_value(fd, ids[i], DRM_MODE_OBJECT_PLANE, "IN_FORMATS", &blob)) { in_formats(fd, (uint32_t)blob, f); got_formats = 1; }
        } else if (t == PLANE_OVERLAY) f->overlay++;
        else if (t == PLANE_CURSOR) f->cursor++;
    }
}

static void connectors(int fd, DrmFacts *f) {
    uint32_t ids[MAX_CONNECTORS];
    struct drm_mode_card_res r = { 0 };
    if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r)) return;
    if (r.count_connectors > MAX_CONNECTORS) r.count_connectors = MAX_CONNECTORS;
    uint32_t want = r.count_connectors;
    /* only the connectors: the other counts 0 so the kernel copies nothing else */
    r.count_fbs = r.count_crtcs = r.count_encoders = 0;
    r.connector_id_ptr = (uintptr_t)ids;
    if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r)) return;
    if (r.count_connectors < want) want = r.count_connectors;
    f->connectors = (int)want;
    f->non_desktop = 0;
    for (uint32_t i = 0; i < want; i++) {
        uint64_t v;
        if (!prop_value(fd, ids[i], DRM_MODE_OBJECT_CONNECTOR, "non-desktop", &v) && v) f->non_desktop++;
    }
}

/* the card's render node, "renderD128", from sysfs */
static int render_name(const char *card, char *out, size_t n) {
    char p[96];
    snprintf(p, sizeof p, "/sys/class/drm/%s/device/drm", card);
    DIR *d = opendir(p);
    int ok = -1;
    for (struct dirent *e; d && (e = readdir(d));)
        if (!strncmp(e->d_name, "renderD", 7) && strlen(e->d_name) < 12 && strspn(e->d_name + 7, "0123456789") == strlen(e->d_name + 7)) {
            snprintf(out, n, "%s", e->d_name);
            ok = 0;
            break;
        }
    if (d) closedir(d);
    return ok;
}

static int valid_card(const char *card) {
    return card && !strncmp(card, "card", 4) && card[4] && strlen(card) < 10 && strspn(card + 4, "0123456789") == strlen(card + 4);
}

int drmprobe_card(const char *card, DrmFacts *f) {
    drmprobe_init(f);
    if (!valid_card(card)) return -1;
    f->probed = 1;
    char path[64], rn[16];
    uint64_t v;
    if (!render_name(card, rn, sizeof rn)) {
        snprintf(path, sizeof path, "/dev/dri/%s", rn);
        int fd = open(path, O_RDWR | O_CLOEXEC | O_NOCTTY);
        if (fd >= 0) {
            f->render_node = 1;
            if (!get_cap(fd, DRM_CAP_SYNCOBJ, &v)) f->syncobj = v != 0;
            if (!get_cap(fd, DRM_CAP_SYNCOBJ_TIMELINE, &v)) f->syncobj_timeline = v != 0;
            if (!get_cap(fd, DRM_CAP_PRIME, &v)) { f->prime_import = !!(v & DRM_PRIME_CAP_IMPORT); f->prime_export = !!(v & DRM_PRIME_CAP_EXPORT); }
            close(fd);
        }
    }
    snprintf(path, sizeof path, "/dev/dri/%s", card);
    int fd = open(path, O_RDWR | O_CLOEXEC | O_NOCTTY);
    if (fd >= 0) {
        f->card_node = 1;
        xioctl(fd, DRM_IOCTL_DROP_MASTER, NULL);    /* master only when nobody drives the card: given back at once */
        /* a display-less card (a render-only GPU) answers the mode ioctls with an error: those facts stay unknown */
        if (f->syncobj < 0 && !get_cap(fd, DRM_CAP_SYNCOBJ, &v)) f->syncobj = v != 0;
        if (f->syncobj_timeline < 0 && !get_cap(fd, DRM_CAP_SYNCOBJ_TIMELINE, &v)) f->syncobj_timeline = v != 0;
        if (!get_cap(fd, DRM_CAP_ADDFB2_MODIFIERS, &v)) f->fb_modifiers = v != 0;
        struct drm_set_client_cap cc = { .capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES, .value = 1 };
        int universal = !xioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cc);
        cc = (struct drm_set_client_cap){ .capability = DRM_CLIENT_CAP_ATOMIC, .value = 1 };
        int at = xioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cc);
        struct drm_mode_card_res r = { 0 };
        int kms = !xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r);
        if (kms) {
            f->atomic = !at;
            if (universal) planes(fd, f);
            connectors(fd, f);
        }
        close(fd);
    }
    return f->card_node || f->render_node ? 0 : -1;
}
