/* share.h — the public screen-sharing API (share.c, docs/SCREEN-SHARING.md). A separate door from remote.sock:
 * its own socket, table and parser; only the composed frame, the shared encoder and the formats are in common. */
#ifndef LORD_REMOTE_SHARE_H
#define LORD_REMOTE_SHARE_H

#include <poll.h>
#include <stddef.h>
#include <stdint.h>

/* What main.c gives the API. Coordinates are the frame's pixels; the checks answer for right now. */
typedef struct {
    void (*pointer)(int x, int y);
    void (*button)(int button, int down);
    void (*wheel)(int steps);
    void (*scroll)(int phase, double dx, double dy);
    void (*key)(int code, int value);
    void (*clipboard)(const char *text, unsigned len);
    void (*resize)(int w, int h);
    void (*activity)(void);             /* SHARE_AWAKE: the display doesn't go idle */
    void (*text)(const char *utf8, unsigned len);
    void (*event)(const char *json);    /* to the shell: the prompt and the indicator */
    void (*no_taps)(void);              /* text queued but not typed yet is dropped */
    int  (*person)(void);               /* can someone at this computer answer a prompt? */
    int  (*surface_up)(void);           /* something shows in the overlay, modal or system layers */
    int  (*key_protected)(void);        /* a key now would go to a protected window */
    int  (*point_protected)(void);      /* a click now would land on a protected window */
    int  (*restricted)(const char *id); /* the firewall forbids this app the screen */
} ShareHost;

/* Not called for the login screen: that session has no listener at all. 0 on success. */
int  share_start(const ShareHost *h);
void share_stop(void);
int  share_pollfds(struct pollfd *p, int max);
void share_dispatch(const struct pollfd *p, int n);
void share_tick(void);                  /* limits, the grant still holding, the wait for the indicator */
double share_timeout(void);             /* ms until share_tick has work, -1 none */

/* Session state, called synchronously where it changes, before any other frame can go out. */
typedef enum { SHARE_S_LOCK, SHARE_S_SWITCH, SHARE_S_LEFT, SHARE_S_SHOWN } ShareSession;
void share_session(ShareSession what);

/* mirror.c: the holder as one more viewer of the session's encoder */
int  share_watching(void);              /* the share is running: frames are wanted */
int  share_codec(void);                 /* 1, 2, or 0 */
int  share_audio_on(void);
unsigned share_kbps(void);
int  share_fps(void);
const char *share_holder(void);         /* the app holding the session, or NULL: the other door stays shut */
void share_video(int codec, int key, uint32_t ms, const uint8_t *p, size_t n);
void share_audio(int codec, uint32_t samples, const uint8_t *p, size_t n);
void share_cursor(int x, int y, int mode);
void share_shape(const uint32_t *argb, int w, int h, int hx, int hy);
void share_size(int w, int h);
void share_vinfo(int codec, const void *m, size_t n);
void share_changed(void);               /* the screen changed (keyframe limit) */
void share_clipboard(const char *text, unsigned gen, int sensitive);

/* the shell (lord.share) */
void share_shown(unsigned token);       /* the indicator is on screen */
void share_hidden(void);                /* it no longer is: the share ends */
void share_answer(unsigned token);      /* the prompt was answered: the store decides */
int  share_user_stop(void);             /* the indicator's Stop, or the shortcut in C; 1 if a share was ended */
void share_reload(void);                /* the store changed */
void share_shell_gone(void);            /* the shell's context is being replaced */
/* {"app","token","input","started"} of the running share, or "null" */
void share_status(char *json, size_t n);

#endif
