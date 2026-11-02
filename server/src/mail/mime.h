/* mime.h — mail messages (RFC 5322, MIME 2045-2049, 2047, 2231) for axia-maild.
 *
 * Everything works on the raw in-memory message and checks every index against the length:
 * a broken or malicious message yields empty parts, never a read outside the buffer. What is
 * handed to the UI is always valid UTF-8 (invalid input becomes U+FFFD). */
#ifndef LORD_MAIL_MIME_H
#define LORD_MAIL_MIME_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define MIME_MAX_PARTS 256
#define MIME_MAX_DEPTH 16

typedef struct {
    int parent;               /* index of the parent part (-1 for the root) */
    int depth;
    char type[48], subtype[64];       /* lowercase; "text"/"plain" if there is no Content-Type */
    char charset[40];
    char boundary[128];
    char encoding[24];                /* content-transfer-encoding, lowercase */
    char disposition[16];             /* "inline" | "attachment" | "" */
    char filename[256];               /* UTF-8 (2231/2047 already decoded) */
    char cid[160];                    /* Content-ID without <> */
    char method[24];                  /* text/calendar; method= */
    const char *hdr; size_t hlen;     /* raw header of the part */
    const char *body; size_t blen;    /* raw body (encoded) */
} MimePart;

typedef struct {
    MimePart parts[MIME_MAX_PARTS];
    int nparts;
    int truncated;                    /* there were more parts than fit */
} MimeMsg;

/* Splits header and body of a message or part (CRLF or LF). */
void mime_split(const char *m, size_t len, const char **hdr, size_t *hlen, const char **body, size_t *blen);
/* Value of a header (first occurrence), unfolded (no CRLF+whitespace), raw. 1 if present. */
int  mime_header(const char *hdr, size_t hlen, const char *name, char *out, size_t cap);
/* Same but in newly allocated memory (malloc), for long headers (References). NULL if absent. */
char *mime_header_dup(const char *hdr, size_t hlen, const char *name);
/* Decodes encoded words (=?cs?B|Q?...?=) to UTF-8 and strips control characters. */
void mime_decode_words(const char *in, char *out, size_t cap);
/* Parameter of a structured value ("text/plain; charset=utf-8"), with RFC 2231 (name*=, name*0*=). */
int  mime_param(const char *value, const char *name, char *out, size_t cap);
/* Builds the part tree. Always leaves at least the root. */
void mime_parse(const char *m, size_t len, MimeMsg *out);
/* Decoded body (base64, quoted-printable, 7bit/8bit/binary) in newly allocated memory. */
char *mime_decode_body(const MimePart *p, size_t *olen);
/* Text of a text/... part in UTF-8 (charset converted). */
char *mime_part_text(const MimePart *p, size_t *olen);

/* Character sets → UTF-8 (UTF-8, ASCII, Latin-1, the ISO-8859 and windows-125x families, KOI8,
 * UTF-16/32 with or without BOM). Unknown: treated as UTF-8 with replacement. malloc. */
char *charset_to_utf8(const char *cs, const char *in, size_t len, size_t *olen);
int  charset_known(const char *cs);
/* Copies UTF-8, replacing invalid sequences with U+FFFD. malloc. */
char *utf8_clean(const char *in, size_t len, size_t *olen);

/* base64 and quoted-printable */
size_t b64_decode(const char *in, size_t len, unsigned char *out);   /* out ≥ len*3/4+3 */
char  *b64_encode(const unsigned char *in, size_t len, int wrap, size_t *olen);  /* wrap at 76 with CRLF */
size_t qp_decode(const char *in, size_t len, char *out, int header); /* out ≥ len */
char  *qp_encode(const char *in, size_t len, size_t *olen);          /* body, lines ≤ 76, CRLF */

/* One address from a list (From, To, Cc). */
typedef struct { char name[160]; char email[256]; } MimeAddr;
/* Splits an address list (with quoted names, comments and groups). Returns
 * how many were stored in out (at most max). The value may come with encoded words
 * decoded or not: names are decoded here. */
int  mime_addrs(const char *value, MimeAddr *out, int max);
/* RFC 5322 date ("Tue, 3 Jun 2025 10:22:01 -0300") → UNIX seconds, 0 if unparseable. */
time_t mime_date(const char *value);
/* Date for the Date header (local time with its offset). */
void mime_format_date(time_t t, char *out, size_t cap);
/* Header with encoded words if needed (UTF-8 → =?UTF-8?B?...?=, ≤ 75 per word). */
char *mime_encode_header(const char *utf8);
/* An address for a header: "Name" <a@b> with the name encoded if needed. */
char *mime_format_addr(const char *name, const char *email);
/* Subject without repeated "Re:", "RE:", "Fwd:", "RV:", "AW:"…, for grouping conversations. */
void mime_base_subject(const char *subj, char *out, size_t cap);

/* A safe basename for an attacker-chosen file name (see mime.c). Never empty. */
void mime_safe_filename(const char *in, char *out, size_t cap);

#endif
