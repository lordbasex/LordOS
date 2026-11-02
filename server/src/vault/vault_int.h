/* vault_int.h — what the parts of axia-vault share (vault.c: process, channels and requests;
 * keyring.c: the system keyring and Secret Service). */
#ifndef LORD_VAULT_INT_H
#define LORD_VAULT_INT_H

#include <stddef.h>
#include <stdint.h>
#include "vfile.h"
#include "vmodel.h"
#include "vjson.h"
#include "vwire.h"

enum { V_MAIN = 0, V_LOGIN = 1, V_COUNT };
enum { ROLE_SYSTEM = 1, ROLE_APP = 2 };

/* attachment added and not yet saved (encrypted, in memory) */
typedef struct { uint8_t id[16], nonce[24], mac[16]; uint8_t *cipher; size_t len; } PendAtt;

typedef struct {
    const char *file;          /* name in the vaults folder */
    int unlocked;
    VHeader h;                 /* slots (also when locked, if the file exists) */
    int exists;
    uint8_t *vk;               /* 32 bytes in smem; NULL when locked */
    VModel m;
    int fd;                    /* current file open (source of the attachments) or -1 */
    PendAtt *pend; int npend;
    double last_use;
    int autolock_s;            /* 0 = never (the session keyring) */
} Vault;

typedef struct { int fd, role; VwRx rx; } Chan;

extern Vault vaults[V_COUNT];
extern int vault_dirfd;

/* replies */
void reply(Chan *c, uint32_t id, VjB *b);             /* frees b */
void reply_err(Chan *c, uint32_t id, const char *code);
void reply_ok(Chan *c, uint32_t id);
void broadcast_event(const char *json);               /* to all channels */

/* vaults */
int  vault_save(Vault *v);                            /* VF_OK or error */
void vault_lock(Vault *v);
int  vault_unlock_with(Vault *v, int type, const VFactors *f, char *err, size_t errn);
int  vault_create(Vault *v, int type, const VFactors *f, const VKdf *kdf, const char *label, const char *meta_json, char *recovery_out);

/* keyring (keyring.c) */
int  keyring_handle(Chan *c, uint32_t id, const char *op, const Vj *j);   /* 1 if it handled the op */
void keyring_session_password(const char *pass);      /* on login: open or create login.lvault */
void keyring_start_bus(int busfd);
int  keyring_bus_fd(void);
int  keyring_bus_wants_out(void);
void keyring_bus_dispatch(void);
void keyring_lock_changed(void);
void keyring_prompt_result(unsigned prompt, int ok);

#endif
