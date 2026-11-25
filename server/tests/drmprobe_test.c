/* drmprobe_test.c — the display engine's facts (src/sysreport/drmprobe.c): IN_FORMATS blobs (well formed, cut, lying
 * about their counts or offsets, random bytes), the text and JSON of known facts, and with DRMPROBE_LIVE=1 the probe
 * on this machine's cards when this user may open them (printed; a machine without them still passes). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <drm/drm.h>
#include <drm/drm_mode.h>

#include "../src/sysreport/drmprobe.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* a blob with nf formats and nm modifier entries (the modifiers cycle through k distinct values) */
static size_t blob(unsigned char *out, size_t cap, uint32_t nf, uint32_t nm, int k) {
    struct drm_format_modifier_blob h = { .version = FORMAT_BLOB_CURRENT, .count_formats = nf, .count_modifiers = nm };
    h.formats_offset = sizeof h;
    h.modifiers_offset = (uint32_t)(sizeof h + ((nf * 4 + 7) & ~7u));
    size_t len = h.modifiers_offset + nm * sizeof(struct drm_format_modifier);
    if (len > cap) return 0;
    memset(out, 0, len);
    memcpy(out, &h, sizeof h);
    for (uint32_t i = 0; i < nf; i++) { uint32_t f = 0x34325258 + i; memcpy(out + h.formats_offset + i * 4, &f, 4); }
    for (uint32_t i = 0; i < nm; i++) {
        struct drm_format_modifier m = { .formats = ~0ull, .offset = 0, .modifier = (uint64_t)(i % (uint32_t)k) << 52 };
        memcpy(out + h.modifiers_offset + i * sizeof m, &m, sizeof m);
    }
    return len;
}

