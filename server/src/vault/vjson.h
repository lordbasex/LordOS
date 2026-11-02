/* vjson.h — JSON inside axia-vault without going through the libc heap: a reader that marks
 * tokens over the text (without copying it) and a writer that grows in the secrets arena (smem.h).
 * Used by the protocol with axia-server/the app, the vault payload and the importers.
 *
 * Reader: vj_parse builds an array of tokens in order; each one knows where its subtree ends
 * (next), so walking an object is: for (k = o + 1; k < t[o].next; k = t[k + 1].next) { key k,
 * value k + 1 }. Strict (RFC 8259), with a depth limit (64). */
#ifndef LORD_VAULT_VJSON_H
#define LORD_VAULT_VJSON_H

#include <stddef.h>
#include <stdint.h>

enum { VJ_OBJ = 1, VJ_ARR, VJ_STR, VJ_NUM, VJ_TRUE, VJ_FALSE, VJ_NULL };

typedef struct { uint8_t type; uint32_t start, end; int next; int count; } VjTok;  /* start/end: bytes (STR without quotes) */
typedef struct { const char *s; size_t len; VjTok *t; int n, cap; } Vj;

int    vj_parse(Vj *j, const char *s, size_t len);   /* 0 on success; tokens in smem */
void   vj_free(Vj *j);
int    vj_get(const Vj *j, int obj, const char *key); /* value token, or -1 */
int    vj_at(const Vj *j, int arr, int i);            /* i-th element, or -1 */
int    vj_type(const Vj *j, int tok);                 /* 0 if tok is -1 */
char  *vj_str(const Vj *j, int tok);                  /* text without escapes, in smem (NULL if not STR) */
size_t vj_strbuf(const Vj *j, int tok, char *buf, size_t n);   /* bounded copy; returns the length */
int    vj_streq(const Vj *j, int tok, const char *s);
double vj_num(const Vj *j, int tok, double def);
int    vj_bool(const Vj *j, int tok, int def);
/* shortcuts on an object */
char  *vj_gets(const Vj *j, int obj, const char *key);
size_t vj_getsbuf(const Vj *j, int obj, const char *key, char *buf, size_t n);
double vj_getn(const Vj *j, int obj, const char *key, double def);
int    vj_getb(const Vj *j, int obj, const char *key, int def);

/* Writer (in smem). The caller adds the commas, except with vb_k* ("key":value with a
 * comma when needed: tracked per level with vb_open/vb_close). */
typedef struct { char *p; size_t n, cap; int oom; uint8_t first[64]; int depth; } VjB;
void vb_init(VjB *b);
void vb_free(VjB *b);                      /* wipes and frees */
void vb_raw(VjB *b, const char *s);
void vb_rawn(VjB *b, const char *s, size_t n);
void vb_str(VjB *b, const char *s);        /* "text" with escapes; NULL → null */
void vb_strn(VjB *b, const char *s, size_t n);
void vb_num(VjB *b, double v);
void vb_int(VjB *b, long long v);
void vb_open(VjB *b, char c);              /* '{' or '[' */
void vb_close(VjB *b, char c);             /* '}' or ']' */
void vb_sep(VjB *b);                       /* comma if not the first of the level */
void vb_key(VjB *b, const char *k);        /* vb_sep + "k": */
void vb_ks(VjB *b, const char *k, const char *v);
void vb_kn(VjB *b, const char *k, double v);
void vb_ki(VjB *b, const char *k, long long v);
void vb_kb(VjB *b, const char *k, int v);
/* Base64 (RFC 4648) in smem */
char    *b64_encode(const uint8_t *in, size_t n);
uint8_t *b64_decode(const char *in, size_t n, size_t *outlen);   /* also accepts base64url */

#endif
