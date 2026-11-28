/* vmdispconn.h — lord-vm's bound on a machine's display connections (VM_DISPLAY, docs/VIRTUALIZATION.md 6.1).
 *
 * Each VM_DISPLAY hands the client one end of a fresh socketpair, QEMU the other (getfd + add_client). lord-vm used to
 * allow 16 per run of the machine in all, never given back, so after 16 reconnects ("Display again", All Displays) the
 * machine's window couldn't connect again until it restarted (QA of 0.8.9.6.2). The bound is now on the connections
 * alive at once, plus a rate: at most VM_DISP_RATE_N new ones in any VM_DISP_RATE_MS, so a hostile or looping client
 * still can't make QEMU take connection after connection.
 *
 * A connection is alive while the client's end exists: lord-vm keeps that socket's inode (fstat before it is sent) and,
 * when the bound is reached, looks for it in its own network namespace's /proc/net/unix (opened before lord-vm's own
 * Landlock, read again from the start each time): a socket the client closed is gone from it. When the list can't be
 * read, nothing is given back (the old bound). Pure functions here; tests/vm_test.c checks them. */
#ifndef LORDOS_VMDISPCONN_H
#define LORDOS_VMDISPCONN_H

#include <stddef.h>

#define VM_DISP_LIVE_MAX 16
#define VM_DISP_RATE_N 8
#define VM_DISP_RATE_MS 10000LL

typedef struct {
    unsigned long ino[VM_DISP_LIVE_MAX];       /* the client's end of each live connection */
    int n;
    long long at[VM_DISP_RATE_N];              /* when the last VM_DISP_RATE_N were handed out (a ring) */
    int at_i;
} VmDispConns;

/* may one more be handed out now: under the rate (the oldest of the last VM_DISP_RATE_N is older than the window) */
static inline int vm_disp_rate_ok(const VmDispConns *d, long long now) {
    long long oldest = d->at[d->at_i];
    return oldest == 0 || now - oldest >= VM_DISP_RATE_MS;
}

/* one handed out: its inode kept, the time noted. 0 fine, -1 already VM_DISP_LIVE_MAX alive */
static inline int vm_disp_add(VmDispConns *d, unsigned long ino, long long now) {
    if (d->n >= VM_DISP_LIVE_MAX) return -1;
    d->ino[d->n++] = ino;
    d->at[d->at_i] = now ? now : 1;
    d->at_i = (d->at_i + 1) % VM_DISP_RATE_N;
    return 0;
}

/* the connections whose inode isn't in /proc/net/unix's text any more are gone: dropped. The text is the whole file
 * (a header line, then one socket per line, its inode the 7th field). Returns how many were dropped; text NULL drops
 * none. */
static inline int vm_disp_prune(VmDispConns *d, const char *text, size_t n) {
    if (!text || !n) return 0;
    unsigned char seen[VM_DISP_LIVE_MAX] = { 0 };
    size_t i = 0;
    while (i < n && text[i] != '\n') i++;            /* the header */
    while (i < n) {
        i++;
        size_t s = i;
        while (i < n && text[i] != '\n') i++;
        /* the 7th field: skip six fields and their spaces */
        size_t k = s;
        int f = 0;
        while (k < i && f < 6) {
            while (k < i && text[k] == ' ') k++;
            while (k < i && text[k] != ' ') k++;
            f++;
        }
        while (k < i && text[k] == ' ') k++;
        if (f < 6 || k >= i || text[k] < '0' || text[k] > '9') continue;
        unsigned long ino = 0;
        int digits = 0;
        while (k < i && text[k] >= '0' && text[k] <= '9' && digits < 20) { ino = ino * 10 + (unsigned long)(text[k++] - '0'); digits++; }
        if (k < i && text[k] != ' ') continue;
        for (int j = 0; j < d->n; j++) if (d->ino[j] == ino) seen[j] = 1;
    }
    int w = 0, dropped = 0;
    for (int j = 0; j < d->n; j++) {
        if (seen[j]) d->ino[w++] = d->ino[j];
        else dropped++;
    }
    d->n = w;
    return dropped;
}

#endif
