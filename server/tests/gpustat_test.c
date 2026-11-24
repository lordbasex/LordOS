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
    /* the PCIe link, a model's name from argv, NVIDIA's information file */
    CHECK(gpu_link_speed("8.0 GT/s PCIe\n") == 80 && gpu_link_speed("2.5 GT/s PCIe") == 25 && gpu_link_speed("16 GT/s\n") == 160);
    CHECK(gpu_link_speed("32.0 GT/s PCIe") == 320 && gpu_link_speed("Unknown\n") == -1 && gpu_link_speed("8.0 GT/s PCIe garbage") == -1);
    CHECK(gpu_link_speed("8.05 GT/s") == -1 && gpu_link_speed("9999 GT/s") == -1 && gpu_link_speed("") == -1 && gpu_link_speed(".5 GT/s") == -1);
    char model[64];
    static const char a1[] = "llama-server\0-c\0008192\0-m\0/home/u/models/Qwen3-8B-Q4_K_M.gguf\0--port\0008080\0";
    gpu_argv_model(a1, sizeof a1 - 1, model, sizeof model);
    CHECK(!strcmp(model, "Qwen3-8B-Q4_K_M.gguf"));
    static const char a2[] = "x\0--model=/m/tiny.bin\0";
    gpu_argv_model(a2, sizeof a2 - 1, model, sizeof model);
    CHECK(!strcmp(model, "tiny.bin"));
    static const char a3[] = "x\0-m\0/m/evil\"<b>.gguf\0";          /* not a clean name: nothing */
    gpu_argv_model(a3, sizeof a3 - 1, model, sizeof model);
    CHECK(!model[0]);
    static const char a4[] = "x\0-m";                                   /* -m last, cut, no NUL */
    gpu_argv_model(a4, sizeof a4 - 1, model, sizeof model);
    CHECK(!model[0]);
    static const char a5[] = "x\0-m\0/m/\0";                           /* a folder: no base name */
    gpu_argv_model(a5, sizeof a5 - 1, model, sizeof model);
    CHECK(!model[0]);
    static const char a6[] = "x\0-m\0/m/..\0";
    gpu_argv_model(a6, sizeof a6 - 1, model, sizeof model);
    CHECK(!model[0]);
    char longname[200] = "x";
    memcpy(longname + 2, "-m", 3);
    memset(longname + 5, 'a', 120);
    longname[125] = 0;
    gpu_argv_model(longname, 126, model, sizeof model);
    CHECK(!model[0]);                                                  /* too long for the field: left out, never cut */
    GpuCard nv;
    memset(&nv, 0, sizeof nv);
    gpu_nv_info("Model: \t\t NVIDIA GeForce RTX 3060\nIRQ:   \t\t 140\nGPU UUID: \t GPU-1234\nVideo BIOS: \t 94.06.2f.00.9a\nBus Type: \t PCIe\n", &nv);
    CHECK(!strcmp(nv.nv_model, "NVIDIA GeForce RTX 3060") && !strcmp(nv.nv_vbios, "94.06.2f.00.9a"));
    memset(&nv, 0, sizeof nv);
    gpu_nv_info("Model: \x1b[31mred\nVideo BIOS: 0123456789012345678901234567890\n", &nv);
    CHECK(!nv.nv_model[0] && !nv.nv_vbios[0]);
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
    D("hwmon/hwmon3/temp1_crit", "94000\n");
    D("current_link_speed", "8.0 GT/s PCIe\n");
    D("current_link_width", "16\n");
    D("max_link_speed", "8.0 GT/s PCIe\n");
    D("max_link_width", "16\n");
    D("mem_busy_percent", "38\n");
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
    /* an Intel iGPU (i915), an Intel Arc (xe) and an NVIDIA card, each with what its driver reports */
    const char *igp = "sys/devices/pci0000:00/0000:00:02.0";
    snprintf(p, sizeof p, "%s/vendor", igp); put(p, "0x8086\n");
    snprintf(p, sizeof p, "%s/device", igp); put(p, "0x3e92\n");
    snprintf(p, sizeof p, "%s/current_link_speed", igp); put(p, "Unknown\n");
    snprintf(p, sizeof p, "%s/current_link_width", igp); put(p, "0\n");
    snprintf(p, sizeof p, "%s/driver", igp); link_to(p, "../../../bus/pci/drivers/i915");
    link_to("sys/class/drm/card2/device", "../../../devices/pci0000:00/0000:00:02.0");
    put("sys/class/drm/card2/gt_act_freq_mhz", "350\n");
    put("sys/class/drm/card2/gt_max_freq_mhz", "1200\n");
    put("sys/class/drm/card2/gt_RP0_freq_mhz", "1200\n");
    put("sys/class/drm/card2/power/rc6_residency_ms", "59211630\n");
    const char *arc = "sys/devices/pci0000:00/0000:00:1c.0/0000:03:00.0";
    snprintf(p, sizeof p, "%s/vendor", arc); put(p, "0x8086\n");
    snprintf(p, sizeof p, "%s/device", arc); put(p, "0xe20b\n");
    snprintf(p, sizeof p, "%s/current_link_speed", arc); put(p, "2.5 GT/s PCIe\n");
    snprintf(p, sizeof p, "%s/current_link_width", arc); put(p, "1\n");
    snprintf(p, sizeof p, "%s/max_link_speed", arc); put(p, "16.0 GT/s PCIe\n");
    snprintf(p, sizeof p, "%s/max_link_width", arc); put(p, "999\n");   /* out of range */
    snprintf(p, sizeof p, "%s/tile0/gt0/freq0/act_freq", arc); put(p, "2400\n");
    snprintf(p, sizeof p, "%s/tile0/gt0/freq0/max_freq", arc); put(p, "2850\n");
    snprintf(p, sizeof p, "%s/tile0/gt0/freq0/rp0_freq", arc); put(p, "2850\n");
    snprintf(p, sizeof p, "%s/tile0/gt0/gtidle/idle_residency_ms", arc); put(p, "1000\n");
    snprintf(p, sizeof p, "%s/tile0/gt0/gtidle/name", arc); put(p, "gt0-rc\n");
    snprintf(p, sizeof p, "%s/tile0/gt1/freq0/act_freq", arc); put(p, "0\n");
    snprintf(p, sizeof p, "%s/tile0/gt1/gtidle/name", arc); put(p, "gt1-\"mc\n");   /* not gtN-rc/mc: no kind */
    snprintf(p, sizeof p, "%s/driver", arc); link_to(p, "../../../../bus/pci/drivers/xe");
    link_to("sys/class/drm/card3/device", "../../../devices/pci0000:00/0000:00:1c.0/0000:03:00.0");
    const char *nvd = "sys/devices/pci0000:00/0000:00:03.0/0000:05:00.0";
    snprintf(p, sizeof p, "%s/vendor", nvd); put(p, "0x10de\n");
    snprintf(p, sizeof p, "%s/device", nvd); put(p, "0x2504\n");
    snprintf(p, sizeof p, "%s/driver", nvd); link_to(p, "../../../../bus/pci/drivers/nvidia");
    link_to("sys/class/drm/card4/device", "../../../devices/pci0000:00/0000:00:03.0/0000:05:00.0");
    put("proc/driver/nvidia/gpus/0000:05:00.0/information", "Model: \t\t NVIDIA GeForce RTX 3060\nIRQ:   \t\t 140\nVideo BIOS: \t 94.06.2f.00.9a\n");
    /* ROCm's KFD: the RX 580 is gpu 9989 */
    put("sys/class/kfd/kfd/topology/nodes/0/gpu_id", "0\n");
    put("sys/class/kfd/kfd/topology/nodes/1/gpu_id", "9989\n");
    put("sys/class/kfd/kfd/topology/nodes/1/properties", "cpu_cores_count 0\nsimd_count 144\nlocation_id 256\ndomain 0\nbogus\nlocation_id 99999999999999999999999\n");
    put("sys/class/kfd/kfd/topology/nodes/2/gpu_id", "77\n");
    put("sys/class/kfd/kfd/topology/nodes/2/properties", "location_id abc\ndomain 0\n");   /* no slot: skipped */
    put("sys/class/kfd/kfd/proc/4242/vram_9989", "5242880000\n");
    put("sys/class/kfd/kfd/proc/4242/sdma_9989", "1234\n");
    put("sys/class/kfd/kfd/proc/4242/stats_9989/cu_occupancy", "24\n");
    put("sys/class/kfd/kfd/proc/4242/vram_77x", "1\n");               /* not vram_<number>: skipped */
    put("sys/class/kfd/kfd/proc/4242/pasid", "32770\n");
    put("pci.ids", "1002  Advanced Micro Devices, Inc. [AMD/ATI]\n\t67df  Ellesmere [Radeon RX 470/480/570/570X/580/580X/590]\n");

    /* a game: two fds of one client, a third client, /dev/null */
    char me[16];
    snprintf(me, sizeof me, "%d", 4242);
    put("proc/4242/stat", "4242 (game (x)) S 1 4242 4242 0 -1 4194560 100 0 0 0 250 50 0 0 20 0 8 0 777 100000 2500 0\n");
    { static const char argv[] = "/usr/games/game\0--fast\0-m\0/home/u/models/Qwen3-8B-Q4_K_M.gguf\0"; char ap[300]; snprintf(ap, sizeof ap, "%s/proc/4242/cmdline", root);
      FILE *f = fopen(ap, "w"); if (f) { fwrite(argv, 1, sizeof argv - 1, f); fclose(f); } }
    put("proc/4242/maps", "7f-8f r-xp 00000000 08:02 1 /usr/lib/x86_64-linux-gnu/libvulkan.so.1\n7f-8f r-xp 0 08:02 2 /usr/lib/x86_64-linux-gnu/libvulkan_radeon.so\n");
    link_to("proc/4242/exe", "/usr/games/game");
    link_to("proc/4242/fd/5", "/dev/dri/renderD128");
    link_to("proc/4242/fd/6", "/dev/dri/renderD128");
    link_to("proc/4242/fd/7", "/dev/null");
    link_to("proc/4242/fd/8", "/dev/dri/card0");
    link_to("proc/4242/fd/9", "/dev/kfd");
    link_to("proc/4242/fd/10", "/dev/nvidia0");
    link_to("proc/4242/fd/11", "/dev/nvidiactl");
    link_to("proc/4242/fd/12", "/dev/nvidia99");                      /* too high a minor: ignored */
    put("proc/4242/fdinfo/9", "pos:\t0\n");
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
    put("proc/4600/stat", "4600 (llama-server) S 1 4600 4600 0 -1 0 0 0 0 0 900 100 0 0 20 0 4 0 80 1000 4096 0\n");
    { static const char argv[] = "llama-server\0-m\0/srv/m/Qwen3-4B-Q4_K_M.gguf\0"; char ap[300]; snprintf(ap, sizeof ap, "%s/proc/4600/cmdline", root);
      FILE *f = fopen(ap, "w"); if (f) { fwrite(argv, 1, sizeof argv - 1, f); fclose(f); } }
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
    CHECK(n == 4);
    GpuCard *g = &cards[0];
    CHECK(g->link_speed == 80 && g->link_width == 16 && g->link_max_speed == 80 && g->link_max_width == 16 && g->mem_busy == 38);
    CHECK(g->temp[0].crit_mc == 94000 && g->rp0_mhz == -1 && g->rc6_ms == -1 && g->ngt == 0);
    GpuCard *ig = &cards[1], *xe = &cards[2], *nvc = &cards[3];
    CHECK(!strcmp(ig->driver, "i915") && ig->cur_mhz == 350 && ig->max_mhz == 1200 && ig->rp0_mhz == 1200 && ig->rc6_ms == 59211630);
    CHECK(ig->link_speed == -1 && ig->link_width == -1);       /* "Unknown" and 0 lanes: an integrated GPU */
    CHECK(!strcmp(xe->driver, "xe") && xe->link_speed == 25 && xe->link_width == 1 && xe->link_max_width == -1 && xe->link_max_speed == -1);
    CHECK(xe->ngt == 2 && xe->gt[0].act == 2400 && xe->gt[0].rp0 == 2850 && xe->gt[0].idle_ms == 1000 && !strcmp(xe->gt[0].kind, "rc"));
    CHECK(xe->gt[1].gt == 1 && xe->gt[1].act == 0 && !xe->gt[1].kind[0] && xe->cur_mhz == 2400);
    CHECK(!strcmp(nvc->driver, "nvidia") && !strcmp(nvc->nv_model, "NVIDIA GeForce RTX 3060") && !strcmp(nvc->nv_vbios, "94.06.2f.00.9a"));
    CHECK(!strcmp(g->card, "card0") && !strcmp(g->driver, "amdgpu") && !strcmp(g->slot, "0000:01:00.0"));
    CHECK(!strcmp(g->vendor, "1002") && !strcmp(g->device, "67df") && strstr(g->dname, "Ellesmere") && strstr(g->vname, "AMD"));
    CHECK(g->render && g->busy == 37 && g->vram_used == 361758720 && g->vram_total == 8589934592ll && g->gtt_used == 45453312 && g->gtt_total == -1);
    CHECK(g->ntemp == 1 && g->temp[0].mc == 47000 && !strcmp(g->temp[0].label, "edge"));
    CHECK(g->fan_rpm == 755 && g->fan_max == 3700 && g->fan_pct == 33);
    CHECK(g->power_uw == 22135000 && g->cap_uw == 145000000);
    CHECK(g->sclk_mhz == 1329 && g->sclk_max == 1340 && g->mclk_mhz == 300 && g->mclk_max == 2000 && g->cur_mhz == -1);

    snprintf(home, sizeof home, "%s/home", root);
    char *j = gpu_json(root, ids, (unsigned)getuid(), 4242, home, "[{\"busId\":\"x\"}]");
    CHECK(j != NULL);
    if (!j) return;
    CHECK(strstr(j, "\"cards\":[{\"card\":\"card0\""));
    CHECK(strstr(j, "\"pid\":4242,\"ppid\":1,\"name\":\"game (x)\",\"cmd\":\"game\",\"exe\":\"/usr/games/game\",\"model\":\"Qwen3-8B-Q4_K_M.gguf\",\"self\":true"));
    CHECK(strstr(j, "\"kfd\":[{\"slot\":\"0000:01:00.0\",\"vram\":5242880000,\"sdmaUs\":1234,\"cu\":24}]"));
    CHECK(strstr(j, "\"nv\":[0]}"));
    CHECK(strstr(j, "\"link\":{\"gts\":8.0,\"width\":16}") && strstr(j, "\"memBusy\":38") && strstr(j, "\"crit\":94.0"));
    CHECK(strstr(j, "\"rc6Ms\":59211630") && strstr(j, "\"gts\":[{\"tile\":0,\"gt\":0,\"kind\":\"rc\",\"act\":2400"));
    CHECK(strstr(j, "\"nvModel\":\"NVIDIA GeForce RTX 3060\""));
    CHECK(strstr(j, "\"nvml\":[{\"busId\":\"x\"}]"));
    CHECK(strstr(j, "\"name\":\"llama-server\",\"state\":\"S\",\"own\":true,\"ticks\":1000,") && strstr(j, "\"model\":\"Qwen3-4B-Q4_K_M.gguf\"}"));
    CHECK(strstr(j, "\"hidden\":[]"));
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
    /* between rescans only the kept fds are read again: a new value shows, a new process doesn't yet */
    put("proc/4242/fdinfo/8", "drm-driver:\tamdgpu\ndrm-client-id:\t300\ndrm-pdev:\t0000:01:00.0\ndrm-engine-gfx:\t777 ns\n");
    put("proc/4700/stat", "4700 (newgame) S 1 4700 4700 0 -1 0 0 0 0 0 1 1 0 0 20 0 1 0 90 1000 100 0\n");
    link_to("proc/4700/fd/3", "/dev/dri/renderD128");
    put("proc/4700/fdinfo/3", "drm-driver:\tamdgpu\ndrm-client-id:\t400\ndrm-pdev:\t0000:01:00.0\n");
    j = gpu_json(root, ids, (unsigned)getuid(), 0, home, NULL);
    CHECK(j && strstr(j, "\"gfx\":777") && !strstr(j, "\"pid\":4700") && strstr(j, "\"nvml\":null"));
    free(j);
    gpu_cache_reset();
    j = gpu_json(root, ids, (unsigned)getuid(), 0, home, NULL);
    CHECK(j && strstr(j, "\"pid\":4700"));
    free(j);
    /* the pid reused by another process (its start time changed): dropped at once, before the next rescan */
    put("proc/4700/stat", "4700 (other) S 1 4700 4700 0 -1 0 0 0 0 0 1 1 0 0 20 0 1 0 91 1000 100 0\n");
    j = gpu_json(root, ids, (unsigned)getuid(), 0, home, NULL);
    CHECK(j && !strstr(j, "\"pid\":4700"));
    free(j);
    /* seen as another user: no process's fds are read, the KFD process is listed by pid and name only, no model */
    gpu_cache_reset();
    j = gpu_json(root, ids, (unsigned)getuid() + 1, 0, home, NULL);
    CHECK(j && strstr(j, "\"list\":[]") && strstr(j, "\"hidden\":[{\"pid\":4242,\"name\":\"game (x)\",\"kind\":\"kfd\"}]"));
    CHECK(j && strstr(j, "\"own\":false,\"ticks\":1000,\"rss\":") && !strstr(j, "Qwen3"));
    free(j);
    gpu_cache_reset();
    /* a note that isn't lord-say's own format, or isn't a regular file, is ignored */
    put("home/.cache/lordos/say/4400", "lord-say 1 vulkan hd es speak; rm -rf /\n");
    gpu_cache_reset();
    j = gpu_json(root, ids, (unsigned)getuid(), 0, home, "{\"not\":\"an array\"}");
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
