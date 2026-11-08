/* proto.h — protocol between lord-server and its document helper lord-doc (see docs/DOCUMENTS.md).
 *
 * lord-doc is a separate process, without network and locked down (Landlock + seccomp), that opens PDFs
 * (Poppler), archives (libarchive) and fonts (FreeType). lord-server never links those
 * libraries: a malicious PDF or .zip can only bring down the helper.
 *
 * The channel is a SOCK_SEQPACKET socketpair (fd 3 of the helper). Each message is a DocHdr + JSON
 * (UTF-8) and, with SCM_RIGHTS, up to DOC_MAX_FDS descriptors. If the JSON doesn't fit in a message
 * (more than DOC_INLINE_MAX) it travels in a sealed memfd and the header carries DOCF_JSON_FD.
 *
 * Startup: lord-server first sends DQ_INIT_FD messages (the files the helper may
 * use: the document, the destination folder of an extraction, the items to compress) and
 * then DQ_INIT_GO. Only then does the helper lock itself down, with Landlock built from those
 * fds, and start serving. What arrives afterwards never adds file permissions.
 *
 * Replies carry the request id. An image (page, font sample) comes back in a
 * sealed memfd with premultiplied ARGB32 pixels (those of cairo and of the LordOS engine). */
#ifndef LORD_DOC_PROTO_H
#define LORD_DOC_PROTO_H

#include <stdint.h>

#define DOC_INLINE_MAX (48 * 1024)
#define DOC_JSON_MAX   (16 * 1024 * 1024)   /* largest JSON accepted (in memfd) */
#define DOC_MAX_FDS    16
#define DOC_IMG_MAX    8192                 /* maximum side of a returned image */
#define DOC_INIT_FDS   256                  /* maximum number of startup fds */

typedef struct {
    uint32_t op;        /* DQ_* in requests, DR_* in replies */
    uint32_t id;        /* request id (0 at startup) */
    uint32_t len;       /* bytes of JSON that follow in the message (0 if it's in a memfd or there's none) */
    uint32_t flags;     /* DOCF_* */
} DocHdr;

enum { DOCF_JSON_FD = 1 };                  /* the first fd is a memfd with the JSON */

/* Requests */
enum {
    DQ_INIT_FD = 1,     /* {"role":"ro"|"rw"|"file"|"out"} + 1..16 fds, before locking down */
    DQ_INIT_GO,         /* {"mode":"pdf"|"arc"|"font"|"make"} */

    DQ_PDF_OPEN = 10,   /* {"slot":0,"password":""} */
    DQ_PDF_RENDER,      /* {"page","width","rot","invert"} → DR_IMG */
    DQ_PDF_SELECT,      /* {"page","x1","y1","x2","y2","style":0|1|2} → {text, rects} */
    DQ_PDF_FIND,        /* {"text","case","whole"} → DR_PART per page, DR_DONE */
    DQ_PDF_OUTLINE,     /* → {items:[{title,page,kids}]} */
    DQ_PDF_PAGEINFO,    /* {"page"} → {links, fields, annots} */
    DQ_PDF_SETFIELD,    /* {"id","value"} */
    DQ_PDF_ANNOT,       /* {"page","type":"highlight"|"note"|"ink",...} */
    DQ_PDF_SAVE,        /* {"slot"} (output fd) → DR_DONE */
    DQ_PDF_BLOB,        /* → DR_BLOB (the PDF with the changes, in a memfd) */
    DQ_PDF_UNDO,        /* {"page"}: removes the last annotation added on that page */
    DQ_PDF_SIZES,       /* → {sizes:[[w,h]...], forms} (open with "quick" only brings the first) */
    DQ_PDF_THUMB,       /* (util session) fd + {"width","height","invert"} → DR_IMG of page 1 */
    DQ_WARM,            /* (util session) prepares fontconfig and Poppler, so Quick Preview doesn't wait */

    DQ_ARC_LIST = 30,   /* {"slot"} → {entries:[{p,s,m,d,l}], truncated, format} */
    DQ_ARC_EXTRACT,     /* {"slot","dir","only":[...]} → DR_PROG..., DR_DONE */
    DQ_ARC_CREATE,      /* {"out","items":[{"slot","name"}],"format":"zip"} → DR_PROG..., DR_DONE */

    DQ_FONT_INFO = 50,  /* fd + {"index"} → {family, style, ...} */
    DQ_FONT_RENDER,     /* fd + {"index","lines":[{text,size}],"width","fg","bg"} → DR_IMG */
    DQ_FONT_GRID,       /* fd + {"index","cps":[...],"cell","cols","fg","bg"} → DR_IMG + {present} */

    DQ_MAKE_TEXT = 70,  /* {"text","title","mono","size","paper"} → DR_BLOB (PDF) */
    DQ_MAKE_IMAGE,      /* fd + {"paper","fit"} → DR_BLOB (PDF) */
    DQ_IMG_DECODE,      /* fd + {"maxw","maxh"} → DR_IMG */
    DQ_IMG_CONVERT,     /* input fd + output fd + {"format":"png"|"jpeg"|"pdf","quality"} → DR_DONE */

    DQ_TTS = 82,        /* (lord-tts) {"slot","file","text","backend","threads","voice_id","seed"} + voice WAV fd? → DR_BLOB WAV (h_tts.c) */
    DQ_OCR = 80,        /* (lord-ocr) fd (w*h premultiplied ARGB32) + {"w","h","langs"} → {text} (h_ocr.c) */

    DQ_SELFTEST = 90,   /* lockdown test: tries the forbidden and says what happened (tests/doc_test.c) */
};

/* Replies */
enum {
    DR_DONE = 1,        /* finished: JSON with the result */
    DR_ERR,             /* finished with an error: {"error":"code","detail":"…"} */
    DR_PROG,            /* progress (doesn't finish) */
    DR_PART,            /* partial result (doesn't finish) */
    DR_IMG,             /* finished: {"w","h"} + ARGB32 memfd (w*h*4) */
    DR_BLOB,            /* finished: {"size"} + memfd with the bytes */
};

#endif
