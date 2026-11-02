/* hwdec.h — hardware VP9/AV1 decoding probe, once per boot (see hwdec.c and docs/VIDEO.md). */
#ifndef LORD_MEDIA_HWDEC_H
#define LORD_MEDIA_HWDEC_H

#include <stddef.h>

/* JSON with the answer, or NULL while axia-media is still probing (the first call starts it):
 * {"boot","vm","gpu","virtualGpu","vaapi","vendor","v4l2","vp9","av1","h264","hevc","reason"} */
const char *hwdec_result(void);
int  hwdec_pollfd(void);                /* -1 when nothing is pending (main loop, via media_pollfds) */
void hwdec_dispatch(void);
void hwdec_shutdown(void);

/* Pure helpers, exposed for tests. */
int hwdec_virtual_gpu(const char *drm_driver);          /* virtio_gpu, vmwgfx, qxl… (or empty) */
int hwdec_vm_name(const char *sys_vendor, const char *product, int hypervisor_flag, char *out, size_t n);

#endif
