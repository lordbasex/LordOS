/* npdns.h — where axia-netperm learns the names of addresses from (for domain rules). Only
 * from responses produced by the system resolver (docs/FIREWALL.md, step 2,
 * "Where the names come from"):
 *
 *   1. systemd-resolved: its varlink monitoring interface
 *      (/run/systemd/resolve/io.systemd.Resolve.Monitor, SubscribeQueryResults; root only). Every
 *      answer resolved gave to any program (NSS, the 127.0.0.53 stub, D-Bus) arrives, with the
 *      records in wire format ("raw").
 *   2. Without resolved: the UDP responses that arrive FROM port 53 of the servers in
 *      /etc/resolv.conf (a root file, written by LordOS: docs/NETWORK.md), read with a receive-only
 *      AF_PACKET socket (classic BPF filter: UDP from port 53). What an app asks another server
 *      isn't learned.
 *
 * Both sources deliver a complete DNS response message to cb, which passes it to dnsmap_learn. */
#ifndef LORD_NP_DNS_H
#define LORD_NP_DNS_H

#include <stddef.h>
#include <stdint.h>

typedef void (*NpDnsCb)(void *u, const uint8_t *pkt, size_t len);
typedef struct NpDns NpDns;

NpDns      *npdns_new(NpDnsCb cb, void *u);
void        npdns_free(NpDns *d);
int         npdns_fd(const NpDns *d);           /* the fd to wait on (-1 if none) */
void        npdns_ready(NpDns *d);              /* the fd has data */
void        npdns_tick(NpDns *d);               /* every second: reconnect, pick a source */
const char *npdns_source(const NpDns *d);       /* "resolved", "packet" or "none" */

/* A response from resolved's monitor (varlink JSON, without the trailing 0) → one DNS message per
 * question, to cb. Returns how many it built, 0 if it wasn't a successful response, -1 if invalid.
 * Kept separate to test it without resolved (tests/npcompile_test.c). */
int npdns_from_varlink(const char *json, size_t len, NpDnsCb cb, void *u);

/* A network packet (from the IP header, as AF_PACKET/SOCK_DGRAM delivers it) → the DNS message if
 * it's UDP from port 53 of one of the given addresses (servers: fam 4/6 + 16 bytes). */
typedef struct { int fam; uint8_t addr[16]; } NpAddr;
int npdns_from_packet(const uint8_t *p, size_t len, const NpAddr *servers, int nservers, const uint8_t **dns, size_t *dnslen);

#endif
