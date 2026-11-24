/* vmimage_group_test.c — the admin actions vm-image-list and vm-image-get for a machine group's archive (Whonix,
 * src/vm/vmimage_admin.c; docs/VIRTUALIZATION.md 19.6) with the network replaced: fetch_batch_in serves files of the
 * test's own. Run in a user namespace where this account is root (`unshare -Ur`, mk/vm.mk), so the store's files are
 * "root's" as the helper requires. Checks:
 *   - the release folder: below the catalog's floor, not a release, or below the newest one ever verified (the root-side
 *     high-water mark lists/whonix-amd64.newest) is refused;
 *   - a list or an archive whose name gives another release than its folder's (an older signed file beside a newer
 *     folder) is refused; the real 18.2.4.3 list verifies and records its release and the high-water mark;
 *   - the archive is downloaded under a temporary name only (<sha512>.tar.xz.dl), the final name exists only after the
 *     SHA-512 matched: a mismatch leaves neither; a file planted at the final name that isn't in the verified state
 *     (root's, one link, 0644) is thrown away, not trusted. */
#include "admin.h"
#include "vm/vm_admin.h"
#include "pkg/http.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define T_DIR "build/vmimage-group-test"
static char said[1024];
static int fails;
#define CHECK(c, what) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s (said: %s)\n", __FILE__, __LINE__, what, said); fails++; } } while (0)

void admin_say(const char *kind, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int k = snprintf(said, sizeof said, "%s ", kind);
    vsnprintf(said + k, sizeof said - (size_t)k, fmt, ap);
    va_end(ap);
}

/* the "network": URL → a local file; the archive's bytes are whatever archive_body says */
static const char *archive_body = "not the archive";
static int saw_final_dest;
static const char *serve(const char *url) {
    const char *b = strrchr(url, '/') + 1;
    if (!strcmp(url, "https://download.whonix.org/libvirt/")) return "tests/vmimage/whonix.sums";   /* any listing */
    if (strstr(b, ".sha512sums.asc")) return "tests/vmimage/whonix.asc";
    if (strstr(b, ".sha512sums")) return "tests/vmimage/whonix.sums";
    return NULL;
}
int fetch_batch_in(FetchItem *items, int n, FetchProgress cb, void *ud, const char *scope) {
    (void)cb; (void)ud; (void)scope;
    int got = 0;
    for (int i = 0; i < n; i++) {
        FetchItem *it = &items[i];
        size_t dl = strlen(it->dest);
        if (dl > 7 && !strcmp(it->dest + dl - 7, ".tar.xz")) saw_final_dest = 1;
        const char *src = serve(it->url);
        FILE *o = fopen(it->dest, "w");
        if (!o) { it->status = 0; continue; }
        if (src) {
            FILE *in = fopen(src, "r");
            char b[4096];
            size_t k;
            while (in && (k = fread(b, 1, sizeof b, in)) > 0) fwrite(b, 1, k, o);
            if (in) fclose(in);
        } else fputs(archive_body, o);
        fclose(o);
        it->status = 200;
        it->size = 1;
        got++;
    }
    return got;
}
int fetch_batch(FetchItem *items, int n, FetchProgress cb, void *ud) { return fetch_batch_in(items, n, cb, ud, NULL); }

static AdminReq R = { .id = 1, .user = "t", .home = "/", .uid = 0, .gid = 0, .admin = 1 };
static void list(const char *arg) { said[0] = 0; vm_act_image_list(&R, arg, strlen(arg)); }
static void get(const char *arg) { said[0] = 0; vm_act_image_get(&R, arg, strlen(arg)); }
static int exists(const char *p) { struct stat st; return !lstat(p, &st); }
static void put(const char *p, const char *s, mode_t mode) { FILE *f = fopen(p, "w"); if (f) { fputs(s, f); fclose(f); } chmod(p, mode); }

#define LIST "Whonix-LXQt-18.2.4.3.Intel_AMD64.qcow2.libvirt.xz.sha512sums"
#define FILEN "Whonix-LXQt-18.2.4.3.Intel_AMD64.qcow2.libvirt.xz"
#define SHA "e57c79cb137f941a4f5341d841c1da8866182ebc9115acb17e21aed883c72c1566e3ef4e1078a07b018f41f60182db050cbcd931235051f0d7e90c0005fa5354"
#define FINAL T_DIR "/machines/images/" SHA ".tar.xz"

