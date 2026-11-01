/* hwdec.c — can this computer decode VP9 and AV1 video in hardware? (docs/VIDEO.md)
 *
 * The answer drives "Optimize video for this computer" (Settings → Displays): with Automatic,
 * the browsers are asked for H.264 instead of the codecs the hardware can't decode. Asked once per
 * boot and cached in ~/.cache/lordos/video-hw.json (keyed by /proc/sys/kernel/random/boot_id), since
 * a live USB boots on a different computer every time.
 *
 *   1. Virtual GPU (virtio-gpu, VMware SVGA, QXL, Bochs, Hyper-V, VirtualBox): no, without asking.
 *      A virtual machine with a real GPU passed through is probed like any other computer.
 *   2. VA-API: lord-media (the only binary that links libva) receives the render nodes already open
 *      and lists the profiles with a decode entrypoint (lm_hwprobe.c, MK_HWPROBE).
 *   3. V4L2 memory-to-memory decoders (ARM boards): VP9/AV1/H.264/HEVC as stateless (VP9F, AV1F,
 *      S264, S265) or stateful (VP90, AV01, H264, HEVC) formats on the OUTPUT queue. The Raspberry
 *      Pi 5 only lists HEVC there, so VP9 and AV1 come out "no".
 * No driver, no profiles or a helper that doesn't answer within 5 s: "no". */
#include "hwdec.h"
#include "media_int.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <drm/drm.h>

#define PROBE_TIMEOUT_MS 5000

static struct {
    int state;                  /* 0 not asked, 1 probing, 2 done */
    int sock;
    pid_t pid;
    double deadline;
    char boot[40], vm[64], gpu[48], vendor[128];
    int virt;                   /* the only GPU is a virtual one */
    int vaapi, va_vp9, va_av1, va_h264, va_hevc, drmprime;
    int v4l2, v4_vp9, v4_av1, v4_h264, v4_hevc;
    char json[1024];
} H = { .sock = -1 };

static double mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void read_line(const char *path, char *out, size_t n) {
    out[0] = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    ssize_t r = read(fd, out, n - 1);
    close(fd);
    if (r <= 0) { out[0] = 0; return; }
    out[r] = 0;
    out[strcspn(out, "\n")] = 0;
}

int hwdec_virtual_gpu(const char *driver) {
    static const char *const v[] = { "virtio_gpu", "vmwgfx", "qxl", "bochs-drm", "bochs", "cirrus", "cirrus-qemu",
                                     "hyperv_drm", "vboxvideo", "simpledrm", "vkms", "xlnx", NULL };
    if (!driver || !*driver) return 1;
    for (int i = 0; v[i]; i++) if (!strcmp(driver, v[i])) return 1;
    return 0;
}

int hwdec_vm_name(const char *vendor, const char *product, int hypervisor_flag, char *out, size_t n) {
    static const struct { const char *needle, *name; } k[] = {
        { "QEMU", "qemu" }, { "VMware", "vmware" }, { "innotek", "virtualbox" }, { "VirtualBox", "virtualbox" },
        { "Xen", "xen" }, { "Parallels", "parallels" }, { "Bochs", "bochs" }, { "KVM", "kvm" },
        { "Virtual Machine", "hyperv" }, { NULL, NULL } };
    out[0] = 0;
    for (int i = 0; k[i].needle; i++)
        if ((vendor && strstr(vendor, k[i].needle)) || (product && strstr(product, k[i].needle))) { snprintf(out, n, "%s", k[i].name); return 1; }
    if (hypervisor_flag) { snprintf(out, n, "other"); return 1; }
    return 0;
}

static int cpu_hypervisor_flag(void) {
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) return 0;
    char line[4096];
    int yes = 0;
    while (!yes && fgets(line, sizeof line, f))
        if (!strncmp(line, "flags", 5) && strstr(line, " hypervisor")) yes = 1;
    fclose(f);
    return yes;
}

static void drm_driver(int fd, char *out, size_t n) {
    char name[48] = "";
    struct drm_version v;
    memset(&v, 0, sizeof v);
    v.name = name;
    v.name_len = sizeof name - 1;
    if (ioctl(fd, DRM_IOCTL_VERSION, &v) == 0) name[v.name_len < sizeof name ? v.name_len : sizeof name - 1] = 0;
    else name[0] = 0;
    snprintf(out, n, "%s", name);
}

/* ---- V4L2 decoders ---- */

#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

