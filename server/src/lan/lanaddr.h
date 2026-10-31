/* lanaddr.h — which addresses Files → Network may touch, and the arguments of the share-* actions.
 * Pure functions, the same on both sides of the admin helper (tests/lan_test.c). */
#ifndef LORD_LAN_ADDR_H
#define LORD_LAN_ADDR_H

#include <stddef.h>
#include <stdint.h>

/* a (host order) in 10/8, 172.16/12, 192.168/16 or 169.254/16. Never loopback, multicast, CGNAT or
 * a public address. */
int lan_private(uint32_t a);

/* The sweep's range for an interface: [*first, *last] (host order). Its subnet, or the /24 around
 * the address when the subnet is larger, so a sweep is never more than 254 addresses; a /31 or
 * /32 has none. Returns how many. */
int lan_sweep_range(uint32_t addr, uint32_t mask, uint32_t *first, uint32_t *last);

/* printable name for the interface: no control characters, UTF-8 only, at most n-1 bytes, trimmed.
 * Returns 0 if nothing is left. */
int lan_clean_text(const char *in, char *out, size_t n);

/* ---- share-mount: "smb|nfs\n<ipv4>\n<share or path>\n<user>\n<domain>\n<password>" ---- */
typedef struct {
    int nfs;
    char addr[16];
    char share[256];        /* SMB: the share's name; NFS: the export's absolute path */
    char user[64];          /* SMB; "" is the guest */
    char domain[64];        /* "" for none */
    const char *password;   /* inside the parsed buffer; "" for none */
} ShareReq;

/* Parses and validates buf in place (it stays NUL-separated). 0 ok, -1 refused. */
int share_req_parse(char *buf, ShareReq *r);

/* The folder's name under /media/<user>: "<share> on <addr>", without / or control characters and
 * never starting with a dot. */
void share_leaf(const ShareReq *r, char *out, size_t n);

/* /media/<user>/<one folder>: 0 ok. For share-unmount, before it looks at the mount table. */
int share_path_ok(const char *path, const char *user);

/* kernel file system names a share may have */
int share_fstype(const char *fstype);

#endif
