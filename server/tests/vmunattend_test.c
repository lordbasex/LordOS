/* vmunattend_test.c — unattended installs (src/vm/vmunattend.c): the spec and what it refuses, the three answer files
 * (the password only as its hash), the kernel command line, the FAT seed disk read back (label, long names, data; and
 * fsck.fat when it's there), the cpio after an initrd, ISO 9660 paths (Rock Ridge, plain names, Joliet) in images made
 * here, broken images, and vmu_prepare end to end. ASan and UBSan.
 *
 *   build/vmunattend-test */
#include "../src/vm/vmunattend.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int pass, fail;
#define T(name, cond) do { if (cond) pass++; else { fail++; printf("FAIL  %s\n", name); } } while (0)

#define HASH "$6$rounds=5000$saltsalt$Dq0rRrJfxHPNvJ7n2XhPgYV5L8w6k1bW6bW6Zf6n0eA6z6QmF4q1G9G4fQ1m1o2Yx4a8o1o2Yx4a8o1o2Yx4a8"

/* the spec of this kind; `over` ("\"key\":value", or NULL) replaces that key's value (our JSON refuses a key twice) */
static const char *spec(const char *kind, const char *over) {
    static char b[2400];
    const char *keys[][2] = {
        { "kind", NULL }, { "user", "\"ana\"" }, { "fullname", "\"Ana Pérez\"" }, { "hash", "\"" HASH "\"" }, { "hostname", "\"debian-13\"" },
        { "timezone", "\"America/Argentina/Buenos_Aires\"" }, { "keymap", "\"latam\"" }, { "variant", "\"\"" }, { "locale", "\"C.UTF-8\"" },
        { "release", "\"45\"" }, { "arch", "\"amd64\"" }, { "kernel", "\"install.amd/vmlinuz\"" }, { "initrd", "\"install.amd/initrd.gz\"" },
        { "append", "\"auto=true priority=critical inst.stage2=hd:LABEL={label} --- quiet\"" },
    };
    char kq[64];
    snprintf(kq, sizeof kq, "\"%s\"", kind);
    keys[0][1] = kq;
    size_t n = (size_t)snprintf(b, sizeof b, "{");
    int extra = 1;
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        char k[40];
        snprintf(k, sizeof k, "\"%s\":", keys[i][0]);
        const char *v = keys[i][1];
        if (over && !strncmp(over, k, strlen(k))) { v = over + strlen(k); extra = 0; }
        n += (size_t)snprintf(b + n, sizeof b - n, "%s%s%s", i ? "," : "", k, v);
    }
    if (over && extra) n += (size_t)snprintf(b + n, sizeof b - n, ",%s", over);
    snprintf(b + n, sizeof b - n, "}");
    return b;
}

/* ---- a small ISO 9660 image made here ---- */

