/* ssh_root.h — remote login as root over SSH, an advanced option off by default (see ssh_root.c and
 * docs/REMOTE.md, "Root over SSH (advanced)"). */
#ifndef LORD_SSH_ROOT_H
#define LORD_SSH_ROOT_H

#include "../admin.h"
#include <stddef.h>
#include <stdint.h>

void ssh_act_root(const AdminReq *r, const char *arg, size_t len);
void ssh_act_root_key(const AdminReq *r, const char *arg, size_t len);
void ssh_act_root_password(const AdminReq *r, const char *arg, size_t len);
void ssh_act_root_status(const AdminReq *r, const char *arg, size_t len);

/* An OpenSSH public key line "<type> <base64> [comment]" (no options, one line): 0 and its parts, the
 * decoded blob (which must start with the same type), or -1. Exposed for tests. */
#define SSHKEY_BLOB_MAX 2048
typedef struct {
    char type[48];
    uint8_t blob[SSHKEY_BLOB_MAX];
    size_t bloblen;
    char b64[(SSHKEY_BLOB_MAX + 2) / 3 * 4 + 1];
    char comment[101];
} SshKey;
int sshkey_parse(const char *line, SshKey *k);
/* "SHA256:<base64 without padding>" of the blob, as ssh-keygen -l prints it */
void sshkey_fingerprint(const SshKey *k, char out[60]);
/* The drop-in text for a mode: "off" → NULL (no file), "key", "password"; -1 for anything else. */
int ssh_root_text(const char *mode, const char **text);

#define SSH_ROOT_ACTIONS \
    { "ssh-root", ssh_act_root, A_FORK }, \
    { "ssh-root-key", ssh_act_root_key, A_FORK }, \
    { "ssh-root-password", ssh_act_root_password, A_FORK }, \
    { "ssh-root-status", ssh_act_root_status, A_FORK },

#endif
