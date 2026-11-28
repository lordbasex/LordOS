/* zipread.c — a strict reader of a community theme's .zip (zipread.h). Inflating is stb_image's zlib decoder (raw
 * deflate into a buffer of exactly the size the header says: more is refused). */
#include "zipread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* stb_image's raw-deflate decoder (lord-themec's only use of it; the image decoders are compiled out) */
int stbi_zlib_decode_noheader_buffer(char *obuffer, int olen, const char *ibuffer, int ilen);

#define WHY(...) do { if (why && why_n) snprintf(why, why_n, __VA_ARGS__); } while (0)

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

uint32_t zip_crc32(const uint8_t *p, size_t n) {
    static uint32_t table[256];
    static int ready;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = 1;
    }
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

void zip_free(ZipTheme *z) {
    for (int i = 0; i < z->n; i++) free(z->files[i].data);
    free(z->files);
    memset(z, 0, sizeof *z);
}

static int id_ok(const char *s, size_t n) {
    if (!n || n > 26) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0;
    }
    return 1;
}

/* the entry's name as written: no control characters, "\", leading "/", "C:", empty, "." or ".." segment */
static int name_ok(const char *s, size_t n) {
    if (!n || n > ZIP_NAME_MAX || s[0] == '/') return 0;
    if (n >= 2 && s[1] == ':') return 0;
    size_t seg = 0;
    for (size_t i = 0; i <= n; i++) {
        unsigned char c = i < n ? (unsigned char)s[i] : '/';
        if (i < n && (c < 0x20 || c == 0x7f || c == '\\')) return 0;
        if (c == '/') {
            size_t l = i - seg;
            /* an empty segment is allowed only as a folder entry's trailing "/" */
            if (l == 0 && i != n) return 0;
            if ((l == 1 && s[seg] == '.') || (l == 2 && s[seg] == '.' && s[seg + 1] == '.')) return 0;
            seg = i + 1;
        }
    }
    return 1;
}