typedef struct { uint8_t *d; size_t n; } Img;
static void put_le_be32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) { p[i] = (uint8_t)(v >> (8 * i)); p[7 - i] = (uint8_t)(v >> (8 * i)); } }
static void put_le_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
/* a directory record; rr: a Rock Ridge NM name (or NULL); joliet: the name in UCS-2 */
static size_t rec(uint8_t *p, uint32_t lba, uint32_t size, int dir, const char *name, const char *rr, int joliet) {
    size_t nl = joliet ? strlen(name) * 2 : strlen(name);
    if (!strcmp(name, "\x00") || !strcmp(name, "\x01")) nl = 1;
    size_t len = 33 + nl + (nl % 2 ? 0 : 1);
    if (rr) len += 5 + strlen(rr);
    if (len % 2) len++;
    memset(p, 0, len);
    p[0] = (uint8_t)len;
    put_le_be32(p + 2, lba);
    put_le_be32(p + 10, size);
    p[25] = dir ? 2 : 0;
    put_le_be16(p + 28, 1);
    p[32] = (uint8_t)nl;
    if (joliet) for (size_t i = 0; i < strlen(name); i++) { p[33 + 2 * i] = 0; p[34 + 2 * i] = (uint8_t)name[i]; }
    else memcpy(p + 33, name, nl);
    if (rr) {
        uint8_t *e = p + 33 + nl + (nl % 2 ? 0 : 1);
        e[0] = 'N'; e[1] = 'M'; e[2] = (uint8_t)(5 + strlen(rr)); e[3] = 1; e[4] = 0;
        memcpy(e + 5, rr, strlen(rr));
    }
    return len;
}
/* sectors: 16 PVD, 17 Joliet SVD, 18 terminator, 20 root, 21 install.amd, 22 joliet root, 23 joliet dir, 24.. files */
static Img make_iso(int rr, int joliet_only, const char *label) {
    Img m;
    m.n = 40 * 2048;
    m.d = calloc(1, m.n);
    uint8_t *pvd = m.d + 16 * 2048;
    pvd[0] = joliet_only ? 1 : 1; memcpy(pvd + 1, "CD001", 5); pvd[6] = 1;
    memset(pvd + 40, ' ', 32); memcpy(pvd + 40, label, strlen(label));
    rec(pvd + 156, joliet_only ? 30 : 20, 2048, 1, "\x00", NULL, 0);
    uint8_t *svd = m.d + 17 * 2048;
    svd[0] = 2; memcpy(svd + 1, "CD001", 5); svd[6] = 1; svd[88] = 0x25; svd[89] = 0x2F; svd[90] = 0x45;
    rec(svd + 156, 22, 2048, 1, "\x00", NULL, 0);
    uint8_t *term = m.d + 18 * 2048;
    term[0] = 255; memcpy(term + 1, "CD001", 5);
    /* files: kernel at 24 (5000 bytes), initrd at 28 (6000 bytes) */
    for (size_t i = 0; i < 5000; i++) m.d[24 * 2048 + i] = (uint8_t)('K' + i % 7);
    for (size_t i = 0; i < 6000; i++) m.d[28 * 2048 + i] = (uint8_t)('I' + i % 5);
    uint8_t *root = m.d + 20 * 2048, *sub = m.d + 21 * 2048;
    size_t o = rec(root, 20, 2048, 1, "\x00", NULL, 0);
    o += rec(root + o, 20, 2048, 1, "\x01", NULL, 0);
    o += rec(root + o, 21, 2048, 1, rr ? "INSTALL_AMD" : "INSTALL.AMD", rr ? "install.amd" : NULL, 0);
    o = rec(sub, 21, 2048, 1, "\x00", NULL, 0);
    o += rec(sub + o, 20, 2048, 1, "\x01", NULL, 0);
    o += rec(sub + o, 24, 5000, 0, "VMLINUZ.;1", rr ? "vmlinuz" : NULL, 0);
    o += rec(sub + o, 28, 6000, 0, rr ? "INITRD.GZ;1" : "INITRD.GZ;1", rr ? "initrd.gz" : NULL, 0);
    /* Joliet tree */
    uint8_t *jr = m.d + 22 * 2048, *js = m.d + 23 * 2048;
    o = rec(jr, 22, 2048, 1, "\x00", NULL, 0);
    o += rec(jr + o, 22, 2048, 1, "\x01", NULL, 0);
    o += rec(jr + o, 23, 2048, 1, "install.amd", NULL, 1);
    o = rec(js, 23, 2048, 1, "\x00", NULL, 0);
    o += rec(js + o, 22, 2048, 1, "\x01", NULL, 0);
    o += rec(js + o, 24, 5000, 0, "vmlinuz", NULL, 1);
    o += rec(js + o, 28, 6000, 0, "initrd.gz", NULL, 1);
    if (joliet_only) { uint8_t *e = m.d + 30 * 2048; rec(e, 30, 2048, 1, "\x00", NULL, 0); }   /* an empty primary tree */
    return m;
}
static int to_fd(const Img *m, const char *path) {
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || write(fd, m->d, m->n) != (ssize_t)m->n) return -1;
    return fd;
}

/* ---- reading a FAT12 image back ---- */

