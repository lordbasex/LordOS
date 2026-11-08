/* vmimport.c — the parsers of vmimport.h. No file access: the caller reads (bounded) and passes the text.
 *
 * How each source's SMBIOS system UUID reaches its guest (vmi_guest_uuid's rules, docs/VIRTUALIZATION.md 6.22):
 * an app writes 16 bytes into its SMBIOS type 1 table and states an SMBIOS version. The guest decodes them: Linux
 * (dmidecode, /sys/class/dmi/id/product_uuid) swaps the first three fields when the table says 2.6 or later and not
 * before; Windows always reads them little-endian. QEMU writes the UUID it is given little-endian in a table of a newer
 * version, so both kinds of guest read back exactly the -uuid string. Hence: lord-vm's -uuid is the string the guest
 * read on the source.
 *   - VMware-style .vmx: the bytes are uuid.bios in the order written. The table is 2.7 with acpi.smbiosVersion2.7 =
 *     "TRUE", or, when the key is absent, for virtualHW.version 13 and later; 2.4 before (Reported: the vendor's
 *     knowledge base, Red Hat's article on virt-who). So Windows reads it swapped, and Linux swapped on 2.7 only.
 *   - VirtualBox .vbox: the bytes are the hardware UUID (<Hardware uuid=...>, else the machine's uuid), in network
 *     order unless <SmbiosUuidLittleEndian enabled="true"/>; a DmiSystemUuid extra-data value is always written in
 *     network order. The table is 2.5. (From its source: DevPcBios.cpp, DevFwCommon.cpp, Settings.cpp.) So Linux reads
 *     the string as written in network order, swapped when little-endian; Windows the other way round.
 *   - Parallels-style config.pvs: SystemUuid (else VmUuid), assumed written as the SMBIOS spec says, so every guest
 *     reads it as written (**To test**: no such app or guest was available).
 * A VirtualBox SATA or IDE disk's serial, when the source doesn't set one, is "VB%08x-%08x" of the base image's
 * UUID's first and last 32-bit words in memory order (DevAHCI.cpp, DevATA.cpp). */
#include "vmimport.h"
#include "vmxml.h"
#include "machine.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int fail(char *err, size_t n, const char *m) { if (err && n) snprintf(err, n, "%s", m); return -1; }

static void copy(char *dst, size_t cap, const char *src) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)(src ? src : ""); *p && o + 1 < cap; p++) dst[o++] = *p < 0x20 || *p == 0x7f ? ' ' : (char)*p;
    dst[o] = 0;
}

/* printable ASCII for an SMBIOS string (QEMU's option, a guest's table): others become '?' */
static void smb(char *dst, const char *src) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)(src ? src : ""); *p && o + 1 < VMI_STR; p++) dst[o++] = *p >= 0x20 && *p < 0x7f ? (char)*p : '?';
    while (o && dst[o - 1] == ' ') o--;
    dst[o] = 0;
}

static long long num(const char *s, long long def) {
    if (!s) return def;
    while (*s == ' ') s++;
    if (!*s) return def;
    char *e;
    long long v = strtoll(s, &e, 0);
    while (*e == ' ') e++;
    return *e || v < 0 ? def : v;
}

static int is_true(const char *s) { return s && (!strcasecmp(s, "true") || !strcasecmp(s, "yes") || !strcmp(s, "1")); }

/* ---- small helpers ---- */

int vmi_rel_ok(const char *s) {
    if (!s || !s[0] || strlen(s) >= VMI_PATH || s[0] == '/' || s[0] == '~') return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) if (*p < 0x20 || *p == 0x7f || *p == '\\' || *p == ':') return 0;
    /* no ".." component, no "." component, no empty component */
    const char *c = s;
    for (;;) {
        const char *sl = strchr(c, '/');
        size_t l = sl ? (size_t)(sl - c) : strlen(c);
        if (!l || (l == 1 && c[0] == '.') || (l == 2 && c[0] == '.' && c[1] == '.')) return 0;
        if (!sl) break;
        c = sl + 1;
    }
    return 1;
}

static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1; }

int vmi_mac(const char *in, char out[18]) {
    if (!in) return -1;
    uint8_t b[6];
    int n = 0, h = -1;
    for (const char *p = in; *p; p++) {
        if (*p == ':' || *p == '-' || *p == ' ' || *p == '.') { if (h >= 0) return -1; continue; }
        int v = hexv((unsigned char)*p);
        if (v < 0) return -1;
        if (h < 0) h = v;
        else { if (n >= 6) return -1; b[n++] = (uint8_t)(h << 4 | v); h = -1; }
    }
    if (n != 6 || h >= 0) return -1;
    if (b[0] & 1) return -1;                                  /* multicast */
    if (!(b[0] | b[1] | b[2] | b[3] | b[4] | b[5])) return -1;
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1], b[2], b[3], b[4], b[5]);
    return 0;
}

int vmi_uuid_bytes(const char *in, uint8_t out[16]) {
    if (!in) return -1;
    int n = 0, h = -1;
    const char *p = in;
    while (*p == ' ') p++;
    int brace = *p == '{';
    if (brace) p++;
    for (; *p && *p != '}'; p++) {
        if (*p == '-' || *p == ' ') { if (h >= 0) return -1; continue; }
        int v = hexv((unsigned char)*p);
        if (v < 0) return -1;
        if (h < 0) h = v;
        else { if (n >= 16) return -1; out[n++] = (uint8_t)(h << 4 | v); h = -1; }
    }
    if (brace != (*p == '}')) return -1;
    if (*p == '}') { p++; while (*p == ' ') p++; if (*p) return -1; }
    return n == 16 && h < 0 ? 0 : -1;
}

void vmi_uuid_str(const uint8_t b[16], char out[37]) {
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3], b[4], b[5],
             b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

void vmi_uuid_swap(uint8_t b[16]) {
    uint8_t t;
    t = b[0]; b[0] = b[3]; b[3] = t; t = b[1]; b[1] = b[2]; b[2] = t;
    t = b[4]; b[4] = b[5]; b[5] = t;
    t = b[6]; b[6] = b[7]; b[7] = t;
}

static int all_zero(const uint8_t b[16]) { for (int i = 0; i < 16; i++) if (b[i]) return 0; return 1; }

/* the guest's reading of 16 SMBIOS bytes: little-endian (Windows always; others from SMBIOS 2.6), or as stored */
static void guest_reads(const uint8_t raw[16], int little, char out[37]) {
    uint8_t b[16];
    memcpy(b, raw, 16);
    if (little) vmi_uuid_swap(b);
    vmi_uuid_str(b, out);
}

static void guest_os(VmiMachine *m, const char *s) {
    copy(m->guest_os, sizeof m->guest_os, s);
    char low[VMI_STR];
    size_t i = 0;
    for (; m->guest_os[i] && i + 1 < sizeof low; i++) low[i] = (char)tolower((unsigned char)m->guest_os[i]);
    low[i] = 0;
    m->windows = !strncmp(low, "win", 3) || strstr(low, "windows") != NULL;
    if (strstr(low, "arm64") || strstr(low, "aarch64") || !strncmp(low, "arm-", 4) || strstr(low, "_arm")) m->arch = VM_ARCH_ARM64;
}

static VmiNic *nic_add(VmiMachine *m) {
    if (m->nnic >= VMI_MAX_NICS) { m->more_nics++; return NULL; }
    VmiNic *n = &m->nic[m->nnic++];
    memset(n, 0, sizeof *n);
    return n;
}

