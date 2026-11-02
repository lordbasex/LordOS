/* vcard.h — vCard 2.1, 3.0 and 4.0 (RFC 6350) for Contacts in axia-maild.
 *
 * Reading goes through the shared content-line parser (ical.h). The JSON a contact becomes:
 *   {"uid","fn","n":{"family","given","middle","prefix","suffix"},"nick","org","dept","title",
 *    "emails":[{"type","value"}],"phones":[…],"addrs":[{"type","street","city","region","code","country"}],
 *    "urls":[…],"im":[…],"bday":"YYYY-MM-DD","notes","cats":[…],"photo":true,"kind":"individual"|"group",
 *    "members":["uid"…],"rev"}
 * Writing produces vCard 3.0 (what the common CardDAV servers, Nextcloud and Radicale among them, accept). */
#ifndef LORD_MAIL_VCARD_H
#define LORD_MAIL_VCARD_H

#include <stddef.h>
#include "../doc/wire.h"
#include "../sec/json.h"

#define VCARD_MAX_PHOTO (4u << 20)

/* Appends one JSON object per VCARD found in text to b (comma separated, no brackets).
 * Returns the count, or -1 if the text is not a vCard. */
int  vcard_json(const char *text, size_t len, JBuf *b);
/* Decoded PHOTO of the first VCARD (base64 or data: URI). malloc; NULL if none. */
unsigned char *vcard_photo(const char *text, size_t len, size_t *olen);
/* UID of the first VCARD into out ("" if none). */
void vcard_uid(const char *text, size_t len, char *out, size_t cap);
/* vCard 3.0 from a contact JSON object (see above). photo may be NULL. malloc. */
char *vcard_build(const JVal *c, const unsigned char *photo, size_t photo_len, const char *photo_type, size_t *olen);
/* Name and e-mail pairs of every VCARD, for Mail's autocomplete: calls fn(name, email). */
void vcard_emails(const char *text, size_t len, void (*fn)(void *u, const char *name, const char *email), void *u);

#endif
