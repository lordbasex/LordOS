/* smbios.h — the memory modules from the firmware's SMBIOS tables (type 17, "Memory Device"), for
 * System Information (docs/BENCHMARK.md). The kernel exposes each entry as
 * /sys/firmware/dmi/entries/17-<n>/raw, readable by root only, so the read runs in the admin helper's
 * read-only action `hw-memory-modules` (hw/hw_admin.c) and only these fields leave it: slot, bank,
 * size, type, speed, configured speed and manufacturer. Serial numbers, asset tags and part numbers
 * are never read. The parser takes untrusted bytes: every offset and string index is checked. */
#ifndef LORD_SYSREPORT_SMBIOS_H
#define LORD_SYSREPORT_SMBIOS_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char slot[32], bank[32];            /* device and bank locators ("DIMM_A1", "BANK 0") */
    unsigned size_mb;                   /* 0: an empty slot */
    char type[12];                      /* "DDR4", "LPDDR5", "other", "" unknown */
    unsigned speed, configured;         /* MT/s, 0 unknown */
    char maker[32];                     /* "" unknown or not filled in by the firmware */
} SmMem;

/* Parses one raw type-17 structure (formatted area + its string set). 0, or -1 if it isn't one or
 * is malformed. Strings come out as printable ASCII without '|', placeholders ("Unknown",
 * "Not Specified", "To Be Filled By O.E.M."…) as "". */
int smbios_memory_parse(const uint8_t *raw, size_t len, SmMem *m);

/* The action's line for one module: "mem <size_mb>|<type>|<speed>|<configured>|<slot>|<bank>|<maker>". */
void smbios_memory_line(const SmMem *m, char *out, size_t n);

#endif
