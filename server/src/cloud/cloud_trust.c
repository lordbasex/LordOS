/* cloud_trust.c — who the server is (docs/CLOUD.md 17.2 as built, B9 and B6), the model of Connect to Server
 * (docs/FILES.md, "Who the server is"; lan/rconnect.c).
 *
 * SFTP: the host's keys are read with ssh-keyscan before an account is added or updated. Keys never seen are shown to the
 * person as fingerprints (a hostKey question); accepted ones go to ~/.config/lordos/cloud/known_hosts/<id> (0600), and
 * rclone always gets known_hosts_file pointing there, so it checks the same keys on every connection. A key that changed
 * is refused (cloud.hostKeyChanged) with both fingerprints.
 * TLS (an https address: WebDAV, S3 and the rest): openssl s_client reads the certificate. One the system trusts goes on;
 * one it doesn't is shown (a certificate question: fingerprint, subject, issuer) and, accepted, kept in
 * ~/.config/lordos/cloud/certs/<id>.pem (0600) and given to that account alone (rclone's override.ca_cert): only that
 * certificate is trusted for it, never no_check_certificate. A pinned certificate that changed is refused
 * (cloud.certChanged). The certificate must name the host it is used for.
 * ssh-keyscan, ssh-keygen and openssl run through rconnect_run in Cloud's scope: the system read-only, a deadline,
 * no TCP for the offline ones. */
#include "cloud_int.h"
#include "../doc/wire.h"
#include "../lan/rconnect.h"
#include "../sec/json.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define KEYSCAN "/usr/bin/ssh-keyscan"
#define KEYGEN "/usr/bin/ssh-keygen"
#define OPENSSL "/usr/bin/openssl"
#define TRUST_MS 15000

/* an option of the account: its non-secret ones, or what add() was given as secret (rclone marks a host or a user
 * Sensitive, so they travel with the password until the keyring has them) */
static const char *cur_sec;
static char sec_val[3][1024];
static int sec_next;
static const char *param(const Acc *a, const char *k) {
    for (int i = 0; i < a->p.n; i++) if (!strcmp(a->p.kv[i].key, k)) return a->p.kv[i].val;
    JDoc d;
    const char *r = NULL;
    if (cur_sec && !json_parse(cur_sec, strlen(cur_sec), &d, NULL, 0)) {
        const char *v = json_str(d.root, k);
        if (v && strlen(v) < sizeof sec_val[0]) { char *o = sec_val[sec_next++ % 3]; cl_pf(o, sizeof sec_val[0], "%s", v); r = o; }
        json_free(&d);
    }
    return r;
}

static int run(char *const argv[], const char *in, int net, char *out, size_t cap) {
    RconnectRun j = { .argv = argv, .input = in ? in : "", .ilen = in ? strlen(in) : 0, .out = out, .cap = cap,
                      .timeout_ms = TRUST_MS, .offline = !net, .any_status = net, .scope = CLOUD_SCOPE };
    return rconnect_run(&j);
}

static char *slurp(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return NULL;
    char *b = malloc(262144);
    ssize_t n = b ? read(fd, b, 262143) : -1;
    close(fd);
    if (n < 0) { free(b); return NULL; }
    b[n] = 0;
    return b;
}

static void path_of(const Acc *a, int ssh, char *out, size_t n) {
    cl_pf(out, n, "%s/%s/%s%s", cl_cfg, ssh ? "known_hosts" : "certs", a->id, ssh ? "" : ".pem");
}

static int is_sftp(const Acc *a) { return !strcmp(a->k->type, "sftp"); }