static void nic_model(VmiNic *n, const char *src) {
    copy(n->src_model, sizeof n->src_model, src);
    static const struct { const char *from, *to; } MAP[] = {
        /* .vmx virtualDev, OVF ResourceSubType */
        { "e1000e", "e1000e" }, { "e1000", "e1000" }, { "vmxnet3", "vmxnet3" }, { "vlance", "pcnet" }, { "pcnet32", "pcnet" },
        { "virtio", "virtio" }, { "rtl8139", "rtl8139" },
        /* .vbox Adapter type */
        { "82540EM", "e1000" }, { "82545EM", "e1000-82545em" }, { "82543GC", "e1000-82544gc" }, { "Am79C970A", "pcnet" },
        { "Am79C973", "pcnet" }, { "Am79C960", "pcnet" },
    };
    n->model[0] = 0;
    for (size_t i = 0; src && i < sizeof MAP / sizeof MAP[0]; i++) if (!strcasecmp(src, MAP[i].from)) { snprintf(n->model, sizeof n->model, "%s", MAP[i].to); return; }
}

static VmiDisk *disk_add(VmiMachine *m) {
    if (m->ndisk >= VMI_MAX_DISKS) { m->more_disks++; return NULL; }
    VmiDisk *d = &m->disk[m->ndisk++];
    memset(d, 0, sizeof *d);
    return d;
}

static void defaults(VmiMachine *m, int kind) {
    memset(m, 0, sizeof *m);
    m->kind = kind;
    m->arch = -1;
    m->firmware = VM_FW_BIOS;
    m->vcpus = 1;
}

/* sockets × cores × threads must make the vCPUs; otherwise the topology is dropped (QEMU's default) */
static void topology(VmiMachine *m) {
    if (m->vcpus < 1) m->vcpus = 1;
    if (m->vcpus > VM_VCPUS_MAX) m->vcpus = VM_VCPUS_MAX;
    if (m->cores < 1 || m->threads < 1 || m->vcpus % (m->cores * m->threads)) { m->sockets = m->cores = m->threads = 0; return; }
    m->sockets = m->vcpus / (m->cores * m->threads);
}

/* ---- .vmx ---- */

/* the value of a .vmx line, quotes removed and |XX escapes decoded; keys are compared without case */
typedef struct { char key[96]; char val[VMI_PATH]; } VmxKV;

static int vmx_lines(const char *t, size_t len, VmxKV **out, int *n) {
    int cap = 0;
    *out = NULL; *n = 0;
    size_t i = 0;
    while (i < len) {
        size_t e = i;
        while (e < len && t[e] != '\n') e++;
        size_t a = i, b = e;
        i = e + 1;
        while (a < b && (t[a] == ' ' || t[a] == '\t' || t[a] == '\r')) a++;
        while (b > a && (t[b - 1] == ' ' || t[b - 1] == '\t' || t[b - 1] == '\r')) b--;
        if (a == b || t[a] == '#' || t[a] == '.') continue;
        const char *eq = memchr(t + a, '=', b - a);
        if (!eq) continue;
        size_t ke = (size_t)(eq - t);
        while (ke > a && (t[ke - 1] == ' ' || t[ke - 1] == '\t')) ke--;
        size_t vs = (size_t)(eq - t) + 1;
        while (vs < b && (t[vs] == ' ' || t[vs] == '\t')) vs++;
        if (ke == a || ke - a >= 96) continue;
        if (*n >= 20000) return -1;
        if (*n >= cap) {
            cap = cap ? cap * 2 : 128;
            VmxKV *nw = realloc(*out, sizeof **out * (size_t)cap);
            if (!nw) return -1;
            *out = nw;
        }
        VmxKV *kv = &(*out)[(*n)++];
        for (size_t k = 0; k < ke - a; k++) kv->key[k] = (char)tolower((unsigned char)t[a + k]);
        kv->key[ke - a] = 0;
        size_t ve = b;
        if (vs < ve && t[vs] == '"') { vs++; if (ve > vs && t[ve - 1] == '"') ve--; }
        size_t o = 0;
        for (size_t k = vs; k < ve && o + 1 < sizeof kv->val; k++) {
            if (t[k] == '|' && k + 2 < ve && hexv((unsigned char)t[k + 1]) >= 0 && hexv((unsigned char)t[k + 2]) >= 0) {
                int c = hexv((unsigned char)t[k + 1]) << 4 | hexv((unsigned char)t[k + 2]);
                kv->val[o++] = c >= 0x20 && c != 0x7f ? (char)c : ' ';
                k += 2;
            } else kv->val[o++] = (unsigned char)t[k] < 0x20 ? ' ' : t[k];
        }
        kv->val[o] = 0;
    }
    return 0;
}

static const char *vmx_get(const VmxKV *kv, int n, const char *key) {
    const char *r = NULL;
    for (int i = 0; i < n; i++) if (!strcmp(kv[i].key, key)) r = kv[i].val;          /* the last one wins, as there */
    return r;
}
static const char *vmx_getf(const VmxKV *kv, int n, const char *fmt, const char *a, int b) {
    char k[96];
    (void)b;
    snprintf(k, sizeof k, fmt, a);
    return vmx_get(kv, n, k);
}

