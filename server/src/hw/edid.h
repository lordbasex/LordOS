/* edid.h — our own EDID 1.3/1.4 reader (the 128-byte base block a monitor sends).
 * Used to name displays in Settings → Displays and to store each display's configuration by
 * its identity (vendor, product and serial number) rather than by connector. See
 * docs/SETTINGS.md, "Displays". */
#ifndef LORD_HW_EDID_H
#define LORD_HW_EDID_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char vendor[4];             /* 3-letter PNP code ("DEL", "SAM"...) */
    uint16_t product;
    uint32_t serial;            /* binary serial number (0 if not reported) */
    char name[14];              /* 0xFC descriptor: monitor name ("DELL U2720Q") */
    char serial_text[14];       /* 0xFF descriptor: serial number as text */
    int year, week;             /* manufacture date */
    int width_cm, height_cm;    /* physical size (0 = unknown, e.g. a projector) */
    int pref_w, pref_h;         /* first detailed timing (the native mode) */
    int version, revision;      /* 1.3, 1.4 */
    int digital;
    /* chromaticities (bytes 25-34): red, green, blue and white x, y ×1,000,000 from the 10-bit values; all 0 when a
     * value is 0 (not given). color-management-v1 tells apps the display's own gamut with them (wl/wlcolor.h). */
    int chroma[8];
} Edid;

/* 0 if the base block is a valid EDID (header and checksum). Reads only validated bytes. */
int  edid_parse(const uint8_t *data, size_t len, Edid *e);
/* Stable identity used as the settings key: "DEL-a0c4-12345678" or, without a serial, with the
 * name. Two identical monitors without serial numbers share an identity (see docs/SETTINGS.md). */
void edid_identity(const Edid *e, char *out, size_t n);
/* Display name: the descriptor's, or "<vendor> <product>". */
void edid_display_name(const Edid *e, char *out, size_t n);
/* Diagonal in tenths of an inch (240 = 24.0"), 0 if unknown. */
int  edid_diagonal_tenths(const Edid *e);

#endif
