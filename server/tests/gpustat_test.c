/* gpustat_test.c — Monitor → GPU's reader (src/monitor/gpustat.c): fdinfo of amdgpu, i915, xe and v3d clients,
 * pp_dpm levels, lord-say's note, maps lines, and a fake machine (an RX 580-like card with its hwmon, a process with
 * a client shared by two fds, an AI helper, a virtual machine, the installed APIs) and hostile values. */
#include "../src/monitor/gpustat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static char root[256];

static void mkparents(char *p) {
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
}
static void put(const char *path, const char *text) {
    char p[768];
    snprintf(p, sizeof p, "%s/%s", root, path);
    mkparents(p);
    FILE *f = fopen(p, "w");
    if (!f) { perror(p); exit(1); }
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}
static void link_to(const char *path, const char *target) {
    char p[768];
    snprintf(p, sizeof p, "%s/%s", root, path);
    mkparents(p);
    if (symlink(target, p)) { perror(p); exit(1); }
}

static const GpuKV *kv(const GpuKV *a, int n, const char *name) {
    for (int i = 0; i < n; i++) if (!strcmp(a[i].name, name)) return &a[i];
    return NULL;
}
static const GpuMem *mem(const GpuClient *c, const char *name) {
    for (int i = 0; i < c->nmem; i++) if (!strcmp(c->mem[i].name, name)) return &c->mem[i];
    return NULL;
}

static const char AMD[] =
    "pos:\t0\nflags:\t0100002\nmnt_id:\t26\nino:\t497\ndrm-driver:\tamdgpu\ndrm-client-id:\t217\ndrm-pdev:\t0000:01:00.0\npasid:\t217\n"
    "drm-memory-vram:\t174900 KiB\ndrm-memory-gtt: \t8192 KiB\ndrm-memory-cpu: \t0 KiB\namd-memory-visible-vram:\t174900 KiB\n"
    "drm-shared-vram:\t34560 KiB\ndrm-engine-gfx:\t24300500083 ns\ndrm-engine-compute:\t356992855 ns\ndrm-engine-dec:\t435950736 ns\n";

