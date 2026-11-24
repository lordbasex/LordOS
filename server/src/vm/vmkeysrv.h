/* vmkeysrv.h — lord-server's side of an encrypted machine's key (docs/VIRTUALIZATION.md 8, "Encrypted machines").
 *
 * The key is 32 random bytes as 64 lowercase hex digits (vm_key_valid), made here, kept in the session keyring by
 * lord-vault (owner "machines", service "machines:<id>": the machine's id, or a group's id for its members) and read
 * back from it by lord-server's own C code on a worker thread (vc_request_wait): it never reaches JS. lord-vm gets it on
 * a pipe (--key-fd). Here it lives for VMKEY_TTL_S in one locked page that never goes to a core dump, then is wiped.
 * The one way in from JS is vmkey_enter: the recovery key the person types to bring an exported machine in (a protected
 * window), checked and kept like the others. */
#ifndef LORD_VM_VMKEYSRV_H
#define LORD_VM_VMKEYSRV_H

#define VMKEY_TTL_S 120
enum { VMK_NONE = 0, VMK_PENDING, VMK_READY, VMK_LOCKED, VMK_MISSING, VMK_ERROR };
extern const char *const VMK_NAMES[];   /* "none", "pending", "ready", "locked", "missing", "error" */

/* the key of id (a machine's or a group's): ready now, or asked of the keyring (pending; the answer comes later) */
int vmkey_fetch(const char *id);
/* a new key for a machine or group about to be made under id, kept in the keyring first (label: "Machines: <name>") */
int vmkey_new(const char *id, const char *name);
/* the same key as from's, kept under to as well (a Clone's) */
int vmkey_copy(const char *from, const char *to, const char *name);
/* a recovery key typed by the person (spaces, dashes and case ignored) kept under id; -1 when it isn't one */
int vmkey_enter(const char *id, const char *typed, const char *name);
/* the machine was deleted for good: its key goes from the keyring */
void vmkey_forget(const char *id);
/* VMK_* of id */
int vmkey_state(const char *id);
/* a pipe's read end (O_CLOEXEC) holding id's key when it's ready, else -1 */
int vmkey_pipe(const char *id);
/* wipe every key held here (the session locks) */
void vmkey_wipe_all(void);

#endif
