/* tpsim.c — simulated touchpad via uinput, for testing the LordOS driver (server/src/tp) in a
 * VM without a touchpad. Creates a clickpad similar to a laptop Synaptics (gestures.h) and replays
 * scripted gestures with their real timing. See docs/TRACKPAD.md.
 *
 *   tpsim list                          the gestures
 *   tpsim dump GESTURE...               the evdev sequence as text ("ms type code value")
 *   sudo tpsim play [--touchscreen] [--wait S] GESTURE...
 *                                       creates the device, waits S seconds (2) and replays them
 *   sudo tpsim serve [--touchscreen] FIFO
 *                                       creates the device and waits for gesture names
 *                                       (one or more per line) on FIFO; "pause MS" waits, "quit" exits
 *   sudo tpsim play --mouse [--wait S] wheel-up|wheel-down|hwheel-left|hwheel-right|mouse-move...
 *                                       a mouse with a high-resolution wheel (and horizontal)
 *   sudo tpsim record /dev/input/eventN SECONDS
 *                                       records a real device (for tp-test --replay)
 *   sudo tpsim play-file [--bcm5974] [--wait S] FILE.ev
 *                                       replays a recording or a dump ("ms type code value"); with
 *                                       --bcm5974 the device looks like a laptop's bcm5974 (16 slots,
 *                                       no resolution, touch size, no pressure, USB 05ac:0273)
 *
 * Build: cc -O2 -Wall -Wextra -o tpsim tpsim.c gestures.c -lm */
#include "gestures.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static double mono(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void sleep_ms(double ms) {
    if (ms <= 0) return;
    struct timespec t = { (time_t)(ms / 1000), (long)((ms - (time_t)(ms / 1000) * 1000) * 1e6) };
    while (nanosleep(&t, &t) && errno == EINTR) {}
}

static void dump_emit(void *ud, double t, int type, int code, int value) {
    (void)ud;
    printf("%.1f %d %d %d\n", t, type, code, value);
}

typedef struct { int fd; double t0; } Live;

static void live_emit(void *ud, double t, int type, int code, int value) {
    Live *l = ud;
    sleep_ms(l->t0 + t - mono());
    struct input_event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = (unsigned short)type;
    ev.code = (unsigned short)code;
    ev.value = value;
    if (write(l->fd, &ev, sizeof ev) != sizeof ev) perror("tpsim: write");
}

static void abs_setup(int fd, int code, int min, int max, int res) {
    struct uinput_abs_setup a;
    memset(&a, 0, sizeof a);
    a.code = (unsigned short)code;
    a.absinfo.minimum = min;
    a.absinfo.maximum = max;
    a.absinfo.resolution = res;
    ioctl(fd, UI_SET_ABSBIT, code);
    if (ioctl(fd, UI_ABS_SETUP, &a)) perror("tpsim: UI_ABS_SETUP");
}

static int create(int touchscreen) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { perror("tpsim: /dev/uinput (sudo?)"); return -1; }
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    int keys[] = { BTN_LEFT, BTN_TOUCH, BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP, BTN_TOOL_QUADTAP, BTN_TOOL_QUINTTAP };
    for (size_t i = touchscreen ? 1 : 0; i < (touchscreen ? 2 : sizeof keys / sizeof keys[0]); i++) ioctl(fd, UI_SET_KEYBIT, keys[i]);
    abs_setup(fd, ABS_X, SIM_X_MIN, SIM_X_MAX, SIM_RES);
    abs_setup(fd, ABS_Y, SIM_Y_MIN, SIM_Y_MAX, SIM_RES);
    abs_setup(fd, ABS_PRESSURE, 0, SIM_PMAX, 0);
    abs_setup(fd, ABS_MT_SLOT, 0, SIM_SLOTS - 1, 0);
    abs_setup(fd, ABS_MT_TRACKING_ID, 0, 65535, 0);
    abs_setup(fd, ABS_MT_POSITION_X, SIM_X_MIN, SIM_X_MAX, SIM_RES);
    abs_setup(fd, ABS_MT_POSITION_Y, SIM_Y_MIN, SIM_Y_MAX, SIM_RES);
    abs_setup(fd, ABS_MT_PRESSURE, 0, SIM_PMAX, 0);
    if (touchscreen) ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
    else { ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_POINTER); ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_BUTTONPAD); }
    struct uinput_setup u;
    memset(&u, 0, sizeof u);
    u.id.bustype = touchscreen ? BUS_USB : BUS_I8042;
    u.id.vendor = touchscreen ? 0x0eef : 0x0002;
    u.id.product = touchscreen ? 0x0001 : 0x0007;
    snprintf(u.name, sizeof u.name, "%s", touchscreen ? "LordOS Pantalla táctil (simulada)" : SIM_NAME);
    if (ioctl(fd, UI_DEV_SETUP, &u) || ioctl(fd, UI_DEV_CREATE)) { perror("tpsim: create device"); close(fd); return -1; }
    char sys[64] = "";
    if (ioctl(fd, UI_GET_SYSNAME(sizeof sys), sys) >= 0) fprintf(stderr, "tpsim: %s created (/sys/devices/virtual/input/%s)\n", u.name, sys);
    return fd;
}

