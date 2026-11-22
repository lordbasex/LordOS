/* bellgate.h — how often an app may ring the bell (xdg-system-bell-v1, sysbell.c). Pure, for
 * tests/bellgate_test.c. A terminal printing a file full of BEL characters, or an app ringing in a loop, must
 * neither flood the speakers nor the shell: per client at most one ring every BELL_GAP_MS and BELL_BURST in
 * BELL_WINDOW_MS; across all clients at most one every BELL_ANY_MS. A ring held back is dropped, not delayed. */
#ifndef LORD_BELLGATE_H
#define LORD_BELLGATE_H

#include <stdint.h>
#include <string.h>

#define BELL_GAP_MS 250u
#define BELL_ANY_MS 100u
#define BELL_BURST 5
#define BELL_WINDOW_MS 10000u
#define BELL_CLIENTS 16

typedef struct { const void *who; uint32_t last, start; int n; } BellSlot;
typedef struct { BellSlot s[BELL_CLIENTS]; uint32_t any; int rang; } BellGate;

/* 1 if who's ring at now_ms rings. who is the client (any pointer; NULL for none). */
static inline int bell_pass(BellGate *g, const void *who, uint32_t now) {
    if (g->rang && now - g->any < BELL_ANY_MS) return 0;
    BellSlot *slot = NULL, *old = &g->s[0];
    for (int i = 0; i < BELL_CLIENTS; i++) {
        BellSlot *b = &g->s[i];
        if (b->who == who && b->n) { slot = b; break; }
        if (!b->n || now - b->last > now - old->last) old = b;   /* a free slot, else the longest quiet */
    }
    if (!slot) {
        slot = old;
        memset(slot, 0, sizeof *slot);
        slot->who = who;
    } else {
        if (now - slot->last < BELL_GAP_MS) return 0;
        if (now - slot->start >= BELL_WINDOW_MS) { slot->start = now; slot->n = 0; }
        if (slot->n >= BELL_BURST) return 0;
    }
    if (!slot->n) slot->start = now;
    slot->n++;
    slot->last = now;
    g->any = now;
    g->rang = 1;
    return 1;
}

#endif
