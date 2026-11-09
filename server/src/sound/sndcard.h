/* sndcard.h — the sound cards of a PulseAudio-protocol server (GET_CARD_INFO replies), used by
 * snd.c for Bluetooth headsets (issue #78). A headset is one card with several profiles: high
 * fidelity playback (A2DP or LE Audio, output only) and headset mode (HSP/HFP, a low-quality
 * output plus the microphone). Which one is active decides whether its output sounds well, so
 * LordOS keeps the Bluetooth cards and their profiles and can change the profile (SET_CARD_PROFILE).
 * Only Bluetooth cards are kept: other cards' profiles are not used by the UI. Separate from
 * snd.c so tests/sound_test.c can check the parser without a server. */
#ifndef LORD_SNDCARD_H
#define LORD_SNDCARD_H

#include "pulse.h"

#define SND_MAX_PROFILES 24

typedef struct {
    char name[96], desc[128];
    uint32_t sinks, sources, priority;
    int available;                      /* the server's "available": 0 only when it says it is not */
} SndProfile;

typedef struct {
    uint32_t index;
    char name[128], desc[128], api[16], bus[16];
    int nprofiles;
    SndProfile profiles[SND_MAX_PROFILES];
    char active[96];
} SndCard;

/* One card of a GET_CARD_INFO(_LIST) reply, for protocol version 29..33 (the profile's
 * "available" from v29, ports from v26, their latency offset from v27; snd.c speaks 32).
 * Returns 0 if it was read whole; t->err otherwise. Profiles past SND_MAX_PROFILES are read
 * and dropped. */
int snd_card_parse(TsIn *t, SndCard *c);

/* A Bluetooth card: device.api "bluez5" or device.bus "bluetooth" in its properties. */
int snd_card_is_bluetooth(const SndCard *c);

/* The profile by name, or NULL. */
const SndProfile *snd_card_profile(const SndCard *c, const char *name);

#endif
