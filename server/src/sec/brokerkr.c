/* brokerkr.c — the session keyring for isolated .lpk apps (BOP_KEYRING, see broker.h and
 * docs/PASSWORDS.md, "Keyring for .lpk apps").
 *
 * The app sends "op\0service\0account\0label\0secret"; axia-server builds the axia-vault request
 * itself, with owner = "app.<id>", where <id> is the app id axia-server verified when it spawned
 * the process (brokersrv_add). The app never chooses the owner, and the "app." prefix keeps
 * isolated apps apart from the owners the desktop's own apps use ("mail", "settings"…).
 *
 * The request goes to axia-vault on axia-server's system channel as a private request
 * (vc_request_private): the reply never reaches the desktop's JS. axia-vault only lets an owner
 * see its own items; when an app asks for an item that belongs to another app (axia-vault answers
 * "not-shared"), axia-server shows the system consent prompt (privacy.js, "keyring") and, only if
 * the user allows it, records the share (kr-share) and asks again. Nothing here blocks: replies
 * and the user's answer arrive later (brokerkr_tick, brokerkr_resolve). Buffers that may hold
 * a secret are wiped before they are freed. */
#include "broker_int.h"
#include "json.h"
#include "../vault/vclient.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KR_MAX       32
#define KR_FIELD_MAX 255             /* service, account, label (bytes) */
#define KR_SECRET_MAX (64 * 1024)
#define KR_PROMPT_TAG 0xC0000000u    /* prompt ids: never a small app reqid */

enum { ST_FREE, ST_REQ, ST_PROMPT, ST_SHARE, ST_RETRY };

typedef struct {
    int st, conn, fd, dead;
    uint32_t reqid;                  /* the app's request */
    int vid;                         /* axia-vault request id */
    uint32_t prompt;
    char op;                         /* 'g' get, 's' set, 'd' delete, 'l' list */
    char owner[64], item[40];
    char service[KR_FIELD_MAX + 1], account[KR_FIELD_MAX + 1];
} KrReq;

static KrReq rq[KR_MAX];
static uint32_t prompt_seq;

/* Items the user refused to share with an app, for the rest of this session: the app cannot keep
 * the prompt on screen by asking again in a loop. */
#define KR_DENY_MAX 64
static struct { char owner[64], item[40]; } denied[KR_DENY_MAX];
static int ndenied;

static int was_denied(const char *owner, const char *item) {
    for (int i = 0; i < ndenied; i++) if (!strcmp(denied[i].owner, owner) && !strcmp(denied[i].item, item)) return 1;
    return 0;
}
static void remember_denied(const char *owner, const char *item) {
    if (was_denied(owner, item)) return;
    int i = ndenied < KR_DENY_MAX ? ndenied++ : (int)(prompt_seq % KR_DENY_MAX);   /* full: overwrite one */
    snprintf(denied[i].owner, sizeof denied[i].owner, "%s", owner);
    snprintf(denied[i].item, sizeof denied[i].item, "%s", item);
}

/* axia-vault's error code → the code line the app gets (assets/locales/<lang>/errors.json) */
static const char *map_code(const char *v) {
    if (!v) return "keyring.failed";
    if (!strcmp(v, "locked")) return "keyring.locked";
    if (!strcmp(v, "no-keyring")) return "keyring.noKeyring";
    if (!strcmp(v, "not-found")) return "keyring.notFound";
    if (!strcmp(v, "not-shared")) return "keyring.notShared";
    if (!strcmp(v, "bad-request") || !strcmp(v, "bad-owner") || !strcmp(v, "bad-json")) return "keyring.badRequest";
    if (!strcmp(v, "save")) return "keyring.saveFailed";
    if (!strcmp(v, "crash")) return "vault.unavailable";
    return "keyring.failed";
}

static void reply_code(KrReq *r, int status, const char *code) {
    if (!r->dead) brokersrv_reply(r->fd, r->reqid, status, code, (uint32_t)strlen(code), -1);
}

static void finish(KrReq *r) {
    explicit_bzero(r, sizeof *r);
    r->st = ST_FREE;
}

/* the JSON text of s, in a malloc'd buffer big enough for any escaping */
static char *quote(const char *s) {
    size_t n = strlen(s) * 6 + 8;
    char *q = malloc(n);
    if (q) json_quote(q, n, s);
    return q;
}

static void free_wiped(char *s) { if (s) { explicit_bzero(s, strlen(s)); free(s); } }

