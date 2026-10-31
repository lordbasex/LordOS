/* cleanproto.h — the channel between lord-server (clean.c) and lord-clean, Cleaner's sandboxed scanner
 * (docs/CLEANER-ARCHITECTURE.md).
 *
 * The transport is lord-doc's (doc/wire.h): a SOCK_SEQPACKET pair on fd 3, messages of a header and JSON,
 * a batch too large for one message travelling in a sealed memfd. lord-server sends one CQ_GO with the
 * opened roots as fds, in the order of "roots"; lord-clean answers with CR_BATCH records and ends with
 * CR_DONE or CR_ERR. Everything lord-clean says is validated by lord-server (clean.c); to stop a scan it
 * kills the helper.
 *
 *   CQ_FDS    {}                                 + up to CLEAN_FDS_MSG root fds, in the order of "roots"
 *   CQ_GO     {"mode":"rules"|"tree"|"dups"|"images","home","maxFiles","maxDepth","minSize","exclude":[paths],
 *              "roots":[{"rule","pattern","base","unit","own"}]}   unit > 0: entries at that depth are reported whole
 *              as one folder record ("d":true, s and a its contents' totals); own: only what the session
 *              user owns is looked at, for a root other users write in
 *   CR_BATCH  {"seen","dirs","hashed","items":[{"r","p","M","m","i","l","s","a","t","n","u"}],
 *              "nodes":[{"id","up","name","s","a","f"}],"group":bool}
 *             r the source, p path relative to it, M/m device major and minor, i inode (decimal text: it
 *             may not fit a JSON number exactly), l links, s size, a allocated bytes, t/n modification
 *             time, u owner. "tree": a node is a folder, reported after its contents, with id > up;
 *             items are files of at least minSize. "dups": each batch with "group" is one group of
 *             files with equal SHA-256. "images": each batch with "group" is one group of similar images,
 *             largest first, and each item adds W and H (pixels) and ph (the difference hash, 16 hex digits).
 *   CR_DONE   {"seen","dirs","limit":""|"files"|"depth"|"nodes"|"results","symlinks","special","mounts",
 *              "unreadable","protected","names","nomatch","excluded","undecoded","types":[{"s","a","f"}]}
 *   CR_ERR    {"error":"sandbox"|"request"}                                                        */
#ifndef LORD_CLEAN_PROTO_H
#define LORD_CLEAN_PROTO_H

#define CLEAN_MAX_ROOTS 64
#define CLEAN_MAX_DEPTH 32          /* folders below a root */
#define CLEAN_MAX_FILES 200000      /* entries examined in one scan: past this the scan is incomplete */
#define CLEAN_PATH_MAX  1024        /* bytes of a path relative to its root */
#define CLEAN_BATCH     256         /* records in one CR_BATCH */

#define CLEAN_TREE_MAX_FILES 5000000 /* the tree keeps totals, not entries */
#define CLEAN_MAX_NODES  100000     /* folders kept in a tree: the rest count in their parent */
#define CLEAN_MAX_LARGE  50000      /* large files kept from a tree */
#define CLEAN_MAX_GROUP  1024       /* files shown in one group of duplicates */
#define CLEAN_MAX_EXCLUDE 32

/* similar images: what is decoded, in lord-clean only (helper/lcimage.c) */
#define CLEAN_MAX_IMAGES   20000    /* images read in one scan: past this the scan is incomplete */
#define CLEAN_IMAGE_MIN    8192     /* bytes: smaller files are icons, not photos */
#define CLEAN_IMAGE_MAX    (64u << 20)    /* bytes of a file read into memory */
#define CLEAN_IMAGE_PIXELS 40000000LL     /* pixels of an image decoded */
#define CLEAN_IMAGE_AS     (480ull << 20) /* address space while decoding, under the scope's 512 MB: a picture that needs more is left out */
#define CLEAN_IMAGE_MIN_DIM 32      /* pixels of the shorter side */
#define CLEAN_SIMILAR_BITS 6        /* bits of 64 two similar images may differ in */
#define CLEAN_SIMILAR_ASPECT 0.02   /* how far their proportions may differ */

enum { CLEAN_TYPE_OTHER, CLEAN_TYPE_IMAGE, CLEAN_TYPE_VIDEO, CLEAN_TYPE_AUDIO, CLEAN_TYPE_DOCUMENT, CLEAN_TYPE_ARCHIVE, CLEAN_TYPE_CODE, CLEAN_TYPES };

#define CLEAN_MAX_RULES 64
#define CLEAN_FDS_MSG   12         /* root fds in one CQ_FDS */

enum { CQ_GO = 1, CR_BATCH = 2, CR_DONE = 3, CR_ERR = 4, CQ_FDS = 5 };

#endif
