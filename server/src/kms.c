/* kms.c — modesetting with kernel ioctls: connector, CRTC, dumb buffers, page flip. */
#include "kms.h"
#include "platform/seat.h"

#include <errno.h>
#include <drm/drm_fourcc.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static int xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do r = ioctl(fd, req, arg);
    while (r == -1 && (errno == EINTR || errno == EAGAIN));
    return r;
}

static int create_buffer(Kms *k, KmsBuffer *b) {
    struct drm_mode_create_dumb c = { .width = k->width, .height = k->height, .bpp = 32 };
    if (xioctl(k->fd, DRM_IOCTL_MODE_CREATE_DUMB, &c)) { perror("CREATE_DUMB"); return -1; }
    b->handle = c.handle; b->pitch = c.pitch; b->size = c.size;

    struct drm_mode_fb_cmd f = { .width = k->width, .height = k->height, .pitch = c.pitch,
                                 .bpp = 32, .depth = 24, .handle = c.handle };
    if (xioctl(k->fd, DRM_IOCTL_MODE_ADDFB, &f)) { perror("ADDFB"); return -1; }
    b->fb_id = f.fb_id;

    struct drm_mode_map_dumb m = { .handle = c.handle };
    if (xioctl(k->fd, DRM_IOCTL_MODE_MAP_DUMB, &m)) { perror("MAP_DUMB"); return -1; }
    b->map = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, k->fd, m.offset);
    if (b->map == MAP_FAILED) { b->map = NULL; perror("mmap"); return -1; }
    memset(b->map, 0, b->size);
    return 0;
}

static void destroy_buffer(Kms *k, KmsBuffer *b) {
    if (b->map) munmap(b->map, b->size);
    if (b->fb_id) xioctl(k->fd, DRM_IOCTL_MODE_RMFB, &b->fb_id);
    if (b->handle) {
        struct drm_mode_destroy_dumb d = { .handle = b->handle };
        xioctl(k->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
    }
    memset(b, 0, sizeof *b);
}

/* ---- outputs (connectors), shared by the initial pick and Settings → Displays ---- */

/* CRTCs (bit = index in GETRESOURCES) driven by a head of this process: a second display never
 * takes the main one's CRTC (docs/DISPLAYS.md). */
static uint32_t busy_crtcs;

static KmsPicker picker;
void kms_set_picker(KmsPicker fn) { picker = fn; }

const char *kms_connector_type_name(uint32_t t) {
    static const char *const names[] = { "Unknown", "VGA", "DVI-I", "DVI-D", "DVI-A", "Composite", "SVIDEO", "LVDS",
        "Component", "DIN", "DP", "HDMI-A", "HDMI-B", "TV", "eDP", "Virtual", "DSI", "DPI", "Writeback", "SPI", "USB" };
    return t < sizeof names / sizeof names[0] ? names[t] : "Unknown";
}

/* Reads the EDID blob of a connector (property "EDID"). */
static void read_edid(int fd, uint32_t conn, KmsOutput *o) {
    uint64_t blob = 0;
    if (kms_obj_prop(fd, conn, DRM_MODE_OBJECT_CONNECTOR, "EDID", &blob, NULL) || !blob) return;
    struct drm_mode_get_blob gb = { .blob_id = (uint32_t)blob };
    if (xioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &gb) || gb.length < 128 || gb.length > 32768) return;
    uint8_t *d = malloc(gb.length);
    if (!d) return;
    gb.data = (uintptr_t)d;
    if (xioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &gb)) { free(d); return; }
    o->edid = d;
    o->edid_len = gb.length;
}

