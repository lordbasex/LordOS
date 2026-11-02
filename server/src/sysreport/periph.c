/* periph.c — the peripherals on the USB and input buses (see periph.h). */
#include "periph.h"
#include "ids.h"
#include "../hw/jbuf.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define PERIPH_MAX 48
#define NAME_MAX_LEN 96

typedef struct {
    char name[NAME_MAX_LEN], kind[12], conn[12];
    char hid[24];                       /* a receiver's device: its HID id, to find its battery */
    int battery;
} Periph;

typedef struct { Periph v[PERIPH_MAX]; int n; } List;

/* The first line of <root><dir>/<name>, printable ASCII only. "" when missing. */
static void attr(const char *root, const char *dir, const char *name, char *out, size_t size) {
    char p[1024], buf[1024];
    out[0] = 0;
    snprintf(p, sizeof p, "%s%s/%s", root, dir, name);
    int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return;
    size_t o = 0;
    for (ssize_t i = 0; i < n && buf[i] && buf[i] != '\n' && o + 1 < size; i++)
        out[o++] = (buf[i] >= 0x20 && buf[i] < 0x7f) ? buf[i] : '?';
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
}

static int cmp_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static char **list_dir(const char *root, const char *dir, int *count) {
    char p[1024];
    snprintf(p, sizeof p, "%s%s", root, dir);
    *count = 0;
    DIR *d = opendir(p);
    if (!d) return NULL;
    char **v = NULL;
    int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 1024) {
        if (e->d_name[0] == '.') continue;
        if (n == cap) {
            int nc = cap ? cap * 2 : 32;
            char **nv = realloc(v, (size_t)nc * sizeof *v);
            if (!nv) break;
            v = nv;
            cap = nc;
        }
        if (!(v[n] = strdup(e->d_name))) break;
        n++;
    }
    closedir(d);
    if (v) qsort(v, (size_t)n, sizeof *v, cmp_name);
    *count = n;
    return v;
}

static void free_list(char **v, int n) {
    for (int i = 0; i < n; i++) free(v[i]);
    free(v);
}

static Periph *add(List *l, const char *name, const char *kind, const char *conn) {
    for (int i = 0; i < l->n; i++)                  /* one device, several interfaces or input nodes */
        if (!strcmp(l->v[i].name, name) && !strcmp(l->v[i].conn, conn)) return NULL;
    if (l->n >= PERIPH_MAX) return NULL;
    Periph *p = &l->v[l->n++];
    memset(p, 0, sizeof *p);
    snprintf(p->name, sizeof p->name, "%s", name);
    snprintf(p->kind, sizeof p->kind, "%s", kind);
    snprintf(p->conn, sizeof p->conn, "%s", conn);
    p->battery = -1;
    return p;
}

/* ---- USB ---- */

static int named_receiver(const char *name) {
    static const char *const w[] = { "receiver", "unifying", "nano", "dongle" };
    for (size_t i = 0; i < sizeof w / sizeof w[0]; i++)
        for (const char *s = name; *s; s++) if (!strncasecmp(s, w[i], strlen(w[i]))) return 1;
    return 0;
}

/* What a USB device is, from its interfaces (class, boot protocol) and its name. */
static const char *usb_kind(const char *root, char **v, int n, const char *dev, const char *name) {
    size_t dl = strlen(dev);
    int hid = 0, kbd = 0, mouse = 0, audio = 0, video = 0, storage = 0, printer = 0, net = 0, bt = 0;
    for (int k = 0; k < n; k++) {
        if (strncmp(v[k], dev, dl) || v[k][dl] != ':') continue;
        char dir[320], cls[8], proto[8];
        snprintf(dir, sizeof dir, "/sys/bus/usb/devices/%s", v[k]);
        attr(root, dir, "bInterfaceClass", cls, sizeof cls);
        attr(root, dir, "bInterfaceProtocol", proto, sizeof proto);
        long c = strtol(cls, NULL, 16), p = strtol(proto, NULL, 16);
        if (c == 3) { hid = 1; kbd |= p == 1; mouse |= p == 2; }
        else if (c == 1) audio = 1;
        else if (c == 0x0e) video = 1;
        else if (c == 8) storage = 1;
        else if (c == 7) printer = 1;
        else if (c == 2 || c == 0x0a) net = 1;
        else if (c == 0xe0) bt = 1;
    }
    if (audio) return "audio";                      /* a sound card's buttons are HID too */
    if (video) return "camera";
    if (hid && named_receiver(name)) return "receiver";
    if (kbd) return "keyboard";
    if (mouse) return "mouse";
    if (hid) return "input";
    return storage ? "storage" : printer ? "printer" : net ? "network" : bt ? "bluetooth" : "other";
}

