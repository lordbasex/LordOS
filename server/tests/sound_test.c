/* sound_test.c — PulseAudio protocol serialization (sound/pulse.c) without a server:
 * round trip of each type, property lists, descriptors and truncated data. */
#include "../src/sound/pulse.h"
#include "../src/sound/sndcard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static int props_seen;
static void on_prop(void *ud, const char *key, const char *val, uint32_t len) {
    (void)ud;
    if (!strcmp(key, "application.name")) CHECK(!strcmp(val, "LordOS") && len == 6);
    if (!strcmp(key, "media.role")) CHECK(!strcmp(val, "event"));
    props_seen++;
}

/* One card as pipewire-pulse sends it to a v32 client (PulseAudio's card_fill_tagstruct). */
static void put_profile(TsOut *o, const char *name, const char *desc, uint32_t sinks, uint32_t sources, uint32_t prio, uint32_t avail) {
    ts_str(o, name); ts_str(o, desc); ts_u32(o, sinks); ts_u32(o, sources); ts_u32(o, prio); ts_u32(o, avail);
}

static void put_card(TsOut *o, uint32_t index, const char *name, const char *api, const char *bus, const char *active) {
    ts_u32(o, index);
    ts_str(o, name);
    ts_u32(o, PA_INVALID_INDEX);
    ts_str(o, "module-pipewire");
    if (!strcmp(api, "bluez5")) {
        ts_u32(o, 5);
        put_profile(o, "off", "Off", 0, 0, 0, 1);
        put_profile(o, "a2dp-sink-sbc", "High Fidelity Playback (A2DP Sink, codec SBC)", 1, 0, 18, 1);
        put_profile(o, "a2dp-sink", "High Fidelity Playback (A2DP Sink, codec AAC)", 1, 0, 19, 1);
        put_profile(o, "headset-head-unit-cvsd", "Headset Head Unit (HSP/HFP, codec CVSD)", 1, 1, 1, 1);
        put_profile(o, "headset-head-unit", "Headset Head Unit (HSP/HFP, codec mSBC)", 1, 1, 2, 0);
    } else {
        ts_u32(o, 2);
        put_profile(o, "off", "Off", 0, 0, 0, 1);
        put_profile(o, "output:analog-stereo+input:analog-stereo", "Analog Stereo Duplex", 1, 1, 6565, 1);
    }
    ts_str(o, active);
    const char *kv[] = { "device.api", api, "device.bus", bus, "device.description", "Test headset" };
    ts_proplist(o, kv, 3);
    ts_u32(o, 2);                                   /* ports */
    for (int i = 0; i < 2; i++) {
        ts_str(o, i ? "headset-input" : "headset-output");
        ts_str(o, i ? "Handsfree" : "Headset");
        ts_u32(o, 0);
        ts_u32(o, 2);
        ts_u8(o, i ? 2 : 1);                        /* direction: input, output */
        const char *pkv[] = { "port.type", "headset" };
        ts_proplist(o, pkv, 1);
        ts_u32(o, 2);
        ts_str(o, "a2dp-sink");
        ts_str(o, "headset-head-unit");
        ts_s64(o, -5);                              /* latency offset */
    }
}

static void card_tests(void) {
    TsOut o;
    ts_init(&o);
    put_card(&o, 57, "bluez_card.00_AA_01_01_00_01", "bluez5", "bluetooth", "headset-head-unit-cvsd");
    put_card(&o, 46, "alsa_card.pci-0000_00_1b.0", "alsa", "pci", "output:analog-stereo+input:analog-stereo");
    size_t n = ts_finish(&o);
    CHECK(n > 0);
    const uint8_t *body = o.d + PA_DESCRIPTOR_SIZE;
    size_t len = n - PA_DESCRIPTOR_SIZE;

    static SndCard c;
    TsIn t;
    ts_in(&t, body, len);
    CHECK(snd_card_parse(&t, &c) == 0);
    CHECK(c.index == 57 && !strcmp(c.name, "bluez_card.00_AA_01_01_00_01") && !strcmp(c.desc, "Test headset"));
    CHECK(snd_card_is_bluetooth(&c) && !strcmp(c.active, "headset-head-unit-cvsd") && c.nprofiles == 5);
    const SndProfile *p = snd_card_profile(&c, "a2dp-sink");
    CHECK(p && p->sinks == 1 && p->sources == 0 && p->priority == 19 && p->available);
    p = snd_card_profile(&c, "headset-head-unit");
    CHECK(p && p->sinks == 1 && p->sources == 1 && !p->available);
    CHECK(!snd_card_profile(&c, "a2dp") && !snd_card_profile(&c, NULL));
    /* the next card starts right after the ports */
    CHECK(snd_card_parse(&t, &c) == 0 && c.index == 46 && !snd_card_is_bluetooth(&c) && c.nprofiles == 2);
    CHECK(ts_eof(&t) && !t.err);

    /* every cut fails cleanly, inside the buffer */
    int bad = 0;
    for (size_t cut = 0; cut < len; cut++) {
        ts_in(&t, body, cut);
        int r = 0;
        while (!ts_eof(&t) && !t.err && (r = snd_card_parse(&t, &c)) == 0) {}
        if (t.err || r) bad++;
    }
    CHECK(bad > 0);
    ts_free(&o);

    /* a profile name that doesn't fit is dropped, never cut (it would name a profile that doesn't exist) */
    char longname[200];
    memset(longname, 'a', sizeof longname - 1);
    longname[sizeof longname - 1] = 0;
    ts_init(&o);
    ts_u32(&o, 3); ts_str(&o, "bluez_card.x"); ts_u32(&o, PA_INVALID_INDEX); ts_str(&o, NULL);
    ts_u32(&o, 2);
    put_profile(&o, longname, "Long", 1, 0, 1, 1);
    put_profile(&o, "a2dp-sink", "A2DP", 1, 0, 2, 1);
    ts_str(&o, longname);
    const char *kv[] = { "device.bus", "bluetooth" };
    ts_proplist(&o, kv, 1);
    ts_u32(&o, 0);
    n = ts_finish(&o);
    ts_in(&t, o.d + PA_DESCRIPTOR_SIZE, n - PA_DESCRIPTOR_SIZE);
    CHECK(snd_card_parse(&t, &c) == 0 && c.nprofiles == 1 && !strcmp(c.profiles[0].name, "a2dp-sink") && !c.active[0]);
    CHECK(snd_card_is_bluetooth(&c) && !strcmp(c.desc, ""));
    ts_free(&o);
}

