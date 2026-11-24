/* nplocal.h — this computer's own interface addresses (Whonix groups, docs/FIREWALL.md "The Tor gateway"): an
 * RTM_GETADDR dump over rtnetlink (NETLINK_ROUTE, no AF_INET socket), each address (IFA_LOCAL, IFA_ADDRESS) once.
 * lord-netperm keeps them in np_local; the Tor gateway's built-in restriction treats them as LAN. */
#ifndef LORD_NP_LOCAL_H
#define LORD_NP_LOCAL_H

#include "netperm/np_shared.h"

/* Up to max addresses into out; how many, or -1 (errno) when the dump failed. */
int np_host_addrs(NpLocalKey *out, int max);

#endif