int vmi_parse_vmx(const char *text, size_t len, VmiMachine *m, char *err, size_t errlen) {
    defaults(m, VMI_VMX);
    if (len > (1u << 20)) return fail(err, errlen, "the settings file is larger than 1 MB");
    if (memchr(text, 0, len)) return fail(err, errlen, "the settings file isn't text");
    VmxKV *kv;
    int n;
    if (vmx_lines(text, len, &kv, &n)) { free(kv); return fail(err, errlen, "the settings file has too many lines"); }
    /* an encrypted machine keeps only its name and the wrapped key in the clear: it is a .vmx, and what blocks it is the encryption */
    if (!vmx_get(kv, n, "config.version") && !vmx_get(kv, n, "virtualhw.version")) {
        if (vmx_get(kv, n, "encryption.keysafe") || vmx_get(kv, n, "encryption.data")) {
            const char *dn = vmx_get(kv, n, "displayname");
            if (dn) snprintf(m->name, sizeof m->name, "%s", dn);
            m->encrypted = 1;
            free(kv);
            return 0;
        }
        free(kv);
        return fail(err, errlen, "not a .vmx settings file");
    }
    copy(m->name, sizeof m->name, vmx_get(kv, n, "displayname"));
    guest_os(m, vmx_get(kv, n, "guestos"));
    const char *fw = vmx_get(kv, n, "firmware");
    m->firmware = fw && !strcasecmp(fw, "efi") ? VM_FW_UEFI : VM_FW_BIOS;
    m->secure_boot = is_true(vmx_get(kv, n, "uefi.secureboot.enabled"));
    m->vcpus = (int)num(vmx_get(kv, n, "numvcpus"), 1);
    m->cores = (int)num(vmx_get(kv, n, "cpuid.corespersocket"), 1);
    m->threads = 1;
    topology(m);
    m->memory_mb = (int)num(vmx_get(kv, n, "memsize"), 0);
    m->rtc_local = m->windows;
    /* identity */
    uint8_t raw[16];
    int hw = (int)num(vmx_get(kv, n, "virtualhw.version"), 0);
    const char *v27 = vmx_get(kv, n, "acpi.smbiosversion2.7");
    int smbios27 = v27 ? is_true(v27) : hw >= 13;
    if (!vmi_uuid_bytes(vmx_get(kv, n, "uuid.bios"), raw) && !all_zero(raw)) {
        int little = m->windows || smbios27;
        guest_reads(raw, little, m->uuid);
        snprintf(m->uuid_how, sizeof m->uuid_how, "uuid.bios, SMBIOS %s, read %s", smbios27 ? "2.7" : "2.4", little ? "little-endian" : "as stored");
    }
    smb(m->sm_serial, vmx_get(kv, n, "serialnumber"));
    smb(m->sm_product, vmx_get(kv, n, "hw.model"));
    /* network cards: ethernet0 … ethernet9 */
    for (int i = 0; i < 10; i++) {
        char pfx[16];
        snprintf(pfx, sizeof pfx, "ethernet%d", i);
        if (!is_true(vmx_getf(kv, n, "%s.present", pfx, -1))) continue;
        VmiNic *c = nic_add(m);
        if (!c) continue;
        const char *type = vmx_getf(kv, n, "%s.addresstype", pfx, -1);
        const char *a = vmx_getf(kv, n, "%s.address", pfx, -1), *g = vmx_getf(kv, n, "%s.generatedaddress", pfx, -1);
        const char *mac = type && (!strcasecmp(type, "static")) ? a : g ? g : a;
        if (!mac || vmi_mac(mac, c->mac)) c->mac[0] = 0;
        const char *dev = vmx_getf(kv, n, "%s.virtualdev", pfx, -1);
        nic_model(c, dev ? dev : "vlance");
        const char *sc = vmx_getf(kv, n, "%s.startconnected", pfx, -1);
        c->connected = sc ? is_true(sc) : 1;
    }
    /* disks: the boot disk first (bios.hddOrder), then scsi, sata, nvme, ide in their numbering */
    const char *buses[] = { "scsi", "sata", "nvme", "ide" };
    const char *order = vmx_get(kv, n, "bios.hddorder");
    char first[32] = "";
    if (order) { size_t k = 0; while (order[k] && order[k] != ',' && order[k] != ' ' && k + 1 < sizeof first) { first[k] = (char)tolower((unsigned char)order[k]); k++; } first[k] = 0; }
    int uuid_serial = is_true(vmx_get(kv, n, "disk.enableuuid"));
    for (int pass = 0; pass < 2; pass++)
        for (size_t b = 0; b < sizeof buses / sizeof buses[0]; b++)
            for (int c = 0; c < 4; c++)
                for (int u = 0; u < 30; u++) {
                    char dk[32];
                    snprintf(dk, sizeof dk, "%s%d:%d", buses[b], c, u);
                    int is_first = first[0] && !strcmp(dk, first);
                    if ((pass == 0) != is_first) continue;
                    if (!is_true(vmx_getf(kv, n, "%s.present", dk, -1))) continue;
                    const char *fn = vmx_getf(kv, n, "%s.filename", dk, -1), *dt = vmx_getf(kv, n, "%s.devicetype", dk, -1);
                    if (!fn) continue;
                    if (dt && (strstr(dt, "cdrom") || strstr(dt, "floppy"))) { if (fn[0] && strcasecmp(fn, "auto detect")) m->cdrom_files++; continue; }
                    size_t fl = strlen(fn);
                    if (fl < 5 || strcasecmp(fn + fl - 5, ".vmdk")) { if (fl > 4 && !strcasecmp(fn + fl - 4, ".iso")) m->cdrom_files++; continue; }
                    VmiDisk *d = disk_add(m);
                    if (!d) continue;
                    snprintf(d->chain[0].path, VMI_PATH, "%s", fn);
                    d->chain[0].fmt = VMF_VMDK;
                    d->nchain = 1;
                    snprintf(d->bus, sizeof d->bus, "%s", buses[b]);
                    d->boot = m->ndisk == 1;
                    if (uuid_serial) snprintf(d->serial, sizeof d->serial, "ddb.uuid");       /* filled from the descriptor */
                }
    /* what doesn't come along */
    const char *ck = vmx_get(kv, n, "checkpoint.vmstate");
    if (ck && ck[0]) m->saved_state = 1;
    if (is_true(vmx_get(kv, n, "vtpm.present"))) m->tpm = 1;
    if (vmx_get(kv, n, "encryption.keysafe") || vmx_get(kv, n, "encryption.data")) m->encrypted = 1;
    for (int i = 0; i < 64; i++) {
        char k[48];
        snprintf(k, sizeof k, "sharedfolder%d.present", i);
        if (is_true(vmx_get(kv, n, k))) m->shared_folders++;
    }
    free(kv);
    if (m->memory_mb <= 0) return fail(err, errlen, "the settings file doesn't say how much memory the machine has");
    return 0;
}

/* ---- .vbox ---- */

static int vbox_bus(const char *type, char out[8]) {
    static const struct { const char *t, *b; } B[] = {
        { "AHCI", "sata" }, { "PIIX3", "ide" }, { "PIIX4", "ide" }, { "ICH6", "ide" }, { "LsiLogic", "scsi" }, { "BusLogic", "scsi" },
        { "LsiLogicSas", "scsi" }, { "NVMe", "nvme" }, { "VirtioSCSI", "virtio" }, { "USB", "usb" },
    };
    for (size_t i = 0; type && i < sizeof B / sizeof B[0]; i++) if (!strcmp(type, B[i].t)) { snprintf(out, 8, "%s", B[i].b); return 0; }
    snprintf(out, 8, "?");
    return -1;
}

static int vbox_fmt(const char *f) {
    if (!f) return VMF_UNKNOWN;
    if (!strcasecmp(f, "VDI")) return VMF_VDI;
    if (!strcasecmp(f, "VMDK")) return VMF_VMDK;
    if (!strcasecmp(f, "VHD")) return VMF_VPC;
    if (!strcasecmp(f, "VHDX")) return VMF_VHDX;
    if (!strcasecmp(f, "Parallels")) return VMF_PARALLELS;
    if (!strcasecmp(f, "RAW")) return VMF_RAW;
    return VMF_UNKNOWN;
}

/* a HardDisk with this uuid in the media registry (they nest: a child is a differencing image of its parent) */
static const VxNode *vbox_medium(const VxNode *reg, const char *uuid) {
    for (const VxNode *n = reg; n; ) {
        if (!strcmp(n->name, "HardDisk")) { const char *u = vx_attr(n, "uuid"); if (u && !strcasecmp(u, uuid)) return n; }
        if (n->kid) n = n->kid;
        else {
            while (n && n != reg && !n->next) n = n->parent;
            if (!n || n == reg) break;
            n = n->next;
        }
    }
    return NULL;
}

static int count_named(const VxNode *top, const char *name) {
    int c = 0;
    for (const VxNode *n = top ? top->kid : NULL; n; ) {
        if (!strcmp(n->name, name)) c++;
        if (n->kid) n = n->kid;
        else {
            while (n && n != top && !n->next) n = n->parent;
            if (!n || n == top) break;
            n = n->next;
        }
    }
    return c;
}

static const char *extra(const VxNode *mach, const char *name) {
    const VxNode *ed = vx_child(mach, "ExtraData");
    for (const VxNode *i = vx_child(ed, "ExtraDataItem"); i; i = vx_next(i, "ExtraDataItem")) {
        const char *k = vx_attr(i, "name");
        if (k && !strcmp(k, name)) return vx_attr(i, "value");
    }
    return NULL;
}
static const char *dmi(const VxNode *mach, const char *key) {
    char k[96];
    snprintf(k, sizeof k, "VBoxInternal/Devices/pcbios/0/Config/%s", key);
    const char *v = extra(mach, k);
    if (v) return v;
    snprintf(k, sizeof k, "VBoxInternal/Devices/efi/0/Config/%s", key);
    return extra(mach, k);
}

