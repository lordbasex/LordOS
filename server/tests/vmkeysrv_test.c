/* vmkeysrv_test.c — lord-server's side of encrypted machines' keys (src/vm/vmkeysrv.c, docs/VIRTUALIZATION.md 8) against
 * a stand-in for lord-vault's keyring (vc_request_wait below): new keys, fetch, copy for a Clone, a typed recovery key, a
 * locked keyring, a missing item, the pipe lord-vm reads, the requests' JSON (never a key in a request other than kr-set),
 * and hostile ids and recovery keys. Under ASan and UBSan. */
#include "../src/vm/vmkeysrv.h"
#include "../src/vm/machine.h"
#include "../src/sec/json.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- the keyring stand-in ---- */
static pthread_mutex_t kmu = PTHREAD_MUTEX_INITIALIZER;
static struct { char service[64], secret[80], label[128]; } items[16];
static int nitems, locked, requests, bad_requests;
int vc_request_wait(const char *json, size_t len, char **rep, size_t *rlen, int timeout_ms) {
    (void)timeout_ms;
    JDoc d;
    char out[512];
    pthread_mutex_lock(&kmu);
    requests++;
    if (json_parse(json, len, &d, NULL, 0)) { bad_requests++; pthread_mutex_unlock(&kmu); return -1; }
    const char *op = json_str(d.root, "op"), *owner = json_str(d.root, "owner"), *svc = json_str(d.root, "service"), *sec = json_str(d.root, "secret");
    if (!op || !owner || strcmp(owner, "machines") || !svc || strncmp(svc, "machines:", 9)) bad_requests++;
    if (sec && strcmp(op, "kr-set")) bad_requests++;                 /* a key travels only to be kept */
    int i = -1;
    for (int k = 0; k < nitems; k++) if (svc && !strcmp(items[k].service, svc)) i = k;
    if (locked) snprintf(out, sizeof out, "{\"error\":\"locked\"}");
    else if (!strcmp(op, "kr-get")) {
        if (i < 0) snprintf(out, sizeof out, "{\"error\":\"not-found\"}");
        else snprintf(out, sizeof out, "{\"ok\":true,\"item\":{\"secret\":\"%s\"}}", items[i].secret);
    } else if (!strcmp(op, "kr-set") && sec && svc) {
        if (i < 0 && nitems < 16) i = nitems++;
        if (i >= 0) {
            snprintf(items[i].service, sizeof items[i].service, "%s", svc);
            snprintf(items[i].secret, sizeof items[i].secret, "%s", sec);
            const char *lb = json_str(d.root, "label");
            snprintf(items[i].label, sizeof items[i].label, "%s", lb ? lb : "");
        }
        snprintf(out, sizeof out, "{\"ok\":true}");
    } else if (!strcmp(op, "kr-del")) {
        if (i >= 0) { items[i] = items[--nitems]; }
        snprintf(out, sizeof out, "{\"ok\":true}");
    } else snprintf(out, sizeof out, "{\"error\":\"bad\"}");
    json_free(&d);
    pthread_mutex_unlock(&kmu);
    *rep = strdup(out);
    *rlen = strlen(out);
    return 0;
}

static int wait_state(const char *id) {
    for (int i = 0; i < 300; i++) {
        int st = vmkey_state(id);
        if (st != VMK_PENDING) return st;
        usleep(10000);
    }
    return VMK_PENDING;
}
static int read_pipe(const char *id, char out[80]) {
    int fd = vmkey_pipe(id);
    if (fd < 0) return -1;
    ssize_t n = read(fd, out, 79);
    close(fd);
    out[n > 0 ? n : 0] = 0;
    return (int)n;
}