int zip_read_theme(const uint8_t *buf, size_t len, ZipTheme *z, char *why, size_t why_n) {
    memset(z, 0, sizeof *z);
    if (len < 22) { WHY("not a .zip file"); return -1; }
    /* the end of central directory record: the last one, within the last 64 KB + 22 bytes */
    size_t eocd = (size_t)-1;
    size_t lo = len > 65557 ? len - 65557 : 0;
    for (size_t i = len - 22 + 1; i-- > lo;) if (rd32(buf + i) == 0x06054b50) { eocd = i; break; }
    if (eocd == (size_t)-1) { WHY("not a .zip file"); return -1; }
    const uint8_t *e = buf + eocd;
    if (rd16(e + 4) || rd16(e + 6) || rd16(e + 8) != rd16(e + 10)) { WHY("multi-part archives are not accepted"); return -1; }
    uint32_t count = rd16(e + 10), cd_size = rd32(e + 12), cd_off = rd32(e + 16);
    if (count == 0xffff || cd_off == 0xffffffffu || cd_size == 0xffffffffu) { WHY("Zip64 archives are not accepted"); return -1; }
    if (!count) { WHY("the archive is empty"); return -1; }
    if (count > ZIP_FILES_MAX) { WHY("more than %d entries", ZIP_FILES_MAX); return -1; }
    if ((size_t)cd_off > eocd || (size_t)cd_size > eocd - cd_off) { WHY("the archive's directory is out of bounds"); return -1; }
    z->files = calloc(count, sizeof *z->files);
    if (!z->files) { WHY("out of memory"); return -1; }
    size_t pos = cd_off, total = 0;
    for (uint32_t k = 0; k < count; k++) {
        if (pos + 46 > (size_t)cd_off + cd_size || rd32(buf + pos) != 0x02014b50) { WHY("a bad directory entry"); goto bad; }
        const uint8_t *c = buf + pos;
        uint16_t made_by = rd16(c + 4), flags = rd16(c + 8), method = rd16(c + 10);
        uint32_t crc = rd32(c + 16), csize = rd32(c + 20), usize = rd32(c + 24);
        uint16_t nlen = rd16(c + 28), xlen = rd16(c + 30), clen = rd16(c + 32);
        uint32_t ext = rd32(c + 38), loff = rd32(c + 42);
        if (pos + 46 + (size_t)nlen + xlen + clen > (size_t)cd_off + cd_size) { WHY("a bad directory entry"); goto bad; }
        const char *name = (const char *)c + 46;
        pos += 46 + (size_t)nlen + xlen + clen;
        if (memchr(name, 0, nlen) || !name_ok(name, nlen)) { WHY("an entry's name is not allowed (\"..\", an absolute path or odd characters)"); goto bad; }
        if (flags & 0x0001) { WHY("encrypted entries are not accepted"); goto bad; }
        int is_dir = name[nlen - 1] == '/';
        /* the Unix file type, when the archive was made on Unix: only folders and regular files */
        if ((made_by >> 8) == 3) {
            unsigned type = (ext >> 16) & 0170000;
            if (type && type != 0100000 && type != 0040000) { WHY("links and special files are not accepted"); goto bad; }
            if (type == 0040000 && !is_dir) { WHY("a folder entry without its \"/\""); goto bad; }
        }
        /* one folder at the top: the theme's id */
        const char *slash = memchr(name, '/', nlen);
        if (!slash) { WHY("every file must be inside one folder named after the theme"); goto bad; }
        size_t idl = (size_t)(slash - name);
        if (!id_ok(name, idl)) { WHY("the archive's folder is not a theme id (letters, digits, - and _, at most 26)"); goto bad; }
        if (!z->id[0]) { memcpy(z->id, name, idl); z->id[idl] = 0; }
        else if (strlen(z->id) != idl || memcmp(z->id, name, idl)) { WHY("the archive holds more than one folder at the top"); goto bad; }
        size_t rl = nlen - idl - 1;
        if (is_dir) { if (usize || csize) { WHY("a folder entry with data"); goto bad; } continue; }
        if (!rl) { WHY("a bad entry"); goto bad; }
        for (int i = 0; i < z->n; i++) if (strlen(z->files[i].name) == rl && !memcmp(z->files[i].name, slash + 1, rl)) { WHY("the archive holds a file twice"); goto bad; }
        if (method != 0 && method != 8) { WHY("only stored or deflated entries are accepted"); goto bad; }
        if (usize > ZIP_FILE_MAX) { WHY("a file is larger than %u bytes", ZIP_FILE_MAX); goto bad; }
        total += usize;
        if (total > ZIP_TOTAL_MAX) { WHY("more than %u bytes in all", ZIP_TOTAL_MAX); goto bad; }
        if (method == 0 && csize != usize) { WHY("a stored entry's sizes disagree"); goto bad; }
        /* the local header: same name, data within the file */
        if ((size_t)loff + 30 > cd_off || rd32(buf + loff) != 0x04034b50) { WHY("a bad local header"); goto bad; }
        const uint8_t *l = buf + loff;
        uint16_t lnlen = rd16(l + 26), lxlen = rd16(l + 28);
        if (rd16(l + 8) != method || lnlen != nlen || memcmp(l + 30, name, nlen)) { WHY("local and central headers disagree"); goto bad; }
        size_t doff = (size_t)loff + 30 + lnlen + lxlen;
        if (doff > cd_off || csize > cd_off - doff) { WHY("an entry's data is out of bounds"); goto bad; }
        uint8_t *out = malloc(usize ? usize : 1);
        if (!out) { WHY("out of memory"); goto bad; }
        if (method == 0) memcpy(out, buf + doff, usize);
        else if (csize > 0x7fffffffu || usize > 0x7fffffffu ||
                 stbi_zlib_decode_noheader_buffer((char *)out, (int)usize, (const char *)buf + doff, (int)csize) != (int)usize) {
            free(out);
            WHY("an entry doesn't inflate to its size");
            goto bad;
        }
        if (zip_crc32(out, usize) != crc) { free(out); WHY("an entry's CRC-32 doesn't match"); goto bad; }
        ZipFile *f = &z->files[z->n++];
        memcpy(f->name, slash + 1, rl);
        f->name[rl] = 0;
        f->data = out;
        f->size = usize;
    }
    if (!z->n) { WHY("the archive holds no files"); goto bad; }
    return 0;
bad:
    zip_free(z);
    return -1;
}