/* the machine element of a .vbox (or of an OVF's vbox:Machine): mach; reg: the media registry, NULL inside an OVF */
static int vbox_machine(const VxNode *mach, VmiMachine *m, int with_disks, char *err, size_t errlen) {
    const VxNode *hw = vx_child(mach, "Hardware");
    if (!hw) return fail(err, errlen, "the machine has no hardware section");
    copy(m->name, sizeof m->name, vx_attr(mach, "name"));
    guest_os(m, vx_attr(mach, "OSType"));
    const VxNode *plat = vx_find(hw, "Platform");
    const char *pa = vx_attr(plat, "architecture");
    if (pa) m->arch = !strcasecmp(pa, "ARM") ? VM_ARCH_ARM64 : VM_ARCH_AMD64;
    const VxNode *cpu = vx_find(hw, "CPU");
    m->vcpus = (int)num(vx_attr(cpu, "count"), 1);
    m->cores = m->vcpus; m->threads = 1;                    /* one socket with that many cores (Reported) */
    topology(m);
    m->memory_mb = (int)num(vx_attr(vx_find(hw, "Memory"), "RAMSize"), 0);
    const char *ft = vx_attr(vx_find(hw, "Firmware"), "type");
    m->firmware = ft && !strncasecmp(ft, "EFI", 3) ? VM_FW_UEFI : VM_FW_BIOS;
    const VxNode *sb = vx_find(hw, "SecureBoot");
    m->secure_boot = sb && is_true(vx_attr(sb, "enabled"));
    const char *rtc = vx_attr(vx_find(hw, "RTC"), "localOrUTC");
    m->rtc_local = !(rtc && !strcasecmp(rtc, "UTC"));      /* VirtualBox's default is local time */
    const VxNode *tpm = vx_find(hw, "TrustedPlatformModule");
    const char *tt = vx_attr(tpm, "type");
    m->tpm = tt && strcasecmp(tt, "None");
    /* identity: DmiSystemUuid (network order), else the hardware uuid, else the machine's */
    uint8_t raw[16];
    const char *over = dmi(mach, "DmiSystemUuid");
    const char *hu = vx_attr(hw, "uuid");
    const char *mu = vx_attr(mach, "uuid");
    int le = 0;
    const VxNode *sle = vx_find(hw, "SmbiosUuidLittleEndian");
    if (sle && is_true(vx_attr(sle, "enabled"))) le = 1;
    const char *src = NULL;
    if (over && !vmi_uuid_bytes(over, raw)) { src = "DmiSystemUuid"; le = 0; }
    else if (hu && !vmi_uuid_bytes(hu, raw)) src = "the hardware uuid";
    else if (mu && !vmi_uuid_bytes(mu, raw)) src = "the machine uuid";
    if (src && !all_zero(raw)) {
        char canon[37];
        vmi_uuid_str(raw, canon);
        if (le) vmi_uuid_swap(raw);                         /* the bytes the table holds */
        /* SMBIOS 2.5: Linux reads the bytes as stored, Windows little-endian */
        guest_reads(raw, m->windows, m->uuid);
        snprintf(m->uuid_how, sizeof m->uuid_how, "%s, %s, SMBIOS 2.5, read %s", src, le ? "little-endian" : "network order",
                 m->windows ? "little-endian" : "as stored");
        const VxNode *asn = vx_find(hw, "AutoSerialNumGen");
        if (asn && is_true(vx_attr(asn, "enabled"))) snprintf(m->sm_serial, sizeof m->sm_serial, "VirtualBox-%s", canon);
    }
    if (dmi(mach, "DmiSystemVendor")) smb(m->sm_manufacturer, dmi(mach, "DmiSystemVendor"));
    if (dmi(mach, "DmiSystemProduct")) smb(m->sm_product, dmi(mach, "DmiSystemProduct"));
    if (dmi(mach, "DmiSystemVersion")) smb(m->sm_version, dmi(mach, "DmiSystemVersion"));
    if (dmi(mach, "DmiSystemSerial")) smb(m->sm_serial, dmi(mach, "DmiSystemSerial"));
    if (dmi(mach, "DmiSystemSKU")) smb(m->sm_sku, dmi(mach, "DmiSystemSKU"));
    if (dmi(mach, "DmiSystemFamily")) smb(m->sm_family, dmi(mach, "DmiSystemFamily"));
    /* network cards, by slot */
    const VxNode *net = vx_find(hw, "Network");
    for (const VxNode *a = vx_child(net, "Adapter"); a; a = vx_next(a, "Adapter")) {
        if (!is_true(vx_attr(a, "enabled"))) continue;
        VmiNic *c = nic_add(m);
        if (!c) continue;
        const char *mac = vx_attr(a, "MACAddress");
        if (!mac || vmi_mac(mac, c->mac)) c->mac[0] = 0;
        const char *type = vx_attr(a, "type");
        nic_model(c, type ? type : "Am79C973");
        c->connected = a->kid != NULL && (!vx_attr(a, "cable") || is_true(vx_attr(a, "cable")));
    }
    m->shared_folders = count_named(vx_child(hw, "SharedFolders"), "SharedFolder");
    m->usb_filters = count_named(vx_find(hw, "USB"), "DeviceFilter");
    if (!with_disks) return 0;
    /* disks: the controllers in order, each one's devices by port and device */
    const VxNode *reg = vx_child(mach, "MediaRegistry");
    const VxNode *ctls = vx_find(hw, "StorageControllers");
    for (const VxNode *ct = vx_child(ctls, "StorageController"); ct; ct = vx_next(ct, "StorageController")) {
        char bus[8];
        vbox_bus(vx_attr(ct, "type"), bus);
        for (int port = 0; port < 64; port++)
            for (int dev = 0; dev < 2; dev++)
                for (const VxNode *ad = vx_child(ct, "AttachedDevice"); ad; ad = vx_next(ad, "AttachedDevice")) {
                    if (num(vx_attr(ad, "port"), 0) != port || num(vx_attr(ad, "device"), 0) != dev) continue;
                    const char *type = vx_attr(ad, "type");
                    const VxNode *img = vx_child(ad, "Image");
                    const char *iu = vx_attr(img, "uuid");
                    if (type && strcmp(type, "HardDisk")) { if (iu) m->cdrom_files++; continue; }
                    if (!iu) continue;
                    VmiDisk *d = disk_add(m);
                    if (!d) continue;
                    snprintf(d->bus, sizeof d->bus, "%s", bus);
                    d->boot = m->ndisk == 1;
                    /* the chain: from the attached (current) image up to the base */
                    const VxNode *hd = vbox_medium(reg, iu);
                    if (!hd) { snprintf(m->problem, sizeof m->problem, "a disk named in the settings isn't in its media registry"); return 0; }
                    const VxNode *chain[VMI_MAX_CHAIN];
                    int nc = 0;
                    for (const VxNode *x = hd; x && !strcmp(x->name, "HardDisk"); x = x->parent) {
                        if (nc >= VMI_MAX_CHAIN) { snprintf(m->problem, sizeof m->problem, "a disk has more than %d snapshot layers", VMI_MAX_CHAIN); return 0; }
                        chain[nc++] = x;
                    }
                    d->nchain = nc;
                    for (int k = 0; k < nc; k++) {
                        const VxNode *x = chain[nc - 1 - k];
                        copy(d->chain[k].path, VMI_PATH, vx_attr(x, "location"));
                        d->chain[k].fmt = vbox_fmt(vx_attr(x, "format"));
                        const char *u = vx_attr(x, "uuid");
                        uint8_t ub[16];
                        if (u && !vmi_uuid_bytes(u, ub)) vmi_uuid_str(ub, d->chain[k].uuid);
                        for (const VxNode *p = vx_child(x, "Property"); p; p = vx_next(p, "Property")) {
                            const char *pn = vx_attr(p, "name");
                            if (pn && !strncmp(pn, "CRYPT/", 6)) m->encrypted = 1;
                        }
                    }
                    if (nc > 1) m->snapshots = m->snapshots > nc - 1 ? m->snapshots : nc - 1;
                    /* its serial: the source's own setting, else the default made from the base image's uuid */
                    char sk[96];
                    snprintf(sk, sizeof sk, "VBoxInternal/Devices/%s/0/Config/Port%d/SerialNumber", !strcmp(bus, "sata") ? "ahci" : "piix3ide", port);
                    const char *ov = extra(mach, sk);
                    uint8_t ub[16];
                    if (ov) smb(d->serial, ov);
                    else if ((!strcmp(bus, "sata") || !strcmp(bus, "ide")) && !vmi_uuid_bytes(d->chain[0].uuid, ub)) {
                        /* RTUUID in memory: the first three fields little-endian; au32[0] is time_low, au32[3] bytes 12..15 */
                        unsigned a0 = (unsigned)ub[0] << 24 | (unsigned)ub[1] << 16 | (unsigned)ub[2] << 8 | ub[3];
                        unsigned a3 = (unsigned)ub[15] << 24 | (unsigned)ub[14] << 16 | (unsigned)ub[13] << 8 | ub[12];
                        snprintf(d->serial, sizeof d->serial, "VB%08x-%08x", a0, a3);
                    }
                }
    }
    m->snapshots = m->snapshots > count_named(mach, "Snapshot") ? m->snapshots : count_named(mach, "Snapshot");
    const char *sf = vx_attr(mach, "stateFile");
    if (sf && sf[0]) m->saved_state = 1;
    return 0;
}

