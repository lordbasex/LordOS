/* vmsig.h — the OpenPGP check of a system the person added (Machines settings → Systems; approved by the maintainer on
 * 2026-11-22, AGENTS.md "lord-pgpv"). lord-pgpv (lordpgpv.c) runs vmsig_verify in the helpers' sandbox on three
 * sealed memfds lord-server made (the system's public key, the checksum file, the detached signature) and prints one
 * line; lord-server reads that line with vmsig_reply and only then looks for the image's SHA-256 in the very bytes of
 * the checksum file it handed over (vmsig_sums_has_hash). Pure functions over bytes; tests/vmsig_test.c. */
#ifndef LORD_VM_SIG_H
#define LORD_VM_SIG_H

#include <stddef.h>
#include <stdint.h>

#define VMSIG_KEY_MAX  (32u << 10)          /* an armored public key from the system's definition */
#define VMSIG_SIG_MAX  (64u << 10)
#define VMSIG_SUMS_MAX (1u << 20)

/* the reasons an invalid answer may give (a fixed list: lord-server takes no other text from the helper) */
#define VMSIG_REASONS "malformed", "noKey", "otherKey", "badSignature", "keyExpired", "keyRevoked", "keyNotSigning", \
                      "signatureExpired", "futureSignature", "unsupported", "tooLarge"

/* Verifies the detached signature sig over sums with the primary key whose v4 fingerprint is fpr (40 hex digits, any
 * case) in key (armored or binary), or one of its subkeys; no other key in key counts. 1 = valid (out_fpr: that
 * fingerprint, upper case), 0 = invalid with a reason word from VMSIG_REASONS in why. */
int vmsig_verify(const char *key, size_t kl, const char *sums, size_t sl, const char *sig, size_t gl,
                 const char *fpr, int64_t now, char out_fpr[41], char *why, size_t whyn);

/* The helper's answer line as lord-pgpv prints it: "valid <40 hex>\n" or "invalid <reason>\n". */
void vmsig_answer(int ok, const char *fpr_or_reason, char *out, size_t n);

/* lord-server's reading of that answer: 1 when it is exactly "valid <fpr>\n" with the fingerprint it asked for, 0 when
 * it is "invalid <reason>\n" with a known reason (copied to why), -1 for anything else (treated as invalid). */
int vmsig_reply(const char *out, size_t n, const char *fpr, char *why, size_t whyn);

/* 1 if a line of the checksum file gives this SHA-256 (64 hex, any case) for any file: GNU "<hash>  <name>" /
 * "<hash> *<name>" or BSD "SHA256 (<name>) = <hash>" */
int vmsig_sums_has_hash(const char *sums, size_t n, const char *sha);

/* 1 if s is a v4 fingerprint: 40 hex digits */
int vmsig_fpr_ok(const char *s);

#endif