int kms_outputs(int fd, KmsOutput *out, int max) {
    struct drm_mode_card_res res = {0};
    if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) return 0;
    uint32_t *conns = calloc(res.count_connectors + 1, 4), *crtcs = calloc(res.count_crtcs + 1, 4);
    uint32_t *encs = calloc(res.count_encoders + 1, 4), *fbs = calloc(res.count_fbs + 1, 4);
    int n = 0;
    if (!conns || !crtcs || !encs || !fbs) goto out;
    res.connector_id_ptr = (uintptr_t)conns;
    res.crtc_id_ptr = (uintptr_t)crtcs;
    res.encoder_id_ptr = (uintptr_t)encs;
    res.fb_id_ptr = (uintptr_t)fbs;
    if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) goto out;
    for (uint32_t i = 0; i < res.count_connectors && n < max; i++) {
        struct drm_mode_get_connector c = { .connector_id = conns[i] };
        if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c)) continue;
        KmsOutput *o = &out[n];
        memset(o, 0, sizeof *o);
        uint32_t nm = c.count_modes, ne = c.count_encoders;
        struct drm_mode_modeinfo *modes = nm ? calloc(nm, sizeof *modes) : NULL;
        uint32_t *cencs = calloc(ne + 1, 4);
        c.modes_ptr = (uintptr_t)modes;
        c.encoders_ptr = (uintptr_t)cencs;
        c.count_props = 0; c.props_ptr = 0; c.prop_values_ptr = 0;
        if ((nm && !modes) || !cencs || xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c)) { free(modes); free(cencs); continue; }
        if (c.count_modes < nm) nm = c.count_modes;      /* the list can shrink between the two calls */
        if (c.count_encoders < ne) ne = c.count_encoders;
        o->id = c.connector_id;
        o->type = c.connector_type;
        o->type_id = c.connector_type_id;
        o->connected = c.connection == 1;
        o->mm_w = c.mm_width;
        o->mm_h = c.mm_height;
        o->modes = modes;
        o->nmodes = (int)nm;
        o->current_crtc = 0;
        if (c.encoder_id) {
            struct drm_mode_get_encoder e = { .encoder_id = c.encoder_id };
            if (!xioctl(fd, DRM_IOCTL_MODE_GETENCODER, &e)) o->current_crtc = e.crtc_id;
        }
        for (uint32_t e_i = 0; e_i < ne; e_i++) {
            struct drm_mode_get_encoder e = { .encoder_id = cencs[e_i] };
            if (xioctl(fd, DRM_IOCTL_MODE_GETENCODER, &e)) continue;
            for (uint32_t j = 0; j < res.count_crtcs && j < 32; j++)
                if (e.possible_crtcs & (1u << j)) o->possible_crtcs |= 1u << j;
        }
        free(cencs);
        if (o->connected) read_edid(fd, o->id, o);
        n++;
    }
out:
    free(conns); free(crtcs); free(encs); free(fbs);
    return n;
}

void kms_outputs_free(KmsOutput *o, int n) {
    for (int i = 0; i < n; i++) { free(o[i].modes); free(o[i].edid); o[i].modes = NULL; o[i].edid = NULL; }
}

/* CRTC id (and index) able to drive `conn`: the one it already has, else `prefer` if possible,
 * else the first free in possible_crtcs. CRTCs of other heads (busy_crtcs, except `mine`) are
 * skipped. 0 if none. */
static uint32_t crtc_for(int fd, const KmsOutput *o, uint32_t prefer, int *index, int mine) {
    struct drm_mode_card_res res = {0};
    if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) || !res.count_crtcs) return 0;
    uint32_t *crtcs = calloc(res.count_crtcs, 4);
    if (!crtcs) return 0;
    struct drm_mode_card_res r2 = { .crtc_id_ptr = (uintptr_t)crtcs, .count_crtcs = res.count_crtcs };
    uint32_t got = 0;
    if (!xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r2)) {
        uint32_t n = r2.count_crtcs < res.count_crtcs ? r2.count_crtcs : res.count_crtcs;
        uint32_t want[3] = { o->current_crtc, prefer, 0 };
        uint32_t busy = busy_crtcs & ~(mine >= 0 ? 1u << mine : 0);
        for (int w = 0; w < 2 && !got; w++)
            for (uint32_t j = 0; j < n && j < 32 && want[w]; j++)
                if (crtcs[j] == want[w] && (o->possible_crtcs >> j & 1) && !(busy >> j & 1)) { got = crtcs[j]; *index = (int)j; }
        for (uint32_t j = 0; j < n && j < 32 && !got; j++)
            if ((o->possible_crtcs >> j & 1) && !(busy >> j & 1)) { got = crtcs[j]; *index = (int)j; }
    }
    free(crtcs);
    return got;
}

/* Picks a connected connector and its mode (Settings' choice through the picker, else the first
 * connected one with its preferred mode) and a CRTC that can drive it. */