int vmi_parse_vbox(const char *text, size_t len, VmiMachine *m, char *err, size_t errlen) {
    defaults(m, VMI_VBOX);
    const char *xe = NULL;
    VxNode *root = vx_parse(text, len, &xe);
    if (!root) { if (err) snprintf(err, errlen, "the settings file isn't valid XML: %s", xe ? xe : "?"); return -1; }
    int r = -1;
    if (strcmp(root->name, "VirtualBox")) { fail(err, errlen, "not a .vbox settings file"); goto out; }
    const VxNode *mach = vx_child(root, "Machine");
    if (!mach) {
        if (vx_child(root, "MachineEncrypted")) { m->encrypted = 1; snprintf(m->problem, sizeof m->problem, "the machine is encrypted"); r = 0; }
        else fail(err, errlen, "the settings file has no machine");
        goto out;
    }
    r = vbox_machine(mach, m, 1, err, errlen);
    if (!r && m->memory_mb <= 0) r = fail(err, errlen, "the settings file doesn't say how much memory the machine has");
out:
    vx_free(root);
    return r;
}

/* ---- config.pvs ---- */

int vmi_parse_pvs(const char *text, size_t len, VmiMachine *m, char *err, size_t errlen) {
    defaults(m, VMI_PVS);
    const char *xe = NULL;
    VxNode *root = vx_parse(text, len, &xe);
    if (!root) { if (err) snprintf(err, errlen, "the settings file isn't valid XML: %s", xe ? xe : "?"); return -1; }
    int r = -1;
    if (strcmp(root->name, "ParallelsVirtualMachine")) { fail(err, errlen, "not a config.pvs settings file"); goto out; }
    const VxNode *id = vx_child(root, "Identification"), *hw = vx_child(root, "Hardware"), *set = vx_child(root, "Settings");
    if (!hw) { fail(err, errlen, "the settings file has no hardware section"); goto out; }
    copy(m->name, sizeof m->name, vx_child_text(id, "VmName"));
    const VxNode *gen = vx_child(set, "General");
    const char *ost = vx_child_text(gen, "OsType");
    /* the guest type is a number; 8 is the Windows family in the SDK's list (Reported) */
    m->windows = num(ost, -1) == 8;
    snprintf(m->guest_os, sizeof m->guest_os, "type %.20s, version %.20s", ost, vx_child_text(gen, "OsNumber"));
    const VxNode *cpu = vx_child(hw, "Cpu");
    m->vcpus = (int)num(vx_child_text(cpu, "Number"), 1);
    m->cores = m->vcpus; m->threads = 1;
    topology(m);
    m->memory_mb = (int)num(vx_child_text(vx_child(hw, "Memory"), "RAM"), 0);
    const VxNode *bios = vx_find(set, "Bios");
    m->firmware = num(vx_child_text(bios, "EfiEnabled"), 0) == 1 ? VM_FW_UEFI : VM_FW_BIOS;
    m->secure_boot = num(vx_child_text(bios, "EfiSecureBoot"), 0) == 1;
    m->rtc_local = m->windows;
    /* identity: SystemUuid where the file has one, else the machine's uuid */
    const VxNode *su = vx_find(root, "SystemUuid");
    uint8_t raw[16];
    const char *how = NULL;
    if (su && !vmi_uuid_bytes(vx_text(su), raw)) how = "SystemUuid";
    else if (!vmi_uuid_bytes(vx_child_text(id, "VmUuid"), raw)) how = "VmUuid";
    if (how && !all_zero(raw)) {
        vmi_uuid_str(raw, m->uuid);
        snprintf(m->uuid_how, sizeof m->uuid_how, "%s, as written (assumed)", how);
    }
    for (const VxNode *a = vx_child(hw, "NetworkAdapter"); a; a = vx_next(a, "NetworkAdapter")) {
        if (num(vx_child_text(a, "Enabled"), 1) != 1) continue;
        VmiNic *c = nic_add(m);
        if (!c) continue;
        if (vmi_mac(vx_child_text(a, "MAC"), c->mac)) c->mac[0] = 0;
        /* the adapter's number isn't mapped (To test): an Intel card the guest has a driver for */
        snprintf(c->src_model, sizeof c->src_model, "type %.12s", vx_child_text(a, "AdapterType"));
        snprintf(c->model, sizeof c->model, "e1000e");
        c->connected = num(vx_child_text(a, "Connected"), 1) == 1;
    }
    static const char *const IFACE[] = { "ide", "scsi", "sata", "nvme", "virtio" };
    for (const VxNode *h = vx_child(hw, "Hdd"); h; h = vx_next(h, "Hdd")) {
        if (num(vx_child_text(h, "Enabled"), 1) != 1) continue;
        VmiDisk *d = disk_add(m);
        if (!d) continue;
        /* the .hdd folder by its name: SystemName is a path on the computer that made it, so only its last part is used */
        const char *sn = vx_child_text(h, "SystemName");
        const char *base = strrchr(sn, '/');
        base = base ? base + 1 : sn;
        if (!base[0]) base = vx_child_text(h, "UserFriendlyName");
        copy(d->chain[0].path, VMI_PATH, base);
        d->chain[0].fmt = VMF_PARALLELS;
        d->nchain = 1;
        long long it = num(vx_child_text(h, "InterfaceType"), -1);
        snprintf(d->bus, sizeof d->bus, "%s", it >= 0 && it < 5 ? IFACE[it] : "?");
        d->boot = m->ndisk == 1;
        smb(d->serial, vx_child_text(h, "Serial"));
    }
    for (const VxNode *c = vx_child(hw, "CdRom"); c; c = vx_next(c, "CdRom")) if (vx_child_text(c, "SystemName")[0]) m->cdrom_files++;
    const VxNode *tpm = vx_find(hw, "TpmChip");
    if (tpm && num(vx_child_text(tpm, "Type"), 0) > 0) m->tpm = 1;
    if (vx_find(root, "Encrypted") && num(vx_text(vx_find(root, "Encrypted")), 0) > 0) m->encrypted = 1;
    if (m->memory_mb <= 0) { fail(err, errlen, "the settings file doesn't say how much memory the machine has"); goto out; }
    r = 0;
out:
    vx_free(root);
    return r;
}

/* ---- .ovf ---- */

static long long units(const char *u, long long v) {
    if (!u || !u[0]) return v;
    /* "byte * 2^20", "byte*2^30", "MegaBytes", "GigaBytes", "KiloBytes", "bytes" */
    if (!strcasecmp(u, "MegaBytes") || !strcasecmp(u, "MB")) return v << 20;
    if (!strcasecmp(u, "GigaBytes") || !strcasecmp(u, "GB")) return v << 30;
    if (!strcasecmp(u, "KiloBytes") || !strcasecmp(u, "KB")) return v << 10;
    const char *c = strstr(u, "2^");
    if (c) { long long e = num(c + 2, -1); return e >= 0 && e <= 50 ? v << e : -1; }
    return !strncasecmp(u, "byte", 4) ? v : -1;
}

