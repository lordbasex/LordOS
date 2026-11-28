/* wallpaper_admin_test.c — the closed admin actions wallpaper-get, wallpaper-remove and wallpaper-login
 * (server/src/hw/wallpaper_admin.c) against a catalog of our own: a video is kept as <sha256>.mp4, a still picture
 * (codec jpeg) as <sha256>.jpg, the extension decided from the catalog's codec; refused arguments never fetch; a file
 * whose size or hash isn't the catalog's is never kept; remove deletes either kind; the login screen takes a catalog
 * picture only as "still cat:<id>", never as a video. fetch_batch is replaced by a fake that "downloads" fixed bytes;
 * the catalog and the folder are under build/ (WALLPAPER_CATALOG, WALLPAPER_STORE). Also checks the shipped catalog
 * assets/wallpapers/motion.json: every variant is one the action accepts, every still has its thumbnail. Run from server/. */
#include "../src/hw/wallpaper_find.h"
#include "../src/hw/hw_admin.h"
#include "../src/pkg/crypto.h"
#include "../src/pkg/http.h"
#include "../src/sec/json.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
    if (nsaid == 64) nsaid = 0;
    snprintf(said[nsaid++], sizeof said[0], "%s %s", kind, line);
}
static const char *last(void) { return nsaid ? said[nsaid - 1] : ""; }

static const char *fake_body = "fake wallpaper bytes";
static int fake_status = 200, fetches;
static char fetched_url[900], fetched_dest[600];
int fetch_batch(FetchItem *items, int n, FetchProgress cb, void *ud) {
    fetches++;
    for (int i = 0; i < n; i++) {
        snprintf(fetched_url, sizeof fetched_url, "%s", items[i].url);
        snprintf(fetched_dest, sizeof fetched_dest, "%s", items[i].dest);
        FILE *f = fopen(items[i].dest, "wb");
        if (f) { fwrite(fake_body, 1, strlen(fake_body), f); fclose(f); }
        items[i].status = fake_status;
        items[i].size = strlen(fake_body);
        sha256(fake_body, strlen(fake_body), items[i].sha256);
        if (cb) cb(items[i].size, items[i].expect, ud);
    }
    return 0;
}

#define DIR "build/wallpaper-admin-files"
static void write_catalog(const char *sha, size_t size) {
    FILE *f = fopen(DIR "/catalog.json", "w");
    if (!f) { printf("FAIL: can't write " DIR "/catalog.json\n"); exit(1); }
    fprintf(f, "{\"format\":1,\"base\":\"https://raw.githubusercontent.com/lordbasex/LordOS-wallpapers/main\",\"items\":["
               "{\"id\":\"crown\",\"section\":\"dynamic\",\"variants\":[{\"name\":\"1080.hevc\",\"codec\":\"hevc\",\"w\":1920,\"h\":1080,\"file\":\"crown/1080.hevc.mp4\",\"sha256\":\"%s\",\"size\":%zu}]},"
               "{\"id\":\"epic-01-king\",\"section\":\"epic\",\"variants\":[{\"name\":\"1080.jpeg\",\"codec\":\"jpeg\",\"w\":1920,\"h\":1080,\"fps\":0.0,\"seconds\":0.0,"
               "\"file\":\"stills/epic/epic-01-king/1080.jpg\",\"sha256\":\"%s\",\"size\":%zu}]}]}",
            sha, size, sha, size);
    fclose(f);
}
static int exists(const char *p) { struct stat st; return !stat(p, &st); }

