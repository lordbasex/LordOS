/* sshtun.c — see sshtun.h. */
#include "sshtun.h"
#include "../pkg/crypto.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *const TYPES[] = {
    "ssh-ed25519", "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384", "ecdsa-sha2-nistp521", "rsa-sha2-512", "rsa-sha2-256", "ssh-rsa",
    "sk-ssh-ed25519@openssh.com", "sk-ecdsa-sha2-nistp256@openssh.com", NULL,
};
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64v(char c) { const char *p = c ? strchr(B64, c) : NULL; return p ? (int)(p - B64) : -1; }

/* strict base64 (with = padding at the end only) → bytes; -1 if it isn't */
static long b64_dec(const char *in, size_t n, uint8_t *out, size_t cap) {
    if (!n || n % 4) return -1;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 4) {
        int v[4];
        for (int k = 0; k < 4; k++) {
            char c = in[i + k];
            if (c == '=' && i + 4 == n && k >= 2 && (k == 3 || in[i + 3] == '=')) v[k] = -2;
            else if ((v[k] = b64v(c)) < 0) return -1;
        }
        if (v[0] < 0 || v[1] < 0) return -1;
        uint32_t x = (uint32_t)v[0] << 18 | (uint32_t)v[1] << 12 | (uint32_t)(v[2] < 0 ? 0 : v[2]) << 6 | (uint32_t)(v[3] < 0 ? 0 : v[3]);
        int bytes = v[2] == -2 ? 1 : v[3] == -2 ? 2 : 3;
        if (o + (size_t)bytes > cap) return -1;
        out[o++] = (uint8_t)(x >> 16);
        if (bytes > 1) out[o++] = (uint8_t)(x >> 8);
        if (bytes > 2) out[o++] = (uint8_t)x;
    }
    return (long)o;
}

/* the key's type and blob: -1 unless "type base64" with a known type that the blob repeats */
static long key_blob(const char *s, uint8_t *blob, size_t cap) {
    if (!s) return -1;
    size_t n = strlen(s);
    if (!n || n > SSH_KEY_MAX) return -1;
    const char *sp = strchr(s, ' ');
    if (!sp || strchr(sp + 1, ' ')) return -1;
    size_t tl = (size_t)(sp - s);
    int known = 0;
    for (int i = 0; TYPES[i]; i++) if (strlen(TYPES[i]) == tl && !memcmp(TYPES[i], s, tl)) known = 1;
    if (!known) return -1;
    long bl = b64_dec(sp + 1, strlen(sp + 1), blob, cap);
    if (bl < 4) return -1;
    uint32_t inner = (uint32_t)blob[0] << 24 | (uint32_t)blob[1] << 16 | (uint32_t)blob[2] << 8 | blob[3];
    /* the blob starts with its own type: the same name, except rsa-sha2-* whose keys are "ssh-rsa" */
    const char *want = !strncmp(s, "rsa-sha2-", 9) ? "ssh-rsa" : NULL;
    size_t wl = want ? strlen(want) : tl;
    if (inner != wl || (size_t)bl < 4 + wl || memcmp(blob + 4, want ? want : s, wl)) return -1;
    return bl;
}

int ssh_hostkey_ok(const char *s) {
    uint8_t blob[SSH_KEY_MAX];
    return key_blob(s, blob, sizeof blob) > 0;
}

int ssh_fingerprint(const char *key, char *out, size_t cap) {
    uint8_t blob[SSH_KEY_MAX], h[32];
    long bl = key_blob(key, blob, sizeof blob);
    if (bl <= 0 || cap < 7 + 44) return -1;
    sha256(blob, (size_t)bl, h);
    size_t o = (size_t)snprintf(out, cap, "SHA256:");
    for (int i = 0; i < 32; i += 3) {
        uint32_t x = (uint32_t)h[i] << 16 | (uint32_t)(i + 1 < 32 ? h[i + 1] : 0) << 8 | (uint32_t)(i + 2 < 32 ? h[i + 2] : 0);
        int chars = i + 2 < 32 ? 4 : i + 1 < 32 ? 3 : 2;
        for (int k = 0; k < chars; k++) out[o++] = B64[(x >> (18 - 6 * k)) & 63];
    }
    out[o] = 0;
    return 0;
}

/* a host as ssh and known_hosts take it: letters, digits, . - : _ (IPv6 too), never starting with - */
static int host_ok(const char *h) {
    if (!h || !h[0] || h[0] == '-' || strlen(h) > 253) return 0;
    for (const char *p = h; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == ':' || c == '_')) return 0;
    }
    return 1;
}
static int user_ok(const char *u) {
    if (!u || !u[0] || u[0] == '-' || strlen(u) > 64) return 0;
    for (const char *p = u; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')) return 0;
    }
    return 1;
}
/* a path ssh gets in -o Option=path: absolute, no spaces, quotes, % (ssh expands %-tokens), or control characters */
static int path_ok(const char *p) {
    if (!p || p[0] != '/' || strlen(p) > 1000) return 0;
    for (const char *q = p; *q; q++) if ((unsigned char)*q < 0x21 || *q == 0x7f || *q == '"' || *q == '\'' || *q == '%' || *q == '\\') return 0;
    return 1;
}

