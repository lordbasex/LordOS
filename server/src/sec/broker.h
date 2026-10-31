/* broker.h — the "broker": the only channel between an isolated .lpk app (lord-server --app) and
 * lord-server. The SDK inside the app process proxies the lord.* calls over this socket;
 * lord-server checks the permission for each one (server/src/sec/perms.c) and, if needed, draws
 * the system prompt. See appclient.c (app side), brokersrv.c (server side) and
 * docs/PRIVACY.md.
 *
 * Protocol: SOCK_SEQPACKET messages. Each message = BrokerHdr + payload (text/JSON). Replies
 * carry the same reqid. The file picker (powerbox) and the devices pass
 * fds via SCM_RIGHTS. */
#ifndef LORD_SEC_BROKER_H
#define LORD_SEC_BROKER_H

#include <stddef.h>
#include <stdint.h>
#include <poll.h>

#define BROKER_FD_ENV "LORD_BROKER_FD"
#define BROKER_MAX_MSG (1024 * 1024)

enum BrokerOp {
    BOP_FS_HOME = 1,    /* → path of the app's data folder */
    BOP_FS_READ,        /* payload: path relative to the data folder → contents */
    BOP_FS_WRITE,       /* payload: "path\0contents" inside the data folder */
    BOP_FS_LIST,        /* payload: path → JSON [{name,dir}] */
    BOP_CLIP_GET,       /* (clipboard permission) → clipboard text */
    BOP_CLIP_SET,       /* payload: text → to the clipboard */
    BOP_PERM,           /* payload: permission name → "1"/"0" (asks if needed) */
    BOP_DEVICE,         /* payload: "camera on" | "microphone off" → sets the indicator */
    BOP_PICK,           /* (powerbox) opens the system dialog → fd of the chosen file */
    BOP_THEME,          /* → JSON of the theme tokens */
    BOP_QUIT,           /* the app is closing */
    BOP_STATE,          /* → JSON { theme, locale, keyboard }: the current state, at startup */
    BOP_MENU,           /* payload: JSON of the app's menus (for the system menu bar) */
    BOP_MIC,            /* payload "1"/"0": request/release the microphone. With permission, → read-only fd of the ring */
    BOP_CAMERA,         /* payload "list" | "open dev w h" | "ring sid" (→ read-only ring fd) | "ack sid id" |
                         * "size sid w h" | "photo sid mirror name" | "record sid mirror mic name" |
                         * "recstop sid" | "close sid". See brokercam.c */
    BOP_VAULT_FIDO = 40,/* (Passwords only) open the FIDO2 key for lord-vault after the `usb` permission →
                         * "name"; the app never gets the device (docs/PASSWORDS.md) */
    BOP_CLIP_SECRET,    /* payload "seconds\0text": a secret to the clipboard, marked sensitive (out of
                         * the history, x-kde-passwordManagerHint) and cleared after `seconds` if unchanged */
    BOP_KEYRING,        /* payload "op\0service\0account\0label\0secret" (op: get | set | delete | list) → the
                         * app's own session-keyring items, owner = the verified app id (brokerkr.c,
                         * docs/PASSWORDS.md). Error replies carry an error code line (keyring.*) */
    BOP_MAIL,           /* (system Mail, Calendar, Contacts, Reminders only) lord-maild through lord-server:
                         * payload "verb\0rest", fds via SCM_RIGHTS for "req"/"reqm". Replies and events
                         * come back as "mail" events, in chunks. See brokermail.c, docs/PRIVACY.md */
    BOP_SYS_OPEN,       /* (those same system apps only) payload "app\0new\0json": open another of them
                         * with these args (Mail → Calendar for an invitation, Contacts → a new message) */
    BOP_PROBE,          /* (the system package `sysprobe` only, app/probe.c) payload: JSON of what its sandbox
                         * refused; lord-server adds the process's own status and keeps it for lord.sysinfo */
    BOP_TEXT_FOCUS,     /* payload "on y h": a text field of the app took or lost focus (window coordinates).
                         * Only a hint for the on-screen keyboard: no text, and no way to press a key */
    BOP_EVENT = 100,    /* server → app, unrequested (reqid 0): payload "type\0json" */
};

