/* keyring.c — the system keyring (see docs/PASSWORDS.md, "The session keyring").
 *
 * It is a separate vault, login.lvault, a "login" keyring: it unlocks by itself at
 * login with the session password (lord-sessiond verifies it and hands it to the session
 * only once; axia-server forwards it here and wipes it), and stays open until logout.
 * Locking the screen does not lock it (the Passwords vault does lock).
 *
 * It stores two kinds of items (vault entries with an "x" that says whose they are):
 *   - from the LordOS keyring (lord.keyring / sdk keyring): x.kr = {owner, service, account, shared};
 *     an app only sees its own, unless the user shares them with it (shared);
 *   - from Secret Service (D-Bus, Linux apps): x.ss = {attrs, ctype}; as in gnome-keyring,
 *     any app of the session that speaks D-Bus can read them while the keyring is open. */
#include "vault_int.h"
#include "smem.h"
#include "secretsvc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LOGIN (&vaults[V_LOGIN])

static int valid_owner(const char *s) {
    if (!s || !s[0] || strlen(s) > 64) return 0;
    for (const char *p = s; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == '_')) return 0;
    return 1;
}

/* x.kr of an entry: owner/service/account (copied, bounded) and whether `who`'s request can see it */
typedef struct { char owner[72], service[256], account[256]; int shared_with; } KrInfo;

static int kr_info(const VEntry *e, const char *who, KrInfo *k) {
    memset(k, 0, sizeof *k);
    if (!e->x) return 0;
    Vj j;
    if (vj_parse(&j, e->x, strlen(e->x))) return 0;
    int kr = vj_get(&j, 0, "kr");
    int ok = vj_type(&j, kr) == VJ_OBJ;
    if (ok) {
        vj_getsbuf(&j, kr, "owner", k->owner, sizeof k->owner);
        vj_getsbuf(&j, kr, "service", k->service, sizeof k->service);
        vj_getsbuf(&j, kr, "account", k->account, sizeof k->account);
        int sh = vj_get(&j, kr, "shared");
        for (int i = 0, t = sh + 1; who && vj_type(&j, sh) == VJ_ARR && i < j.t[sh].count; i++, t = j.t[t].next)
            if (vj_streq(&j, t, who) || vj_streq(&j, t, "*")) k->shared_with = 1;
    }
    vj_free(&j);
    return ok;
}

static void kr_item_json(VjB *b, const VEntry *e, const KrInfo *k, int with_secret) {
    vb_sep(b);
    vb_open(b, '{');
    vb_ks(b, "id", e->id);
    vb_ks(b, "owner", k->owner);
    vb_ks(b, "service", k->service);
    vb_ks(b, "account", k->account);
    vb_ks(b, "label", e->title);
    vb_kn(b, "modified", (double)e->modified);
    if (with_secret) { const char *s = vm_field(e, "password"); vb_ks(b, "secret", s ? s : ""); }
    if (e->x) { Vj j; if (!vj_parse(&j, e->x, strlen(e->x))) { int kr = vj_get(&j, 0, "kr"); int sh = vj_get(&j, kr, "shared");
        if (vj_type(&j, sh) == VJ_ARR) { vb_key(b, "shared"); vb_rawn(b, j.s + j.t[sh].start, j.t[sh].end - j.t[sh].start); } vj_free(&j); } }
    vb_close(b, '}');
}

/* builds the JSON of a keyring entry and stores it with vm_upsert */
static int kr_store(const char *id, const char *owner, const char *service, const char *account, const char *label, const char *secret, const char *shared_json) {
    VjB b;
    vb_init(&b);
    vb_open(&b, '{');
    if (id) vb_ks(&b, "id", id);
    vb_ks(&b, "cat", "keyring");
    vb_ks(&b, "title", label && label[0] ? label : service);
    vb_key(&b, "f");
    vb_open(&b, '[');
    vb_sep(&b); vb_open(&b, '{'); vb_ks(&b, "k", "service"); vb_ks(&b, "t", "text"); vb_ks(&b, "v", service); vb_close(&b, '}');
    if (account && account[0]) { vb_sep(&b); vb_open(&b, '{'); vb_ks(&b, "k", "username"); vb_ks(&b, "t", "text"); vb_ks(&b, "v", account); vb_close(&b, '}'); }
    vb_sep(&b); vb_open(&b, '{'); vb_ks(&b, "k", "password"); vb_ks(&b, "t", "hidden"); vb_ks(&b, "v", secret ? secret : ""); vb_close(&b, '}');
    vb_close(&b, ']');
    vb_key(&b, "x");
    vb_open(&b, '{');
    vb_key(&b, "kr");
    vb_open(&b, '{');
    vb_ks(&b, "owner", owner);
    vb_ks(&b, "service", service);
    vb_ks(&b, "account", account ? account : "");
    vb_key(&b, "shared"); vb_raw(&b, shared_json ? shared_json : "[]");
    vb_close(&b, '}');
    vb_close(&b, '}');
    vb_close(&b, '}');
    Vj j;
    int r = -1;
    char err[64];
    if (!b.oom && !vj_parse(&j, b.p, b.n)) { r = vm_upsert(&LOGIN->m, &j, 0, NULL, err, sizeof err); vj_free(&j); }
    vb_free(&b);
    return r;
}

