/* ifwatch.c — every interface gets its saved state once, whenever it appears.
 *
 * Boot used to be a single pass over the interfaces that existed at that moment. A radio whose driver
 * finishes loading its firmware later than lord-netd starts (issue #8) was not in that list, nothing
 * looked again, and it stayed down until the person switched it on. A radio udev had yet to rename was
 * configured under a name its saved config doesn't have.
 *
 * The table remembers each device by index, so an interface is brought up once: what the person does
 * afterwards (net-down) is never undone by a later notification. No system calls here: actions.c
 * supplies them, and tests/net_test.c fakes them. */
#include "ifwatch.h"

#include <stdio.h>
#include <string.h>

static IfSeen *find(IfTable *t, int index) {
    for (int i = 0; i < t->n; i++) if (t->v[i].index == index) return &t->v[i];
    return NULL;
}

static int present(const IfNow *now, int n, int index) {
    for (int i = 0; i < n; i++) if (now[i].index == index) return 1;
    return 0;
}

int ifwatch_step(IfTable *t, const IfNow *now, int n, const IfOps *o, long ms, int booting, int *applied) {
    for (int i = 0; i < t->n;) {
        if (present(now, n, t->v[i].index)) { i++; continue; }
        o->stop(t->v[i].name);
        t->v[i] = t->v[--t->n];
    }
    int pending = 0;
    for (int i = 0; i < n; i++) {
        IfSeen *e = find(t, now[i].index);
        if (!e) {
            if (t->n == IFW_MAX) continue;
            e = &t->v[t->n++];
            memset(e, 0, sizeof *e);
            e->index = now[i].index;
            e->boot = booting;
            e->first_ms = ms;
            snprintf(e->name, sizeof e->name, "%s", now[i].name);
        } else if (strcmp(e->name, now[i].name)) {
            if (e->done) { o->stop(e->name); e->done = e->fails = e->boot = 0; e->first_ms = ms; e->next_ms = 0; }
            snprintf(e->name, sizeof e->name, "%s", now[i].name);
        }
        if (e->done) continue;
        if (!o->candidate(e->name)) { e->done = 1; continue; }
        long age = ms - e->first_ms;
        if (age < IFW_WAIT_MS &&
            (!o->initialized(e->index) || (!e->boot && age < IFW_GRACE_MS) || o->busy(e->name))) { pending++; continue; }
        if (ms < e->next_ms) { pending++; continue; }
        if (o->managed(e->name)) { e->done = 1; continue; }
        if (!o->apply(e->name)) { e->done = 1; if (applied) ++*applied; continue; }
        if (++e->fails >= IFW_TRIES) { e->done = 1; continue; }
        e->next_ms = ms + IFW_RETRY_MS;
        pending++;
    }
    return pending;
}
