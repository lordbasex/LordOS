/* sftp.c — see sftp.h. */
#include "sftp.h"

#include <string.h>

void mxb_u64(MuxBuf *b, uint64_t v) { mxb_u32(b, (uint32_t)(v >> 32)); mxb_u32(b, (uint32_t)v); }

int mxr_u64(MuxRd *r, uint64_t *v) {
    uint32_t hi, lo;
    if (mxr_u32(r, &hi) || mxr_u32(r, &lo)) return -1;
    *v = (uint64_t)hi << 32 | lo;
    return 0;
}

int sf_get_attrs(MuxRd *r, SfAttrs *a) {
    memset(a, 0, sizeof *a);
    if (mxr_u32(r, &a->flags)) return -1;
    if ((a->flags & SSH_FILEXFER_ATTR_SIZE) && mxr_u64(r, &a->size)) return -1;
    if ((a->flags & SSH_FILEXFER_ATTR_UIDGID) && (mxr_u32(r, &a->uid) || mxr_u32(r, &a->gid))) return -1;
    if ((a->flags & SSH_FILEXFER_ATTR_PERMISSIONS) && mxr_u32(r, &a->perm)) return -1;
    if ((a->flags & SSH_FILEXFER_ATTR_ACMODTIME) && (mxr_u32(r, &a->atime) || mxr_u32(r, &a->mtime))) return -1;
    if (a->flags & SSH_FILEXFER_ATTR_EXTENDED) {
        uint32_t n;
        if (mxr_u32(r, &n) || n > 64) return -1;
        for (uint32_t i = 0; i < n; i++) {
            const char *s;
            size_t k;
            if (mxr_str(r, &s, &k) || mxr_str(r, &s, &k)) return -1;
        }
    }
    return 0;
}

void sf_put_attrs(MuxBuf *b, const SfAttrs *a) {
    uint32_t f = a ? a->flags & ~SSH_FILEXFER_ATTR_EXTENDED : 0;
    mxb_u32(b, f);
    if (!a) return;
    if (f & SSH_FILEXFER_ATTR_SIZE) mxb_u64(b, a->size);
    if (f & SSH_FILEXFER_ATTR_UIDGID) { mxb_u32(b, a->uid); mxb_u32(b, a->gid); }
    if (f & SSH_FILEXFER_ATTR_PERMISSIONS) mxb_u32(b, a->perm);
    if (f & SSH_FILEXFER_ATTR_ACMODTIME) { mxb_u32(b, a->atime); mxb_u32(b, a->mtime); }
}

static void head(MuxBuf *b, int type, uint32_t id) {
    mxb_reset(b);
    mxb_u8(b, (uint8_t)type);
    mxb_u32(b, id);
}

void sf_req_path(MuxBuf *b, int type, uint32_t id, const char *path) { head(b, type, id); mxb_str(b, path, strlen(path)); }
void sf_req_handle(MuxBuf *b, int type, uint32_t id, const char *h, size_t hn) { head(b, type, id); mxb_str(b, h, hn); }

void sf_req_open(MuxBuf *b, uint32_t id, const char *path, uint32_t pflags, const SfAttrs *a) {
    head(b, SSH_FXP_OPEN, id);
    mxb_str(b, path, strlen(path));
    mxb_u32(b, pflags);
    sf_put_attrs(b, a);
}

void sf_req_read(MuxBuf *b, uint32_t id, const char *h, size_t hn, uint64_t off, uint32_t len) {
    head(b, SSH_FXP_READ, id);
    mxb_str(b, h, hn);
    mxb_u64(b, off);
    mxb_u32(b, len);
}

void sf_req_write(MuxBuf *b, uint32_t id, const char *h, size_t hn, uint64_t off, const void *data, uint32_t len) {
    head(b, SSH_FXP_WRITE, id);
    mxb_str(b, h, hn);
    mxb_u64(b, off);
    mxb_str(b, data, len);
}