/* the https endpoint of an account: host and port, 0; -1 none */
static int tls_endpoint(const Acc *a, char *host, size_t hn, int *port) {
    const char *u = param(a, "url"), *e = param(a, "endpoint");
    const char *v = u && !strncmp(u, "https://", 8) ? u + 8 : e && !strncmp(e, "https://", 8) ? e + 8
                  : e && !strstr(e, "://") && !strcmp(a->k->group, "object") ? e : NULL;
    if (!v) return -1;
    size_t l = strcspn(v, "/?#");
    char hp[300];
    if (!l || l >= sizeof hp) return -1;
    memcpy(hp, v, l);
    hp[l] = 0;
    *port = 443;
    char *c = hp[0] == '[' ? strstr(hp, "]:") : strrchr(hp, ':');
    if (c && hp[0] == '[') { *c = 0; *port = atoi(c + 2); memmove(hp, hp + 1, strlen(hp)); }
    else if (c && !strchr(c + 1, ':')) { *c = 0; *port = atoi(c + 1); }
    if (*port <= 0 || *port > 65535 || !rconnect_host_ok(hp) || strlen(hp) >= hn) return -1;
    cl_pf(host, hn, "%s", hp);
    return 0;
}

int ct_needed(const Acc *a, const char *sec) {
    cur_sec = sec;
    char h[256];
    int p;
    int r = is_sftp(a) || !tls_endpoint(a, h, sizeof h, &p);
    cur_sec = NULL;
    return r;
}

static char *question(const Acc *a, const char *kind, const char *host, int port, const char *fp, const char *subject, const char *issuer) {
    char st[64];
    cl_pf(st, sizeof st, "%s:lordos-trust", a->id);
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"question\":{\"kind\":");
    jb_str(&b, kind);
    jb_raw(&b, ",\"state\":");
    jb_str(&b, st);
    jb_raw(&b, ",\"name\":");
    jb_str(&b, kind);
    jb_raw(&b, ",\"host\":");
    jb_str(&b, host);
    jb_printf(&b, ",\"port\":%d,\"fingerprint\":", port);
    jb_str(&b, fp);
    if (subject) { jb_raw(&b, ",\"subject\":"); jb_str(&b, subject); }
    if (issuer) { jb_raw(&b, ",\"issuer\":"); jb_str(&b, issuer); }
    jb_raw(&b, ",\"type\":\"bool\",\"help\":\"\",\"examples\":[{\"value\":\"trust\",\"help\":\"\"},{\"value\":\"refuse\",\"help\":\"\"}],"
               "\"isPassword\":false,\"required\":true,\"exclusive\":true,\"default\":\"\"}}");
    return b.oom ? (jb_free(&b), NULL) : b.p;
}

static void pend(Acc *a, char kind, const char *data) {
    pthread_mutex_lock(&cl_mu);
    free(a->trust_data);
    a->trust_kind = kind;
    a->trust_data = data ? strdup(data) : NULL;
    pthread_mutex_unlock(&cl_mu);
}

/* fingerprints of known_hosts lines: "ED25519 SHA256:… · RSA SHA256:…" */
static int prints(const char *lines, char *out, size_t n) {
    char *kg[] = { KEYGEN, "-lf", "-", NULL };
    char fp[4096] = "";
    if (run(kg, lines, 0, fp, sizeof fp) <= 0) return -1;
    return rconnect_prints(fp, out, n);
}

