/* drmprobe.h — the display engine's facts the newer Wayland protocols depend on, read with the kernel's DRM uapi
 * (<drm/drm.h>, <drm/drm_mode.h>; no libdrm) on the nodes this user may open:
 *
 *   - explicit sync (linux-drm-syncobj-v1): DRM_CAP_SYNCOBJ and DRM_CAP_SYNCOBJ_TIMELINE, on the card's render node;
 *   - atomic modesetting: DRM_CLIENT_CAP_ATOMIC accepted on the card node (set on this fd only);
 *   - the planes: how many, of which type (primary, overlay, cursor);
 *   - drm-lease-v1: how many connectors the kernel marks "non-desktop" (a VR headset) — a lease needs one;
 *   - linux-dmabuf-v1's scanout side: the primary plane's IN_FORMATS (formats and modifiers it can show directly),
 *     DRM_CAP_ADDFB2_MODIFIERS and PRIME import/export.
 *
 * Only the CLI calls it (`lordos-sysreport report`, `lordos-sysreport gpu`, so `lordos-issue --gpu`), never
 * lord-server. It changes nothing: no modeset and no connector probe (DRM_IOCTL_MODE_GETCONNECTOR is never called,
 * since it re-reads the monitors). A card nobody drives makes its opener DRM master: the probe drops that at once
 * (DRM_IOCTL_DROP_MASTER) and closes the node after a few reads. Nodes the user can't open stay unknown (-1). */
#ifndef LORD_SYSREPORT_DRMPROBE_H
#define LORD_SYSREPORT_DRMPROBE_H

#include <stddef.h>
#include <stdio.h>

typedef struct {
    int probed;                         /* 1: the probe ran for this card */
    int card_node, render_node;         /* 1 opened, 0 not allowed or absent */
    int syncobj, syncobj_timeline;      /* 1 yes, 0 no, -1 unknown */
    int atomic;                         /* 1 yes, 0 no, -1 unknown */
    int planes, primary, overlay, cursor;          /* -1 unknown */
    int connectors, non_desktop;        /* -1 unknown */
    int fb_modifiers;                   /* DRM_CAP_ADDFB2_MODIFIERS: 1, 0, -1 */
    int prime_import, prime_export;     /* 1, 0, -1 */
    int scanout_formats, scanout_modifiers;        /* the first primary plane's IN_FORMATS; -1 unknown */
} DrmFacts;

/* Everything unknown, probed 0 */
static inline void drmprobe_init(DrmFacts *f) {
    *f = (DrmFacts){ 0 };
    f->syncobj = f->syncobj_timeline = f->atomic = -1;
    f->planes = f->primary = f->overlay = f->cursor = -1;
    f->connectors = f->non_desktop = -1;
    f->fb_modifiers = f->prime_import = f->prime_export = -1;
    f->scanout_formats = f->scanout_modifiers = -1;
}

/* Probes /dev/dri/<card> ("card0") and its render node (from /sys/class/drm/<card>/device/drm). 0 when at least one
 * node opened. */
int drmprobe_card(const char *card, DrmFacts *f);

/* An IN_FORMATS blob (struct drm_format_modifier_blob): the number of formats and of distinct modifiers. Every
 * count and offset is checked against len; 0 when the blob is well formed, -1 otherwise (outputs untouched). */
int drmprobe_in_formats(const void *blob, size_t len, int *formats, int *modifiers);

/* The formatting is header-only (static inline), so the reports that print these facts (gpureport.c, sysreport_out.c,
 * also linked into lord-server for Settings) need no probing code. */

/* "yes", "no", "unknown" */
static inline const char *drmprobe_tri(int v) { return v > 0 ? "yes" : v == 0 ? "no" : "unknown"; }

