/* studio_helper_test.c — lord-studio itself, without a camera (docs/CAMERA-STUDIO-PLAN.md; make test-full):
 *   - probe=1: the model through an fd, the pipeline inside the sandbox on synthetic frames, on the GPU when a render
 *     node is given (one MM_RESULT {"gpu","segMs","composeMs"});
 *   - a file that is not a camera is refused (camera.notACamera, MS_FAILED);
 *   - any job other than the camera is refused (the helper exits);
 *   - a model with one byte changed is refused (studio.failed).
 * build/studio-helper-test build/lord-studio   (run from server/) */
#include "media/proto.h"
#include "media/studio/models.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *bin;

static int spawn(pid_t *pid) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) return -1;
    *pid = fork();
    if (*pid == 0) {
        dup2(sv[1], 3);
        char *argv[] = { (char *)bin, NULL };
        char *envp[] = { "LANG=C.UTF-8", NULL };
        execve(bin, argv, envp);
        _exit(127);
    }
    close(sv[1]);
    return sv[0];
}

/* waits up to ms for the next message; 1 if one came */
static int next(int s, MediaMsg *m, char *payload, int ms) {
    struct pollfd p = { s, POLLIN, 0 };
    if (poll(&p, 1, ms) <= 0) return 0;
    int fds[MP_MAX_FDS];
    int r = mp_recv(s, m, payload, MP_PAYLOAD_MAX + 1, fds, 0);
    for (int i = 0; i < MP_MAX_FDS; i++) if (r == 1 && fds[i] >= 0) close(fds[i]);
    return r == 1;
}

static int reap(pid_t pid, int ms) {
    int st = 0;
    for (int t = 0; t < ms; t += 20) {
        if (waitpid(pid, &st, WNOHANG) == pid) return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
        usleep(20000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
    return -1;
}

static int open_model(void) { return open("../assets/studio/models/" STUDIO_SEG_FILE, O_RDONLY | O_CLOEXEC); }

static void probe(int render) {
    pid_t pid;
    int s = spawn(&pid);
    int model = open_model();
    char params[64];
    snprintf(params, sizeof params, "probe=1\nrender=%d\n", render >= 0);
    int fds[2] = { render, model }, n = 0, f2[2];
    for (int i = 0; i < 2; i++) if (fds[i] >= 0) f2[n++] = fds[i];
    int64_t v[6] = { MK_CAMERA, 0, 0, 0, 0, 0 };
    CHECK(s >= 0 && model >= 0 && !mp_send(s, MM_JOB, 0, v, params, strlen(params), f2, n), "probe: send");
    close(model);
    static char payload[MP_PAYLOAD_MAX + 1];
    MediaMsg m;
    int got = 0;
    while (next(s, &m, payload, 20000)) if (m.type == MM_RESULT) { got = 1; break; }
    CHECK(got && m.v[0] == 0, "probe%s: no result (%s)", render >= 0 ? " (GPU)" : "", got ? payload : "nothing");
    if (got) printf("probe%s: %s\n", render >= 0 ? " with a render node" : " without a render node", payload);
    if (render >= 0 && got && strstr(payload, "\"gl\":true")) CHECK(strstr(payload, "\"gpu\":true"), "the GPU path did not run inside the sandbox");
    CHECK(reap(pid, 5000) == 0, "probe: the helper did not end well");
    close(s);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: studio-helper-test build/lord-studio\n"); return 2; }
    bin = argv[1];
    signal(SIGPIPE, SIG_IGN);
    probe(-1);
    int render = -1;
    for (int i = 128; i < 136 && render < 0; i++) {
        char dev[32];
        snprintf(dev, sizeof dev, "/dev/dri/renderD%d", i);
        render = open(dev, O_RDWR | O_CLOEXEC);
    }
    if (render >= 0) { probe(render); close(render); }
    else printf("probe with a render node: skipped (none)\n");
    static char payload[MP_PAYLOAD_MAX + 1];
    MediaMsg m;
    {   /* not a camera */
        pid_t pid;
        int s = spawn(&pid);
        int cam = open("/dev/null", O_RDWR | O_CLOEXEC), model = open_model();
        int fds[2] = { cam, model };
        const char *params = "render=0\n";
        int64_t v[6] = { MK_CAMERA, 0, 0, 0, 0, 0 };
        mp_send(s, MM_JOB, 0, v, params, strlen(params), fds, 2);
        close(cam);
        close(model);
        int refused = 0;
        while (next(s, &m, payload, 10000)) if (m.type == MM_ERROR && strstr(payload, "camera.notACamera")) refused = 1; else if (m.type == MM_STATE && m.v[0] == MS_FAILED) break;
        CHECK(refused, "/dev/null was taken for a camera");
        close(s);
        reap(pid, 3000);
    }
    {   /* another job */
        pid_t pid;
        int s = spawn(&pid);
        int64_t v[6] = { MK_WORKER, 0, 0, 0, 0, 0 };
        mp_send(s, MM_JOB, 0, v, NULL, 0, NULL, 0);
        CHECK(reap(pid, 5000) == 1, "lord-studio accepted a worker job");
        close(s);
    }
    {   /* a damaged model (through a sealed copy with one byte changed) */
        int src = open_model();
        char tmpl[] = "/tmp/studio-model-XXXXXX";
        int t = mkstemp(tmpl);
        unlink(tmpl);
        char buf[65536];
        ssize_t r;
        while (src >= 0 && t >= 0 && (r = read(src, buf, sizeof buf)) > 0) CHECK(write(t, buf, (size_t)r) == r, "copy");
        if (src >= 0) close(src);
        unsigned char b = 0;
        CHECK(pread(t, &b, 1, 70) == 1, "read");
        b ^= 0xff;
        CHECK(pwrite(t, &b, 1, 70) == 1, "write");        /* the first tensor's kind */
        pid_t pid;
        int s = spawn(&pid);
        const char *params = "probe=1\nrender=0\n";
        int64_t v[6] = { MK_CAMERA, 0, 0, 0, 0, 0 };
        mp_send(s, MM_JOB, 0, v, params, strlen(params), &t, 1);
        close(t);
        int refused = 0;
        while (next(s, &m, payload, 10000)) if (m.type == MM_ERROR && strstr(payload, "studio.failed")) { refused = 1; break; }
        CHECK(refused, "a damaged model was accepted");
        reap(pid, 3000);
        close(s);
    }
    if (fails) { printf("studio-helper-test: %d failures\n", fails); return 1; }
    printf("studio-helper-test: ok\n");
    return 0;
}
