/* lm_hwfall.h — when the player leaves a failing hardware decoder (docs/MEDIA.md, "Hardware acceleration").
 * Failures in a row, so one damaged packet, which a software decoder would report too, changes nothing. No FFmpeg
 * here: build/hwfall-test checks these rules. */
#ifndef LM_HWFALL_H
#define LM_HWFALL_H

#include <stdint.h>

#define LM_HWFALL_ERRS 8                /* failures in a row before going to software */

typedef struct { int fails; } LmHwFall;

static inline void lm_hwfall_reset(LmHwFall *s) { s->fails = 0; }
/* one decoder result: a frame (failed 0) clears the count */
static inline void lm_hwfall_note(LmHwFall *s, int failed) { s->fails = failed ? s->fails + 1 : 0; }
static inline int  lm_hwfall_due(const LmHwFall *s) { return s->fails >= LM_HWFALL_ERRS; }
#endif
