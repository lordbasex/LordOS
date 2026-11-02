/* gpucap.h — the composed frame for screen sharing, read back without stopping the compositor.
 * The compositor copies the frame to a texture on the GPU and goes on; a thread with a shared context
 * reads the texture. Only one frame is under way at a time: a new one waits for the last to be taken.
 * Off (gpucap_on 0) without OpenGL ES 3 or a surfaceless context, or with LORD_CAPTURE_ASYNC=0: the
 * caller then reads back the old way. See docs/REMOTE.md, "Video (WebRTC)". */
#ifndef LORD_GPUCAP_H
#define LORD_GPUCAP_H

#include <stdint.h>

int  gpucap_start(void *egl_display, void *egl_config, void *egl_context);  /* with the context current */
void gpucap_stop(void);
int  gpucap_on(void);
int  gpucap_idle(void);                 /* a new frame can start */
int  gpucap_kick(int w, int h);         /* the bound frame, with the compositor's context; 0 ok */
int  gpucap_fd(void);                   /* readable when a frame is ready; -1 if off */
/* Clears gpucap_fd. The frame (0xAARRGGBB, rows top to bottom), or NULL; gpucap_release after using it. */
const uint32_t *gpucap_take(int *w, int *h);
void gpucap_release(void);

#endif