int keyring_handle(Chan *c, uint32_t id, const char *op, const Vj *j) {
    if (strncmp(op, "kr-", 3)) return 0;
    Vault *v = LOGIN;
    if (!v->unlocked) { reply_err(c, id, v->exists == 1 ? "locked" : "no-keyring"); return 1; }
    char owner[72] = "", service[256] = "", account[256] = "";
    vj_getsbuf(j, 0, "owner", owner, sizeof owner);
    vj_getsbuf(j, 0, "service", service, sizeof service);
    vj_getsbuf(j, 0, "account", account, sizeof account);
    /* the Passwords app manages the whole keyring; everything else goes through axia-server, which sets the owner */
    int admin = c->role == ROLE_APP || (c->role == ROLE_SYSTEM && vj_getb(j, 0, "admin", 0));
    if (!admin && !valid_owner(owner)) { reply_err(c, id, "bad-owner"); return 1; }
    if (!strcmp(op, "kr-set")) {
        if (!service[0] || admin) { reply_err(c, id, "bad-request"); return 1; }
        char *secret = vj_gets(j, 0, "secret"), *label = vj_gets(j, 0, "label");
        const char *eid = NULL;
        for (int i = 0; i < v->m.n; i++) {
            KrInfo k;
            if (kr_info(v->m.e[i], owner, &k) && !strcmp(k.owner, owner) && !strcmp(k.service, service) && !strcmp(k.account, account)) { eid = v->m.e[i]->id; break; }
        }
        int r = kr_store(eid, owner, service, account, label, secret, NULL);
        sm_free(secret); sm_free(label);
        if (r || vault_save(v) != VF_OK) reply_err(c, id, "save"); else reply_ok(c, id);
        broadcast_event("{\"ev\":\"changed\",\"v\":\"login\"}");
        return 1;
    }
    if (!strcmp(op, "kr-get") || !strcmp(op, "kr-del")) {
        VEntry *found = NULL, *other = NULL;
        KrInfo fk;
        for (int i = 0; i < v->m.n; i++) {
            KrInfo k;
            if (!kr_info(v->m.e[i], owner, &k) || strcmp(k.service, service) || (account[0] && strcmp(k.account, account))) continue;
            if (admin || !strcmp(k.owner, owner) || (k.shared_with && op[3] == 'g')) { found = v->m.e[i]; fk = k; break; }
            if (!other) other = v->m.e[i];
        }
        if (!found) {
            if (other) {
                /* it exists but belongs to another app: the interface can ask the user whether to share it */
                KrInfo ok;
                kr_info(other, NULL, &ok);
                VjB b; vb_init(&b); vb_open(&b, '{'); vb_ks(&b, "error", "not-shared"); vb_ks(&b, "item", other->id); vb_ks(&b, "itemOwner", ok.owner); vb_close(&b, '}');
                reply(c, id, &b);
            } else reply_err(c, id, "not-found");
            return 1;
        }
        if (op[3] == 'd') {
            vm_remove(&v->m, found->id);
            if (vault_save(v) != VF_OK) reply_err(c, id, "save"); else reply_ok(c, id);
            broadcast_event("{\"ev\":\"changed\",\"v\":\"login\"}");
            return 1;
        }
        VjB b; vb_init(&b); vb_open(&b, '{'); vb_kb(&b, "ok", 1); vb_key(&b, "item"); b.first[b.depth] = 1; kr_item_json(&b, found, &fk, 1); vb_close(&b, '}');
        reply(c, id, &b);
        return 1;
    }
    if (!strcmp(op, "kr-list")) {
        VjB b; vb_init(&b); vb_open(&b, '{'); vb_kb(&b, "ok", 1); vb_key(&b, "items"); vb_open(&b, '[');
        for (int i = 0; i < v->m.n; i++) {
            KrInfo k;
            if (!kr_info(v->m.e[i], owner, &k)) continue;
            if (!admin && strcmp(k.owner, owner) && !k.shared_with) continue;
            kr_item_json(&b, v->m.e[i], &k, 0);
        }
        vb_close(&b, ']'); vb_close(&b, '}');
        reply(c, id, &b);
        return 1;
    }
    if (!strcmp(op, "kr-share")) {
        /* only axia-server (after asking the user) or the Passwords app */
        if (c->role != ROLE_APP && !vj_getb(j, 0, "admin", 0)) { reply_err(c, id, "denied"); return 1; }
        char eid[40] = "", app[72] = "";
        vj_getsbuf(j, 0, "id", eid, sizeof eid);
        vj_getsbuf(j, 0, "app", app, sizeof app);
        int allow = vj_getb(j, 0, "allow", 1);
        VEntry *e = vm_find(&v->m, eid);
        KrInfo k;
        if (!e || !kr_info(e, NULL, &k) || (!valid_owner(app) && strcmp(app, "*"))) { reply_err(c, id, "not-found"); return 1; }
        /* new list of shares */
        VjB sh; vb_init(&sh); vb_open(&sh, '[');
        Vj xj;
        if (!vj_parse(&xj, e->x, strlen(e->x))) {
            int s = vj_get(&xj, vj_get(&xj, 0, "kr"), "shared");
            for (int i = 0, t = s + 1; vj_type(&xj, s) == VJ_ARR && i < xj.t[s].count; i++, t = xj.t[t].next)
                if (!vj_streq(&xj, t, app)) { char a[72]; vj_strbuf(&xj, t, a, sizeof a); vb_sep(&sh); vb_str(&sh, a); }
            vj_free(&xj);
        }
        if (allow) { vb_sep(&sh); vb_str(&sh, app); }
        vb_close(&sh, ']');
        const char *secret = vm_field(e, "password");
        char *sec = sm_strdup(secret ? secret : "");
        char *label = sm_strdup(e->title);
        int r = kr_store(e->id, k.owner, k.service, k.account, label, sec, sh.p);
        sm_free(sec); sm_free(label); vb_free(&sh);
        if (r || vault_save(v) != VF_OK) reply_err(c, id, "save"); else reply_ok(c, id);
        return 1;
    }
    reply_err(c, id, "unknown-op");
    return 1;
}

