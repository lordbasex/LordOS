/* sessiond.h — lord-sessiond: the LordOS session manager (see sessiond.c,
 * docs/SESSIONS.md and docs/REMOTE.md). A root process with three parts:
 *   - the main one (sessiond.c): configuration, session list, login screen;
 *   - the monitor (monitor.c): lends the card and input to the session being shown;
 *   - the remote desktop (conn.c, if enabled): one child per network connection. */
#ifndef LORD_SESSIOND_H
#define LORD_SESSIOND_H

#include <pthread.h>
#include <pwd.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <openssl/ssl.h>

#include "ipnet.h"
#include "lockout.h"

#define RD_CONF_DIR "/etc/lordos/remote"
#define SD_RUN_DIR "/run/lordos"
#define SD_STATUS SD_RUN_DIR "/status.json"
/* created when the first session frame is on the monitor; lordos-splash-quit.service waits for it to
 * end the boot splash (plymouth quit --retain-splash). See docs/SESSIONS.md, "Boot splash". */
#define SD_FIRST_FRAME SD_RUN_DIR "/first-frame"
#define SD_NO_DISPLAY SD_RUN_DIR "/no-display"   /* lordos-splash: the console has to show, nothing else will */
#define SD_GREETER_RUN SD_RUN_DIR "/greeter"        /* XDG_RUNTIME_DIR of the login screen */
#define SD_SESSION_CONF "/etc/lordos/session.conf"   /* AUTOLOGIN=user (empty: login screen) */
#define SD_STATE_DIR "/var/lib/lordos"               /* boot-times.jsonl (docs/SESSIONS.md, "Boot time") */
#define SD_USERS_DIR "/var/lib/lordos/users"         /* avatars: <user>.json and <user>.png */
#define SD_GREETER_USER "lordos-greeter"
#define RD_DEFAULT_PORT 5673          /* "LORD" on a phone keypad */
#define RD_MAX_CONNS 32                /* signed-in browsers (bridges) */
#define RD_MAX_CHILDREN 48             /* connection processes in all, signing in or not */
#define RD_MAX_PER_SOURCE 8            /* of those, from one address (IPv6: one /64) */
#define RD_ACCESS RD_CONF_DIR "/access"  /* manual allow/block lists (admin actions, root 0600) */
#define RD_COOKIE "__Host-lordos_remote"
#define RD_TURN_KEY RD_CONF_DIR "/turn.key"  /* the TURN password (admin action remote-ice, root 0600) */
#define RD_RTC_DIR SD_RUN_DIR "/rtc"         /* ICE-TCP handoff sockets, one per bridge (root 0700; conn.c) */
#define SD_MAX_SESSIONS 32

/* State shared between the main process and its children. */
typedef struct { char key[56]; int fails; double until; } RdFail;   /* key: source, "monitor:<user>" or "u:<user>" */
/* A signed-in browser. Only the SHA-256 of the cookie is kept, and a digest of the account's shadow
 * hash at sign-in: changing or locking the password ends the cookie. */
typedef struct { unsigned char hash[32], pwh[16]; uid_t uid; char user[33]; double expires; } RdToken;
/* "source blocked" notices from the connection processes; the main process shows them on the
 * monitor's session (RP_NOTICE) */
typedef struct { char key[48]; double until, at; int strikes; } RdNotice;
#define RD_NOTICES 8
typedef struct { pid_t pid; char ip[48]; char user[33]; double since; unsigned long long rx, tx; } RdConn;   /* rx/tx: bytes from/to the browser */
/* Recent failed sign-ins (source, attempted name already cleaned to printable ASCII), for Monitor →
 * Network through the admin action remote-failures (administrators only; never a password). */
typedef struct { char key[48]; char user[33]; int count; double first, last; } RdFailLog;
#define RD_FAILLOG 32
#define SD_FAILURES SD_RUN_DIR "/remote-failures"     /* root 0600: "key count first last user" */
typedef struct { pid_t pid; uid_t uid; char user[33]; double since; } SdSession;    /* a user's lord-server */
typedef struct {
    pthread_mutex_t lock;
    RdFail fails[128];
    RdToken tokens[64];
    RlTable lockout;            /* failed-login blocks per source (lockout.h) */
    int lockout_dirty;          /* changed: the main process saves RL_FILE */
    RdNotice notices[RD_NOTICES];
    RdFailLog faillog[RD_FAILLOG];
    unsigned notice_seq;
    RdConn conns[RD_MAX_CONNS];
    SdSession sessions[SD_MAX_SESSIONS];
    char monitor[33];           /* user shown on the monitor ("" = login screen) */
    unsigned version;           /* changes whenever anything above changes (status.json) */
} RdShared;

typedef struct {
    int enabled;                /* remote desktop enabled (Settings → Sharing) */
    int port;
    int group_only;             /* only the lordos-remote group */
    int insecure_dev;           /* allows binaries not owned by root (development VMs only) */
    int net_mode;               /* RS_NET_LAN (default) | RS_NET_LIST | RS_NET_ANY: who may connect */
    RsAccess access;            /* RD_ACCESS */
    /* the WebRTC connection (docs/REMOTE.md, "Video (WebRTC)"): up to 4 stun=host:port, turn=host:port,
     * turn_user=…, turn_transport=udp|tcp|tls and, for TLS, an optional pinned turn_fp=<sha-256>; the
     * TURN password is in RD_TURN_KEY (root 0600) */
    int nstun, turn_transport, turn_has_fp;
    char stun[4][96], turn[96], turn_user[129];
    uint8_t turn_fp[32];
    char server[512], theme[512], greeter_theme[512], fonts[512], shell[512], workdir[512];
} RdConfig;

