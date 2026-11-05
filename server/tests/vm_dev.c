/* vm_dev.c — lordvm-dev: plays lord-server's part for Machines (docs/VIRTUALIZATION.md, "Phase 1 implementation"),
 * so the helpers can be driven and tested without the UI. It is also the reference for lord-server's own code:
 * spawn lord-vm with a control socketpair, ask it for the display connection, spawn lord-vmdisplay with it, make
 * the sealed picture memfd, acknowledge frames, send input.
 *
 *   lordvm-dev run DIR [lord-vm --start options]   starts the machine (lord-vm --control-fd 3 --paused ...)
 *   lordvm-dev attach DIR                          a machine already running (its runtime socket)
 *
 * stdin, one command per line:
 *   png PATH            write the current picture (and print how much of it isn't black)
 *   type TEXT           type ASCII ("\n" = Enter)        key CODE          one evdev key, press and release
 *   move X Y            pointer, picture coordinates     click X Y [B]     move, press and release button B (0)
 *   wheel N             N steps (+ up)                   resize W H        ask the guest for a size
 *   rate HZ             copy rate                        release           let go of everything
 *   status | pause | resume | reset | shutdown [S] | poweroff | qmp JSON
 *   noack 0|1           stop acknowledging frames (tests the 250 ms fallback)
 *   wait-frames N [S]   wait until N more frames arrived (at most S seconds, default 30)
 *   quit
 * stdout: one JSON line per event or answer. Binaries are looked for next to this one, or in $LORDVM_BIN. */
#include "vm/vmproto.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_ONLY_PNG
#include "stb_image_write.h"

static char bindir[PATH_MAX];
static int vm = -1, vd = -1;
static pid_t vm_pid, vd_pid;
static char machine_id[40];
static int display_asked, display_up, noack;
static uint8_t *pic;
static int pw, ph;
static size_t plen;
static long frames, frames_target = -1;
static long long frames_deadline;
static uint32_t seq = 100;
static int vm_state = -1;
static int cursor_w, cursor_h;

static void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void out(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

static long long now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000; }

static void find_bindir(void) {
    const char *e = getenv("LORDVM_BIN");
    if (e) { snprintf(bindir, sizeof bindir, "%s", e); return; }
    ssize_t l = readlink("/proc/self/exe", bindir, sizeof bindir - 1);
    if (l <= 0) { strcpy(bindir, "."); return; }
    bindir[l] = 0;
    char *s = strrchr(bindir, '/');
    if (s) *s = 0;
}

static const char *const STATES[] = { "stopped", "starting", "running", "paused", "stopping", "crashed" };

static void ask(uint16_t type, int a, const char *text) {
    VmMsg m = vm_msg(type, ++seq, a, 0, 0, 0, text);
    if (vm_send(vm, &m, -1)) out("{\"ev\":\"error\",\"what\":\"can't reach lord-vm\"}");
}

static void spawn_vmdisplay(int dbus_fd) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) return;
    pid_t p = fork();
    if (!p) {
        int a = fcntl(sv[1], F_DUPFD, 20), b = fcntl(dbus_fd, F_DUPFD, 21);
        dup2(a, 3);
        dup2(b, 4);
        syscall(SYS_close_range, 5, ~0u, 0);
        char bin[PATH_MAX + 32];
        snprintf(bin, sizeof bin, "%s/lord-vmdisplay", bindir);
        execl(bin, "lord-vmdisplay", machine_id, (char *)NULL);
        _exit(127);
    }
    close(sv[1]);
    close(dbus_fd);
    vd = sv[0];
    vd_pid = p;
    out("{\"ev\":\"vmdisplay\",\"pid\":%d}", (int)p);
}

static void picture_for(int w, int h) {
    if (pic) munmap(pic, plen);
    pic = NULL;
    size_t len = (size_t)w * h * 4;
    int fd = vm_picture_memfd(len);
    if (fd < 0) return;
    pic = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
    if (pic == MAP_FAILED) { pic = NULL; close(fd); return; }
    pw = w; ph = h; plen = len;
    VdMsg m = vd_msg(VD_SHM, 0, 0, 0, 0, 0, 0);
    vd_send(vd, &m, fd);
    close(fd);
    out("{\"ev\":\"picture\",\"w\":%d,\"h\":%d}", w, h);
}