static int pick_output(Kms *k) {
    KmsOutput outs[KMS_MAX_OUTPUTS];
    int n = kms_outputs(k->fd, outs, KMS_MAX_OUTPUTS), sel = -1, ret = -1;
    struct drm_mode_modeinfo mode;
    if (picker && picker(outs, n, &sel, &mode) == 0 && sel >= 0 && sel < n && outs[sel].connected) {
        /* the picker's choice */
    } else {
        sel = -1;
        for (int i = 0; i < n && sel < 0; i++) {
            if (!outs[i].connected || !outs[i].nmodes) continue;
            sel = i;
            mode = outs[i].modes[0];
            for (int m = 0; m < outs[i].nmodes; m++)
                if (outs[i].modes[m].type & DRM_MODE_TYPE_PREFERRED) { mode = outs[i].modes[m]; break; }
        }
    }
    int idx = 0;
    uint32_t crtc = sel >= 0 ? crtc_for(k->fd, &outs[sel], 0, &idx, -1) : 0;
    if (crtc) {
        k->conn_id = outs[sel].id;
        k->crtc_id = crtc;
        k->crtc_index = idx;
        k->mode = mode;
        k->width = mode.hdisplay;
        k->height = mode.vdisplay;
        ret = 0;
    }
    kms_outputs_free(outs, n);
    if (ret) fprintf(stderr, "kms: no monitor connected\n");
    return ret;
}

/* ---- primary plane formats (IN_FORMATS) ---- */

#define PLANE_TYPE_PRIMARY 1        /* the kernel's enum drm_plane_type (not in the uapi) */

/* Value (and property id) of property `name` of a KMS object. 0 if found. */
int kms_obj_prop(int fd, uint32_t obj, uint32_t type, const char *name, uint64_t *val, uint32_t *prop_id) {
    struct drm_mode_obj_get_properties op = { .obj_id = obj, .obj_type = type };
    if (xioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op) || !op.count_props) return -1;
    uint32_t ids[64], n = op.count_props > 64 ? 64 : op.count_props;
    uint64_t vals[64];
    op.count_props = n;
    op.props_ptr = (uintptr_t)ids;
    op.prop_values_ptr = (uintptr_t)vals;
    if (xioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op)) return -1;
    if (op.count_props < n) n = op.count_props;
    for (uint32_t i = 0; i < n; i++) {
        struct drm_mode_get_property pr = { .prop_id = ids[i] };
        if (xioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &pr)) continue;
        pr.name[DRM_PROP_NAME_LEN - 1] = 0;
        if (!strcmp(pr.name, name)) { *val = vals[i]; if (prop_id) *prop_id = ids[i]; return 0; }
    }
    return -1;
}
static int obj_prop(int fd, uint32_t obj, uint32_t type, const char *name, uint64_t *val) { return kms_obj_prop(fd, obj, type, name, val, NULL); }

static int rgb32(uint32_t f) {
    return f == DRM_FORMAT_XRGB8888 || f == DRM_FORMAT_ARGB8888 || f == DRM_FORMAT_XBGR8888 || f == DRM_FORMAT_ABGR8888;
}

/* Reads the IN_FORMATS blob (struct drm_format_modifier_blob) and keeps the 32-bit RGB pairs. */
static void parse_in_formats(Kms *k, uint32_t blob_id) {
    struct drm_mode_get_blob gb = { .blob_id = blob_id };
    if (xioctl(k->fd, DRM_IOCTL_MODE_GETPROPBLOB, &gb) || gb.length < sizeof(struct drm_format_modifier_blob) || gb.length > (1u << 20)) return;
    uint8_t *d = malloc(gb.length);
    if (!d) return;
    gb.data = (uintptr_t)d;
    if (xioctl(k->fd, DRM_IOCTL_MODE_GETPROPBLOB, &gb)) { free(d); return; }
    struct drm_format_modifier_blob h;
    memcpy(&h, d, sizeof h);
    uint64_t len = gb.length;
    if ((uint64_t)h.formats_offset + (uint64_t)h.count_formats * 4 > len ||
        (uint64_t)h.modifiers_offset + (uint64_t)h.count_modifiers * sizeof(struct drm_format_modifier) > len) { free(d); return; }
    k->nscan = 0;
    for (uint32_t m = 0; m < h.count_modifiers; m++) {
        struct drm_format_modifier fm;
        memcpy(&fm, d + h.modifiers_offset + (size_t)m * sizeof fm, sizeof fm);
        for (int b = 0; b < 64; b++) {
            if (!(fm.formats >> b & 1)) continue;
            uint64_t idx = (uint64_t)fm.offset + (uint64_t)b;
            if (idx >= h.count_formats) break;
            uint32_t f;
            memcpy(&f, d + h.formats_offset + idx * 4, 4);
            if (!rgb32(f) || k->nscan >= (int)(sizeof k->scan / sizeof k->scan[0])) continue;
            k->scan[k->nscan].fourcc = f;
            k->scan[k->nscan].modifier = fm.modifier;
            k->nscan++;
        }
    }
    k->scan_known = 1;
    free(d);
}

/* Finds the CRTC's primary plane and what it accepts. Without IN_FORMATS (old drivers)
 * scan_known stays 0 and direct scanout is tried with ADDFB2 as before. */
