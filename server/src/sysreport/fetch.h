/* fetch.h — `lordos-fetch`: the crown and a short summary of this computer for a terminal, from the
 * same collector as System Information (sysreport.h), plus a few live facts (uptime, memory and disk
 * in use, packages, shell). The user and host names appear only on screen and only without --share,
 * so a screenshot made for sharing carries neither. */
#ifndef LORD_SYSREPORT_FETCH_H
#define LORD_SYSREPORT_FETCH_H

#include "sysreport.h"

#include <stddef.h>

typedef enum { FETCH_PLAIN, FETCH_256, FETCH_TRUECOLOR } FetchColor;

typedef struct {
    char user[64], host[64];            /* empty with --share */
    long uptime_s;                      /* -1 unknown */
    unsigned long long mem_used_kb;     /* in use, without cache or the kernel's reserve (meminfo.h) */
    unsigned long long swap_total_kb, swap_used_kb;
    int swap_known;                     /* /proc/meminfo had SwapTotal */
    unsigned long long disk_used, disk_total;   /* the root file system, bytes */
    int packages;                       /* Debian packages installed, -1 unknown */
    char shell[32];                     /* the login shell's name */
    char session[24];                   /* "Wayland", "X11", "" */
    char desktop[64];                   /* the LordOS desktop in use (settings.json "theme" → its theme.json "name") */
    char terminal[64];                  /* "LordOS Terminal", $TERM_PROGRAM, "SSH", "Linux console" or the parent's name */
    char xwayland[32];                  /* Xwayland's version when it runs for this user ("" not running) */
    char ip[192];                       /* the default route's interface and its addresses; empty with --share */
    char locale[96];                    /* LANG and the LordOS interface language */
    double boot_s;                      /* this boot, kernel start to the first frame (0 unknown), and */
    char boot_shown[16];                /*   what it showed: "login" or "session" */
    unsigned long long server_rss_kb;   /* this user's lord-server (the session, not an --app one); 0 unknown */
    /* set by the caller (cli.c, from the build): */
    char server[64];                    /* "lord-server 0.8.9.4" */
    char engine[64];                    /* "QuickJS 2025-04-26 + React 19.3.1" */
    char build[128];                    /* "20261122.adae1e89 · development build" */
} FetchLive;

/* The live facts from <root> (proc, dpkg status, boot times) and this process (user, shell, disk, terminal,
 * settings, addresses: only for the real system, root ""). share: no user or host name, no address. */
void fetch_live(FetchLive *l, const char *root, int share);

/* ---- parsing helpers (pure; server/tests/sysreport_test.c) ---- */

/* SwapTotal and SwapFree from /proc/meminfo's text. 0 when SwapTotal is there. */
int fetch_swap(const char *meminfo, unsigned long long *total_kb, unsigned long long *used_kb);
/* settings.json's "theme" (a safe id; "default" when null or missing) and "language" ("" when null). 0 if parsed. */
int fetch_settings(const char *json, char *theme, size_t tn, char *lang, size_t ln);
/* theme.json's "name". 0 if found. */
int fetch_theme_name(const char *json, char *out, size_t n);
/* A package's upstream version from dpkg's status file (no epoch, no Debian revision): "2:24.1.6-1" → "24.1.6".
 * Only an installed one. 0 if found. */
int fetch_dpkg_version(const char *status, const char *pkg, char *out, size_t n);
/* The interface of the default route with the lowest metric in /proc/net/route. 0 if found. */
int fetch_route_iface(const char *route, char *out, size_t n);
/* The first global IPv6 address of iface in /proc/net/if_inet6, as text with its prefix. 0 if found. */
int fetch_inet6(const char *if_inet6, const char *iface, char *out, size_t n);
/* This boot's line in /var/lib/lordos/boot-times.jsonl (the last one with bootId == boot_id): seconds and "shown". */
int fetch_boot_time(const char *jsonl, const char *boot_id, double *secs, char *shown, size_t sn);
/* VmRSS from /proc/<pid>/status, in kB (0 unknown). */
unsigned long long fetch_status_rss(const char *status);
/* The terminal's name from the environment and the ancestors' names (nearest first; shells, su, sudo and the
 * like are skipped). tty: ttyname(0) or NULL. */
void fetch_terminal_pick(const char *term_program, const char *term_version, int ssh, const char *tty,
                         const char *const *ancestors, int nanc, char *out, size_t n);

/* The whole output (malloc'd, NULL on out-of-memory): the crown beside the summary. */
char *fetch_render(const SysReport *r, const FetchLive *l, FetchColor color);

/* How to color stdout: --color=always|never|auto (auto: a terminal, no NO_COLOR, TERM not "dumb";
 * true color when COLORTERM says so). */
FetchColor fetch_color_mode(const char *opt, int is_tty);

#endif
