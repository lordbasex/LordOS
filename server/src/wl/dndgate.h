/* dndgate.h — who may drop files on LordOS (wl.c, the dnd_* code; see dndgate.c). */
#ifndef LORD_DNDGATE_H
#define LORD_DNDGATE_H

#define DNDG_READ_MS 5000               /* how long a claimed drop's list may take to arrive */

/* 1 if rootfd (a directory: a client's /proc/<pid>/root) is a Flatpak sandbox's root: .flatpak-info is
 * there (any kind of entry; a symlink is not followed). Also 1 if rootfd < 0: what can't be looked at counts
 * as sandboxed. */
int dndg_root_sandboxed(int rootfd);

/* 1 if the process runs in a sandbox (dndg_root_sandboxed of its /proc/<pid>/root), or if it can't be looked
 * at (pid <= 0, gone, not ours). Its drags never hand files to LordOS. */
int dndg_pid_sandboxed(int pid);

/* A Linux app's drop on LordOS, and who may take it: only what runs while the UI handles the release that
 * made the drop. pending: a drop is waiting; claimable: that release is being handled; claimed: someone took it. */
typedef struct { int pending, claimable, claimed; } DropGate;
void dndg_drop(DropGate *g);            /* the release dropped on LordOS: claimable until dndg_release_done */
int  dndg_claim(DropGate *g);           /* 1 once, and only while claimable */
int  dndg_release_done(DropGate *g);    /* the release went to the UI: 1 if its drop was not claimed (cancel it) */
void dndg_reset(DropGate *g);           /* the drop ended (read, cancelled, or its source gone) */

#endif
