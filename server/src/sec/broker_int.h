/* broker_int.h — internals shared by the broker's server-side files (brokersrv.c,
 * brokercam.c and brokerkr.c). Not for the rest of lord-server: use broker.h. */
#ifndef LORD_SEC_BROKER_INT_H
#define LORD_SEC_BROKER_INT_H

#include "broker.h"

/* App id of broker connection `conn` (index), or NULL if it is not in use. */
const char *brokersrv_conn_app(int conn);
/* Event to that app (never blocks). 0 if sent, -1 if the socket is full or gone. */
int  brokersrv_conn_event(int conn, const char *kind, const char *data, size_t len);
/* Reply to request reqid on fd, optionally passing passfd (SCM_RIGHTS). */
int  brokersrv_reply(int fd, uint32_t reqid, int status, const void *data, uint32_t len, int passfd);
/* A live link's end to the companion of connection `conn`, with a line naming it. -1 if it has none running. */
int  brokersrv_conn_link(int conn, const char *msg, int passfd);
/* Camera/microphone indicator for the menu bar (only the session's JS draws it). */
void brokersrv_device(const char *app, const char *kind, int on);
/* System save dialog (powerbox) for `app`; the answer comes back as brokersrv_pick_result(reqid). */
void brokersrv_powerbox_save(uint32_t reqid, const char *app, const char *suggest);

/* A system consent prompt (privacy.js draws it; the answer comes back as brokersrv_resolve). */
void brokersrv_prompt_event(const char *json);

/* ---- brokerkr.c: the session keyring of isolated apps (BOP_KEYRING) ---- */
void brokerkr_handle(int conn, int fd, uint32_t reqid, const char *app, char *payload, uint32_t len);
int  brokerkr_resolve(uint32_t prompt, int allow);             /* 1 if it was a keyring prompt */
void brokerkr_conn_closed(int conn);
void brokerkr_tick(void);
int  brokerkr_pollfd(void);                                    /* lord-vault replies waiting, or -1 */

/* Is broker connection `conn` a system package? */
int  brokersrv_conn_system(int conn);
/* Event for the session's JS (privacy.js __lordBroker): kind, preformatted JSON (≤ 4 KiB). */
void brokersrv_shell_event(const char *kind, const char *json);

/* ---- brokermail.c: lord-maild for the isolated system Mail, Calendar, Contacts and Reminders ---- */
/* fds: received with the request; the handler closes or keeps every one of them. */
void brokermail_handle(int conn, int fd, uint32_t reqid, const char *app, char *payload, uint32_t len, int *fds, int nfds);
void brokermail_sys_open(int conn, int fd, uint32_t reqid, const char *app, char *payload, uint32_t len);
void brokermail_conn_closed(int conn);
void brokermail_tick(void);
int  brokermail_pollfd(void);                                  /* lord-maild replies waiting, or -1 */
int  brokermail_wants_write(int conn);                         /* output queued for that app */

/* ---- brokercam.c: the camera of isolated apps ---- */
void brokercam_handle(int conn, int fd, uint32_t reqid, const char *payload, uint32_t len);
int  brokercam_pick_result(uint32_t reqid, const char *path);   /* 1 if it was a camera save */
void brokercam_conn_closed(int conn);
void brokercam_revoke(const char *app);
void brokercam_revoke_mic(const char *app);                     /* stops recordings that use the microphone */
void brokercam_tick(void);

#endif