/* ---- unlock with the session password ---- */

void keyring_session_password(const char *pass) {
    Vault *v = LOGIN;
    if (v->unlocked) return;
    char err[64];
    VFactors f = { .password = pass };
    /* does it exist? */
    if (v->exists != 1) {
        int fd = -1;
        (void)fd;
    }
    if (vault_unlock_with(v, VS_SESSION, &f, err, sizeof err) == 0) return;
    if (!strcmp(err, "not-found")) {
        VKdf kdf;
        vf_calibrate(400, &kdf, NULL);               /* login must not wait long */
        if (vault_create(v, VS_SESSION, &f, &kdf, "", "{\"autolock\":0}", NULL) == VF_OK) {
            fprintf(stderr, "axia-vault: session keyring created\n");
            keyring_lock_changed();
        }
        return;
    }
    /* the session password changed outside LordOS (passwd): the interface asks for the old one */
    char ev[160];
    snprintf(ev, sizeof ev, "{\"ev\":\"keyring-mismatch\",\"error\":\"%s\"}", err);
    broadcast_event(ev);
}

/* ================= Secret Service ================= */

static int ss_active;

/* x.ss → attributes (in smem) */
static int ss_attrs(const VEntry *e, SsAttr **out, int *n, char *ctype, size_t cn) {
    *out = NULL;
    *n = 0;
    if (ctype) snprintf(ctype, cn, "text/plain");
    if (!e->x) return 0;
    Vj j;
    if (vj_parse(&j, e->x, strlen(e->x))) return 0;
    int ss = vj_get(&j, 0, "ss");
    int ok = vj_type(&j, ss) == VJ_OBJ;
    if (ok) {
        if (ctype) vj_getsbuf(&j, ss, "ctype", ctype, cn);
        int a = vj_get(&j, ss, "attrs");
        if (vj_type(&j, a) == VJ_OBJ && j.t[a].count) {
            SsAttr *v = sm_alloc(sizeof(SsAttr) * (size_t)j.t[a].count);
            int k = 0;
            for (int t = a + 1; v && t < j.t[a].next; t = j.t[t + 1].next) {
                v[k].name = vj_str(&j, t);
                v[k].value = vj_str(&j, t + 1);
                if (v[k].name && v[k].value) k++;
                else { sm_free((void *)v[k].name); sm_free((void *)v[k].value); }
            }
            *out = v;
            *n = k;
        }
    }
    vj_free(&j);
    return ok;
}

