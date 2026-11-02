/* fetch.c — lord-fetch: downloads the subscribed blocklists over HTTPS for lord-server, sandboxed
 * (see fetchproto.h). With lord-sessiond and lord-maild, the only binaries that link OpenSSL. */
#include "fetchproto.h"
#include "../doc/wire.h"
#include "../fw/fwlists.h"
#include "../mail/http.h"
#include "../mail/net.h"
#include "../sec/json.h"
#include "../sec/sandbox.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#define SOCK 3

static void reply(uint32_t op, uint32_t id, JBuf *b, int fd) {
    doc_send(SOCK, op, id, b->p ? b->p : "{}", b->p ? b->n : 2, fd >= 0 ? &fd : NULL, fd >= 0 ? 1 : 0);
}

static void fail(uint32_t id, const char *code, const char *detail) {
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"error\":");
    jb_str(&b, code);
    jb_raw(&b, ",\"detail\":");
    jb_str(&b, detail ? detail : "");
    jb_raw(&b, "}");
    reply(FR_ERR, id, &b, -1);
    jb_free(&b);
}

/* host: a DNS name or an address, printable ASCII without spaces; path: starts with /, printable */
static int good_host(const char *h) {
    size_t n = h ? strlen(h) : 0;
    if (!n || n > 253) return 0;
    for (const char *p = h; *p; p++) if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == ':')) return 0;
    return 1;
}

static int good_path(const char *p) {
    size_t n = p ? strlen(p) : 0;
    if (!n || n > 1000 || p[0] != '/') return 0;
    for (; *p; p++) if ((unsigned char)*p <= 0x20 || (unsigned char)*p >= 0x7f) return 0;
    return 1;
}

static void get(uint32_t id, const char *json, size_t len, int fd) {
    JDoc d;
    if (json_parse(json, len, &d, NULL, 0)) { fail(id, "bad-request", ""); return; }
    const char *host = json_str(d.root, "host"), *path = json_str(d.root, "path"), *fmt = json_str(d.root, "format");
    int sub = json_num(d.root, "sub", 0) == 1;
    FwlFormat f = fwl_format_from_name(fmt);
    if (!good_host(host) || !good_path(path) || !f) { json_free(&d); fail(id, "bad-request", ""); return; }
    Conn *c = conn_new(fd, host, 30000);
    if (!c) { close(fd); json_free(&d); fail(id, "oom", ""); return; }
    TlsInfo info;
    memset(&info, 0, sizeof info);
    if (conn_tls(c, NULL, &info)) {
        fail(id, info.code ? "cert" : "tls", info.code ? info.reason : conn_error(c));
        conn_close(c);
        json_free(&d);
        return;
    }
    HttpResp r;
    memset(&r, 0, sizeof r);
    int rc = http_request(c, "GET", host, path, "Accept-Encoding: identity\r\nUser-Agent: LordOS-blocklists/1\r\n", NULL, 0,
                          FWL_RAW_MAX, 180000, &r);
    conn_close(c);
    json_free(&d);
    if (rc == -2) { fail(id, "big", ""); return; }
    if (rc) { fail(id, "http", ""); http_resp_free(&r); return; }
    if (r.status == 301 || r.status == 302 || r.status == 303 || r.status == 307 || r.status == 308) {
        JBuf b;
        jb_init(&b);
        jb_raw(&b, "{\"location\":");
        jb_str(&b, r.location);
        jb_raw(&b, "}");
        reply(FR_REDIRECT, id, &b, -1);
        jb_free(&b);
        http_resp_free(&r);
        return;
    }
    if (r.status != 200) {
        char s[16];
        snprintf(s, sizeof s, "%d", r.status);
        fail(id, "status", s);
        http_resp_free(&r);
        return;
    }
    char *out = NULL, err[32] = "";
    size_t olen = 0;
    FwlStats st;
    if (fwl_parse_raw(r.body ? r.body : "", r.blen, f, sub, &out, &olen, &st, err, sizeof err)) {
        fail(id, err, "");
        http_resp_free(&r);
        return;
    }
    size_t bytes = r.blen;
    http_resp_free(&r);
    int mfd = doc_memfd("lord-fetch-list", out, olen);
    free(out);
    if (mfd < 0) { fail(id, "oom", ""); return; }
    JBuf b;
    jb_init(&b);
    jb_printf(&b, "{\"domains\":%zu,\"nets\":%zu,\"skipped\":%zu,\"bad\":%zu,\"dups\":%zu,\"truncated\":%d,\"bytes\":%zu,\"lines\":%zu}",
              st.domains, st.nets, st.skipped, st.bad, st.dups, st.truncated, bytes, st.lines);
    reply(FR_DONE, id, &b, mfd);
    jb_free(&b);
    close(mfd);
}

int main(void) {
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    prctl(PR_SET_DUMPABLE, 0);
    struct rlimit nocore = { 0, 0 };
    setrlimit(RLIMIT_CORE, &nocore);
    signal(SIGPIPE, SIG_IGN);
    struct stat st;
    if (fstat(SOCK, &st) || !S_ISSOCK(st.st_mode)) { fprintf(stderr, "lord-fetch: started by lord-server only\n"); return 2; }
    if (net_init()) { fprintf(stderr, "lord-fetch: TLS init failed\n"); return 3; }
    SandboxHelperCfg cfg = {
        .name = "lordos-fetch",          /* cgroup scope lordos-fetch.scope (lord-sessiond creates it) */
        .ro_paths = NULL,
        .extra_syscalls = (int[]){ -1 },
        .allow_ioctl_classes = 0,
        .mem_max_mb = 256,
        .ro_dir_fds = NULL,
        .rw_dir_fds = NULL,
    };
    setpriority(PRIO_PROCESS, 0, 10);
    sandbox_apply_helper(&cfg);
    for (;;) {
        DocMsg m;
        int rc = doc_recv(SOCK, &m);
        if (rc) return rc == 1 ? 0 : 1;
        if (m.h.op == FQ_END) { doc_msg_free(&m); return 0; }
        if (m.h.op == FQ_GET && m.nfds == 1 && m.json) {
            int fd = m.fds[0];
            m.fds[0] = -1;
            m.nfds = 0;
            get(m.h.id, m.json, m.len, fd);     /* conn_close closes fd */
        } else fail(m.h.id, "bad-request", "");
        doc_msg_free(&m);
    }
}
