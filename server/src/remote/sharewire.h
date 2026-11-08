/* sharewire.h — the wire format of the public screen-sharing API (docs/SCREEN-SHARING.md) and the small
 * pure rules around it: framing, one strict parser per message, rate limits. No I/O and no state of the
 * session, so tests/share_test.c covers it and tests/share_fuzz.c runs it under ASan+UBSan.
 *
 * The framing is proto.h's: u32 length (of what follows, little endian), u8 type, data. Unlike the
 * session socket, nothing here is lenient: a wrong length or an unknown type ends the connection. */
#ifndef LORD_REMOTE_SHAREWIRE_H
#define LORD_REMOTE_SHAREWIRE_H

#include <stddef.h>
#include <stdint.h>

#define SW_MAX_MSG (1u << 20)           /* the biggest message a client may send: a clipboard text */
#define SW_TEXT_MAX 1024                /* SHARE_TEXT, bytes */

/* client → system, besides the RP_* input and video messages of proto.h */
enum {
    SHARE_START = 50,   /* u8 flags (SW_F_*), u8 codec (1 H.264, 2 VP8), u16 refresh rate, u32 kbit/s */
    SHARE_QUERY = 51,   /* no data: answers SHARE_STATE; starts nothing and prompts nobody */
    SHARE_STOP = 52,    /* no data */
    SHARE_AWAKE = 53,   /* u8 on: keeps the display from going idle while the share lasts */
    SHARE_LOCK = 54,    /* no data: locks the session, which ends the share */
    SHARE_TEXT = 55,    /* UTF-8, at most SW_TEXT_MAX bytes, typed through the active keyboard layout */
    SHARE_HEAR = 56,    /* u8 on: RP_AFRAME is sent, or dropped, from now on */
};

/* system → client, besides RP_SIZE, RP_VFRAME, RP_AFRAME, RP_CURSOR, RP_SHAPE, RP_CLIP and RP_VINFO */
enum {
    SHARE_STATE = 60,   /* u8 reason a start would be refused now (0: it wouldn't), u8 granted (SW_F_*), u8 codecs (bit 0
                         * H.264, bit 1 VP8), u8 0, u16 w, u16 h, u16 hz */
    SHARE_REFUSED = 61, /* u8 reason */
    SHARE_END = 62,     /* u8 reason */
};

enum { SW_F_VIDEO = 1, SW_F_AUDIO = 2, SW_F_INPUT = 4, SW_F_CLIPBOARD = 8, SW_F_ALL = 15 };

/* A closed list. The text names are what the app and the logs see. */
typedef enum {
    SW_OK = 0,
    SW_NO_IDENTITY, SW_DENIED, SW_NO_PERSON, SW_NO_INDICATOR, SW_BUSY, SW_LOCKED, SW_AWAY, SW_REMOTE_DESKTOP,
    SW_RESTRICTED, SW_REVOKED, SW_STOPPED, SW_BAD_REQUEST, SW_ENDED_BY_APP, SW_REASONS,
} SwReason;
const char *sw_reason_name(int reason);

/* One message out of a byte stream. 1: f is set (it points into buf); 0: more bytes are needed; -1: the stream
 * is wrong (length 0 or above SW_MAX_MSG + 1) and the connection must end. */
typedef struct { int type; const uint8_t *data; uint32_t n; size_t used; } SwFrame;
int sw_next(const uint8_t *buf, size_t len, SwFrame *f);

/* One message, checked: every length is exact and every value is in range. */
typedef struct {
    int type;
    int flags, codec, hz;               /* SHARE_START */
    unsigned kbps;                      /* SHARE_START, RP_RATE */
    int on;                             /* SHARE_AWAKE, SHARE_HEAR */
    int x, y;                           /* RP_PTR (0..32767), RP_RESIZE (w, h) */
    int button, down;                   /* RP_BTN */
    int steps;                          /* RP_WHEEL */
    int phase, dx, dy;                  /* RP_SCROLL: 1/256 pixel */
    int code, value;                    /* RP_KEY */
    const char *text; uint32_t tlen;    /* SHARE_TEXT, RP_SETCLIP: valid UTF-8 without NUL, not terminated */
} SwMsg;
int sw_parse(const SwFrame *f, SwMsg *m);   /* 0, or -1: the connection must end */

/* Is this valid UTF-8 with no NUL and no code point above U+10FFFF or in the surrogates? */
int sw_utf8_valid(const char *s, size_t n);
/* The next code point of valid text: returns its length in bytes (s[0..n) must be valid). */
int sw_utf8_next(const char *s, size_t n, uint32_t *cp);

/* The keys one remote client holds down (RP_KEY codes 1..0x2fe), so that each key goes down once and up once
 * whatever the client sends. xkbcommon counts the presses of a modifier: a second "pressed" without a release
 * (Windows repeats a held Ctrl in the browser) would leave Ctrl on after the only release (#44).
 * sw_held_key() takes the client's value (0 released, 1 pressed, 2 repeat) and returns what the session gets:
 * a press of a key already down becomes a repeat; -1 drops a repeat or a release of a key not pressed here.
 * Everything still held is released (value 0) when the client goes. */
#define SW_KEY_CODES 0x300
typedef struct { uint8_t bit[SW_KEY_CODES / 8]; } SwHeld;
int sw_held_key(SwHeld *h, int code, int value);
int sw_held_down(const SwHeld *h, int code);     /* 1 if `code` is down */

/* A rate limit: `rate` events a second, `burst` at once. */
typedef struct { double tokens, at; } SwBucket;
int sw_bucket_take(SwBucket *b, double now_ms, double rate, double burst);  /* 1 if the event may go */

/* Keyframes (the encoder makes one only when asked). A request never gets lost: it waits for its turn.
 *  - at most one every SW_KEY_MIN_MS, always;
 *  - when the screen changed since the last one, that's the only limit;
 *  - on a still screen a keyframe is the only thing that would be encoded, and a viewer that asks on a
 *    timer would make a stream of them, so requests spend tokens: SW_KEY_BURST, one more every
 *    SW_KEY_STILL_MS. A first request, or a new viewer after a quiet spell, is served at once. */
#define SW_KEY_MIN_MS 500.0
#define SW_KEY_STILL_MS 30000.0
#define SW_KEY_BURST 3.0
typedef struct { double last, tokens, at; int pending, changed; } SwKeyLimit;
void   sw_key_init(SwKeyLimit *k, double now_ms);
void   sw_key_request(SwKeyLimit *k);           /* a client asked (or fell behind) */
void   sw_key_changed(SwKeyLimit *k);           /* the screen changed */
int    sw_key_due(SwKeyLimit *k, double now_ms);  /* 1: ask the encoder now (the request is spent) */
double sw_key_wait(const SwKeyLimit *k, double now_ms);    /* ms until it can be due, -1 with nothing pending */

#endif