enum BrokerStatus { BST_OK = 0, BST_DENIED = 1, BST_ERROR = 2, BST_PENDING = 3 };

typedef struct {
    uint32_t reqid;
    uint16_t op;        /* BrokerOp (request) */
    uint16_t status;    /* BrokerStatus (reply); 0xffff in a request */
    uint32_t len;       /* payload bytes that follow */
} BrokerHdr;

#define BROKER_REQ 0xffff

/* ---- server side (lord-server, brokersrv.c) ---- */

/* Registers a new broker for app <id> (pid, for the cgroup/monitor). Returns -1 if there is no
 * room. main.c polls the fd. */
int  brokersrv_add(int fd, const char *id, int pid);
/* fds for poll(); returns how many it wrote and their indices. */
int  brokersrv_pollfds(struct pollfd *pfd, int max);
void brokersrv_dispatch(struct pollfd *pfd, int n);
/* Delivers pending prompts and indicator changes to JS. */
typedef void (*BrokerEventFn)(const char *kind, const char *json);
void brokersrv_events(BrokerEventFn fn);
/* User's answer to a prompt (from JS: lord.broker.resolve). */
void brokersrv_resolve(uint32_t reqid, int allow);
int  brokersrv_count(void);
/* The app on broker fd `fd` was spawned with a channel to lord-vault (system Passwords app): it may
 * use BOP_VAULT_FIDO. */
void brokersrv_set_vault(int fd);
/* The app on broker fd `fd` is a system package (assets/sysapps, verified by app_package_dir): it
 * keeps the keyring owner and lord-maild access it had in-process. */
void brokersrv_set_system(int fd);
/* Args for an isolated app (wm.open(id, { args }) in the session): sent now if it is running, or
 * kept and sent when it starts. `json` is a JSON object (≤ 16 KiB). 0 if sent or kept. */
int  brokersrv_open_args(const char *id, const char *json, size_t len, int new_window);
/* The app <id> is running: ask it for a new window. 0 if sent, -1 if it is not running. */
int  brokersrv_new_window(const char *id);
/* Quit app <id> (❖Q, Dock → Quit): the app host closes all its windows, its close guards still
 * apply, and the process ends when the last one closes. 0 if sent, -1 if it is not running. */
int  brokersrv_quit(const char *id);
/* The id of the isolated app whose process is `pid` (a Wayland client's pid), or NULL. */
const char *brokersrv_app_of_pid(int pid);
/* The theme publishes its state (type "theme" or "locale", JSON) and it's forwarded live to every
 * isolated app; it's also kept for the ones that start later (BOP_STATE). */
int  brokersrv_publish(const char *kind, const char *json, size_t len);
/* On every loop iteration: forwards the keyboard layout if it changed and retries the sends
 * that didn't fit (the server never blocks waiting on an app). */
void brokersrv_tick(void);
/* The user revoked a permission in Settings → Privacy: the app is notified (it releases the
 * device) and its indicator is turned off. */
void brokersrv_revoke(const char *app, const char *perm);
/* The user picked a file in the system dialog (powerbox): lord-server opens it and passes
 * the fd to the app. Empty or NULL path = cancelled. */
void brokersrv_pick_result(uint32_t reqid, const char *path);
/* The user picked an item from an app's menu (from the system menu bar): the app is notified
 * through the broker. */
void brokersrv_menu_click(const char *app, const char *item);
/* JSON of the menus an app published (for the system menu bar); "[]" if none. */
const char *brokersrv_menus(const char *app);
/* System Information's last sandbox probe (BOP_PROBE): JSON { probe, proc, at } or NULL. */
const char *brokersrv_probe_result(void);
/* The microphone (and later the camera) runs in lord-server: its PipeWire socket has to be
 * serviced in the main loop. */
int  brokersrv_audio_pollfds(struct pollfd *pfd, int max);
void brokersrv_audio_dispatch(struct pollfd *pfd, int n);


#endif
