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
    SHARE_DISPLAY = 57, /* u32 id: show this display (an id of SHARE_DISPLAYS), SW_DISPLAY_ALL every display in one
                         * picture, 0 the default (Settings → Displays, "Screenshots and sharing") */
};

/* system → client, besides RP_SIZE, RP_VFRAME, RP_AFRAME, RP_CURSOR, RP_SHAPE, RP_CLIP and RP_VINFO */
enum {
    SHARE_STATE = 60,   /* u8 reason a start would be refused now (0: it wouldn't), u8 granted (SW_F_*), u8 codecs (bit 0
                         * H.264, bit 1 VP8), u8 0, u16 w, u16 h, u16 hz */
    SHARE_REFUSED = 61, /* u8 reason */
    SHARE_END = 62,     /* u8 reason */
    SHARE_DISPLAYS = 63,/* u32 current (the id shown now), u8 n, then n × { u32 id, i32 x, i32 y, u16 w, u16 h (logical),
                         * u16 pw, u16 ph (the picture's pixels), u8 flags (SW_DF_*), u8 name length, name (UTF-8) } */
};

/* The displays a share can show (SHARE_DISPLAY, SHARE_DISPLAYS). An id is the display's connector, stable while it
 * stays plugged; a session with no monitor has one display, id 1. With more than one display the list ends with
 * SW_DISPLAY_ALL: the whole logical space at 100 %, as the "all displays" setting gives. */
#define SW_DISPLAY_ALL 0xffffffffu
#define SW_DISPLAYS_MAX 8               /* real displays in the list (OUT_MAX); the "all" entry comes on top */
#define SW_DISPLAY_NAME 64              /* bytes of a name, cut at a code point */
enum { SW_DF_MAIN = 1, SW_DF_ALL = 2 };
typedef struct { uint32_t id; int x, y, w, h, pw, ph, flags; char name[SW_DISPLAY_NAME + 1]; } SwDisplay;
/* SHARE_DISPLAYS' data for n entries (at most SW_DISPLAYS_MAX + 1): the bytes written, 0 if it doesn't fit in cap.
 * Names are cleaned on the way: control characters and invalid UTF-8 become nothing, and they're cut to
 * SW_DISPLAY_NAME bytes at a code point. */
size_t sw_displays_encode(uint8_t *out, size_t cap, uint32_t current, const SwDisplay *d, int n);
/* Is id one a client may ask for, given that list? (0, the default, always is.) */
int sw_display_known(uint32_t id, const SwDisplay *d, int n);

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
    uint32_t display;                   /* SHARE_DISPLAY */
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

/* SHARE_TEXT on a layout with dead keys: a precomposed Latin letter (á, Ñ, ü, ç, å…) as its base letter (UTF-8, in
 * base) and the dead key's mark ("´", "`", "^", "~", "¨", "¸", "°", the labels kb_label gives dead keys). 1 if it has
 * one, 0 if not. The session types the dead key and then the letter, and the layout's own compose makes the letter. */
int sw_dead_split(uint32_t cp, char base[8], const char **mark);

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
