/* mpulse.h — playback or recording stream for media over the PulseAudio protocol,
 * asynchronous, in axia-server (mpulse.c). */
#ifndef LORD_MEDIA_MPULSE_H
#define LORD_MEDIA_MPULSE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int fd, version, killed, underflows;
    int want, ready, failed;            /* 1 play / 2 record; stream created; PipeWire said no */
    char app_name[64], app_id[64], media_name[128], role[32], source[256];
    uint32_t tag;
    uint8_t *in, *out;
    size_t in_len, in_cap, out_len, out_cap;
    /* waiting for a command's reply */
    uint32_t wait_tag;
    int wait_what, wait_done;
    /* playback */
    uint32_t play_ch, play_index, frame_bytes;
    int rate, channels, corked;
    int64_t requested;                  /* bytes the server asked for that weren't sent */
    int64_t written;                    /* bytes sent since the stream was created */
    /* latency (GET_PLAYBACK_LATENCY) */
    uint32_t lat_tag;
    int lat_pending, lat_valid, lat_playing;
    double lat_sent_ms, lat_at_ms;
    int64_t read_index, write_index;
    uint64_t sink_usec;
    /* recording */
    uint32_t rec_ch, rec_index;
    void (*on_record)(void *ud, const uint8_t *d, size_t n);
    void *ud;
} LmPulse;

/* Starts the handshake (doesn't wait). Then lmp_playback or lmp_record say which stream to create;
 * it becomes ready on its own, as the replies arrive in lmp_dispatch. */
int  lmp_connect(LmPulse *p, int fd, const char *app_name, const char *app_id);
int  lmp_playback(LmPulse *p, int rate, int ch, const char *media_name, const char *role);
int  lmp_record(LmPulse *p, const char *source, int rate, int ch, const char *media_name);
int  lmp_dispatch(LmPulse *p);          /* reads what arrived; -1 if the connection dropped */
int  lmp_write(LmPulse *p, const void *d, size_t n);
void lmp_cork(LmPulse *p, int on);
void lmp_flush(LmPulse *p);
void lmp_latency(LmPulse *p);
void lmp_volume(LmPulse *p, double v);  /* 0..1.5, on the stream (visible in Settings → Sound) */
void lmp_close(LmPulse *p);

#endif
