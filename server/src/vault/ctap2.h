/* ctap2.h — our own FIDO2 client (CTAP 2.0/2.1 over USB HID) to unlock the vault with a
 * security key (see docs/PASSWORDS.md, "FIDO2 security key").
 *
 * Uses the `hmac-secret` extension: the key stores a per-credential secret (CredRandom) and, given
 * a 32-byte "salt", returns HMAC-SHA256(CredRandom, salt). That result is the key that
 * wraps the vault key: without the physical key (and its touch) the vault cannot be opened.
 *
 * Runs in axia-vault, sandboxed: it receives the fd of /dev/hidrawN already opened by axia-server
 * (after the user's permission) and only does read/write of 64-byte reports, no ioctl.
 * Protocol: CTAPHID (INIT with nonce, own channel, CBOR, KEEPALIVE, CANCEL, ERROR) +
 * authenticatorGetInfo, authenticatorClientPIN (getKeyAgreement and getPinToken, PIN protocol 1:
 * ECDH P-256 + SHA-256 + AES-256-CBC), authenticatorMakeCredential and authenticatorGetAssertion
 * with hmac-secret. The key agreement uses our own P-256 (p256.h). */
#ifndef LORD_VAULT_CTAP2_H
#define LORD_VAULT_CTAP2_H

#include <stddef.h>
#include <stdint.h>

#define FIDO_RP_ID "contrasenas.lordos"      /* rp.id of the vault credentials */
#define FIDO_CRED_MAX 255

enum {
    FIDO_OK = 0,
    FIDO_ERR_IO = -1,          /* the device does not respond or was unplugged */
    FIDO_ERR_TIMEOUT = -2,     /* nobody touched the key in time */
    FIDO_ERR_PROTOCOL = -3,    /* invalid response */
    FIDO_ERR_NO_HMAC = -4,     /* the key has no hmac-secret (U2F only, or very old) */
    FIDO_ERR_PIN_REQUIRED = -5,/* the key has a PIN and none was given */
    FIDO_ERR_PIN_INVALID = -6,
    FIDO_ERR_PIN_BLOCKED = -7,
    FIDO_ERR_NO_CREDENTIAL = -8,/* the credential is not from this key (another key) */
    FIDO_ERR_CANCELLED = -9,
    FIDO_ERR_DENIED = -10,     /* the key refused (CTAP2_ERR_OPERATION_DENIED…) */
};

typedef struct {
    int hmac_secret;           /* advertises the extension */
    int client_pin;            /* -1 no PIN support, 0 supported but not set, 1 has a PIN */
    int uv;                    /* built-in user verification (fingerprint) */
    uint8_t aaguid[16];
    char versions[64];         /* "FIDO_2_0,FIDO_2_1,U2F_V2" */
    int pin_retries;           /* -1 if unknown */
} FidoInfo;

/* Notices while waiting: FIDO_ST_TOUCH = the key asks to be touched (KEEPALIVE UPNEEDED). */
enum { FIDO_ST_PROCESSING = 1, FIDO_ST_TOUCH = 2 };
typedef void (*FidoStatusCb)(void *ud, int status);

/* All: hidraw fd open for reading and writing; timeout_ms to wait for the touch. */
int  fido_get_info(int fd, FidoInfo *info);
/* Creates a new credential with hmac-secret for the vault (asks for a touch). cred receives the
 * credential id (up to FIDO_CRED_MAX bytes). pin: NULL or the key's PIN (UTF-8). */
int  fido_make_credential(int fd, const char *pin, uint8_t *cred, size_t *cred_len,
                          int timeout_ms, FidoStatusCb cb, void *ud);
/* hmac-secret of that credential with salt (asks for a touch): out = HMAC-SHA256(CredRandom, salt). */
int  fido_hmac_secret(int fd, const char *pin, const uint8_t *cred, size_t cred_len,
                      const uint8_t salt[32], uint8_t out[32], int timeout_ms, FidoStatusCb cb, void *ud);
/* Aborts a wait in progress (from another thread): sends CTAPHID_CANCEL. */
void fido_cancel(int fd);
const char *fido_strerror(int err);    /* short code in English: "timeout", "pin-required"… */

/* ---- additions ----
 * Usage notes:
 *   - cred must have room for FIDO_CRED_MAX bytes (*cred_len is output only).
 *   - hmac-secret with PIN (pin != NULL → pinAuth, UV) and without PIN (touch only) may give
 *     DIFFERENT results on CTAP 2.1 keys (CredRandomWithUV / WithoutUV): the vault must
 *     store in the slot whether a PIN was used and repeat the same when unlocking.
 *   - With a PIN set on the key, makeCredential requires the PIN (FIDO_ERR_PIN_REQUIRED). */

/* Sets a new PIN on a key that has none (clientPIN setPIN, protocol 1; 4–63 bytes). */
int  fido_set_pin(int fd, const char *pin);
/* Raw transport: CTAPHID_INIT and a command (cmd with the 0x80 bit) with its response. For
 * tests and vendor commands. Returns 0 and *resp_len, or a FIDO_ERR_*. */
int  ctaphid_command(int fd, uint8_t cmd, const uint8_t *data, size_t len,
                     uint8_t *resp, size_t cap, size_t *resp_len, int timeout_ms);

#endif
