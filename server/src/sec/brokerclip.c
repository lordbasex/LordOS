/* brokerclip.c — an isolated app's own copy on the clipboard (brokerclip.h). One copy at a time: the
 * last app that copied. The text is never kept here, only the generation it got. */
#include "brokerclip.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static struct {
    char app[64];
    unsigned gen;           /* the clipboard's generation right after the copy */
    double deadline;        /* a secret's clearing time (0: none, or cancelled) */
    int cleared;            /* cleared on time; cleared_gen is the empty clipboard's generation */
    unsigned cleared_gen;
} C;

void bclip_copied(const char *app, unsigned gen, int secret, int secs, double now) {
    snprintf(C.app, sizeof C.app, "%s", app ? app : "");
    C.gen = gen;
    C.deadline = secret && secs > 0 && secs <= 600 ? now + secs : 0;
    C.cleared = 0;
    C.cleared_gen = 0;
}

int bclip_due(unsigned gen, double now) {
    if (!C.deadline || now < C.deadline) return 0;
    C.deadline = 0;
    return gen == C.gen;                    /* replaced meanwhile: nothing to clear */
}

void bclip_cleared(unsigned gen) { C.cleared = 1; C.cleared_gen = gen; }

int bclip_status(const char *app, unsigned gen, double now, int *left) {
    if (left) *left = 0;
    if (!app || !C.app[0] || strcmp(app, C.app)) return BCLIP_NONE;
    if (gen == C.gen) {
        if (left && C.deadline > now) *left = (int)ceil(C.deadline - now);
        return BCLIP_MINE;
    }
    return C.cleared && gen == C.cleared_gen ? BCLIP_CLEARED : BCLIP_REPLACED;
}

int bclip_keep(const char *app, unsigned gen) {
    if (bclip_status(app, gen, 0, NULL) != BCLIP_MINE) return 0;
    C.deadline = 0;
    return 1;
}
