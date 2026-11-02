/* pin.h — apt's preferences (/etc/apt/preferences and /etc/apt/preferences.d), so the package engine
 * picks the same candidate versions as apt (docs/PACKAGES.md, "Priorities").
 *
 * Read the way apt reads them: /etc/apt/preferences first, then the files of preferences.d in C
 * sort order whose names are only [A-Za-z0-9_.-] and have no extension or ".pref". A stanza has
 * `Package:` (glob patterns, /regex/ or src:name, separated by spaces; exactly "*" makes it a
 * general pin), `Pin:` (`origin <host>`, `release a=,n=,c=,o=,l=` or `version <glob>`) and
 * `Pin-Priority:`; `Explanation:` and comments are ignored, a malformed stanza is skipped.
 *
 * A version's priority, as in apt: the first specific pin that names its package decides, for the
 * versions it matches; otherwise the first general pin matching its source; otherwise the source's
 * own priority (500, 100 for backports, 1 for experimental). Nothing here is written or executed;
 * the files are world-readable configuration owned by root. */
#ifndef LORD_PKG_PIN_H
#define LORD_PKG_PIN_H

#include "idx.h"

/* Prefix for /etc/apt (tests point it at a fake tree; "" for the real system). */
extern const char *pin_root;

typedef struct PinSet PinSet;

PinSet *pin_load(void);                                   /* NULL: no preferences (or no memory) */
void pin_free(PinSet *p);
/* A cheap fingerprint of the preference files (names, sizes, dates), to notice a change. */
unsigned long pin_stamp(void);
/* Priority of index record `rec` (a version from one source). */
int pin_priority(const PinSet *p, const Idx *x, long rec);

#endif
