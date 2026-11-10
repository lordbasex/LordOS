/* mprismeta.h — the pure part of the MPRIS client (hw/mpris.c): reading a player's properties
 * (org.mpris.MediaPlayer2 and org.mpris.MediaPlayer2.Player, MPRIS 2.2) from a validated D-Bus
 * body into bounded fields, without a bus. Tested by tests/mpris_test.c.
 *
 * Everything a player says is untrusted: every string is cut to its field (at a UTF-8 character
 * boundary) with control characters turned into spaces, lengths and positions are clamped to
 * [0, MPRIS_MAX_US], and the cover art is kept only when it is a local file (file://, decoded to
 * an absolute path without "..", control characters or a query); remote and data: art is dropped. */
#ifndef LORD_HW_MPRISMETA_H
#define LORD_HW_MPRISMETA_H

#include <stdint.h>
#include "../vault/dbus.h"

#define MPRIS_PREFIX     "org.mpris.MediaPlayer2."
#define MPRIS_MAX_US     (7ll * 24 * 3600 * 1000000)   /* a week: longer lengths and positions are clamped */
#define MPRIS_ARTISTS    4                             /* xesam:artist entries joined with ", " */

enum { MPRIS_STOPPED = 0, MPRIS_PAUSED = 1, MPRIS_PLAYING = 2 };

/* What changed in one dictionary (mpris_read_props' result). */
enum { MP_IDENTITY = 1, MP_STATUS = 2, MP_META = 4, MP_POS = 8, MP_CAPS = 16 };

typedef struct {
    char identity[96];
    char title[256], artist[256], album[256];
    char art[1024];                      /* a local path, or "" */
    char trackid[256];                   /* mpris:trackid (an object path), for SetPosition */
    int status;                          /* MPRIS_STOPPED / PAUSED / PLAYING */
    int64_t length_us, position_us;
    uint8_t can_play, can_pause, can_next, can_prev, can_seek, can_control;
} MprisProps;

/* Default values (CanControl true until the player says otherwise). */
void mpris_props_init(MprisProps *p);
/* Reads an a{sv} dictionary at r (the body of GetAll, or the second argument of PropertiesChanged)
 * into p. Unknown keys and keys of an unexpected type are skipped. Returns the MP_* bits that were
 * present, or -1 if the body is not a dictionary. A Metadata entry replaces the whole track. */
int  mpris_read_props(DRd *r, MprisProps *p);
/* The answer to Get(Position): a variant holding an integer (r at its start). 0 ok (*us clamped), -1 not. */
int  mpris_read_position(DRd *r, int64_t *us);
/* Copies at most n-1 bytes of in, never cutting a UTF-8 character, with control characters (and
 * invalid bytes) as spaces and surrounding spaces removed. */
void mpris_clean(const char *in, char *out, size_t n);
/* file:///path → /path (percent-decoded). 0 ok, -1 refused (another scheme, a host, "..", a query,
 * a control character, an encoded NUL or slash, too long). */
int  mpris_art_path(const char *url, char *out, size_t n);
/* A bus name a player may own: org.mpris.MediaPlayer2.<something>, a valid name, ≤ 255 bytes. */
int  mpris_name_ok(const char *name);
/* "Playing" / "Paused" / "Stopped" → MPRIS_*; -1 for anything else. */
int  mpris_status_of(const char *s);
/* Clamps a length or position (µs) to [0, MPRIS_MAX_US]. */
int64_t mpris_clamp_us(int64_t v);

#endif