/* Like bcm5974 (laptops up to 2015): what matters to the driver, not the USB protocol. */
static int create_bcm5974(void) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { perror("tpsim: /dev/uinput (sudo?)"); return -1; }
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    int keys[] = { BTN_LEFT, BTN_TOUCH, BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP, BTN_TOOL_QUADTAP, BTN_TOOL_QUINTTAP };
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) ioctl(fd, UI_SET_KEYBIT, keys[i]);
    abs_setup(fd, ABS_X, -4828, 5345, 0);
    abs_setup(fd, ABS_Y, -203, 6803, 0);
    abs_setup(fd, ABS_PRESSURE, 0, 256, 0);
    abs_setup(fd, ABS_TOOL_WIDTH, 0, 16, 0);
    abs_setup(fd, ABS_MT_SLOT, 0, 15, 0);
    abs_setup(fd, ABS_MT_TRACKING_ID, 0, 65535, 0);
    abs_setup(fd, ABS_MT_TOUCH_MAJOR, 0, 2048, 0);
    abs_setup(fd, ABS_MT_TOUCH_MINOR, 0, 2048, 0);
    abs_setup(fd, ABS_MT_WIDTH_MAJOR, 0, 2048, 0);
    abs_setup(fd, ABS_MT_WIDTH_MINOR, 0, 2048, 0);
    abs_setup(fd, ABS_MT_ORIENTATION, -16384, 16384, 0);
    abs_setup(fd, ABS_MT_POSITION_X, -4828, 5345, 0);
    abs_setup(fd, ABS_MT_POSITION_Y, -203, 6803, 0);
    ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_POINTER);
    ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_BUTTONPAD);
    struct uinput_setup u;
    memset(&u, 0, sizeof u);
    u.id.bustype = BUS_USB;
    u.id.vendor = 0x05ac;
    u.id.product = 0x0273;
    snprintf(u.name, sizeof u.name, "bcm5974");
    if (ioctl(fd, UI_DEV_SETUP, &u) || ioctl(fd, UI_DEV_CREATE)) { perror("tpsim: create device"); close(fd); return -1; }
    char sys[64] = "";
    if (ioctl(fd, UI_GET_SYSNAME(sizeof sys), sys) >= 0) fprintf(stderr, "tpsim: bcm5974 (simulated) created (/sys/devices/virtual/input/%s)\n", sys);
    return fd;
}

/* "ms type code value" lines (comments with #), with their timing */
static int play_file(int fd, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return 1; }
    Live l = { fd, mono() };
    char line[256];
    double first = -1;
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        double t;
        int type, code, value;
        if (line[0] == '#' || sscanf(line, "%lf %d %d %d", &t, &type, &code, &value) != 4) continue;
        if (first < 0) first = t;
        live_emit(&l, t - first, type, code, value);
        n++;
    }
    fclose(f);
    fprintf(stderr, "tpsim: %s: %d events in %.0f ms\n", path, n, mono() - l.t0);
    return 0;
}

/* ---- mouse with a high-resolution wheel (REL_WHEEL_HI_RES, REL_HWHEEL_HI_RES) ---- */

static int create_mouse(void) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { perror("tpsim: /dev/uinput (sudo?)"); return -1; }
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_REL);
    int keys[] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE };
    for (size_t i = 0; i < 3; i++) ioctl(fd, UI_SET_KEYBIT, keys[i]);
    int rels[] = { REL_X, REL_Y, REL_WHEEL, REL_HWHEEL, REL_WHEEL_HI_RES, REL_HWHEEL_HI_RES };
    for (size_t i = 0; i < 6; i++) ioctl(fd, UI_SET_RELBIT, rels[i]);
    struct uinput_setup u;
    memset(&u, 0, sizeof u);
    u.id.bustype = BUS_USB;
    u.id.vendor = 0x046d;
    u.id.product = 0x4082;
    snprintf(u.name, sizeof u.name, "LordOS mouse con rueda de alta resolución (simulado)");
    if (ioctl(fd, UI_DEV_SETUP, &u) || ioctl(fd, UI_DEV_CREATE)) { perror("tpsim: create mouse"); close(fd); return -1; }
    fprintf(stderr, "tpsim: %s created\n", u.name);
    return fd;
}

