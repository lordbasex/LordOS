/* streamurl.h — the addresses lord-stream may open (0.8.8.10; cameras and IPTV, maintainer's approval 2026-11-16) and
 * the M3U playlists it reads. Pure functions, no I/O (tests/streamurl_test.c); used by lord-server before it launches
 * lord-stream and by lord-stream again before FFmpeg sees the address.
 *
 * An address is "scheme://rest": a scheme from STREAM_SCHEMES, then printable ASCII with no space, quote, backslash
 * or control character, at most STREAM_URL_MAX bytes. Nothing else reaches FFmpeg: no file:, pipe:, concat:,
 * subfile:, data:, async:, crypto+ or "a|b" chains, and FFmpeg's own protocol_whitelist in lord-stream repeats the
 * network-only list. Credentials in the address (rtsp://user:password@host) are allowed: lord-server gets them from
 * the keyring and passes them inside the job, never through a file or the command line. */
#ifndef LORD_MEDIA_STREAMURL_H
#define LORD_MEDIA_STREAMURL_H

#include <stddef.h>

#define STREAM_URL_MAX 2048
#define STREAM_SCHEMES { "rtsp", "rtsps", "rtmp", "rtmps", "rtmpt", "srt", "rtp", "udp", "http", "https", NULL }
/* what FFmpeg may use underneath them (HLS and DASH over http(s), AES-128 HLS, RTSP's RTP over UDP or TCP) */
#define STREAM_FF_PROTOCOLS "rtsp,rtsps,rtmp,rtmps,rtmpt,srt,rtp,srtp,udp,tcp,tls,http,https,httpproxy,hls,crypto"

/* 1 if lord-stream may open it. */
int stream_url_ok(const char *url);
/* 1 if the scheme is one where a playlist (M3U) can be fetched: http, https. */
int stream_m3u_url_ok(const char *url);

/* ---- M3U / M3U8 channel lists (IPTV) ----
 * One entry per address line; the #EXTINF line before it gives its name (after the last comma) and the attributes
 * group-title and tvg-logo. Lines that aren't an allowed address are skipped. Names are cut at STREAM_NAME_MAX bytes,
 * control characters dropped. */
#define STREAM_NAME_MAX 120
typedef struct {
    char name[STREAM_NAME_MAX + 1];
    char group[STREAM_NAME_MAX + 1];
    char logo[512];                  /* http(s) only, else empty */
    char url[STREAM_URL_MAX + 1];
} StreamEntry;
/* Calls fn for each channel, at most max; returns how many. A file that isn't an M3U (no #EXTM3U and no address)
 * gives 0. */
int stream_m3u_parse(const char *text, size_t len, int max, void (*fn)(const StreamEntry *e, void *ud), void *ud);

#endif
