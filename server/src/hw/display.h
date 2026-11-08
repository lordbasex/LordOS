/* display.h — Settings → Displays on the lord-server side (docs/SETTINGS.md, "Displays", and
 * docs/DISPLAYS.md for several displays and HiDPI).
 *
 *   - lists the card's outputs with their EDID name, modes, scale and place in the arrangement;
 *   - decides the configuration of every connected display (display_plan): mode, rotation, scale
 *     (100–300 % in 25 % steps), position in the global logical space, extend or mirror, and the
 *     main display (menu bar and Dock). main.c applies it between frames (display_take_resync);
 *   - live changes are kept per display (EDID identity) and saved on "Keep" in
 *     ~/.config/lordos/displays.json;
 *   - Night Light: a color temperature on every CRTC's gamma LUT, or a software LUT applied while
 *     copying frames when a card has no gamma (virtio-gpu, some ARM boards);
 *   - idle: screen off (DPMS on every display) after N seconds without input, plus "lock" and
 *     "sleep" notices for the theme. Deadlines go into the main loop's poll timeout: no polling;
 *   - hotplug: hw/uevent.c calls display_hotplug() on a DRM "change" uevent. */
#ifndef LORD_HW_DISPLAY_H
#define LORD_HW_DISPLAY_H

#include <stddef.h>
#include <stdint.h>
#include "../kms.h"
#include "../canvas.h"
#include "../outputs.h"

typedef struct {
    Kms *(*kms)(void);                  /* the main card head while the session is on the monitor, else NULL */
    void (*redraw)(void);               /* full damage (the software tint changed, screen back on) */
    void (*event)(const char *kind, const char *json);   /* → __lordHw(kind, data) */
    int (*heads)(Kms **out, int max);   /* every display head in use (gamma, DPMS); NULL = only kms() */
} DisplayHost;

/* One display of the plan. */
typedef struct {
    uint32_t conn;
    char id[96], name[64], connector[32];
    struct drm_mode_modeinfo mode;
    int rot;                            /* 0..3: ×90° clockwise, by software */
    int scale;                          /* percent: 100, 125, 150, 175, 200 */
    int recommended;
    int mm_w, mm_h;
    OutView v;                          /* its place in the global logical space */
} DisplayOut;

typedef struct {
    int n, main, mirror;                /* main: index of the display with the menu bar and the Dock */
    int bw, bh;                         /* size of the global logical space */
    DisplayOut o[OUT_MAX];
} DisplayPlan;

void display_init(const DisplayHost *h);                /* after dropping root (needs $HOME) */
/* For kms_set_picker: the display the session opens first (the main one), and its mode. */
int  display_pick(const KmsOutput *o, int n, int *index, struct drm_mode_modeinfo *mode);
/* The configuration for the connected outputs (o from kms_outputs). Returns plan->n. */
int  display_plan(const KmsOutput *o, int n, DisplayPlan *plan);
/* main.c: the configuration changed (Settings, hotplug): build a plan and apply it now (no flip
 * pending). 1 if there is something to do. */
int  display_take_resync(void);
/* main.c after applying: ok = 0 → the previous configuration is restored (and applied again). */
void display_resync_done(int ok, const DisplayPlan *applied);
void display_attached(void);            /* the session got the monitor (or started on it) */
void display_detached(void);
void display_hotplug(void);             /* DRM uevent: connectors may have changed */
/* Counts DRM uevents, also without a monitor: main.c retries a lent card that had nothing connected
 * (a board booted headless, the monitor plugged in later). */
unsigned display_hotplug_seq(void);

/* JSON for lord.display.list(). Returns a malloc'd string. */
char *display_list_json(void);
/* Apply live (not saved): output id, w×h, refresh in mHz (0 = best), rotation 0..3 (×90°), scale in
 * percent (100–300, 25 % steps; 1 and 2 are read as 100 and 200). 0 if accepted. */
int  display_request(uint32_t conn, int w, int h, int refresh_mhz, int rotation, int scale);
/* Arrangement (extend): drop display conn at logical (x, y) relative to the main display's top-left
 * corner; it snaps next to the others (outputs.c). apply = 0 only computes. Returns 0 and the
 * snapped position. */
int  display_arrange(uint32_t conn, int x, int y, int apply, int *sx, int *sy);
int  display_set_mirror(int on);        /* mirror every display to the main one, or extend */
int  display_set_main(uint32_t conn);   /* the display with the menu bar and the Dock */
/* Screenshots, recording and the remote desktop see the main display (0) or every one (1). */
void display_set_capture_all(int all);
int  display_capture_all(void);
/* Save the current configuration (modes, scales, arrangement, mirror, main display). 0 if written. */
int  display_save(uint32_t conn);
int  display_cpu_only(void);            /* a rotated display: composed by the CPU, no GPU path */
const DisplayPlan *display_current(void);   /* the plan main.c applied last (n = 0 before any) */

/* Software rotation (CPU path): rect r of the upright frame (lw×lh) in physical coordinates of the
 * card's buffer, and the copy of that rect into it. rot 1 = 90° clockwise. */
Rect display_rotate_rect(Rect r, int lw, int lh, int rot);
void display_rotate_copy(const Canvas *src, Rect r, Canvas *dst, int rot);
/* Same, from a buffer whose first pixel is (ox, oy) of the upright frame (a cursor composited apart). */
void display_rotate_copy_at(const uint32_t *px, int stride, int ox, int oy, Rect r, int lw, int lh, Canvas *dst, int rot);
/* Physical normalized position (0..1, as a tablet reports it) → upright normalized position. */
void display_unrotate_point(int rot, double px, double py, double *lx, double *ly);

/* Night Light: kelvin (6500 = off). Hardware gamma when the CRTC has one, else software. */
void display_set_temperature(int kelvin);
int  display_temperature(void);
const uint8_t *display_soft_lut(void);  /* 3×256 (r, g, b) when the software tint is on, else NULL */
void display_tint(Canvas *c, Rect r);   /* apply the software LUT in place */
/* Sunrise/sunset for a date at the time zone's coordinates (zone1970.tab), minutes UTC. */
int  display_sun(const char *zone, int y, int m, int d, double *rise, double *set);

/* Idle (seconds; 0 = never): screen off, lock notice, sleep notice. sleep_first_s (0 = sleep_s) is the
 * first sleep deadline, until the next input: a locked session's time left, kept by the login screen. */
void display_set_idle(int off_s, int lock_s, int sleep_s, int sleep_first_s);
void display_activity(double now_ms);   /* any activity (input, a video keeping the screen on, remote input) */
void display_input(double now_ms);      /* local input: a lock already due goes first (issue #36) */
double display_timeout(double now_ms);  /* ms to the next idle deadline, or -1 */
void display_tick(double now_ms);
int  display_is_off(void);              /* DPMS off: main.c does not render */
int  display_on_monitor(void);          /* this session has the card (it is the one on the screen) */
double display_dark_ms(double now_ms);  /* how long the screen has been off after idle (0 if it is on) */
void display_left(void);                /* RP_UNDISPLAY (for LORD_IDLE_HEADLESS, development only) */
void display_wake(void);                /* turn the screen back on now */
/* Turn the screen off as soon as this process has the card (now, or when it is lent), until the next
 * input: the login screen shown by an idle lock stays dark (docs/SETTINGS.md). */
void display_blank(void);
/* For a lock: is the screen off after idle, the display-off and sleep timeouts (s, 0 = never) and the
 * seconds left until the sleep deadline (0 = none, or due now). */
void display_idle_state(int *dark, int *off_s, int *sleep_s, int *sleep_left_s);

#endif
