/* vmurl.h — the address, size and space checks of vm-image-get-user (vm_admin.h; AGENTS.md "Machines' downloads of
 * your own systems", approved by the maintainer on 2026-11-22): pure functions, no network and no files, tested by
 * tests/vmurl_test.c (with a fuzz pass).
 *
 * An address is accepted only when it is exactly:
 *   "https://" host [":443"] path ["?" query]           at most VMURL_MAX bytes, nothing else
 *   host    a public DNS name (pkg/pubhost.h: no IPv4 or IPv6 literal, no localhost, .local, .internal, .lan, …),
 *           lower case (the app writes it so); no user name or password (no '@' before the path), no other port
 *   path    '/' and segments of RFC 3986 unreserved characters, sub-delimiters, ':' and '@', and %XX escapes (never
 *           %00); no "." or ".." segment, no empty last segment (it names a file)
 *   query   the same characters plus '/' and '?'
 *   never a fragment ('#'), a space, a control character, a backslash or a byte past 0x7e. */
#ifndef LORD_VM_URL_H
#define LORD_VM_URL_H

#include <stddef.h>
#include <stdint.h>

#define VMURL_MAX       1000                            /* the downloader's FetchItem.url holds 1024 */
#define VMURL_IMAGE_MAX (16ull << 30)                   /* 16 GiB: no installer is larger */
#define VMURL_RESERVE   (2ull << 30)                    /* what a download always leaves free on the image store */
#define VMURL_SIG_MAX   (64u << 10)                     /* a detached signature */
#define VMURL_SUMS_MAX  (1u << 20)                      /* the checksum file it signs */

/* 0 when url is acceptable; host (at least 254 bytes) gets its host name, file (at least 128) the last path segment
 * when it is a plain file name (vmcatalog.h vmcat_name_ok) or "image.iso" */
int vmurl_check(const char *url, char *host, size_t hn, char *file, size_t fn);

/* url without its query (its user name and password too, if a refused one had any): "https://host/path?…". For
 * logs and the image's description; never the query, which may hold a token. */
void vmurl_redact(const char *url, char *out, size_t n);

/* "<bytes>" (1 to VMURL_IMAGE_MAX, decimal, no sign or leading zero) or "-" (not known, 0): 0, or -1 */
int vmurl_size(const char *s, uint64_t *out);

/* How much a download may take: size known → that size, unknown → the most an image may be (VMURL_IMAGE_MAX); then
 * the store must have it, less what a kept part already holds, plus VMURL_RESERVE free. Returns the most the
 * download may be (the downloader's max), or 0 when it doesn't fit (*need gets the bytes it would need). */
uint64_t vmurl_room(uint64_t size, uint64_t part, uint64_t free_bytes, uint64_t *need);

#endif
