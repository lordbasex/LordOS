/* dndgate.h — who may drop files on LordOS (wl.c, the dnd_* code; see dndgate.c). */
#ifndef LORD_DNDGATE_H
#define LORD_DNDGATE_H

#define DNDG_READ_MS 5000               /* how long a claimed drop's list may take to arrive */

/* 1 if rootfd (a directory: a client's /proc/<pid>/root) is a Flatpak sandbox's root: .flatpak-info is
 * there (any kind of entry; a symlink is not followed). Also 1 if rootfd < 0: what can't be looked at counts
 * as sandboxed. */
int dndg_root_sandboxed(int rootfd);

/* 0 only on positive evidence that the process is not sandboxed: its /proc/<pid>/root has no .flatpak-info
 * and is our own "/", and its mount, pid and user namespaces are ours. 1 for anything else, and if it can't be
 * looked at (pid <= 0, gone, not ours). Its drags and clipboard file lists never hand files to LordOS. */
int dndg_pid_sandboxed(int pid);
/* The same, for a process's /proc/<pid> directory already open (procfd): what it says is that process's,
 * even if its pid is someone else's by then. 1 if procfd < 0. */
int dndg_proc_sandboxed(int procfd);

/* 1 if a drag's type mime is a list of files (text/uri-list, and the file managers' and the portals' lists): left
 * out of a contained drag's offer to a target that would open those files and hand them on (wl.c: lord-web gives a
 * page the files of a drop). */
int dndg_file_mime(const char *mime);

/* A Linux app's drop on LordOS, and who may take it: only what runs while the UI handles the release that
 * made the drop. pending: a drop is waiting; claimable: that release is being handled; claimed: someone took it. */
typedef struct { int pending, claimable, claimed; } DropGate;
void dndg_drop(DropGate *g);            /* the release dropped on LordOS: claimable until dndg_release_done */
int  dndg_claim(DropGate *g);           /* 1 once, and only while claimable */
int  dndg_release_done(DropGate *g);    /* the release went to the UI: 1 if its drop was not claimed (cancel it) */
void dndg_reset(DropGate *g);           /* the drop ended (read, cancelled, or its source gone) */

#endif
