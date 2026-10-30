/* hwsnap.h — the tested-hardware catalogue's signed snapshot (0.8.7, docs/BENCHMARK.md "0.8.7 — Community
 * hardware reports"): what System Information's Compatibility panel shows as community data.
 *
 * tools/hwreport/catalog.mjs builds <assets>/hwreport/snapshot.json from the accepted reports;
 * tools/release/sign-hwsnapshot.sh signs it on the maintainer's machine with the LordOS release key (Ed25519,
 * the same key and the same file format as code-manifest.sig: base64 of the 64-byte signature over the exact
 * bytes) into snapshot.json.sig. Both ship in the lordos package and update with it through the signed APT
 * repository; downloading a newer snapshot by itself is left for later.
 *
 * lord.sysinfo.community() (ui/js_hw.c) takes no arguments: it reads the two files from the fixed folder,
 * and only when the signature verifies with the key this build trusts (codesig_key()) and the structure is
 * the expected one does JS get the snapshot. Unsigned, badly signed or malformed snapshots are never shown,
 * in a development build either. */
#ifndef LORD_SYSREPORT_HWSNAP_H
#define LORD_SYSREPORT_HWSNAP_H

#include <stddef.h>
#include <stdint.h>

#define HWSNAP_FILE        "snapshot.json"
#define HWSNAP_SIG         "snapshot.json.sig"
#define HWSNAP_FORMAT      "lordos-hwsnapshot"
#define HWSNAP_MAX         (1u << 20)      /* bytes of snapshot.json */
#define HWSNAP_SIG_MAX     256             /* bytes of snapshot.json.sig */
#define HWSNAP_DEVICES_MAX 20000
#define HWSNAP_KEY_MAX     160
#define HWSNAP_NAME_MAX    128

typedef enum {
    HWSNAP_OK,          /* signed with the trusted key and well formed */
    HWSNAP_MISSING,     /* no snapshot.json (or not a regular file, or a link) */
    HWSNAP_UNSIGNED,    /* snapshot.json without snapshot.json.sig */
    HWSNAP_BAD_SIG,     /* the signature doesn't verify, or isn't 64 bytes of base64 */
    HWSNAP_INVALID,     /* signed, but not a snapshot of this format (or too big) */
} HwSnapState;

/* Checks a snapshot's text: the signature first (nothing of an unsigned or badly signed text is parsed),
 * then the structure: {format: "lordos-hwsnapshot", version: 1, generated: "YYYY-MM-DD", reports: n,
 * devices: [{key, component, name, lordos, works, limits, no, reports, references}]} with the limits above,
 * a known component, an x.y.z[.w] version and whole counts. sig NULL: HWSNAP_UNSIGNED. err gets the reason. */
HwSnapState hwsnap_check(const char *json, size_t len, const char *sig, size_t siglen, const uint8_t pub[32],
                         char *err, size_t errlen);

/* Reads <dir>/snapshot.json and <dir>/snapshot.json.sig (regular files, no links, within the limits) and
 * checks them. On HWSNAP_OK *json is the malloc'd, NUL-terminated text (len bytes); otherwise NULL. */
HwSnapState hwsnap_load(const char *dir, const uint8_t pub[32], char **json, size_t *len, char *err, size_t errlen);

const char *hwsnap_state_name(HwSnapState s);   /* "ok", "missing", "unsigned", "bad-signature", "invalid" */

#endif