int vmi_parse_ovf(const char *text, size_t len, VmiMachine *m, char *err, size_t errlen) {
    defaults(m, VMI_OVF);
    const char *xe = NULL;
    VxNode *root = vx_parse(text, len, &xe);
    if (!root) { if (err) snprintf(err, errlen, "the descriptor isn't valid XML: %s", xe ? xe : "?"); return -1; }
    int r = -1;
    if (strcmp(root->name, "Envelope")) { fail(err, errlen, "not an OVF descriptor"); goto out; }
    const VxNode *refs = vx_child(root, "References"), *ds = vx_child(root, "DiskSection");
    const VxNode *vs = vx_child(root, "VirtualSystem");
    if (!vs) { if (vx_child(root, "VirtualSystemCollection")) snprintf(m->problem, sizeof m->problem, "the package holds several machines: import them one at a time"); else fail(err, errlen, "the descriptor has no machine"); r = m->problem[0] ? 0 : -1; goto out; }
    copy(m->name, sizeof m->name, vx_child_text(vs, "Name")[0] ? vx_child_text(vs, "Name") : vx_attr(vs, "id"));
    const VxNode *os = vx_child(vs, "OperatingSystemSection");
    const char *ost = vx_attr(os, "osType");
    guest_os(m, ost ? ost : vx_child_text(os, "Description"));
    if (!m->windows && vx_child_text(os, "Description")[0]) {
        char g[VMI_STR];
        copy(g, sizeof g, vx_child_text(os, "Description"));
        VmiMachine t;
        defaults(&t, VMI_OVF);
        guest_os(&t, g);
        m->windows = t.windows;
    }
    const VxNode *vh = vx_child(vs, "VirtualHardwareSection");
    if (!vh) { fail(err, errlen, "the descriptor has no hardware section"); goto out; }
    const VxNode *items[256];
    int ni = 0;
    for (const VxNode *it = vh->kid; it; it = it->next) if ((!strcmp(it->name, "Item") || !strcmp(it->name, "StorageItem") || !strcmp(it->name, "EthernetPortItem")) && ni < 256) items[ni++] = it;
    for (int i = 0; i < ni; i++) {
        long long rt = num(vx_child_text(items[i], "ResourceType"), -1);
        if (rt == 3) {
            m->vcpus = (int)num(vx_child_text(items[i], "VirtualQuantity"), 1);
            m->cores = (int)num(vx_child_text(items[i], "CoresPerSocket"), 1);
            m->threads = 1;
            topology(m);
        } else if (rt == 4) {
            long long b = units(vx_child_text(items[i], "AllocationUnits"), num(vx_child_text(items[i], "VirtualQuantity"), 0));
            if (vx_child_text(items[i], "AllocationUnits")[0] == 0) b <<= 20;
            m->memory_mb = b > 0 ? (int)(b >> 20) : 0;
        } else if (rt == 10) {
            VmiNic *c = nic_add(m);
            if (!c) continue;
            const char *a = vx_child_text(items[i], "Address");
            if (!a[0] || vmi_mac(a, c->mac)) c->mac[0] = 0;
            const char *st = vx_child_text(items[i], "ResourceSubType");
            nic_model(c, st);
            if (!c->model[0] && !strcasecmp(st, "E1000E")) snprintf(c->model, sizeof c->model, "e1000e");
            const char *aa = vx_child_text(items[i], "AutomaticAllocation");
            c->connected = !aa[0] || is_true(aa);
        } else if (rt == 15 || rt == 16) {
            if (vx_child_text(items[i], "HostResource")[0]) m->cdrom_files++;
        }
    }
    /* disks: Items of type 17 in their order, each naming a Disk, which names a File */
    for (int i = 0; i < ni; i++) {
        if (num(vx_child_text(items[i], "ResourceType"), -1) != 17) continue;
        const char *hr = vx_child_text(items[i], "HostResource");
        const char *did = !strncmp(hr, "ovf:/disk/", 10) ? hr + 10 : !strncmp(hr, "/disk/", 6) ? hr + 6 : NULL;
        if (!did) continue;
        const VxNode *dk = NULL;
        for (const VxNode *x = vx_child(ds, "Disk"); x; x = vx_next(x, "Disk")) { const char *a = vx_attr(x, "diskId"); if (a && !strcmp(a, did)) { dk = x; break; } }
        if (!dk) { snprintf(m->problem, sizeof m->problem, "a disk named by the descriptor isn't in its disk list"); r = 0; goto out; }
        const char *fref = vx_attr(dk, "fileRef");
        if (!fref) { snprintf(m->problem, sizeof m->problem, "a disk has no data (an empty disk isn't imported)"); continue; }
        const VxNode *f = NULL;
        for (const VxNode *x = vx_child(refs, "File"); x; x = vx_next(x, "File")) { const char *a = vx_attr(x, "id"); if (a && !strcmp(a, fref)) { f = x; break; } }
        if (!f || !vx_attr(f, "href")) { snprintf(m->problem, sizeof m->problem, "a disk's file isn't in the descriptor's references"); r = 0; goto out; }
        if (vx_attr(f, "compression") && strcasecmp(vx_attr(f, "compression"), "identity")) { snprintf(m->problem, sizeof m->problem, "a disk is compressed with %.20s, which isn't supported", vx_attr(f, "compression")); r = 0; goto out; }
        if (vx_attr(f, "chunkSize")) { snprintf(m->problem, sizeof m->problem, "a disk is split into chunks, which isn't supported"); r = 0; goto out; }
        VmiDisk *d = disk_add(m);
        if (!d) continue;
        copy(d->chain[0].path, VMI_PATH, vx_attr(f, "href"));
        const char *fmt = vx_attr(dk, "format");
        d->chain[0].fmt = fmt && strstr(fmt, "vmdk") ? VMF_VMDK : fmt && strstr(fmt, "vhd") ? VMF_VPC : fmt && strstr(fmt, "qcow") ? VMF_QCOW2 : VMF_UNKNOWN;
        d->nchain = 1;
        long long cap = num(vx_attr(dk, "capacity"), 0);
        d->vsize = units(vx_attr(dk, "capacityAllocationUnits"), cap);
        if (d->vsize < 0) d->vsize = 0;
        d->boot = m->ndisk == 1;
        snprintf(d->bus, sizeof d->bus, "?");
    }
    /* firmware and identity: VMware-style extra settings, or the VirtualBox machine section a package may carry */
    for (const VxNode *c = vh->kid; c; c = c->next) {
        if (strcmp(c->name, "Config") && strcmp(c->name, "ExtraConfig")) continue;
        const char *k = vx_attr(c, "key"), *v = vx_attr(c, "value");
        if (!k || !v) continue;
        if (!strcmp(k, "firmware")) m->firmware = !strcasecmp(v, "efi") ? VM_FW_UEFI : VM_FW_BIOS;
        else if (!strcmp(k, "uefi.secureBoot.enabled")) m->secure_boot = is_true(v);
        else if (!strcmp(k, "uuid.bios")) {
            uint8_t raw[16];
            if (!vmi_uuid_bytes(v, raw) && !all_zero(raw)) { guest_reads(raw, 1, m->uuid); snprintf(m->uuid_how, sizeof m->uuid_how, "uuid.bios in the package, read little-endian"); }
        }
    }
    const VxNode *vm = vx_child(vs, "Machine");
    if (vm && vx_child(vm, "Hardware")) {
        VmiMachine v;
        defaults(&v, VMI_VBOX);
        if (!vbox_machine(vm, &v, 0, NULL, 0)) {
            m->firmware = v.firmware; m->secure_boot = v.secure_boot; m->rtc_local = v.rtc_local; m->tpm = v.tpm;
            if (v.arch >= 0) m->arch = v.arch;
            if (v.guest_os[0]) { snprintf(m->guest_os, sizeof m->guest_os, "%s", v.guest_os); m->windows = v.windows; }
            memcpy(m->uuid, v.uuid, sizeof m->uuid); memcpy(m->uuid_how, v.uuid_how, sizeof m->uuid_how);
            memcpy(m->sm_serial, v.sm_serial, VMI_STR); memcpy(m->sm_manufacturer, v.sm_manufacturer, VMI_STR);
            memcpy(m->sm_product, v.sm_product, VMI_STR); memcpy(m->sm_version, v.sm_version, VMI_STR);
            memcpy(m->sm_sku, v.sm_sku, VMI_STR); memcpy(m->sm_family, v.sm_family, VMI_STR);
            /* its cards carry the MACs the package kept (export may strip them); the slots match the Items' order */
            for (int i = 0; i < v.nnic && i < m->nnic; i++) {
                if (v.nic[i].mac[0]) memcpy(m->nic[i].mac, v.nic[i].mac, 18);
                if (v.nic[i].model[0]) { memcpy(m->nic[i].model, v.nic[i].model, sizeof m->nic[i].model); memcpy(m->nic[i].src_model, v.nic[i].src_model, sizeof m->nic[i].src_model); }
            }
            if (!m->nnic) for (int i = 0; i < v.nnic; i++) m->nic[m->nnic++] = v.nic[i];
            /* the topology the guest had there: VirtualBox gives one socket with every core */
            if (m->vcpus > 0) { m->cores = m->vcpus; m->threads = 1; topology(m); }
        }
    } else m->rtc_local = m->windows;
    if (m->memory_mb <= 0) { fail(err, errlen, "the descriptor doesn't say how much memory the machine has"); goto out; }
    r = 0;
out:
    vx_free(root);
    return r;
}

