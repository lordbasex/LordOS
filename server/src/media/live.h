/* live.h — the live media link between a packaged app's companion and its lord-media (docs/PACKAGED-APPS.md, "Live media").
 *
 * One SOCK_STREAM socketpair per stream, no other way in: lord-server makes it, gives one end to lord-media and the other
 * to the companion through fd 6. Both ends are sandboxed and neither trusts the other. Every message is a LiveHdr plus
 * `len` bytes. An encoded picture may be large, so the link is a stream and the length is checked before anything is read. */
#ifndef LORD_MEDIA_LIVE_H
#define LORD_MEDIA_LIVE_H

#include <stdint.h>

#define LIVE_MAX (8u << 20)             /* one message, as MM_PACKET's limit */

typedef struct {
    uint32_t len;                       /* bytes after this header */
    uint8_t  type;                      /* LL_* */
    uint8_t  flags;                     /* LF_* */
    uint16_t pad;
    int64_t  pts;                       /* ms */
} LiveHdr;

_Static_assert(sizeof(LiveHdr) == 16, "live header");

enum {
    /* companion → lord-media */
    LL_CODEC = 1,                       /* payload "h264" or "vp8": the pictures that follow */
    LL_VIDEO,                           /* one encoded picture */
    LL_OPUS,                            /* one Opus packet, played */
    LL_KEYFRAME,                        /* a camera: the next picture is a keyframe */
    /* lord-media → companion */
    LL_WANTKEY = 64,                    /* the decoder lost the picture: ask the far side for a keyframe */
    LL_MIC,                             /* one Opus packet of the microphone */
    LL_CAMERA,                          /* one encoded picture of the camera */
};

enum { LF_KEY = 1 };

#endif
