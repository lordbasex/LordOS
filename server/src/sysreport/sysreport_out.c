/* sysreport_out.c — the report as JSON (schema 1) and as a Markdown summary (see sysreport.h). Both
 * print only the structure's fields, so neither can carry more than the collector kept. */
#include "sysreport.h"
#include "../hw/jbuf.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

/* ---- JSON ---- */

static void jkey(HBuf *b, int *first, const char *key) {
    hb_printf(b, "%s\"%s\":", *first ? "" : ",", key);
    *first = 0;
}
static void js(HBuf *b, int *first, const char *key, const char *v) { jkey(b, first, key); hb_str(b, v); }
static void ji(HBuf *b, int *first, const char *key, long long v) { jkey(b, first, key); hb_printf(b, "%lld", v); }
static void jb(HBuf *b, int *first, const char *key, int v) { jkey(b, first, key); hb_printf(b, "%s", v ? "true" : "false"); }
/* -1 (or INT32_MIN) means unknown: null */
static void jn(HBuf *b, int *first, const char *key, long long v, long long unknown) {
    jkey(b, first, key);
    if (v == unknown) hb_printf(b, "null"); else hb_printf(b, "%lld", v);
}

char *sysreport_json(const SysReport *r) {
    HBuf b = { 0 };
    int f = 1, g;
    hb_printf(&b, "{");
    ji(&b, &f, "schema", r->schema);
    js(&b, &f, "date", r->date);

    const SrMachine *m = &r->machine;
    jkey(&b, &f, "machine"); hb_printf(&b, "{"); g = 1;
    js(&b, &g, "vendor", m->vendor); js(&b, &g, "product", m->product); js(&b, &g, "version", m->version);
    js(&b, &g, "boardVendor", m->board_vendor); js(&b, &g, "boardName", m->board_name); js(&b, &g, "boardVersion", m->board_version);
    js(&b, &g, "chassis", m->chassis); js(&b, &g, "model", m->model);
    js(&b, &g, "biosVendor", m->bios_vendor); js(&b, &g, "biosVersion", m->bios_version); js(&b, &g, "biosDate", m->bios_date);
    js(&b, &g, "firmware", m->firmware);
    jkey(&b, &g, "secureBoot"); hb_printf(&b, "%s", m->secure_boot < 0 ? "null" : m->secure_boot ? "true" : "false");
    js(&b, &g, "virtualization", m->virt);
    hb_printf(&b, "}");

    const SrCpu *c = &r->cpu;
    jkey(&b, &f, "cpu"); hb_printf(&b, "{"); g = 1;
    js(&b, &g, "model", c->model); js(&b, &g, "vendor", c->vendor); js(&b, &g, "arch", c->arch);
    ji(&b, &g, "sockets", c->sockets); ji(&b, &g, "cores", c->cores); ji(&b, &g, "threads", c->threads);
    ji(&b, &g, "minMHz", c->min_mhz); ji(&b, &g, "maxMHz", c->max_mhz); js(&b, &g, "governor", c->governor);
    ji(&b, &g, "l1dKB", c->l1d_kb); ji(&b, &g, "l1iKB", c->l1i_kb); ji(&b, &g, "l2KB", c->l2_kb); ji(&b, &g, "l3KB", c->l3_kb);
    js(&b, &g, "flags", c->flags); js(&b, &g, "microcode", c->microcode);
    hb_printf(&b, "}");

    jkey(&b, &f, "memory"); hb_printf(&b, "{"); g = 1;
    ji(&b, &g, "totalKB", (long long)r->mem.total_kb); ji(&b, &g, "swapKB", (long long)r->mem.swap_kb);
    jkey(&b, &g, "zram"); hb_printf(&b, "[");
    for (int i = 0; i < r->mem.zram_n; i++) {
        int h = 1;
        hb_printf(&b, "%s{", i ? "," : "");
        ji(&b, &h, "sizeKB", (long long)r->mem.zram_kb[i]); js(&b, &h, "algorithm", r->mem.zram_algo[i]);
        hb_printf(&b, "}");
    }
    hb_printf(&b, "]}");

    jkey(&b, &f, "gpus"); hb_printf(&b, "[");
    for (int i = 0; i < r->ngpu; i++) {
        const SrGpu *x = &r->gpu[i];
        int h = 1;
        hb_printf(&b, "%s{", i ? "," : "");
        js(&b, &h, "card", x->card); js(&b, &h, "bus", x->bus); js(&b, &h, "vendorId", x->vendor_id); js(&b, &h, "deviceId", x->device_id);
        js(&b, &h, "vendor", x->vendor); js(&b, &h, "device", x->device); js(&b, &h, "driver", x->driver);
        jb(&b, &h, "bootVga", x->boot_vga); jb(&b, &h, "renderNode", x->render_node);
        ji(&b, &h, "connectors", x->connectors); ji(&b, &h, "connected", x->connected);
        hb_printf(&b, "}");
    }
    hb_printf(&b, "]");

    jkey(&b, &f, "displays"); hb_printf(&b, "[");
    for (int i = 0; i < r->ndisplay; i++) {
        const SrDisplay *x = &r->display[i];
        int h = 1;
        hb_printf(&b, "%s{", i ? "," : "");
        js(&b, &h, "card", x->card); js(&b, &h, "connector", x->connector); jb(&b, &h, "enabled", x->enabled);
        js(&b, &h, "name", x->name); js(&b, &h, "manufacturer", x->pnp); js(&b, &h, "product", x->product);
        ji(&b, &h, "year", x->year); ji(&b, &h, "widthCm", x->width_cm); ji(&b, &h, "heightCm", x->height_cm);
        ji(&b, &h, "diagonalTenths", x->diag_tenths); ji(&b, &h, "nativeWidth", x->pref_w); ji(&b, &h, "nativeHeight", x->pref_h);
        js(&b, &h, "mode", x->mode);
        hb_printf(&b, "}");
    }
    hb_printf(&b, "]");

    jkey(&b, &f, "storage"); hb_printf(&b, "[");
    for (int i = 0; i < r->ndisk; i++) {
        const SrDisk *x = &r->disk[i];
        int h = 1;
        hb_printf(&b, "%s{", i ? "," : "");
        js(&b, &h, "name", x->name); js(&b, &h, "model", x->model); js(&b, &h, "vendor", x->vendor);
        ji(&b, &h, "bytes", (long long)x->bytes); jb(&b, &h, "rotational", x->rotational); jb(&b, &h, "removable", x->removable);
        js(&b, &h, "transport", x->transport); js(&b, &h, "link", x->link);
        hb_printf(&b, "}");
    }
    hb_printf(&b, "]");

    jkey(&b, &f, "pci"); hb_printf(&b, "[");
    for (int i = 0; i < r->npci; i++) {
        const SrPci *x = &r->pci[i];
        int h = 1;
        hb_printf(&b, "%s{", i ? "," : "");
        js(&b, &h, "class", x->slot_class); js(&b, &h, "className", x->class_name);
        js(&b, &h, "vendorId", x->vendor_id); js(&b, &h, "deviceId", x->device_id);
        js(&b, &h, "vendor", x->vendor); js(&b, &h, "device", x->device); js(&b, &h, "driver", x->driver);
        hb_printf(&b, "}");
    }
    hb_printf(&b, "]");

    jkey(&b, &f, "usb"); hb_printf(&b, "[");
    for (int i = 0; i < r->nusb; i++) {
        const SrUsb *x = &r->usb[i];
        int h = 1;
        hb_printf(&b, "%s{", i ? "," : "");
        js(&b, &h, "vendorId", x->vendor_id); js(&b, &h, "productId", x->product_id);
        js(&b, &h, "vendor", x->vendor); js(&b, &h, "product", x->product); js(&b, &h, "class", x->cls);
        ji(&b, &h, "speedMbps", x->speed_mbps); js(&b, &h, "driver", x->driver);
        hb_printf(&b, "}");
    }
    hb_printf(&b, "]");

    jkey(&b, &f, "network"); hb_printf(&b, "[");
    for (int i = 0; i < r->nnet; i++) {
        const SrNet *x = &r->net[i];
        int h = 1;
        hb_printf(&b, "%s{", i ? "," : "");
        js(&b, &h, "kind", x->kind); js(&b, &h, "driver", x->driver); jb(&b, &h, "up", x->up);
        jn(&b, &h, "speedMbps", x->speed_mbps, -1);
        hb_printf(&b, "}");
    }
    hb_printf(&b, "]");

    jkey(&b, &f, "audio"); hb_printf(&b, "[");
    for (int i = 0; i < r->naudio; i++) { if (i) hb_printf(&b, ","); hb_str(&b, r->audio[i]); }
    hb_printf(&b, "]");
    jkey(&b, &f, "bluetooth"); hb_printf(&b, "[");
    for (int i = 0; i < r->nbt; i++) { int h = 1; hb_printf(&b, "%s{", i ? "," : ""); js(&b, &h, "driver", r->bt_driver[i]); hb_printf(&b, "}"); }
    hb_printf(&b, "]");

    jkey(&b, &f, "power"); hb_printf(&b, "[");
    for (int i = 0; i < r->npower; i++) {
        const SrPower *x = &r->power[i];
        int h = 1;
        hb_printf(&b, "%s{", i ? "," : "");
        js(&b, &h, "kind", x->kind); js(&b, &h, "vendor", x->vendor); js(&b, &h, "model", x->model);
        js(&b, &h, "technology", x->technology); js(&b, &h, "status", x->status);
        jn(&b, &h, "capacity", x->capacity, -1); jn(&b, &h, "health", x->health, -1); jn(&b, &h, "online", x->online, -1);
        hb_printf(&b, "}");
    }
    hb_printf(&b, "]");

    jkey(&b, &f, "thermal"); hb_printf(&b, "[");
    for (int i = 0; i < r->nthermal; i++) {
        const SrThermal *x = &r->thermal[i];
        int h = 1;
        hb_printf(&b, "%s{", i ? "," : "");
        js(&b, &h, "sensor", x->type); jn(&b, &h, "milliC", x->milli_c, INT32_MIN); ji(&b, &h, "fans", x->fans);
        hb_printf(&b, "}");
    }
    hb_printf(&b, "]");

    if (r->pi.is_pi) {
        jkey(&b, &f, "raspberryPi"); hb_printf(&b, "{"); g = 1;
        js(&b, &g, "model", r->pi.model); js(&b, &g, "revision", r->pi.revision);
        js(&b, &g, "bootloaderVersion", r->pi.bootloader_version); js(&b, &g, "bootloaderDate", r->pi.bootloader_date);
        jn(&b, &g, "throttled", r->pi.throttled, -1);
        if (r->pi.uv_alarm >= 0) jb(&b, &g, "underVoltageAlarm", r->pi.uv_alarm);
        else { jkey(&b, &g, "underVoltageAlarm"); hb_printf(&b, "null"); }
        hb_printf(&b, "}");
    }

    const SrSoftware *s = &r->sw;
    jkey(&b, &f, "software"); hb_printf(&b, "{"); g = 1;
    js(&b, &g, "os", s->os); js(&b, &g, "osId", s->os_id); js(&b, &g, "osVersion", s->os_version);
    js(&b, &g, "kernel", s->kernel); js(&b, &g, "lordos", s->lordos); js(&b, &g, "lsm", s->lsm);
    jn(&b, &g, "landlockAbi", s->landlock_abi, -1); jb(&b, &g, "bpfLsm", s->bpf_lsm); jb(&b, &g, "cgroup2", s->cgroup2);
    jb(&b, &g, "live", s->live);
    js(&b, &g, "bootMedium", s->boot_medium); jn(&b, &g, "bootLinkMbps", s->boot_link_mbps, -1);
    jb(&b, &g, "bootToRam", s->boot_toram);
    jkey(&b, &g, "kernelConfig"); hb_printf(&b, "{");
    for (int i = 0; i < s->nkconf; i++) {
        if (i) hb_printf(&b, ",");
        hb_str(&b, s->kconf[i].name);
        hb_printf(&b, ":");
        if (s->kconf[i].value[0]) hb_str(&b, s->kconf[i].value); else hb_printf(&b, "null");
    }
    hb_printf(&b, "}");
    jkey(&b, &g, "services"); hb_printf(&b, "{"); int h = 1;
    jb(&b, &h, "lord-sessiond", s->sessiond); jb(&b, &h, "lord-netd", s->netd);
    jb(&b, &h, "lord-netperm", s->netperm); jb(&b, &h, "lord-fand", s->fand);
    hb_printf(&b, "}}");

    hb_printf(&b, "}\n");
    return hb_take(&b);
}