int main(void) {
    const char *A = "11111111-2222-4333-8444-555555555555", *B = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee", *C = "99999999-8888-4777-8666-555555555555";
    char k1[80], k2[80];
    /* a new key: kept, then handed out on a pipe, 64 hex */
    CHECK(vmkey_new(A, "Debian") == 0);
    CHECK(wait_state(A) == VMK_READY);
    CHECK(read_pipe(A, k1) == 64 && vm_key_valid(k1, 64));
    CHECK(nitems == 1 && !strcmp(items[0].secret, k1) && !strcmp(items[0].label, "Machines: Debian"));
    /* never two keys under one id: a second new key for A is refused, the first stays */
    CHECK(vmkey_new(A, "Debian") == 0);
    CHECK(wait_state(A) == VMK_ERROR);
    CHECK(vmkey_pipe(A) < 0);
    CHECK(vmkey_fetch(A) == 0 && wait_state(A) == VMK_READY && read_pipe(A, k2) == 64 && !strcmp(k1, k2));
    /* a Clone's: the same key under B */
    CHECK(vmkey_copy(A, B, "Debian clone") == 0);
    CHECK(wait_state(B) == VMK_READY && read_pipe(B, k2) == 64 && !strcmp(k1, k2));
    /* a missing one, then the recovery key typed (dashes, spaces and capitals ignored) */
    CHECK(vmkey_fetch(C) == 0 && wait_state(C) == VMK_MISSING);
    char typed[100];
    snprintf(typed, sizeof typed, "%.8s-%.8s %.8s-%.8s\n%.8s-%.8s-%.8s-%.8s", k1, k1 + 8, k1 + 16, k1 + 24, k1 + 32, k1 + 40, k1 + 48, k1 + 56);
    for (char *p = typed; *p; p++) if (*p >= 'a' && *p <= 'f') *p = (char)(*p - 'a' + 'A');
    CHECK(vmkey_enter(C, typed, "Imported") == 0);
    CHECK(wait_state(C) == VMK_READY && read_pipe(C, k2) == 64 && !strcmp(k1, k2));
    /* hostile recovery keys and ids */
    const char *badk[] = { "", "1234", "zz", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcde",
                           "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0", "\"},\"op\":\"kr-del" };
    for (size_t i = 0; i < sizeof badk / sizeof *badk; i++) CHECK(vmkey_enter(C, badk[i], "x") == -1);
    const char *badid[] = { "", "x", "11111111-2222-4333-8444-55555555555\"", "../../etc", "11111111-2222-4333-8444-5555555555551" };
    for (size_t i = 0; i < sizeof badid / sizeof *badid; i++) { CHECK(vmkey_fetch(badid[i]) == -1); CHECK(vmkey_new(badid[i], "n") == -1); CHECK(vmkey_pipe(badid[i]) < 0); }
    /* a locked keyring */
    vmkey_wipe_all();
    CHECK(vmkey_state(A) == VMK_NONE && vmkey_pipe(A) < 0);
    locked = 1;
    CHECK(vmkey_fetch(A) == 0 && wait_state(A) == VMK_LOCKED && vmkey_pipe(A) < 0);
    locked = 0;
    CHECK(vmkey_fetch(A) == 0 && wait_state(A) == VMK_READY);
    /* a name with quotes stays inside its JSON string */
    const char *D = "12345678-1234-4234-8234-123456789abc";
    CHECK(vmkey_new(D, "a\"b\\c") == 0 && wait_state(D) == VMK_READY);
    /* forgetting: the item goes from the keyring */
    int before = nitems;
    vmkey_forget(D);
    for (int i = 0; i < 100 && nitems == before; i++) usleep(10000);
    CHECK(nitems == before - 1 && vmkey_state(D) == VMK_NONE);
    /* many machines at once: a bounded table (8 held or waiting); a request with no room is refused (-1), never a
     * key dropped while it's being asked for; the ones taken all end ready */
    char ids[20][37];
    int took[20], ntook = 0;
    for (int i = 0; i < 20; i++) { snprintf(ids[i], sizeof ids[i], "%08x-0000-4000-8000-%012x", i + 1, i + 1); took[i] = vmkey_new(ids[i], "m") == 0; ntook += took[i]; }
    CHECK(ntook >= 1 && ntook <= 20);
    for (int i = 0; i < 20; i++) if (took[i]) { int st = wait_state(ids[i]); CHECK(st == VMK_READY || st == VMK_NONE); }
    for (int i = 0; i < 20; i++) if (!took[i]) { CHECK(vmkey_new(ids[i], "m") == 0); CHECK(wait_state(ids[i]) == VMK_READY); }
    CHECK(bad_requests == 0);
    printf("vmkeysrv: %d checks, %d failed (%d keyring requests)\n", checks, fails, requests);
    return fails ? 1 : 0;
}
