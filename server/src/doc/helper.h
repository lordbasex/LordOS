/* helper.h — what the parts of the lord-doc helper share (helper.c, h_pdf.c, h_arc.c,
 * h_font.c, h_make.c). It runs already locked down: it doesn't open files by path, it only uses the fds
 * lord-server passed to it. */
#ifndef LORD_DOC_HELPER_H
#define LORD_DOC_HELPER_H

#include <stddef.h>
#include <stdint.h>
#include "proto.h"
#include "wire.h"
#include "../sec/json.h"

/* startup fds (DQ_INIT_FD), by slot number */
typedef struct { int fd; char role[8]; } Slot;
extern Slot hslots[DOC_INIT_FDS];
extern int hnslots;
int slot_fd(const JVal *args, const char *key);     /* fd of slot args[key] or -1 */

/* Replies to request id */
void reply_json(uint32_t kind, uint32_t id, JBuf *b);        /* frees b */
void reply_err(uint32_t id, const char *code, const char *detail);
void reply_done_empty(uint32_t id);
/* Premultiplied ARGB32 image (stride = w*4) in a sealed memfd; extra = additional JSON
 * already built without braces (",\"present\":[...]") or NULL. */
void reply_image(uint32_t id, const uint32_t *px, int w, int h, const char *extra);
/* Pixels in an already written memfd (closes it). */
void reply_image_fd(uint32_t id, int fd, int w, int h, const char *extra);
void reply_blob(uint32_t id, int fd, size_t size);           /* sealed memfd; closes it */
int  hsock(void);

/* Each part handles its own requests; they return 0 if the op isn't theirs. */
int pdf_handle(const DocMsg *m, const JVal *args);
int arc_handle(const DocMsg *m, const JVal *args);
int font_handle(const DocMsg *m, const JVal *args);
int make_handle(const DocMsg *m, const JVal *args);
int ocr_handle(const DocMsg *m, const JVal *args);     /* lord-ocr only (h_ocr.c) */
void pdf_thumb(const DocMsg *m, const JVal *args);     /* util session: thumbnail of a PDF */
void pdf_warm(uint32_t id);

/* Inverts the luminosity of an image keeping the color (the viewer's dark mode). */
void invert_lightness(uint32_t *px, size_t n);

#endif
