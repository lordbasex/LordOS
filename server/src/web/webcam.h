/* webcam.h — the cameras in WebKit's web process sandbox (docs/WEBAPPS.md, "Cameras"), pure (tests/webua_test.c).
 *
 * In WPE WebKit 2.54 a page's camera is found and opened in its web process, inside WebKit's bubblewrap sandbox:
 * GStreamer's v4l2 device provider lists the video4linux devices through libudev, and libudev counts a device with a
 * device node only once udev has finished with it, which it reads from udev's database, /run/udev/data/c<major>:<minor>.
 * WebKit's sandbox already has every /dev/video* node (BubblewrapLauncher's bindV4l, for hardware video decoding) and
 * /sys/class, /sys/devices read-only, but not /run/udev: every camera was skipped as "not initialized", the list was
 * empty and getUserMedia({ video }) failed with OverconstrainedError. (PipeWire and the Camera portal, the other way
 * WebKit has, is compiled out for WPE: PipeWireCaptureDeviceManager returns at once under PLATFORM(WPE).)
 *
 * lord-web adds to the sandbox, read-only, only the database entries of the video4linux nodes (char major 81) that
 * exist when its web context is made, each one a file: never /run/udev, never another device's entry. The paths are
 * made from the numbers read here, never from a name read from the file system. Which site may use a camera is still
 * the person's decision, asked by LordOS (webengine.c, on_permission). */
#ifndef LORD_WEB_WEBCAM_H
#define LORD_WEB_WEBCAM_H

#define WEBCAM_MAX       32             /* video nodes taken at most */
#define WEBCAM_PATH_MAX  40             /* "/run/udev/data/c81:<minor>" */
#define WEBCAM_V4L_MAJOR 81             /* the kernel's VIDEO_MAJOR */

/* The udev database entries to add to WebKit's sandbox, sorted by minor, into out: their count. root is "" on the
 * system (a test passes a folder that holds sys/class/video4linux, dev and run/udev/data). A node counts only when
 * sys/class/video4linux/video<N>/dev reads exactly "81:<minor>", dev/video<N> is a character device with that number
 * (not a link) and run/udev/data/c81:<minor> is a regular file (not a link). The paths written are the system's
 * ("/run/udev/data/c81:<minor>"), without root. */
int webcam_udev_paths(const char *root, char out[][WEBCAM_PATH_MAX], int max);

/* One node's checks, as webcam_udev_paths makes them: name ("video<N>", one to four digits), the text of its sysfs dev
 * file, its /dev node's st_mode and st_rdev (lstat) and its database entry's st_mode (lstat): the minor, else -1. */
int webcam_entry(const char *name, const char *dev_text, unsigned node_mode, unsigned long long node_rdev, unsigned db_mode);

#endif
