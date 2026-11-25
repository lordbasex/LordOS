/* xz.h — in-memory decompression of .xz (LZMA2, our own) and .gz (stb_image's inflate). */
#ifndef LORD_PKG_XZ_H
#define LORD_PKG_XZ_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* 0 ok; *out with malloc. max: maximum accepted output size. */
int xz_decode(const uint8_t *in, size_t len, uint8_t **out, size_t *outlen, size_t max);
int gz_decode(const uint8_t *in, size_t len, uint8_t **out, size_t *outlen, size_t max);   /* not with -DXZ_NO_GZ */

/* Streaming .xz (lord-vmunpack: archives of several GB). The input comes from rd (bytes read into buf, at most n:
 * >0, 0 at its end, -1 an error); xzs_read hands out the decoded bytes in order. Same container rules as xz_decode
 * (one or more streams, stream padding, blocks with one LZMA2 filter), and stricter: the block check must be none,
 * CRC32 or CRC64 (verified; SHA-256 and the reserved types are refused), the sizes a block header states must be
 * the block's, the index must list exactly the blocks seen (their unpadded and uncompressed sizes), and a block's
 * dictionary may be at most max_dict bytes (the window is that plus 2 MiB). */
typedef struct XzStream XzStream;
enum { XZS_E_DATA = 1, XZS_E_TRUNCATED, XZS_E_TRAILING, XZS_E_DICT, XZS_E_CHECK, XZS_E_MEM, XZS_E_IO };
XzStream *xzs_new(int (*rd)(void *ctx, uint8_t *buf, size_t n), void *ctx, size_t max_dict);
/* >0 bytes into out; 0 the whole input ended cleanly (every stream with its index and footer, then only stream
 * padding up to the input's end); -1 an error (xzs_error: XZS_E_*), and every later call is -1 too */
ssize_t xzs_read(XzStream *s, uint8_t *out, size_t n);
int xzs_error(const XzStream *s);
uint64_t xzs_in_pos(const XzStream *s);              /* input bytes consumed so far */
void xzs_free(XzStream *s);

#endif
