/* vmlink.h — a machine group's internal link, lord-server's side (docs/VIRTUALIZATION.md 19.3; approved by the maintainer
 * on 2026-11-23, AGENTS.md "Machine groups: the internal link").
 *
 * One socketpair(AF_UNIX, SOCK_STREAM) per pairing, made here, one end to each member's lord-vm with VM_LINK (through each
 * one's runtime socket, $XDG_RUNTIME_DIR/lordos-vm/<id>.sock, which checks the peer's uid). The pair is anonymous: no
 * path, no port, nothing another process can connect to. Neither end is kept here: after both lord-vms answered, the
 * only holders are the two QEMUs. Which machines are paired comes from the group's own group.json and its members'
 * machine.json (vm_link_group), never from JS.
 *
 * Used by ui/js_vm.c (lord.vm.link) and by tests/vm_link_tool.c (the headless tests). */
#ifndef LORD_VM_LINK_H
#define LORD_VM_LINK_H

#include <stddef.h>

/* lord-vm's check of a VM_LINK descriptor: a connected AF_UNIX SOCK_STREAM socket (not listening) whose peer is a process
 * of this account (SO_PEERCRED); a pipe, a file, a datagram socket, an internet socket or a listening one is refused.
 * 1 yes, else 0 and why (English) */
int vm_link_fd_ok(int fd, char *why, size_t wn);

/* the runtime socket's folder ($XDG_RUNTIME_DIR, else /run/user/<uid>) into out */
void vm_link_runtime(char *out, size_t n);

/* sends VM_LINK to the machines a_id and b_id (their ids) for the group `group` (its id): on 1, one end of a new pair to
 * each; on 0, both cables pulled. Waits at most timeout_ms for both answers. 0 when both took it, else -1 and why (English)
 * in err. Either may have taken it when the other refused: the other side of a pair nobody holds is a pulled cable. */
int vm_link_pair(const char *rundir, const char *a_id, const char *b_id, const char *group, int on, int timeout_ms, char *err, size_t errlen);

/* the group folder gdir (vm_group_dir_ok, read with vm_group_load): its gateway's and workstation's machine ids, checked
 * against their machine.json (the same group id, the roles group.json names, the link network); then vm_link_pair. */
int vm_link_group(const char *gdir, int on, int timeout_ms, char *err, size_t errlen);

#endif