static void test_actions(void) {
    mkdir("build", 0755);
    mkdir(DIR, 0755);
    mkdir(DIR "/store", 0755);
    uint8_t h[32];
    char sha[65];
    sha256(fake_body, strlen(fake_body), h);
    for (int i = 0; i < 32; i++) snprintf(sha + 2 * i, 3, "%02x", h[i]);
    write_catalog(sha, strlen(fake_body));
    AdminReq r = { .id = 1, .user = "u", .home = "/nonexistent", .uid = 1000, .gid = 1000, .admin = 1 };
    char jpg[256], mp4[256], want[80];
    snprintf(jpg, sizeof jpg, DIR "/store/%s.jpg", sha);
    snprintf(mp4, sizeof mp4, DIR "/store/%s.mp4", sha);
    snprintf(want, sizeof want, "OK %s", sha);
    unlink(jpg);
    unlink(mp4);
    /* refused arguments never fetch */
    static const char *const refused[] = { "", "epic-01-king", "epic-01-king 2160.jpeg", "../../etc/passwd x", "epic-01-king 1080.jpeg extra",
                                           "https://evil.example/x 1080.jpeg", "/etc/shadow 1080.jpeg", "epic-01-king 1080.jpeg\n",
                                           "cat:epic-01-king 1080.jpeg", "Epic-01-king 1080.jpeg" };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        int before = fetches;
        hw_act_wallpaper_get(&r, refused[i], strlen(refused[i]));
        CHECK(fetches == before && !strncmp(last(), "ERR admin.invalidArgument", 25), "wallpaper-get \"%s\": %s", refused[i], last());
        hw_act_wallpaper_remove(&r, refused[i], strlen(refused[i]));
        CHECK(!strncmp(last(), "ERR admin.invalidArgument", 25), "wallpaper-remove \"%s\": %s", refused[i], last());
    }
    hw_act_wallpaper_get(&r, "epic-01-king 1080.jpeg\0x", 24);
    CHECK(!strncmp(last(), "ERR admin.invalidArgument", 25), "a NUL inside the argument: %s", last());
    /* a still picture: fetched from the catalog's base, kept as <sha>.jpg, never as .mp4 */
    hw_act_wallpaper_get(&r, "epic-01-king 1080.jpeg", 22);
    CHECK(!strcmp(last(), want), "wallpaper-get of a picture: %s", last());
    CHECK(!strcmp(fetched_url, "https://raw.githubusercontent.com/lordbasex/LordOS-wallpapers/main/stills/epic/epic-01-king/1080.jpg"), "url %s", fetched_url);
    struct stat st;
    CHECK(!stat(jpg, &st) && (st.st_mode & 0777) == 0644, "the picture isn't kept as <sha>.jpg 0644 (%s)", fetched_dest);
    CHECK(!exists(mp4), "a picture was kept as .mp4");
    /* again: already there, nothing fetched */
    int before = fetches;
    hw_act_wallpaper_get(&r, "epic-01-king 1080.jpeg", 22);
    CHECK(fetches == before && !strcmp(last(), want), "a second get fetched again");
    /* a file named after the catalog's hash that holds something else: at the catalog's size (same length, other bytes)
     * or not (13 bytes of text, as the ws170 QA found): fetched again over it, never answered OK as it is */
    for (int k = 0; k < 2; k++) {
        FILE *w = fopen(jpg, "w");
        if (w) { fputs(k ? "crystal-crown" : "FAKE wallpaper bytes", w); fclose(w); }
        before = fetches;
        hw_act_wallpaper_get(&r, "epic-01-king 1080.jpeg", 22);
        char got[64] = "";
        FILE *rd = fopen(jpg, "r");
        if (rd) { if (!fgets(got, sizeof got, rd)) got[0] = 0; fclose(rd); }
        CHECK(fetches == before + 1 && !strcmp(last(), want) && !strcmp(got, fake_body), "a wrong file (%s) was kept: %s, %s", k ? "short" : "same size", last(), got);
    }
    /* a video still goes to .mp4 */
    hw_act_wallpaper_get(&r, "crown 1080.hevc", 15);
    CHECK(!strcmp(last(), want) && exists(mp4), "the video isn't <sha>.mp4: %s", last());
    /* the login screen: a picture of the catalog is never linked as its video */
    hw_act_wallpaper_login(&r, "epic-01-king 1080.jpeg", 22);
    CHECK(!strncmp(last(), "ERR admin.invalidArgument", 25) && !exists(DIR "/store/login.mp4"), "a picture as the login video: %s", last());
    /* … but by its id, "still cat:<id>", kept in login-still */
    hw_act_wallpaper_login(&r, "still cat:epic-01-king", 22);
    char mark[64] = "";
    FILE *f = fopen(DIR "/store/login-still", "r");
    if (f) { if (!fgets(mark, sizeof mark, f)) mark[0] = 0; fclose(f); }
    CHECK(!strcmp(last(), "OK still") && !strcmp(mark, "cat:epic-01-king"), "still cat:<id>: %s, %s", last(), mark);
    static const char *const bad_still[] = { "still cat:", "still cat:../x", "still cat:a/b", "still cat:A", "still cat:cat:x", "still :x", "still x:y" };
    for (size_t i = 0; i < sizeof bad_still / sizeof bad_still[0]; i++) {
        hw_act_wallpaper_login(&r, bad_still[i], strlen(bad_still[i]));
        CHECK(!strncmp(last(), "ERR admin.invalidArgument", 25), "wallpaper-login \"%s\": %s", bad_still[i], last());
    }
    hw_act_wallpaper_login(&r, "still crystal-crown", 19);
    CHECK(!strcmp(last(), "OK still"), "a built-in still: %s", last());
    /* (ws170 QA BUG-13) the same login video chosen twice, then a still: rename() over the same inode left the temporary
     * name linked to the video, and the still's mark was written into it ("crystal-crown", 13 bytes, under the hash) */
    hw_act_wallpaper_login(&r, "crown 1080.hevc", 15);
    hw_act_wallpaper_login(&r, "crown 1080.hevc", 15);
    CHECK(!strcmp(last(), want) && !exists(DIR "/store/.login.part"), "the login video twice: %s, temporary left %d", last(), exists(DIR "/store/.login.part"));
    hw_act_wallpaper_login(&r, "still crystal-crown", 19);
    {
        char vb[64] = "";
        FILE *vf = fopen(mp4, "r");
        if (vf) { if (!fgets(vb, sizeof vb, vf)) vb[0] = 0; fclose(vf); }
        CHECK(!strcmp(last(), "OK still") && !strcmp(vb, fake_body), "a still after the login video wrote into the video: %s", vb);
    }
    /* the bytes differ from the catalog's hash: deleted, an error */
    unlink(jpg);
    write_catalog("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", strlen(fake_body));
    hw_act_wallpaper_get(&r, "epic-01-king 1080.jpeg", 22);
    CHECK(!strncmp(last(), "ERR wallpaper.downloadFailed", 28) && !exists(DIR "/store/0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef.jpg"),
          "a wrong hash was kept: %s", last());
    /* the server said 404: deleted */
    write_catalog(sha, strlen(fake_body));
    fake_status = 404;
    hw_act_wallpaper_get(&r, "epic-01-king 1080.jpeg", 22);
    CHECK(!strncmp(last(), "ERR wallpaper.downloadFailed", 28) && !exists(jpg), "a 404 was kept: %s", last());
    fake_status = 200;
    /* remove: the picture's .jpg, the video's .mp4 */
    hw_act_wallpaper_get(&r, "epic-01-king 1080.jpeg", 22);
    hw_act_wallpaper_remove(&r, "epic-01-king 1080.jpeg", 22);
    CHECK(!strcmp(last(), "OK removed") && !exists(jpg) && exists(mp4), "wallpaper-remove of a picture: %s", last());
    hw_act_wallpaper_remove(&r, "epic-01-king 1080.jpeg", 22);
    CHECK(!strcmp(last(), "OK removed"), "removing twice: %s", last());
    hw_act_wallpaper_remove(&r, "crown 1080.hevc", 15);
    CHECK(!strcmp(last(), "OK removed") && !exists(mp4), "wallpaper-remove of a video: %s", last());
    unlink(DIR "/store/login-still");
}