int main(void) {
    card_tests();
    TsOut o;
    ts_init(&o);
    ts_u32(&o, 0xdeadbeef);
    ts_u8(&o, 7);
    ts_u64(&o, 0x0102030405060708ull);
    ts_bool(&o, 1);
    ts_bool(&o, 0);
    ts_str(&o, "hola");
    ts_str(&o, NULL);
    ts_arb(&o, "\x01\x02\x03", 3);
    ts_ss(&o, PA_SAMPLE_FLOAT32LE, 2, 48000);
    uint8_t map[2] = { 1, 2 };
    ts_cmap(&o, 2, map);
    uint32_t vol[2] = { PA_VOLUME_NORM, PA_VOLUME_NORM / 2 };
    ts_cvol(&o, 2, vol);
    const char *kv[] = { "application.name", "LordOS", "media.role", "event" };
    ts_proplist(&o, kv, 2);
    ts_format_pcm(&o);
    size_t n = ts_finish(&o);
    CHECK(n == o.len && !o.err);

    uint32_t len, ch, flags;
    uint64_t off;
    pa_descriptor_read(o.d, &len, &ch, &off, &flags);
    CHECK(len == n - PA_DESCRIPTOR_SIZE);
    CHECK(ch == 0xffffffffu && off == 0 && flags == 0);

    TsIn t;
    ts_in(&t, o.d + PA_DESCRIPTOR_SIZE, len);
    CHECK(ts_get_u32(&t) == 0xdeadbeef);
    CHECK(ts_get_u8(&t) == 7);
    CHECK(ts_get_u64(&t) == 0x0102030405060708ull);
    CHECK(ts_get_bool(&t) == 1);
    CHECK(ts_get_bool(&t) == 0);
    const char *s = ts_get_str(&t);
    CHECK(s && !strcmp(s, "hola"));
    CHECK(ts_get_str(&t) == NULL && !t.err);
    uint32_t an;
    const uint8_t *a = ts_get_arb(&t, &an);
    CHECK(an == 3 && a && a[2] == 3);
    uint8_t f, c; uint32_t r;
    ts_get_ss(&t, &f, &c, &r);
    CHECK(f == PA_SAMPLE_FLOAT32LE && c == 2 && r == 48000);
    uint8_t m2[PA_CHANNELS_MAX];
    CHECK(ts_get_cmap(&t, m2) == 2 && m2[1] == 2);
    uint32_t v2[PA_CHANNELS_MAX];
    CHECK(ts_get_cvol(&t, v2) == 2 && v2[0] == PA_VOLUME_NORM && v2[1] == PA_VOLUME_NORM / 2);
    ts_get_proplist(&t, on_prop, NULL);
    CHECK(props_seen == 2 && !t.err);
    ts_skip_format(&t);
    CHECK(!t.err && ts_eof(&t));

    /* ts_skip walks everything the same way as the typed read */
    ts_in(&t, o.d + PA_DESCRIPTOR_SIZE, len);
    int k = 0;
    while (!ts_eof(&t) && !t.err) { ts_skip(&t); k++; }
    CHECK(!t.err && k == 13);

    /* truncated: every prefix fails without reading outside the buffer */
    int errs = 0;
    for (size_t cut = 0; cut < len; cut++) {
        ts_in(&t, o.d + PA_DESCRIPTOR_SIZE, cut);
        while (!ts_eof(&t) && !t.err) ts_skip(&t);
        if (t.err) errs++;
    }
    CHECK(errs > 0);

    /* tipo equivocado */
    ts_in(&t, o.d + PA_DESCRIPTOR_SIZE, len);
    ts_get_str(&t);
    CHECK(t.err);

    /* data packet */
    size_t total;
    uint8_t *p = ts_data_packet(5, "abcd", 4, &total);
    pa_descriptor_read(p, &len, &ch, &off, &flags);
    CHECK(total == PA_DESCRIPTOR_SIZE + 4 && len == 4 && ch == 5 && !memcmp(p + PA_DESCRIPTOR_SIZE, "abcd", 4));
    free(p);
    ts_free(&o);

    printf("%s (%d failures)\n", fails ? "sound-test: FAIL" : "sound-test: ok", fails);
    return fails != 0;
}
