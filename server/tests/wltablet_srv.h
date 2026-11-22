/* wltablet_srv.h — the compositor side of tests/wltablet_test.c (wltablet_srv.c). */
#ifndef LORD_TESTS_WLTABLET_SRV_H
#define LORD_TESTS_WLTABLET_SRV_H

#include <stdint.h>

/* What wl.c would say. Surfaces are numbered from 1 in the order the clients made them; 0 is none. */
typedef struct {
    int under;                  /* the surface under (100..500, 50..350) on screen */
    int key, pointer;           /* the surface with the keyboard, with the pointer */
    int held, dragging, locked;
    float scale;                /* Xwayland's X pixels per logical one (0: 1) */
    uint32_t enter_serial;
    /* what was done */
    int presses, releases, pointer_leaves, cursor_set, cursor_hx, cursor_hy, warps;
    float warp_x, warp_y;
} SrvScene;
extern SrvScene scene;

int  srv_start(const int *fds, int n);      /* one server end of a socketpair per client */
void srv_dispatch(void);
void srv_flush(void);
int  srv_surfaces(void);
void srv_key_focus(int surface);
void srv_stop(void);

#endif
