/* rules.h — per-app network rules: the machine's owner decides which of their apps use the network
 * (like the "network access" permission on other systems). See docs/FIREWALL.md, "Rules".
 *
 * This part is only the rule store and the decider: it doesn't touch the kernel or the network.
 * It's used later by the daemon (with the cgroup hooks), the Monitor ("Rule" column) and Settings
 * ("Trusted apps" and "Restricted apps" lists).
 *
 * Decision order (fw_decide), from strongest to weakest:
 *   1. the app's restrictions (FW_R_*): always deny;
 *   2. explicit rules: the one with the most specific destination wins; with the same specificity,
 *      deny beats allow;
 *      2b. a subscribed blocklist (FwQuery.listed) denies, unless the winning rule names the
 *          destination (a domain, an address or a network): lists beat lan/internet/any rules,
 *          never a rule the owner wrote for that destination;
 *   3. trusted app: allow without logging or asking;
 *   4. mode: silent = allow and log, ask = pending, off = allow, deny = deny and log (loopback
 *      stays allowed).
 *
 * Lookups: hash table of apps; per app, a hash table of exact domains, a suffix table for
 * wildcards (the name's labels are walked from right to left), a binary prefix trie per family
 * for addresses and networks. Everything is O(1) or O(key length). */
#ifndef LORD_FW_RULES_H
#define LORD_FW_RULES_H

#include <stddef.h>
#include <stdint.h>

#define FW_APP_MAX   128        /* bytes of an app id, with the trailing 0 */
#define FW_NAME_MAX  254        /* domain name as text (253 + 0) */
#define FW_MAX_RULES 65536      /* cap on rules per store (also when loading) */
#define FW_MAX_APPS  4096       /* cap on apps with state */
#define FW_FILE_MAX  (4u << 20) /* cap on the JSON file */

typedef enum { FW_OUT = 0, FW_IN = 1 } FwDir;
typedef enum { FW_PROTO_ANY = 0, FW_PROTO_TCP = 1, FW_PROTO_UDP = 2 } FwProto;
typedef enum { FW_ALLOW = 0, FW_DENY = 1 } FwAction;
/* forever (saved) / until the app closes / just once (not saved) */
typedef enum { FW_LIFE_FOREVER = 0, FW_LIFE_UNTIL_QUIT = 1, FW_LIFE_ONCE = 2 } FwLife;
/* deny (0.8.5, "deny silently"): what no rule allows is denied and recorded; loopback stays allowed */
typedef enum { FW_MODE_SILENT = 0, FW_MODE_ASK = 1, FW_MODE_OFF = 2, FW_MODE_DENY = 3 } FwMode;

typedef enum {
    FW_DST_ANY = 0,
    FW_DST_DOMAIN,      /* exact, or "*.suffix" (wildcard = 1): the suffix and everything below it */
    FW_DST_CIDR,        /* an address is a /32 or /128 */
    FW_DST_LAN,         /* see fw_addr_is_lan */
    FW_DST_INTERNET,    /* everything that isn't LAN */
} FwDstKind;

/* Restrictions of an app in the "Restricted apps" list. The network ones are applied by fw_decide
 * (and, in the kernel, by lord-netperm). The others are checked with fw_blocks, through
 * sec/restrict.h, where the resource is handed out (docs/FIREWALL.md, "Non-network
 * restrictions"): the broker for isolated apps, js_media.c and sdk/notify.js for built-in apps. */
enum {
    FW_R_NO_INTERNET      = 1u << 0,
    FW_R_NO_LAN           = 1u << 1,
    FW_R_NO_NETWORK       = 1u << 2,
    FW_R_NO_INCOMING      = 1u << 3,
    FW_R_NO_CAMERA        = 1u << 4,
    FW_R_NO_MIC           = 1u << 5,
    FW_R_NO_SCREEN        = 1u << 6,
    FW_R_NO_USB           = 1u << 7,
    FW_R_NO_BLUETOOTH     = 1u << 8,
    FW_R_NO_FILES         = 1u << 9,    /* no user files (isolated apps: the file picker is refused) */
    FW_R_NO_NOTIFICATIONS = 1u << 10,
    FW_R_NO_CLIPBOARD     = 1u << 11,   /* no reading the clipboard */
    FW_R_ALL              = (1u << 12) - 1,
};

