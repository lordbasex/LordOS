/* lm_encheck.h — is a hardware encoder's first H.264 keyframe something a browser can show?
 * (docs/REMOTE.md, "One encoder per session"; #94)
 *
 * A VA-API driver can open the H.264 encoder and hand back packets a browser never decodes: no SPS or PPS in
 * the stream (a driver without packed headers), or a broken picture. The remote desktop then connects and
 * shows nothing. So the live encoder checks its first keyframe the way a browser would: FFmpeg's software
 * H.264 decoder must make a picture of the right size out of that one packet, and the picture must look like
 * the frame that went in (a sparse grid of luma samples, mean difference below LM_ENCHECK_MAX_DIFF).
 * Anything else and lm_stream.c goes on with libx264. Only libavcodec here: build/encheck-test checks it. */
#ifndef LM_ENCHECK_H
#define LM_ENCHECK_H

#include <stddef.h>
#include <stdint.h>

#define LM_ENCHECK_STEP 8               /* one luma sample every 8 pixels, every 8 rows */
#define LM_ENCHECK_MAX_DIFF 24          /* mean absolute difference (0-255) a sane keyframe stays well under */

typedef struct {
    uint8_t *ref;                       /* the grid of the frame that went in, or NULL */
    int w, h, gw, gh;
} LmEncCheck;

/* Keeps the grid of the first frame's luma plane (w×h, linesize bytes per row). 0 or -1 (no memory). */
int  lm_encheck_ref(LmEncCheck *c, const uint8_t *y, int linesize, int w, int h);
/* 0 if the Annex B packet decodes on its own into that frame; else -1, and *why says what was wrong. */
int  lm_encheck_packet(const LmEncCheck *c, const uint8_t *pkt, size_t n, const char **why);
void lm_encheck_free(LmEncCheck *c);

#endif
