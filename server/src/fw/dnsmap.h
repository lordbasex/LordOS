/* dnsmap.h — which names belong to an IP, per app, learned from the DNS responses the system
 * resolver already received (see docs/FIREWALL.md, "Names for an address").
 *
 * A connection carries only an IP; domain rules ("*.example.com") need the name.
 * When the app resolved www.example.com → CNAME cdn.example.net → A 192.0.2.7, this keeps
 * (app, 192.0.2.7) → { www.example.com, cdn.example.net } until the TTL expires, and fw_decide
 * receives those names in FwQuery.hosts. Nobody is queried: neither reverse DNS nor services.
 *
 * It is a bounded LRU (max_entries app+IP+name pairs in total, DNSMAP_PER_IP names per IP):
 * when the table is full, what was used longest ago is forgotten. */
#ifndef LORD_FW_DNSMAP_H
#define LORD_FW_DNSMAP_H

#include <stddef.h>
#include <stdint.h>

#include "fw/rules.h"

#define DNS_MAX_ADDRS   64          /* A/AAAA records taken from one response */
#define DNS_MAX_CNAMES  16          /* CNAMEs of one response (and maximum chain length) */
#define DNSMAP_PER_IP   8           /* names per (app, IP) */
#define DNSMAP_TTL_MIN  60          /* TTLs are clamped to [60 s, 1 day] */
#define DNSMAP_TTL_MAX  86400

typedef struct {
    int fam;                        /* 4 or 6 */
    uint8_t addr[16];
    uint32_t ttl;
    char owner[FW_NAME_MAX];
} DnsAddrRec;

typedef struct {
    char from[FW_NAME_MAX], to[FW_NAME_MAX];
    uint32_t ttl;
} DnsCname;

typedef struct {
    char qname[FW_NAME_MAX];        /* the question, normalized */
    uint16_t qtype;
    int rcode;
    int naddr, ncname;
    DnsAddrRec addr[DNS_MAX_ADDRS];
    DnsCname cname[DNS_MAX_CNAMES];
} DnsResponse;

/* Reads a DNS response (the message, without TCP's 2-byte length). Only the answer section,
 * class IN, types A, AAAA and CNAME; ignores authority and additional. Every access is checked:
 * labels up to 63, names up to 255 bytes, compression pointers only backwards from the span being
 * read (no loops) and at most 64 jumps per name.
 *   0  ok (it may carry nothing useful: rcode != 0, names that aren't host names, another class);
 *  -1  malformed, truncated, or not a standard response with a single question. */
int dns_parse_response(const uint8_t *pkt, size_t len, DnsResponse *out);

typedef struct DnsMap DnsMap;

DnsMap *dnsmap_new(size_t max_entries);     /* 0 = 4096 */
void    dnsmap_free(DnsMap *m);

/* Times (now) are seconds of a monotonic clock supplied by the caller. */
int dnsmap_add(DnsMap *m, const char *app, const char *name, int fam, const uint8_t *addr, uint32_t ttl, long long now);
/* Reads the response and associates each address with the question and all the names of the
 * CNAME chain that leads to it (records that don't come from the question are ignored). Returns
 * how many associations it added, or -1 if the packet is invalid. */
int dnsmap_learn(DnsMap *m, const char *app, const uint8_t *pkt, size_t len, long long now);
/* Current names of (app, IP), from most recent to oldest, copied into out. Returns how many. */
int dnsmap_lookup(DnsMap *m, const char *app, int fam, const uint8_t *addr, long long now, char (*out)[FW_NAME_MAX], int max);

/* Walks an app's addresses with their current names (without changing the LRU); cb returns
 * != 0 to stop. Returns how many addresses it walked. axia-netperm uses it to compile the
 * domain rules into BPF maps (docs/FIREWALL.md, step 2). */
int dnsmap_each(DnsMap *m, const char *app, long long now,
                int (*cb)(void *u, int fam, const uint8_t *addr, const char (*names)[FW_NAME_MAX], int n), void *u);

size_t dnsmap_count(const DnsMap *m);
void   dnsmap_expire(DnsMap *m, long long now);         /* deletes what expired */
void   dnsmap_forget_app(DnsMap *m, const char *app);

#endif
