/* blackfb.h — the black frame lord-sessiond puts on a lit CRTC at a hand-off (monitor.c black_out).
 * A dumb buffer is not zeroed by every driver: radeon gives one in VRAM with whatever was there
 * last, which showed as a moment of noise between the console and the login screen and between the
 * login screen and the desktop (GitHub #94). So the buffer is mapped and cleared before it becomes
 * a framebuffer. Only DRM ioctls and mmap on the fd it is given, so build/blackfb-test checks it
 * with those calls wrapped. */
#ifndef LORD_REMOTE_BLACKFB_H
#define LORD_REMOTE_BLACKFB_H

#include <stdint.h>

/* A cleared XRGB8888 framebuffer of w x h on the card fd, or 0 if any step fails (nothing is left
 * behind then). The dumb buffer's handle is closed: the framebuffer keeps the buffer. */
uint32_t black_fb(int fd, uint32_t w, uint32_t h);

#endif