static int check_ssh(Acc *a, char **q, char *code, size_t cn, char *msg, size_t mn) {
    const char *host = param(a, "host"), *ps = param(a, "port");
    int port = ps ? atoi(ps) : 22;
    if (!host || !rconnect_host_ok(host) || port <= 0 || port > 65535) { cl_pf(code, cn, "cloud.badAddress"); return -1; }
    if (access(KEYSCAN, X_OK) || access(KEYGEN, X_OK)) { cl_pf(code, cn, "cloud.addFailed"); cl_pf(msg, mn, "openssh-client is missing"); return -1; }
    char tag[300], pt[8], kh[PATH_MAX];
    rconnect_host_tag(host, port, tag, sizeof tag);
    cl_pf(pt, sizeof pt, "%d", port);
    char *scan[] = { KEYSCAN, "-T", "5", "-p", pt, "-t", "ed25519,ecdsa,rsa", "--", (char *)host, NULL };
    char *out = malloc(32768), *good = malloc(32768);
    if (!out || !good) { free(out); free(good); cl_pf(code, cn, "cloud.addFailed"); return -1; }
    int n = run(scan, NULL, 1, out, 32768);
    path_of(a, 1, kh, sizeof kh);
    char *file = slurp(kh);
    int have = 0, match = 0, changed = 0;
    size_t gl = 0;
    good[0] = 0;
    for (char *line = n > 0 ? strtok(out, "\n") : NULL; line; line = strtok(NULL, "\n")) {
        if (line[0] == '#' || !rconnect_keyline_ok(line, tag)) continue;
        have = 1;
        int k = file ? rconnect_known(file, tag, line) : -1;
        match |= k == 1;
        changed |= k == 0;
        gl += (size_t)cl_pf(good + gl, 32768 - gl, "%s\n", line);
        if (gl >= 32768) { gl = 32767; break; }
    }
    int r = 0;
    char fp[1024] = "";
    if (!have) { cl_pf(code, cn, "cloud.unreachable"); cl_pf(msg, mn, "no SSH host key from %s port %d", host, port); r = -1; }
    else if (match) r = 0;
    else if (changed) {
        /* both fingerprints, the kept ones and the new */
        char old[32768];
        size_t ol = 0;
        for (const char *p = file; p && *p;) {
            const char *e = strchr(p, '\n');
            size_t l = e ? (size_t)(e - p) : strlen(p);
            if (l > strlen(tag) && !strncmp(p, tag, strlen(tag)) && p[strlen(tag)] == ' ' && ol + l + 2 < sizeof old) { memcpy(old + ol, p, l); ol += l; old[ol++] = '\n'; }
            p = e ? e + 1 : NULL;
        }
        old[ol] = 0;
        char ofp[1024] = "";
        prints(old, ofp, sizeof ofp);
        prints(good, fp, sizeof fp);
        cl_pf(code, cn, "cloud.hostKeyChanged");
        cl_pf(msg, mn, "%s\t%s", ofp, fp);
        r = -1;
    } else if (prints(good, fp, sizeof fp)) { cl_pf(code, cn, "cloud.addFailed"); cl_pf(msg, mn, "ssh-keygen could not read the keys"); r = -1; }
    else { pend(a, 'h', good); *q = question(a, "hostKey", host, port, fp, NULL, NULL); r = *q ? 1 : -1; }
    free(file);
    free(out);
    free(good);
    return r;
}

static int check_tls(Acc *a, char **q, char *code, size_t cn, char *msg, size_t mn) {
    char host[256], conn[300], cf[PATH_MAX];
    int port;
    if (tls_endpoint(a, host, sizeof host, &port)) return 0;
    if (access(OPENSSL, X_OK)) { cl_pf(code, cn, "cloud.addFailed"); cl_pf(msg, mn, "openssl is missing"); return -1; }
    cl_pf(conn, sizeof conn, "%s:%d", host, port);
    char *sc[] = { OPENSSL, "s_client", "-connect", conn, "-servername", host, "-verify_hostname", host, NULL };
    char *out = malloc(65536), pem[16384];
    if (!out) { cl_pf(code, cn, "cloud.addFailed"); return -1; }
    int n = run(sc, NULL, 1, out, 65536);
    int v = n > 0 ? rconnect_tls_parse(out, pem, sizeof pem) : -1;
    free(out);
    if (v < 0) { cl_pf(code, cn, "cloud.unreachable"); cl_pf(msg, mn, "no TLS answer from %s", conn); return -1; }
    path_of(a, 0, cf, sizeof cf);
    char *pin = slurp(cf);
    int how = rconnect_tls_decide(pin, pem, v);
    char fp[256] = "", who[1024] = "";
    char *x[] = { OPENSSL, "x509", "-noout", "-fingerprint", "-sha256", NULL };
    if (how == RC_TLS_CHANGED || how == RC_TLS_ASK) { run(x, pem, 0, fp, sizeof fp); fp[strcspn(fp, "\n")] = 0; }
    int r = 0;
    if (how == RC_TLS_PIN || how == RC_TLS_SYSTEM) r = 0;
    else if (how == RC_TLS_CHANGED) {
        char ofp[256] = "";
        run(x, pin, 0, ofp, sizeof ofp);
        ofp[strcspn(ofp, "\n")] = 0;
        cl_pf(code, cn, "cloud.certChanged");
        cl_pf(msg, mn, "%s\t%s", ofp, fp);
        r = -1;
    } else {
        /* the certificate must name this host, as rclone checks with any trust */
        struct in_addr ia;
        struct in6_addr i6;
        int isip = inet_pton(AF_INET, host, &ia) == 1 || inet_pton(AF_INET6, host, &i6) == 1;
        char *chk[] = { OPENSSL, "x509", "-noout", isip ? "-checkip" : "-checkhost", host, NULL };
        char said[400] = "";
        if (run(chk, pem, 0, said, sizeof said) <= 0 || !rconnect_name_ok(said)) {
            cl_pf(code, cn, "cloud.certUntrusted");
            cl_pf(msg, mn, "the certificate of %s doesn't name %s", conn, host);
            r = -1;
        } else {
            char *si[] = { OPENSSL, "x509", "-noout", "-subject", "-issuer", NULL };
            run(si, pem, 0, who, sizeof who);
            char subject[400] = "", issuer[400] = "";
            for (char *l = strtok(who, "\n"); l; l = strtok(NULL, "\n")) {
                if (!strncmp(l, "subject=", 8)) cl_pf(subject, sizeof subject, "%s", l + 8);
                else if (!strncmp(l, "issuer=", 7)) cl_pf(issuer, sizeof issuer, "%s", l + 7);
            }
            pend(a, 'c', pem);
            *q = question(a, "certificate", host, port, fp, subject, issuer);
            r = *q ? 1 : -1;
        }
    }
    free(pin);
    return r;
}

