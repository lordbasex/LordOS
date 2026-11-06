/* cloud_kr.c — Cloud's secrets in the session keyring (docs/CLOUD.md 4.3): one item per account, owner "cloud",
 * service "cloud:<id>", the secret a JSON object of rclone options. lord-server's cloud thread asks lord-vault
 * itself on the system channel (vc_request_wait): the reply never reaches JS. */
#include "cloud_kr.h"
#include "../sec/json.h"
#include "../vault/vclient.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KR_WAIT_MS 10000

/* -2 locked, -3 no keyring, 1 not found, -1 anything else */
static int code_of(const char *rep, size_t len) {
    JDoc d;
    int r = -1;
    if (!json_parse(rep, len, &d, NULL, 0)) {
        const char *e = json_str(d.root, "error");
        r = !e ? -1 : !strcmp(e, "locked") ? -2 : !strcmp(e, "no-keyring") ? -3 : !strcmp(e, "not-found") ? 1 : -1;
        json_free(&d);
    }
    return r;
}

static int ask(const char *op, const char *id, const char *label, const char *secret, char **out) {
    size_t n = 256 + (label ? strlen(label) * 6 : 0) + (secret ? strlen(secret) * 6 : 0);
    char *j = malloc(n), *ql = NULL, *qs = NULL;
    if (!j) return -1;
    if (label) { ql = malloc(strlen(label) * 6 + 8); if (ql) json_quote(ql, strlen(label) * 6 + 8, label); }
    if (secret) { qs = malloc(strlen(secret) * 6 + 8); if (qs) json_quote(qs, strlen(secret) * 6 + 8, secret); }
    if ((label && !ql) || (secret && !qs)) { free(j); free(ql); if (qs) { explicit_bzero(qs, strlen(qs)); free(qs); } return -1; }
    int len = snprintf(j, n, "{\"op\":\"%s\",\"owner\":\"cloud\",\"service\":\"cloud:%s\",\"account\":\"\"%s%s%s%s}", op, id,
                       ql ? ",\"label\":" : "", ql ? ql : "", qs ? ",\"secret\":" : "", qs ? qs : "");
    free(ql);
    if (qs) { explicit_bzero(qs, strlen(qs)); free(qs); }
    char *rep = NULL;
    size_t rl = 0;
    int r = len > 0 && (size_t)len < n ? vc_request_wait(j, (size_t)len, &rep, &rl, KR_WAIT_MS) : -1;
    explicit_bzero(j, n);
    free(j);
    if (r || !rep) return -1;
    if (!strncmp(rep, "{\"ok\":true", 10)) {
        r = 0;
        if (out) {
            JDoc d;
            *out = NULL;
            if (!json_parse(rep, rl, &d, NULL, 0)) {
                const char *s = json_str(json_get(d.root, "item"), "secret");
                if (s) *out = strdup(s);
                json_free(&d);
            }
            if (!*out) r = -1;
        }
    } else r = code_of(rep, rl);
    explicit_bzero(rep, rl);
    free(rep);
    return r;
}

static int kr_get(const char *id, char **secret) { *secret = NULL; return ask("kr-get", id, NULL, NULL, secret); }
static int kr_set(const char *id, const char *label, const char *secret) { return ask("kr-set", id, label, secret, NULL); }
static int kr_del(const char *id) { int r = ask("kr-del", id, NULL, NULL, NULL); return r == 1 ? 0 : r; }

const CloudSecrets cloud_keyring = { kr_get, kr_set, kr_del };