static void read_scanout_formats(Kms *k) {
    struct drm_version v;
    memset(&v, 0, sizeof v);
    v.name = k->driver;
    v.name_len = sizeof k->driver - 1;
    if (xioctl(k->fd, DRM_IOCTL_VERSION, &v)) snprintf(k->driver, sizeof k->driver, "?");
    else k->driver[v.name_len < sizeof k->driver - 1 ? v.name_len : sizeof k->driver - 1] = 0;
    struct drm_set_client_cap cap = { .capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES, .value = 1 };
    if (xioctl(k->fd, DRM_IOCTL_SET_CLIENT_CAP, &cap)) return;
    struct drm_mode_get_plane_res pr = {0};
    if (xioctl(k->fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr) || !pr.count_planes) return;
    uint32_t planes[64], np = pr.count_planes > 64 ? 64 : pr.count_planes;
    pr.count_planes = np;
    pr.plane_id_ptr = (uintptr_t)planes;
    if (xioctl(k->fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr)) return;
    if (pr.count_planes < np) np = pr.count_planes;
    for (uint32_t i = 0; i < np; i++) {
        struct drm_mode_get_plane gp = { .plane_id = planes[i] };
        if (xioctl(k->fd, DRM_IOCTL_MODE_GETPLANE, &gp) || !(gp.possible_crtcs >> k->crtc_index & 1)) continue;
        uint64_t type = 0, blob = 0;
        if (obj_prop(k->fd, planes[i], DRM_MODE_OBJECT_PLANE, "type", &type) || type != PLANE_TYPE_PRIMARY) continue;
        if (!obj_prop(k->fd, planes[i], DRM_MODE_OBJECT_PLANE, "IN_FORMATS", &blob) && blob) parse_in_formats(k, (uint32_t)blob);
        break;
    }
    if (!k->scan_known) { fprintf(stderr, "kms: card %s, the primary plane reports no modifiers\n", k->driver); return; }
    fprintf(stderr, "kms: card %s, XRGB8888 on the primary plane with modifiers:", k->driver);
    for (int i = 0; i < k->nscan; i++)
        if (k->scan[i].fourcc == DRM_FORMAT_XRGB8888) fprintf(stderr, " 0x%llx", (unsigned long long)k->scan[i].modifier);
    fputc('\n', stderr);
}

int kms_scanout_ok(const Kms *k, uint32_t fourcc, uint64_t modifier) {
    if (!k->scan_known) return 1;
    uint64_t m = modifier == DRM_FORMAT_MOD_INVALID ? DRM_FORMAT_MOD_LINEAR : modifier;
    for (int i = 0; i < k->nscan; i++)
        if (k->scan[i].fourcc == fourcc && k->scan[i].modifier == m) return 1;
    return 0;
}

static int kms_setup(Kms *k, int keep) {
    if (pick_output(k)) return -1;
    read_scanout_formats(k);
    k->saved.crtc_id = k->crtc_id;
    xioctl(k->fd, DRM_IOCTL_MODE_GETCRTC, &k->saved);
    if (create_buffer(k, &k->buf[0]) || create_buffer(k, &k->buf[1])) return -1;
    /* the first frame in black (not what the previous session left) */
    memset(k->buf[0].map, 0, k->buf[0].size);
    memset(k->buf[1].map, 0, k->buf[1].size);
    k->front = 0;
    if (keep) {
        /* the boot splash stays on the screen until our first frame (the modeset is done there) */
        k->modeset_pending = 1;
    } else {
        struct drm_mode_crtc set = {
            .crtc_id = k->crtc_id, .fb_id = k->buf[0].fb_id,
            .set_connectors_ptr = (uintptr_t)&k->conn_id, .count_connectors = 1,
            .mode = k->mode, .mode_valid = 1,
        };
        if (xioctl(k->fd, DRM_IOCTL_MODE_SETCRTC, &set)) { perror("SETCRTC"); return -1; }
        k->presented = 1;
    }
    busy_crtcs |= 1u << k->crtc_index;
    fprintf(stderr, "kms: %ux%u@%u on connector %u, crtc %u\n",
            k->width, k->height, k->mode.vrefresh, k->conn_id, k->crtc_id);
    return 0;
}