static void rel(int fd, int code, int value) {
    struct input_event ev[2];
    memset(ev, 0, sizeof ev);
    ev[0].type = EV_REL; ev[0].code = (unsigned short)code; ev[0].value = value;
    ev[1].type = EV_SYN; ev[1].code = SYN_REPORT;
    if (write(fd, ev, sizeof ev) != sizeof ev) perror("tpsim: write");
}

/* A "free-spinning" high-resolution wheel: each step (120) arrives as 4 of 30, and with the last one the
 * usual whole step (REL_WHEEL), as the kernel does. */
static void play_mouse(int fd, char **names, int n) {
    for (int i = 0; i < n; i++) {
        const char *g = names[i];
        int vert = !strncmp(g, "wheel", 5), sign = strstr(g, "up") || strstr(g, "right") ? 1 : -1;
        if (!strcmp(g, "mouse-move")) { for (int k = 0; k < 30; k++) { rel(fd, REL_X, 8); sleep_ms(8); } }
        else if (vert || !strncmp(g, "hwheel", 6)) {
            for (int step = 0; step < 3; step++)
                for (int k = 0; k < 4; k++) {
                    struct input_event ev[3];
                    memset(ev, 0, sizeof ev);
                    int m = 0;
                    ev[m].type = EV_REL; ev[m].code = vert ? REL_WHEEL_HI_RES : REL_HWHEEL_HI_RES; ev[m++].value = 30 * sign;
                    if (k == 3) { ev[m].type = EV_REL; ev[m].code = vert ? REL_WHEEL : REL_HWHEEL; ev[m++].value = sign; }
                    ev[m].type = EV_SYN; ev[m++].code = SYN_REPORT;
                    if (write(fd, ev, sizeof ev[0] * (size_t)m) < 0) perror("tpsim: write");
                    sleep_ms(15);
                }
        } else { fprintf(stderr, "tpsim: unknown mouse gesture: %s\n", g); continue; }
        fprintf(stderr, "tpsim: %s\n", g);
    }
}

static int play(int fd, char **names, int n) {
    for (int i = 0; i < n; i++) {
        const SimGesture *g = sim_find(names[i]);
        if (!g) { fprintf(stderr, "tpsim: unknown gesture: %s\n", names[i]); continue; }
        Live l = { fd, mono() };
        SimGen gen;
        sim_init(&gen, live_emit, &l);
        double t0 = mono();
        g->play(&gen);
        sleep_ms(l.t0 + gen.t - mono());
        fprintf(stderr, "tpsim: %s (%s) at %.0f ms\n", g->name, g->what, mono() - t0);
    }
    return 0;
}

