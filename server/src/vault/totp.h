/* totp.h — one-time codes (2FA): HOTP (RFC 4226) and TOTP (RFC 6238) with SHA-1, SHA-256 and
 * SHA-512, base32 (RFC 4648) and otpauth:// URIs (the Google Authenticator format carried by
 * the QR codes of websites). The secret never leaves axia-vault: the UI receives only the code. */
#ifndef LORD_VAULT_TOTP_H
#define LORD_VAULT_TOTP_H

#include <stddef.h>
#include <stdint.h>

enum { TOTP_SHA1 = 0, TOTP_SHA256, TOTP_SHA512 };

typedef struct {
    uint8_t key[128]; size_t keylen;
    int algo;                 /* TOTP_* */
    int digits;               /* 6–10 */
    int period;               /* seconds (30) */
    int hotp;                 /* 1 = otpauth://hotp (counter) */
    uint64_t counter;
    char issuer[128], account[128];
} Totp;

/* base32 → bytes (ignores spaces, dashes and '='; upper or lower case). Length or -1. */
long base32_decode(const char *s, uint8_t *out, size_t cap);
size_t base32_encode(const uint8_t *in, size_t n, char *out, size_t cap);   /* without '=' */

/* Reads an otpauth://totp/… or a bare base32 secret. 0 on success. */
int  totp_parse(const char *text, Totp *t);
uint32_t hotp_code(const Totp *t, uint64_t counter);
/* Code for unix time `now`; *remaining = seconds left in the period. */
uint32_t totp_code(const Totp *t, uint64_t now, int *remaining);
/* Code with leading zeros: out of at least 11 bytes. */
void totp_format(const Totp *t, uint32_t code, char *out);
void totp_wipe(Totp *t);

#endif
