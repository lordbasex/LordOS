/* sshmux.h — a client of OpenSSH's connection sharing (ControlMaster), the mux protocol of ssh's
 * PROTOCOL.mux (version 4). The Terminal's ssh is made a master by the shell integration
 * (ControlPath in the session's own folder, docs/TERMINAL.md "Files mode"): through its control
 * socket lord-server opens new channels on the connection the user already has, with no second
 * login and no program run: an `sftp` subsystem (lord-sftp speaks it), a remote command from a
 * closed list, or a local port forward. Only lord-server uses it; nothing here reaches JS.
 *
 * Every call blocks (each with its own timeout) and is made from a worker thread. The socket path
 * comes from sshmux_find(), never from JS or from the terminal's output. */
#ifndef LORD_TERM_SSHMUX_H
#define LORD_TERM_SSHMUX_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define MUX_MSG_HELLO          0x00000001u
#define MUX_C_NEW_SESSION      0x10000002u
#define MUX_C_ALIVE_CHECK      0x10000004u
#define MUX_C_OPEN_FWD         0x10000006u
#define MUX_C_CLOSE_FWD        0x10000007u
#define MUX_S_OK               0x80000001u
#define MUX_S_PERMISSION_DENIED 0x80000002u
#define MUX_S_FAILURE          0x80000003u
#define MUX_S_EXIT_MESSAGE     0x80000004u
#define MUX_S_ALIVE            0x80000005u
#define MUX_S_SESSION_OPENED   0x80000006u
#define MUX_S_REMOTE_PORT      0x80000007u
#define MUX_S_TTY_ALLOC_FAIL   0x80000008u
#define MUX_FWD_LOCAL          1u

#define MUX_PACKET_MAX         (256u * 1024u)   /* a master's packet past this is refused */

/* ---- the wire (pure: tests/sshmux_test.c) ---- */
typedef struct { uint8_t *p; size_t n, cap; int oom; } MuxBuf;
void mxb_reset(MuxBuf *b);
void mxb_u8(MuxBuf *b, uint8_t v);
void mxb_u32(MuxBuf *b, uint32_t v);
void mxb_str(MuxBuf *b, const char *s, size_t n);
void mxb_free(MuxBuf *b);
/* A packet's body (after the length) read field by field; 0 ok, -1 short or too long. */
typedef struct { const uint8_t *p; size_t n, off; } MuxRd;
int  mxr_u32(MuxRd *r, uint32_t *v);
int  mxr_str(MuxRd *r, const char **s, size_t *n);
/* The request bodies (without the length prefix), as ssh's mux client writes them. */
void mux_hello(MuxBuf *b);
void mux_alive(MuxBuf *b, uint32_t id);
void mux_new_session(MuxBuf *b, uint32_t id, int subsystem, const char *command);
void mux_fwd(MuxBuf *b, int open, uint32_t id, const char *listen_host, uint32_t listen_port, const char *connect_host, uint32_t connect_port);

/* A remote command quoted for the remote shell: 'arg' for each argument, joined by spaces. -1 when an argument
 * holds a quote, a backslash or a control character (login shells quote differently), or out is too small. */
int  mux_quote_args(const char *const *argv, char *out, size_t cap);

/* ---- talking to a master ---- */
typedef struct {
    int fd;               /* the control connection, -1 when closed */
    uint32_t next_id;
    pid_t master;         /* the master's pid (MUX_S_ALIVE) */
    int refused;          /* mux_connect: nobody listens on the socket (ECONNREFUSED) */
    char why[160];        /* why the last call failed */
} Mux;

/* Connects to the control socket at path and exchanges hellos and an alive check. 0 ok. The
 * socket's owner must be this user (SO_PEERCRED). */
int  mux_connect(Mux *m, const char *path, int timeout_ms);
void mux_close(Mux *m);
/* Opens a session channel: subsystem ("sftp") or command (already quoted). in/out/err are the fds
 * the master uses as the channel's stdin, stdout and stderr (sent with SCM_RIGHTS; the caller
 * closes its copies after). 0 ok, the session open; the control connection must stay open while
 * it runs (closing it ends the session). */
int  mux_session(Mux *m, int subsystem, const char *command, int in, int out, int err, int timeout_ms);
/* Waits up to timeout_ms for the session's end: its exit status, or -1 (timeout, or the master
 * went away without one). */
int  mux_wait_exit(Mux *m, int timeout_ms);
/* A local forward on the master: listen_port on 127.0.0.1 (only) to connect_host:connect_port
 * as seen from the server. 0 ok. */
int  mux_forward(Mux *m, int open, uint32_t listen_port, const char *connect_host, uint32_t connect_port, int timeout_ms);

/* 1 when name is the shell integration's ControlPath in the folder: hex (ssh's %C), '-', the shell's pid. */
int  sshmux_name_ok(const char *name);
/* The master of a Terminal session: in dir (the session's own folder, 0700 and this user's), the
 * control socket whose master's pid is want (the pane's foreground process). The path in out.
 * Only sockets named as sshmux_name_ok() says are tried. One that refuses the connect (its ssh was
 * killed and left it) does not count against the 32 masters asked, and is removed when it is still
 * this user's socket. 0 found, -1 none. */
int  sshmux_find(const char *dir, pid_t want, char *out, size_t cap);

#endif
