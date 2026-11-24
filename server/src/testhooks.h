/* testhooks.h — what the Wayland conformance tool needs from a headless lord-server that a real session never has
 * (tools/wl-conformance.mjs, docs/WAYLAND.md "Conformance").
 *
 * Compiled only into build/lord-server-test (the same sources with -DLORD_TEST_HOOKS, `make -C server
 * build/lord-server-test`): the installed lord-server has no `--test-hooks` flag, no socket and none of this code.
 * Even there, only with `--headless WxH --test-hooks` (refused otherwise, and on the login screen). It makes
 * one socket, $XDG_RUNTIME_DIR/lordos-test-hooks.sock (0600, SOCK_SEQPACKET, the same user only: SO_PEERCRED), and
 * takes one short text command per message:
 *
 *   ptr X Y             the pointer moves to (X, Y), logical pixels: the remote desktop's injection (main.c)
 *   btn B 1|0           button B (0 left, 1 middle, 2 right) pressed or released, the same way
 *   wheel N             N wheel steps (+ up)
 *   key C 1|0           key C (an evdev code, 1 to 0x2ff) pressed or released, as the remote desktop types it (main.c's
 *                       remote_key): the window drag modifier's test (tests/moddrag_headless.sh) and the Machines harness
 *                       (tools/vm/e2e/machines), whose text goes in this way (remote.sock takes only root's connections)
 *   tablet add|remove   a fake graphics tablet's pen and pad appear or go away (tablet.c's own tablet_new)
 *   pen|pad T C V       one evdev event (type, code, value) of that pen or pad, through tablet.c and wl_tablet.c as a
 *                       real device's
 *   xwl                 a Wayland connection lord-server makes itself and treats as Xwayland's (the only client that
 *                       may bind xwayland_shell_v1), its socket's other end handed back (SCM_RIGHTS); refused while
 *                       Xwayland runs
 *   lease plug|unplug   a fake card with a desktop display and a non-desktop one (a VR headset): drm-lease-v1's global,
 *                       its connectors and leases (memfds, never a real card) with the real permission prompt
 *   frame W [N]         one picture written as $XDG_RUNTIME_DIR/lordos-test-frame-W.ppm (binary PPM): W output, what the
 *                       headless display shows; screenshot, media_screenshot of the whole screen; mirror, what
 *                       media_composed hands the remote desktop, the screen-sharing API and the recording; portal, the
 *                       Screenshot portal's still of the screen (cast_still); window N, a window (node N) shared alone.
 *                       The protected windows' test (tests/protected_headless.sh, docs/PRIVACY.md)
 *   painted             "ok N": the pixels of the UI repainted (its damage, summed over every frame) since the last
 *                       time it was asked. A fullscreen window's subsurface repaints its own box, not the window's
 *                       place (tests/fsub_headless.sh, GitHub #232)
 *
 * The answer is "ok" or "err <why>". Nothing here reaches a real device, card or another session. */
#ifndef LORD_TESTHOOKS_H
#define LORD_TESTHOOKS_H

#include <poll.h>

typedef struct {
    void (*pointer)(int x, int y);
    void (*button)(int button, int down);
    void (*wheel)(int steps);
    int  (*tablet)(int add);                                     /* 0 on success */
    int  (*tablet_event)(int pad, unsigned type, unsigned code, int value);
    int  (*xwl)(void);                                           /* the client's end, or -1 */
    int  (*lease)(int plugged);                                  /* 0 on success */
    void (*key)(int code, int down);                             /* a key (testhooks.c checks the code's range) */
    int  (*frame)(const char *which, long node);                 /* 0 when the picture was written */
    long long (*painted)(void);                                  /* UI pixels repainted since the last call */
} TestHooks;

int  testhooks_start(const TestHooks *h);   /* after dropping privileges; 0 when the socket listens */
int  testhooks_pollfds(struct pollfd *p, int max);
void testhooks_dispatch(const struct pollfd *p, int n);
void testhooks_stop(void);

#endif
