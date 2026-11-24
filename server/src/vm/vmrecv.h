/* vmrecv.h — the machine's quarantine folder for files sent from the guest (docs/VIRTUALIZATION.md 6.21, "Send to
 * LordOS"). lord-server (ui/js_vm.c) uses it, lordvm-dev plays lord-server with it, vm-test tests it.
 *
 * Every file from a guest lands in <library>/Received/<machine folder's name without .lordvm>, made here (0700, each step
 * opened without following a link, this account's own, not writable by others), never anywhere the guest names. lord-vm
 * gets only the descriptor of a part file, ".lordos-part-<the guest's resume key in hex>" (the same file offered again
 * after a lost connection finds what came before), opened O_RDWR without following a link and kept only as a regular file
 * of this account with one link. Once lord-vm says the SHA-256 matched, the part is made 0600 (never executable), marked
 * as from that machine (user.lordos.quarantine "1", user.lordos.from its name, user.lordos.machine its id,
 * user.xdg.origin.url "lordos-machine:<id>"; Files shows them) and named (ag_recv_name's name, given to the checked file itself by its descriptor, never replacing a file:
 * "name (2).ext"…). Parts nobody came back for go after VMRECV_PART_DAYS, and at most VMRECV_PARTS_MAX stay. */
#ifndef LORD_VM_RECV_H
#define LORD_VM_RECV_H

#include <stddef.h>
#include <stdint.h>

#define VMRECV_RESERVE   (1ULL << 30)       /* an offer must leave this much free on the folder's file system */
#define VMRECV_PARTS_MAX 8
#define VMRECV_PART_DAYS 7
#define VMRECV_PART_NAME 80                 /* ".lordos-part-" + 64 hex + NUL fits */

/* the quarantine folder of the machine at machine_dir (an absolute path ending ".lordvm"), made when missing: its
 * descriptor (O_DIRECTORY, close-on-exec), -1 when it can't be one of this account's own folders. shown: its path */
int vmrecv_dir_open(const char *machine_dir, char *shown, size_t sn);
/* parts older than VMRECV_PART_DAYS go, then the oldest until fewer than VMRECV_PARTS_MAX remain; never `keep` */
void vmrecv_parts_trim(int dfd, const char *keep);
/* the part file for this key: its descriptor (O_RDWR), the part's name, and *have, the bytes already there (a part longer than
 * size can't be this file's start: it is emptied). -1 with *status: 2 not now (it isn't a plain file of ours), 3 no room
 * (size - have + VMRECV_RESERVE isn't free; a new empty part is removed again). Status values are vmagent.h's AGSA_* */
int vmrecv_part_open(int dfd, const uint8_t key[32], uint64_t size, char part[VMRECV_PART_NAME], uint64_t *have, int *status);
/* the part is whole and checked: 0600, marked, named. 0 (final gets the name, *marked 1 when the file system took the
 * marks), -1 not (the part is removed) */
int vmrecv_finish(int dfd, int fd, const char *part, const char *name, const char *machine_name, const char *machine_id,
                  uint64_t size, char *final, size_t fn, int *marked);

#endif