static unsigned fat12_get(const uint8_t *fat, unsigned n) {
    unsigned off = n * 3 / 2, v = fat[off] | fat[off + 1] << 8;
    return n & 1 ? v >> 4 : v & 0xFFF;
}
/* the file with this long name: its data (pointer into img) and size, or NULL */
static const uint8_t *fat_file(const uint8_t *img, const char *want, size_t *size) {
    const uint8_t *fat = img + 512, *root = img + 512 * 25;
    char lfn[256] = "";
    for (int e = 0; e < 64; e++) {
        const uint8_t *r = root + 32 * e;
        if (!r[0]) break;
        if (r[11] == 0x0F) {
            int seq = r[0] & 0x1F;
            static const int pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
            if (r[0] & 0x40) memset(lfn, 0, sizeof lfn);
            for (int c = 0; c < 13; c++) { unsigned ch = r[pos[c]] | r[pos[c] + 1] << 8; if (ch && ch != 0xFFFF) lfn[(seq - 1) * 13 + c] = (char)ch; }
            continue;
        }
        if (r[11] & 0x08) continue;
        if (!strcmp(lfn, want)) {
            unsigned cl = r[26] | r[27] << 8;
            *size = (size_t)(r[28] | r[29] << 8 | r[30] << 16 | (size_t)r[31] << 24);
            /* the chain must be contiguous here and end where the size says */
            size_t need = (*size + 511) / 512;
            for (size_t k = 0; k < need; k++) if (fat12_get(fat, cl + (unsigned)k) != (k + 1 == need ? 0xFFF : cl + (unsigned)k + 1)) return NULL;
            return img + 512 * (29 + cl - 2);
        }
        lfn[0] = 0;
    }
    return NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    VmUnattend u;
    char err[200];
    const char *s = spec("preseed", NULL);
    T("spec: good", !vmu_parse(s, strlen(s), &u, err, sizeof err) && u.kind == VMU_PRESEED && u.desktop == 1);
    T("spec: C.UTF-8 becomes en_US.UTF-8", !strcmp(u.locale, "en_US.UTF-8"));
    static const char *const bad[][2] = {
        { "user", "\"user\":\"root\"" }, { "user", "\"user\":\"Ana\"" }, { "user", "\"user\":\"a b\"" }, { "user", "\"user\":\"1ana\"" },
        { "fullname", "\"fullname\":\"Ana \\\"x\\\"\"" }, { "fullname", "\"fullname\":\"a\\nb\"" },
        { "hash", "\"hash\":\"plainpassword1234567890\"" }, { "hash", "\"hash\":\"$6$a b$c\"" },
        { "hostname", "\"hostname\":\"-x\"" }, { "hostname", "\"hostname\":\"a.b\"" }, { "timezone", "\"timezone\":\"../etc\"" },
        { "kernel", "\"kernel\":\"/install.amd/vmlinuz\"" }, { "kernel", "\"kernel\":\"../x\"" }, { "initrd", "\"initrd\":\"a//b\"" },
        { "append", "\"append\":\"a; rm -rf /\"" }, { "append", "\"append\":\"a\\nb\"" }, { "append", "\"append\":\"$(x)\"" },
        { "arch", "\"arch\":\"riscv64\"" }, { "kind", "\"kind\":\"windows\"" },
        { "user", "\"user\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"" }, { "keymap", "\"keymap\":\"la tam\"" },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        char b[2400], lab[96];
        snprintf(b, sizeof b, "%s", spec("preseed", bad[i][1]));
        snprintf(lab, sizeof lab, "spec refused: %s %zu", bad[i][0], i);
        VmUnattend v;
        T(lab, vmu_parse(b, strlen(b), &v, err, sizeof err) && !v.kind);
    }
    T("spec: not JSON", vmu_parse("{x", 2, &u, err, sizeof err));
    s = spec("kickstart", "\"release\":\"\"");
    T("spec: kickstart needs a release", vmu_parse(s, strlen(s), &u, err, sizeof err));

    /* the answer files */
    const char *nm;
    s = spec("preseed", NULL);
    vmu_parse(s, strlen(s), &u, err, sizeof err);
    char *a = vmu_answer(&u, &nm);
    T("preseed: name", a && !strcmp(nm, "preseed.cfg"));
    T("preseed: account and hash", a && strstr(a, "d-i passwd/username string ana\n") && strstr(a, "d-i passwd/user-password-crypted password " HASH "\n"));
    T("preseed: no plain password question", a && !strstr(a, "passwd/user-password password") && !strstr(a, "user-password-again"));
    T("preseed: desktop, agent, zone, keyboard", a && strstr(a, "gnome-desktop") && strstr(a, "qemu-guest-agent") &&
      strstr(a, "time/zone string America/Argentina/Buenos_Aires") && strstr(a, "xkb-keymap select latam") && strstr(a, "Ana Pérez"));
    free(a);
    s = spec("preseed", "\"desktop\":false");
    vmu_parse(s, strlen(s), &u, err, sizeof err);
    a = vmu_answer(&u, &nm);
    T("preseed: a console system honours the power button", a && !strstr(a, "gnome-desktop") && strstr(a, "acpid acpi-support-base"));
    free(a);
    s = spec("autoinstall", NULL);
    vmu_parse(s, strlen(s), &u, err, sizeof err);
    a = vmu_answer(&u, &nm);
    T("autoinstall: name and form", a && !strcmp(nm, "user-data") && !strncmp(a, "#cloud-config\n", 14) && strstr(a, "autoinstall:\n  version: 1\n"));
    T("autoinstall: identity with the hash", a && strstr(a, "    username: \"ana\"\n") && strstr(a, "    password: \"" HASH "\"\n") && strstr(a, "hostname: \"debian-13\""));
    T("autoinstall: desktop, agent, reboot", a && strstr(a, "ubuntu-desktop-minimal") && strstr(a, "qemu-guest-agent") && strstr(a, "shutdown: reboot"));
    free(a);
    char *md = vmu_meta_data(&u, "f0add705-5b88-4a90-8745-7fcc7713d19d");
    T("meta-data", md && strstr(md, "instance-id: f0add705-5b88-4a90-8745-7fcc7713d19d\n") && strstr(md, "local-hostname: debian-13\n"));
    free(md);
    md = vmu_meta_data(&u, "bad id\nx");
    T("meta-data: a bad instance id isn't used", md && !strstr(md, "bad id"));
    free(md);
    s = spec("kickstart", NULL);
    vmu_parse(s, strlen(s), &u, err, sizeof err);
    a = vmu_answer(&u, &nm);
    T("kickstart: name", a && !strcmp(nm, "ks.cfg"));
    T("kickstart: user with the hash", a && strstr(a, "user --name=ana --gecos=\"Ana Pérez\" --groups=wheel --iscrypted --password=" HASH "\n") && strstr(a, "rootpw --lock\n"));
    T("kickstart: mirrors, release, arch", a && strstr(a, "repo=fedora-45&arch=x86_64") && strstr(a, "updates-released-f45&arch=x86_64"));
    T("kickstart: workstation, reboot", a && strstr(a, "@^workstation-product-environment") && strstr(a, "\nreboot\n") && strstr(a, "keyboard --xlayouts='latam'"));
    free(a);

    /* the command line */
    char cmd[640];
    T("cmdline: label", !vmu_cmdline(&u, "Fedora-E-dvd-x86_64-45", cmd, sizeof cmd) && !strcmp(cmd, "auto=true priority=critical inst.stage2=hd:LABEL=Fedora-E-dvd-x86_64-45 --- quiet"));
    T("cmdline: a label with a space is refused", vmu_cmdline(&u, "a b", cmd, sizeof cmd));
    T("cmdline: too long", vmu_cmdline(&u, "x", cmd, 20));

    /* the FAT seed disk */
    const char ud[] = "#cloud-config\nautoinstall:\n  version: 1\n";
    static char big[3000];
    memset(big, 'b', sizeof big);
    VmuFile f[3] = { { "user-data", ud, sizeof ud - 1 }, { "meta-data", "instance-id: x\n", 15 }, { "a-very-long-file-name-of-thirty.txt", big, sizeof big } };
    uint8_t *img;
    size_t il;
    T("fat: made", !vmu_fat_image("CIDATA", f, 3, &img, &il) && il == 4096 * 512);
    if (img) {
        size_t sz;
        const uint8_t *p = fat_file(img, "user-data", &sz);
        T("fat: label in the boot sector", !memcmp(img + 43, "CIDATA     ", 11) && img[510] == 0x55 && img[511] == 0xAA);
        T("fat: label in the root", !memcmp(img + 512 * 25, "CIDATA     ", 11) && img[512 * 25 + 11] == 0x08);
        T("fat: user-data by its long name", p && sz == sizeof ud - 1 && !memcmp(p, ud, sz));
        p = fat_file(img, "meta-data", &sz);
        T("fat: meta-data", p && sz == 15 && !memcmp(p, "instance-id: x\n", 15));
        p = fat_file(img, "a-very-long-file-name-of-thirty.txt", &sz);
        T("fat: a name of three long entries, a file of six clusters", p && sz == sizeof big && !memcmp(p, big, sz));
        T("fat: both FATs the same", !memcmp(img + 512, img + 512 * 13, 512 * 12));
        /* fsck.fat, where it's installed */
        if (!access("/usr/sbin/fsck.fat", X_OK)) {
            int fd = open("/tmp/vmunattend-test.img", O_RDWR | O_CREAT | O_TRUNC, 0600);
            int ok = fd >= 0 && write(fd, img, il) == (ssize_t)il;
            if (fd >= 0) close(fd);
            int st = ok ? system("/usr/sbin/fsck.fat -n /tmp/vmunattend-test.img >/dev/null 2>&1") : -1;
            T("fat: fsck.fat finds nothing wrong", ok && WIFEXITED(st) && WEXITSTATUS(st) == 0);
            unlink("/tmp/vmunattend-test.img");
        } else printf("      fsck.fat isn't installed: skipped\n");
        free(img);
    }
    T("fat: a bad label", vmu_fat_image("cidata", f, 1, &img, &il) && vmu_fat_image("TOOLONGLABEL1", f, 1, &img, &il));
    VmuFile slash = { "a/b", "x", 1 };
    T("fat: a name with a slash", vmu_fat_image("OEMDRV", &slash, 1, &img, &il));
    static char huge[1 << 20 | 1];
    VmuFile hf = { "big", huge, sizeof huge };
    T("fat: a file too large", vmu_fat_image("OEMDRV", &hf, 1, &img, &il));

    /* cpio */
    uint8_t *cp;
    size_t cl;
    VmuFile pf = { "preseed.cfg", "d-i x string y\n", 15 };
    T("cpio: made", !vmu_cpio(&pf, 1, &cp, &cl));
    if (cp) {
        T("cpio: newc header, name, data", !memcmp(cp, "070701", 6) && !memcmp(cp + 110, "preseed.cfg", 12) && cl % 4 == 0 &&
          !memcmp(cp + 124, "d-i x string y\n", 15));
        T("cpio: size field", !memcmp(cp + 6 + 6 * 8, "0000000F", 8));
        T("cpio: trailer", memmem(cp, cl, "TRAILER!!!", 10) != NULL);
        free(cp);
    }
    VmuFile pd = { "../x", "", 0 };
    T("cpio: a path is refused", vmu_cpio(&pd, 1, &cp, &cl));

    /* ISO 9660 */
    uint64_t off, sz;
    Img m = make_iso(1, 0, "DEBIAN_13");
    int fd = to_fd(&m, "/tmp/vmunattend-rr.iso");
    char label[33];
    T("iso: label", fd >= 0 && !vmu_iso_label(fd, label) && !strcmp(label, "DEBIAN_13"));
    T("iso: Rock Ridge path", !vmu_iso_find(fd, "install.amd/vmlinuz", &off, &sz) && off == 24 * 2048 && sz == 5000);
    T("iso: Rock Ridge initrd", !vmu_iso_find(fd, "install.amd/initrd.gz", &off, &sz) && off == 28 * 2048 && sz == 6000);
    T("iso: Rock Ridge names are exact", vmu_iso_find(fd, "INSTALL.AMD/VMLINUZ", &off, &sz) != 0);
    T("iso: a missing file", vmu_iso_find(fd, "install.amd/nothing", &off, &sz));
    T("iso: a directory isn't a file", vmu_iso_find(fd, "install.amd", &off, &sz));
    T("iso: a path out", vmu_iso_find(fd, "../etc/passwd", &off, &sz) && vmu_iso_find(fd, "/install.amd/vmlinuz", &off, &sz));
    close(fd);
    free(m.d);
    m = make_iso(0, 0, "PLAIN");
    fd = to_fd(&m, "/tmp/vmunattend-plain.iso");
    T("iso: plain names without ;1, any case", fd >= 0 && !vmu_iso_find(fd, "install.amd/vmlinuz", &off, &sz) && sz == 5000);
    close(fd);
    free(m.d);
    m = make_iso(0, 1, "JOLIET");
    fd = to_fd(&m, "/tmp/vmunattend-joliet.iso");
    T("iso: Joliet when the primary tree lacks it", fd >= 0 && !vmu_iso_find(fd, "install.amd/initrd.gz", &off, &sz) && sz == 6000);
    close(fd);
    /* broken images */
    m.d[22 * 2048 + 68 + 0] = 20;                     /* a record shorter than a record can be */
    fd = to_fd(&m, "/tmp/vmunattend-bad.iso");
    T("iso: a broken record isn't followed", fd >= 0 && vmu_iso_find(fd, "install.amd/initrd.gz", &off, &sz));
    close(fd);
    free(m.d);
    m = make_iso(1, 0, "X");
    put_le_be32(m.d + 20 * 2048 + 68 + 10, 64u << 20);   /* install.amd says it is 64 MiB */
    m.d[17 * 2048] = 3;                                  /* and no Joliet tree to find it in instead */
    fd = to_fd(&m, "/tmp/vmunattend-big.iso");
    T("iso: an oversized directory", fd >= 0 && vmu_iso_find(fd, "install.amd/vmlinuz", &off, &sz));
    close(fd);
    free(m.d);
    m = make_iso(1, 0, "X");
    put_le_be32(m.d + 21 * 2048 + 68 + 2, 100000);      /* vmlinuz past the image's end */
    m.d[17 * 2048] = 3;
    fd = to_fd(&m, "/tmp/vmunattend-past.iso");
    T("iso: a file past the end", fd >= 0 && vmu_iso_find(fd, "install.amd/vmlinuz", &off, &sz));
    close(fd);
    free(m.d);
    T("iso: not an ISO", (fd = open("/tmp/vmunattend-none", O_RDWR | O_CREAT | O_TRUNC, 0600)) >= 0 && vmu_iso_label(fd, label) &&
      vmu_iso_find(fd, "install.amd/vmlinuz", &off, &sz));
    close(fd);

    /* vmu_prepare end to end on the Rock Ridge image */
    char dir[] = "/tmp/vmunattend-disks-XXXXXX";
    T("prepare: temp folder", mkdtemp(dir) != NULL);
    s = spec("preseed", "\"append\":\"auto=true priority=critical --- quiet\"");
    vmu_parse(s, strlen(s), &u, err, sizeof err);
    int pr = vmu_prepare(dir, "/tmp/vmunattend-rr.iso", &u, "id-1", err, sizeof err);
    T("prepare: preseed", !pr);
    if (pr) printf("      %s\n", err);
    char p[600];
    struct stat st;
    snprintf(p, sizeof p, "%s/unattended/initrd", dir);
    T("prepare: initrd with the answers after it", !stat(p, &st) && st.st_size > 6000 && (st.st_mode & 077) == 0);
    {
        FILE *fi = fopen(p, "rb");
        static char ib[16384];
        size_t got = fi ? fread(ib, 1, sizeof ib, fi) : 0;
        if (fi) fclose(fi);
        T("prepare: initrd = the ISO's, padded, then a cpio with preseed.cfg", got > 6000 && ib[0] == 'I' && !memcmp(ib + 6000, "070701", 6) &&
          memmem(ib, got, "preseed.cfg", 11) && memmem(ib, got, HASH, strlen(HASH)));
    }
    snprintf(p, sizeof p, "%s/unattended/cmdline", dir);
    {
        FILE *fc = fopen(p, "r");
        char line[700] = "";
        if (fc) { if (!fgets(line, sizeof line, fc)) line[0] = 0; fclose(fc); }
        T("prepare: cmdline", !strcmp(line, "auto=true priority=critical --- quiet"));
    }
    snprintf(p, sizeof p, "%s/unattended/seed.img", dir);
    T("prepare: no seed disk for preseed", access(p, F_OK));
    s = spec("autoinstall", "\"append\":\"autoinstall --- quiet\"");
    vmu_parse(s, strlen(s), &u, err, sizeof err);
    T("prepare: autoinstall", !vmu_prepare(dir, "/tmp/vmunattend-rr.iso", &u, "id-2", err, sizeof err));
    T("prepare: a CIDATA seed disk", !stat(p, &st) && st.st_size == 4096 * 512);
    s = spec("kickstart", "\"kernel\":\"install.amd/nothing\"");
    vmu_parse(s, strlen(s), &u, err, sizeof err);
    char sub[600];
    snprintf(sub, sizeof sub, "%s/unattended", dir);
    T("prepare: a kernel that isn't there fails and leaves nothing", vmu_prepare(dir, "/tmp/vmunattend-rr.iso", &u, "id-3", err, sizeof err) &&
      strstr(err, "no installer kernel") && access(sub, F_OK));
    rmdir(dir);
    unlink("/tmp/vmunattend-rr.iso"); unlink("/tmp/vmunattend-plain.iso"); unlink("/tmp/vmunattend-joliet.iso");
    unlink("/tmp/vmunattend-bad.iso"); unlink("/tmp/vmunattend-big.iso"); unlink("/tmp/vmunattend-past.iso"); unlink("/tmp/vmunattend-none");

    printf("== vmunattend-test: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
