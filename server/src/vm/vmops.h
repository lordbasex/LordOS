/* vmops.h — the intent of a multi-step operation on a machine (docs/VIRTUALIZATION.md 3.6, "Interrupted
 * operations"). Before a suspend, a resume, a snapshot (take, revert, delete), a compaction or a linked clone changes
 * anything, lord-vm writes <machine>/operation.json naming the work; the new files are made beside the old ones and
 * replace them by rename; operation.json is removed last. At the next open under the machine's lock, lord-vm reads
 * it and finishes the work or rolls it back (lordvm.c, ops_recover), and says which in the log and as a notice.
 *
 *   {"op":"snapshot-take","tag":"s20261105-120000","a":"","b":"","started":"2026-11-05T12:00:00Z"}
 *
 * Only lord-vm writes it, in the machine's folder (QEMU can't: it may write only in disks/). */
#ifndef LORD_VM_OPS_H
#define LORD_VM_OPS_H

#include "machine.h"

typedef struct {
    char op[24];                /* suspend, resume, snapshot-take, snapshot-revert, snapshot-delete, compact, link */
    char tag[64];               /* the snapshot's tag, when there is one */
    char a[VM_PATH_MAX];        /* op-specific: a path (the shared base of a linked clone, a temporary file) */
    char b[VM_PATH_MAX];
    char started[21];
} VmOp;

/* writes operation.json atomically (temporary file, fsync, rename, fsync of the folder). 0 ok */
int vm_op_begin(const char *dir, const char *op, const char *tag, const char *a, const char *b);
/* reads it: 1 there is one (o filled), 0 none, -1 unreadable (it's removed: an intent that can't be read was never
 * acted on, since the work starts only after the rename) */
int vm_op_read(const char *dir, VmOp *o);
/* reads it without touching it (an unreadable one stays for lord-vm): 1 there is one */
int vm_op_read_peek(const char *dir, VmOp *o);
/* removes it (and syncs the folder) */
void vm_op_end(const char *dir);

/* a snapshot's tag: s + UTC time, unique within the machine: "s20261105-120000"; letters, digits and '-' only */
void vm_snap_tag(char out[24]);
int vm_snap_tag_valid(const char *s);

/* fsync of a folder, after a rename in it */
void vm_sync_dir(const char *dir);

/* <machine>/snapshots.json (6.2): the snapshots that are complete, with their names. Written by lord-vm only, last in
 * each operation, so an entry is proof that every disk has the tag. Pictures: <machine>/snapshots/<tag>.png (lord-server).
 *   {"snapshots":[{"tag":"s20261105-120000","name":"Before the upgrade","created":"2026-11-05T12:00:00Z",
 *                  "memory":true,"vmstate":301989888}]} */
#define VM_SNAPS_MAX  64           /* refused beyond */
#define VM_SNAPS_WARN 20           /* internal snapshots slow down with many: the app warns from here (6.2) */
typedef struct {
    char tag[33];
    char name[VM_NAME_MAX + 1];
    char created[21];
    int memory;                    /* taken while running: memory and devices too, it resumes where it was */
    double vmstate;                /* bytes of that memory */
} VmSnap;
int vm_snaps_load(const char *dir, VmSnap *s, int max);           /* how many (0 none or unreadable) */
int vm_snaps_save(const char *dir, const VmSnap *s, int n);       /* atomically; 0 ok */
int vm_snaps_find(const VmSnap *s, int n, const char *tag);       /* index or -1 */
/* A snapshot's note (6.2): <machine>/snapshots/<tag>.note, the person's own words, written by lord-server; removed with
 * the snapshot (lord-vm). UTF-8 without control characters but the line break, at most VM_SNAP_NOTE_MAX bytes; empty
 * is no note */
#define VM_SNAP_NOTE_MAX 1000
int vm_snap_note_valid(const char *s);

/* A Clone's own identity inside the guest (6.3): <machine>/identity.json, {"hostname":"debian-clone"}, written by
 * lord-vm --clone (unless the identity is kept) and removed once the guest agent gave the guest that hostname, a new
 * machine-id and new SSH host keys at the clone's first start. */
#define VM_HOSTNAME_MAX 63
int vm_hostname_valid(const char *s);                               /* one DNS label: a-z 0-9 '-', not at the ends */
void vm_hostname_from_name(const char *name, char *out, size_t n); /* "Debian 13" → "debian-13" ("machine" if empty) */
/* a Windows computer name (NetBIOS rules): 1 to 15 letters, digits and '-', not at the start, not only digits */
#define VM_WINNAME_MAX 15
int vm_winname_valid(const char *s);
/* a clone's hostname (one DNS label) as a Windows computer name: as it is when it fits, else its first 10 characters,
 * '-' and 4 hex digits of the machine's id (so two long names that start alike stay apart); 0, -1 if none can be made */
int vm_winname_from_host(const char *host, const char *id, char out[VM_WINNAME_MAX + 1]);
int vm_identity_write(const char *dir, const char *hostname);       /* 0 ok */
int vm_identity_read(const char *dir, char *hostname, size_t n);    /* 0 there is one, -1 none or unreadable */
void vm_identity_done(const char *dir);

#endif
