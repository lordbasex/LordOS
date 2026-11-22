/* studio_bg_admin_test.c — LordOS Studio's closed admin actions studio-bg-get and studio-bg-remove (E-S4;
 * server/src/hw/studio_bg_admin.c) and their catalog lookup (studio_bg_find.c): what they accept as an argument, what
 * they accept from the catalog, and that a file whose size or hash isn't the catalog's is never kept. fetch_batch is
 * replaced by a fake that "downloads" fixed bytes; the catalog and the folder are under build/ (STUDIO_BG_CATALOG,
 * STUDIO_BG_STORE). Also checks the shipped catalog assets/studio/backgrounds.json: every entry is one the action
 * would accept, and every license is one of the allowed ones (docs/CAMERA-STUDIO-PLAN.md 6.1). Run from server/. */
#include "../src/hw/studio_bg_find.h"
#include "../src/hw/hw_admin.h"
#include "../src/pkg/crypto.h"
#include "../src/pkg/http.h"
#include "../src/sec/json.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define H "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
static int bad;
#define CHECK(c, ...) do { if (!(c)) { bad++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the admin helper's side, faked ---- */
static char said[64][300];
static int nsaid;
void admin_say(const char *kind, const char *fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (nsaid < 64) snprintf(said[nsaid++], sizeof said[0], "%s %s", kind, line);
}
static const char *last(void) { return nsaid ? said[nsaid - 1] : ""; }

static const char *fake_body = "fake picture bytes";
static int fake_status = 200, fetches;
static char fetched_url[900];
int fetch_batch(FetchItem *items, int n, FetchProgress cb, void *ud) {
    fetches++;
    for (int i = 0; i < n; i++) {
        snprintf(fetched_url, sizeof fetched_url, "%s", items[i].url);
        FILE *f = fopen(items[i].dest, "wb");
        if (f) { fwrite(fake_body, 1, strlen(fake_body), f); fclose(f); }
        items[i].status = fake_status;
        items[i].size = strlen(fake_body);
        sha256(fake_body, strlen(fake_body), items[i].sha256);
        if (cb) cb(items[i].size, items[i].expect, ud);
    }
    return 0;
}

/* ---- the lookup ---- */
static int find(const char *json, const char *arg, const char **ext) {
    char url[300] = "", sha[65] = "";
    unsigned long long size = 0;
    const char *e = NULL;
    int r = hw_studio_bg_find(json, strlen(json), arg, url, sizeof url, sha, sizeof sha, &size, &e);
    if (ext) *ext = e;
    return r;
}

