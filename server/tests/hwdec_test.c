/* hwdec_test.c — the pure parts of the hardware decoding probe (src/media/hwdec.c, docs/VIDEO.md):
 * which DRM drivers are virtual GPUs and how a virtual machine is recognized. Run by `make test`. */
#include "media/hwdec.h"
#include "media/media_int.h"

#include <stdio.h>
#include <string.h>

/* hwdec.c's other neighbours in axia-server, not used by these tests */
int media_spawn(int kind, const char *params, const int *fds, int nfds, pid_t *pid) { (void)kind; (void)params; (void)fds; (void)nfds; (void)pid; return -1; }
const char *media_home(void) { return "/nonexistent"; }
const char *media_hwaccel(void) { return "vaapi"; }
void media_json_str(char *out, size_t n, const char *s) { snprintf(out, n, "\"%s\"", s); }

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void) {
    CHECK(hwdec_virtual_gpu("virtio_gpu") && hwdec_virtual_gpu("vmwgfx") && hwdec_virtual_gpu("qxl") && hwdec_virtual_gpu(""));
    CHECK(hwdec_virtual_gpu(NULL) && hwdec_virtual_gpu("hyperv_drm") && hwdec_virtual_gpu("vboxvideo"));
    CHECK(!hwdec_virtual_gpu("i915") && !hwdec_virtual_gpu("xe") && !hwdec_virtual_gpu("amdgpu") && !hwdec_virtual_gpu("nouveau"));
    CHECK(!hwdec_virtual_gpu("v3d") && !hwdec_virtual_gpu("vc4") && !hwdec_virtual_gpu("panfrost"));
    char n[32];
    CHECK(hwdec_vm_name("QEMU", "Standard PC (Q35 + ICH9, 2009)", 1, n, sizeof n) && !strcmp(n, "qemu"));
    CHECK(hwdec_vm_name("VMware, Inc.", "VMware20,1", 1, n, sizeof n) && !strcmp(n, "vmware"));
    CHECK(hwdec_vm_name("innotek GmbH", "VirtualBox", 1, n, sizeof n) && !strcmp(n, "virtualbox"));
    CHECK(hwdec_vm_name("Microsoft Corporation", "Virtual Machine", 1, n, sizeof n) && !strcmp(n, "hyperv"));
    CHECK(hwdec_vm_name("Some Cloud", "Instance", 1, n, sizeof n) && !strcmp(n, "other"));
    CHECK(!hwdec_vm_name("LENOVO", "20XW", 0, n, sizeof n) && !n[0]);
    CHECK(!hwdec_vm_name("", "", 0, n, sizeof n));          /* Raspberry Pi: no DMI, no flag */
    printf("hwdec: %d/%d passed\n", checks - fails, checks);
    return fails != 0;
}
