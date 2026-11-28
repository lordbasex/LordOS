/* camuse.h — the capture indicator's camera rows for what opens a camera itself (sdk/capturestatus.jsx; docs/WEBAPPS.md,
 * "Cameras").
 *
 * LordOS shows what its own apps capture (lord.media, the broker), what Linux apps capture through PipeWire (pwguard.c)
 * and who reads LordOS Studio's camera (mstudio.c). A web app's camera is none of these: WebKit's web process opens the
 * video4linux device itself (/dev/videoN, bound into its sandbox by WebKit), and so does a Linux app using V4L2 directly.
 * Here the kernel tells: an inotify watch on every /dev/video* node wakes the loop when one is opened or closed (a watch
 * is on the inode, so it fires for an open inside another mount namespace too), and a short while later the user's
 * processes holding a video4linux device are found under /proc (camera_scan_holders) and named by their cgroup
 * (appid_of_pid): hw event "camera-use", {"apps":["desktop:lordos-web-<app>", ""]}, only when the list changes. While
 * somebody holds one it is checked again every few seconds too. Only to show it, never for a decision: Turn off has
 * nothing to act on for these rows. No privileges: the watch needs read access to the node, as opening it does. */
#ifndef LORD_HW_CAMUSE_H
#define LORD_HW_CAMUSE_H

void   camuse_start(void (*emit)(const char *kind, const char *json));
int    camuse_fd(void);                 /* the inotify fd, -1 when there is none */
void   camuse_dispatch(void);           /* the fd is readable: a camera was opened or closed */
void   camuse_rewatch(void);            /* a camera came or went (a video4linux uevent): watch the nodes there are now */
void   camuse_tick(double now_ms);      /* once per loop turn: the scan, when it is due */
double camuse_timeout(double now_ms);   /* ms to the next scan, or -1 */

#endif
