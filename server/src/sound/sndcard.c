/* sndcard.c — a card from a GET_CARD_INFO reply (see sndcard.h). The layout follows PulseAudio's
 * protocol-native.c (card_fill_tagstruct), which pipewire-pulse reproduces:
 *
 *   u32 index, string name, u32 owner module, string driver,
 *   u32 n_profiles, n × { string name, string description, u32 n_sinks, u32 n_sources,
 *                         u32 priority, u32 available (v29) },
 *   string active profile, proplist,
 *   u32 n_ports (v26), n × { string name, string description, u32 priority, u32 available,
 *                            u8 direction, proplist, u32 n_profiles, n × string,
 *                            s64 latency offset (v27) }
 *
 * Fields from v34 (a port's availability group and type) never come: snd.c asks for v32. */
#include "sndcard.h"

#include <stdio.h>
#include <string.h>

/* A name that doesn't fit is not kept at all: a cut name would name a profile that doesn't exist. */
static int fits(const char *s, size_t cap) { return s && strlen(s) < cap; }

static void card_prop(void *ud, const char *key, const char *val, uint32_t len) {
    (void)len;
    SndCard *c = ud;
    if (!strcmp(key, "device.api")) snprintf(c->api, sizeof c->api, "%s", val);
    else if (!strcmp(key, "device.bus")) snprintf(c->bus, sizeof c->bus, "%s", val);
    else if (!strcmp(key, "device.description")) snprintf(c->desc, sizeof c->desc, "%s", val);
}

int snd_card_parse(TsIn *t, SndCard *c) {
    memset(c, 0, sizeof *c);
    c->index = ts_get_u32(t);
    const char *name = ts_get_str(t);
    snprintf(c->name, sizeof c->name, "%s", name ? name : "");
    ts_get_u32(t);                                  /* owner module */
    ts_get_str(t);                                  /* driver */
    uint32_t np = ts_get_u32(t);
    for (uint32_t i = 0; i < np && !t->err; i++) {
        SndProfile p = { 0 };
        const char *pn = ts_get_str(t), *pd = ts_get_str(t);
        p.sinks = ts_get_u32(t);
        p.sources = ts_get_u32(t);
        p.priority = ts_get_u32(t);
        p.available = ts_get_u32(t) != 0;
        if (t->err || !fits(pn, sizeof p.name) || !*pn || c->nprofiles >= SND_MAX_PROFILES) continue;
        snprintf(p.name, sizeof p.name, "%s", pn);
        snprintf(p.desc, sizeof p.desc, "%s", pd ? pd : "");
        c->profiles[c->nprofiles++] = p;
    }
    const char *active = ts_get_str(t);
    if (fits(active, sizeof c->active)) snprintf(c->active, sizeof c->active, "%s", active);
    ts_get_proplist(t, card_prop, c);
    uint32_t nports = ts_get_u32(t);
    for (uint32_t i = 0; i < nports && !t->err; i++) {
        ts_get_str(t);                              /* name */
        ts_get_str(t);                              /* description */
        ts_get_u32(t);                              /* priority */
        ts_get_u32(t);                              /* available */
        ts_get_u8(t);                               /* direction */
        ts_get_proplist(t, NULL, NULL);
        uint32_t n = ts_get_u32(t);
        for (uint32_t k = 0; k < n && !t->err; k++) ts_get_str(t);
        ts_get_u64(t);                              /* latency offset (s64) */
    }
    return t->err ? -1 : 0;
}

int snd_card_is_bluetooth(const SndCard *c) {
    return !strcmp(c->api, "bluez5") || !strcmp(c->bus, "bluetooth");
}

const SndProfile *snd_card_profile(const SndCard *c, const char *name) {
    for (int i = 0; i < c->nprofiles; i++) if (name && !strcmp(c->profiles[i].name, name)) return &c->profiles[i];
    return NULL;
}
