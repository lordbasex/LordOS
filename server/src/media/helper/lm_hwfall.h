/* lm_hwfall.h — when the player leaves a failing hardware decoder (docs/MEDIA.md, "Hardware acceleration").
 * Failures in a row, so one damaged packet, which a software decoder would report too, changes nothing. Decoding
 * and reading a frame back are counted apart: a driver can decode every frame and fail to hand any of them over, and
 * each frame decoded must not clear those failures. No FFmpeg here: build/hwfall-test checks these rules. */
#ifndef LM_HWFALL_H
#define LM_HWFALL_H

#include <stdint.h>

#define LM_HWFALL_ERRS 8                /* failures in a row before going to software */

typedef struct { int fails, rb_fails; } LmHwFall;

static inline void lm_hwfall_reset(LmHwFall *s) { s->fails = s->rb_fails = 0; }
/* one decoder result: a frame (failed 0) clears the count */
static inline void lm_hwfall_note(LmHwFall *s, int failed) { s->fails = failed ? s->fails + 1 : 0; }
/* one frame read back from the hardware: a frame read (failed 0) clears the count */
static inline void lm_hwfall_note_rb(LmHwFall *s, int failed) { s->rb_fails = failed ? s->rb_fails + 1 : 0; }
static inline int  lm_hwfall_due(const LmHwFall *s) { return s->fails >= LM_HWFALL_ERRS || s->rb_fails >= LM_HWFALL_ERRS; }
#endif
