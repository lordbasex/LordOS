/* secretsvc.h — the freedesktop Secret Service API (org.freedesktop.secrets) over D-Bus, with its
 * own minimal D-Bus client (dbus.h: no libdbus or sd-bus). See docs/PASSWORDS.md,
 * "Secret Service".
 *
 * This is how Chromium, Firefox, libsecret/secret-tool and other Linux apps store their passwords in
 * the LordOS keyring without gnome-keyring or kwallet. Runs in axia-vault: axia-server connects the
 * session bus socket and hands it over already open (the helper cannot open sockets).
 *
 * Objects:
 *   /org/freedesktop/secrets                       Service (OpenSession, SearchItems, Unlock,
 *                                                  Lock, GetSecrets, ReadAlias, SetAlias,
 *                                                  CreateCollection; prop. Collections)
 *   /org/freedesktop/secrets/collection/login      Collection (alias "default")
 *   /org/freedesktop/secrets/collection/login/<id> Item
 *   /org/freedesktop/secrets/session/<n>           Session (algoritmos "plain" y
 *                                                  "dh-ietf1024-sha256-aes128-cbc-pkcs7")
 *   /org/freedesktop/secrets/prompt/<n>            Prompt (unlock with a system notice)
 * Storage belongs to the keyring (keyring.c), through the interface below. */
#ifndef LORD_VAULT_SECRETSVC_H
#define LORD_VAULT_SECRETSVC_H

#include <stddef.h>
#include <stdint.h>

typedef struct { const char *name, *value; } SsAttr;

typedef struct {
    char id[40];               /* item id (hex, no '/') */
    char *label;               /* from the backend; free with ss_info_free */
    SsAttr *attrs; int nattrs;
    uint64_t created, modified;
} SsItemInfo;

/* What the keyring provides (keyring.c). They return 0 ok, -1 error; the pointers they hand out
 * (ss_info, secrets) are freed with free_info/free_secret of the same backend. */
typedef struct {
    int  (*locked)(void *ud);                                           /* 1 if the keyring is locked */
    /* Unlock request from an app: 0 = already unlocked, 1 = a notice was shown to the
     * user (answer later with ss_prompt_done), -1 = not possible. */
    int  (*request_unlock)(void *ud, unsigned prompt_id);
    int  (*lock)(void *ud);
    /* finds items whose attributes contain all the requested ones; ids[i] up to 40 bytes */
    int  (*search)(void *ud, const SsAttr *attrs, int nattrs, char (*ids)[40], int max);
    int  (*info)(void *ud, const char *id, SsItemInfo *out);
    void (*free_info)(void *ud, SsItemInfo *in);
    int  (*get_secret)(void *ud, const char *id, uint8_t **secret, size_t *len, char ctype[64]);
    void (*free_secret)(void *ud, uint8_t *secret, size_t len);
    /* creates (or replaces if replace and one exists with the same attributes) → out_id */
    int  (*create)(void *ud, const char *label, const SsAttr *attrs, int nattrs,
                   const uint8_t *secret, size_t len, const char *ctype, int replace, char out_id[40]);
    int  (*set_secret)(void *ud, const char *id, const uint8_t *secret, size_t len, const char *ctype);
    int  (*set_label)(void *ud, const char *id, const char *label);
    int  (*set_attrs)(void *ud, const char *id, const SsAttr *attrs, int nattrs);
    int  (*remove)(void *ud, const char *id);
    /* who is calling (unique bus name and its pid, if the bus provided it): for logging and notices */
    void (*caller)(void *ud, const char *sender, uint32_t pid);
} SsBackend;

/* Starts on a socket already connected to the session bus: SASL EXTERNAL, Hello and
 * RequestName("org.freedesktop.secrets", DO_NOT_QUEUE). 0 ok; 1 if the name already belongs to another
 * (gnome-keyring or kwallet running): not served; -1 error. */
int  ss_start(int busfd, const SsBackend *be, void *ud);
int  ss_fd(void);                 /* for poll(); -1 if not started */
/* Serves whatever is on the socket (does not block). -1 if the bus closed. */
int  ss_dispatch(void);
/* Result of the unlock notice prompt_id (1 unlocked, 0 cancelled): emits Completed. */
void ss_prompt_done(unsigned prompt_id, int unlocked);
/* Tell clients that something changed (ItemCreated/Changed/Deleted signals, Locked prop.). */
void ss_notify_locked(int locked);
void ss_notify_item(const char *id, int what);  /* 0 created, 1 changed, 2 deleted */
void ss_stop(void);

/* ---- additions ---- */
int  ss_pending_out(void);        /* 1 if something is left to send: add POLLOUT to poll() */
const char *ss_unique_name(void); /* ":1.42" of the bus, or "" */
/* Notes:
 *   - The connection state uses malloc (wiped with explicit_bzero), not smem: sm_wipe_all() on
 *     lock does not break it. The secrets of each request go through smem and are wiped immediately.
 *   - Do not call ss_dispatch() at the same time as sm_wipe_all() from another thread.
 *   - ss_notify_item() is for changes made elsewhere (the app); those arriving over D-Bus already
 *     emit their signal. */
/* DH session arithmetic (also for tests): base^exp mod mod, all 128-byte big endian,
 * mod odd and base < mod. 0 ok. */
int  ss_dh_modexp(const uint8_t base[128], const uint8_t exp[128], const uint8_t mod[128], uint8_t out[128]);
const uint8_t *ss_dh_prime(void); /* the 1024-bit prime of RFC 2409 group 2 */

#endif
