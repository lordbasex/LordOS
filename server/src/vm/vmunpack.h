/* vmunpack.h — lord-vmunpack's reading of a machine group's archive (docs/VIRTUALIZATION.md 19.6, "Unpacking"): a GNU
 * tar, inside xz, holding the members' disks. Pure apart from the two output descriptors it writes with pwrite and
 * ftruncate, so tests/vmunpack_test.c runs it in-process; vmunpack.c's main() is the sandboxed helper around it.
 *
 * The tar reader is strict. 512-byte headers whose checksum is right (the unsigned sum, or the signed one some old
 * tars wrote); numbers in octal or GNU's base-256. A member's name must be a plain name (1 to 99 bytes of printable
 * ASCII, no '/', not starting with '.'). Only regular members (type '0' or NUL) and old-GNU sparse members (type 'S',
 * magic "ustar  ", with extension headers) are allowed; anything else (links, devices, folders, GNU long names 'L'/'K',
 * pax headers 'x'/'g', multi-volume 'M', 'V', 'D') ends the job. A member is written when its name matches exactly one
 * role's pattern (role 0 the gateway, 1 the workstation), each role exactly once; other members are read past. A
 * sparse map must be ascending, non-overlapping and inside the member's real size, and its lengths add up to the
 * stored size. Real sizes are at most max_apparent; the stored data of all members together at most max_data. Data
 * goes to its offset by pwrite, and runs of zeros aligned on 4 KiB are not written (holes stay holes); then the file
 * is cut to its real size. After the two zero blocks that end the archive only zeros may follow (at most 1 MiB), and
 * then the source must end. */
#ifndef LORD_VM_VMUNPACK_H
#define LORD_VM_VMUNPACK_H

#include <stddef.h>
#include <stdint.h>

#define VMU_RE_MAX      256
#define VMU_REQ_MAX     1024            /* the request line, newline included */
#define VMU_MEMBERS_MAX 64              /* headers in one archive */
#define VMU_SPARSE_MAX  (1u << 20)      /* entries of one sparse map */
#define VMU_TAIL_MAX    (1u << 20)      /* zeros after the end of the archive */

/* the answer's codes (vmu_code_name: their words on the answer line) */
enum {
    VMU_OK = 0, VMU_BADXZ, VMU_TRUNCATED, VMU_TRAILING, VMU_BADTAR, VMU_BADCHECKSUM, VMU_BADTYPE, VMU_BADNAME,
    VMU_DUPLICATE, VMU_MISSING, VMU_BADSPARSE, VMU_TOOLARGE, VMU_TOOMUCHDATA, VMU_IO, VMU_REQUEST, VMU_COUNT
};
const char *vmu_code_name(int code);

/* the request, one line on fd 5: "<max apparent bytes> <max data bytes> <gateway pattern> <workstation pattern>\n";
 * the sizes 1 .. 2^62 in decimal, each pattern an anchored ("^…$") extended regular expression of printable ASCII
 * without spaces, shorter than VMU_RE_MAX */
typedef struct {
    uint64_t max_apparent, max_data;
    char re[2][VMU_RE_MAX];
} VmuReq;
/* 0, or -1 when the line isn't exactly that (or a pattern doesn't compile) */
int vmu_req_parse(const char *line, size_t n, VmuReq *r);

/* where the tar comes from: read gives >0 bytes, 0 at its end, -1 an error (then err() says which VMU_* code, or
 * VMU_IO when err is NULL) */
typedef struct {
    int (*read)(void *ctx, uint8_t *buf, size_t n);
    int (*err)(void *ctx);
    void *ctx;
} VmuSrc;

typedef struct {
    uint64_t real[2];                   /* each role's real (apparent) size */
    uint64_t data;                      /* the stored data of the two written members */
    char name[2][100];                  /* each role's member name */
} VmuResult;

/* reads the whole archive from src and writes the two roles' members into out[0] and out[1] (empty regular files).
 * VMU_OK or the first problem's code. */
int vmu_unpack(const VmuReq *r, const VmuSrc *src, const int out[2], VmuResult *res);

/* the VMU_* code of an XZS_E_* error (pkg/xz.h) */
int vmu_xz_code(int xzs_err);

#endif
