/* imapp.h — IMAP4rev1 response parser (RFC 3501) for axia-maild, pure and bounds-checked.
 *
 * The connection layer (imap.c) assembles one complete response: the line plus any literals,
 * kept inline exactly as on the wire ("{N}\r\n" followed by N bytes). imap_parse() turns it into
 * a small tree of values (atoms, strings, NIL, lists) that live in one arena. Bracketed sections
 * ("BODY[HEADER.FIELDS (FROM)]<0>", "[UIDVALIDITY 7]") stay a single atom, as clients expect. */
#ifndef LORD_MAIL_IMAPP_H
#define LORD_MAIL_IMAPP_H

#include <stddef.h>
#include <stdint.h>

#define IMAP_MAX_VALUES 200000
#define IMAP_MAX_NEST   32

typedef enum { IV_ATOM, IV_STR, IV_NIL, IV_LIST } IvType;

typedef struct IVal IVal;
struct IVal {
    IvType type;
    const char *s;          /* IV_ATOM / IV_STR: bytes (points into the response or the arena) */
    size_t n;
    IVal *kid, *next;       /* IV_LIST children; next sibling */
};

typedef struct {
    char tag[32];           /* "*", "+" or the command tag */
    IVal *first;            /* tokens after the tag */
    char *arena;            /* owns values and unescaped strings */
} IResp;

/* 0 ok, -1 malformed (unbalanced lists, bad literal, too many values). */
int  imap_parse(const char *buf, size_t len, IResp *out);
void imap_resp_free(IResp *r);

/* Helpers */
int   iv_is(const IVal *v, const char *atom);          /* case-insensitive atom match */
long long iv_num(const IVal *v, long long def);
IVal *iv_at(IVal *first, int idx);                     /* n-th sibling */
int   iv_count(const IVal *first);
/* In a FETCH list ("UID 5 FLAGS (\Seen) BODY[...] {12}..."), the value after `key`.
 * The key match is case-insensitive; a key with "[" (e.g. "BODY[HEADER") matches by prefix. */
IVal *imap_fetch_item(IVal *list, const char *key);
/* Copies a string/atom value (NIL → "") into out; returns length. */
size_t iv_copy(const IVal *v, char *out, size_t cap);
/* Response code number from an "[CODE n]" atom anywhere in the response (e.g. UIDVALIDITY). */
int   imap_code_num(const IResp *r, const char *code, unsigned long long *out);

/* IMAP quoted string for a command, or a literal "{n}\r\n…" when needed. The caller frees.
 * nonsync = 1 uses "{n+}" (LITERAL+). For literals, *needs_cont is set when the server must
 * answer "+" before the bytes are sent (synchronizing literal). */
char *imap_quote(const char *s, int nonsync, int *is_literal);
/* Modified UTF-7 (RFC 3501 §5.1.3) mailbox names. */
void imap_utf7_decode(const char *in, char *out, size_t cap);
void imap_utf7_encode(const char *in, char *out, size_t cap);
/* Compact UID set ("1:5,7,9:12") from an ascending array; returns bytes written. */
size_t imap_uidset(const uint32_t *uids, size_t n, char *out, size_t cap);
/* Splits "1:5,7" into the uids (ascending, up to max; "*" is ignored). Returns the count. */
size_t imap_parse_uidset(const char *s, uint32_t *out, size_t max);

#endif
