/* webargs.h — how lord-web may be started (docs/WEBAPPS.md, "Starting an app" and "Adding an app from an address"),
 * and nothing else:
 *
 *   lord-web --app <app> --profile <profile>    one app and profile, both ids checked again against the registry
 *   lord-web --probe                            one address on fd 3; answers the manifest on fd 3
 *   lord-web --probe-icon                       one icon address on fd 3; answers its bytes on fd 3
 *
 * Pure (tests/webapp_test.c). */
#ifndef LORD_WEB_WEBARGS_H
#define LORD_WEB_WEBARGS_H

#include "webreg.h"

#include <stddef.h>

enum { WEBARGS_BAD, WEBARGS_APP, WEBARGS_PROBE, WEBARGS_PROBE_ICON };

typedef struct {
    int mode;
    char app[WEBREG_ID_MAX + 1];
    char profile[WEBREG_ID_MAX + 1];
} WebArgs;

/* The mode of argv (WEBARGS_BAD for anything but the three forms above, in that exact order). */
int webargs_parse(int argc, char *const *argv, WebArgs *out);

#define WEBARGS_LINE_MAX 2048
/* The request line of a probe: one address of at most WEBARGS_LINE_MAX bytes ending in "\n" and nothing after it,
 * https (or http on this computer). 0 and url filled, or -1. */
int webargs_probe_line(const char *buf, size_t len, char *url, size_t n);

#endif
