/* mok_admin.c — dkms-mok-enroll (see mok_admin.h). Runs as root in the admin helper. */
#include "mok_admin.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

/* who may own the key, its folder and mokutil (tests build it with their own uid) */
#ifndef MOK_OWNER_OK
#define MOK_OWNER_OK(st) ((st).st_uid == 0)
#endif

#define EFI_GLOBAL "-8be4df61-93ca-11d2-aa0d-00e098032b8c"
#define SHIM_GUID "-605dab50-e046-4300-abb6-3dd810dd8b23"
#define VAR_MAX (1 << 20)                       /* a shim list larger than this isn't read */

const char *mok_state_name(int state) {
    static const char *const names[] = { "unsupported", "off", "nokey", "enrolled", "pending", "needed" };
    return state >= 0 && state <= MOK_NEEDED ? names[state] : "unsupported";
}

/* a whole small file (at most cap bytes; more is an error) */
static ssize_t read_all(int fd, uint8_t *buf, size_t cap) {
    size_t n = 0;
    for (;;) {
        if (n == cap) {
            uint8_t extra;
            ssize_t r = read(fd, &extra, 1);
            if (r < 0 && errno == EINTR) continue;
            return r == 0 ? (ssize_t)n : -1;
        }
        ssize_t r = read(fd, buf + n, cap - n);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return (ssize_t)n;
        n += (size_t)r;
    }
}

int mok_secure_boot(const char *efivars) {
    struct stat st;
    if (stat(efivars, &st) || !S_ISDIR(st.st_mode)) return -1;
    char p[512];
    snprintf(p, sizeof p, "%s/SecureBoot" EFI_GLOBAL, efivars);
    int fd = open(p, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    uint8_t b[8];
    ssize_t n = read_all(fd, b, sizeof b);
    close(fd);
    return n == 5 && b[4] == 1;                 /* 4 bytes of attributes, then the value */
}

/* a file or folder of the key pair: what lstat says, no link, the right owner, nobody else can write it */
static int safe_node(const char *path, int dir, struct stat *out) {
    struct stat st;
    if (lstat(path, &st) || (dir ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode)) || !MOK_OWNER_OK(st) || (st.st_mode & 022)) return -1;
    if (out) *out = st;
    return 0;
}