static void collect_usb(List *l, const char *root, const IdsDb *ids) {
    int n;
    char **v = list_dir(root, "/sys/bus/usb/devices", &n);
    for (int i = 0; i < n; i++) {
        if (strchr(v[i], ':')) continue;            /* an interface, read with its device */
        char dir[320], vid[8], pid[8], cls[8], maker[64], product[64], name[NAME_MAX_LEN], vn[64] = "", dn[64] = "";
        snprintf(dir, sizeof dir, "/sys/bus/usb/devices/%s", v[i]);
        attr(root, dir, "idVendor", vid, sizeof vid);
        attr(root, dir, "idProduct", pid, sizeof pid);
        attr(root, dir, "bDeviceClass", cls, sizeof cls);
        if (strlen(vid) != 4 || strlen(pid) != 4 || !strcmp(vid, "1d6b") || strtol(cls, NULL, 16) == 9) continue;   /* root hubs, hubs */
        attr(root, dir, "manufacturer", maker, sizeof maker);
        attr(root, dir, "product", product, sizeof product);
        ids_lookup(ids, vid, pid, vn, sizeof vn, dn, sizeof dn);
        if (!product[0]) snprintf(product, sizeof product, "%s", dn);
        if (!maker[0]) snprintf(maker, sizeof maker, "%s", vn);
        if (!product[0]) snprintf(name, sizeof name, "%s:%s", vid, pid);
        else if (!maker[0] || !strncasecmp(product, maker, strlen(maker))) snprintf(name, sizeof name, "%s", product);
        else snprintf(name, sizeof name, "%.40s %.54s", maker, product);
        add(l, name, usb_kind(root, v, n, v[i], name), "usb");
    }
    free_list(v, n);
}

/* ---- input devices ---- */

/* Bit `bit` of a capabilities attribute: hexadecimal words, the highest first. */
static int cap_bit(const char *root, const char *dir, const char *name, int bit) {
    char v[1024], p[320];
    snprintf(p, sizeof p, "%s/capabilities", dir);
    attr(root, p, name, v, sizeof v);
    const int w = (int)sizeof(long) * 8;
    int word = 0;
    for (char *e = v + strlen(v); e > v; word++) {
        char *s = e;
        while (s > v && s[-1] != ' ') s--;
        if (word == bit / w) return (int)((strtoul(s, NULL, 16) >> (bit % w)) & 1);
        e = s;
        while (e > v && e[-1] == ' ') e--;
    }
    return 0;
}

enum { REL_X_BIT = 0, KEY_Q_BIT = 16, KEY_A_BIT = 30, BTN_LEFT_BIT = 272, BTN_SOUTH_BIT = 304, BTN_TOOL_FINGER_BIT = 325, BTN_TOUCH_BIT = 330 };

static const char *input_kind(const char *root, const char *dir) {
    if (cap_bit(root, dir, "key", BTN_TOUCH_BIT) && cap_bit(root, dir, "key", BTN_TOOL_FINGER_BIT)) return "touchpad";
    if (cap_bit(root, dir, "rel", REL_X_BIT) && cap_bit(root, dir, "key", BTN_LEFT_BIT)) return "mouse";
    if (cap_bit(root, dir, "key", KEY_A_BIT) && cap_bit(root, dir, "key", KEY_Q_BIT)) return "keyboard";
    if (cap_bit(root, dir, "key", BTN_SOUTH_BIT)) return "gamepad";
    return NULL;                                    /* power button, media keys, a monitor's jack */
}

static int is_hid_id(const char *s, size_t n) {     /* "0003:046D:4054.000D" */
    if (n != 19 || s[4] != ':' || s[9] != ':' || s[14] != '.') return 0;
    for (size_t i = 0; i < n; i++) if (i != 4 && i != 9 && i != 14 && !isxdigit((unsigned char)s[i])) return 0;
    return 1;
}

/* A device behind a receiver is a HID device inside the receiver's: two ids in a row in its sysfs path. The inner
 * one goes to hid. */