static void from_vm(void) {
    for (int k = 0; k < VD_POLL_MAX; k++) {
        VmMsg m;
        int fd = -1;
        int r = vm_recv(vm, &m, &fd);
        if (r < 0) { out("{\"ev\":\"lord-vm-gone\"}"); close(vm); vm = -1; return; }
        if (!r) return;
        if (!vm_msg_valid(&m, 0)) { if (fd >= 0) close(fd); out("{\"ev\":\"refused\",\"from\":\"lord-vm\",\"type\":%u}", m.type); continue; }
        if (m.type == VM_INFO) {
            memcpy(machine_id, m.text, 36);
            machine_id[36] = 0;
            /* VM_INFO can come after the first VM_STATE */
            if ((vm_state == VS_RUNNING || vm_state == VS_PAUSED) && !display_asked) { display_asked = 1; ask(VM_DISPLAY, 0, NULL); } out("{\"ev\":\"info\",\"vcpus\":%d,\"memory_mb\":%d,\"sandbox\":%d,\"cgroup\":%d,\"id\":\"%s\"}", m.a, m.b, m.c, m.d, m.text); }
        else if (m.type == VM_STATE) {
            vm_state = m.a;
            out("{\"ev\":\"state\",\"state\":\"%s\",\"reason\":%d,\"code\":%d,\"qemu_pid\":%d,\"detail\":\"%s\"}", STATES[m.a], m.b, m.c, m.d, m.text);
            if ((m.a == VS_RUNNING || m.a == VS_PAUSED) && !display_asked && machine_id[0]) { display_asked = 1; ask(VM_DISPLAY, 0, NULL); }
        } else if (m.type == VM_REPLY) out("{\"ev\":\"reply\",\"seq\":%u,\"code\":%d,\"text\":\"%s\"}", m.seq, m.a, m.text);
        else if (m.type == VM_DISPLAY_FD) {
            if (fd < 0) { out("{\"ev\":\"error\",\"what\":\"VM_DISPLAY_FD without a descriptor\"}"); continue; }
            out("{\"ev\":\"display_fd\"}");
            spawn_vmdisplay(fd);
            /* started paused so that the first picture is seen: now let it run */
            if (vm_state == VS_PAUSED && getenv("LORDVM_DEV_STAY_PAUSED") == NULL) ask(VM_RESUME, 0, NULL);
        }
        if (fd >= 0 && m.type != VM_DISPLAY_FD) close(fd);
    }
}

static void from_vd(void) {
    for (int k = 0; k < VD_POLL_MAX; k++) {
        VdMsg m;
        int fd = -1;
        int r = vd_recv(vd, &m, &fd);
        if (r < 0) { out("{\"ev\":\"vmdisplay-gone\"}"); close(vd); vd = -1; return; }
        if (!r) return;
        if (!vd_msg_valid(&m, 0)) { if (fd >= 0) close(fd); out("{\"ev\":\"refused\",\"from\":\"lord-vmdisplay\",\"type\":%u}", m.type); continue; }
        switch (m.type) {
        case VD_READY: display_up = 1; out("{\"ev\":\"display_ready\",\"w\":%d,\"h\":%d,\"abs\":%d}", m.a, m.b, m.c); break;
        case VD_SIZE: picture_for(m.a, m.b); break;
        case VD_FRAME:
            if (!vd_rect_inside(&m, pw, ph)) { out("{\"ev\":\"refused\",\"what\":\"frame outside the picture\"}"); break; }
            frames++;
            if (frames == 1) out("{\"ev\":\"first_frame\",\"w\":%d,\"h\":%d}", pw, ph);
            if (!noack) { VdMsg a = vd_msg(VD_FRAME_DONE, 0, 0, 0, 0, m.e, 0); vd_send(vd, &a, -1); }
            break;
        case VD_CURSOR:
            if (fd < 0 || vm_cursor_check(fd, m.a, m.b)) { out("{\"ev\":\"refused\",\"what\":\"cursor memfd\"}"); break; }
            cursor_w = m.a; cursor_h = m.b;
            out("{\"ev\":\"cursor\",\"w\":%d,\"h\":%d,\"hot_x\":%d,\"hot_y\":%d}", m.a, m.b, m.c, m.d);
            break;
        case VD_POINTER: break;
        case VD_STATE: out("{\"ev\":\"display_state\",\"state\":%d,\"reason\":%d}", m.a, m.b); break;
        case VD_STATS: out("{\"ev\":\"display_stats\",\"refused\":%d,\"updates\":%d,\"copies\":%d,\"w\":%d,\"h\":%d,\"frame\":%d,\"frames_here\":%ld}", m.a, m.b, m.c, m.d, m.e, m.f, frames); break;
        }
        if (fd >= 0) close(fd);
    }
}