static void v4l2_probe(void) {
    for (int i = 0; i < 64; i++) {
        char p[32];
        snprintf(p, sizeof p, "/dev/video%d", i);
        int fd = open(p, O_RDWR | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) continue;
        struct v4l2_capability cap;
        memset(&cap, 0, sizeof cap);
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap)) { close(fd); continue; }
        uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
        if (!(caps & (V4L2_CAP_VIDEO_M2M | V4L2_CAP_VIDEO_M2M_MPLANE))) { close(fd); continue; }
        static const uint32_t types[] = { V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_BUF_TYPE_VIDEO_OUTPUT };
        for (int t = 0; t < 2; t++) {
            for (uint32_t k = 0; k < 64; k++) {
                struct v4l2_fmtdesc f;
                memset(&f, 0, sizeof f);
                f.index = k;
                f.type = types[t];
                if (ioctl(fd, VIDIOC_ENUM_FMT, &f)) break;
                uint32_t pf = f.pixelformat;
                if (pf == FOURCC('V', 'P', '9', 'F') || pf == FOURCC('V', 'P', '9', '0')) H.v4_vp9 = 1;
                else if (pf == FOURCC('A', 'V', '1', 'F') || pf == FOURCC('A', 'V', '0', '1')) H.v4_av1 = 1;
                else if (pf == FOURCC('S', '2', '6', '4') || pf == FOURCC('H', '2', '6', '4')) H.v4_h264 = 1;
                else if (pf == FOURCC('S', '2', '6', '5') || pf == FOURCC('H', 'E', 'V', 'C')) H.v4_hevc = 1;
                else continue;
                H.v4l2 = 1;
            }
        }
        close(fd);
    }
}

/* ---- result ---- */

static const char *b(int v) { return v ? "true" : "false"; }

static void build_json(void) {
    char vm[80], gpu[64], vendor[200];
    media_json_str(vm, sizeof vm, H.vm);
    media_json_str(gpu, sizeof gpu, H.gpu);
    media_json_str(vendor, sizeof vendor, H.vendor);
    int vp9 = !H.virt && (H.va_vp9 || H.v4_vp9), av1 = !H.virt && (H.va_av1 || H.v4_av1);
    /* a V4L2 decoder only counts for HEVC when this FFmpeg can drive it (lm_hwprobe.c) */
    int h264 = !H.virt && (H.va_h264 || H.v4_h264), hevc = !H.virt && (H.va_hevc || (H.v4_hevc && H.drmprime));
    const char *why = H.virt ? "virtualGpu" : (vp9 && av1) ? "ok" : (!H.vaapi && !H.v4l2) ? "noDriver" : "noProfiles";
    snprintf(H.json, sizeof H.json,
             "{\"boot\":\"%s\",\"vm\":%s,\"gpu\":%s,\"virtualGpu\":%s,\"vaapi\":%s,\"vendor\":%s,\"v4l2\":%s,"
             "\"vp9\":%s,\"av1\":%s,\"h264\":%s,\"hevc\":%s,\"reason\":\"%s\"}",
             H.boot, vm, gpu, b(H.virt), b(H.vaapi), vendor, b(H.v4l2), b(vp9), b(av1), b(h264), b(hevc), why);
}

static void cache_path(char *out, size_t n) { snprintf(out, n, "%s/.cache/lordos/video-hw.json", media_home()); }

static void save_cache(void) {
    char dir[4096], path[4200], tmp[4300];
    snprintf(dir, sizeof dir, "%s/.cache", media_home());
    mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/.cache/lordos", media_home());
    mkdir(dir, 0700);
    cache_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.%d.part", path, (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return;
    size_t len = strlen(H.json);
    int ok = write(fd, H.json, len) == (ssize_t)len;
    close(fd);
    if (!ok || rename(tmp, path)) unlink(tmp);
}

/* The cached answer if it was written in this boot (and looks like ours). */
static int load_cache(void) {
    char path[4200];
    cache_path(path, sizeof path);
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0;
    char buf[sizeof H.json];
    ssize_t r = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (r <= 0) return 0;
    buf[r] = 0;
    char want[64];
    snprintf(want, sizeof want, "{\"boot\":\"%s\",", H.boot);
    if (!H.boot[0] || strncmp(buf, want, strlen(want)) || buf[r - 1] != '}' || strchr(buf, '\n')) return 0;
    memcpy(H.json, buf, (size_t)r + 1);
    return 1;
}