int main(void) {
    if (getuid() != 0) { printf("vmimage-group-test: skipped: needs to run as root in a user namespace (unshare -Ur)\n"); return 77; }
    /* the catalog, the keys and the store: the real catalog and the pinned keys, a fresh store */
    if (system("rm -rf " T_DIR " && mkdir -p " T_DIR "/keys && cp ../assets/machines/systems.json " T_DIR "/systems.json && cp ../packaging/keys/machines/whonix-derivative.asc " T_DIR "/keys/")) return 1;

    /* the release folder */
    list("whonix amd64 18.2.4.2");
    CHECK(strstr(said, "admin.invalidArgument"), "a release below the catalog's floor");
    list("whonix amd64 ../18.2.4.3");
    CHECK(strstr(said, "admin.invalidArgument"), "not a release");
    list("whonix amd64 18.2.4.3");
    CHECK(!strncmp(said, "OK ", 3), "the floor's own folder listing");

    /* a list naming another release than its folder */
    list("whonix amd64 18.2.4.5 " LIST);
    CHECK(strstr(said, "admin.invalidArgument"), "an older release's list beside a newer folder");
    list("whonix amd64 18.2.4.3 Whonix-LXQt-18.2.4.5.Intel_AMD64.qcow2.libvirt.xz.sha512sums");
    CHECK(strstr(said, "admin.invalidArgument"), "a newer release's list in an older folder");
    CHECK(!exists(T_DIR "/machines/lists/whonix-amd64.release"), "nothing recorded");

    /* the real list: verified, its release recorded, the high-water mark raised */
    list("whonix amd64 18.2.4.3 " LIST);
    CHECK(!strncmp(said, "OK ", 3), "the real 18.2.4.3 list verifies");
    char b[64] = "";
    FILE *f = fopen(T_DIR "/machines/lists/whonix-amd64.newest", "r");
    if (f) { if (!fgets(b, sizeof b, f)) b[0] = 0; fclose(f); }
    CHECK(!strcmp(b, "18.2.4.3"), "the newest verified release, root-side");

    /* the archive: another release's name, or a name and hash not in the list */
    get("whonix amd64 Whonix-LXQt-18.2.4.4.Intel_AMD64.qcow2.libvirt.xz " SHA);
    CHECK(strstr(said, "vm.notListed"), "an archive of another release than the list's");
    get("whonix amd64 " FILEN " " SHA "x");
    CHECK(strstr(said, "admin.invalidArgument"), "a bad hash");

    /* a download whose bytes aren't the archive: never under the final name, nothing left */
    saw_final_dest = 0;
    get("whonix amd64 " FILEN " " SHA);
    CHECK(strstr(said, "vm.hashMismatch"), "the SHA-512 is checked");
    CHECK(!saw_final_dest, "the download never goes to the final name");
    CHECK(!exists(FINAL) && !exists(FINAL ".dl"), "a mismatch leaves nothing");

    /* a file planted at the final name that isn't in the verified state: thrown away, downloaded again */
    put(FINAL, "planted", 0600);
    get("whonix amd64 " FILEN " " SHA);
    CHECK(strstr(said, "vm.hashMismatch") && !exists(FINAL), "an unverified file at the final name isn't trusted");
    put(FINAL, "planted", 0644);
    if (link(FINAL, T_DIR "/machines/images/other")) {}
    get("whonix amd64 " FILEN " " SHA);
    CHECK(strstr(said, "vm.hashMismatch") && !exists(FINAL), "a second link to it isn't the verified state");
    unlink(T_DIR "/machines/images/other");

    /* the high-water mark: a newer release once verified, the older one is refused */
    put(T_DIR "/machines/lists/whonix-amd64.newest", "18.2.5.0", 0644);
    list("whonix amd64 18.2.4.3 " LIST);
    CHECK(strstr(said, "admin.invalidArgument"), "below the newest release ever verified");
    get("whonix amd64 " FILEN " " SHA);
    CHECK(strstr(said, "vm.notListed"), "an archive below the newest release ever verified");

    if (system("rm -rf " T_DIR)) {}
    if (fails) { fprintf(stderr, "vmimage-group-test: %d failures\n", fails); return 1; }
    printf("vmimage-group-test: ok\n");
    return 0;
}
