/* nplocal.h — this computer's own networks (Whonix groups, docs/FIREWALL.md "The Tor gateway"), for np_local: an
 * RTM_GETADDR and an RTM_GETROUTE dump over rtnetlink (NETLINK_ROUTE, no AF_INET socket). Each interface address as a
 * host and with its on-link prefix (/8 or longer for IPv4, /16 or longer for IPv6), and each directly connected route
 * (unicast or local, no gateway, /8 or /16 or longer: never a default route or a VPN's 0/1 and 128/1). lord-netperm
 * keeps them in np_local; the Tor gateway's built-in restriction treats them as LAN. */
#ifndef LORD_NP_LOCAL_H
#define LORD_NP_LOCAL_H

#include <stddef.h>

#include "netperm/np_shared.h"

/* Up to max networks into out (masked trie keys, each once); how many, or -1 when a dump failed. *full: 1 when some
 * didn't fit (the caller then fails closed). */
int np_host_addrs(NpLocalKey *out, int max, int *full);
/* The parser of one netlink buffer (RTM_NEWADDR, RTM_NEWROUTE), for tests: adds to out[*n]; 1 at NLMSG_DONE, -1 at an
 * error message, 0 to read on. */
int np_local_parse(const void *buf, size_t len, NpLocalKey *out, int max, int *n, int *full);

#endif
