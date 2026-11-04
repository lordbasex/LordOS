/* netpriv.h — internal pieces of the network module (netlink, cryptography, configurations). */
#ifndef LORD_NETPRIV_H
#define LORD_NETPRIV_H

#include <stddef.h>
#include <stdint.h>
#include <netinet/in.h>

/* ---- text: growing buffer and JSON ---- */

typedef struct { char *p; size_t len, cap; int oom; } Buf;
void buf_printf(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void buf_json_str(Buf *b, const char *s);       /* "text" with escapes */
void buf_free(Buf *b);

/* ---- netlink ---- */

typedef struct { char *p; size_t len, cap; } NlMsg;      /* message under construction */

int  nl_open(int proto, unsigned groups);              /* SOCK_RAW | CLOEXEC */
void nlm_init(NlMsg *m, uint16_t type, uint16_t flags);
void nlm_put(NlMsg *m, const void *data, size_t len);  /* fixed body (ifinfomsg, genlmsghdr...) */
void nlm_attr(NlMsg *m, uint16_t type, const void *data, size_t len);
void nlm_attr_u8(NlMsg *m, uint16_t type, uint8_t v);
void nlm_attr_u16(NlMsg *m, uint16_t type, uint16_t v);
void nlm_attr_u32(NlMsg *m, uint16_t type, uint32_t v);
void nlm_attr_str(NlMsg *m, uint16_t type, const char *s);
size_t nlm_nest_begin(NlMsg *m, uint16_t type);
void nlm_nest_end(NlMsg *m, size_t at);
void nlm_free(NlMsg *m);
/* Sends and waits for the ACK; returns 0 or -errno. */
int  nl_talk(int fd, NlMsg *m);
/* Sends a dump request and calls fn for each reply message. */
struct nlmsghdr;
typedef void (*NlFn)(const struct nlmsghdr *h, void *ud);
int  nl_dump(int fd, NlMsg *m, NlFn fn, void *ud);

/* Attributes of a message: tb[type] points to the rtattr (or NULL). */
#define NL_MAXATTR 64
struct rtattr;
void nl_parse(struct rtattr **tb, int max, const void *attrs, size_t len);

/* ---- rtnetlink: operations (need CAP_NET_ADMIN) ---- */

int  rt_ifindex(const char *name);
int  rt_link_set(int fd, int ifindex, int up);
int  rt_link_new(int fd, const char *name, const char *kind);   /* "wireguard" */
int  rt_link_del(int fd, int ifindex);
int  rt_link_mtu(int fd, int ifindex, unsigned mtu);
/* family AF_INET/AF_INET6; addr of 4 or 16 bytes. valid/pref in seconds (0 = forever). */
int  rt_addr(int fd, int add, int ifindex, int family, const void *addr, int prefix, uint32_t valid, uint32_t pref);
int  rt_addr_flush(int fd, int ifindex, int family);
/* gw may be NULL; table 0 = main. */
int  rt_route(int fd, int add, int family, const void *dst, int prefix, const void *gw, int ifindex, uint32_t metric, uint32_t table);
/* Routing rule like wg-quick: "not fwmark M table T" (invert) or "table main suppress_prefixlength 0". */
#define RTPROT_LORDOS 76                /* routes of LordOS's VPNs (so they can always be cleaned up) */
int  rt_route_proto(int fd, int add, int family, const void *dst, int prefix, const void *gw, int ifindex, uint32_t metric, uint32_t table, int proto);
int  rt_get(int fd, int family, const void *dst, uint8_t gw[16], int *has_gw, int *oif);   /* ip route get */
int  rt_flush_proto(int fd, int proto, int family, const void *dst);
int  rt_link_new_xfrm(int fd, const char *name, uint32_t if_id);
int  rt_rule(int fd, int add, int family, uint32_t fwmark, int invert, uint32_t table, int suppress_prefix, uint32_t priority);

/* ---- addresses ---- */

typedef struct { int family; uint8_t a[16]; int prefix; } Cidr;
int  parse_cidr(const char *s, Cidr *c, int need_prefix);   /* "10.0.0.2/24", "fd00::1/64" */
int  parse_ip(const char *s, int *family, uint8_t a[16]);
void fmt_ip(int family, const void *a, char *out, size_t n);

/* ---- minimal cryptography ---- */

void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
void x25519_public(uint8_t pub[32], const uint8_t priv[32]);
void x25519_clamp(uint8_t k[32]);
int  b64_encode(const uint8_t *in, size_t n, char *out, size_t cap);
int  b64_decode(const char *in, uint8_t *out, size_t cap);   /* returns bytes or -1 */
void pbkdf2_sha1(const uint8_t *pass, size_t plen, const uint8_t *salt, size_t slen, unsigned iter, uint8_t *out, size_t olen);
void sha1(const uint8_t *data, size_t n, uint8_t out[20]);
int  random_bytes(void *out, size_t n);

/* ---- configuration files (root) ---- */

#define NET_DIR     "/etc/lordos/net"
#define NET_IF_DIR  NET_DIR "/if"
#define NET_VPN_DIR NET_DIR "/vpn"
#define NET_WIFI_DIR NET_DIR "/wifi"
#define NET_RUN_DIR "/run/lordos"

int  valid_ifname(const char *s);
int  valid_vpn_name(const char *s);
int  write_file_atomic(const char *path, const char *data, size_t len, unsigned mode);
char *read_file(const char *path, size_t max, size_t *len);    /* malloc, 0-terminated */
/* Value of "key=value" in a text of lines (returns 1 if it was there). Walks all of them with idx. */
int  kv_get(const char *text, const char *key, int idx, char *out, size_t n);

/* ---- system ---- */

int  managed_by_system(const char *ifname);     /* systemd-networkd or ifupdown manage it */
int  resolved_active(void);
/* DNS of an interface (systemd-resolved with resolvectl, or /etc/resolv.conf). domain "~." = everything. */
/* mode: DNS_PLAIN doesn't touch resolved's "default-route" (DHCP, static); DNS_FULL (full
 * tunnel) adds "~." and receives all queries; DNS_SPLIT (split tunnel) only those for its
 * domains, if it has any. */
enum { DNS_PLAIN, DNS_FULL, DNS_SPLIT };
int  dns_set(const char *ifname, const char *servers, const char *domains, int mode);
void dns_revert(const char *ifname);

/* ---- pieces of the actions ---- */

void dhcp_run(const char *ifname, int wifi);    /* DHCP client (doesn't return); wifi: waits for association */
int  dhcp_lease_router(const char *ifname, uint32_t *router);  /* gateway of its saved lease; 0 ok */
/* Does dst answer an ARP request from src on ifname (addresses in network order)? 0 yes. */
int  arp_probe(const char *ifname, uint32_t src, uint32_t dst, int tries, int wait_ms);
/* After waking from sleep: is this Wi-Fi interface carrying traffic? WK_OK or what fails (wake.h). */
int  net_wake_health(const char *ifname);
int  wg_up(const char *name, char *err, size_t errn);
int  wg_down(const char *name);
int  wg_status_json(const char *name, Buf *out);
int  wg_import(const char *name, const char *text, char *pub_out, char *err, size_t errn);
int  ovpn_import(const char *name, const char *text, char *err, size_t errn);
void ovpn_run(const char *name);                /* daemon: launches openvpn and manages it */
int  wifi_start(const char *ifname);            /* wpa_supplicant + the interface's daemon */
int  wifi_ctrl(const char *ifname, const char *cmd, char *reply, size_t n);
void wifi_act_scan(const char *ifname);
void wifi_act_connect(const char *ifname, const char *ssid, const char *pass);
void wifi_act_disconnect(const char *ifname);
void wifi_act_forget(const char *ssid);
int  ovpn_down(const char *name);
int  ovpn_status_json(const char *name, Buf *out);
int  ovpn_set_credentials(const char *name, const char *user, const char *pass);
/* Interface of a VPN (wg-<name> or ovpn-<name>, truncated to 15). */
void vpn_ifname(const char *type, const char *name, char *out, size_t n);

/* Format of the EVENTs: "net <topic> <object> <state> [detail]". */
#define NET_EVENT(...) admin_event(__VA_ARGS__)

#endif