typedef struct {
    uint32_t id;                    /* set by the store (fw_rule_add); 0 = unassigned */
    char     app[FW_APP_MAX];       /* canonical id (see fw_app_canon) */
    FwDir    dir;
    FwDstKind kind;
    int      wildcard;              /* FW_DST_DOMAIN: 1 if it was "*.suffix" */
    char     domain[FW_NAME_MAX];   /* FW_DST_DOMAIN: lowercase, without "*." or trailing dot */
    int      fam;                   /* FW_DST_CIDR: 4 or 6 */
    uint8_t  addr[16];              /* FW_DST_CIDR: network (bits outside the prefix set to 0) */
    int      prefix;                /* FW_DST_CIDR: 0..32 or 0..128 */
    uint16_t port_lo, port_hi;      /* range; 0..65535 (or 0..0) = any port */
    FwProto  proto;
    FwAction action;
    FwLife   life;
    long long when;                 /* Unix seconds of the decision (informational) */
} FwRule;

/* A connection to decide. */
typedef struct {
    const char *app;                /* any form accepted by fw_app_canon */
    FwDir    dir;
    FwProto  proto;                 /* FW_PROTO_TCP or FW_PROTO_UDP */
    int      fam;                   /* 4, 6, or 0 if the address isn't known */
    uint8_t  addr[16];              /* remote address (IPv4 in the first 4 bytes) */
    uint16_t port;                  /* remote port (outbound) or local port (inbound) */
    const char *const *hosts;       /* names of that address for this app (dnsmap_lookup) */
    int      nhosts;
    int      listed;                /* 1: a blocklist the user subscribed to covers this destination
                                     * (fw/fwlists.h); denies unless a rule naming the destination
                                     * (a domain, an address or a network) decides first */
} FwQuery;

typedef enum { FW_V_ALLOW = 0, FW_V_DENY = 1, FW_V_ASK = 2 } FwVerdict;
typedef enum { FW_BY_RESTRICTED, FW_BY_RULE, FW_BY_TRUSTED, FW_BY_MODE, FW_BY_LIST } FwReason;

typedef struct {
    FwVerdict verdict;
    FwReason  reason;
    uint32_t  rule_id;              /* FW_BY_RULE: the rule that decided (a "once" rule was already deleted) */
    uint32_t  flag;                 /* FW_BY_RESTRICTED: the restriction that denied */
    int       record;               /* 1: log it so the owner can review it (silent mode) */
} FwDecision;

typedef struct FwStore FwStore;

FwStore *fw_store_new(void);
void     fw_store_free(FwStore *s);

/* Canonical id: "lordos", "desktop:<id>", "lpk:<id>" or an absolute executable path. cgroup
 * scopes are translated: app-lordos-lpk-<id>.scope → lpk:<id>, app-lordos-<id>.scope →
 * desktop:<id>. 0 ok, -1 if it isn't a valid id (empty, too long, odd characters). */
int fw_app_canon(const char *in, char *out, size_t n);

/* (0.8.5) LordOS's own components that use the network, each in its own scope
 * lordos-net-<name>.scope so the kernel enforces their rules (docs/MONITOR.md, "Native apps as
 * their own rows"): their firewall id is "lordos:<name>". 1 if name is one of them. */
#define FW_COMPONENTS { "mail", "store", "print", "terminal", "lists", "lan", "machines", NULL }
int fw_component_ok(const char *name);
/* "lordos-net-<name>.scope" of a known component → "lordos:<name>" in app. 0 ok, -1 otherwise. */
int fw_component_scope(const char *scope, char *app, size_t n);

