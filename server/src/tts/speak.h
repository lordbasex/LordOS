/* speak.h — reading text aloud from the desktop: lord-server starts lord-say (speak.c, docs/VOICE.md). */
#ifndef LORD_SPEAK_H
#define LORD_SPEAK_H

#include <stddef.h>
#include <stdint.h>

#include "levels.h"

#define SPEAK_TEXT_MAX (1u << 20)

int speak_available(void);                                  /* lord-say and lord-tts are installed */
int speak_start(const char *text, size_t n, const char *lang, const char *voice);   /* ends the reading before; lang: the interface's
                                    (es, en, pt or NULL), for a text too short to tell; voice: say's -v, or NULL */
void speak_stop(void);
int speak_state(int *exit_code);    /* 1 while reading; exit_code: how the last one ended (lord-say's status), -1 none */
int speak_pause(int on);            /* freezes the reading where it is (1) or goes on (0); -1 when nothing is reading */
int speak_paused(void);
int speak_replay(void);             /* reads the last text again from the start; -1 when there is none */
int speak_preview(const char *lang, const char *voice);   /* a voice's recorded sample, with the player's wave */
int speak_save(const char *text, size_t n, const char *lang, const char *target);  /* a WAV at target (resolved and
                                    checked by the caller, js_voice.c), nothing heard; ends any reading before */
int speak_saving(void);             /* 1 while the one in progress writes a file */
int speak_levels(uint8_t out[SAY_BANDS]);   /* how loud each band of what is playing now is; 0 when nothing is */

#endif
