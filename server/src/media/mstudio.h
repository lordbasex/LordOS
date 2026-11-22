/* mstudio.h — LordOS Studio from lord-server (docs/CAMERA-STUDIO-PLAN.md; AGENTS.md, lord-studio and lord.studio).
 *
 * "LordOS Studio" is one more camera in lord.media.cameras() (device "studio") for LordOS's own apps, isolated .lpk
 * apps (BOP_CAMERA) and packaged apps' companions: opening it starts lord-studio (lord-media's camera built with
 * LM_STUDIO) on the real camera the person chose, with the effects of the moment. The camera permission is checked
 * exactly as for a device, by the caller. lord-server opens every file: the real camera (/dev/videoN), the render
 * node, the segmenter's model (size and SHA-256 checked against models.h first) and the background picture (a user
 * file inside lord.fs's roots or one of LordOS's wallpapers); the helper gets fds and numbers only. The settings
 * (lord.studio.set, desktop context only) reach every running LordOS Studio source at once. */
#ifndef LORD_MEDIA_MSTUDIO_H
#define LORD_MEDIA_MSTUDIO_H

#include <stddef.h>
#include <sys/types.h>

#define STUDIO_DEVICE "studio"

/* lord-studio and the model are there (checked once; the model's hash at the first open) */
int  mstudio_available(void);
/* lord.media.cameras()' entry, appended to json (empty when unavailable or no real camera): ',{"device":"studio",…}' */
void mstudio_camera_entry(char *json, size_t n, int first);
/* starts lord-studio for a source: the chosen real camera (or the first one), w×h asked. Returns the socket or -1;
 * code: why (camera.openFailed, media.noHelper, studio.noCamera, studio.unavailable) */
int  mstudio_spawn(int w, int h, pid_t *pid, const char **code);
/* a source just spawned: sends it the current effects and background */
void mstudio_attach(int sock);

/* the settings (lord.studio.set): camera "/dev/videoN" ("" = the first), mode 0..2, blur 0..100, mirror. Every running
 * source gets them now; a new camera applies to sources opened from now on */
int  mstudio_set(const char *camera, int mode, int blur, int mirror);
/* the background picture: "asset:wallpapers/…" or a path inside lord.fs's roots; NULL or "" clears it. 0 ok, -1 refused */
int  mstudio_set_background(const char *path);
/* status JSON for lord.studio.status() */
void mstudio_status(char *json, size_t n);
/* lord-studio's MM_STUDIO_STATS (values checked) */
void mstudio_stats(int src_id, long long frames, long long seg_us, long long fx_us, long long gpu);

#endif