static volatile sig_atomic_t stop;
static void on_sig(int s) { (void)s; stop = 1; }

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: tpsim list | dump GESTURE... | play [--touchscreen] [--wait S] GESTURE... | serve [--touchscreen] FIFO | record DEV SECS\n");
        return 2;
    }
    if (!strcmp(argv[1], "list")) {
        int n;
        const SimGesture *g = sim_gestures(&n);
        for (int i = 0; i < n; i++) printf("%-14s %s\n", g[i].name, g[i].what);
        return 0;
    }
    if (!strcmp(argv[1], "dump")) {
        printf("# tpsim %s: %.0f x %.0f mm, %d contacts, resolution %d\n", SIM_NAME, SIM_W_MM, SIM_H_MM, SIM_SLOTS, SIM_RES);
        double t = 0;
        for (int i = 2; i < argc; i++) {
            const SimGesture *g = sim_find(argv[i]);
            if (!g) { fprintf(stderr, "tpsim: unknown gesture: %s\n", argv[i]); return 1; }
            SimGen gen;
            sim_init(&gen, dump_emit, NULL);
            gen.t = t;
            g->play(&gen);
            t = gen.t + 200;
        }
        return 0;
    }
    if (!strcmp(argv[1], "record") && argc >= 4) {
        int fd = open(argv[2], O_RDONLY | O_CLOEXEC);
        if (fd < 0) { perror(argv[2]); return 1; }
        int clk = CLOCK_MONOTONIC;
        ioctl(fd, EVIOCSCLOCKID, &clk);
        char name[128] = "";
        ioctl(fd, EVIOCGNAME(sizeof name), name);
        struct input_absinfo x, y, m, p, sl;
        memset(&m, 0, sizeof m);
        memset(&p, 0, sizeof p);
        memset(&sl, 0, sizeof sl);
        ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &x);
        ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &y);
        struct input_id id;
        memset(&id, 0, sizeof id);
        ioctl(fd, EVIOCGID, &id);
        printf("# record %s\n# caps x %d %d %d y %d %d %d\n# caps id %d %d %d\n", name, x.minimum, x.maximum, x.resolution, y.minimum,
               y.maximum, y.resolution, id.bustype, id.vendor, id.product);
        if (ioctl(fd, EVIOCGABS(ABS_MT_SLOT), &sl) == 0) printf("# caps slots %d\n", sl.maximum + 1);
        if (ioctl(fd, EVIOCGABS(ABS_MT_TOUCH_MAJOR), &m) == 0 && m.maximum > m.minimum) printf("# caps major %d %d\n", m.minimum, m.maximum);
        if (ioctl(fd, EVIOCGABS(ABS_MT_PRESSURE), &p) == 0 && p.maximum > p.minimum) printf("# caps pressure %d %d\n", p.minimum, p.maximum);
        printf("# caps name %s\n", name);
        double end = mono() + atof(argv[3]) * 1000, t0 = -1;
        fcntl(fd, F_SETFL, O_NONBLOCK);
        while (mono() < end) {
            struct input_event ev;
            ssize_t r = read(fd, &ev, sizeof ev);
            if (r != sizeof ev) { sleep_ms(2); continue; }
            double t = ev.input_event_sec * 1e3 + ev.input_event_usec / 1e3;
            if (t0 < 0) t0 = t;
            printf("%.1f %d %d %d\n", t - t0, ev.type, ev.code, ev.value);
        }
        return 0;
    }
    int touchscreen = 0, mouse = 0, bcm = 0;
    double wait = 2;
    int i = 2;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "--touchscreen")) touchscreen = 1;
        else if (!strcmp(argv[i], "--mouse")) mouse = 1;
        else if (!strcmp(argv[i], "--bcm5974")) bcm = 1;
        else if (!strcmp(argv[i], "--wait") && i + 1 < argc) wait = atof(argv[++i]);
    }
    if (!strcmp(argv[1], "play")) {
        int fd = mouse ? create_mouse() : create(touchscreen);
        if (fd < 0) return 1;
        sleep_ms(wait * 1000);
        if (mouse) play_mouse(fd, argv + i, argc - i);
        else play(fd, argv + i, argc - i);
        sleep_ms(300);
        ioctl(fd, UI_DEV_DESTROY);
        close(fd);
        return 0;
    }
    if (!strcmp(argv[1], "play-file") && i < argc) {
        int fd = bcm ? create_bcm5974() : create(0);
        if (fd < 0) return 1;
        sleep_ms(wait * 1000);
        for (; i < argc; i++) { play_file(fd, argv[i]); sleep_ms(300); }
        ioctl(fd, UI_DEV_DESTROY);
        close(fd);
        return 0;
    }
    if (!strcmp(argv[1], "serve") && i < argc) {
        const char *fifo = argv[i];
        unlink(fifo);
        if (mkfifo(fifo, 0666)) { perror(fifo); return 1; }
        chmod(fifo, 0666);
        int fd = create(touchscreen);
        if (fd < 0) return 1;
        signal(SIGINT, on_sig);
        signal(SIGTERM, on_sig);
        while (!stop) {
            FILE *f = fopen(fifo, "r");             /* blocks until someone writes */
            if (!f) break;
            char line[512];
            while (!stop && fgets(line, sizeof line, f)) {
                char *names[32];
                int n = 0;
                for (char *tok = strtok(line, " \t\r\n"); tok && n < 32; tok = strtok(NULL, " \t\r\n")) names[n++] = tok;
                for (int k = 0; k < n; k++) {
                    if (!strcmp(names[k], "quit")) stop = 1;
                    else if (!strcmp(names[k], "pause") && k + 1 < n) sleep_ms(atof(names[++k]));
                    else play(fd, names + k, 1);
                }
            }
            fclose(f);
        }
        ioctl(fd, UI_DEV_DESTROY);
        close(fd);
        unlink(fifo);
        return 0;
    }
    fprintf(stderr, "tpsim: unknown command\n");
    return 2;
}