int kms_head_open(Kms *k, int fd, uint32_t conn_id, const struct drm_mode_modeinfo *mode) {
    memset(k, 0, sizeof *k);
    k->fd = fd;
    k->shared = 1;
    k->borrowed = 1;
    KmsOutput outs[KMS_MAX_OUTPUTS];
    int n = kms_outputs(fd, outs, KMS_MAX_OUTPUTS), sel = -1, idx = 0;
    for (int i = 0; i < n; i++) if (outs[i].id == conn_id && outs[i].connected) sel = i;
    uint32_t crtc = sel >= 0 ? crtc_for(fd, &outs[sel], 0, &idx, -1) : 0;
    kms_outputs_free(outs, n);
    if (!crtc) { fprintf(stderr, "kms: no free CRTC for connector %u\n", conn_id); k->fd = -1; return -1; }
    k->conn_id = conn_id;
    k->crtc_id = crtc;
    k->crtc_index = idx;
    k->mode = *mode;
    k->width = mode->hdisplay;
    k->height = mode->vdisplay;
    read_scanout_formats(k);
    k->saved.crtc_id = k->crtc_id;
    xioctl(fd, DRM_IOCTL_MODE_GETCRTC, &k->saved);
    if (create_buffer(k, &k->buf[0]) || create_buffer(k, &k->buf[1])) { kms_close(k); return -1; }
    struct drm_mode_crtc set = { .crtc_id = k->crtc_id, .fb_id = k->buf[0].fb_id,
        .set_connectors_ptr = (uintptr_t)&k->conn_id, .count_connectors = 1, .mode = k->mode, .mode_valid = 1 };
    if (xioctl(fd, DRM_IOCTL_MODE_SETCRTC, &set)) { perror("kms: SETCRTC (second display)"); kms_close(k); return -1; }
    busy_crtcs |= 1u << k->crtc_index;
    fprintf(stderr, "kms: display %ux%u@%u on connector %u, crtc %u\n", k->width, k->height, k->mode.vrefresh, conn_id, crtc);
    return 0;
}

int kms_open(Kms *k, const char *path) {
    memset(k, 0, sizeof *k);
    k->fd = seat_device_open(path, O_RDWR | O_CLOEXEC);
    if (k->fd < 0) { perror(path); return -1; }
    if (!seat_managed() && xioctl(k->fd, DRM_IOCTL_SET_MASTER, NULL))
        fprintf(stderr, "kms: warning, we are not DRM master (%s)\n", strerror(errno));
    if (kms_setup(k, 0)) { kms_close(k); return -1; }
    return 0;
}

int kms_attach(Kms *k, int fd, int keep) {
    memset(k, 0, sizeof *k);
    k->fd = fd;
    k->borrowed = 1;
    if (kms_setup(k, keep)) { kms_close(k); return -1; }
    return 0;
}