static void test_find(void) {
    const char *ok = "{\"base\":\"https://raw.githubusercontent.com/lordbasex/LordOS-wallpapers/main/studio\",\"items\":["
                     "{\"id\":\"office-a\",\"kind\":\"picture\",\"variants\":[{\"name\":\"1080\",\"file\":\"pictures/office-a.1080.jpg\",\"sha256\":\"" H "\",\"size\":10}]},"
                     "{\"id\":\"sky-b\",\"kind\":\"video\",\"variants\":[{\"name\":\"720.h264\",\"file\":\"videos/sky-b.720.h264.mp4\",\"sha256\":\"" H "\",\"size\":10}]},"
                     "{\"id\":\"odd\",\"kind\":\"video\",\"variants\":[{\"name\":\"v\",\"file\":\"videos/odd.jpg\",\"sha256\":\"" H "\",\"size\":10}]},"
                     "{\"id\":\"what\",\"kind\":\"script\",\"variants\":[{\"name\":\"v\",\"file\":\"x.jpg\",\"sha256\":\"" H "\",\"size\":10}]},"
                     "{\"id\":\"up\",\"kind\":\"picture\",\"variants\":[{\"name\":\"v\",\"file\":\"../x.jpg\",\"sha256\":\"" H "\",\"size\":10}]},"
                     "{\"id\":\"abs\",\"kind\":\"picture\",\"variants\":[{\"name\":\"v\",\"file\":\"/etc/x.jpg\",\"sha256\":\"" H "\",\"size\":10}]},"
                     "{\"id\":\"frac\",\"kind\":\"picture\",\"variants\":[{\"name\":\"v\",\"file\":\"f.jpg\",\"sha256\":\"" H "\",\"size\":10.5}]}"
                     "]}";
    const char *ext;
    CHECK(find(ok, "office-a 1080", &ext) == 0 && !strcmp(ext, "jpg"), "a picture");
    CHECK(find(ok, "sky-b 720.h264", &ext) == 0 && !strcmp(ext, "mp4"), "a video");
    CHECK(find(ok, "odd v", NULL) == -1, "a video whose file isn't an .mp4");
    CHECK(find(ok, "what v", NULL) == -1, "an unknown kind");
    CHECK(find(ok, "up v", NULL) == -1, "a file outside the base");
    CHECK(find(ok, "abs v", NULL) == -1, "an absolute file");
    CHECK(find(ok, "frac v", NULL) == -1, "a fractional size");
    /* the argument itself */
    static const char *const args[] = { "", "office-a", "office-a ", " 1080", "office-a 1080 x", "../office-a 1080", "office-a ../1080",
                                        "Office-a 1080", "office-a 1080\n", "office/a 1080", "office-a 2160", "nope 1080",
                                        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa 1080", "office-a\t1080" };
    for (size_t i = 0; i < sizeof args / sizeof args[0]; i++) CHECK(find(ok, args[i], NULL) == -1, "argument \"%s\" was accepted", args[i]);
    CHECK(find("{\"base\":\"https://example.org/x\",\"items\":[]}", "office-a 1080", NULL) == -1, "another host");
    CHECK(find("{\"base\":\"https://raw.githubusercontent.com/someoneelse/x\",\"items\":[{\"id\":\"a\",\"kind\":\"picture\",\"variants\":[{\"name\":\"v\",\"file\":\"a.jpg\",\"sha256\":\"" H "\",\"size\":1}]}]}", "a v", NULL) == -1, "another owner");
    CHECK(find("{\"base\":\"https://raw.githubusercontent.com/lordbasex/x?y=\",\"items\":[{\"id\":\"a\",\"kind\":\"picture\",\"variants\":[{\"name\":\"v\",\"file\":\"a.jpg\",\"sha256\":\"" H "\",\"size\":1}]}]}", "a v", NULL) == -1, "a query in the base");
    CHECK(find("{\"base\":\"https://raw.githubusercontent.com/lordbasex/x\",\"items\":[{\"id\":\"a\",\"kind\":\"picture\",\"variants\":[{\"name\":\"v\",\"file\":\"a.jpg\",\"sha256\":\"ABC\",\"size\":1}]}]}", "a v", NULL) == -1, "a bad hash");
    CHECK(find("{\"base\":\"https://raw.githubusercontent.com/lordbasex/x\",\"items\":[{\"id\":\"a\",\"kind\":\"picture\",\"variants\":[{\"name\":\"v\",\"file\":\"a.jpg\",\"sha256\":\"" H "\",\"size\":200000000}]}]}", "a v", NULL) == -1, "too big");
    CHECK(find("not json", "a v", NULL) == -1, "not json");
}

/* ---- the actions, against a catalog of our own ---- */
#define DIR "build/studio-bg-test"
static void write_catalog(const char *sha, size_t size) {
    FILE *f = fopen(DIR "/catalog.json", "w");
    fprintf(f, "{\"format\":1,\"base\":\"https://raw.githubusercontent.com/lordbasex/LordOS-wallpapers/main/studio\",\"items\":["
               "{\"id\":\"office-a\",\"kind\":\"picture\",\"variants\":[{\"name\":\"1080\",\"file\":\"pictures/office-a.1080.jpg\",\"sha256\":\"%s\",\"size\":%zu}]},"
               "{\"id\":\"sky-b\",\"kind\":\"video\",\"variants\":[{\"name\":\"720.h264\",\"file\":\"videos/sky-b.720.h264.mp4\",\"sha256\":\"%s\",\"size\":%zu}]}]}",
            sha, size, sha, size);
    fclose(f);
}

static void test_actions(void) {
    mkdir("build", 0755);
    mkdir(DIR, 0755);
    mkdir(DIR "/store", 0755);
    uint8_t h[32];
    char sha[65];
    sha256(fake_body, strlen(fake_body), h);
    for (int i = 0; i < 32; i++) snprintf(sha + 2 * i, 3, "%02x", h[i]);
    write_catalog(sha, strlen(fake_body));
    AdminReq r = { .id = 1, .user = "u", .home = "/nonexistent", .uid = 1000, .gid = 1000, .admin = 0 };
    char path[256];
    snprintf(path, sizeof path, DIR "/store/%s.jpg", sha);
    unlink(path);
    /* refused arguments never fetch */
    static const char *const refused[] = { "", "office-a", "office-a 2160", "../../etc/passwd x", "office-a 1080 extra",
                                           "https://evil.example/x 1080", "/etc/shadow 1080", "office-a 1080\n" };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        int before = fetches;
        hw_act_studio_bg_get(&r, refused[i], strlen(refused[i]));
        CHECK(fetches == before && !strncmp(last(), "ERR admin.invalidArgument", 25), "studio-bg-get \"%s\": %s", refused[i], last());
        hw_act_studio_bg_remove(&r, refused[i], strlen(refused[i]));
        CHECK(!strncmp(last(), "ERR admin.invalidArgument", 25), "studio-bg-remove \"%s\": %s", refused[i], last());
    }
    /* an argument with a NUL inside: the length says more than the string */
    hw_act_studio_bg_get(&r, "office-a 1080\0x", 15);
    CHECK(!strncmp(last(), "ERR admin.invalidArgument", 25), "a NUL inside the argument: %s", last());
    /* a good one: fetched from the catalog's base, kept as <sha>.jpg, answered with its hash */
    hw_act_studio_bg_get(&r, "office-a 1080", 13);
    char want[80];
    snprintf(want, sizeof want, "OK %s", sha);
    CHECK(!strcmp(last(), want), "studio-bg-get: %s", last());
    CHECK(!strcmp(fetched_url, "https://raw.githubusercontent.com/lordbasex/LordOS-wallpapers/main/studio/pictures/office-a.1080.jpg"), "url %s", fetched_url);
    struct stat st;
    CHECK(!stat(path, &st) && (st.st_mode & 0777) == 0644, "the file isn't kept 0644");
    /* again: already there with the right hash, nothing fetched */
    int before = fetches;
    hw_act_studio_bg_get(&r, "office-a 1080", 13);
    CHECK(fetches == before && !strcmp(last(), want), "a second get fetched again");
    /* a video goes to .mp4 */
    hw_act_studio_bg_get(&r, "sky-b 720.h264", 14);
    char vpath[256];
    snprintf(vpath, sizeof vpath, DIR "/store/%s.mp4", sha);
    CHECK(!stat(vpath, &st), "the video isn't <sha>.mp4");
    /* the bytes differ from the catalog's hash: deleted, an error */
    write_catalog(H, strlen(fake_body));
    char hpath[256];
    snprintf(hpath, sizeof hpath, DIR "/store/%s.jpg", H);
    hw_act_studio_bg_get(&r, "office-a 1080", 13);
    CHECK(!strncmp(last(), "ERR studio.downloadFailed", 25) && stat(hpath, &st) != 0, "a wrong hash was kept: %s", last());
    /* the server said 404: deleted */
    write_catalog(sha, strlen(fake_body));
    unlink(path);
    fake_status = 404;
    hw_act_studio_bg_get(&r, "office-a 1080", 13);
    CHECK(!strncmp(last(), "ERR studio.downloadFailed", 25) && stat(path, &st) != 0, "a 404 was kept: %s", last());
    fake_status = 200;
    /* remove */
    hw_act_studio_bg_get(&r, "office-a 1080", 13);
    hw_act_studio_bg_remove(&r, "office-a 1080", 13);
    CHECK(!strcmp(last(), "OK removed") && stat(path, &st) != 0, "studio-bg-remove: %s", last());
    hw_act_studio_bg_remove(&r, "office-a 1080", 13);
    CHECK(!strcmp(last(), "OK removed"), "removing twice: %s", last());
    unlink(vpath);
}

