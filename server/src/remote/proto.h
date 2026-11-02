/* proto.h — local protocol between lord-server (the session), lord-sessiond (which lends it the
 * monitor) and the remote desktop network bridge.
 *
 * Session unix socket: $XDG_RUNTIME_DIR/lordos/remote.sock (lord-server creates it already as the
 * user, 0600; root can also get in). Each message: u32 length (of what follows, little endian),
 * u8 type and the data. See docs/REMOTE.md and docs/SESSIONS.md. */
#ifndef LORD_REMOTE_PROTO_H
#define LORD_REMOTE_PROTO_H

#define RP_MAX_MSG (40u * 1024 * 1024)   /* a 2560×1600 rectangle fits with room to spare */

/* session → bridge */
enum {
    RP_SIZE = 1,        /* u16 width, u16 height: screen size (on connect and when it changes) */
    RP_RECT = 2,        /* u16 x, y, w, h + w*h u32 pixels (0xAARRGGBB in little-endian memory) */
    RP_END = 3,         /* end of frame: the bridge answers RP_READY once it delivered it */
    RP_CURSOR = 4,      /* i16 x, y, u8 mode (0 arrow, 1 image, 2 hidden) */
    RP_SHAPE = 5,       /* u16 w, h, hx, hy + w*h u32 pixels: an app's cursor image */
    RP_CLIP = 6,        /* UTF-8 text: the session clipboard changed */
    RP_SHOWN = 7,       /* no data, to lord-sessiond only: the first frame after an RP_DISPLAY with
                         * RP_DISPLAY_KEEP is on the screen (the boot splash can go; docs/SESSIONS.md) */
    RP_SESSION = 9,     /* JSON: request from the session to the monitor ({"a":"lock"} or {"a":"switch"}) */
    RP_GREETER = 20,    /* JSON: request from the login screen to lord-sessiond (greeter mode only) */
    /* the video path (docs/REMOTE.md, "Video (WebRTC)"): the session's shared encoder */
    RP_VFRAME = 40,     /* u8 codec (1 H.264, 2 VP8), u8 keyframe, u16 0, u32 ms + the encoded frame */
    RP_AFRAME = 41,     /* u32 first sample (48 kHz) + one Opus packet */
    RP_DAMAGE = 42,     /* video mode: u16 x, y, w, h per rectangle that changed in this frame */
    RP_VINFO = 43,      /* u8 encoder in hardware (VA-API), u8 composed on the GPU, u16 the encoder's frames a
                         * second, u16 encoding time per frame in 1/100 ms: shown in the page's top bar */
};

/* bridge → session */
enum {
    RP_READY = 10,      /* ready for the next frame (flow control) */
    RP_PTR = 11,        /* i16 x, y (absolute, in session pixels) */
    RP_BTN = 12,        /* u8 button (0 left, 1 middle, 2 right), u8 pressed */
    RP_WHEEL = 13,      /* i8 steps (+ up) */
    RP_KEY = 14,        /* u16 evdev code, u8 value (1 pressed, 0 released, 2 repeat) */
    RP_SETCLIP = 15,    /* UTF-8 text the user copied in their browser */
    RP_HELLO = 16,      /* "ip user": who connected (notice on the monitor) */
    RP_RESIZE = 17,     /* u16 width, u16 height: requested size (ignored if the session is on the monitor) */
    RP_SEAT = 18,       /* this connection is lord-sessiond, which drives the monitor: it doesn't get the screen,
                         * only the requests (RP_SESSION, RP_GREETER); root only */
    RP_DISPLAY = 19,    /* + fds (SCM_RIGHTS): the video card (already master) and the input
                         * devices. The session starts showing on the monitor (with GPU if any). Root only.
                         * Data: u8 number of fds, optional u8 flags (RP_DISPLAY_KEEP) */
    RP_UNDISPLAY = 22,  /* the session leaves the monitor (goes back to drawing in memory). Root only */
    RP_GREETER_IN = 21, /* JSON: reply or notice from lord-sessiond to the login screen */
    RP_INPUT_ADD = 23,  /* + fds: keyboards, mice or touchpads plugged in later (only to the monitor's
                         * session; the session closes unplugged ones when it gets ENODEV). Root only */
    RP_KEYRING = 24,    /* text: the password just verified on the login screen; the session opens
                         * its keyring with it (lord-vault, docs/PASSWORDS.md) and wipes it. Root only */
    RP_NOTICE = 25,     /* JSON, from lord-sessiond (root, seat) only: a notice for the user on the monitor,
                         * {"t":"blocked","ip":"…","min":N,"n":N} (a source blocked after failed remote logins) */
    RP_SCROLL = 30,     /* u8 phase (1 begins, 2 continues, 3 ends), i32 dx, dy in 1/256 pixel: browser trackpad */
    RP_PINCH = 31,      /* u8 phase, i32 scale ×65536 (relative to the start), i32 rotation in degrees ×256 */
    RP_VIDEO = 32,      /* u8 codec (0 stop), u8 sound, u16 0, u32 kbit/s: this viewer takes the encoded video */
    RP_KEYFRAME = 33,   /* the viewer lost a frame: the next one is a keyframe */
    RP_RATE = 34,       /* u32 kbit/s: what this viewer's network takes now */
    RP_VMODE = 35,      /* u8 1: video mode (RP_RECT only once the screen is still, RP_DAMAGE at once); 0: tiles */
};

/* RP_DISPLAY flags */
#define RP_DISPLAY_KEEP 1   /* the screen still shows the boot splash: keep it (no black first frame)
                             * until the session's first frame, then answer RP_SHOWN */

#endif
