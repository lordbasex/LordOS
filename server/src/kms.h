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
    void *flip_data;    /* what the flip pending was queued with (kms_flip_fb), for kms_flip_abandon */
    unsigned flip_seq;
    /* tearing (tearing-control-v1, docs/WAYLAND.md "Frame timing"): async_cap, the driver takes DRM_MODE_PAGE_FLIP_ASYNC
     * on the legacy page flip (DRM_CAP_ASYNC_PAGE_FLIP); async_next, the next kms_flip_fb may tear (gpu_scanout sets it
     * for a fullscreen game's own buffer); flip_async, the last flip queued was one (its time is no vblank's) */
    int async_cap, async_next, flip_async;
    /* hardware cursor (cursor plane, legacy ioctl) */
    uint32_t cursor_handle, cursor_w, cursor_h, cursor_pitch;   /* plane size (the driver's cap) and row in pixels */
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
    /* the modeset after a dropped flip (kms_flip_abandon) runs on a thread of its own: recover_bg asks for it, recover
     * is the one running (kms.c struct recover), recover_back the dumb buffer it shows (the CPU path) */
    int recover_bg, recover_back;
    void *recover, *recover_data;
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

/* A page flip the kernel never completed (virtio-gpu after an s2idle suspend never sent its event: QA 0.8.9.6.2,
 * BUG-B). kms_flip_stuck: 1 when the flip pending was queued more than limit_ns before now_ns. kms_flip_abandon takes
 * it as done (kms_flip_fb's on_flip with its data, else the two buffers swap, as the event would) and makes the next
 * frame a full modeset (SETCRTC, as the first one after the boot splash) instead of a page flip. A late event for it
 * finds no flip pending and is ignored. Nothing when no flip is pending. */
int  kms_flip_stuck(const Kms *k, uint64_t now_ns, uint64_t limit_ns);
void kms_flip_abandon(Kms *k);

/* That full modeset may block in the kernel too: virtio-gpu after an s2idle wake held SETCRTC with the whole desktop
 * (QA ws174). So the one after kms_flip_abandon runs on a thread: the caller waits for it at most KMS_RECOVER_WAIT_MS
 * (a real card's modeset answers well within it, and goes on as before). Past that, the frame counts as a flip pending
 * that no watchdog drops again; kms_recover_poll, called every loop, completes it when the thread answers (on_flip,
 * or the buffers swap) and, if the modeset failed, leaves the next frame to try once more, on a thread again. While it
 * runs, nothing else goes to that CRTC (the cursor waits). kms_recover_busy: modesets still running. */
#define KMS_RECOVER_WAIT_MS 250
void kms_recover_poll(Kms *k);
int  kms_recover_busy(void);
#ifdef KMS_TEST
extern int (*kms_test_setcrtc)(int fd, struct drm_mode_crtc *set);   /* tests: stands in for the ioctl */
#endif

/* Does the primary plane show this format with this modifier? 1 yes (or unknown), 0 no.
 * DRM_FORMAT_MOD_INVALID (implicit) counts as linear. */
int  kms_scanout_ok(const Kms *k, uint32_t fourcc, uint64_t modifier);

/* Hardware cursor: ARGB image (at most the plane, the driver's cursor size: 64×64 to 256×256) and position.
 * 0 if the driver supports it. */
int  kms_cursor_image(Kms *k, const uint32_t *argb, int w, int h, int hot_x, int hot_y);
/* 1 if a w×h image fits this display's cursor plane (made by the first kms_cursor_image). */
int  kms_cursor_fits(const Kms *k, int w, int h);
void kms_cursor_move(Kms *k, int x, int y);
void kms_cursor_show(Kms *k, int visible);

/* ---- outputs and Settings → Displays (see hw/display.c, docs/SETTINGS.md) ---- */

#define KMS_MAX_OUTPUTS 8
typedef struct {
    uint32_t id, type, type_id;         /* connector id; type (DRM_MODE_CONNECTOR_*) and its number */
    int connected;                      /* a desktop display is plugged in (never a non-desktop one) */
    int non_desktop;                    /* plugged in, and the kernel's "non-desktop" property is set (a VR headset) */
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
/* DRM leases (wl/drmlease.c): the CRTCs (bit = GETRESOURCES index) leased now, which no head takes; and the objects
 * of a lease for connector o (a non-desktop one): o itself, a free CRTC that can drive it (not a head's, not in
 * avoid) and that CRTC's primary plane. objs gets 3 ids, *crtc_bit the CRTC's bit. 0 if found. */
void kms_set_leased_crtcs(uint32_t mask);
int  kms_lease_objects(int fd, const KmsOutput *o, uint32_t avoid, uint32_t objs[3], uint32_t *crtc_bit);
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