static void ss_attrs_free(SsAttr *a, int n) {
    for (int i = 0; i < n; i++) { sm_free((void *)a[i].name); sm_free((void *)a[i].value); }
    sm_free(a);
}

static int be_locked(void *ud) { (void)ud; return !LOGIN->unlocked; }

static int be_request_unlock(void *ud, unsigned prompt) {
    (void)ud;
    if (LOGIN->unlocked) return 0;
    if (LOGIN->exists != 1) return -1;
    char ev[80];
    snprintf(ev, sizeof ev, "{\"ev\":\"ss-prompt\",\"prompt\":%u}", prompt);
    broadcast_event(ev);
    return 1;
}

static int be_lock(void *ud) { (void)ud; vault_lock(LOGIN); return 0; }

static int matches(const SsAttr *have, int nh, const SsAttr *want, int nw) {
    for (int w = 0; w < nw; w++) {
        int ok = 0;
        for (int h = 0; h < nh && !ok; h++) if (!strcmp(have[h].name, want[w].name) && !strcmp(have[h].value, want[w].value)) ok = 1;
        if (!ok) return 0;
    }
    return 1;
}

static int be_search(void *ud, const SsAttr *attrs, int nattrs, char (*ids)[40], int max) {
    (void)ud;
    Vault *v = LOGIN;
    if (!v->unlocked) return 0;
    int n = 0;
    for (int i = 0; i < v->m.n && n < max; i++) {
        SsAttr *a;
        int na;
        if (!ss_attrs(v->m.e[i], &a, &na, NULL, 0)) continue;
        if (matches(a, na, attrs, nattrs)) snprintf(ids[n++], 40, "%s", v->m.e[i]->id);
        ss_attrs_free(a, na);
    }
    return n;
}

static int be_info(void *ud, const char *id, SsItemInfo *out) {
    (void)ud;
    memset(out, 0, sizeof *out);
    VEntry *e = LOGIN->unlocked ? vm_find(&LOGIN->m, id) : NULL;
    if (!e) return -1;
    snprintf(out->id, sizeof out->id, "%s", e->id);
    out->label = sm_strdup(e->title);
    if (!ss_attrs(e, &out->attrs, &out->nattrs, NULL, 0)) { sm_free(out->label); return -1; }
    out->created = (uint64_t)e->created;
    out->modified = (uint64_t)e->modified;
    return 0;
}

static void be_free_info(void *ud, SsItemInfo *in) { (void)ud; sm_free(in->label); ss_attrs_free(in->attrs, in->nattrs); memset(in, 0, sizeof *in); }

static int be_get_secret(void *ud, const char *id, uint8_t **secret, size_t *len, char ctype[64]) {
    (void)ud;
    VEntry *e = LOGIN->unlocked ? vm_find(&LOGIN->m, id) : NULL;
    if (!e) return -1;
    SsAttr *a;
    int na;
    if (!ss_attrs(e, &a, &na, ctype, 64)) return -1;
    ss_attrs_free(a, na);
    const char *s = vm_field(e, "password");
    const char *enc = vm_field(e, "encoding");
    if (!s) s = "";
    if (enc && !strcmp(enc, "base64")) {
        *secret = b64_decode(s, strlen(s), len);
        return *secret ? 0 : -1;
    }
    *len = strlen(s);
    *secret = (uint8_t *)sm_strndup(s, *len);
    return *secret ? 0 : -1;
}