int ct_check(Acc *a, const char *sec, char **q, char *code, size_t cn, char *msg, size_t mn) {
    *q = NULL;
    code[0] = msg[0] = 0;
    cur_sec = sec;                  /* the thread only */
    int r = is_sftp(a) ? check_ssh(a, q, code, cn, msg, mn) : 0;
    if (!r) r = check_tls(a, q, code, cn, msg, mn);
    cur_sec = NULL;
    explicit_bzero(sec_val, sizeof sec_val);
    return r;
}

/* the person accepted what was shown: kept, 0600 in a 0700 folder */
int ct_accept(Acc *a) {
    pthread_mutex_lock(&cl_mu);
    char kind = a->trust_kind, *data = a->trust_data;
    a->trust_kind = 0;
    a->trust_data = NULL;
    pthread_mutex_unlock(&cl_mu);
    if (!kind || !data) { free(data); return -1; }
    char p[PATH_MAX], dir[PATH_MAX];
    path_of(a, kind == 'h', p, sizeof p);
    cl_pf(dir, sizeof dir, "%s/%s", cl_cfg, kind == 'h' ? "known_hosts" : "certs");
    mkdir(dir, 0700);
    int fd = open(p, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW | (kind == 'h' ? O_APPEND : O_TRUNC), 0600);
    size_t l = strlen(data);
    int ok = fd >= 0 && write(fd, data, l) == (ssize_t)l;
    if (fd >= 0) close(fd);
    free(data);
    return ok ? 0 : -1;
}

void ct_refuse(Acc *a) { pend(a, 0, NULL); }

/* what rclone gets for this account: its own known_hosts (always, for SFTP) and its accepted certificate */
void ct_params(const Acc *a, JBuf *b, int *first) {
    char p[PATH_MAX];
    if (is_sftp(a)) {
        path_of(a, 1, p, sizeof p);
        jb_raw(b, *first ? "" : ",");
        jb_raw(b, "\"known_hosts_file\":");
        jb_str(b, p);
        *first = 0;
    }
    path_of(a, 0, p, sizeof p);
    if (!access(p, R_OK)) {
        jb_raw(b, *first ? "" : ",");
        jb_raw(b, "\"override.ca_cert\":");
        jb_str(b, p);
        *first = 0;
    }
}

void ct_forget(const Acc *a) {
    char p[PATH_MAX];
    path_of(a, 1, p, sizeof p);
    unlink(p);
    path_of(a, 0, p, sizeof p);
    unlink(p);
}
