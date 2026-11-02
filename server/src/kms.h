/* kms.h — video output straight to the kernel (DRM/KMS), without libdrm. */
#ifndef LORD_KMS_H
#define LORD_KMS_H

#include <stddef.h>
#include <stdint.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

typedef struct {
    uint32_t fb_id, handle, pitch;
    uint64_t size;
    uint32_t *map;
} KmsBuffer;

typedef struct {
    int fd;
    uint32_t conn_id, crtc_id;
    struct drm_mode_modeinfo mode;
    uint32_t width, height;
    KmsBuffer buf[2];
    int front;          /* buffer shown on screen */
    int flip_pending;   /* waiting for vblank */
    struct drm_mode_crtc saved;
    /* flips of our own framebuffers (GPU path, direct scanout): when done, on_flip is called
     * with what was passed to kms_flip_fb; the dumb buffers above are not used */
    void (*on_flip)(void *data);
    int custom;
    /* last flip: time (CLOCK_MONOTONIC, ns) and vblank number (wp_presentation) */
    uint64_t flip_ns;
    uint64_t flip_submit_ns; /* when that flip was queued (CLOCK_MONOTONIC, ns) */
    unsigned flip_seq;
    /* hardware cursor (cursor plane, legacy ioctl) */
    uint32_t cursor_handle, cursor_w, cursor_h;
    uint32_t *cursor_map;
    uint64_t cursor_size;
    int cursor_ok, cursor_visible;
    int borrowed;       /* the fd came from lord-sessiond (it handles master and the previous mode) */
    /* boot splash hand-off (kms_attach with keep): no modeset until the first real frame, so the
     * splash stays up instead of a black frame. presented: a frame of ours is on the screen. */
    int modeset_pending, presented;
    int shared;         /* a second display (kms_head_open): the fd belongs to the main head */
    /* the CRTC's primary plane: formats and modifiers it accepts (IN_FORMATS property). On the
     * Raspberry Pi (vc4) RGB is only shown linear: buffers with v3d's UIF tiling do not go
     * straight to the screen. scan_known = 0: the driver does not say (ADDFB2 is tried). */
    char driver[32];
    int crtc_index, scan_known, nscan;
    struct { uint32_t fourcc; uint64_t modifier; } scan[96];
} Kms;

int  kms_open(Kms *k, const char *path);
/* With a card fd passed by lord-sessiond (already master): same setup as kms_open. keep: what
 * the screen shows (the boot splash) stays until the first kms_flip/kms_set_fb/kms_flip_fb. */
int  kms_attach(Kms *k, int fd, int keep);
void kms_close(Kms *k);
/* Free buffer to draw on (the one not on screen). */
KmsBuffer *kms_back(Kms *k);
/* Shows the back buffer at the next vblank. */
int  kms_flip(Kms *k);
/* Handles events of the DRM fd (page flip done). */
void kms_dispatch(Kms *k);
/* Several displays on one card: the events go to the head whose CRTC flipped. */
void kms_dispatch_heads(Kms **heads, int n);
/* A second display on the main head's fd: connector, mode, a free CRTC and its own dumb buffers.
 * kms_close turns its CRTC off and leaves the fd open. 0 if it shows. */
int  kms_head_open(Kms *k, int fd, uint32_t conn_id, const struct drm_mode_modeinfo *mode);

/* Framebuffer from GEM handles of the KMS fd (a GBM BO, or an imported dmabuf). With modifier
 * != DRM_FORMAT_MOD_INVALID, DRM_MODE_FB_MODIFIERS is used. 0 on success. */
int  kms_add_fb2(Kms *k, uint32_t w, uint32_t h, uint32_t fourcc, const uint32_t handles[4],
                 const uint32_t pitches[4], const uint32_t offsets[4], uint64_t modifier, uint32_t *fb);
void kms_rm_fb(Kms *k, uint32_t fb);
int  kms_prime_import(Kms *k, int dmabuf_fd, uint32_t *handle);    /* dmabuf → GEM handle */
void kms_close_handle(Kms *k, uint32_t handle);
/* First modeset with our own framebuffer, and page flip to one (data comes back in on_flip). */
int  kms_set_fb(Kms *k, uint32_t fb);
int  kms_flip_fb(Kms *k, uint32_t fb, void *data);

/* Does the primary plane show this format with this modifier? 1 yes (or unknown), 0 no.
 * DRM_FORMAT_MOD_INVALID (implicit) counts as linear. */
int  kms_scanout_ok(const Kms *k, uint32_t fourcc, uint64_t modifier);

/* Hardware cursor: ARGB image (up to 64×64) and position. 0 if the driver supports it. */
int  kms_cursor_image(Kms *k, const uint32_t *argb, int w, int h, int hot_x, int hot_y);
void kms_cursor_move(Kms *k, int x, int y);
void kms_cursor_show(Kms *k, int visible);

/* ---- outputs and Settings → Displays (see hw/display.c, docs/SETTINGS.md) ---- */

#define KMS_MAX_OUTPUTS 8
typedef struct {
    uint32_t id, type, type_id;         /* connector id; type (DRM_MODE_CONNECTOR_*) and its number */
    int connected;
    uint32_t mm_w, mm_h;
    uint32_t possible_crtcs;            /* bit j = the j-th CRTC of GETRESOURCES */
    uint32_t current_crtc;              /* driving it now, 0 if none */
    int nmodes;
    struct drm_mode_modeinfo *modes;    /* malloc */
    uint8_t *edid;                      /* malloc, or NULL */
    size_t edid_len;
} KmsOutput;

/* Every connector of the card (connected or not), with modes and EDID. Free with
 * kms_outputs_free. Returns how many (≤ max). */
int  kms_outputs(int fd, KmsOutput *out, int max);
void kms_outputs_free(KmsOutput *o, int n);
const char *kms_connector_type_name(uint32_t type);        /* "HDMI-A", "eDP", "Virtual"... */
/* Optional choice for kms_open/kms_attach: which connected output (index in o) and mode. Returns
 * 0 if it chose; otherwise the first connected output with its preferred mode is used. */
typedef int (*KmsPicker)(const KmsOutput *o, int n, int *index, struct drm_mode_modeinfo *mode);
void kms_set_picker(KmsPicker fn);
/* Value and property id of a KMS object property by name. 0 if found. */
int  kms_obj_prop(int fd, uint32_t obj, uint32_t type, const char *name, uint64_t *val, uint32_t *prop_id);
/* Runtime modeset (CPU path: the dumb buffers are recreated at the new size). The GPU path must
 * be shut down first and started again after. Not while a flip is pending. On failure the
 * previous mode is restored and -1 returned. */
int  kms_modeset(Kms *k, uint32_t conn_id, const struct drm_mode_modeinfo *mode);
/* Hardware gamma LUT of the CRTC: entries per channel (0 = none, e.g. virtio-gpu), and set it. */
int  kms_gamma_size(Kms *k);
int  kms_set_gamma(Kms *k, int n, const uint16_t *r, const uint16_t *g, const uint16_t *b);
/* Connector DPMS (screen off/on). 0 if the driver took it. */
int  kms_dpms(Kms *k, int on);

#endif