void sf_req_mkdir(MuxBuf *b, uint32_t id, const char *path, uint32_t perm) {
    head(b, SSH_FXP_MKDIR, id);
    mxb_str(b, path, strlen(path));
    SfAttrs a = { .flags = SSH_FILEXFER_ATTR_PERMISSIONS, .perm = perm };
    sf_put_attrs(b, &a);
}

void sf_req_rename(MuxBuf *b, uint32_t id, const char *from, const char *to) {
    head(b, SSH_FXP_RENAME, id);
    mxb_str(b, from, strlen(from));
    mxb_str(b, to, strlen(to));
}

void sf_req_setstat(MuxBuf *b, uint32_t id, const char *path, const SfAttrs *a) {
    head(b, SSH_FXP_SETSTAT, id);
    mxb_str(b, path, strlen(path));
    sf_put_attrs(b, a);
}

int sf_reply_head(MuxRd *r, int *type, uint32_t *id) {
    if (r->n - r->off < 1) return -1;
    *type = r->p[r->off++];
    return mxr_u32(r, id);
}

int sf_status(MuxRd *r, uint32_t *code, char *msg, size_t cap) {
    const char *s = "";
    size_t n = 0;
    if (mxr_u32(r, code)) return -1;
    if (mxr_str(r, &s, &n)) n = 0;             /* version 3 servers may leave the message out */
    size_t k = 0;
    for (size_t i = 0; i < n && k + 1 < cap; i++) msg[k++] = (unsigned char)s[i] < 0x20 || (unsigned char)s[i] >= 0x7f ? ' ' : s[i];
    if (cap) msg[k] = 0;
    return 0;
}

char sf_kind(uint32_t perm) {
    switch (perm & 0170000) {
    case 0040000: return 'd';
    case 0120000: return 'l';
    case 0100000: return '-';
    default: return '?';
    }
}

/* a field of the long name ("-rw-r--r--  1 pi www-data 12 Nov 9 17:31 x"), if it looks like a name */
static void long_field(const char *s, size_t n, int which, char *out, size_t cap) {
    out[0] = 0;
    size_t i = 0;
    for (int f = 0; f <= which; f++) {
        while (i < n && s[i] == ' ') i++;
        size_t st = i;
        while (i < n && s[i] != ' ') i++;
        if (f < which) continue;
        size_t len = i - st;
        if (!len || len >= cap || len > 32) return;
        for (size_t k = 0; k < len; k++) {
            char c = s[st + k];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return;
        }
        memcpy(out, s + st, len);
        out[len] = 0;
    }
}

int sf_name_entry(MuxRd *r, SfEntry *e) {
    const char *nm, *ln;
    size_t nn, lnn;
    if (mxr_str(r, &nm, &nn) || mxr_str(r, &ln, &lnn) || sf_get_attrs(r, &e->a)) return -1;
    e->owner[0] = e->group[0] = 0;
    e->kind = e->a.flags & SSH_FILEXFER_ATTR_PERMISSIONS ? sf_kind(e->a.perm) : '?';
    if (!nn || nn > SFTP_NAME_MAX) return -2;
    for (size_t i = 0; i < nn; i++) if (nm[i] == '/' || (unsigned char)nm[i] < 0x20 || nm[i] == 0x7f) return -2;
    memcpy(e->name, nm, nn);
    e->name[nn] = 0;
    long_field(ln, lnn, 2, e->owner, sizeof e->owner);
    long_field(ln, lnn, 3, e->group, sizeof e->group);
    return 0;
}

int sf_path_ok(const char *p) {
    if (!p || p[0] != '/') return 0;
    size_t n = strlen(p);
    if (n > SFTP_NAME_MAX) return 0;
    for (size_t i = 0; i < n; i++) if ((unsigned char)p[i] < 0x20 || p[i] == 0x7f) return 0;
    return 1;
}
