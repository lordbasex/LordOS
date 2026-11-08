/* h_ocr.c — lord-ocr: the text in a picture (Screenshots' "copy the text", docs/MEDIA.md), with Tesseract.
 *
 * lord-ocr is lord-doc's helper loop (helper.c, built with LORD_HELPER_OCR) with only this part: the only binary
 * that links libtesseract (and what it brings: Leptonica, libarchive, libcurl, libgomp), so neither lord-server nor
 * lord-doc does. It gets the pixels in a memfd lord-server wrote, never a path; its sandbox reads /usr only (the
 * language data in /usr/share/tesseract-ocr) and has no network.
 *
 * DQ_OCR: fd (w × h premultiplied ARGB32, stride w*4) + {"w","h","langs":"eng+spa"} → {"text"}. The picture is
 * turned to gray, made dark-on-light (Tesseract reads that best) and enlarged to twice its size when it is a
 * screen's small text, then read in the given languages. */
#include "helper.h"

#include <tesseract/capi.h>

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define OCR_MAX_SIDE 8192          /* as DOC_IMG_MAX: the largest side read */

/* "eng", "spa+eng": a few lowercase language codes joined by '+' (the tessdata file names) */
static int valid_langs(const char *s) {
    size_t n = s ? strlen(s) : 0, run = 0;
    if (!n || n > 64) return 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '+') { if (run < 3) return 0; run = 0; continue; }
        if (!((s[i] >= 'a' && s[i] <= 'z') || s[i] == '_')) return 0;
        if (++run > 16) return 0;
    }
    return run >= 3;
}

/* Gray, dark text on a light background, at scale k (1 or 2: bilinear). NULL if out of memory. */
static uint8_t *prepare(const uint32_t *px, int w, int h, int k) {
    uint8_t *g = malloc((size_t)w * h);
    if (!g) return NULL;
    uint64_t sum = 0;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint32_t p = px[i], a = p >> 24;
        uint32_t r = (p >> 16) & 255, gr = (p >> 8) & 255, b = p & 255;
        uint32_t y = (r * 77 + gr * 150 + b * 29) >> 8;      /* premultiplied: over black */
        y += 255 - a;                                         /* transparent → white */
        g[i] = (uint8_t)(y > 255 ? 255 : y);
        sum += g[i];
    }
    if (sum / ((uint64_t)w * h) < 128) for (size_t i = 0; i < (size_t)w * h; i++) g[i] = (uint8_t)(255 - g[i]);   /* light on dark */
    if (k == 1) return g;
    int W = w * k, H = h * k;
    uint8_t *o = malloc((size_t)W * H);
    if (!o) { free(g); return NULL; }
    for (int y = 0; y < H; y++) {
        float fy = (y + 0.5f) / k - 0.5f;
        int y0 = fy < 0 ? 0 : (int)fy, y1 = y0 + 1 < h ? y0 + 1 : h - 1;
        float ty = fy - y0 < 0 ? 0 : fy - y0;
        for (int x = 0; x < W; x++) {
            float fx = (x + 0.5f) / k - 0.5f;
            int x0 = fx < 0 ? 0 : (int)fx, x1 = x0 + 1 < w ? x0 + 1 : w - 1;
            float tx = fx - x0 < 0 ? 0 : fx - x0;
            float top = g[(size_t)y0 * w + x0] * (1 - tx) + g[(size_t)y0 * w + x1] * tx;
            float bot = g[(size_t)y1 * w + x0] * (1 - tx) + g[(size_t)y1 * w + x1] * tx;
            o[(size_t)y * W + x] = (uint8_t)(top * (1 - ty) + bot * ty + 0.5f);
        }
    }
    free(g);
    return o;
}

int ocr_handle(const DocMsg *m, const JVal *args) {
    if (m->h.op != DQ_OCR) return 0;
    int w = (int)json_num(args, "w", 0), h = (int)json_num(args, "h", 0);
    const char *langs = json_str(args, "langs");
    if (m->nfds < 1 || w < 1 || h < 1 || w > OCR_MAX_SIDE || h > OCR_MAX_SIDE || !valid_langs(langs)) {
        reply_err(m->h.id, "bad-request", "");
        return 1;
    }
    struct stat st;
    size_t need = (size_t)w * h * 4;
    if (fstat(m->fds[0], &st) || (size_t)st.st_size < need) { reply_err(m->h.id, "bad-image", ""); return 1; }
    const uint32_t *px = mmap(NULL, need, PROT_READ, MAP_SHARED, m->fds[0], 0);
    if (px == MAP_FAILED) { reply_err(m->h.id, "bad-image", ""); return 1; }
    /* a screen's text is ~10 px high: Tesseract reads it far better twice as large */
    int k = h <= 1200 && w * 2 <= OCR_MAX_SIDE && h * 2 <= OCR_MAX_SIDE ? 2 : 1;
    uint8_t *g = prepare(px, w, h, k);
    munmap((void *)px, need);
    if (!g) { reply_err(m->h.id, "memory", ""); return 1; }
    TessBaseAPI *api = TessBaseAPICreate();
    if (!api || TessBaseAPIInit3(api, NULL, langs)) {
        if (api) TessBaseAPIDelete(api);
        free(g);
        reply_err(m->h.id, "no-language", langs);
        return 1;
    }
    TessBaseAPISetPageSegMode(api, PSM_AUTO);
    TessBaseAPISetImage(api, g, w * k, h * k, 1, w * k);
    TessBaseAPISetSourceResolution(api, 96 * k);
    char *text = TessBaseAPIGetUTF8Text(api);
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"text\":");
    jb_str(&b, text ? text : "");
    jb_raw(&b, "}");
    if (text) TessDeleteText(text);
    TessBaseAPIEnd(api);
    TessBaseAPIDelete(api);
    free(g);
    reply_json(DR_DONE, m->h.id, &b);
    return 1;
}
