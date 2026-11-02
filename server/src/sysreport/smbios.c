/* smbios.c — SMBIOS type 17 (Memory Device) reader, see smbios.h. Offsets from the SMBIOS
 * specification (DSP0134): 0x0C size, 0x10 device locator, 0x11 bank locator, 0x12 memory type,
 * 0x15 speed (2.3+), 0x17 manufacturer (2.3+), 0x1C extended size (2.7+), 0x20 configured speed
 * (2.7+), 0x54/0x58 extended speeds (3.3+). */
#include "smbios.h"

#include <stdio.h>
#include <string.h>

static unsigned u16(const uint8_t *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }
static unsigned long u32(const uint8_t *p) { return (unsigned long)u16(p) | (unsigned long)u16(p + 2) << 16; }

/* String number idx (1-based) of the set that starts at s and ends at end (the set ends with
 * "\0\0"). Copied as printable ASCII, '|' replaced, trimmed; placeholders become "". */
static void str_at(const uint8_t *s, const uint8_t *end, unsigned idx, char *out, size_t n) {
    out[0] = 0;
    if (!idx) return;
    for (unsigned i = 1; s < end && *s; i++) {
        const uint8_t *e = memchr(s, 0, (size_t)(end - s));
        if (!e) return;
        if (i == idx) {
            size_t o = 0;
            while (s < e && *s == ' ') s++;
            for (; s < e && o + 1 < n; s++) out[o++] = (*s >= 0x20 && *s < 0x7f && *s != '|') ? (char)*s : '?';
            while (o && out[o - 1] == ' ') o--;
            out[o] = 0;
            static const char *junk[] = { "Unknown", "UNKNOWN", "Not Specified", "NO DIMM", "None", "Undefined",
                                          "To Be Filled By O.E.M.", "Manufacturer00", "0000", "00000000" };
            for (size_t j = 0; j < sizeof junk / sizeof junk[0]; j++) if (!strcmp(out, junk[j])) out[0] = 0;
            return;
        }
        s = e + 1;
    }
}

static const char *mem_type(unsigned t) {
    switch (t) {
    case 0x07: return "RAM";
    case 0x0F: return "SDRAM";
    case 0x12: return "DDR";
    case 0x13: return "DDR2";
    case 0x18: return "DDR3";
    case 0x1A: return "DDR4";
    case 0x1B: return "LPDDR";
    case 0x1C: return "LPDDR2";
    case 0x1D: return "LPDDR3";
    case 0x1E: return "LPDDR4";
    case 0x22: return "DDR5";
    case 0x23: return "LPDDR5";
    case 0x00: case 0x01: case 0x02: return "";
    default: return "other";
    }
}

int smbios_memory_parse(const uint8_t *raw, size_t len, SmMem *m) {
    memset(m, 0, sizeof *m);
    if (!raw || len < 0x15 || raw[0] != 17) return -1;
    size_t fl = raw[1];                 /* the formatted area's length */
    if (fl < 0x15 || fl > len) return -1;
    const uint8_t *strs = raw + fl, *end = raw + len;
    /* the string set must end with a double NUL inside the buffer */
    int closed = 0;
    for (const uint8_t *p = strs; p + 1 < end; p++) if (!p[0] && !p[1]) { closed = 1; end = p + 1; break; }
    if (!closed) return -1;

    unsigned size = u16(raw + 0x0C);
    if (size == 0xFFFF) m->size_mb = 0;                 /* unknown: treated as empty */
    else if (size == 0x7FFF && fl >= 0x20) m->size_mb = (unsigned)(u32(raw + 0x1C) & 0x7FFFFFFFul);
    else if (size & 0x8000) m->size_mb = (size & 0x7FFF) / 1024;   /* in KB */
    else m->size_mb = size;
    str_at(strs, end, raw[0x10], m->slot, sizeof m->slot);
    str_at(strs, end, raw[0x11], m->bank, sizeof m->bank);
    snprintf(m->type, sizeof m->type, "%s", mem_type(raw[0x12]));
    if (fl >= 0x17) {
        m->speed = u16(raw + 0x15);
        if (m->speed == 0xFFFF && fl >= 0x58) m->speed = (unsigned)u32(raw + 0x54);
    }
    if (fl >= 0x18) str_at(strs, end, raw[0x17], m->maker, sizeof m->maker);
    if (fl >= 0x22) {
        m->configured = u16(raw + 0x20);
        if (m->configured == 0xFFFF && fl >= 0x5C) m->configured = (unsigned)u32(raw + 0x58);
    }
    if (m->speed > 100000) m->speed = 0;
    if (m->configured > 100000) m->configured = 0;
    return 0;
}

void smbios_memory_line(const SmMem *m, char *out, size_t n) {
    snprintf(out, n, "mem %u|%s|%u|%u|%s|%s|%s", m->size_mb, m->type, m->speed, m->configured, m->slot, m->bank, m->maker);
}