/* Builds and sends a kr-* request; 0 if sent (r->vid set). `extra` is appended as is (trusted). */
static int send_vault(KrReq *r, const char *op, const char *label, const char *secret, const char *extra) {
    char *qo = quote(r->owner), *qs = quote(r->service), *qa = quote(r->account);
    char *ql = quote(label ? label : ""), *qx = quote(secret ? secret : "");
    int rc = -1;
    if (qo && qs && qa && ql && qx) {
        size_t n = strlen(qo) + strlen(qs) + strlen(qa) + strlen(ql) + strlen(qx) + (extra ? strlen(extra) : 0) + 128;
        char *j = malloc(n);
        if (j) {
            int len = snprintf(j, n, "{\"op\":\"%s\",\"owner\":%s,\"service\":%s,\"account\":%s%s%s%s%s%s}", op, qo, qs, qa,
                               secret ? ",\"label\":" : "", secret ? ql : "", secret ? ",\"secret\":" : "", secret ? qx : "",
                               extra ? extra : "");
            if (len > 0 && (size_t)len < n) { r->vid = vc_request_private(j, (size_t)len); rc = r->vid > 0 ? 0 : -1; }
            explicit_bzero(j, n);
            free(j);
        }
    }
    free_wiped(qo); free_wiped(qs); free_wiped(qa); free_wiped(ql); free_wiped(qx);
    return rc;
}

/* The kr-share axia-server sends after the user allowed it (the only "admin" request built here). */
static int send_share(KrReq *r) {
    char *qi = quote(r->item), *qo = quote(r->owner);
    int rc = -1;
    if (qi && qo) {
        char j[256];
        int len = snprintf(j, sizeof j, "{\"op\":\"kr-share\",\"admin\":true,\"id\":%s,\"app\":%s,\"allow\":true}", qi, qo);
        if (len > 0 && (size_t)len < sizeof j) { r->vid = vc_request_private(j, (size_t)len); rc = r->vid > 0 ? 0 : -1; }
    }
    free(qi); free(qo);
    return rc;
}

/* Field k of the payload (NUL-separated, inside len bytes; payload[len] is 0): NULL if missing
 * or longer than max. Trailing fields may be left out (they read as ""). */
static const char *field(const char *p, uint32_t len, int k, size_t max) {
    uint32_t at = 0;
    for (int i = 0; i < k && at < len; i++) {
        const char *z = memchr(p + at, 0, len - at);
        at = z ? (uint32_t)(z - p) + 1 : len;
    }
    return strlen(p + at) > max ? NULL : p + at;
}

void brokerkr_handle(int conn, int fd, uint32_t reqid, const char *app, char *payload, uint32_t len) {
    KrReq *r = NULL;
    for (int i = 0; i < KR_MAX && !r; i++) if (rq[i].st == ST_FREE) r = &rq[i];
    const char *op = field(payload, len, 0, 8), *service = field(payload, len, 1, KR_FIELD_MAX);
    const char *account = field(payload, len, 2, KR_FIELD_MAX), *label = field(payload, len, 3, KR_FIELD_MAX);
    const char *secret = field(payload, len, 4, KR_SECRET_MAX);
    /* the secret is the last field and runs to the end: a NUL inside it is not accepted */
    if (secret && (size_t)(secret - payload) + strlen(secret) != len) secret = NULL;
    char o = !op ? 0 : !strcmp(op, "get") ? 'g' : !strcmp(op, "set") ? 's' : !strcmp(op, "delete") ? 'd' : !strcmp(op, "list") ? 'l' : 0;
    const char *bad = NULL;
    if (!app || !app[0] || strlen(app) > 40) bad = "keyring.badRequest";
    else if (!o || !service || !account || !label || !secret) bad = "keyring.badRequest";
    else if (o != 'l' && !service[0]) bad = "keyring.badRequest";
    else if (!r) bad = "keyring.busy";
    if (bad) {
        brokersrv_reply(fd, reqid, BST_ERROR, bad, (uint32_t)strlen(bad), -1);
        explicit_bzero(payload, len);
        return;
    }
    *r = (KrReq){ .st = ST_REQ, .conn = conn, .fd = fd, .reqid = reqid, .op = o };
    /* a system app (assets/sysapps) keeps the owner it had in the session (Mail's account passwords
     * stay "mail", where the session's need-auth handler reads them); a user package is "app.<id>" */
    snprintf(r->owner, sizeof r->owner, brokersrv_conn_system(conn) ? "%s" : "app.%s", app);
    snprintf(r->service, sizeof r->service, "%s", service);
    snprintf(r->account, sizeof r->account, "%s", account);
    int sent = o == 's' ? send_vault(r, "kr-set", label, secret, NULL)
             : send_vault(r, o == 'g' ? "kr-get" : o == 'd' ? "kr-del" : "kr-list", NULL, NULL, NULL);
    explicit_bzero(payload, len);
    if (sent) { reply_code(r, BST_ERROR, "vault.unavailable"); finish(r); }
}