static void test_fdinfo(void) {
    GpuClient c;
    CHECK(!gpu_fdinfo_parse(AMD, strlen(AMD), &c));
    CHECK(!strcmp(c.driver, "amdgpu") && !strcmp(c.pdev, "0000:01:00.0") && c.has_id && c.id == 217);
    CHECK(c.neng == 3 && kv(c.eng, c.neng, "gfx")->v == 24300500083ull && kv(c.eng, c.neng, "dec")->v == 435950736ull);
    const GpuMem *v = mem(&c, "vram");
    CHECK(v && v->memory == 174900ll * 1024 && v->shared == 34560ll * 1024 && v->total == -1 && v->resident == -1);
    CHECK(mem(&c, "gtt")->memory == 8192 * 1024);
    CHECK(!mem(&c, "visible-vram"));                        /* amd-* lines are not the common keys */

    const char *i915 = "drm-driver:\ti915\ndrm-pdev:\t0000:00:02.0\ndrm-client-id:\t7\ndrm-engine-render:\t9288864723 ns\ndrm-engine-copy:\t0 ns\n"
                       "drm-engine-video:\t0 ns\ndrm-engine-capacity-video:\t2\ndrm-engine-video-enhance:\t0 ns\n"
                       "drm-total-system0:\t2304 KiB\ndrm-resident-system0:\t2304 KiB\ndrm-shared-system0:\t0\n";
    CHECK(!gpu_fdinfo_parse(i915, strlen(i915), &c));
    CHECK(c.neng == 4 && kv(c.eng, c.neng, "video-enhance") && kv(c.cap, c.ncap, "video")->v == 2);
    CHECK(mem(&c, "system0")->total == 2304 * 1024 && mem(&c, "system0")->resident == 2304 * 1024 && mem(&c, "system0")->shared == 0);

    const char *xe = "drm-driver:\txe\ndrm-client-id:\t40\ndrm-pdev:\t0000:03:00.0\ndrm-total-vram0:\t12 MiB\ndrm-resident-vram0:\t12 MiB\n"
                     "drm-cycles-rcs:\t28257900\ndrm-total-cycles-rcs:\t7655183225\ndrm-cycles-ccs:\t0\ndrm-total-cycles-ccs:\t7655183225\n";
    CHECK(!gpu_fdinfo_parse(xe, strlen(xe), &c));
    CHECK(c.neng == 0 && c.ncyc == 2 && c.ntcyc == 2 && kv(c.cyc, c.ncyc, "rcs")->v == 28257900 && kv(c.tcyc, c.ntcyc, "rcs")->v == 7655183225ull);
    CHECK(mem(&c, "vram0")->total == 12ll << 20);

    const char *v3d = "drm-driver:\tv3d\ndrm-client-id:\t3\ndrm-engine-bin:\t5000 ns\ndrm-engine-render:\t12000 ns\ndrm-engine-tfu:\t0 ns\n"
                      "drm-engine-csd:\t0 ns\ndrm-engine-cache_clean:\t0 ns\ndrm-total-memory:\t1024 KiB\n";
    CHECK(!gpu_fdinfo_parse(v3d, strlen(v3d), &c));
    CHECK(c.neng == 5 && !c.pdev[0] && kv(c.eng, c.neng, "cache_clean"));

    /* not a DRM client, and hostile values: each bad line is left out, never guessed */
    CHECK(gpu_fdinfo_parse("pos:\t0\nflags:\t02\n", 17, &c));
    const char *bad = "drm-driver:\tamd gpu\n";
    CHECK(gpu_fdinfo_parse(bad, strlen(bad), &c));
    const char *host = "drm-driver:\tamdgpu\ndrm-client-id:\t-1\ndrm-pdev:\t../../etc\ndrm-engine-gfx:\t99999999999999999999999 ns\n"
                       "drm-engine-x\"y:\t1 ns\ndrm-engine-ok:\t5 lightyears\ndrm-memory-vram:\t18446744073709551615 GiB\ndrm-memory-gtt:\t12abc\n"
                       "drm-engine-capacity-gfx:\t0\ndrm-engine-capacity-dma:\t1000\ndrm-engine-good:\t7\n"
                       "drm-engine-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa:\t1 ns\n";
    CHECK(!gpu_fdinfo_parse(host, strlen(host), &c));
    CHECK(!c.has_id && !c.pdev[0] && c.ncap == 0 && c.nmem == 0);
    CHECK(c.neng == 1 && kv(c.eng, c.neng, "good")->v == 7);
    /* more engines than kept: the rest are dropped */
    char many[4096] = "drm-driver:\tx\n";
    for (int i = 0; i < 40; i++) { char l[64]; snprintf(l, sizeof l, "drm-engine-e%d:\t%d ns\n", i, i); strcat(many, l); }
    CHECK(!gpu_fdinfo_parse(many, strlen(many), &c) && c.neng == GPU_KEYS);
    /* no newline at the end, and a cut file */
    CHECK(!gpu_fdinfo_parse("drm-driver:\tv3d", 15, &c) && !strcmp(c.driver, "v3d"));
    CHECK(!gpu_fdinfo_parse(AMD, 120, &c) && c.has_id && c.neng == 0);
}

