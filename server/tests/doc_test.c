/* doc_test.c — test client for lord-doc (the document helper), without lord-server.
 *
 *   doc-test <lord-doc> selftest                    what the confinement blocks (JSON with errno)
 *   doc-test <lord-doc> list <archive>
 *   doc-test <lord-doc> extract <archive> <folder>    (the folder must exist)
 *   doc-test <lord-doc> zip <out.zip> <paths…>
 *   doc-test <lord-doc> pdf <file.pdf> [width]      opens, renders page 1 and measures
 *   doc-test <lord-doc> font <file.ttf>
 *   doc-test <lord-ocr> selftest-ocr                what lord-ocr's confinement blocks
 *   doc-test <lord-ocr> ocr <file.pgm> [langs]      the text in a gray picture (P5)
 *
 * tests/doc_evil.sh uses it with malicious archives built by tests/make_evil_archives.py. */
#include "doc/proto.h"
#include "doc/wire.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static int sock;
static pid_t child;

static void start(const char *helper, const char *mode, const int *fds, const char *const *roles, int n) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) { perror("socketpair"); exit(2); }
    child = fork();
    if (!child) {
        dup2(sv[1], 3);
        char *argv[] = { "lord-doc", NULL };
        execv(helper, argv);
        _exit(127);
    }
    close(sv[1]);
    sock = sv[0];
    for (int i = 0; i < n; i++) {
        char j[48];
        int l = snprintf(j, sizeof j, "{\"role\":\"%s\"}", roles[i]);
        doc_send(sock, DQ_INIT_FD, 0, j, (size_t)l, &fds[i], 1);
        close(fds[i]);
    }
    char j[48];
    int l = snprintf(j, sizeof j, "{\"mode\":\"%s\"}", mode);
    doc_send(sock, DQ_INIT_GO, 0, j, (size_t)l, NULL, 0);
    DocMsg m;
    if (doc_recv(sock, &m) || m.h.op != DR_DONE) { fprintf(stderr, "the helper did not start\n"); exit(2); }
    doc_msg_free(&m);
}

