/* cleanproto.h — the channel between lord-server (clean.c) and lord-clean, Cleaner's sandboxed scanner
 * (docs/CLEANER-ARCHITECTURE.md).
 *
 * The transport is lord-doc's (doc/wire.h): a SOCK_SEQPACKET pair on fd 3, messages of a header and JSON,
 * a batch too large for one message travelling in a sealed memfd. lord-server sends one CQ_GO with the
 * opened roots as fds, in the order of "roots"; lord-clean answers with CR_BATCH records and ends with
 * CR_DONE or CR_ERR. Everything lord-clean says is validated by lord-server (clean.c); to stop a scan it
 * kills the helper.
 *
 *   CQ_GO     {"mode":"rules"|"tree"|"dups","home","maxFiles","maxDepth","minSize","exclude":[paths],
 *              "roots":[{"rule","pattern","base"}]}                               + one fd per root
 *   CR_BATCH  {"seen","dirs","hashed","items":[{"r","p","M","m","i","l","s","a","t","n","u"}],
 *              "nodes":[{"id","up","name","s","a","f"}],"group":bool}
 *             r the source, p path relative to it, M/m device major and minor, i inode (decimal text: it
 *             may not fit a JSON number exactly), l links, s size, a allocated bytes, t/n modification
 *             time, u owner. "tree": a node is a folder, reported after its contents, with id > up;
 *             items are files of at least minSize. "dups": each batch with "group" is one group of
 *             files with equal SHA-256.
 *   CR_DONE   {"seen","dirs","limit":""|"files"|"depth"|"nodes"|"results","symlinks","special","mounts",
 *              "unreadable","protected","names","nomatch","excluded","types":[{"s","a","f"}]}
 *   CR_ERR    {"error":"sandbox"|"request"}                                                        */
#ifndef LORD_CLEAN_PROTO_H
#define LORD_CLEAN_PROTO_H

#define CLEAN_MAX_ROOTS 12         /* fds in one message: below DOC_MAX_FDS */
#define CLEAN_MAX_DEPTH 32          /* folders below a root */
#define CLEAN_MAX_FILES 200000      /* entries examined in one scan: past this the scan is incomplete */
#define CLEAN_PATH_MAX  1024        /* bytes of a path relative to its root */
#define CLEAN_BATCH     256         /* records in one CR_BATCH */

#define CLEAN_TREE_MAX_FILES 5000000 /* the tree keeps totals, not entries */
#define CLEAN_MAX_NODES  100000     /* folders kept in a tree: the rest count in their parent */
#define CLEAN_MAX_LARGE  50000      /* large files kept from a tree */
#define CLEAN_MAX_GROUP  1024       /* files shown in one group of duplicates */
#define CLEAN_MAX_EXCLUDE 32

enum { CLEAN_TYPE_OTHER, CLEAN_TYPE_IMAGE, CLEAN_TYPE_VIDEO, CLEAN_TYPE_AUDIO, CLEAN_TYPE_DOCUMENT, CLEAN_TYPE_ARCHIVE, CLEAN_TYPE_CODE, CLEAN_TYPES };

enum { CQ_GO = 1, CR_BATCH = 2, CR_DONE = 3, CR_ERR = 4 };

#endif
