/* tb.h — what lordos-testbench's parts share: the connection, the window and the input serials
 * (testbench.c), and the protocol checklist (checks.c). */
#ifndef LORD_TESTBENCH_TB_H
#define LORD_TESTBENCH_TB_H

#include <stdint.h>
#include <wayland-client.h>

struct xdg_wm_base;
struct xdg_surface;
struct xdg_toplevel;

extern struct wl_display *dpy;
extern struct wl_registry *registry;
extern struct wl_compositor *comp;
extern struct wl_shm *shm;
extern struct xdg_wm_base *wm;
extern struct wl_seat *seat;
extern struct wl_pointer *pointer;
extern struct wl_keyboard *keyboard;
extern struct wl_surface *surf;
extern struct xdg_surface *xsurf;
extern struct xdg_toplevel *top;
extern int win_w, win_h;
extern uint32_t kb_serial, ptr_serial;     /* the last enter or input serials (0: none yet) */
extern uint32_t toplevel_states;           /* bit per xdg_toplevel state of the last configure */

typedef struct { uint32_t name, version; char iface[64]; } Global;
extern Global globals[96];
extern int nglobals;

/* ---- the protocol checklist (checks.c) ---- */
enum { CK_UNTESTED, CK_PASS, CK_FAIL, CK_INFO };
typedef struct { char id[40]; int state; char detail[160]; } Check;
extern Check checks[48];
extern int nchecks;
extern int checks_done;
extern uint32_t out_refresh_mhz;          /* the first output's current mode, 0 unknown */

/* Runs the whole checklist (blocking, dispatching events meanwhile; frames keep coming). Returns 0, or
 * -1 if the compositor ended the connection (a protocol error, named in the failing check). */
int checks_run(void);

/* Dispatches until *flag is set or ms pass; -1 if the connection died. */
int tb_wait(volatile int *flag, int ms);

/* A small shm buffer of one colour (for test surfaces). */
struct wl_buffer *tb_solid_buffer(int w, int h, uint32_t argb);

#endif