static void test_small(void) {
    unsigned long long v;
    CHECK(!gpu_parse_u64("18446744073709551615\n", &v) && v == 18446744073709551615ull);
    CHECK(gpu_parse_u64("18446744073709551616", &v));
    CHECK(gpu_parse_u64("", &v) && gpu_parse_u64("-1", &v) && gpu_parse_u64("1 2", &v) && gpu_parse_u64("1\n\n", &v));
    int cur, max;
    gpu_dpm_parse("0: 300Mhz \n1: 600Mhz \n7: 1340Mhz *\n", &cur, &max);
    CHECK(cur == 1340 && max == 1340);
    gpu_dpm_parse("0: 300Mhz *\n1: 1000Mhz \n2: 2000Mhz \n", &cur, &max);
    CHECK(cur == 300 && max == 2000);
    gpu_dpm_parse("garbage\nS: 19Mhz *\n0: 99999999Mhz *\n", &cur, &max);
    CHECK(cur == -1 && max == -1);
    GpuSay s;
    CHECK(!gpu_say_parse("lord-say 1 vulkan hd es speak\n", &s) && !strcmp(s.backend, "vulkan") && !strcmp(s.quality, "hd") && !strcmp(s.lang, "es"));
    CHECK(!gpu_say_parse("lord-say 1 cpu standard en save", &s) && !strcmp(s.mode, "save"));
    CHECK(gpu_say_parse("lord-say 1 cuda hd es speak\n", &s));
    CHECK(gpu_say_parse("lord-say 1 cpu hd ES speak\n", &s));
    CHECK(gpu_say_parse("lord-say 1 cpu hd es speak\nmore", &s));
    CHECK(gpu_say_parse("lord-say 2 cpu hd es speak\n", &s));
    char libs[8][64];
    const char *maps =
        "7f1c2a000000-7f1c2a100000 r-xp 00000000 08:02 1234 /usr/lib/x86_64-linux-gnu/libvulkan_radeon.so\n"
        "7f1c2a100000-7f1c2a200000 r--p 00000000 08:02 1234 /usr/lib/x86_64-linux-gnu/libvulkan_radeon.so\n"
        "7f1c2b000000-7f1c2b100000 r-xp 00000000 08:02 99 /usr/lib/x86_64-linux-gnu/libvulkan.so.1.4.309\n"
        "7f1c2c000000-7f1c2c100000 r-xp 00000000 08:02 98 /usr/lib/x86_64-linux-gnu/libc.so.6\n"
        "7f1c2d000000-7f1c2d100000 r-xp 00000000 08:02 97 /usr/lib/x86_64-linux-gnu/dri/radeonsi_dri.so\n"
        "7f1c2e000000-7f1c2e100000 r-xp 00000000 08:02 96 /usr/lib/x86_64-linux-gnu/dri/radeonsi_drv_video.so\n"
        "7f1c2f000000-7f1c2f100000 rw-p 00000000 00:00 0 [heap]\n"
        "7f1c30000000-7f1c30100000 r-xp 00000000 08:02 95 /usr/lib/x86_64-linux-gnu/libnvidia-encode.so.550.54\n"
        "7f1c31000000-7f1c31100000 r-xp 00000000 08:02 94 /tmp/evil\"name/libcuda.so.1\n";
    int n = gpu_maps_libs(maps, strlen(maps), libs, 8);
    CHECK(n == 6);
    CHECK(n > 0 && !strcmp(libs[0], "libvulkan_radeon.so") && !strcmp(libs[1], "libvulkan.so.1.4.309") && !strcmp(libs[2], "radeonsi_dri.so"));
    CHECK(n > 5 && !strcmp(libs[3], "radeonsi_drv_video.so") && !strcmp(libs[4], "libnvidia-encode.so.550.54") && !strcmp(libs[5], "libcuda.so.1"));
    CHECK(gpu_maps_libs(maps, strlen(maps), libs, 2) == 2);
}

