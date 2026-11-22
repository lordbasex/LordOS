/* x11clip.h — lordos-guest's own owner of the X CLIPBOARD selection (docs/VIRTUALIZATION.md 6.8): the host's clipboard
 * on demand. No Xlib: the X11 protocol itself over the display's Unix socket ($DISPLAY, ":N" → /tmp/.X11-unix/XN, or
 * the abstract socket of that name), authorised with the MIT-MAGIC-COOKIE-1 of $XAUTHORITY (or ~/.Xauthority) when
 * there is one. Under Wayland that's Xwayland, which GNOME, KDE and wlroots desktops bridge to the Wayland clipboard.
 *
 * xc_own puts the host's offer on the CLIPBOARD (its forms, no data). A program that pastes sends a SelectionRequest;
 * the forms it asks for come out of xc_wanted, and xc_supply answers every request waiting for that form, with the
 * data (as INCR, in pieces, when it's larger than one request may carry) or with a refusal. TARGETS is answered at
 * once; MULTIPLE and other targets are refused. Losing the selection (another program copied) drops the offer. */
#ifndef LORD_GUEST_X11CLIP_H
#define LORD_GUEST_X11CLIP_H

#include <stddef.h>
#include <stdint.h>

/* connects (once; again after a failure, at most every 10 s). 0 connected, -1 not */
int xc_open(void);
int xc_fd(void);                    /* the connection to poll, -1 none */
void xc_close(void);
/* owns CLIPBOARD for an offer of these forms (AGM_*): 0 done */
int xc_own(uint8_t forms);
int xc_owned(void);                 /* 1 while the offer is ours */
/* reads what the server sent (call when xc_fd is readable): -1 the connection broke (closed) */
int xc_read(void);
/* a form (AGF_*) a program waits for that nobody asked the host for yet: marked asked and returned, 0 none */
int xc_wanted(void);
/* the host's answer for a form: data (n bytes) or NULL (refused); every request waiting for it is answered. The data is
 * kept for later pastes of the same offer */
void xc_supply(int form, const uint8_t *d, size_t n);
/* requests whose answer never came, and transfers whose reader stopped reading: let go after ms */
void xc_expire(long long now_ms, long long wait_ms);

#endif
