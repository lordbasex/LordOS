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
/* removes it (and syncs the folder) */
void vm_op_end(const char *dir);

/* a snapshot's tag: s + UTC time, unique within the machine: "s20261105-120000"; letters, digits and '-' only */
void vm_snap_tag(char out[24]);
int vm_snap_tag_valid(const char *s);

/* fsync of a folder, after a rename in it */
void vm_sync_dir(const char *dir);

#endif
