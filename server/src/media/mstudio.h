/* mstudio.h — LordOS Studio from lord-server (docs/CAMERA-STUDIO-PLAN.md; AGENTS.md, lord-studio and lord.studio).
 *
 * "LordOS Studio" is one more camera in lord.media.cameras() (device "studio") for LordOS's own apps, isolated .lpk
 * apps (BOP_CAMERA) and packaged apps' companions, and (phase 2, with the optional lordos-studio-camera package) a
 * V4L2 device for every Linux app: the v4l2loopback device /dev/video50, "LordOS Studio".
 *
 * With that device, one lord-studio per session (the hub) is its only writer: the idle card with the real camera
 * closed while nobody reads, the camera with the effects while somebody does (v4l2loopback's client-usage event), the
 * off card when the person turned LordOS Studio off while an app still reads. LordOS's own apps then read the same
 * device through lord-media like any camera, so the camera is opened once and the effects run once. Without the
 * device (the package isn't installed), each LordOS Studio source starts its own lord-studio on the real camera
 * (phase 1). The camera permission is checked exactly as for a device, by the caller. lord-server opens every file:
 * the real camera, the loopback, the render node, the segmenter's model (size and SHA-256 checked against models.h
 * first) and the background picture; the helpers get fds and numbers only. */
#ifndef LORD_MEDIA_MSTUDIO_H
#define LORD_MEDIA_MSTUDIO_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define STUDIO_DEVICE "studio"

/* MediaSrc.studio: what kind of LordOS Studio source it is */
enum { STUDIO_SRC_OWN = 1,      /* phase 1: its own lord-studio on the real camera */
       STUDIO_SRC_HUB = 2,      /* the session's lord-studio that writes the loopback (one at most) */
       STUDIO_SRC_LOOP = 3 };   /* a LordOS app reading the loopback through lord-media */

/* lord-studio and the model are there (checked once; the model's hash at the first open) */
int  mstudio_available(void);
/* lord.media.cameras()' entry, appended to json (empty when unavailable or no real camera): ',{"device":"studio",…}' */
void mstudio_camera_entry(char *json, size_t n, int first);
/* the "LordOS Studio" loopback device itself (its card): never listed as a real camera */
int  mstudio_is_loopback_card(const char *card);
/* A LordOS Studio source for an app (the permission already checked): through the loopback when the hub runs,
 * else its own lord-studio on the real camera. Sets s->sock, s->pid and s->studio. 0 ok, -1 with code (camera.openFailed,
 * media.noHelper, studio.noCamera, studio.unavailable). src: a MediaSrc (media_int.h) just made by media_src_new. */
int  mstudio_open(void *src, int w, int h, const char **code);

/* the settings (lord.studio.set): on, camera "/dev/videoN" ("" = the first), mode 0 none, 1 blur, 2 picture, 3 video
 * (2 and 3 both mean "the background file", whichever it is), blur 0..100, mirror, and phase 3's: key 0 none, 1 green,
 * 2 blue; similarity, smoothness, spill, framing (Auto Framing), light (Face Light), filter_amt and retouch 0..100;
 * match 0..60; filter 0..STUDIO_FILTERS. Out-of-range values are clamped. Every running source gets them now; a new
 * camera applies to sources opened from now on (and to the hub's next open). */
typedef struct {
    int on; const char *camera;
    int mode, blur, mirror, key, similarity, smoothness, spill, framing, light, match, filter, filter_amt, retouch;
} MStudioSet;
int  mstudio_set(const MStudioSet *c);
/* the background (picture or video): "asset:wallpapers/…", "asset:studio/backgrounds/…", a catalog download in
 * /var/lib/lordos/studio or a path inside lord.fs's roots; NULL or "" clears it. 0 ok, -1 refused */
int  mstudio_set_background(const char *path);
/* status JSON for lord.studio.status() */
void mstudio_status(char *json, size_t n);
/* lord-studio's MM_STUDIO_STATS (values checked) */
void mstudio_stats(int src_id, const int64_t v[6]);
/* the hub's MM_STUDIO_READERS (values checked) */
void mstudio_readers(int src_id, long long readers, long long cam_open, long long what);

/* The popover's preview: the hub's own frames while it runs (its id), with the real camera opened for it. 0 when
 * there is no hub (the caller then starts its own source). Ended by mstudio_preview_end. */
int  mstudio_preview_hub(void);
void mstudio_preview_end(void);
/* the hub's source can't be closed or driven from JS (lord.media.close): it belongs to the session */
int  mstudio_is_hub(int src_id);

/* once a second from the main loop (media_reap): starts, stops and drives the hub; finds the loopback's readers */
void mstudio_tick(void);
/* ms until the next tick, -1 with nothing to do */
double mstudio_timeout(void);
/* the capture indicator's event for the readers of the loopback (hw.c: hw_emit, kind "studio-use") */
void mstudio_on_use(void (*emit)(const char *kind, const char *json));

#endif