/* ---- VMDK descriptor ---- */

int vmi_parse_vmdk(const char *text, size_t len, VmiVmdk *d, char *err, size_t errlen) {
    memset(d, 0, sizeof *d);
    if (len > (1u << 20)) return fail(err, errlen, "the disk's descriptor is larger than 1 MB");
    size_t i = 0;
    int header = 0;
    while (i < len) {
        size_t e = i;
        while (e < len && text[e] != '\n' && text[e]) e++;
        char line[VMI_PATH + 128];
        /* a line longer than any we accept is refused, never cut: QEMU reads the descriptor too, and must read the same */
        if (e - i >= sizeof line - 1) return fail(err, errlen, "a line of the disk's descriptor is too long");
        size_t l = e - i;
        memcpy(line, text + i, l);
        line[l] = 0;
        int nul = e < len && !text[e];
        i = e + 1;
        while (l && (line[l - 1] == '\r' || line[l - 1] == ' ' || line[l - 1] == '\t')) line[--l] = 0;
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (!strncmp(s, "# Disk DescriptorFile", 21)) header = 1;
        if (nul) break;                                       /* the end of an embedded descriptor */
        if (!*s || *s == '#') continue;
        if (!strncmp(s, "RW ", 3) || !strncmp(s, "RDONLY ", 7) || !strncmp(s, "NOACCESS ", 9)) {
            if (d->nextents >= VMI_MAX_EXTENTS) return fail(err, errlen, "the disk has too many extents");
            char *p = strchr(s, ' ') + 1;
            char *endp;
            long long sec = strtoll(p, &endp, 10);
            if (endp == p || sec < 0) return fail(err, errlen, "a bad extent line");
            p = endp;
            while (*p == ' ') p++;
            char kind[12];
            size_t k = 0;
            while (*p && *p != ' ' && k + 1 < sizeof kind) kind[k++] = *p++;
            kind[k] = 0;
            while (*p == ' ') p++;
            typeof(d->ext[0]) *x = &d->ext[d->nextents++];
            memset(x, 0, sizeof *x);
            snprintf(x->kind, sizeof x->kind, "%s", kind);
            x->sectors = sec;
            if (*p == '"') {
                char *q = strchr(p + 1, '"');
                if (!q || (size_t)(q - p - 1) >= VMI_PATH) return fail(err, errlen, "a bad extent file name");
                memcpy(x->file, p + 1, (size_t)(q - p - 1));
                x->file[q - p - 1] = 0;
                p = q + 1;
                while (*p == ' ') p++;
                if (*p) {
                    char *oe;
                    x->offset = strtoll(p, &oe, 10);
                    while (*oe == ' ') oe++;
                    if (oe == p || x->offset < 0 || *oe) return fail(err, errlen, "a bad extent offset");
                }
            } else if (strcmp(kind, "ZERO")) return fail(err, errlen, "an extent without a file");
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) continue;
        char key[64];
        size_t kl = (size_t)(eq - s);
        while (kl && (s[kl - 1] == ' ' || s[kl - 1] == '\t')) kl--;
        if (kl >= sizeof key) continue;
        memcpy(key, s, kl);
        key[kl] = 0;
        char *v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        size_t vl = strlen(v);
        if (vl >= 2 && v[0] == '"' && v[vl - 1] == '"') { v[vl - 1] = 0; v++; }
        if (!strcmp(key, "createType")) snprintf(d->create_type, sizeof d->create_type, "%s", v);
        else if (!strcmp(key, "parentFileNameHint")) snprintf(d->parent, sizeof d->parent, "%s", v);
        else if (!strcmp(key, "ddb.uuid")) snprintf(d->uuid, sizeof d->uuid, "%s", v);
        else if (!strncmp(key, "encryption.", 11) || !strcmp(key, "ddb.encryption")) d->encrypted = 1;
    }
    if (!header && !d->create_type[0]) return fail(err, errlen, "not a VMDK descriptor");
    if (!d->nextents) return fail(err, errlen, "the disk's descriptor names no data");
    return 0;
}

/* ---- DiskDescriptor.xml ---- */

