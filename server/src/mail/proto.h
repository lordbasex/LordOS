/* proto.h — channel between axia-server and axia-maild (see docs/MAIL.md).
 *
 * The same framing as axia-doc (doc/wire.h): a SOCK_SEQPACKET socketpair on fd 3, each message a
 * DocHdr + JSON (+ up to DOC_MAX_FDS-1 descriptors via SCM_RIGHTS; large JSON goes in a sealed
 * memfd). Requests carry an id; replies use the same id; events use id 0.
 *
 * axia-maild has no way to open a socket (seccomp). To reach a server it sends MR_CONNECT
 * {"host","port"}; axia-server answers MQ_SOCK with a connected TCP socket, but only if that
 * endpoint is one of the configured accounts (or a remote-image host the user just allowed). */
#ifndef LORD_MAIL_PROTO_H
#define LORD_MAIL_PROTO_H

/* axia-server → axia-maild */
enum {
    MQ_INIT = 1,        /* {"config":{…},"zone":"Europe/Madrid","lang":"es"} + fd of the cache folder */
    MQ_REQ,             /* {"op":…} (+ fds of files to attach / photos) */
    MQ_SOCK,            /* reply to MR_CONNECT: 1 fd, or {"error":"denied"|"dns"|"connect"} */
    MQ_CONFIG,          /* the accounts file changed: {"config":{…}} */
};

/* axia-maild → axia-server */
enum {
    MR_DONE = 1,        /* request finished: JSON result */
    MR_ERR,             /* request failed: {"error":"code","detail":"…"} */
    MR_EVT,             /* event (id 0): {"ev":"new"|"sync"|"changed"|"need-auth"|"cert"|"error"|…} */
    MR_CONNECT,         /* {"host","port"}: please connect me (id = connect id) */
    MR_IMG,             /* request finished with an image: {"w","h"} + sealed memfd of w*h ARGB32 */
};

#define MAIL_MAX_ACCOUNTS 32
#define MAIL_CONFIG_MAX   (512 * 1024)

#endif
