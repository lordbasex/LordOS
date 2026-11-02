/* snd.h — LordOS sound: our own PipeWire client over the PulseAudio protocol that
 * pipewire-pulse serves (see snd.c and docs/SOUND.md). Runs inside axia-server, as the
 * session user. */
#ifndef LORD_SND_H
#define LORD_SND_H

#include <stdint.h>

enum { SND_SINK, SND_SOURCE, SND_INPUT, SND_OUTPUT };   /* output, input, app playing, app recording */

int  snd_start(void);                   /* after dropping privileges; retries by itself if there's no server */
void snd_stop(void);
int  snd_fd(void);                      /* for poll (the socket, or a timer while reconnecting) */
void snd_dispatch(void);

/* Events for JS: "state" (something changed; json = the full state, like snd_state_json) and
 * "peak" ({"source":n,"level":x}, 25 per second while the meter is on). */
typedef void (*SndEventFn)(const char *type, const char *json);
void snd_events(SndEventFn fn);

char *snd_state_json(void);             /* malloc; see docs/SOUND.md for the shape */

/* Commands. They return 0 if sent (the result shows up in the next "state"). */
int snd_set_volume(int kind, uint32_t index, const double *v, int n);   /* 1.0 = 100 % per channel */
int snd_set_level(int kind, uint32_t index, double level);              /* the highest ends up at level, keeping the balance */
int snd_set_mute(int kind, uint32_t index, int mute);
int snd_set_default(int kind, const char *name);                        /* SND_SINK or SND_SOURCE */
int snd_set_port(int kind, uint32_t index, const char *port);           /* SND_SINK or SND_SOURCE */
int snd_move(int kind, uint32_t index, uint32_t device);                /* SND_INPUT → output, SND_OUTPUT → input */
int snd_meter(uint32_t source);         /* peak meter of an input; 0xffffffff turns it off */
int snd_play(const char *name);         /* system sound: "pop", "notify", "error" */

#endif