static void vdsend(int type, int a, int b) { if (vd >= 0) { VdMsg m = vd_msg((uint16_t)type, a, b, 0, 0, 0, 0); vd_send(vd, &m, -1); } }

/* ASCII to evdev (US layout); bit 8: shift */
static int ascii_key(char c) {
    static const char *row1 = "1234567890-=", *row1s = "!@#$%^&*()_+";
    static const char *q = "qwertyuiop[]", *a = "asdfghjkl;'", *z = "zxcvbnm,./";
    static const char *qs = "QWERTYUIOP{}", *as = "ASDFGHJKL:\"", *zs = "ZXCVBNM<>?";
    const char *p;
    if (c == '\n') return 28;
    if (c == ' ') return 57;
    if (c == '\t') return 15;
    if (c == '`') return 41;
    if (c == '~') return 41 | 0x100;
    if (c == '\\') return 43;
    if (c == '|') return 43 | 0x100;
    if ((p = strchr(row1, c))) return 2 + (int)(p - row1);
    if ((p = strchr(row1s, c))) return (2 + (int)(p - row1s)) | 0x100;
    if ((p = strchr(q, c))) return 16 + (int)(p - q);
    if ((p = strchr(qs, c))) return (16 + (int)(p - qs)) | 0x100;
    if ((p = strchr(a, c))) return 30 + (int)(p - a);
    if ((p = strchr(as, c))) return (30 + (int)(p - as)) | 0x100;
    if ((p = strchr(z, c))) return 44 + (int)(p - z);
    if ((p = strchr(zs, c))) return (44 + (int)(p - zs)) | 0x100;
    return -1;
}

/* keys go out spaced, as a person types: a queue drained by the loop */
static struct { int code, down; } kq[8192];
static int kq_h, kq_t;
static long long kq_next;
static void kpush(int code, int down) { if ((kq_t + 1) % 8192 != kq_h) { kq[kq_t].code = code; kq[kq_t].down = down; kq_t = (kq_t + 1) % 8192; } }
static void kpump(void) {
    if (kq_h == kq_t || now_ms() < kq_next) return;
    vdsend(VD_KEY, kq[kq_h].code, kq[kq_h].down);
    kq_h = (kq_h + 1) % 8192;
    kq_next = now_ms() + 12;
}

static void save_png(const char *path) {
    if (!pic) { out("{\"ok\":false,\"cmd\":\"png\",\"why\":\"no picture\"}"); return; }
    uint8_t *rgb = malloc((size_t)pw * ph * 3);
    long lit = 0;
    for (int i = 0; i < pw * ph; i++) {
        rgb[i * 3] = pic[i * 4 + 2]; rgb[i * 3 + 1] = pic[i * 4 + 1]; rgb[i * 3 + 2] = pic[i * 4];
        if (pic[i * 4] > 16 || pic[i * 4 + 1] > 16 || pic[i * 4 + 2] > 16) lit++;
    }
    int ok = stbi_write_png(path, pw, ph, 3, rgb, pw * 3);
    free(rgb);
    out("{\"ok\":%s,\"cmd\":\"png\",\"w\":%d,\"h\":%d,\"lit_pct\":%.2f}", ok ? "true" : "false", pw, ph, 100.0 * lit / ((double)pw * ph));
}