void kms_close(Kms *k) {
    if (k->fd <= 0) return;
    if (k->crtc_id) busy_crtcs &= ~(1u << k->crtc_index);
    if (k->cursor_handle) {
        struct drm_mode_cursor2 c = { .flags = DRM_MODE_CURSOR_BO, .crtc_id = k->crtc_id };
        xioctl(k->fd, DRM_IOCTL_MODE_CURSOR2, &c);
        if (k->cursor_map) munmap(k->cursor_map, k->cursor_size);
        struct drm_mode_destroy_dumb d = { .handle = k->cursor_handle };
        xioctl(k->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
        k->cursor_handle = 0;
    }
    if (k->saved.mode_valid && k->saved.fb_id && !k->borrowed) {
        struct drm_mode_crtc r = k->saved;
        r.set_connectors_ptr = (uintptr_t)&k->conn_id;
        r.count_connectors = 1;
        xioctl(k->fd, DRM_IOCTL_MODE_SETCRTC, &r);
    }
    /* borrowed (kms_attach): the next one to show puts up its frame; lord-sessiond handles
     * master. The dumb buffers are destroyed after another one took the screen. */
    if (k->shared && k->crtc_id) {             /* a second display: its CRTC stops scanning out */
        struct drm_mode_crtc off = { .crtc_id = k->crtc_id };
        xioctl(k->fd, DRM_IOCTL_MODE_SETCRTC, &off);
    }
    destroy_buffer(k, &k->buf[0]);
    destroy_buffer(k, &k->buf[1]);
    if (k->shared) { k->fd = -1; return; }     /* the fd belongs to the main head */
    if (!k->borrowed && !seat_managed()) xioctl(k->fd, DRM_IOCTL_DROP_MASTER, NULL);
    seat_device_close(k->fd);
    k->fd = -1;
}

KmsBuffer *kms_back(Kms *k) { return &k->buf[k->front ^ 1]; }

int kms_flip(Kms *k) {
    int back = k->front ^ 1;
    if (k->modeset_pending) {               /* the first frame after the boot splash: modeset */
        if (kms_set_fb(k, k->buf[back].fb_id)) { perror("SETCRTC"); return -1; }
        k->front = back;
        return 0;
    }
    struct drm_mode_crtc_page_flip pf = {
        .crtc_id = k->crtc_id, .fb_id = k->buf[back].fb_id,
        .flags = DRM_MODE_PAGE_FLIP_EVENT,
    };
    if (xioctl(k->fd, DRM_IOCTL_MODE_PAGE_FLIP, &pf) == 0) {
        k->flip_pending = 1;
        return 0;
    }
    /* Synchronous fallback if the driver does not support page flip. */
    struct drm_mode_crtc set = {
        .crtc_id = k->crtc_id, .fb_id = k->buf[back].fb_id,
        .set_connectors_ptr = (uintptr_t)&k->conn_id, .count_connectors = 1,
        .mode = k->mode, .mode_valid = 1,
    };
    if (xioctl(k->fd, DRM_IOCTL_MODE_SETCRTC, &set)) { perror("SETCRTC"); return -1; }
    k->front = back;
    return 0;
}

void kms_dispatch(Kms *k) { kms_dispatch_heads(&k, 1); }

/* Flip events of every head on the card's fd, routed by CRTC (drm_event_vblank.crtc_id, Linux
 * 4.12+; without it the first head waiting for a flip takes it). */
void kms_dispatch_heads(Kms **heads, int nh) {
    if (nh < 1 || heads[0]->fd < 0) return;
    /* aligned: events carry 64-bit fields (user_data) and are read with a cast */
    char buf[1024] __attribute__((aligned(8)));
    ssize_t n = read(heads[0]->fd, buf, sizeof buf);
    for (ssize_t off = 0; n > 0 && off + (ssize_t)sizeof(struct drm_event) <= n;) {
        struct drm_event *e = (struct drm_event *)(buf + off);
        Kms *k = NULL;
        if (e->type == DRM_EVENT_FLIP_COMPLETE && e->length >= sizeof(struct drm_event_vblank)) {
            const struct drm_event_vblank *v = (const struct drm_event_vblank *)e;
            for (int i = 0; i < nh && !k; i++) if (v->crtc_id && heads[i]->crtc_id == v->crtc_id) k = heads[i];
            for (int i = 0; i < nh && !k; i++) if (heads[i]->flip_pending) k = heads[i];
        }
        if (!k) { if (e->length == 0) break; off += e->length; continue; }
        if (e->type == DRM_EVENT_FLIP_COMPLETE && e->length >= sizeof(struct drm_event_vblank)) {
            const struct drm_event_vblank *v = (const struct drm_event_vblank *)e;
            k->flip_ns = (uint64_t)v->tv_sec * 1000000000ull + (uint64_t)v->tv_usec * 1000ull;
            k->flip_seq = v->sequence;
        }
        if (e->type == DRM_EVENT_FLIP_COMPLETE && k->flip_pending) {
            k->flip_pending = 0;
            if (k->custom) {
                const struct drm_event_vblank *v = (const struct drm_event_vblank *)e;
                if (k->on_flip) k->on_flip((void *)(uintptr_t)v->user_data);
            } else k->front ^= 1;
        }
        if (e->length == 0) break;
        off += e->length;
    }
}

/* ---- our own framebuffers (GPU and direct scanout) ---- */

int kms_add_fb2(Kms *k, uint32_t w, uint32_t h, uint32_t fourcc, const uint32_t handles[4],
                const uint32_t pitches[4], const uint32_t offsets[4], uint64_t modifier, uint32_t *fb) {
    struct drm_mode_fb_cmd2 f = { .width = w, .height = h, .pixel_format = fourcc };
    for (int i = 0; i < 4; i++) {
        f.handles[i] = handles[i];
        f.pitches[i] = pitches[i];
        f.offsets[i] = offsets[i];
        if (handles[i] && modifier != DRM_FORMAT_MOD_INVALID) f.modifier[i] = modifier;
    }
    if (modifier != DRM_FORMAT_MOD_INVALID) f.flags = DRM_MODE_FB_MODIFIERS;
    if (xioctl(k->fd, DRM_IOCTL_MODE_ADDFB2, &f)) return -1;
    *fb = f.fb_id;
    return 0;
}

void kms_rm_fb(Kms *k, uint32_t fb) { if (fb) xioctl(k->fd, DRM_IOCTL_MODE_RMFB, &fb); }

int kms_prime_import(Kms *k, int dmabuf_fd, uint32_t *handle) {
    struct drm_prime_handle p = { .fd = dmabuf_fd };
    if (xioctl(k->fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &p)) return -1;
    *handle = p.handle;
    return 0;
}

void kms_close_handle(Kms *k, uint32_t handle) {
    if (!handle) return;
    struct drm_gem_close c = { .handle = handle };
    xioctl(k->fd, DRM_IOCTL_GEM_CLOSE, &c);
}

int kms_set_fb(Kms *k, uint32_t fb) {
    struct drm_mode_crtc set = {
        .crtc_id = k->crtc_id, .fb_id = fb,
        .set_connectors_ptr = (uintptr_t)&k->conn_id, .count_connectors = 1,
        .mode = k->mode, .mode_valid = 1,
    };
    if (xioctl(k->fd, DRM_IOCTL_MODE_SETCRTC, &set)) return -1;
    k->modeset_pending = 0;
    k->presented = 1;
    return 0;
}

int kms_flip_fb(Kms *k, uint32_t fb, void *data) {
    struct drm_mode_crtc_page_flip pf = {
        .crtc_id = k->crtc_id, .fb_id = fb, .flags = DRM_MODE_PAGE_FLIP_EVENT, .user_data = (uintptr_t)data,
    };
    k->custom = 1;
    if (!k->modeset_pending && xioctl(k->fd, DRM_IOCTL_MODE_PAGE_FLIP, &pf) == 0) { k->flip_pending = 1; return 0; }
    /* no page flip: synchronous modeset and notify right away */
    if (kms_set_fb(k, fb)) return -1;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    k->flip_ns = (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
    k->flip_seq++;
    if (k->on_flip) k->on_flip(data);
    return 0;
}

/* ---- hardware cursor ---- */

int kms_cursor_image(Kms *k, const uint32_t *argb, int w, int h, int hot_x, int hot_y) {
    if (!k->cursor_handle) {
        struct drm_mode_create_dumb c = { .width = 64, .height = 64, .bpp = 32 };
        if (xioctl(k->fd, DRM_IOCTL_MODE_CREATE_DUMB, &c)) return -1;
        struct drm_mode_map_dumb m = { .handle = c.handle };
        void *map = xioctl(k->fd, DRM_IOCTL_MODE_MAP_DUMB, &m) ? MAP_FAILED
                  : mmap(NULL, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, k->fd, m.offset);
        if (map == MAP_FAILED) {
            struct drm_mode_destroy_dumb d = { .handle = c.handle };
            xioctl(k->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
            return -1;
        }
        k->cursor_handle = c.handle;
        k->cursor_map = map;
        k->cursor_size = c.size;
        k->cursor_w = c.pitch / 4;
        k->cursor_h = 64;
    }
    memset(k->cursor_map, 0, k->cursor_size);
    for (int y = 0; y < h && y < 64; y++)
        for (int x = 0; x < w && x < 64; x++) k->cursor_map[(size_t)y * k->cursor_w + x] = argb[(size_t)y * w + x];
    struct drm_mode_cursor2 c = { .flags = DRM_MODE_CURSOR_BO, .crtc_id = k->crtc_id, .width = 64, .height = 64,
                                  .handle = k->cursor_handle, .hot_x = hot_x, .hot_y = hot_y };
    k->cursor_ok = xioctl(k->fd, DRM_IOCTL_MODE_CURSOR2, &c) == 0;
    k->cursor_visible = k->cursor_ok;
    return k->cursor_ok ? 0 : -1;
}

void kms_cursor_move(Kms *k, int x, int y) {
    if (!k->cursor_ok || !k->cursor_visible) return;
    struct drm_mode_cursor c = { .flags = DRM_MODE_CURSOR_MOVE, .crtc_id = k->crtc_id, .x = x, .y = y };
    xioctl(k->fd, DRM_IOCTL_MODE_CURSOR, &c);
}

void kms_cursor_show(Kms *k, int visible) {
    if (!k->cursor_ok || k->cursor_visible == visible) return;
    struct drm_mode_cursor2 c = { .flags = DRM_MODE_CURSOR_BO, .crtc_id = k->crtc_id, .width = 64, .height = 64,
                                  .handle = visible ? k->cursor_handle : 0 };
    xioctl(k->fd, DRM_IOCTL_MODE_CURSOR2, &c);
    k->cursor_visible = visible;
}

/* ---- Settings → Displays: change the mode at runtime, gamma, DPMS ---- */

int kms_modeset(Kms *k, uint32_t conn_id, const struct drm_mode_modeinfo *mode) {
    if (k->fd < 0 || k->flip_pending) return -1;
    KmsOutput outs[KMS_MAX_OUTPUTS];
    int n = kms_outputs(k->fd, outs, KMS_MAX_OUTPUTS), sel = -1;
    for (int i = 0; i < n; i++) if (outs[i].id == conn_id && outs[i].connected) sel = i;
    int idx = k->crtc_index;
    uint32_t crtc = sel >= 0 ? crtc_for(k->fd, &outs[sel], k->crtc_id, &idx, k->crtc_id ? k->crtc_index : -1) : 0;
    kms_outputs_free(outs, n);
    if (!crtc) return -1;
    Kms old = *k;
    destroy_buffer(k, &k->buf[0]);
    destroy_buffer(k, &k->buf[1]);
    k->conn_id = conn_id;
    k->crtc_id = crtc;
    k->crtc_index = idx;
    k->mode = *mode;
    k->width = mode->hdisplay;
    k->height = mode->vdisplay;
    k->custom = 0;
    k->front = 0;
    int ok = create_buffer(k, &k->buf[0]) == 0 && create_buffer(k, &k->buf[1]) == 0;
    if (ok && crtc != old.crtc_id) {           /* the old CRTC stops scanning out */
        struct drm_mode_crtc off = { .crtc_id = old.crtc_id };
        xioctl(k->fd, DRM_IOCTL_MODE_SETCRTC, &off);
        busy_crtcs = (busy_crtcs & ~(1u << old.crtc_index)) | 1u << idx;
    }
    if (ok) {
        struct drm_mode_crtc set = { .crtc_id = k->crtc_id, .fb_id = k->buf[0].fb_id,
            .set_connectors_ptr = (uintptr_t)&k->conn_id, .count_connectors = 1, .mode = k->mode, .mode_valid = 1 };
        ok = xioctl(k->fd, DRM_IOCTL_MODE_SETCRTC, &set) == 0;
        if (!ok) perror("kms: SETCRTC (new mode)");
        else { k->modeset_pending = 0; k->presented = 1; }
    }
    if (!ok) {                                  /* back to the previous mode */
        destroy_buffer(k, &k->buf[0]);
        destroy_buffer(k, &k->buf[1]);
        k->conn_id = old.conn_id; k->crtc_id = old.crtc_id; k->crtc_index = old.crtc_index;
        k->mode = old.mode; k->width = old.width; k->height = old.height;
        if (create_buffer(k, &k->buf[0]) == 0 && create_buffer(k, &k->buf[1]) == 0) {
            struct drm_mode_crtc set = { .crtc_id = k->crtc_id, .fb_id = k->buf[0].fb_id,
                .set_connectors_ptr = (uintptr_t)&k->conn_id, .count_connectors = 1, .mode = k->mode, .mode_valid = 1 };
            xioctl(k->fd, DRM_IOCTL_MODE_SETCRTC, &set);
        }
        return -1;
    }
    if (crtc != old.crtc_id) { k->scan_known = 0; k->nscan = 0; read_scanout_formats(k); k->cursor_ok = 0; }
    fprintf(stderr, "kms: mode %ux%u@%u on connector %u, crtc %u\n", k->width, k->height, k->mode.vrefresh, k->conn_id, k->crtc_id);
    return 0;
}

int kms_gamma_size(Kms *k) {
    struct drm_mode_crtc c = { .crtc_id = k->crtc_id };
    if (k->fd < 0 || xioctl(k->fd, DRM_IOCTL_MODE_GETCRTC, &c)) return 0;
    return (int)c.gamma_size;
}

int kms_set_gamma(Kms *k, int n, const uint16_t *r, const uint16_t *g, const uint16_t *b) {
    struct drm_mode_crtc_lut l = { .crtc_id = k->crtc_id, .gamma_size = (uint32_t)n,
        .red = (uintptr_t)r, .green = (uintptr_t)g, .blue = (uintptr_t)b };
    return k->fd < 0 || n <= 0 ? -1 : xioctl(k->fd, DRM_IOCTL_MODE_SETGAMMA, &l);
}

int kms_dpms(Kms *k, int on) {
    uint64_t v = 0;
    uint32_t id = 0;
    if (k->fd < 0 || kms_obj_prop(k->fd, k->conn_id, DRM_MODE_OBJECT_CONNECTOR, "DPMS", &v, &id)) return -1;
    struct drm_mode_connector_set_property sp = { .value = on ? DRM_MODE_DPMS_ON : DRM_MODE_DPMS_OFF, .prop_id = id, .connector_id = k->conn_id };
    return xioctl(k->fd, DRM_IOCTL_MODE_SETPROPERTY, &sp);
}