static void be_free_secret(void *ud, uint8_t *s, size_t len) { (void)ud; (void)len; sm_free(s); }

static int utf8_ok(const uint8_t *s, size_t n) {
    for (size_t i = 0; i < n;) {
        uint8_t c = s[i];
        if (!c) return 0;
        int l = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0;
        if (!l || i + (size_t)l > n) return 0;
        for (int k = 1; k < l; k++) if ((s[i + k] & 0xc0) != 0x80) return 0;
        i += (size_t)l;
    }
    return 1;
}

/* stores a Secret Service item (id NULL = new) */
static int ss_store(const char *id, const char *label, const SsAttr *attrs, int nattrs, const uint8_t *secret, size_t len, const char *ctype, char out_id[40]) {
    VjB b;
    vb_init(&b);
    vb_open(&b, '{');
    if (id) vb_ks(&b, "id", id);
    vb_ks(&b, "cat", "secrets");
    vb_ks(&b, "title", label ? label : "");
    const char *app = NULL, *user = NULL, *server = NULL;
    for (int i = 0; i < nattrs; i++) {
        if (!strcmp(attrs[i].name, "application") || !strcmp(attrs[i].name, "xdg:schema")) app = app ? app : attrs[i].value;
        if (!strcmp(attrs[i].name, "username_value") || !strcmp(attrs[i].name, "user") || !strcmp(attrs[i].name, "username")) user = attrs[i].value;
        if (!strcmp(attrs[i].name, "origin_url") || !strcmp(attrs[i].name, "server") || !strcmp(attrs[i].name, "signon_realm")) server = attrs[i].value;
    }
    vb_key(&b, "f");
    vb_open(&b, '[');
    if (user) { vb_sep(&b); vb_open(&b, '{'); vb_ks(&b, "k", "username"); vb_ks(&b, "t", "text"); vb_ks(&b, "v", user); vb_close(&b, '}'); }
    if (server) { vb_sep(&b); vb_open(&b, '{'); vb_ks(&b, "k", "url"); vb_ks(&b, "t", "url"); vb_ks(&b, "v", server); vb_close(&b, '}'); }
    vb_sep(&b); vb_open(&b, '{'); vb_ks(&b, "k", "password"); vb_ks(&b, "t", "hidden");
    int text = utf8_ok(secret, len);
    if (text) { vb_key(&b, "v"); vb_strn(&b, (const char *)secret, len); }
    else { char *e64 = b64_encode(secret, len); vb_ks(&b, "v", e64 ? e64 : ""); sm_free(e64); }
    vb_close(&b, '}');
    if (!text) { vb_sep(&b); vb_open(&b, '{'); vb_ks(&b, "k", "encoding"); vb_ks(&b, "t", "text"); vb_ks(&b, "v", "base64"); vb_close(&b, '}'); }
    vb_close(&b, ']');
    vb_key(&b, "x");
    vb_open(&b, '{');
    vb_key(&b, "ss");
    vb_open(&b, '{');
    vb_ks(&b, "ctype", ctype && ctype[0] ? ctype : "text/plain");
    if (app) vb_ks(&b, "app", app);
    vb_key(&b, "attrs");
    vb_open(&b, '{');
    for (int i = 0; i < nattrs; i++) vb_ks(&b, attrs[i].name, attrs[i].value);
    vb_close(&b, '}');
    vb_close(&b, '}');
    vb_close(&b, '}');
    vb_close(&b, '}');
    Vj j;
    VEntry *e = NULL;
    char err[64];
    int r = -1;
    if (!b.oom && !vj_parse(&j, b.p, b.n)) { r = vm_upsert(&LOGIN->m, &j, 0, &e, err, sizeof err); vj_free(&j); }
    vb_free(&b);
    if (r || !e) return -1;
    if (out_id) snprintf(out_id, 40, "%s", e->id);
    return vault_save(LOGIN) == VF_OK ? 0 : -1;
}

