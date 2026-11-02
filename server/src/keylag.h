/* keylag.h — key latency trace (docs/TROUBLESHOOTING.md, "Typing feels slow").
 *
 * Off by default and free when off (one branch per key). It only turns on when axia-server starts
 * with LORD_INPUT_DEBUG=1, which lord-sessiond passes to user sessions (never the login screen)
 * only when the kernel command line has lordos.inputdebug=1 or its own service sets
 * LORD_INPUT_DEBUG=1: root decides, for one boot. There is no run-time switch.
 *
 * Privacy: it is a timing trace, not a key log. No key code, character or modifier is ever written:
 * only an opaque sequence number and press/release. Only keys handed to a Linux (Wayland) app are
 * traced, never keys that go to LordOS itself (the theme, its text fields, the Terminal, the
 * Passwords app, keyring and admin prompts), to an isolated LordOS app, or to a protected window;
 * those are skipped without being counted. The trace goes to its own file,
 * $XDG_RUNTIME_DIR/axia-keylag.log (0600, tmpfs, gone at logout), never to the session log, and
 * stops by itself 120 s after the first traced key or after 4000 lines.
 *
 * One line per traced key, times in ms after the kernel's evdev timestamp (ev, CLOCK_MONOTONIC ms):
 *
 *   keylag n=12 ev=81234.567 press rx=0.08 sent=0.10 flush=0.31 commit=6.20 shown=16.90
 *
 * rx: axia-server read it; sent: wl_keyboard.key queued for the focused app; flush: written to
 * the app's socket; commit: the app's next commit after that (its reaction: the glyph, or simply
 * its next frame if it is animating); shown: the page flip with that content finished. -1 = the
 * stage did not happen (no flip yet, an app that drew nothing). */
#ifndef LORD_KEYLAG_H
#define LORD_KEYLAG_H

#include <stdint.h>
#include <sys/time.h>

extern int keylag_enabled;

void keylag_init(int greeter);                  /* reads LORD_INPUT_DEBUG once, at start */
void keylag_rx(int value, const struct timeval *ev_time);   /* main.c: a key (not kept unless sent below) */
void keylag_sent_wl(int traceable);             /* wl.c: wl_keyboard.key queued; 0 = protected, skip it */
void keylag_handled(void);                      /* main.c: js_key returned; a key not sent to an app is dropped */
void keylag_flushed(void);                      /* wl.c: wl_display_flush_clients */
void keylag_commit(void);                       /* wl.c: the focused app committed */
void keylag_render(void);                       /* main.c: a frame is being composed */
void keylag_presented(uint64_t flip_ns);        /* main.c: the flip finished (CLOCK_MONOTONIC ns) */

#endif