/* Destination as text ("any", "lan", "internet", "example.com", "*.example.com", "10.0.0.0/8",
 * "2001:db8::1") → kind/wildcard/domain/fam/addr/prefix of r. 0 ok, -1 invalid. */
int fw_parse_dest(const char *s, FwRule *r);
/* Normalized domain name into out (FW_NAME_MAX): lowercase, no trailing dot, labels of
 * 1 to 63 from [a-z0-9-_], up to 253; the last one can't be only digits. 0 ok, -1 invalid. */
int fw_norm_domain(const char *in, char *out);
/* The inverse of fw_parse_dest, for saving and display. */
int fw_format_dest(const FwRule *r, char *out, size_t n);

/* LAN: 0/8, 10/8, 100.64/10 (CGNAT), 127/8, 169.254/16, 172.16/12, 192.168/16, 224/4, 255.255.255.255;
 * ::, ::1, fc00::/7, fe80::/10, ff00::/8; ::ffff:a.b.c.d as its IPv4. */
int fw_addr_is_lan(int fam, const uint8_t *addr);
/* 127/8 and ::1 (and ::ffff:127.x.y.z): what the deny mode never denies. */
int fw_addr_is_loopback(int fam, const uint8_t *addr);

/* Adds a rule (validates and copies it). If there's already one with the same app/dir/destination/
 * ports/protocol, it replaces it and keeps its id. *id_out (optional) receives the id. 0 ok, -1
 * invalid or no room. */
int  fw_rule_add(FwStore *s, const FwRule *r, uint32_t *id_out);
int  fw_rule_remove(FwStore *s, uint32_t id);
const FwRule *fw_rule_get(const FwStore *s, uint32_t id);   /* NULL if it doesn't exist */
/* Copies the rules (of one app, or all with app NULL) in id order; returns the total. */
int  fw_rule_list(const FwStore *s, const char *app, FwRule *out, int max);
size_t fw_rule_count(const FwStore *s);

void fw_app_quit(FwStore *s, const char *app);              /* deletes its "until closed" rules */
void fw_app_forget(FwStore *s, const char *app);            /* everything about the app (uninstall) */

/* Lists. An app is in only one: trusting it removes it from restricted and vice versa. */
int      fw_set_trusted(FwStore *s, const char *app, int on);
int      fw_is_trusted(const FwStore *s, const char *app);
int      fw_set_restricted(FwStore *s, const char *app, uint32_t flags);  /* 0 = remove it */
uint32_t fw_restricted(const FwStore *s, const char *app);
int      fw_blocks(const FwStore *s, const char *app, uint32_t flag);     /* restricted in flag? */
/* Walks the apps with state; cb returns != 0 to stop. */
void     fw_app_each(const FwStore *s, int (*cb)(void *u, const char *app, int trusted, uint32_t flags), void *u);

void   fw_set_mode(FwStore *s, FwMode m);
FwMode fw_mode(const FwStore *s);

/* Decides a connection. If a "once" rule wins, deletes it. */
FwDecision fw_decide(FwStore *s, const FwQuery *q);

/* Names of the restrictions in the JSON ("no-internet"...). */
const char *fw_flag_name(uint32_t flag);
uint32_t    fw_flag_from_name(const char *name);            /* 0 if it doesn't exist */

/* Compact JSON (only the "forever" rules). fw_to_json returns malloc'd memory. */
char *fw_to_json(const FwStore *s, size_t *len);
/* Replaces the contents of s with those of the JSON. If anything fails validation, s stays the same and err says what. */
int   fw_from_json(FwStore *s, const char *text, size_t len, char *err, size_t errlen);
/* Per-user file (~/.config/lordos/firewall.json): atomic write with mode 0600. */
int   fw_save(const FwStore *s, const char *path);
int   fw_load(FwStore *s, const char *path, char *err, size_t errlen);

#endif