/* axia-vault answered request r: `rep` is its JSON ({"ok":true,…} | {"error":…}) */
static void on_reply(KrReq *r, char *rep, size_t len) {
    if (r->dead) { finish(r); return; }
    if (!strncmp(rep, "{\"ok\":true", 10)) {
        if (r->st == ST_SHARE) {                           /* shared: ask again, now it can see it */
            r->st = ST_RETRY;
            if (send_vault(r, "kr-get", NULL, NULL, NULL)) { reply_code(r, BST_ERROR, "vault.unavailable"); finish(r); }
            return;
        }
        brokersrv_reply(r->fd, r->reqid, BST_OK, rep, (uint32_t)len, -1);   /* item(s) as axia-vault sent them */
        finish(r);
        return;
    }
    /* an error reply has no secret in it */
    JDoc d;
    char code[32] = "", item[40] = "", other[72] = "";
    if (!json_parse(rep, len, &d, NULL, 0)) {
        const char *e = json_str(d.root, "error"), *it = json_str(d.root, "item"), *w = json_str(d.root, "itemOwner");
        if (e) snprintf(code, sizeof code, "%s", e);
        if (it) snprintf(item, sizeof item, "%s", it);
        if (w) snprintf(other, sizeof other, "%s", w);
        json_free(&d);
    }
    if (!strcmp(code, "not-shared") && r->op == 'g' && r->st == ST_REQ && item[0] && !was_denied(r->owner, item)) {
        /* another app's item: the system asks the user (the app never draws this). The other
         * app is shown by its plain id ("app.notes" → "notes"). */
        char qa[64], qw[96], ev[256];          /* ids are short: [a-z0-9._-], at most 71 bytes */
        json_quote(qw, sizeof qw, !strncmp(other, "app.", 4) ? other + 4 : other);
        json_quote(qa, sizeof qa, !strncmp(r->owner, "app.", 4) ? r->owner + 4 : r->owner);
        snprintf(r->item, sizeof r->item, "%s", item);
        r->prompt = KR_PROMPT_TAG | (++prompt_seq & 0x3fffffff);
        r->st = ST_PROMPT;
        snprintf(ev, sizeof ev, "{\"reqid\":%u,\"app\":%s,\"perm\":\"keyring\",\"owner\":%s}", r->prompt, qa, qw);
        brokersrv_prompt_event(ev);
        return;
    }
    /* another app's item can only be read, after consent: for set/delete/list it does not exist */
    if (!strcmp(code, "not-shared") && r->op != 'g') snprintf(code, sizeof code, "not-found");
    reply_code(r, !strcmp(code, "not-shared") ? BST_DENIED : BST_ERROR, map_code(code[0] ? code : NULL));
    finish(r);
}

void brokerkr_tick(void) {
    uint32_t id;
    size_t len;
    char *rep;
    while ((rep = vc_take_private(&id, &len))) {
        for (int i = 0; i < KR_MAX; i++)
            if ((rq[i].st == ST_REQ || rq[i].st == ST_SHARE || rq[i].st == ST_RETRY) && rq[i].vid == (int)id) { on_reply(&rq[i], rep, len); break; }
        explicit_bzero(rep, len);
        free(rep);
    }
}

int brokerkr_resolve(uint32_t prompt, int allow) {
    if ((prompt & KR_PROMPT_TAG) != KR_PROMPT_TAG) return 0;
    for (int i = 0; i < KR_MAX; i++) {
        KrReq *r = &rq[i];
        if (r->st != ST_PROMPT || r->prompt != prompt) continue;
        if (r->dead) { finish(r); return 1; }
        if (!allow) { remember_denied(r->owner, r->item); reply_code(r, BST_DENIED, "keyring.notShared"); finish(r); return 1; }
        r->st = ST_SHARE;
        if (send_share(r)) { reply_code(r, BST_ERROR, "vault.unavailable"); finish(r); }
        return 1;
    }
    return 1;                                              /* a keyring prompt whose app is gone */
}

void brokerkr_conn_closed(int conn) {
    for (int i = 0; i < KR_MAX; i++) {
        if (rq[i].st == ST_FREE || rq[i].conn != conn) continue;
        rq[i].dead = 1;                                    /* the vault reply is still drained */
        if (rq[i].st == ST_PROMPT) finish(&rq[i]);
    }
}

int brokerkr_pollfd(void) {
    for (int i = 0; i < KR_MAX; i++) if (rq[i].st == ST_REQ || rq[i].st == ST_SHARE || rq[i].st == ST_RETRY) return vc_private_fd();
    return -1;
}