int ssh_known_line(const char *host, int port, const char *key, char *out, size_t cap) {
    if (!host_ok(host) || !ssh_hostkey_ok(key) || port < 1 || port > 65535) return -1;
    int n = port == 22 ? snprintf(out, cap, "%s %s\n", host, key) : snprintf(out, cap, "[%s]:%d %s\n", host, port, key);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

int ssh_key_of_line(const char *line, char *out, size_t cap) {
    if (!line) return -1;
    const char *sp = strchr(line, ' ');
    if (!sp) return -1;
    size_t n = strcspn(sp + 1, "\r\n");
    if (n >= cap) return -1;
    memcpy(out, sp + 1, n);
    out[n] = 0;
    return ssh_hostkey_ok(out) ? 0 : -1;
}

int ssh_argv(const SshTun *t, char *buf, size_t cap, char **argv, int max) {
    if (!t || !host_ok(t->ssh_host) || !user_ok(t->ssh_user) || t->ssh_port < 1 || t->ssh_port > 65535 ||
        !path_ok(t->known_hosts) || (!t->probe && (!host_ok(t->db_host) || t->db_port < 1 || t->db_port > 65535 || !path_ok(t->key_path))))
        return -1;
    size_t o = 0;
    int n = 0;
    int bad = 0;
#define ARG(...) do { \
        int w_ = snprintf(buf + o, cap - o, __VA_ARGS__); \
        if (w_ < 0 || (size_t)w_ >= cap - o || n >= max - 1) { bad = 1; break; } \
        argv[n++] = buf + o; o += (size_t)w_ + 1; \
    } while (0)
    do {
        ARG("ssh");
        ARG("-F"); ARG("none");                                     /* never the user's config */
        ARG("-o"); ARG("BatchMode=yes");
        ARG("-o"); ARG("UserKnownHostsFile=%s", t->known_hosts);
        ARG("-o"); ARG("GlobalKnownHostsFile=none");
        ARG("-o"); ARG("StrictHostKeyChecking=%s", t->probe ? "accept-new" : "yes");
        ARG("-o"); ARG("UpdateHostKeys=no");
        ARG("-o"); ARG("HashKnownHosts=no");
        ARG("-o"); ARG("CheckHostIP=no");
        ARG("-o"); ARG("ProxyCommand=none");
        ARG("-o"); ARG("ProxyJump=none");
        ARG("-o"); ARG("ControlMaster=no");
        ARG("-o"); ARG("ControlPath=none");
        ARG("-o"); ARG("PermitLocalCommand=no");
        ARG("-o"); ARG("ClearAllForwardings=yes");
        ARG("-o"); ARG("ForwardAgent=no");
        ARG("-o"); ARG("ForwardX11=no");
        ARG("-o"); ARG("ConnectTimeout=15");
        ARG("-o"); ARG("ServerAliveInterval=30");
        ARG("-o"); ARG("ServerAliveCountMax=3");
        ARG("-o"); ARG("LogLevel=ERROR");
        if (t->probe) {
            /* stops after the server's key is known: no key is offered, nothing is forwarded */
            ARG("-o"); ARG("PubkeyAuthentication=no");
            ARG("-o"); ARG("PasswordAuthentication=no");
            ARG("-o"); ARG("KbdInteractiveAuthentication=no");
            ARG("-o"); ARG("HostbasedAuthentication=no");
            ARG("-o"); ARG("GSSAPIAuthentication=no");
            ARG("-o"); ARG("IdentityAgent=none");
            ARG("-o"); ARG("NumberOfPasswordPrompts=0");
            ARG("-N");
        } else {
            ARG("-o"); ARG("IdentitiesOnly=yes");
            ARG("-o"); ARG("IdentityFile=%s", t->key_path);
            ARG("-o"); ARG("PreferredAuthentications=publickey");
            ARG("-o"); ARG("ExitOnForwardFailure=yes");
            ARG("-W"); ARG(strchr(t->db_host, ':') ? "[%s]:%d" : "%s:%d", t->db_host, t->db_port);
        }
        ARG("-p"); ARG("%d", t->ssh_port);
        ARG("-l"); ARG("%s", t->ssh_user);
        ARG("--"); ARG("%s", t->ssh_host);
    } while (0);
#undef ARG
    if (bad) return -1;
    argv[n] = NULL;
    return n;
}

const char *ssh_err_code(const char *e) {
    if (!e) return "ssh";
    if (strstr(e, "REMOTE HOST IDENTIFICATION HAS CHANGED") || strstr(e, "Host key verification failed") || strstr(e, "host key for")) return "hostkey";
    if (strstr(e, "Permission denied")) return "sshauth";
    if (strstr(e, "Could not resolve hostname") || strstr(e, "Name or service not known")) return "dns";
    if (strstr(e, "channel") && (strstr(e, "open failed") || strstr(e, "administratively prohibited"))) return "sshforward";
    if (strstr(e, "stdio forwarding failed")) return "sshforward";
    if (strstr(e, "Load key") || strstr(e, "bad permissions") || strstr(e, "UNPROTECTED PRIVATE KEY") || strstr(e, "no such identity") || strstr(e, "invalid format")) return "sshkey";
    if (strstr(e, "Connection refused") || strstr(e, "timed out") || strstr(e, "No route to host") || strstr(e, "Network is unreachable") || strstr(e, "Connection closed")) return "connect";
    return "ssh";
}
