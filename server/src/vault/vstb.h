/* vstb.h — stb_image (vendored) inside axia-vault, with memory in the secrets arena
 * (smem.h): a decompressed KDBX or the photo of a QR code never goes through the libc heap.
 * The stb symbols stay static (they do not clash with those of ui/stb.c). */
#ifndef LORD_VAULT_VSTB_H
#define LORD_VAULT_VSTB_H

#include <stddef.h>
#include <stdint.h>

/* raw deflate (no zlib header) → bytes in smem (free with sm_free); NULL if invalid or
 * larger than max bytes. */
uint8_t *vstb_inflate_raw(const uint8_t *in, size_t len, size_t max, size_t *outlen);
/* gzip (RFC 1952, like KDBX's) → bytes in smem; NULL if invalid. */
uint8_t *vstb_gunzip(const uint8_t *in, size_t len, size_t max, size_t *outlen);
/* PNG or JPEG → 0xAARRGGBB pixels in smem (w*h); NULL on failure (max side 8192). */
uint32_t *vstb_decode_argb(const uint8_t *in, size_t len, int *w, int *h);

#endif
