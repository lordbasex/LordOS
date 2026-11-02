/* mstream.h — the remote desktop's video, on the session side (see mstream.c, docs/REMOTE.md,
 * "Video (WebRTC)"): one live encoder per codec in use, shared by every viewer of this session. */
#ifndef LORD_MEDIA_MSTREAM_H
#define LORD_MEDIA_MSTREAM_H

#include <stddef.h>
#include <stdint.h>

#define MS_CODECS 3                     /* index 1 H.264, 2 VP8 (RW_CODEC_*) */

typedef struct {
    /* an encoded video frame (H.264 Annex B or VP8) or Opus packet, for every viewer of that codec */
    void (*video)(int codec, int key, uint32_t ms, const uint8_t *p, size_t n);
    void (*audio)(int codec, uint32_t samples, const uint8_t *p, size_t n);
} MStreamOut;

void mstream_init(const MStreamOut *out);
/* How many viewers want this codec, with sound, and the lowest bit rate among them. 0 viewers stops it. */
void mstream_want(int codec, int viewers, int audio, unsigned kbps, int fps);
/* The session's screen changed (the frame stays valid until the next call or mstream_want(…, 0)). */
/* box: what changed since the last call (x0, y0, x1, y1), or NULL for all of it. */
void mstream_frame(const uint32_t *px, int stride, int w, int h, const int *box);
void mstream_keyframe(int codec);
void mstream_tick(void);
double mstream_timeout(void);           /* ms until mstream_tick has work, or -1 */
int  mstream_active(void);              /* an encoder is running */
/* The running encoder of that codec: hardware or not, frames a second, encoding time in 1/100 ms per
 * frame. 0 if none runs. */
int  mstream_info(int codec, int *hw, int *fps, long *enc100);
/* "H.264 (VA-API) 30 fps 2.1 ms/frame" for the logs */
void mstream_describe(int codec, char *out, size_t n);

#endif