int main(void) {
    static unsigned char b[1 << 16];
    int f = -7, m = -7;
    size_t len = blob(b, sizeof b, 12, 10, 5);
    CHECK(len && drmprobe_in_formats(b, len, &f, &m) == 0 && f == 12 && m == 5, "good blob: %d formats %d modifiers", f, m);
    len = blob(b, sizeof b, 3, 0, 1);
    CHECK(drmprobe_in_formats(b, len, &f, &m) == 0 && f == 3 && m == 0, "no modifiers: %d %d", f, m);
    len = blob(b, sizeof b, 8, 200, 100);
    CHECK(drmprobe_in_formats(b, len, &f, &m) == 0 && m == 100, "more distinct modifiers than the seen list: %d", m);
    /* cut, lying counts and offsets, wrong version: refused, outputs untouched */
    len = blob(b, sizeof b, 12, 10, 5);
    f = m = -7;
    CHECK(drmprobe_in_formats(b, len - 1, &f, &m) == -1 && f == -7 && m == -7, "cut blob accepted");
    CHECK(drmprobe_in_formats(b, 10, &f, &m) == -1, "header cut");
    CHECK(drmprobe_in_formats(NULL, 100, &f, &m) == -1, "NULL");
    struct drm_format_modifier_blob h;
    memcpy(&h, b, sizeof h);
    struct drm_format_modifier_blob bad = h; bad.count_formats = 0xffffffffu;
    memcpy(b, &bad, sizeof bad);
    CHECK(drmprobe_in_formats(b, len, &f, &m) == -1, "huge format count");
    bad = h; bad.modifiers_offset = 0xfffffff0u;
    memcpy(b, &bad, sizeof bad);
    CHECK(drmprobe_in_formats(b, len, &f, &m) == -1, "modifiers offset past the end");
    bad = h; bad.formats_offset = 2;
    memcpy(b, &bad, sizeof bad);
    CHECK(drmprobe_in_formats(b, len, &f, &m) == -1, "formats inside the header");
    bad = h; bad.version = 2;
    memcpy(b, &bad, sizeof bad);
    CHECK(drmprobe_in_formats(b, len, &f, &m) == -1, "unknown version");
    /* random bytes: never a crash, never a count past what fits */
    srand(1);
    for (int r = 0; r < 20000; r++) {
        size_t n = (size_t)(rand() % 600);
        for (size_t i = 0; i < n; i++) b[i] = (unsigned char)rand();
        if (r & 1) { struct drm_format_modifier_blob z = { .version = 1, .count_formats = (uint32_t)(rand() % 64), .formats_offset = (uint32_t)(rand() % 80),
                                                           .count_modifiers = (uint32_t)(rand() % 16), .modifiers_offset = (uint32_t)(rand() % 600) };
                      if (n >= sizeof z) memcpy(b, &z, sizeof z); }
        int ff = -1, mm = -1;
        if (drmprobe_in_formats(b, n, &ff, &mm) == 0) CHECK(ff >= 0 && (size_t)ff * 4 <= n && mm >= 0 && (size_t)mm * 24 <= n, "random blob: %d %d of %zu", ff, mm, n);
    }

    /* the text and JSON of known facts */
    DrmFacts d;
    drmprobe_init(&d);
    char t[700];
    drmprobe_json(&d, t, sizeof t);
    CHECK(!strcmp(t, "null"), "not probed: %s", t);
    d.probed = 1;
    drmprobe_text(&d, "  ", t, sizeof t);
    CHECK(strstr(t, "not read (this user can't open"), "no nodes: %s", t);
    d.card_node = d.render_node = 1;
    d.atomic = 1; d.planes = 6; d.primary = 2; d.overlay = 2; d.cursor = 2; d.scanout_formats = 12; d.scanout_modifiers = 5;
    d.fb_modifiers = 1; d.syncobj = 1; d.syncobj_timeline = 0; d.prime_import = 1; d.prime_export = 1; d.connectors = 4; d.non_desktop = 1;
    drmprobe_text(&d, "  ", t, sizeof t);
    CHECK(strstr(t, "  display engine: atomic yes; planes 6 (2 primary, 2 overlay, 2 cursor); scanout formats 12, modifiers 5; framebuffer modifiers yes\n"), "text: %s", t);
    CHECK(strstr(t, "  explicit sync: syncobj yes, timeline no; PRIME import yes, export yes; drm-lease: yes (1 non-desktop connector)\n"), "text: %s", t);
    drmprobe_json(&d, t, sizeof t);
    CHECK(!strcmp(t, "{\"cardNode\":true,\"renderNode\":true,\"atomic\":1,\"planes\":6,\"primary\":2,\"overlay\":2,\"cursor\":2,\"scanoutFormats\":12,"
                     "\"scanoutModifiers\":5,\"fbModifiers\":1,\"syncobj\":1,\"syncobjTimeline\":0,\"primeImport\":1,\"primeExport\":1,\"connectors\":4,\"nonDesktop\":1}"), "json: %s", t);
    d.non_desktop = 0;
    char l[48];
    CHECK(!strcmp(drmprobe_lease(&d, l, sizeof l), "no (no non-desktop connector)"), "lease: %s", l);

    /* names that aren't a card are refused before anything is opened */
    CHECK(drmprobe_card("../../etc/passwd", &d) == -1 && !d.probed, "path accepted");
    CHECK(drmprobe_card("card", &d) == -1 && drmprobe_card("card0x", &d) == -1 && drmprobe_card(NULL, &d) == -1, "bad names accepted");

    /* this machine's cards, when asked (DRMPROBE_LIVE=1) and readable: printed only */
    for (int i = 0; getenv("DRMPROBE_LIVE") && i < 4; i++) {
        char c[8];
        snprintf(c, sizeof c, "card%d", i);
        if (drmprobe_card(c, &d) != 0) continue;
        drmprobe_text(&d, "", t, sizeof t);
        printf("%s:\n%s", c, t);
        CHECK(d.planes < 0 || d.planes == d.primary + d.overlay + d.cursor || d.primary + d.overlay + d.cursor <= d.planes, "plane counts");
    }

    if (fails) { fprintf(stderr, "drmprobe_test: %d failed\n", fails); return 1; }
    printf("drmprobe_test: ok\n");
    return 0;
}