static int handle(char *line) {
    char a1[1024];
    int x, y, b;
    if (!strncmp(line, "png ", 4)) save_png(line + 4);
    else if (!strncmp(line, "type ", 5)) {
        for (const char *p = line + 5; *p; p++) {
            char c = *p;
            if (c == '\\' && p[1] == 'n') { c = '\n'; p++; }
            int k = ascii_key(c);
            if (k < 0) continue;
            if (k & 0x100) kpush(42, 1);
            kpush(k & 0xff, 1); kpush(k & 0xff, 0);
            if (k & 0x100) kpush(42, 0);
        }
        out("{\"ok\":true,\"cmd\":\"type\"}");
    } else if (sscanf(line, "key %d", &x) == 1) { kpush(x, 1); kpush(x, 0); out("{\"ok\":true,\"cmd\":\"key\"}"); }
    else if (sscanf(line, "move %d %d", &x, &y) == 2) { vdsend(VD_MOTION, x, y); out("{\"ok\":true,\"cmd\":\"move\"}"); }
    else if (sscanf(line, "click %d %d %d", &x, &y, &b) >= 2) {
        if (sscanf(line, "click %d %d %d", &x, &y, &b) < 3) b = 0;
        vdsend(VD_MOTION, x, y); vdsend(VD_BUTTON, b, 1); vdsend(VD_BUTTON, b, 0);
        out("{\"ok\":true,\"cmd\":\"click\"}");
    } else if (sscanf(line, "wheel %d", &x) == 1) { VdMsg m = vd_msg(VD_WHEEL, x, 0, 0, 0, 0, 0); if (vd >= 0) vd_send(vd, &m, -1); out("{\"ok\":true,\"cmd\":\"wheel\"}"); }
    else if (sscanf(line, "resize %d %d", &x, &y) == 2) { vdsend(VD_RESIZE, x, y); out("{\"ok\":true,\"cmd\":\"resize\"}"); }
    else if (sscanf(line, "rate %d", &x) == 1) { vdsend(VD_RATE, x, 0); out("{\"ok\":true,\"cmd\":\"rate\"}"); }
    else if (sscanf(line, "noack %d", &x) == 1) { noack = x; out("{\"ok\":true,\"cmd\":\"noack\"}"); }
    else if (!strcmp(line, "release")) { vdsend(VD_RELEASE_ALL, 0, 0); out("{\"ok\":true,\"cmd\":\"release\"}"); }
    else if (!strcmp(line, "status")) ask(VM_STATUS, 0, NULL);
    else if (!strcmp(line, "pause")) ask(VM_PAUSE, 0, NULL);
    else if (!strcmp(line, "resume")) ask(VM_RESUME, 0, NULL);
    else if (!strcmp(line, "reset")) ask(VM_RESET, 0, NULL);
    else if (!strncmp(line, "shutdown", 8)) ask(VM_SHUTDOWN, atoi(line + 8), NULL);
    else if (!strcmp(line, "poweroff")) ask(VM_POWEROFF, 0, NULL);
    else if (!strncmp(line, "qmp ", 4)) { snprintf(a1, sizeof a1, "%s", line + 4); ask(VM_QMP, 0, a1); }
    else if (sscanf(line, "wait-frames %d %d", &x, &y) >= 1) {
        if (sscanf(line, "wait-frames %d %d", &x, &y) < 2) y = 30;
        frames_target = frames + x;
        frames_deadline = now_ms() + (long long)y * 1000;
    } else if (!strcmp(line, "quit")) return 1;
    else out("{\"ok\":false,\"why\":\"unknown command\"}");
    return 0;
}

