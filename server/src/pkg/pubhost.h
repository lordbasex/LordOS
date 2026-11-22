/* pubhost.h — what a download from an address a person typed may reach (Machines' "Added by you" downloads,
 * vm-image-get-user; AGENTS.md "Machines' downloads of your own systems"): a public DNS name and public addresses only.
 * Pure functions, no network; tested by tests/vmurl_test.c.
 *
 * A public DNS name: 2 to 127 labels of 1 to 63 letters, digits and '-' (not first or last), at most 253 bytes, any
 * case, no trailing dot; the last label letters only (2 to 63) or an IDN "xn--" label, so no IPv4 or IPv6 literal ever
 * passes; never localhost or a name under a suffix that only means something on a LAN or in a test (local, localhost,
 * localdomain, internal, intranet, lan, home, corp, private, arpa (home.arpa, in-addr.arpa), test, invalid, example,
 * onion, alt).
 *
 * A public address: none of IPv4's this-network, private (10/8, 172.16/12, 192.168/16), shared (100.64/10), loopback,
 * link-local, IETF protocol (192.0.0/24), documentation, benchmarking (198.18/15), multicast and reserved (224/3)
 * blocks; none of IPv6's unspecified, loopback, discard (100::/64), documentation (2001:db8::/32), unique local
 * (fc00::/7), link-local (fe80::/10), site-local (fec0::/10) and multicast (ff00::/8); an IPv4-mapped, NAT64
 * (64:ff9b::/96) or 6to4 (2002::/16) address is judged by the IPv4 address in it. */
#ifndef LORD_PKG_PUBHOST_H
#define LORD_PKG_PUBHOST_H

#include <stdint.h>
#include <sys/socket.h>

int pubhost_name_ok(const char *host);
int pubhost_ipv4_ok(uint32_t addr_host_order);
int pubhost_ipv6_ok(const uint8_t a[16]);
/* AF_INET or AF_INET6; anything else is not public */
int pubhost_addr_ok(const struct sockaddr *sa, socklen_t len);

#endif