/* ---- the shipped catalog ---- */
static const JVal *get(const JVal *o, const char *k) {
    if (!o || o->type != J_OBJ) return NULL;
    for (const JVal *m = o->kid; m; m = m->next) if (!strcmp(m->key, k)) return m;
    return NULL;
}
static const char *str(const JVal *o, const char *k) { const JVal *v = get(o, k); return v && v->type == J_STR ? v->str : NULL; }

static void test_shipped(void) {
    FILE *f = fopen("../assets/studio/backgrounds.json", "rb");
    if (!f) { printf("shipped catalog: skipped: needs assets/studio/backgrounds.json\n"); return; }
    static char buf[1 << 20];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    JDoc d;
    CHECK(!json_parse(buf, n, &d, NULL, 0), "the catalog isn't JSON");
    const JVal *items = get(d.root, "items");
    int count = 0, pictures = 0, videos = 0;
    static const char *const allowed[] = { "CC0", "Public domain", "CC BY 2.0", "CC BY 3.0", "CC BY 4.0", "CC BY-SA 2.0", "CC BY-SA 3.0",
                                           "CC BY-SA 4.0", "MIT" };
    for (const JVal *it = items && items->type == J_ARR ? items->kid : NULL; it; it = it->next) {
        const char *id = str(it, "id"), *lic = str(it, "license"), *kind = str(it, "kind"), *src = str(it, "source"), *author = str(it, "author");
        int ok = 0;
        for (size_t i = 0; lic && i < sizeof allowed / sizeof allowed[0]; i++) ok |= !strcmp(lic, allowed[i]);
        CHECK(ok, "%s: license \"%s\" isn't allowed", id ? id : "?", lic ? lic : "");
        CHECK(src && !strstr(src, "unsplash") && !strstr(src, "pexels") && !strstr(src, "pixabay"), "%s: source %s", id ? id : "?", src ? src : "");
        CHECK(author && *author, "%s: no author", id ? id : "?");
        if (lic && (!strncmp(lic, "CC BY", 5))) CHECK(str(it, "credit") != NULL, "%s: CC BY needs a credit line", id ? id : "?");
        const JVal *ai = get(it, "ai");      /* our AI-generated pictures are released as CC0 */
        if (ai) CHECK(ai->type == J_BOOL && ai->num == 1 && lic && !strcmp(lic, "CC0"), "%s: \"ai\" must be true and its license CC0", id ? id : "?");
        const JVal *vars = get(it, "variants");
        for (const JVal *v = vars && vars->type == J_ARR ? vars->kid : NULL; v; v = v->next) {
            char arg[128];
            snprintf(arg, sizeof arg, "%s %s", id ? id : "", str(v, "name") ? str(v, "name") : "");
            const char *ext = NULL;
            int r = find(buf, arg, &ext);
            CHECK(r == 0, "the action would refuse the shipped entry \"%s\"", arg);
        }
        char thumb[256];
        snprintf(thumb, sizeof thumb, "../assets/studio/backgrounds/%s.thumb.jpg", id ? id : "");
        struct stat st;
        CHECK(!stat(thumb, &st) && st.st_size > 1000 && st.st_size < 160000, "%s: its thumbnail is missing or odd", id ? id : "?");
        count++;
        pictures += kind && !strcmp(kind, "picture");
        videos += kind && !strcmp(kind, "video");
    }
    printf("shipped catalog: %d backgrounds (%d pictures, %d videos), every entry accepted, licenses allowed\n", count, pictures, videos);
    json_free(&d);
}

int main(void) {
    test_find();
    test_actions();
    test_shipped();
    printf("studio-bg-admin-test: %s\n", bad ? "FAILED" : "ok");
    return bad != 0;
}
