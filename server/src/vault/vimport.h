/* vimport.h — import and export passwords from other managers (see docs/PASSWORDS.md,
 * "Import and export"). Runs inside axia-vault (confined, no network): the file arrives as
 * bytes already read from an fd opened by axia-server. Everything built (strings, attachments) goes in
 * the secrets arena (smem.h) and imp_free wipes it.
 *
 * Formats:
 *   - KeePass/KeePassXC KDBX 4.x (own reader: Argon2d/Argon2id/AES-KDF, AES-256-CBC or ChaCha20,
 *     HMAC-SHA256 blocks, gzip, ChaCha20/Salsa20 inner stream and the KeePass XML), with
 *     password and/or key file; and the CSV exported by KeePassXC.
 *   - Bitwarden JSON (unencrypted) and CSV.
 *   - Chrome and other Chromium-based browsers, Firefox, and the generic
 *     Title,URL,Username,Password,Notes,OTPAuth CSV of other password managers.
 *   - LordOS own CSV (the one exp_csv writes).
 * The parsers are robust against broken or malicious files ("fuzz-ish" tests in tests/). */
#ifndef LORD_VAULT_VIMPORT_H
#define LORD_VAULT_VIMPORT_H

#include <stddef.h>
#include <stdint.h>

enum {
    IMP_UNKNOWN = 0,
    IMP_KDBX,              /* KeePass 2.x / KeePassXC, .kdbx (version 4; 3.1 if possible) */
    IMP_KEEPASSXC_CSV,     /* "Group","Title","Username","Password","URL","Notes","TOTP","Icon","Last Modified","Created" */
    IMP_BITWARDEN_JSON,    /* {"encrypted":false,"folders":[...],"items":[...]} */
    IMP_BITWARDEN_CSV,     /* folder,favorite,type,name,notes,fields,reprompt,login_uri,login_username,login_password,login_totp */
    IMP_CHROME_CSV,        /* name,url,username,password[,note] */
    IMP_FIREFOX_CSV,       /* "url","username","password","httpRealm","formActionOrigin","guid","timeCreated","timeLastUsed","timePasswordChanged" */
    IMP_APPLE_CSV,         /* Title,URL,Username,Password,Notes,OTPAuth */
    IMP_LORD_CSV,          /* category,title,username,password,url,notes,totp,tags,favorite,fields,urls,kind,created,modified (exp_csv) */
};

/* Extra field type (matches the vault's: vmodel.h) */
enum { IMPF_TEXT = 0, IMPF_HIDDEN, IMPF_URL, IMPF_DATE, IMPF_NUMBER, IMPF_TOTP };

typedef struct { char *key; char *value; int type; } ImpField;          /* IMPF_* */
typedef struct { char *name; uint8_t *data; size_t len; } ImpAttach;

typedef struct {
    char *title, *username, *password, *url, *notes;
    char *otp;              /* otpauth:// URI or the base32 secret exactly as it came */
    char *group;            /* category/folder path, separated by '/' ("Internet/Banks"); NULL = none */
    char *kind;             /* type hint: "login", "card", "identity", "note", "wifi", "ssh"… or NULL */
    char **tags; int ntags;
    char **urls; int nurls; /* additional URLs (Bitwarden carries several) */
    ImpField *fields; int nfields;
    ImpAttach *atts; int natts;
    int64_t created, modified;      /* Unix seconds; 0 = unknown */
    int favorite;
} ImpEntry;

typedef struct {
    ImpEntry *v; int n, cap;
    char **groups; int ngroups;     /* folders that existed even if empty (KDBX, Bitwarden) */
    char err[160];                  /* short English code for the UI: "bad-password", "unsupported-cipher"… */
} ImpList;

/* Guesses the format from the content (and optionally the file name). */
int  imp_detect(const uint8_t *data, size_t len, const char *name_hint);
/* Reads the file. password/keyfile only for KDBX (NULL if none). 0 ok; -1 with l->err:
 *   "bad-password" (KDBX: the composite key does not open it), "corrupt", "truncated",
 *   "unsupported-version", "unsupported-cipher", "unsupported-kdf", "encrypted-export"
 *   (encrypted Bitwarden: ask for the unencrypted JSON), "empty", "too-big", "memory", "bad-keyfile" (KeePass XML key file with a
 *   mismatching Hash). */
int  imp_parse(int kind, const uint8_t *data, size_t len, const char *password,
               const uint8_t *keyfile, size_t kflen, ImpList *l);
void imp_free(ImpList *l);           /* wipes and frees everything (including l->v) */

/* LordOS own CSV (IMP_LORD_CSV), with header; entries as in ImpList. *out in smem. */
int  exp_csv(const ImpList *l, char **out, size_t *outlen);

/* Shared utilities */
char *imp_strdup(const char *s);                      /* en smem */
ImpEntry *imp_add(ImpList *l);                        /* new zeroed entry, or NULL */
/* ISO 8601 date ("2024-03-05T10:20:30Z", with fraction or local time taken as UTC; or just
 * the date) → Unix seconds; 0 if not understood. */
int64_t imp_parse_iso(const char *s);

#endif