int vmi_parse_hdd(const char *text, size_t len, VmiHdd *d, char *err, size_t errlen) {
    memset(d, 0, sizeof *d);
    const char *xe = NULL;
    VxNode *root = vx_parse(text, len, &xe);
    if (!root) { if (err) snprintf(err, errlen, "the disk's descriptor isn't valid XML: %s", xe ? xe : "?"); return -1; }
    int r = -1;
    if (strcmp(root->name, "Parallels_disk_image")) { fail(err, errlen, "not a disk descriptor"); goto out; }
    const VxNode *par = vx_child(root, "Disk_Parameters");
    d->sectors = num(vx_child_text(par, "Disk_size"), 0);
    {
        /* an encryption engine other than the zero GUID means the data files are encrypted */
        const char *eng = vx_child_text(vx_child(par, "Encryption"), "Engine");
        if (eng[0] && strcmp(eng, "{00000000-0000-0000-0000-000000000000}")) d->encrypted = 1;
    }
    const VxNode *sd = vx_child(root, "StorageData");
    const VxNode *st = vx_child(sd, "Storage");
    if (!st) { fail(err, errlen, "the disk's descriptor names no data"); goto out; }
    if (vx_next(st, "Storage")) { fail(err, errlen, "the disk is split into several pieces, which isn't supported"); goto out; }
    /* the snapshot tree: the current state is TopGUID when written, else the only leaf */
    const VxNode *sn = vx_child(root, "Snapshots");
    struct { char g[40], p[40]; int child; } shot[VMI_MAX_CHAIN + 1];
    int ns = 0;
    for (const VxNode *s = vx_child(sn, "Shot"); s; s = vx_next(s, "Shot")) {
        if (ns >= VMI_MAX_CHAIN) { fail(err, errlen, "the disk has too many snapshot layers"); goto out; }
        copy(shot[ns].g, 40, vx_child_text(s, "GUID"));
        copy(shot[ns].p, 40, vx_child_text(s, "ParentGUID"));
        shot[ns].child = 0;
        ns++;
    }
    const char *zero = "{00000000-0000-0000-0000-000000000000}";
    for (int i = 0; i < ns; i++) for (int k = 0; k < ns; k++) if (i != k && !strcasecmp(shot[k].p, shot[i].g)) shot[i].child = 1;
    const char *top = vx_child_text(sn, "TopGUID");
    int cur = -1;
    if (top[0]) { for (int i = 0; i < ns; i++) if (!strcasecmp(shot[i].g, top)) cur = i; }
    else {
        int leaves = 0;
        for (int i = 0; i < ns; i++) if (!shot[i].child) { leaves++; cur = i; }
        if (leaves != 1) cur = -1;
    }
    /* walk to the root, base first */
    int path[VMI_MAX_CHAIN], np = 0;
    if (ns) {
        if (cur < 0) { fail(err, errlen, "the disk's snapshots don't say which one is current"); goto out; }
        for (int i = cur; i >= 0 && np < VMI_MAX_CHAIN; ) {
            path[np++] = i;
            if (!strcasecmp(shot[i].p, zero) || !shot[i].p[0]) break;
            int nx = -1;
            for (int k = 0; k < ns; k++) if (!strcasecmp(shot[k].g, shot[i].p)) nx = k;
            if (nx < 0 || np >= ns) { fail(err, errlen, "the disk's snapshot tree is broken"); goto out; }
            i = nx;
        }
    }
    for (int k = 0; k < (ns ? np : 1); k++) {
        const char *g = ns ? shot[path[np - 1 - k]].g : NULL;
        const VxNode *img = NULL;
        for (const VxNode *x = vx_child(st, "Image"); x; x = vx_next(x, "Image")) if (!g || !strcasecmp(vx_child_text(x, "GUID"), g)) { img = x; break; }
        if (!img) { fail(err, errlen, "a snapshot layer of the disk has no data file"); goto out; }
        copy(d->file[d->nlayers++], VMI_PATH, vx_child_text(img, "File"));
    }
    d->snapshots = np > 1 ? np - 1 : 0;
    if (d->sectors <= 0) { fail(err, errlen, "the disk's descriptor has no size"); goto out; }
    r = 0;
out:
    vx_free(root);
    return r;
}

/* ---- OVA (ustar) ---- */

static long long octal(const char *p, size_t n) {
    long long v = 0;
    size_t i = 0;
    while (i < n && (p[i] == ' ' || p[i] == 0)) i++;
    if (i == n) return 0;
    if ((unsigned char)p[0] & 0x80) return -1;                /* base-256 sizes: more than 8 GB in one member is fine in octal (11 digits) */
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) { v = v * 8 + (p[i] - '0'); if (v > (1LL << 50)) return -1; }
    for (; i < n; i++) if (p[i] != ' ' && p[i] != 0) return -1;
    return v;
}

int vmi_tar_list(vmi_read_fn rd, void *ctx, long long size, VmiTarEntry *out, int max, char *err, size_t errlen) {
    int n = 0;
    long long off = 0;
    char h[512];
    while (off + 512 <= size) {
        if (rd(ctx, h, 512, off) != 512) return fail(err, errlen, "the package can't be read");
        int zero = 1;
        for (int i = 0; i < 512; i++) if (h[i]) { zero = 0; break; }
        if (zero) break;
        /* checksum: the header's bytes with the checksum field as spaces */
        long long ck = octal(h + 148, 8), sum = 0;
        for (int i = 0; i < 512; i++) sum += (i >= 148 && i < 156) ? ' ' : (unsigned char)h[i];
        if (ck != sum) return fail(err, errlen, "the package isn't a tar archive (bad header checksum)");
        long long sz = octal(h + 124, 12);
        if (sz < 0) return fail(err, errlen, "a bad member size in the package");
        char type = h[156];
        char name[101];
        memcpy(name, h, 100);
        name[100] = 0;
        if (!memcmp(h + 257, "ustar", 5) && h[345]) return fail(err, errlen, "the package uses long member names, which aren't supported");
        long long data = off + 512;
        if (data + sz > size || data + sz < data) return fail(err, errlen, "the package is cut short");
        if (type == '0' || type == 0) {
            if (!name[0] || strchr(name, '/') || !vmi_rel_ok(name)) return fail(err, errlen, "a member of the package has a name with a folder in it, or \"..\"");
            for (int i = 0; i < n; i++) if (!strcmp(out[i].name, name)) return fail(err, errlen, "the package has two members with the same name");
            if (n >= max) return fail(err, errlen, "the package has too many members");
            memcpy(out[n].name, name, sizeof out[n].name);
            out[n].offset = data;
            out[n].size = sz;
            n++;
        } else if (type == 'x' || type == 'g' || type == 'L' || type == 'K') {
            return fail(err, errlen, "the package uses extended headers, which aren't supported");
        } else if (type != '5') {
            return fail(err, errlen, "the package holds a link or a device, which isn't allowed");
        }
        off = data + ((sz + 511) / 512) * 512;
    }
    if (!n) return fail(err, errlen, "the package is empty");
    return n;
}

/* ---- disk formats ---- */

int vmi_disk_format(const uint8_t *h, size_t n, const char *name) {
    size_t nl = name ? strlen(name) : 0;
    if (n >= 4 && !memcmp(h, "KDMV", 4)) return VMF_VMDK;
    if (n >= 4 && !memcmp(h, "COWD", 4)) return VMF_VMDK;
    if (n >= 21 && !memcmp(h, "# Disk DescriptorFile", 21)) return VMF_VMDK;
    if (n >= 68 && h[64] == 0x7f && h[65] == 0x10 && h[66] == 0xda && h[67] == 0xbe) return VMF_VDI;
    if (n >= 8 && !memcmp(h, "vhdxfile", 8)) return VMF_VHDX;
    if (n >= 8 && !memcmp(h, "conectix", 8)) return VMF_VPC;
    if (n >= 16 && (!memcmp(h, "WithoutFreeSpace", 16) || !memcmp(h, "WithouFreSpacExt", 16))) return VMF_PARALLELS;
    if (n >= 4 && !memcmp(h, "QFI\xfb", 4)) return VMF_QCOW2;
    if (nl > 4 && !strcasecmp(name + nl - 4, ".vhd")) return VMF_VPC;   /* a fixed VHD: its footer is at the end */
    if ((nl > 4 && (!strcasecmp(name + nl - 4, ".img") || !strcasecmp(name + nl - 4, ".raw"))) || (nl > 9 && !strcasecmp(name + nl - 9, "-flat.vmdk")))
        return VMF_RAW;
    return VMF_UNKNOWN;
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }

static void rtuuid(const uint8_t *p, char out[37]) {
    uint8_t b[16];
    memcpy(b, p, 16);
    vmi_uuid_swap(b);                                          /* RTUUID keeps its first three fields little-endian */
    vmi_uuid_str(b, out);
}

int vmi_vdi_header(const uint8_t *h, size_t n, VmiVdi *v) {
    memset(v, 0, sizeof *v);
    if (n < 456 || le32(h + 64) != 0xbeda107f || (le32(h + 68) >> 16) != 1) return -1;
    v->type = (int)le32(h + 76);
    v->size = (long long)le64(h + 368);
    rtuuid(h + 392, v->uuid);
    rtuuid(h + 424, v->parent);
    return v->size > 0 ? 0 : -1;
}