static int behind_receiver(const char *path, char *hid, size_t size) {
    int count = 0;
    for (const char *s = path; *s;) {
        size_t n = strcspn(s, "/");
        if (is_hid_id(s, n)) {
            if (++count == 2) { snprintf(hid, size, "%.*s", (int)n, s); return 1; }
        } else count = 0;
        s += n;
        if (*s == '/') s++;
    }
    return 0;
}

static void collect_input(List *l, const char *root) {
    int n;
    char **v = list_dir(root, "/sys/class/input", &n);
    for (int i = 0; i < n; i++) {
        if (strncmp(v[i], "input", 5) || !isdigit((unsigned char)v[i][5])) continue;
        char dir[320], link[1024], p[1100], bus[8], name[NAME_MAX_LEN], hid[24] = "";
        snprintf(dir, sizeof dir, "/sys/class/input/%s", v[i]);
        snprintf(p, sizeof p, "%s%s", root, dir);
        ssize_t k = readlink(p, link, sizeof link - 1);
        link[k > 0 ? k : 0] = 0;
        char idd[340];
        snprintf(idd, sizeof idd, "%s/id", dir);
        attr(root, idd, "bustype", bus, sizeof bus);
        long b = strtol(bus, NULL, 16);
        const char *conn = NULL;
        if (b == 0x03) { if (behind_receiver(link, hid, sizeof hid)) conn = "receiver"; }   /* else: read as a USB device */
        else if (b == 0x11 || b == 0x18) conn = "internal";     /* i8042, I2C: a laptop's keyboard and touchpad */
        const char *kind = conn ? input_kind(root, dir) : NULL;  /* Bluetooth (0x05) comes from BlueZ */
        if (!kind) continue;
        attr(root, dir, "name", name, sizeof name);
        Periph *x = name[0] ? add(l, name, kind, conn) : NULL;
        if (x) snprintf(x->hid, sizeof x->hid, "%s", hid);
    }
    free_list(v, n);
}

/* The batteries of the devices behind a receiver: a power supply of scope "Device" under the same HID id, whose
 * model name is also better than the generic one the input node has. */
static void collect_batteries(List *l, const char *root) {
    int n;
    char **v = list_dir(root, "/sys/class/power_supply", &n);
    for (int i = 0; i < n; i++) {
        char dir[320], scope[16], link[512], p[1024], cap[8], model[NAME_MAX_LEN];
        snprintf(dir, sizeof dir, "/sys/class/power_supply/%s", v[i]);
        attr(root, dir, "scope", scope, sizeof scope);
        if (strcmp(scope, "Device")) continue;
        snprintf(p, sizeof p, "%s%s/device", root, dir);
        ssize_t k = readlink(p, link, sizeof link - 1);
        if (k <= 0) continue;
        link[k] = 0;
        const char *base = strrchr(link, '/');
        base = base ? base + 1 : link;
        attr(root, dir, "capacity", cap, sizeof cap);
        attr(root, dir, "model_name", model, sizeof model);
        for (int j = 0; j < l->n; j++) {
            Periph *x = &l->v[j];
            if (!x->hid[0] || strcmp(x->hid, base)) continue;
            if (cap[0] && strspn(cap, "0123456789") == strlen(cap) && atoi(cap) <= 100) x->battery = atoi(cap);
            if (model[0]) snprintf(x->name, sizeof x->name, "%s", model);
        }
    }
    free_list(v, n);
}

char *periph_json(const char *root, const char *usb_ids) {
    List *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    if (!root) root = "";
    char ip[1024];
    snprintf(ip, sizeof ip, "%s/usr/share/misc/usb.ids", root);
    IdsDb ids;
    ids_open(&ids, usb_ids ? usb_ids : ip);
    collect_usb(l, root, &ids);
    collect_input(l, root);
    collect_batteries(l, root);
    ids_close(&ids);
    HBuf b = { 0 };
    hb_printf(&b, "[");
    for (int i = 0; i < l->n; i++) {
        const Periph *x = &l->v[i];
        hb_printf(&b, "%s{\"name\":", i ? "," : "");
        hb_str(&b, x->name);
        hb_printf(&b, ",\"kind\":\"%s\",\"connection\":\"%s\",\"battery\":%d}", x->kind, x->conn, x->battery);
    }
    hb_printf(&b, "]");
    free(l);
    return hb_take(&b);
}
