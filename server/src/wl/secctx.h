/* secctx.h — wp_security_context_v1: Wayland connections a sandbox opened for its app (see secctx.c). */
#ifndef LORD_SECCTX_H
#define LORD_SECCTX_H

#include <stddef.h>

#define SECCTX_STR_MAX 256              /* engine, app id and instance id: 1 to 255 bytes */
#define SECCTX_MAX 64                   /* contexts alive at once, over every client */
#define SECCTX_ACCEPT_BURST 16          /* connections taken per wakeup of a listener */

/* What a sandbox engine said of the connections it opens: "" when it said nothing */
typedef struct { char engine[SECCTX_STR_MAX], app_id[SECCTX_STR_MAX], instance_id[SECCTX_STR_MAX]; } SecMeta;

enum { SECCTX_ENGINE, SECCTX_APP_ID, SECCTX_INSTANCE };
enum { SECCTX_OK, SECCTX_ERR_SET, SECCTX_ERR_BAD };

/* 1 if s is 1 to max-1 bytes of printable ASCII without spaces (reverse-DNS names, numbers) */
int secctx_str_ok(const char *s, size_t max);
/* Sets one field (SECCTX_ENGINE…) once: SECCTX_ERR_SET the second time (the already_set error),
 * SECCTX_ERR_BAD for an unknown field or a string secctx_str_ok refuses (invalid_metadata). *set holds
 * which fields were set. */
int secctx_meta_set(SecMeta *m, unsigned *set, int field, const char *s);
/* 1 for a global a client in a security context never sees: the manager itself (no nesting) and the
 * interfaces that only make sense for trusted clients, the ones that read other windows, the screen, the
 * clipboard without focus, or make input (the list is in secctx.c) */
int secctx_hidden(const char *iface);
/* 1 if fd is a listening AF_UNIX stream socket (bind and listen done), the only listen_fd taken */
int secctx_listen_ok(int fd);

struct wl_display;
struct wl_client;
/* The wp_security_context_manager_v1 global and the display's global filter. may_create (may be NULL)
 * says who sees the manager besides being outside a context: lord-server offers it only to clients known
 * to be unsandboxed. 0 or -1. */
int secctx_init(struct wl_display *dpy, int (*may_create)(struct wl_client *c));
/* The metadata of a client that connected through a security context; NULL for any other client */
const SecMeta *secctx_of(struct wl_client *c);

#endif