extern RdShared *rd_shared;
extern RdConfig rd_conf;

double rd_now(void);
void   rd_lock(void);
void   rd_unlock(void);

/* ---- sess.c: users, passwords and sessions (shared by the monitor and the network) ---- */

void sd_log_prefix(const char *prefix);             /* "lord-sessiond[pid] 1.2.3.4" */
void sd_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  sd_valid_username(const char *u);
void sd_dummy_init(void);                           /* hash setting for unknown users (same timing) */
/* A person's regular account: uid 1000..59999, with a shell, and (remote + allow=group) in the group. */
int  sd_user_allowed(const struct passwd *pw, int remote);
int  sd_check_password(const char *user, const char *pass, int remote);
int  sd_remote_account_ok(const char *user);       /* still allowed from the network (cookies) */
double sd_fail_wait(const char *key);               /* seconds to wait for that key */
void sd_fail_note(const char *key, int failed);
int  sd_fail_count(const char *key);
int  sd_run_wait(const char *const argv[]);
/* Socket of that user's session if it's already running (lord-server --headless), or -1. */
int  sd_session_connect(uid_t uid);
/* Connects to the user's session or creates it (w×h); with a per-user lock so the monitor
 * and the network don't create two at once. Returns the fd (as root) or -1. */
int  sd_session_get(const struct passwd *pw, int w, int h);
/* The login screen (lord-server --greeter as lordos-greeter): creates it if it isn't running. */
int  sd_greeter_get(int w, int h);
int  sd_session_running(uid_t uid);                 /* is there a live lord-server for that user? */
/* System user without login for the login screen or the monitor; creates it if missing. */
struct passwd *sd_system_user(const char *name, const char *home);

/* ---- netsup.c: lord-netd, the system network (docs/NETWORK.md) ---- */
void sd_netd_start(void);                           /* at startup; afterwards, sd_netd_tick */
int  sd_netd_fd(void);                              /* pidfd of an adopted lord-netd, or -1 */
void sd_netd_event(void);                           /* that pidfd says it exited */
int  sd_netd_reaped(pid_t pid, int status);         /* 1 if the child was lord-netd */
void sd_netd_tick(void);                            /* restarts it when due */

/* ---- cgsup.c: root-owned app scopes (/run/lordos/cgroup.sock, docs/PRIVACY.md) ---- */
void sd_cgroup_start(void);                         /* after cgroup_service_init */
int  sd_cgroup_reaped(pid_t pid, int status);       /* 1 if the child was the cgroup service */
void sd_cgroup_tick(void);                          /* restarts it if it died */

/* "LORD_GPU=0" with lordos.gpu=0 on the kernel command line (or the service's LORD_GPU), or empty */
extern char sd_gpu_env[24];
#define SD_TUNE_N 4
extern char sd_tune_env[SD_TUNE_N][32]; /* the service's LORD_GPU_SCISSOR, LORD_GPU_DRAG, LORD_PIPELINE, LORD_GPU_VERIFY for user sessions */
extern int sd_input_debug;              /* LORD_INPUT_DEBUG=1 for user sessions (keylag.h) */

/* ---- conn.c: one remote desktop connection (in a child process) ---- */
void rd_serve(SSL_CTX *ctx, int fd, const char *ip, const char *key);
void rd_tokens_drop_uid(uid_t uid);                 /* every cookie of that user (lock) */
void rd_tokens_clear(void);                         /* every cookie (remote turned off) */

/* ---- enc.c: rectangle (0xAARRGGBB) → PNG (the lossless refresh of still areas). malloc'd memory. */
unsigned char *rd_encode_png(const uint32_t *px, int stride, int w, int h, int *len);
unsigned char *rd_encode_rgba(const uint32_t *px, int w, int h, int *len);   /* cursor, with alpha */

/* ---- monitor.c: lending the monitor to a session ---- */
int  sd_mon_open(void);                             /* 0 if there is a video card (and puts the console in graphics mode) */
void sd_mon_close(void);
int  sd_mon_show(int session_fd, const char *who, int keep);  /* gives it the card and input; keeps the fd.
                                                   * keep: the boot splash is still up (RP_DISPLAY_KEEP) */
int  sd_mon_fd(void);                               /* socket of the session being shown, or -1 */
int  sd_mon_read(void (*fn)(int type, const char *json));   /* -1 if the session closed */
void sd_mon_greeter(const char *json);              /* RP_GREETER_IN to the login screen */
void sd_mon_notice(const char *json);               /* RP_NOTICE to the session on the monitor */
void sd_mon_lost(void);                             /* takes it away (revokes) */
void sd_mon_child(void);                            /* in a child: closes the copies without revoking */
int  sd_mon_hotplug_fd(void);                       /* inotify on /dev/input, or -1 */
void sd_mon_hotplug(void);                          /* devices plugged in or removed */

/* Messages from the workers to the main process (SOCK_SEQPACKET socketpair; one line, sometimes with
 * an fd): "show g" + fd · "show u <user>" + fd · "greeter-in <json>" */
int  sd_ctl_send(int ctl, const char *text, int fd);
int  sd_ctl_recv(int ctl, char *buf, size_t cap, int *fd);   /* length, 0 if closed, -1 on error */

#endif
