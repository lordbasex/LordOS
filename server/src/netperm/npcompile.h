/* npcompile.h — from the rule store (fw/rules.c) to the entries of lord-netperm's BPF maps
 * (np_shared.h). It's pure C, no kernel: tests/npcompile_test.c tests it against
 * fw_decide. See docs/FIREWALL.md, "Step 2", "From rules to maps".
 *
 * Per app it produces:
 *   - one NpApp: restrictions, trusted, mode and the generic list (lan / internet / any);
 *   - nodes for the LPM tries: one per network in a rule and one per address that the system
 *     resolver associated with a name matching a domain rule. Each node's list also carries the
 *     rules of the networks that contain it, so the trie's longest match is enough: in the kernel
 *     the first entry matching address, protocol and port wins, in the same order of precedence
 *     as fw_decide.
 *
 * "Just once" rules aren't compiled (there's nobody to delete them in the kernel; without ask
 * mode they don't appear). "Ask" mode is compiled as silent, "deny" (0.8.5) as NP_M_DENY.
 * The blocklist set (NpApp.blk) is lord-netperm's: it sets it after compiling (fw/fwlists.h). */
#ifndef LORD_NP_COMPILE_H
#define LORD_NP_COMPILE_H

#include <stddef.h>
#include <stdint.h>

#include "fw/rules.h"
#include "fw/dnsmap.h"
#include "netperm/np_shared.h"

typedef struct {
    int fam;                    /* 4 or 6 */
    int prefix;                 /* 0..32 or 0..128 */
    uint8_t addr[16];
    int host;                   /* 1: comes from a DNS response (domain rules) */
    NpList list;
} NpNode;

typedef struct {
    NpApp app;
    NpNode *nodes;
    int nnodes, cap;
    int truncated;              /* rules that didn't fit (NP_LIST_MAX / NP_GEN_MAX); reported */
    int has_domains;            /* has domain rules: must be recompiled on every DNS response */
} NpCompiled;

/* Compiles the app. names (may be NULL) are the names learned from the system resolver,
 * stored in dnsmap under the app names_app. 0 ok, -1 out of memory. */
int  np_compile_app(FwStore *s, const char *app, uint32_t idx, DnsMap *names, const char *names_app,
                    long long now, NpCompiled *out);
void np_compiled_free(NpCompiled *c);

/* Only the node of one address with its names (a new DNS response, without recompiling everything).
 * 1 if a node is needed (left in *out), 0 if not, -1 error. */
int  np_compile_host(FwStore *s, const char *app, int fam, const uint8_t *addr,
                     const char (*names)[FW_NAME_MAX], int n, NpNode *out, int *truncated);

/* Trie keys for a node. */
void np_key4(uint32_t idx, const NpNode *n, NpLpm4Key *k);
void np_key6(uint32_t idx, const NpNode *n, NpLpm6Key *k);

/* Protocol of a rule → NP_P_*; does the rule's network contain the address/prefix? */
int  np_cidr_contains(const FwRule *r, int fam, const uint8_t *addr, int prefix);

#endif
