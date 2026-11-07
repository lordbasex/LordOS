/* cloud.h — Cloud (docs/CLOUD.md): the person's cloud storage accounts, mounted in ~/Cloud with files on demand.
 *
 * A lord-server thread runs `rclone rcd` (CLOUD_RCLONE) for the session: on a Unix socket in
 * $XDG_RUNTIME_DIR/lordos/cloud (0700), with a random user and password per start given through a memfd
 * (--rc-htpasswd), an in-memory configuration (--config ""), in its own scope (app-lordos-lordos-cloud.scope,
 * firewall app desktop:lordos-cloud). It talks to it only through the closed method table of cloud_pure.h.
 * Secrets (OAuth tokens, passwords, keys) live in the keyring and reach rclone over the socket; the files in
 * ~/.config/lordos/cloud hold none. Changes reach JS through an eventfd (cloud_fd → cloud_dispatch → __lordCloud).
 *
 * Every call returns at once: long work goes to the thread. Results are JSON text in new memory (free it). */
#ifndef LORD_CLOUD_H
#define LORD_CLOUD_H

#include <stddef.h>

/* where secrets are kept: lord-server uses the keyring (cloud_kr.c); the test harness a file */
typedef struct {
    int (*get)(const char *id, char **secret);      /* 0 and *secret (malloc), 1 none, -2 locked, -3 no keyring, -1 failed */
    int (*set)(const char *id, const char *label, const char *secret);   /* 0, or as get */
    int (*del)(const char *id);
} CloudSecrets;

int   cloud_start(const char *home, const CloudSecrets *sec);   /* once, as the session user */
void  cloud_stop(void);                                         /* end of the session: unmounts, rclone quits */
int   cloud_fd(void);                                           /* -1 if not started */
/* something changed: push(state JSON); a link/unlink finished: reply(req, result JSON); a sign-in address
 * to open in the default browser: open_url(url) → 0 opened */
void  cloud_dispatch(void (*push)(const char *json), void (*reply)(int req, const char *json), int (*open_url)(const char *url));

int   cloud_available(void);                                    /* lordos-rclone is installed */
char *cloud_state_json(void);
/* the calls that take a *req: > 0, the answer comes through reply(req, …); otherwise the JSON returned is the answer */
char *cloud_providers(int *req);                                /* rclone's backends, asked every time (17.2 item 1) */

/* account_id: Sign In Again for that account, whatever name is given */
char *cloud_begin_signin(const char *kind, const char *name, const char *client_id, const char *client_secret, const char *account_id);
int   cloud_signin_url(const char *id, char *url, size_t n);    /* 0: the address of that sign-in */
void  cloud_cancel_signin(const char *id);
char *cloud_add(const char *kind, const char *name, const char *fields_json, const char *account_id, int *req);   /* { id } | { question } | { error } */
char *cloud_answer(const char *state, const char *value, int apply_others, int *req);   /* apply_others: N4 */                       /* rclone's next question, or done */
char *cloud_empty_trash(const char *id, int *req);
char *cloud_measure(const char *id, int *req);                  /* { bytes, count } */
char *cloud_cancel_measure(const char *id, int *req);
char *cloud_delete(const char *const *paths, int n, int *req);
char *cloud_search(const char *path, const char *needle, int *req);   /* { hits: [{ path, dir, size, mtime }], complete } */  /* only strictly under ~/Cloud/<account>, through the mount */
void  cloud_set_assets(const char *dir);                        /* where cloud/providers.json is (the verified kinds) */
char *cloud_remove(const char *id, int discard_pending);
char *cloud_rename(const char *id, const char *name);
void  cloud_refresh_about(const char *id);                      /* also retries an account that isn't connected */
void  cloud_retry(const char *id);                              /* accounts not connected try again; NULL: all */
char *cloud_website(const char *id);                            /* the address, or NULL */
char *cloud_status(const char *const *paths, int n);
/* B10: path is a cloud file not on this computer whose account can't be reached now (offline, locked, not running,
 * not mounted): 1, and lord.fs.read fails at once instead of waiting for rclone */
int   cloud_read_would_block(const char *path);
void  cloud_keep(const char *const *paths, int n, int keep);
char *cloud_free(const char *const *paths, int n);
char *cloud_link(const char *path, const char *expire, int *req);
char *cloud_unlink(const char *path, int *req);
char *cloud_links(const char *id);
void  cloud_pause(const char *id, int pause);                   /* id NULL: every account */
char *cloud_set_options(const char *json);                      /* {ok} | {error} */
void  cloud_restart(void);
const char *cloud_log_name(void);                               /* ~/.cache/lordos/<this>, shown by Logs */

#endif