static void finish(void) {
    if (H.sock >= 0) { close(H.sock); H.sock = -1; }
    if (H.pid > 0) {
        if (waitpid(H.pid, NULL, WNOHANG) != H.pid) { kill(H.pid, SIGKILL); waitpid(H.pid, NULL, 0); }
        H.pid = 0;
    }
    build_json();
    save_cache();
    H.state = 2;
    fprintf(stderr, "video: hardware decoding %s\n", H.json);
}

static void start(void) {
    H.state = 1;
    read_line("/proc/sys/kernel/random/boot_id", H.boot, sizeof H.boot);
    for (char *p = H.boot; *p; p++) if (!strchr("0123456789abcdef-", *p)) { H.boot[0] = 0; break; }
    if (load_cache()) { H.state = 2; return; }

    char vendor[128], product[128];
    read_line("/sys/class/dmi/id/sys_vendor", vendor, sizeof vendor);
    read_line("/sys/class/dmi/id/product_name", product, sizeof product);
    hwdec_vm_name(vendor, product, cpu_hypervisor_flag(), H.vm, sizeof H.vm);

    /* the render nodes that belong to a real GPU go to lord-media */
    int fds[MP_MAX_FDS], nfds = 0, any = 0;
    int allow = strcmp(media_hwaccel(), "none") != 0;
    for (int i = 128; i < 136 && nfds < MP_MAX_FDS; i++) {
        char p[32], drv[48];
        snprintf(p, sizeof p, "/dev/dri/renderD%d", i);
        int fd = open(p, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        drm_driver(fd, drv, sizeof drv);
        if (!any++) snprintf(H.gpu, sizeof H.gpu, "%s", drv);
        if (hwdec_virtual_gpu(drv) || !allow) { close(fd); continue; }
        if (nfds == 0) snprintf(H.gpu, sizeof H.gpu, "%s", drv);
        fds[nfds++] = fd;
    }
    H.virt = any && !nfds && allow;
    v4l2_probe();
    if (!nfds) { finish(); return; }
    H.sock = media_spawn(MK_HWPROBE, NULL, fds, nfds, &H.pid);
    for (int i = 0; i < nfds; i++) close(fds[i]);
    if (H.sock < 0) { finish(); return; }
    H.deadline = mono_ms() + PROBE_TIMEOUT_MS;
}

/* "vp9":true inside lord-media's JSON (fixed keys, written by lm_hwprobe.c). */
static int jbool(const char *json, const char *key) {
    char k[32];
    snprintf(k, sizeof k, "\"%s\":true", key);
    return strstr(json, k) != NULL;
}

void hwdec_dispatch(void) {
    if (H.state != 1 || H.sock < 0) return;
    static char payload[MP_PAYLOAD_MAX + 1];
    MediaMsg m;
    int fds[MP_MAX_FDS];
    int r = mp_recv(H.sock, &m, payload, sizeof payload, fds, 0);
    for (int i = 0; r > 0 && i < MP_MAX_FDS; i++) if (fds[i] >= 0) close(fds[i]);
    if (r < 0) return;                              /* nothing yet */
    if (r > 0 && m.type == MM_RESULT && payload[0] == '{') {
        H.vaapi = jbool(payload, "vaapi");
        H.va_vp9 = jbool(payload, "vp9");
        H.va_av1 = jbool(payload, "av1");
        H.va_h264 = jbool(payload, "h264");
        H.va_hevc = jbool(payload, "hevc");
        H.drmprime = jbool(payload, "drmprime");
        const char *v = strstr(payload, "\"vendor\":\"");
        if (v) {
            v += 10;
            size_t n = 0;
            while (v[n] && v[n] != '"' && v[n] != '\\' && n < sizeof H.vendor - 1) n++;
            memcpy(H.vendor, v, n);
            H.vendor[n] = 0;
        }
    }
    finish();
}

int hwdec_pollfd(void) {
    if (H.state == 1 && H.sock >= 0 && mono_ms() > H.deadline) {
        fprintf(stderr, "video: the decoder probe didn't answer; taking \"no\"\n");
        finish();
    }
    return H.state == 1 ? H.sock : -1;
}

const char *hwdec_result(void) {
    if (H.state == 0) start();
    if (H.state == 1) hwdec_pollfd();               /* the timeout, also without main loop events */
    return H.state == 2 ? H.json : NULL;
}

void hwdec_shutdown(void) {
    if (H.state == 1) { H.state = 0; if (H.sock >= 0) close(H.sock); H.sock = -1; if (H.pid > 0) { kill(H.pid, SIGKILL); waitpid(H.pid, NULL, 0); } H.pid = 0; }
}
