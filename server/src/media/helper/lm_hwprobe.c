/* lm_hwprobe.c — which video codecs the GPU decodes through VA-API (MK_HWPROBE; docs/VIDEO.md).
 *
 * lord-server passes the render nodes it opened (up to four, virtual GPUs left out); the sandbox
 * (with the DRM ioctls) is already on. The answer is one MM_RESULT with JSON: a codec counts if one
 * of the nodes' drivers lists one of its profiles with the VLD (decode) entrypoint. No driver, no
 * profiles: everything false. */
#include "lm.h"

#include <string.h>

#ifdef LM_VAAPI
#include <va/va.h>
#include <va/va_drm.h>

static void va_quiet(void *ctx, const char *msg) { (void)ctx; LM_DBG("va: %s", msg); }

static int decodes(VADisplay d, VAProfile p) {
    VAEntrypoint eps[32];
    int n = 0;
    if (vaMaxNumEntrypoints(d) > 32 || vaQueryConfigEntrypoints(d, p, eps, &n) != VA_STATUS_SUCCESS) return 0;
    for (int i = 0; i < n; i++) if (eps[i] == VAEntrypointVLD) return 1;
    return 0;
}
#endif

void lm_hwprobe(const int *render, int n_render) {
    int vaapi = 0, vp9 = 0, av1 = 0, h264 = 0, hevc = 0;
    char vendor[128] = "";
#ifdef LM_VAAPI
    for (int r = 0; r < n_render; r++) {
        VADisplay d = render[r] >= 0 ? vaGetDisplayDRM(render[r]) : NULL;
        int maj = 0, min = 0;
        if (d) {
            vaSetErrorCallback(d, va_quiet, NULL);
            vaSetInfoCallback(d, va_quiet, NULL);
        }
        if (d && vaInitialize(d, &maj, &min) == VA_STATUS_SUCCESS) {
            vaapi = 1;
            const char *v = vaQueryVendorString(d);
            if (v && !vendor[0]) snprintf(vendor, sizeof vendor, "%s", v);
            int max = vaMaxNumProfiles(d), n = 0;
            VAProfile ps[128];
            if (max > 0 && max <= 128 && vaQueryConfigProfiles(d, ps, &n) == VA_STATUS_SUCCESS) {
                for (int i = 0; i < n; i++) {
                    VAProfile p = ps[i];
                    if (!decodes(d, p)) continue;
                    if (p == VAProfileVP9Profile0 || p == VAProfileVP9Profile2) vp9 = 1;
                    if (p == VAProfileAV1Profile0) av1 = 1;
                    if (p == VAProfileH264Main || p == VAProfileH264High || p == VAProfileH264ConstrainedBaseline) h264 = 1;
                    if (p == VAProfileHEVCMain || p == VAProfileHEVCMain10) hevc = 1;
                }
            }
            vaTerminate(d);
        } else if (d) vaTerminate(d);
    }
#else
    (void)render;
    (void)n_render;
#endif
    int drmprime = 0;
    const AVCodec *hc = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    for (int i = 0; hc; i++) {
        const AVCodecHWConfig *cfg = avcodec_get_hw_config(hc, i);
        if (!cfg) break;
        if (cfg->device_type == AV_HWDEVICE_TYPE_DRM) drmprime = 1;
    }
    LmBuf b = { 0 };
    lm_buf_printf(&b, "{\"vaapi\":%s,\"drmprime\":%s,\"vendor\":", vaapi ? "true" : "false", drmprime ? "true" : "false");
    lm_buf_str(&b, vendor);
    lm_buf_printf(&b, ",\"vp9\":%s,\"av1\":%s,\"h264\":%s,\"hevc\":%s}", vp9 ? "true" : "false", av1 ? "true" : "false",
                  h264 ? "true" : "false", hevc ? "true" : "false");
    lm_result(0, 0, b.d ? b.d : "{}", -1);
    lm_buf_free(&b);
}