static void fake_machine(void) {
    const char *dev = "sys/devices/pci0000:00/0000:00:01.0/0000:01:00.0";
    char p[512];
#define D(f, v) do { snprintf(p, sizeof p, "%s/%s", dev, f); put(p, v); } while (0)
    D("vendor", "0x1002\n");
    D("device", "0x67df\n");
    D("gpu_busy_percent", "37\n");
    D("mem_info_vram_used", "361758720\n");
    D("mem_info_vram_total", "8589934592\n");
    D("mem_info_gtt_used", "45453312\n");
    D("mem_info_gtt_total", "abc\n");                       /* not a number: left out */
    D("pp_dpm_sclk", "0: 300Mhz \n7: 1340Mhz *\n");
    D("pp_dpm_mclk", "0: 300Mhz *\n2: 2000Mhz \n");
    D("drm/renderD128/dev", "226:128\n");
    D("hwmon/hwmon3/name", "amdgpu\n");
    D("hwmon/hwmon3/temp1_input", "47000\n");
    D("hwmon/hwmon3/temp1_label", "edge\n");
    D("hwmon/hwmon3/temp2_input", "999999999\n");          /* out of range */
    D("hwmon/hwmon3/fan1_input", "755\n");
    D("hwmon/hwmon3/fan1_max", "3700\n");
    D("hwmon/hwmon3/pwm1", "83\n");
    D("hwmon/hwmon3/pwm1_max", "255\n");
    D("hwmon/hwmon3/power1_input", "22135000\n");
    D("hwmon/hwmon3/power1_cap", "145000000\n");
    D("hwmon/hwmon3/freq1_input", "1329290000\n");
    D("hwmon/hwmon3/freq1_label", "sclk\n");
    D("hwmon/hwmon3/freq2_input", "300000000\n");
    D("hwmon/hwmon3/freq2_label", "mclk\n");
#undef D
    snprintf(p, sizeof p, "%s/driver", dev);
    link_to(p, "../../../../bus/pci/drivers/amdgpu");
    link_to("sys/class/drm/card0/device", "../../../devices/pci0000:00/0000:00:01.0/0000:01:00.0");
    link_to("sys/class/drm/card0-DP-1/x", "y");             /* a connector, not a card */
    put("sys/class/drm/card1/uevent", "");                  /* a card with no driver bound */
    put("pci.ids", "1002  Advanced Micro Devices, Inc. [AMD/ATI]\n\t67df  Ellesmere [Radeon RX 470/480/570/570X/580/580X/590]\n");

    /* a game: two fds of one client, a third client, /dev/null */
    char me[16];
    snprintf(me, sizeof me, "%d", 4242);
    put("proc/4242/stat", "4242 (game (x)) S 1 4242 4242 0 -1 4194560 100 0 0 0 250 50 0 0 20 0 8 0 777 100000 2500 0\n");
    put("proc/4242/cmdline", "/usr/games/game\0--fast\0");
    put("proc/4242/maps", "7f-8f r-xp 00000000 08:02 1 /usr/lib/x86_64-linux-gnu/libvulkan.so.1\n7f-8f r-xp 0 08:02 2 /usr/lib/x86_64-linux-gnu/libvulkan_radeon.so\n");
    link_to("proc/4242/exe", "/usr/games/game");
    link_to("proc/4242/fd/5", "/dev/dri/renderD128");
    link_to("proc/4242/fd/6", "/dev/dri/renderD128");
    link_to("proc/4242/fd/7", "/dev/null");
    link_to("proc/4242/fd/8", "/dev/dri/card0");
    put("proc/4242/fdinfo/5", AMD);
    put("proc/4242/fdinfo/6", AMD);
    put("proc/4242/fdinfo/7", "pos:\t0\n");
    put("proc/4242/fdinfo/8", "drm-driver:\tamdgpu\ndrm-client-id:\t300\ndrm-pdev:\t0000:01:00.0\ndrm-engine-gfx:\t5 ns\n");
    /* a process with no GPU */
    put("proc/4300/stat", "4300 (bash) S 1 4300 4300 0 -1 0 0 0 0 0 1 1 0 0 20 0 1 0 50 1000 100 0\n");
    /* the voice: lord-say with its note, lord-tts, a virtual machine */
    put("proc/4400/stat", "4400 (lord-say) S 1 4400 4400 0 -1 0 0 0 0 0 1 1 0 0 20 0 1 0 60 1000 100 0\n");
    put("proc/4401/stat", "4401 (lord-tts) R 4400 4400 4400 0 -1 0 0 0 0 0 1 1 0 0 20 0 1 0 61 1000 100 0\n");
    put("home/.cache/lordos/say/4400", "lord-say 1 vulkan hd es speak\n");
    put("proc/4500/stat", "4500 (qemu-system-x86) S 1 4500 4500 0 -1 0 0 0 0 0 400 100 0 0 20 0 4 0 70 1000 2048 0\n");
    /* the installed APIs */
    put("usr/share/vulkan/icd.d/radeon_icd.json", "{\"ICD\":{\"api_version\":\"1.4.305\",\"library_path\":\"libvulkan_radeon.so\"},\"file_format_version\":\"1.0.0\"}");
    put("usr/share/vulkan/icd.d/broken_icd.json", "{not json");
    put("usr/share/vulkan/icd.d/nvidia_icd.json", "{\"ICD\":{\"api_version\":\"1.3.277\",\"library_path\":\"/usr/lib/x86_64-linux-gnu/libGLX_nvidia.so.0\"},\"file_format_version\":\"1.0.0\"}");
    put("etc/OpenCL/vendors/rusticl.icd", "libRusticlOpenCL.so.1\n");
    put("usr/lib/x86_64-linux-gnu/dri/radeonsi_dri.so", "");
    put("usr/lib/x86_64-linux-gnu/dri/radeonsi_drv_video.so", "");
    put("usr/lib/x86_64-linux-gnu/libvulkan.so.1", "");
    put("usr/lib/x86_64-linux-gnu/libgallium-25.0.7-2+deb13u1.so", "");
    put("usr/lib/x86_64-linux-gnu/libcuda.so.1", "");
    put("proc/driver/nvidia/version", "NVRM version: NVIDIA UNIX x86_64 Kernel Module  550.54.14  Thu Feb 22 01:44:30 UTC 2024\nGCC version: 12\n");
}

