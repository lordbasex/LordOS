/* smbauth.h — what listing a server's shares after signing in needs (docs/FILES.md, "Network"): NTLMv2
 * and SMB 2 signing, in lord-lan only. Pure functions over bytes, tested with the protocol's own vectors. */
#ifndef LORD_LAN_SMBAUTH_H
#define LORD_LAN_SMBAUTH_H

#include <stddef.h>
#include <stdint.h>

void md4(const void *data, size_t len, uint8_t out[16]);
void md5(const void *data, size_t len, uint8_t out[16]);
void hmac_md5(const void *key, size_t klen, const void *data, size_t len, uint8_t out[16]);
void hmac_sha256(const void *key, size_t klen, const void *data, size_t len, uint8_t out[32]);

/* UTF-8 → UTF-16LE: bytes written, 0 if it doesn't fit or isn't valid. upper: as a Windows account name. */
size_t text_utf16(const char *s, uint8_t *out, size_t cap, int upper);

/* NTOWFv2 and the response. proof: 16 bytes; base: the session base key; temp (tlen bytes): what follows
 * the proof in the NT response. 0 ok, -1 too long. */
int ntlmv2_hash(const char *user, const char *domain, const char *password, uint8_t out[16]);
size_t ntlmv2_temp(uint8_t *temp, size_t cap, uint64_t filetime, const uint8_t client[8], const uint8_t *av, size_t avlen);
void ntlmv2_proof(const uint8_t hash[16], const uint8_t server[8], const uint8_t *temp, size_t tlen, uint8_t proof[16], uint8_t base[16]);

size_t ntlm_user_negotiate(uint8_t *o, size_t cap);
/* The AUTHENTICATE message for a CHALLENGE message. rnd: 24 random bytes (client challenge + exported key).
 * key: the key that signs from here on. Returns its length, 0 for a challenge that can't be read. */
size_t ntlm_user_auth(uint8_t *o, size_t cap, const uint8_t *challenge, size_t clen, const char *user, const char *domain,
                      const char *password, const uint8_t rnd[24], uint8_t key[16]);

/* Signs an SMB2 message (without the transport header) as dialects 2.0.2 and 2.1 do. */
void smb_sign(uint8_t *msg, size_t len, const uint8_t key[16]);

#endif