/* sends, and prints everything up to the final reply; returns the final type */
static int call(uint32_t op, const char *json, const int *fds, int nfds, int quiet_prog) {
    static uint32_t id = 1;
    doc_send(sock, op, id, json, json ? strlen(json) : 0, fds, nfds);
    for (;;) {
        DocMsg m;
        int r = doc_recv(sock, &m);
        if (r) { printf("{\"crash\":true}\n"); return -1; }
        int kind = (int)m.h.op;
        if (kind == DR_PROG && quiet_prog) { doc_msg_free(&m); continue; }
        const char *names[] = { "", "done", "err", "prog", "part", "img", "blob" };
        printf("%s %s\n", kind >= 1 && kind <= 6 ? names[kind] : "?", m.json ? m.json : "");
        doc_msg_free(&m);
        if (kind != DR_PROG && kind != DR_PART) { id++; return kind; }
    }
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: doc-test <lord-doc> selftest|list|extract|zip|pdf|font …\n"); return 2; }
    const char *helper = argv[1], *cmd = argv[2];
    int st = 0;
    if (!strcmp(cmd, "selftest")) {
        start(helper, "pdf", NULL, NULL, 0);
        st = call(DQ_SELFTEST, NULL, NULL, 0, 0) == DR_DONE ? 0 : 1;
    } else if (!strcmp(cmd, "selftest-ocr")) {
        start(helper, "ocr", NULL, NULL, 0);
        st = call(DQ_SELFTEST, NULL, NULL, 0, 0) == DR_DONE ? 0 : 1;
    } else if (!strcmp(cmd, "ocr") && argc > 3) {
        /* P5 → premultiplied ARGB32 in a memfd, as lord-server sends a screenshot */
        FILE *f = fopen(argv[3], "rb");
        int w = 0, h = 0, max = 0;
        if (!f || fscanf(f, "P5 %d %d %d", &w, &h, &max) != 3 || w < 1 || h < 1 || w > 8192 || h > 8192 || max != 255) { fprintf(stderr, "not a P5 picture\n"); return 2; }
        fgetc(f);
        size_t n = (size_t)w * h;
        unsigned char *g = malloc(n);
        uint32_t *px = malloc(n * 4);
        if (!g || !px || fread(g, 1, n, f) != n) { fprintf(stderr, "short picture\n"); return 2; }
        fclose(f);
        for (size_t i = 0; i < n; i++) px[i] = 0xff000000u | (uint32_t)g[i] << 16 | (uint32_t)g[i] << 8 | g[i];
        int fd = memfd_create("ocr-test", MFD_CLOEXEC);
        if (fd < 0 || write(fd, px, n * 4) != (ssize_t)(n * 4)) { perror("memfd"); return 2; }
        free(g);
        free(px);
        char json[128];
        snprintf(json, sizeof json, "{\"w\":%d,\"h\":%d,\"langs\":\"%s\"}", w, h, argc > 4 ? argv[4] : "eng");
        start(helper, "ocr", NULL, NULL, 0);
        st = call(DQ_OCR, json, &fd, 1, 0) == DR_DONE ? 0 : 1;
    } else if (!strcmp(cmd, "selftest-arc") && argc > 3) {
        /* extraction mode, with a writable folder: it still cannot leave it */
        int fd = open(argv[3], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        const char *roles[] = { "rw" };
        start(helper, "arc", &fd, roles, 1);
        st = call(DQ_SELFTEST, NULL, NULL, 0, 0) == DR_DONE ? 0 : 1;
    } else if (!strcmp(cmd, "list") && argc > 3) {
        int fd = open(argv[3], O_RDONLY | O_CLOEXEC);
        const char *roles[] = { "file" };
        start(helper, "arc", &fd, roles, 1);
        st = call(DQ_ARC_LIST, "{\"slot\":0}", NULL, 0, 1) == DR_DONE ? 0 : 1;
    } else if (!strcmp(cmd, "extract") && argc > 4) {
        int fds[2] = { open(argv[3], O_RDONLY | O_CLOEXEC), open(argv[4], O_RDONLY | O_DIRECTORY | O_CLOEXEC) };
        if (fds[0] < 0 || fds[1] < 0) { perror("open"); return 2; }
        const char *roles[] = { "file", "rw" };
        start(helper, "arc", fds, roles, 2);
        st = call(DQ_ARC_EXTRACT, "{\"slot\":0,\"dir\":1,\"rawname\":\"raw.bin\"}", NULL, 0, 1) == DR_DONE ? 0 : 1;
    } else if (!strcmp(cmd, "zip") && argc > 4) {
        int n = argc - 4;
        int *fds = calloc((size_t)n + 1, sizeof *fds);
        const char **roles = calloc((size_t)n + 1, sizeof *roles);
        char json[8192];
        size_t o = (size_t)snprintf(json, sizeof json, "{\"out\":%d,\"items\":[", n);
        for (int i = 0; i < n; i++) {
            fds[i] = open(argv[4 + i], O_RDONLY | O_CLOEXEC);
            struct stat s;
            fstat(fds[i], &s);
            roles[i] = S_ISDIR(s.st_mode) ? "ro" : "file";
            const char *b = strrchr(argv[4 + i], '/');
            o += (size_t)snprintf(json + o, sizeof json - o, "%s{\"slot\":%d,\"name\":\"%s\"}", i ? "," : "", i, b ? b + 1 : argv[4 + i]);
        }
        snprintf(json + o, sizeof json - o, "]}");
        fds[n] = open(argv[3], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        roles[n] = "out";
        start(helper, "arc", fds, roles, n + 1);
        st = call(DQ_ARC_CREATE, json, NULL, 0, 1) == DR_DONE ? 0 : 1;
    } else if (!strcmp(cmd, "pdf") && argc > 3) {
        double t0 = now_ms();
        int fd = open(argv[3], O_RDONLY | O_CLOEXEC);
        const char *roles[] = { "file" };
        start(helper, "pdf", &fd, roles, 1);
        double t1 = now_ms();
        call(DQ_PDF_OPEN, "{\"slot\":0}", NULL, 0, 0);
        double t2 = now_ms();
        char j[96];
        snprintf(j, sizeof j, "{\"page\":0,\"width\":%s}", argc > 4 ? argv[4] : "600");
        call(DQ_PDF_RENDER, j, NULL, 0, 0);
        double t3 = now_ms();
        call(DQ_PDF_OUTLINE, NULL, NULL, 0, 0);
        call(DQ_PDF_FIND, "{\"text\":\"the\"}", NULL, 0, 0);
        printf("times: start+confinement %.1f ms, open %.1f ms, page 1 %.1f ms, total until the image %.1f ms\n", t1 - t0, t2 - t1, t3 - t2, t3 - t0);
    } else if (!strcmp(cmd, "thumb") && argc > 3) {
        /* Quick Preview: the utility session already warm; the second run is measured */
        start(helper, "util", NULL, NULL, 0);
        for (int i = 0; i < 3; i++) {
            int fd = open(argv[3], O_RDONLY | O_CLOEXEC);
            double t0 = now_ms();
            call(DQ_PDF_THUMB, argc > 4 ? argv[4] : "{\"width\":420,\"height\":420}", &fd, 1, 0);
            printf("miniatura %d: %.1f ms\n", i + 1, now_ms() - t0);
            close(fd);
        }
    } else if (!strcmp(cmd, "save") && argc > 4) {
        /* opens, adds a note and saves a copy (like the viewer's "Save a copy") */
        int fd = open(argv[3], O_RDONLY | O_CLOEXEC);
        const char *roles[] = { "file" };
        start(helper, "pdf", &fd, roles, 1);
        call(DQ_PDF_OPEN, "{\"slot\":0,\"quick\":true}", NULL, 0, 1);
        if (argc > 5) {                      /* like the viewer: sizes, renders, search and selection */
            call(DQ_PDF_SIZES, NULL, NULL, 0, 1);
            for (int p = 0; p < 10; p++) { char j[64]; snprintf(j, sizeof j, "{\"page\":%d,\"width\":800}", p); call(DQ_PDF_RENDER, j, NULL, 0, 1); }
            call(DQ_PDF_FIND, "{\"text\":\"linear memory\"}", NULL, 0, 1);
            call(DQ_PDF_SELECT, "{\"page\":7,\"x1\":100,\"y1\":200,\"x2\":300,\"y2\":210}", NULL, 0, 1);
            call(DQ_PDF_PAGEINFO, "{\"page\":7}", NULL, 0, 1);
        }
        call(DQ_PDF_ANNOT, "{\"page\":0,\"type\":\"note\",\"r\":[50,50,22,22],\"text\":\"hola\"}", NULL, 0, 0);
        call(DQ_PDF_ANNOT, "{\"page\":0,\"type\":\"highlight\",\"rects\":[[60,100,200,14]],\"color\":\"#ffd60a\"}", NULL, 0, 0);
        call(DQ_PDF_ANNOT, getenv("INK") ? getenv("INK") : "{\"page\":0,\"type\":\"ink\",\"strokes\":[[100,300,150,280,200,320,250,290]],\"color\":\"#ff6482\",\"width\":2.2}", NULL, 0, 0);
        int out = open(argv[4], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        st = call(DQ_PDF_SAVE, "{}", &out, 1, 0) == DR_DONE ? 0 : 1;
        close(out);
        /* the file must already be complete, with the helper still alive */
        int chk = open(argv[4], O_RDONLY | O_CLOEXEC);
        char tail[16] = "";
        off_t end = lseek(chk, 0, SEEK_END);
        if (end > 8 && pread(chk, tail, 8, end - 8) == 8) tail[8] = 0;
        close(chk);
        int complete = strstr(tail, "%%EOF") != NULL;
        printf("copy saved: %lld bytes, %s\n", (long long)end, complete ? "complete" : "INCOMPLETE");
        if (!complete) st = 1;
    } else if (!strcmp(cmd, "font") && argc > 3) {
        start(helper, "util", NULL, NULL, 0);
        int fd = open(argv[3], O_RDONLY | O_CLOEXEC);
        call(DQ_FONT_INFO, "{}", &fd, 1, 0);
        call(DQ_FONT_RENDER, "{\"lines\":[{\"text\":\"Hola mundo\",\"size\":32}],\"width\":400}", &fd, 1, 0);
        close(fd);
    } else { fprintf(stderr, "orden desconocida\n"); return 2; }
    close(sock);
    waitpid(child, NULL, 0);
    return st;
}
