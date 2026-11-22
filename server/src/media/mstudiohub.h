/* mstudiohub.h — LordOS Studio's camera for every app, the parts of lord-server's side that are plain logic
 * (docs/CAMERA-STUDIO-PLAN.md 2.2 and 8.2; phase 2). mstudio.c runs the session's one lord-studio that writes the
 * "LordOS Studio" loopback device; this file decides what it shows, finds who reads it and words that for the capture
 * indicator. No lord-server state here, so build/studio-hub-test checks it alone. */
#ifndef LORD_MEDIA_MSTUDIOHUB_H
#define LORD_MEDIA_MSTUDIOHUB_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* the loopback LordOS Studio writes: the number and the label the package's /etc/modprobe.d file sets
 * (packaging: tools/iso/mkdeb-studiocamera.sh) */
#define STUDIO_LOOPBACK_DEV "/dev/video50"
#define STUDIO_LOOPBACK_CARD "LordOS Studio"
#define STUDIO_LOOPBACK_DRIVER "v4l2 loopback"
#define STUDIO_V4L2_MAJOR 81
/* The package's second device is OBS Studio's own virtual camera (OBS takes the first idle loopback in version order,
 * so it sits before ours). LordOS never opens it to write: only STUDIO_LOOPBACK_DEV, after the driver and card checks
 * (mstudio.c loopback_open, and the helper checks the card again). studio_package_test.sh keeps these in step. */
#define STUDIO_OBS_DEV "/dev/video49"
#define STUDIO_OBS_CARD "OBS Virtual Camera"

enum { STUDIO_SHOW_IDLE = 0, STUDIO_SHOW_CAMERA = 1, STUDIO_SHOW_OFF = 2 };

/* What the loopback shows. on: LordOS Studio is on; readers: streaming readers (-1 unknown: the module sends no
 * client-usage event, so scanned is used, the openers lord-server found); preview: the popover's preview wants
 * frames (and the "no camera" restriction of LordOS's apps doesn't forbid it). The real camera is open only for
 * STUDIO_SHOW_CAMERA. */
int studio_hub_show(int on, int readers, int scanned, int preview);
/* Whether the session's lord-studio should run: on, the device there, and the model good; or off while an app still
 * reads the device (it then gets the off card until it lets go, never the raw camera) */
int studio_hub_wanted(int on, int available, int device, int readers, int scanned);

/* The processes that have the loopback open (st_rdev rdev) under proc (normally "/proc"): the same uid only, never self
 * nor self's children (lord-server's own helpers: lord-studio itself, or lord-media reading it for a LordOS app, whose
 * use the shell already shows). pids out, at most max. Returns how many there are. */
int studio_scan_readers(const char *proc, dev_t rdev, pid_t self, uid_t uid, pid_t *out, int max);

/* The indicator's event ("studio-use"): {"apps":["desktop:chromium",""],"camera":"…","effects":true}; apps: n ids of
 * fixed width stride ("" an app LordOS can't name); duplicates said once; an id that isn't plain is said as ""; the
 * camera's name is escaped. -1 when it doesn't fit. */
int studio_use_format(const char *apps, size_t stride, int n, const char *camera, int effects, char *out, size_t cap);

/* ---- phase 3: the effects' settings and the background's name (docs/CAMERA-STUDIO-PLAN.md 4.4, 5, 6) ---- */

/* the filters, in st.h's order (ST_FILTER_*): "none", "warm", … "sepia" */
#define STUDIO_FILTERS 8
const char *studio_filter_name(int id);             /* "none" for an unknown id */
int studio_filter_id(const char *name);             /* -1 for an unknown name */
/* MM_STUDIO's numbers (proto.h): one byte per value, each 0..255 (out-of-range values are clamped to 0..100, or 0..3
 * for the modes and keys, 0..8 for the filter) */
void studio_pack_config(int mode, int key, int filter, int mirror, int blur, int similarity, int smoothness, int spill, int framing,
                        int light, int match, int filter_amt, int retouch, int64_t v[6]);
/* The background's name as lord.studio.setBackground gives it, before any file is looked at: 0 when it is one of
 *   "asset:wallpapers/<file>", "asset:studio/backgrounds/<file>" (plain names, no "..")
 *   "/var/lib/lordos/studio/<64 hex>.jpg" or ".mp4" (what studio-bg-get writes)
 *   another absolute path (checked against lord.fs's roots by the caller)
 * and nothing longer than PATH_MAX or with a control character; -1 otherwise. */
int studio_bg_path_ok(const char *path);

#endif