static void test_machine(void) {
    fake_machine();
    char ids[400], home[300];
    snprintf(ids, sizeof ids, "%s/pci.ids", root);
    GpuCard cards[GPU_CARDS];
    int n = gpu_cards_read(root, ids, cards, GPU_CARDS);
    CHECK(n == 1);
    GpuCard *g = &cards[0];
    CHECK(!strcmp(g->card, "card0") && !strcmp(g->driver, "amdgpu") && !strcmp(g->slot, "0000:01:00.0"));
    CHECK(!strcmp(g->vendor, "1002") && !strcmp(g->device, "67df") && strstr(g->dname, "Ellesmere") && strstr(g->vname, "AMD"));
    CHECK(g->render && g->busy == 37 && g->vram_used == 361758720 && g->vram_total == 8589934592ll && g->gtt_used == 45453312 && g->gtt_total == -1);
    CHECK(g->ntemp == 1 && g->temp[0].mc == 47000 && !strcmp(g->temp[0].label, "edge"));
    CHECK(g->fan_rpm == 755 && g->fan_max == 3700 && g->fan_pct == 33);
    CHECK(g->power_uw == 22135000 && g->cap_uw == 145000000);
    CHECK(g->sclk_mhz == 1329 && g->sclk_max == 1340 && g->mclk_mhz == 300 && g->mclk_max == 2000 && g->cur_mhz == -1);

    snprintf(home, sizeof home, "%s/home", root);
    char *j = gpu_json(root, ids, (unsigned)getuid(), 4242, home);
    CHECK(j != NULL);
    if (!j) return;
    CHECK(strstr(j, "\"cards\":[{\"card\":\"card0\""));
    CHECK(strstr(j, "\"pid\":4242,\"ppid\":1,\"name\":\"game (x)\",\"cmd\":\"game\",\"exe\":\"/usr/games/game\",\"self\":true"));
    CHECK(strstr(j, "\"libs\":[\"libvulkan.so.1\",\"libvulkan_radeon.so\"]"));
    /* fds 5 and 6 are one client: listed once; fd 8 is another */
    const char *c217 = strstr(j, "\"id\":217");
    CHECK(c217 && !strstr(c217 + 1, "\"id\":217") && strstr(j, "\"id\":300"));
    CHECK(!strstr(j, "\"pid\":4300"));                      /* no GPU: not listed */
    CHECK(strstr(j, "\"name\":\"lord-tts\",\"state\":\"R\",\"own\":true"));
    CHECK(strstr(j, "\"say\":[{\"pid\":4400,\"state\":\"S\",\"backend\":\"vulkan\",\"quality\":\"hd\",\"lang\":\"es\",\"mode\":\"speak\"}]"));
    CHECK(strstr(j, "\"vms\":[{\"pid\":4500,\"ticks\":500,"));
    CHECK(strstr(j, "{\"file\":\"radeon_icd.json\",\"lib\":\"libvulkan_radeon.so\",\"api\":\"1.4.305\"}"));
    CHECK(strstr(j, "\"lib\":\"libGLX_nvidia.so.0\""));
    CHECK(!strstr(j, "broken_icd"));
    CHECK(strstr(j, "\"opencl\":[{\"file\":\"rusticl.icd\",\"lib\":\"libRusticlOpenCL.so.1\"}]"));
    CHECK(strstr(j, "\"dri\":[\"radeonsi_dri.so\"]") && strstr(j, "\"va\":[\"radeonsi_drv_video.so\"]"));
    CHECK(strstr(j, "libgallium-25.0.7-2+deb13u1.so") && strstr(j, "libcuda.so.1"));
    CHECK(strstr(j, "\"nvidia\":\"NVRM version: NVIDIA UNIX x86_64 Kernel Module  550.54.14"));
    free(j);
    /* a note that isn't lord-say's own format, or isn't a regular file, is ignored */
    put("home/.cache/lordos/say/4400", "lord-say 1 vulkan hd es speak; rm -rf /\n");
    j = gpu_json(root, ids, (unsigned)getuid(), 0, home);
    CHECK(j && strstr(j, "\"say\":[]"));
    free(j);
}

int main(void) {
    snprintf(root, sizeof root, "/tmp/gpustat-test-%d", (int)getpid());
    mkdir(root, 0755);
    test_fdinfo();
    test_small();
    test_machine();
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    if (system(cmd)) {}
    printf("gpustat-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
