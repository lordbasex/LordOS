/* mpris.h — MPRIS client on the session bus: the media players of Linux apps (browsers, video and
 * music players) publish org.mpris.MediaPlayer2.<name>; lord-server follows them so Control Center's
 * Now Playing shows them and the media keys reach them (sdk/media.js, docs/MEDIA.md, "Linux players").
 * Uses our own D-Bus client (vault/dbus.c) on its own connection.
 *
 *   - Players come and go with NameOwnerChanged (and ListNames at start); each one's Identity,
 *     PlaybackStatus, Metadata, Position and Can* are read with GetAll and followed with
 *     PropertiesChanged and Seeked. Only signals and replies from the name's current owner count.
 *   - Nothing waits for a player: every call is asynchronous, a reply that doesn't come within
 *     MPRIS_CALL_MS is forgotten, and controls are sent without asking for a reply.
 *   - Bounded: at most MPRIS_MAX_PLAYERS players and MPRIS_MAX_PENDING calls in flight; strings,
 *     lengths and cover art are cut and checked by hw/mprismeta.c.
 *
 * The theme gets the list as the hw event "mpris" (__lordHw) whenever it changes. Only the desktop's
 * own context has lord.mpris (ui/js_hw.c); isolated apps have no way to it through the broker. */
#ifndef LORD_HW_MPRIS_H
#define LORD_HW_MPRIS_H

#include <stddef.h>

#define MPRIS_MAX_PLAYERS 16
#define MPRIS_MAX_PENDING 48
#define MPRIS_CALL_MS     3000

typedef void (*MprisEvent)(const char *kind, const char *json);   /* "mpris" */

enum { MPRIS_TOGGLE, MPRIS_PLAY, MPRIS_PAUSE, MPRIS_NEXT, MPRIS_PREV, MPRIS_SEEK };

int  mpris_start(MprisEvent fn);         /* 0 if connected to the session bus */
int  mpris_fd(void);
int  mpris_wants_write(void);
void mpris_dispatch(void);
/* Every pass of the main loop: forgets calls that timed out and sends the list if it changed. */
void mpris_tick(void);
/* The list as JSON (malloc, the caller frees it): [{ name, identity, status, title, artist, album, art,
 * length, pos, canPlay, canPause, canNext, canPrev, canSeek }], times in ms, pos estimated now. */
char *mpris_json(void);
/* A control for the player that owns name: cmd MPRIS_*; for MPRIS_SEEK, ms is the position, clamped to
 * the track's length. 0 sent, -1 no such player or it can't do that. */
int  mpris_control(const char *name, int cmd, double ms);
/* Reads every player's properties again (Position isn't signalled while it plays). */
void mpris_refresh(void);

#endif
