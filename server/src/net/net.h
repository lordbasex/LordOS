/* net.h — LordOS's own networking and VPN (see docs/NETWORK.md).
 *
 * Two sides:
 *  - axia-server (no root): reads the state with rtnetlink (interfaces, addresses, routes,
 *    counters) and listens for changes; lord.net (ui/js_net.c) exposes it to JS.
 *  - the privileged helper (admin.c, root): the "net-*" actions in the table below:
 *    configuring interfaces, our own DHCP client, Wi-Fi (wpa_supplicant), WireGuard (our own
 *    netlink) and OpenVPN. */
#ifndef LORD_NET_H
#define LORD_NET_H

#include <stddef.h>
#include "../admin.h"

/* ---- axia-server side ---- */

int  net_open(void);                    /* rtnetlink notification socket; 0 ok */
int  net_fd(void);                      /* for poll; -1 if not there */
/* There were notifications: consumes them and, if something changed, calls fn once (JS __lordNet). */
void net_dispatch(void (*fn)(const char *what));

/* JSON (malloc; the caller frees) for lord.net. NULL on failure. */
char *net_interfaces_json(void);
char *net_routes_json(void);
char *net_dns_json(void);
char *net_vpn_list_json(void);          /* public metadata from /etc/lordos/net/vpn */
char *net_wifi_known_json(void);
/* [{type, available, optional, package, insecure}] of the VPN types */
char *net_vpn_types_json(void);
char *net_if_config_json(const char *ifname);
/* Reads a user's .ovpn and embeds the files it names (ca, cert, key, tls-auth...)
 * that are in the same folder. NULL + err if it can't. */
char *net_ovpn_inline(const char *path, char *err, size_t errn);

/* ---- helper side (root): actions ---- */

void net_act_restore(const AdminReq *r, const char *arg, size_t len);
void net_act_up(const AdminReq *r, const char *arg, size_t len);
void net_act_down(const AdminReq *r, const char *arg, size_t len);
void net_act_config(const AdminReq *r, const char *arg, size_t len);
void net_act_manage(const AdminReq *r, const char *arg, size_t len);
void net_act_unmanage(const AdminReq *r, const char *arg, size_t len);
void net_act_wifi_scan(const AdminReq *r, const char *arg, size_t len);
void net_act_wifi_connect(const AdminReq *r, const char *arg, size_t len);
void net_act_wifi_disconnect(const AdminReq *r, const char *arg, size_t len);
void net_act_wifi_forget(const AdminReq *r, const char *arg, size_t len);
void net_act_vpn_import(const AdminReq *r, const char *arg, size_t len);
void net_act_vpn_credentials(const AdminReq *r, const char *arg, size_t len);
void net_act_vpn_up(const AdminReq *r, const char *arg, size_t len);
void net_act_vpn_down(const AdminReq *r, const char *arg, size_t len);
void net_act_vpn_delete(const AdminReq *r, const char *arg, size_t len);
void net_act_vpn_status(const AdminReq *r, const char *arg, size_t len);
void net_act_vpn_enable(const AdminReq *r, const char *arg, size_t len);

/* Privileged helper (admin.c) actions this module adds. A_ANYONE: also a user who isn't
 * an administrator (connecting to a network or to an already configured VPN). */
#define NET_ADMIN_ACTIONS \
    { "net-restore", net_act_restore, A_ANYONE }, \
    { "net-up", net_act_up, 0 }, \
    { "net-down", net_act_down, 0 }, \
    { "net-config", net_act_config, 0 }, \
    { "net-manage", net_act_manage, 0 }, \
    { "net-unmanage", net_act_unmanage, 0 }, \
    { "net-wifi-scan", net_act_wifi_scan, A_FORK | A_ANYONE }, \
    { "net-wifi-connect", net_act_wifi_connect, A_ANYONE }, \
    { "net-wifi-disconnect", net_act_wifi_disconnect, A_ANYONE }, \
    { "net-wifi-forget", net_act_wifi_forget, 0 }, \
    { "net-vpn-import", net_act_vpn_import, 0 }, \
    { "net-vpn-credentials", net_act_vpn_credentials, 0 }, \
    { "net-vpn-up", net_act_vpn_up, A_ANYONE }, \
    { "net-vpn-down", net_act_vpn_down, A_ANYONE }, \
    { "net-vpn-delete", net_act_vpn_delete, 0 }, \
    { "net-vpn-status", net_act_vpn_status, A_ANYONE }, \
    { "net-vpn-enable", net_act_vpn_enable, A_FORK },

#endif
