/* monpriv.h — internal monitor pieces shared by mon.c and conn.c. */
#ifndef LORD_MONPRIV_H
#define LORD_MONPRIV_H

#include "mmdb.h"

#include <stddef.h>
#include <stdint.h>

/* Growing JSON (memory errors leave the buffer as NULL: the result is discarded). */
typedef struct { char *p; size_t n, cap; int bad; } Buf;

void mon_buf_printf(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void mon_buf_str(Buf *b, const char *s);          /* "text" with escapes; invalid UTF-8 → '?' */
void mon_buf_free(Buf *b);

/* Reads a whole small file (max. n-1 bytes, NUL-terminated). Returns the bytes read or -1. */
long mon_read_small(const char *path, char *out, size_t n);
double mon_now_ms(void);                      /* monotonic clock */
double mon_wall_ms(void);                     /* wall-clock time (for the JSON) */

/* Connections (conn.c): builds {"t", "others", "list": [...]} with the user's.
 * geo may be NULL (no country database). */
void conn_scan(Buf *b, const Mmdb *geo);
void conn_reset(void);
/* Sentinela: per running machine, counts only (conn.c) */
void conn_machines(Buf *b);

#endif