static int be_create(void *ud, const char *label, const SsAttr *attrs, int nattrs, const uint8_t *secret, size_t len, const char *ctype, int replace, char out_id[40]) {
    (void)ud;
    if (!LOGIN->unlocked) return -1;
    const char *existing = NULL;
    if (replace) {
        char ids[1][40];
        /* the exact same set of attributes */
        for (int i = 0; i < LOGIN->m.n && !existing; i++) {
            SsAttr *a;
            int na;
            if (!ss_attrs(LOGIN->m.e[i], &a, &na, NULL, 0)) continue;
            if (na == nattrs && matches(a, na, attrs, nattrs)) { snprintf(ids[0], 40, "%s", LOGIN->m.e[i]->id); existing = ids[0]; }
            ss_attrs_free(a, na);
        }
        if (existing) return ss_store(existing, label, attrs, nattrs, secret, len, ctype, out_id);
    }
    return ss_store(NULL, label, attrs, nattrs, secret, len, ctype, out_id);
}

static int be_set_secret(void *ud, const char *id, const uint8_t *secret, size_t len, const char *ctype) {
    (void)ud;
    VEntry *e = LOGIN->unlocked ? vm_find(&LOGIN->m, id) : NULL;
    if (!e) return -1;
    SsAttr *a;
    int na;
    if (!ss_attrs(e, &a, &na, NULL, 0)) return -1;
    char *label = sm_strdup(e->title);
    int r = ss_store(id, label, a, na, secret, len, ctype, NULL);
    sm_free(label);
    ss_attrs_free(a, na);
    return r;
}

static int be_set_label(void *ud, const char *id, const char *label) {
    (void)ud;
    VEntry *e = LOGIN->unlocked ? vm_find(&LOGIN->m, id) : NULL;
    if (!e) return -1;
    uint8_t *s;
    size_t n;
    char ct[64];
    if (be_get_secret(NULL, id, &s, &n, ct)) return -1;
    SsAttr *a;
    int na;
    ss_attrs(e, &a, &na, NULL, 0);
    int r = ss_store(id, label, a, na, s, n, ct, NULL);
    ss_attrs_free(a, na);
    sm_free(s);
    return r;
}

static int be_set_attrs(void *ud, const char *id, const SsAttr *attrs, int nattrs) {
    (void)ud;
    VEntry *e = LOGIN->unlocked ? vm_find(&LOGIN->m, id) : NULL;
    if (!e) return -1;
    uint8_t *s;
    size_t n;
    char ct[64];
    if (be_get_secret(NULL, id, &s, &n, ct)) return -1;
    char *label = sm_strdup(e->title);
    int r = ss_store(id, label, attrs, nattrs, s, n, ct, NULL);
    sm_free(label);
    sm_free(s);
    return r;
}

static int be_remove(void *ud, const char *id) {
    (void)ud;
    if (!LOGIN->unlocked || vm_remove(&LOGIN->m, id)) return -1;
    return vault_save(LOGIN) == VF_OK ? 0 : -1;
}

static void be_caller(void *ud, const char *sender, uint32_t pid) {
    (void)ud;
    if (getenv("LORD_VAULT_DEBUG")) fprintf(stderr, "axia-vault: Secret Service: request from %s (pid %u)\n", sender, pid);
}

static const SsBackend backend = {
    .locked = be_locked, .request_unlock = be_request_unlock, .lock = be_lock, .search = be_search, .info = be_info,
    .free_info = be_free_info, .get_secret = be_get_secret, .free_secret = be_free_secret, .create = be_create,
    .set_secret = be_set_secret, .set_label = be_set_label, .set_attrs = be_set_attrs, .remove = be_remove, .caller = be_caller,
};

void keyring_start_bus(int busfd) {
    int r = ss_start(busfd, &backend, NULL);
    ss_active = r == 0;
    if (r == 1) fprintf(stderr, "axia-vault: org.freedesktop.secrets already has an owner (gnome-keyring?): not serving it\n");
    else if (r < 0) fprintf(stderr, "axia-vault: could not talk to the session bus\n");
    else if (getenv("LORD_VAULT_DEBUG")) fprintf(stderr, "axia-vault: Secret Service on the bus (%s)\n", ss_unique_name());
    if (r) close(busfd);
}

int keyring_bus_fd(void) { return ss_active ? ss_fd() : -1; }
int keyring_bus_wants_out(void) { return ss_active && ss_pending_out(); }
void keyring_bus_dispatch(void) { if (ss_active && ss_dispatch() < 0) { ss_active = 0; ss_stop(); } }
void keyring_lock_changed(void) { if (ss_active) ss_notify_locked(!LOGIN->unlocked); }
void keyring_prompt_result(unsigned prompt, int ok) { if (ss_active) ss_prompt_done(prompt, ok && LOGIN->unlocked); }