/* ---- the shipped catalog ---- */
static const JVal *get(const JVal *o, const char *k) {
    if (!o || o->type != J_OBJ) return NULL;
    for (const JVal *m = o->kid; m; m = m->next) if (!strcmp(m->key, k)) return m;
    return NULL;
}
static const char *str(const JVal *o, const char *k) { const JVal *v = get(o, k); return v && v->type == J_STR ? v->str : NULL; }

static void test_shipped(void) {
    FILE *f = fopen("../assets/wallpapers/motion.json", "rb");
    if (!f) { printf("shipped catalog: skipped: needs assets/wallpapers/motion.json\n"); return; }
    static char buf[1 << 20];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    CHECK(n < sizeof buf - 1, "the catalog is 1 MB or more: the action reads at most that");
    JDoc d;
    CHECK(!json_parse(buf, n, &d, NULL, 0), "the catalog isn't JSON");
    const JVal *items = get(d.root, "items");
    int videos = 0, stills = 0, files = 0;
    for (const JVal *it = items && items->type == J_ARR ? items->kid : NULL; it; it = it->next) {
        const char *id = str(it, "id");
        int still = 0;
        const JVal *vars = get(it, "variants");
        for (const JVal *v = vars && vars->type == J_ARR ? vars->kid : NULL; v; v = v->next) {
            char arg[128], url[900], sha[65];
            unsigned long long size = 0;
            const char *ext = NULL;
            snprintf(arg, sizeof arg, "%s %s", id ? id : "", str(v, "name") ? str(v, "name") : "");
            int rc = hw_wallpaper_find(buf, n, arg, url, sizeof url, sha, sizeof sha, &size, &ext);
            CHECK(rc == 0, "the action would refuse the shipped entry \"%s\"", arg);
            if (!rc) still |= !strcmp(ext, "jpg");
            files++;
        }
        char thumb[256];
        snprintf(thumb, sizeof thumb, "../assets/wallpapers/motion/%s.thumb.jpg", id ? id : "");
        CHECK(exists(thumb), "%s: no thumbnail", id ? id : "?");
        if (still) { stills++; CHECK(str(it, "section") && str(it, "license") && str(it, "author"), "%s: a still needs a section, a license and an author", id); }
        else videos++;
    }
    printf("shipped catalog: %d moving, %d still pictures, %d files, every one accepted\n", videos, stills, files);
    json_free(&d);
}

int main(void) {
    test_actions();
    test_shipped();
    printf("wallpaper-admin-test: %s\n", bad ? "FAILED" : "ok");
    return bad != 0;
}