/* drm-lease's possibility in words: "yes (1 non-desktop connector)", "no (no non-desktop connector)", "unknown" */
static inline const char *drmprobe_lease(const DrmFacts *f, char *buf, size_t n) {
    if (!f->probed || f->non_desktop < 0) snprintf(buf, n, "unknown");
    else if (!f->non_desktop) snprintf(buf, n, "no (no non-desktop connector)");
    else snprintf(buf, n, "yes (%d non-desktop connector%s)", f->non_desktop, f->non_desktop == 1 ? "" : "s");
    return buf;
}

/* a count, or "unknown" */
static inline const char *drmprobe_num(int v, char *buf, size_t n) {
    if (v < 0) snprintf(buf, n, "unknown"); else snprintf(buf, n, "%d", v);
    return buf;
}

/* Two lines for a text report, each starting with indent:
 *   display engine: atomic yes; planes 6 (2 primary, 2 overlay, 2 cursor); scanout formats 12, modifiers 5; framebuffer modifiers yes
 *   explicit sync: syncobj yes, timeline yes; PRIME import yes, export yes; drm-lease: no (no non-desktop connector)
 * or one line saying the nodes couldn't be opened. */
static inline void drmprobe_text(const DrmFacts *f, const char *indent, char *buf, size_t n) {
    char a[16], b[16], c[16], d[16], e[16], g[16], l[48];
    if (!f->card_node && !f->render_node) { snprintf(buf, n, "%sdisplay engine: not read (this user can't open the card's nodes)\n", indent); return; }
    snprintf(buf, n, "%sdisplay engine: atomic %s; planes %s (%s primary, %s overlay, %s cursor); scanout formats %s, modifiers %s; framebuffer modifiers %s\n"
                     "%sexplicit sync: syncobj %s, timeline %s; PRIME import %s, export %s; drm-lease: %s\n",
             indent, f->card_node ? drmprobe_tri(f->atomic) : "unknown (card node not readable)", drmprobe_num(f->planes, a, sizeof a),
             drmprobe_num(f->primary, b, sizeof b), drmprobe_num(f->overlay, c, sizeof c), drmprobe_num(f->cursor, d, sizeof d),
             drmprobe_num(f->scanout_formats, e, sizeof e), drmprobe_num(f->scanout_modifiers, g, sizeof g), drmprobe_tri(f->fb_modifiers),
             indent, drmprobe_tri(f->syncobj), drmprobe_tri(f->syncobj_timeline), drmprobe_tri(f->prime_import), drmprobe_tri(f->prime_export),
             drmprobe_lease(f, l, sizeof l));
}

/* The facts as a JSON object (numbers and booleans only; -1 and null are unknown), or "null" when not probed:
 * {"cardNode":true,"renderNode":true,"atomic":1,"planes":6,"primary":2,"overlay":2,"cursor":2,"scanoutFormats":12,
 *  "scanoutModifiers":5,"fbModifiers":1,"syncobj":1,"syncobjTimeline":1,"primeImport":1,"primeExport":1,"connectors":4,
 *  "nonDesktop":0} */
static inline void drmprobe_json(const DrmFacts *f, char *buf, size_t n) {
    if (!f->probed) { snprintf(buf, n, "null"); return; }
    snprintf(buf, n, "{\"cardNode\":%s,\"renderNode\":%s,\"atomic\":%d,\"planes\":%d,\"primary\":%d,\"overlay\":%d,\"cursor\":%d,"
                     "\"scanoutFormats\":%d,\"scanoutModifiers\":%d,\"fbModifiers\":%d,\"syncobj\":%d,\"syncobjTimeline\":%d,"
                     "\"primeImport\":%d,\"primeExport\":%d,\"connectors\":%d,\"nonDesktop\":%d}",
             f->card_node ? "true" : "false", f->render_node ? "true" : "false", f->atomic, f->planes, f->primary, f->overlay, f->cursor,
             f->scanout_formats, f->scanout_modifiers, f->fb_modifiers, f->syncobj, f->syncobj_timeline, f->prime_import, f->prime_export,
             f->connectors, f->non_desktop);
}

#endif
