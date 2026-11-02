/* fetch.h — `lordos-fetch`: the crown and a short summary of this computer for a terminal, from the
 * same collector as System Information (sysreport.h), plus a few live facts (uptime, memory and disk
 * in use, packages, shell). The user and host names appear only on screen and only without --share,
 * so a screenshot made for sharing carries neither. */
#ifndef LORD_SYSREPORT_FETCH_H
#define LORD_SYSREPORT_FETCH_H

#include "sysreport.h"

typedef enum { FETCH_PLAIN, FETCH_256, FETCH_TRUECOLOR } FetchColor;

typedef struct {
    char user[64], host[64];            /* empty with --share */
    long uptime_s;                      /* -1 unknown */
    unsigned long long mem_used_kb;     /* MemTotal − MemAvailable */
    unsigned long long disk_used, disk_total;   /* the root file system, bytes */
    int packages;                       /* Debian packages installed, -1 unknown */
    char shell[32];                     /* the login shell's name */
    char session[24];                   /* "Wayland", "X11", "" */
} FetchLive;

/* The live facts from <root> (proc, dpkg status) and this process (user, shell, disk). share: no
 * user or host name. */
void fetch_live(FetchLive *l, const char *root, int share);

/* The whole output (malloc'd, NULL on out-of-memory): the crown beside the summary. */
char *fetch_render(const SysReport *r, const FetchLive *l, FetchColor color);

/* How to color stdout: --color=always|never|auto (auto: a terminal, no NO_COLOR, TERM not "dumb";
 * true color when COLORTERM says so). */
FetchColor fetch_color_mode(const char *opt, int is_tty);

#endif