static int connect_runtime(const char *dir) {
    /* the id is in machine.json; a tiny search is enough for a test tool */
    char p[PATH_MAX + 32], buf[65536];
    snprintf(p, sizeof p, "%s/machine.json", dir);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    char *id = strstr(buf, "\"id\": \"");
    if (!id) return -1;
    snprintf(machine_id, sizeof machine_id, "%.36s", id + 7);
    const char *rt = getenv("XDG_RUNTIME_DIR");
    char sp[PATH_MAX];
    snprintf(sp, sizeof sp, "%s/lordos-vm/%s.sock", rt ? rt : "/nonexistent", machine_id);
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if (strlen(sp) >= sizeof a.sun_path) { close(fd); return -1; }
    memcpy(a.sun_path, sp, strlen(sp) + 1);
    if (connect(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); return -1; }
    return fd;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    if (argc < 3 || (strcmp(argv[1], "run") && strcmp(argv[1], "attach"))) { fprintf(stderr, "usage: lordvm-dev run|attach DIR [lord-vm options]\n"); return 2; }
    find_bindir();
    if (!strcmp(argv[1], "attach")) {
        vm = connect_runtime(argv[2]);
        if (vm < 0) { out("{\"ev\":\"error\",\"what\":\"not running\"}"); return 3; }
        ask(VM_STATUS, 0, NULL);
    } else {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) return 1;
        pid_t p = fork();
        if (!p) {
            int c = fcntl(sv[1], F_DUPFD, 3);
            char cfd[16], bin[PATH_MAX + 16];
            snprintf(cfd, sizeof cfd, "%d", c);
            snprintf(bin, sizeof bin, "%s/lord-vm", bindir);
            char *args[72];
            int n = 0;
            /* LORDVM_SCOPE=1: lord-vm alone in a delegated scope of the user's manager, so it can make nested leaves */
            if (getenv("LORDVM_SCOPE") && !strcmp(getenv("LORDVM_SCOPE"), "1")) {
                args[n++] = "systemd-run"; args[n++] = "--user"; args[n++] = "--scope"; args[n++] = "-q"; args[n++] = "-p"; args[n++] = "Delegate=yes"; args[n++] = "--";
                args[n++] = bin;
            } else {
                args[n++] = "lord-vm";
            } args[n++] = "--start"; args[n++] = argv[2]; args[n++] = "--control-fd"; args[n++] = cfd; args[n++] = "--paused";
            for (int i = 3; i < argc && n < 70; i++) args[n++] = argv[i];
            args[n] = NULL;
            if (!strcmp(args[0], "systemd-run")) execvp("systemd-run", args);
            else execv(bin, args);
            _exit(127);
        }
        close(sv[1]);
        vm = sv[0];
        vm_pid = p;
        out("{\"ev\":\"lord-vm\",\"pid\":%d}", (int)p);
    }
    char line[4096];
    size_t ln = 0;
    int stdin_open = 1;
    for (;;) {
        kpump();
        if (frames_target >= 0 && (frames >= frames_target || now_ms() >= frames_deadline)) {
            out("{\"ok\":%s,\"cmd\":\"wait-frames\",\"frames\":%ld}", frames >= frames_target ? "true" : "false", frames);
            frames_target = -1;
        }
        if (vm < 0 && vd < 0) break;
        struct pollfd p[3] = { { vm, POLLIN, 0 }, { vd, POLLIN, 0 }, { stdin_open ? 0 : -1, POLLIN, 0 } };
        int to = kq_h != kq_t ? 12 : frames_target >= 0 ? 50 : 500;
        poll(p, 3, to);
        if (p[0].revents) from_vm();
        if (p[1].revents) from_vd();
        if (p[2].revents) {
            ssize_t k = read(0, line + ln, sizeof line - 1 - ln);
            if (k <= 0) { stdin_open = 0; continue; }
            ln += (size_t)k;
            line[ln] = 0;
            char *s = line, *nl;
            int quit = 0;
            while (!quit && (nl = strchr(s, '\n'))) { *nl = 0; quit = handle(s); s = nl + 1; }
            memmove(line, s, strlen(s) + 1);
            ln = strlen(line);
            if (quit) break;
        }
    }
    if (vd >= 0) { vdsend(VD_CLOSE, 0, 0); close(vd); }
    if (vd_pid > 0) waitpid(vd_pid, NULL, 0);
    if (vm >= 0) close(vm);          /* lord-vm shuts the machine down when its control channel closes */
    if (vm_pid > 0) { int st; waitpid(vm_pid, &st, 0); out("{\"ev\":\"lord-vm-exit\",\"status\":%d}", WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st)); }
    return 0;
}
