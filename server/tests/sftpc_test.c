/* sftpc_test.c — lord-server's Files mode side (term/sftpc.c) end to end, against a real ssh master
 * (tests/sftp_e2e.sh starts a private sshd as the user and an ssh ControlMaster to it): the master is
 * found by its pid, an sftp channel opened on it with lord-sftp, a listing, an upload and a download
 * through lord.fs's rules, `file` on the server, a forward to the server's own sshd port, a refused
 * local path, and close.
 *   XDG_RUNTIME_DIR=<rt> ./build/sftpc-test <master pid> <home> <remote dir> <sshd port> */
#include "../src/term/sftpc.h"
#include "../src/fsops.h"
#include "../src/sec/json.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int pass, failn;
#define CHECK(c, ...) do { if (c) pass++; else { failn++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static pid_t master;
static pid_t fg(int sid) { return sid == 7 ? master : -1; }

/* waits for request id's answer (events of job are passed to on_evt); the answer's JSON (malloc'd) */
static char *wait_for(int id, int *type, int job, int *job_done, char *job_err, size_t jen) {
    for (int t = 0; t < 600; t++) {
        SftpItem *it;
        int n = sftp_poll(&it);
        char *got = NULL;
        for (int i = 0; i < n; i++) {
            if (it[i].type == SFTP_EVT && job && strstr(it[i].data, "\"job\":") ) {
                char k[32];
                snprintf(k, sizeof k, "\"job\":%d", job);
                if (strstr(it[i].data, k) && strstr(it[i].data, "\"ev\":\"done\"")) *job_done = 1;
                if (strstr(it[i].data, k) && strstr(it[i].data, "\"ev\":\"error\"")) { *job_done = -1; snprintf(job_err, jen, "%s", it[i].data); }
            }
            if (id && it[i].id == id && !got) { got = strdup(it[i].data); *type = it[i].type; }
        }
        sftp_items_free(it, n);
        if (got) return got;
        if (!id && job && *job_done) return strdup("");
        struct timespec ts = { 0, 50 * 1000000 };
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static char *ask(const char *json, int *type) {
    int id = sftp_request(json, strlen(json));
    if (id < 0) return NULL;
    int d = 0;
    return wait_for(id, type, 0, &d, NULL, 0);
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: sftpc-test <master pid> <home> <remote dir> <sshd port>\n"); return 2; }
    master = (pid_t)atoi(argv[1]);
    const char *home = argv[2], *rdir = argv[3];
    int port = atoi(argv[4]);
    setenv("HOME", home, 1);
    fsops_init(home, getenv("USER") ? getenv("USER") : "user");
    sftp_set_fg(fg);
    CHECK(sftp_available() == 0, "lord-sftp next to the test binary");
    CHECK(sftp_mux_dir() != NULL, "the masters' folder");
    int t;
    char q[1024], *a;

    a = ask("{\"op\":\"open\",\"sid\":3}", &t);
    CHECK(a && t == SFTP_ERR && strstr(a, "nossh"), "a session with no ssh master: %s", a ? a : "-");
    free(a);
    a = ask("{\"op\":\"open\",\"sid\":7}", &t);
    CHECK(a && t == SFTP_OK && strstr(a, "\"conn\":"), "open: %s", a ? a : "-");
    int conn = a ? atoi(strstr(a, "\"conn\":") + 7) : 0;
    free(a);

    snprintf(q, sizeof q, "{\"op\":\"list\",\"conn\":%d,\"path\":\"%s\"}", conn, rdir);
    a = ask(q, &t);
    CHECK(a && t == SFTP_OK && strstr(a, "\"name\":\"hello.txt\""), "list: %.200s", a ? a : "-");
    free(a);

    /* upload a file from home */
    char src[600], dst[600];
    snprintf(src, sizeof src, "%s/up.bin", home);
    FILE *f = fopen(src, "w");
    for (int i = 0; i < 300000; i++) fputc('a' + i % 26, f);
    fclose(f);
    snprintf(q, sizeof q, "{\"op\":\"put\",\"conn\":%d,\"src\":\"%s\",\"path\":\"%s/up.bin\"}", conn, src, rdir);
    a = ask(q, &t);
    CHECK(a && t == SFTP_OK && strstr(a, "\"job\":"), "put started: %s", a ? a : "-");
    int job = a && strstr(a, "\"job\":") ? atoi(strstr(a, "\"job\":") + 6) : 0;
    free(a);
    int done = 0;
    char err[300] = "";
    free(wait_for(0, &t, job, &done, err, sizeof err));
    CHECK(done == 1, "put done %s", err);
    struct stat st;
    snprintf(dst, sizeof dst, "%s/up.bin", rdir);
    CHECK(!stat(dst, &st) && st.st_size == 300000, "the uploaded file on the server: %ld", (long)st.st_size);

    /* download it back into home */
    snprintf(q, sizeof q, "{\"op\":\"get\",\"conn\":%d,\"path\":\"%s/up.bin\",\"dest\":\"%s/down.bin\"}", conn, rdir, home);
    a = ask(q, &t);
    job = a && strstr(a, "\"job\":") ? atoi(strstr(a, "\"job\":") + 6) : 0;
    free(a);
    done = 0;
    free(wait_for(0, &t, job, &done, err, sizeof err));
    snprintf(dst, sizeof dst, "%s/down.bin", home);
    CHECK(done == 1 && !stat(dst, &st) && st.st_size == 300000, "get done: %ld %s", (long)st.st_size, err);
    /* the same again: exists, unless replace */
    snprintf(q, sizeof q, "{\"op\":\"get\",\"conn\":%d,\"path\":\"%s/up.bin\",\"dest\":\"%s/down.bin\"}", conn, rdir, home);
    a = ask(q, &t);
    CHECK(a && t == SFTP_ERR && strstr(a, "exists"), "an existing file is not replaced unasked: %s", a ? a : "-");
    free(a);
    /* a download outside the home folder is refused (lord.fs's rules) */
    snprintf(q, sizeof q, "{\"op\":\"get\",\"conn\":%d,\"path\":\"%s/up.bin\",\"dest\":\"/etc/lordos-test.bin\"}", conn, rdir);
    a = ask(q, &t);
    CHECK(a && t == SFTP_ERR && strstr(a, "denied"), "a download into /etc is refused: %s", a ? a : "-");
    free(a);

    /* file on the server: the closed command list */
    snprintf(q, sizeof q, "{\"op\":\"info\",\"conn\":%d,\"path\":\"%s/hello.txt\",\"what\":\"file\"}", conn, rdir);
    a = ask(q, &t);
    CHECK(a && t == SFTP_OK && strstr(a, "text"), "file: %s", a ? a : "-");
    free(a);
    snprintf(q, sizeof q, "{\"op\":\"info\",\"conn\":%d,\"path\":\"%s/hello.txt\",\"what\":\"rm\"}", conn, rdir);
    a = ask(q, &t);
    CHECK(a && t == SFTP_ERR, "a command outside the list is refused: %s", a ? a : "-");
    free(a);

    /* a forward: the server's own sshd port on a free local port; its banner read through it */
    snprintf(q, sizeof q, "{\"op\":\"forward\",\"sid\":7,\"host\":\"127.0.0.1\",\"port\":%d}", port);
    a = ask(q, &t);
    CHECK(a && t == SFTP_OK && strstr(a, "\"local\":"), "forward: %s", a ? a : "-");
    int local = a && strstr(a, "\"local\":") ? atoi(strstr(a, "\"local\":") + 8) : 0;
    free(a);
    if (local > 0) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons((uint16_t)local) };
        inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
        char banner[64] = "";
        if (!connect(fd, (struct sockaddr *)&sa, sizeof sa)) {
            struct timeval tv = { 5, 0 };
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            ssize_t k = read(fd, banner, sizeof banner - 1);
            if (k > 0) banner[k] = 0;
        }
        close(fd);
        CHECK(!strncmp(banner, "SSH-2.0-", 8), "the server's banner through the forward: '%.20s'", banner);
        /* the same local port again (two tabs of one profile): told it's taken, not a refusal from the server */
        snprintf(q, sizeof q, "{\"op\":\"forward\",\"sid\":7,\"host\":\"127.0.0.1\",\"port\":%d,\"local\":%d}", port, local);
        a = ask(q, &t);
        CHECK(a && t == SFTP_ERR && strstr(a, "inuse"), "a local port in use: %s", a ? a : "-");
        free(a);
        snprintf(q, sizeof q, "{\"op\":\"forward\",\"sid\":7,\"host\":\"127.0.0.1\",\"port\":%d,\"local\":%d,\"close\":true}", port, local);
        a = ask(q, &t);
        CHECK(a && t == SFTP_OK, "forward closed: %s", a ? a : "-");
        free(a);
    }
    a = ask("{\"op\":\"forward\",\"sid\":7,\"host\":\"a b\",\"port\":80}", &t);
    CHECK(a && t == SFTP_ERR && strstr(a, "bad"), "a host with a space is refused: %s", a ? a : "-");
    free(a);

    snprintf(q, sizeof q, "{\"op\":\"close\",\"conn\":%d}", conn);
    a = ask(q, &t);
    CHECK(a && t == SFTP_OK, "close");
    free(a);
    printf("sftpc-test: %d passed, %d failed\n", pass, failn);
    return failn ? 1 : 0;
}