int mok_key_load(const char *dir, uint8_t *out, size_t cap, size_t *n) {
    char pub[512], key[512];
    snprintf(pub, sizeof pub, "%s/mok.pub", dir);
    snprintf(key, sizeof key, "%s/mok.key", dir);
    struct stat ls, fs;
    if (safe_node(dir, 1, NULL) || safe_node(key, 0, NULL) || safe_node(pub, 0, &ls)) return -1;
    if (ls.st_size < 64 || ls.st_size > MOK_KEY_MAX || (size_t)ls.st_size > cap) return -1;
    int fd = open(pub, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    if (fstat(fd, &fs) || fs.st_ino != ls.st_ino || fs.st_dev != ls.st_dev) { close(fd); return -1; }
    ssize_t r = read_all(fd, out, cap);
    close(fd);
    /* one DER certificate: a SEQUENCE with a two-byte length that covers the whole file */
    if (r != ls.st_size || r < 64 || out[0] != 0x30 || out[1] != 0x82 || (size_t)((out[2] << 8 | out[3]) + 4) != (size_t)r) return -1;
    *n = (size_t)r;
    return 0;
}

/* "<prefix>", "<prefix>1", "<prefix>12"… followed by the shim GUID, nothing else */
static int shim_var(const char *name, const char *prefix) {
    size_t pl = strlen(prefix);
    if (strncmp(name, prefix, pl)) return 0;
    const char *p = name + pl;
    while (*p >= '0' && *p <= '9') p++;
    return !strcmp(p, SHIM_GUID);
}

int mok_var_has(const char *efivars, const char *prefix, const uint8_t *der, size_t n) {
    DIR *d = opendir(efivars);
    if (!d) return 0;
    uint8_t *buf = malloc(VAR_MAX);
    int found = 0;
    struct dirent *e;
    while (buf && !found && (e = readdir(d))) {
        if (!shim_var(e->d_name, prefix)) continue;
        int fd = openat(dirfd(d), e->d_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) continue;
        struct stat st;
        ssize_t r = fstat(fd, &st) || !S_ISREG(st.st_mode) ? -1 : read_all(fd, buf, VAR_MAX);
        close(fd);
        if (r > 4 && (size_t)r - 4 >= n && memmem(buf + 4, (size_t)r - 4, der, n)) found = 1;
    }
    free(buf);
    closedir(d);
    return found;
}

int mok_state(const char *efivars, const char *dkms_dir) {
    int sb = mok_secure_boot(efivars);
    if (sb < 0) return MOK_UNSUPPORTED;
    if (!sb) return MOK_OFF;
    uint8_t der[MOK_KEY_MAX];
    size_t n;
    if (mok_key_load(dkms_dir, der, sizeof der, &n)) return MOK_NOKEY;
    if (mok_var_has(efivars, "MokListRT", der, n)) return MOK_ENROLLED;
    if (mok_var_has(efivars, "MokNew", der, n)) return MOK_PENDING;
    return MOK_NEEDED;
}

int mok_password(char pw[MOK_PASSWORD_DIGITS + 1]) {
    int k = 0;
    while (k < MOK_PASSWORD_DIGITS) {
        uint8_t b[16];
        if (getrandom(b, sizeof b, 0) != (ssize_t)sizeof b) { explicit_bzero(b, sizeof b); return -1; }
        for (size_t i = 0; i < sizeof b && k < MOK_PASSWORD_DIGITS; i++)
            if (b[i] < 250) pw[k++] = (char)('0' + b[i] % 10);       /* 250 = 25 × 10: no digit more likely */
        explicit_bzero(b, sizeof b);
    }
    pw[k] = 0;
    return 0;
}

/* mokutil at its fixed path, with input on standard input and its output discarded (it would only echo prompts): the
 * password never reaches argv, a file, a log or the UI's progress lines. Returns its exit code. */
static int run_mokutil(const char *const argv[], const char *input) {
    struct stat st;
    if (safe_node(MOK_UTIL, 0, &st) || !(st.st_mode & 0100)) return -2;
    int in[2];
    if (pipe2(in, O_CLOEXEC)) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(in[0]); close(in[1]); return -1; }
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        dup2(in[0], 0);
        if (null >= 0) { dup2(null, 1); dup2(null, 2); }
        syscall(SYS_close_range, 3, ~0u, 0);
        signal(SIGPIPE, SIG_DFL);
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        char *const env[] = { "PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LANG=C", NULL };
        execve(argv[0], (char *const *)argv, env);
        _exit(127);
    }
    close(in[0]);
    size_t n = input ? strlen(input) : 0;
    for (size_t o = 0; o < n;) {
        ssize_t w = write(in[1], input + o, n - o);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;
        o += (size_t)w;
    }
    close(in[1]);
    int s = 0;
    while (waitpid(pid, &s, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(s) ? WEXITSTATUS(s) : 128;
}

static int revoke_request(void) {
    const char *const argv[] = { MOK_UTIL, "--revoke-import", NULL };
    return run_mokutil(argv, NULL);
}

void hw_act_dkms_mok_enroll(const AdminReq *r, const char *arg, size_t len) {
    (void)r;
    (void)len;
    int st = mok_state(MOK_EFIVARS, MOK_DKMS_DIR);
    if (!strcmp(arg, "status")) { admin_ok("dkms.mokState", "state", mok_state_name(st), NULL); return; }
    int import = !strcmp(arg, "import");
    if (!import && strcmp(arg, "cancel")) { admin_err("dkms.mokInvalid", NULL); return; }
    if (import && st != MOK_NEEDED && st != MOK_PENDING) { admin_err("dkms.mokNotNeeded", "state", mok_state_name(st), NULL); return; }
    if (!import && st != MOK_PENDING) { admin_err("dkms.mokNothingPending", NULL); return; }
    /* a pending request is withdrawn first: a new import makes a new password, and only the last one counts */
    if (st == MOK_PENDING) {
        int rc = revoke_request();
        if (rc == -2) { admin_err("dkms.mokNoTool", NULL); return; }
        if (rc || mok_state(MOK_EFIVARS, MOK_DKMS_DIR) == MOK_PENDING) { admin_err("dkms.mokFailed", NULL); return; }
    }
    if (!import) { admin_ok("dkms.mokState", "state", mok_state_name(mok_state(MOK_EFIVARS, MOK_DKMS_DIR)), NULL); return; }

    char pw[MOK_PASSWORD_DIGITS + 1], twice[2 * MOK_PASSWORD_DIGITS + 3];
    if (mok_password(pw)) { admin_err("dkms.mokFailed", NULL); return; }
    snprintf(twice, sizeof twice, "%s\n%s\n", pw, pw);  /* mokutil asks for it, then again to confirm */
    char key[512];
    snprintf(key, sizeof key, "%s/mok.pub", MOK_DKMS_DIR);
    const char *const argv[] = { MOK_UTIL, "--import", key, NULL };
    int rc = run_mokutil(argv, twice);
    explicit_bzero(twice, sizeof twice);
    if (rc == -2) admin_err("dkms.mokNoTool", NULL);
    else if (rc || mok_state(MOK_EFIVARS, MOK_DKMS_DIR) != MOK_PENDING) admin_err("dkms.mokFailed", NULL);
    else admin_ok("dkms.mokPassword", "password", pw, NULL);      /* once: the UI shows it, nothing keeps it */
    explicit_bzero(pw, sizeof pw);
}
