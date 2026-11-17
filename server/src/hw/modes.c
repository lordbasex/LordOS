/* modes.c — KMS mode helpers (see modes.h). */
#include "modes.h"

#include <stdlib.h>

int mode_refresh_mhz(const struct drm_mode_modeinfo *m) {
    if (!m->htotal || !m->vtotal || !m->clock) return 0;
    long long num = (long long)m->clock * 1000000LL;     /* kHz → mHz numerator */
    long long den = (long long)m->htotal * m->vtotal;
    if (m->flags & DRM_MODE_FLAG_INTERLACE) num *= 2;
    if (m->flags & DRM_MODE_FLAG_DBLSCAN) den *= 2;
    if (m->vscan > 1) den *= m->vscan;
    return (int)((num + den / 2) / den);
}

static int interlaced(const struct drm_mode_modeinfo *m) { return (m->flags & DRM_MODE_FLAG_INTERLACE) != 0; }

int mode_preferred(const struct drm_mode_modeinfo *modes, int n) {
    if (n <= 0) return -1;
    for (int i = 0; i < n; i++) if (modes[i].type & DRM_MODE_TYPE_PREFERRED) return i;
    int best = 0;
    for (int i = 1; i < n; i++)
        if ((long)modes[i].hdisplay * modes[i].vdisplay > (long)modes[best].hdisplay * modes[best].vdisplay) best = i;
    return best;
}

/* GitHub #173: a 3440 × 1440 ultrawide whose EDID marks 3840 × 2160 as preferred (some do on some inputs) was opened
 * at 16:9 and the monitor stretched it, on the login screen at every boot. When the panel's physical size (EDID mm)
 * says another shape, the largest progressive mode of that shape wins. */
#define ASPECT_OFF 0.08                     /* the preferred mode is fine within 8 % of the panel's shape */
#define ASPECT_ON 0.03                      /* a mode that matches the shape: within 3 % */
static double aspect_off(const struct drm_mode_modeinfo *m, double phys) {
    double a = (double)m->hdisplay / m->vdisplay / phys;
    return a > 1 ? a - 1 : 1 - a;
}

int mode_native(const struct drm_mode_modeinfo *modes, int n, int mm_w, int mm_h) {
    int p = mode_preferred(modes, n);
    /* no size, or a bogus one (projectors say 0 or 1 cm; a few EDIDs store only 16:9 as 16 × 9 cm) */
    if (p < 0 || mm_w < 100 || mm_h < 100) return p;
    double phys = (double)mm_w / mm_h;
    if (!modes[p].hdisplay || !modes[p].vdisplay || aspect_off(&modes[p], phys) <= ASPECT_OFF) return p;
    int best = -1;
    for (int i = 0; i < n; i++) {
        if (!modes[i].hdisplay || !modes[i].vdisplay || interlaced(&modes[i]) || aspect_off(&modes[i], phys) > ASPECT_ON) continue;
        long a = (long)modes[i].hdisplay * modes[i].vdisplay, b = best < 0 ? 0 : (long)modes[best].hdisplay * modes[best].vdisplay;
        if (best < 0 || a > b || (a == b && mode_refresh_mhz(&modes[i]) > mode_refresh_mhz(&modes[best]))) best = i;
    }
    return best >= 0 ? best : p;
}

int mode_pick(const struct drm_mode_modeinfo *modes, int n, int w, int h, int refresh_mhz) {
    if (w <= 0 || h <= 0) {
        int p = mode_preferred(modes, n);
        if (p < 0) return -1;
        w = modes[p].hdisplay;
        h = modes[p].vdisplay;
        if (refresh_mhz <= 0) return p;
    }
    int best = -1;
    long best_score = 0;
    for (int i = 0; i < n; i++) {
        if (modes[i].hdisplay != w || modes[i].vdisplay != h) continue;
        int r = mode_refresh_mhz(&modes[i]);
        /* lower is better: interlace penalty, then distance from the wanted rate (or highest) */
        long score = interlaced(&modes[i]) ? 100000000L : 0;
        score += refresh_mhz > 0 ? labs((long)r - refresh_mhz) : 10000000L - r;
        if (modes[i].type & DRM_MODE_TYPE_PREFERRED) score -= 1;      /* tie → the preferred one */
        if (best < 0 || score < best_score) { best = i; best_score = score; }
    }
    return best;
}

int mode_resolutions(const struct drm_mode_modeinfo *modes, int n, int *out, int max) {
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (interlaced(&modes[i])) continue;
        int j = 0;
        while (j < k && (modes[out[j]].hdisplay != modes[i].hdisplay || modes[out[j]].vdisplay != modes[i].vdisplay)) j++;
        if (j < k) {
            if (mode_refresh_mhz(&modes[i]) > mode_refresh_mhz(&modes[out[j]])) out[j] = i;
            continue;
        }
        if (k < max) out[k++] = i;
    }
    /* largest area first (insertion sort: k is small) */
    for (int a = 1; a < k; a++) {
        int v = out[a], b = a;
        long av = (long)modes[v].hdisplay * modes[v].vdisplay;
        while (b > 0 && (long)modes[out[b - 1]].hdisplay * modes[out[b - 1]].vdisplay < av) { out[b] = out[b - 1]; b--; }
        out[b] = v;
    }
    return k;
}

int mode_rates(const struct drm_mode_modeinfo *modes, int n, int w, int h, int *out, int max) {
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (modes[i].hdisplay != w || modes[i].vdisplay != h || interlaced(&modes[i])) continue;
        int r = mode_refresh_mhz(&modes[i]), j = 0;
        while (j < k && abs(out[j] - r) > 5) j++;          /* same rate within 0.005 Hz */
        if (j == k && k < max) out[k++] = r;
    }
    for (int a = 1; a < k; a++) {
        int v = out[a], b = a;
        while (b > 0 && out[b - 1] < v) { out[b] = out[b - 1]; b--; }
        out[b] = v;
    }
    return k;
}
