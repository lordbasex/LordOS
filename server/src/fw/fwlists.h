/* fwlists.h — subscribed blocklists (0.8.5, docs/FIREWALL.md, "Blocklists").
 *
 * Three pieces, all pure C with no network and no files of their own:
 *
 *   1. The raw parsers (fwl_parse_raw): what a list's server sent (a hosts file, one domain per
 *      line, or one address / network per line) → normalized lines. They run in lord-fetch, the
 *      sandboxed helper, on untrusted bytes. Strict: a NUL byte, a line longer than FWL_LINE_MAX,
 *      or too many lines that don't parse reject the whole list; LAN networks, networks broader
 *      than /8 (IPv4) or /16 (IPv6), names without a dot and the usual hosts-file names
 *      (localhost…) are skipped and counted.
 *
 *   2. The set (FwLists): exact names, names with every subdomain, and networks, each with a tag
 *      (which list it came from, for the Monitor's "blocked by the list …"). Names are kept as
 *      64-bit hashes (seeded per process) in sorted arrays: about 10 bytes a name. Name lookups walk
 *      the labels right to left; address lookups try each prefix length present (binary search).
 *
 *   3. The compiled file, ~/.config/lordos/fwlists.txt: written by lord-server (js_fw.c, after
 *      validating every line again) and read by lord-netperm (root) through the same fixed-path,
 *      no-links reader as firewall.json, with fwl_load_text:
 *
 *        lordos-fwlists 1
 *        d tracker.example        an exact name
 *        s ads.example            the name and every name below it
 *        n 192.0.2.0/24           an address or a network (bits outside the prefix are 0)
 *
 *      Any line that doesn't validate rejects the whole file (the previous set stays in use).
 *
 * Lists never override a rule the owner wrote for that destination (rules.h, FwQuery.listed). */
#ifndef LORD_FW_FWLISTS_H
#define LORD_FW_FWLISTS_H

#include <stddef.h>
#include <stdint.h>

#define FWL_RAW_MAX      (16u << 20)    /* the most a list's server may send */
#define FWL_LINE_MAX     1024           /* longer lines reject the list */
#define FWL_LIST_DOMAINS 400000         /* names in one list */
#define FWL_LIST_NETS    100000         /* networks in one list */
#define FWL_MAX_DOMAINS  600000         /* in the compiled set, every list together */
#define FWL_MAX_NETS     150000
#define FWL_FILE_MAX     (32u << 20)    /* the compiled file */
#define FWL_HEADER       "lordos-fwlists 1"

typedef enum { FWL_FMT_HOSTS = 1, FWL_FMT_DOMAINS = 2, FWL_FMT_IPS = 3 } FwlFormat;

typedef struct {
    size_t lines;               /* lines read (without comments and blank lines) */
    size_t domains, nets;       /* entries written */
    size_t skipped;             /* valid lines we don't use (LAN, too broad, localhost, no dot) */
    size_t bad;                 /* lines that didn't parse */
    size_t dups;                /* entries repeated inside the list */
    int    truncated;           /* 1: the list had more than FWL_LIST_DOMAINS / FWL_LIST_NETS */
} FwlStats;

/* Parses a raw list in format fmt; sub = 1 makes every name cover its subdomains ("s" lines).
 * Writes the normalized lines into *out (malloc'd, NUL-terminated, *outlen bytes). 0 ok; -1 the
 * whole list is rejected (err: a short code: "nul", "line", "format", "empty", "big", "oom"). */
int fwl_parse_raw(const char *raw, size_t len, FwlFormat fmt, int sub, char **out, size_t *outlen,
                  FwlStats *st, char *err, size_t errlen);
FwlFormat fwl_format_from_name(const char *name);  /* "hosts" / "domains" / "ips"; 0 if unknown */

typedef struct FwLists FwLists;

FwLists *fwl_new(void);
void     fwl_free(FwLists *l);
/* Adds one normalized line ("d name", "s name", "n cidr") with a tag (1..65535). 0 added (a
 * repeated entry goes at the next lookup or fwl_finish, the first tag stays), -1 invalid line,
 * -2 the set is full. */
int      fwl_add_line(FwLists *l, const char *line, size_t len, uint16_t tag);
/* Only checks a line, as fwl_add_line would (lord-server compiles the file without keeping it). */
int      fwl_check_line(const char *line, size_t len);
/* Sorts and drops repeated entries; returns how many went. Lookups and counts do it on their own. */
size_t   fwl_finish(FwLists *l);
/* Adds every line of a normalized text (the lines fwl_parse_raw writes). Returns the number of
 * lines added, or -1 if a line is invalid (the lines before it stay). */
long     fwl_add_text(FwLists *l, const char *text, size_t len, uint16_t tag);
/* The compiled file: header + lines. Replaces nothing on error: a fresh set is returned, or NULL
 * with err set. */
FwLists *fwl_load_text(const char *text, size_t len, char *err, size_t errlen);

size_t   fwl_domains(const FwLists *l);
size_t   fwl_nets(const FwLists *l);
/* The tag of the list that covers this name (normalized or not), or 0. */
uint16_t fwl_match_name(const FwLists *l, const char *name);
/* The tag of the list whose network contains the address (fam 4: 4 bytes, 6: 16), or 0. */
uint16_t fwl_match_addr(const FwLists *l, int fam, const uint8_t *addr);
/* Walks the networks (fam, network, prefix); cb returns != 0 to stop. */
void     fwl_net_each(const FwLists *l, int (*cb)(void *u, int fam, const uint8_t *addr, int prefix), void *u);

/* Should the resolver's addresses for these names be blocked? Only when at least one name is
 * listed and none is unlisted: an address a listed tracker shares with a site that isn't listed
 * (a shared CDN) stays allowed. Returns the first listed name's tag, or 0. */
uint16_t fwl_block_names(const FwLists *l, const char *const *names, int n);

#endif