/* ---- Markdown ---- */

/* A table cell: '|' and '`' would break the table; "—" for empty. */
static void cell(HBuf *b, const char *s) {
    if (!s || !s[0]) { hb_printf(b, " — |"); return; }
    hb_printf(b, " ");
    for (; *s; s++) hb_printf(b, "%c", *s == '|' || *s == '`' ? '/' : *s);
    hb_printf(b, " |");
}

static void size_text(unsigned long long bytes, char *out, size_t n) {
    if (bytes >= 1000ULL * 1000 * 1000 * 1000) snprintf(out, n, "%.1f TB", bytes / 1e12);
    else if (bytes >= 1000ULL * 1000 * 1000) snprintf(out, n, "%.0f GB", bytes / 1e9);
    else snprintf(out, n, "%.0f MB", bytes / 1e6);
}

static void two(char *out, size_t n, const char *a, const char *b) {
    snprintf(out, n, "%s%s%s", a, a[0] && b[0] ? " " : "", b);
}

char *sysreport_markdown(const SysReport *r) {
    HBuf b = { 0 };
    char t[256], u[64];
    const SrMachine *m = &r->machine;
    hb_printf(&b, "# System report\n\n");
    hb_printf(&b, "Made on %s by LordOS's System Information (schema %d). It lists the hardware and what the system made of it; it holds no serial numbers, addresses, host or user names.\n\n", r->date, r->schema);

    hb_printf(&b, "## Overview\n\n| | |\n|---|---|\n");
    two(t, sizeof t, m->vendor, m->product);
    if (!t[0]) snprintf(t, sizeof t, "%s", m->model);
    hb_printf(&b, "| Computer |"); cell(&b, t); hb_printf(&b, "\n");
    hb_printf(&b, "| Kind |"); cell(&b, m->virt[0] ? m->virt : m->chassis); hb_printf(&b, "\n");
    two(t, sizeof t, m->board_vendor, m->board_name);
    if (m->board_version[0] && strlen(t) + strlen(m->board_version) + 2 < sizeof t) { strcat(t, ", "); strcat(t, m->board_version); }
    hb_printf(&b, "| Board |"); cell(&b, t); hb_printf(&b, "\n");
    snprintf(t, sizeof t, "%s%s%s%s%s", m->firmware, m->bios_version[0] ? ", " : "", m->bios_version, m->bios_date[0] ? ", " : "", m->bios_date);
    hb_printf(&b, "| Firmware |"); cell(&b, t); hb_printf(&b, "\n");
    hb_printf(&b, "| Secure Boot |"); cell(&b, m->secure_boot < 0 ? "" : m->secure_boot ? "on" : "off"); hb_printf(&b, "\n");
    hb_printf(&b, "| System |"); cell(&b, r->sw.os); hb_printf(&b, "\n");
    hb_printf(&b, "| Kernel |"); cell(&b, r->sw.kernel); hb_printf(&b, "\n");
    hb_printf(&b, "| LordOS |"); cell(&b, r->sw.lordos); hb_printf(&b, "\n");
    if (r->sw.live || r->sw.boot_medium[0]) {
        static const char *const media[][2] = {
            { "internal", "an internal disk" }, { "usb", "a USB drive" }, { "sd", "an SD card" },
            { "emmc", "built-in flash memory (eMMC)" }, { "optical", "a DVD or CD" }, { "network", "the network" },
        };
        const char *from = "";
        for (size_t i = 0; i < sizeof media / sizeof media[0]; i++) if (!strcmp(r->sw.boot_medium, media[i][0])) from = media[i][1];
        char bm[96];
        snprintf(bm, sizeof bm, "%s%s%s", r->sw.live ? "live session" : "installed", from[0] ? ", from " : "", from);
        if (from[0] && r->sw.boot_link_mbps > 0) snprintf(bm + strlen(bm), sizeof bm - strlen(bm), " (%d Mbps)", r->sw.boot_link_mbps);
        if (r->sw.boot_toram) snprintf(bm + strlen(bm), sizeof bm - strlen(bm), ", copied to memory");
        hb_printf(&b, "| Started |"); cell(&b, bm); hb_printf(&b, "\n");
    }
    hb_printf(&b, "\n");

    const SrCpu *c = &r->cpu;
    hb_printf(&b, "## Processor\n\n| | |\n|---|---|\n");
    hb_printf(&b, "| Model |"); cell(&b, c->model); hb_printf(&b, "\n");
    snprintf(t, sizeof t, "%s, %d socket%s, %d cores, %d threads", c->arch, c->sockets, c->sockets == 1 ? "" : "s", c->cores, c->threads);
    hb_printf(&b, "| Layout |"); cell(&b, t); hb_printf(&b, "\n");
    if (c->max_mhz) {
        snprintf(t, sizeof t, "%d–%d MHz%s%s", c->min_mhz, c->max_mhz, c->governor[0] ? ", governor " : "", c->governor);
        hb_printf(&b, "| Frequency |"); cell(&b, t); hb_printf(&b, "\n");
    }
    snprintf(t, sizeof t, "L1d %d KB, L1i %d KB, L2 %d KB, L3 %d KB", c->l1d_kb, c->l1i_kb, c->l2_kb, c->l3_kb);
    hb_printf(&b, "| Cache |"); cell(&b, t); hb_printf(&b, "\n");
    hb_printf(&b, "| Instructions |"); cell(&b, c->flags); hb_printf(&b, "\n\n");

    hb_printf(&b, "## Memory\n\n");
    size_text(r->mem.total_kb * 1024ULL, t, sizeof t);
    size_text(r->mem.swap_kb * 1024ULL, u, sizeof u);
    hb_printf(&b, "%s, swap %s", t, r->mem.swap_kb ? u : "none");
    for (int i = 0; i < r->mem.zram_n; i++) {
        size_text(r->mem.zram_kb[i] * 1024ULL, u, sizeof u);
        hb_printf(&b, ", zram %s (%s)", u, r->mem.zram_algo[i][0] ? r->mem.zram_algo[i] : "?");
    }
    hb_printf(&b, ".\n\n");

    hb_printf(&b, "## Graphics\n\n| Card | Device | Driver | Render node | Connected outputs |\n|---|---|---|---|---|\n");
    for (int i = 0; i < r->ngpu; i++) {
        const SrGpu *g = &r->gpu[i];
        hb_printf(&b, "|"); cell(&b, g->card);
        two(t, sizeof t, g->vendor, g->device);
        if (!t[0] && g->vendor_id[0]) snprintf(t, sizeof t, "%s:%s", g->vendor_id, g->device_id);
        cell(&b, t); cell(&b, g->driver); cell(&b, g->render_node ? "yes" : "no");
        snprintf(t, sizeof t, "%d of %d", g->connected, g->connectors); cell(&b, t);
        hb_printf(&b, "\n");
    }
    if (!r->ngpu) hb_printf(&b, "| — | none found | | | |\n");
    hb_printf(&b, "\n");

    if (r->ndisplay) {
        hb_printf(&b, "## Displays\n\n| Output | Monitor | Size | Native mode |\n|---|---|---|---|\n");
        for (int i = 0; i < r->ndisplay; i++) {
            const SrDisplay *x = &r->display[i];
            hb_printf(&b, "|"); cell(&b, x->connector);
            if (x->year) snprintf(t, sizeof t, "%s, %d", x->name[0] ? x->name : x->pnp, x->year);
            else snprintf(t, sizeof t, "%s", x->name[0] ? x->name : x->pnp);
            cell(&b, t);
            if (x->diag_tenths) snprintf(t, sizeof t, "%d.%d\" (%d × %d cm)", x->diag_tenths / 10, x->diag_tenths % 10, x->width_cm, x->height_cm);
            else t[0] = 0;
            cell(&b, t);
            if (x->pref_w) snprintf(t, sizeof t, "%d × %d", x->pref_w, x->pref_h); else snprintf(t, sizeof t, "%s", x->mode);
            cell(&b, t);
            hb_printf(&b, "\n");
        }
        hb_printf(&b, "\n");
    }

    hb_printf(&b, "## Storage\n\n| Disk | Model | Size | Kind |\n|---|---|---|---|\n");
    for (int i = 0; i < r->ndisk; i++) {
        const SrDisk *x = &r->disk[i];
        hb_printf(&b, "|"); cell(&b, x->name); cell(&b, x->model);
        size_text(x->bytes, t, sizeof t); cell(&b, t);
        snprintf(t, sizeof t, "%s%s%s%s%s", x->transport, x->rotational ? ", spinning" : "", x->removable ? ", removable" : "", x->link[0] ? ", " : "", x->link);
        cell(&b, t);
        hb_printf(&b, "\n");
    }
    hb_printf(&b, "\n");

    hb_printf(&b, "## Network, audio and Bluetooth\n\n");
    for (int i = 0; i < r->nnet; i++) {
        const SrNet *x = &r->net[i];
        hb_printf(&b, "- %s%s%s%s", x->kind, x->driver[0] ? " (" : "", x->driver, x->driver[0] ? ")" : "");
        if (x->up && x->speed_mbps > 0) hb_printf(&b, ", up at %d Mbit/s", x->speed_mbps);
        else hb_printf(&b, ", %s", x->up ? "up" : "down");
        hb_printf(&b, "\n");
    }
    for (int i = 0; i < r->naudio; i++) hb_printf(&b, "- audio: %s\n", r->audio[i]);
    for (int i = 0; i < r->nbt; i++) hb_printf(&b, "- Bluetooth%s%s\n", r->bt_driver[i][0] ? ": " : "", r->bt_driver[i]);
    hb_printf(&b, "\n");

    if (r->npower) {
        hb_printf(&b, "## Power\n\n");
        for (int i = 0; i < r->npower; i++) {
            const SrPower *x = &r->power[i];
            if (!strcmp(x->kind, "battery")) {
                hb_printf(&b, "- battery");
                two(t, sizeof t, x->vendor, x->model);
                if (t[0]) hb_printf(&b, " %s", t);
                if (x->capacity >= 0) hb_printf(&b, ", %d %%", x->capacity);
                if (x->health >= 0) hb_printf(&b, ", holds %d %% of its design charge", x->health);
                if (x->status[0]) hb_printf(&b, ", %s", x->status);
                hb_printf(&b, "\n");
            } else {
                hb_printf(&b, "- %s%s\n", x->kind, x->online == 1 ? ", plugged in" : x->online == 0 ? ", unplugged" : "");
            }
        }
        hb_printf(&b, "\n");
    }

    if (r->pi.is_pi) {
        hb_printf(&b, "## Raspberry Pi\n\n%s, revision %s", r->pi.model, r->pi.revision[0] ? r->pi.revision : "unknown");
        if (r->pi.bootloader_date[0]) hb_printf(&b, ", bootloader of %s", r->pi.bootloader_date);
        /* rpi_volt clears the sticky bits every 2 s, so with it they cover that window, not the time since boot */
        const char *since = r->pi.uv_alarm >= 0 ? "in the last 2 s" : "since boot";
        if (r->pi.throttled >= 0) {
            unsigned v = (unsigned)r->pi.throttled;
            hb_printf(&b, "; %s", v & 0x1u ? "under-voltage now" : v & 0x10000u ? "under-voltage" : "no under-voltage");
            if (!(v & 0x1u)) hb_printf(&b, " %s", since);
            hb_printf(&b, "; %s", v & 0xeu ? "slowed down now" : v & 0xe0000u ? "slowed down" : "not slowed down");
            if (!(v & 0xeu)) hb_printf(&b, " %s", since);
            hb_printf(&b, " (get_throttled 0x%x)", v);
        } else if (r->pi.uv_alarm >= 0) {
            hb_printf(&b, "; %s in the last 2 s (rpi_volt); throttling not reported by this kernel",
                      r->pi.uv_alarm ? "under-voltage" : "no under-voltage");
        }
        hb_printf(&b, ".\n\n");
    }

    const SrSoftware *s = &r->sw;
    hb_printf(&b, "## Kernel features LordOS uses\n\n| | |\n|---|---|\n");
    hb_printf(&b, "| Security modules |"); cell(&b, s->lsm); hb_printf(&b, "\n");
    if (s->landlock_abi >= 0) { snprintf(t, sizeof t, "%d", s->landlock_abi); hb_printf(&b, "| Landlock ABI |"); cell(&b, s->landlock_abi ? t : "unavailable"); hb_printf(&b, "\n"); }
    hb_printf(&b, "| cgroup v2 |"); cell(&b, s->cgroup2 ? "yes" : "no"); hb_printf(&b, "\n");
    for (int i = 0; i < s->nkconf; i++) {
        hb_printf(&b, "|"); cell(&b, s->kconf[i].name);
        cell(&b, !strcmp(s->kconf[i].value, "y") ? "built in" : !strcmp(s->kconf[i].value, "m") ? "module"
                 : !strcmp(s->kconf[i].value, "n") ? "not set" : s->kconf[i].value[0] ? s->kconf[i].value : "unknown");
        hb_printf(&b, "\n");
    }
    hb_printf(&b, "\n## Devices\n\n| Kind | Device | Driver |\n|---|---|---|\n");
    for (int i = 0; i < r->npci; i++) {
        const SrPci *x = &r->pci[i];
        hb_printf(&b, "|"); cell(&b, x->class_name[0] ? x->class_name : x->slot_class);
        two(t, sizeof t, x->vendor, x->device);
        if (!t[0]) snprintf(t, sizeof t, "%s:%s", x->vendor_id, x->device_id);
        cell(&b, t); cell(&b, x->driver); hb_printf(&b, "\n");
    }
    for (int i = 0; i < r->nusb; i++) {
        const SrUsb *x = &r->usb[i];
        hb_printf(&b, "| USB |");
        two(t, sizeof t, x->vendor, x->product);
        if (!t[0]) snprintf(t, sizeof t, "%s:%s", x->vendor_id, x->product_id);
        cell(&b, t); cell(&b, x->driver); hb_printf(&b, "\n");
    }
    return hb_take(&b);
}
