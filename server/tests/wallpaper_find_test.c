/* wallpaper_find_test.c — what the admin actions accept from the wallpapers catalog: moving wallpapers (hevc, h264, kept
 * as .mp4) and still pictures (jpeg, kept as .jpg). The extension always comes from the catalog's codec. */
#include "../src/hw/wallpaper_find.h"

#include <stdio.h>
#include <string.h>

#define H "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define BASE "{\"base\":\"https://raw.githubusercontent.com/lordbasex/x\",\"items\":["
static int bad;
static void t(const char *name, const char *json, const char *arg, int want, const char *want_ext) {
    char url[200] = "", sha[65] = "";
    unsigned long long size = 0;
    const char *ext = NULL;
    int r = hw_wallpaper_find(json, strlen(json), arg, url, sizeof url, sha, sizeof sha, &size, &ext);
    if (r != want) { printf("FAIL %s: %d\n", name, r); bad++; return; }
    if (!r && want_ext && (!ext || strcmp(ext, want_ext))) { printf("FAIL %s: extension %s\n", name, ext ? ext : "(none)"); bad++; }
}
/* one item "a" with one variant "v" */
static const char *one(char *buf, size_t n, const char *codec, const char *file, const char *size) {
    snprintf(buf, n, BASE "{\"id\":\"a\",\"variants\":[{\"name\":\"v\",\"codec\":\"%s\",\"file\":\"%s\",\"sha256\":\"" H "\",\"size\":%s}]}]}",
             codec, file, size);
    return buf;
}
int main(void) {
    const char *ok = BASE "{\"id\":\"a\",\"variants\":[{\"name\":\"1080.hevc\",\"codec\":\"hevc\",\"file\":\"a/1080.hevc.mp4\",\"sha256\":\"" H "\",\"size\":10},"
                     "{\"name\":\"1080.h264\",\"codec\":\"h264\",\"file\":\"a/1080.h264.mp4\",\"sha256\":\"" H "\",\"size\":10}]},"
                     "{\"id\":\"epic-01-king\",\"section\":\"epic\",\"variants\":[{\"name\":\"1080.jpeg\",\"codec\":\"jpeg\",\"w\":1920,\"h\":1080,\"file\":\"stills/epic/epic-01-king/1080.jpg\",\"sha256\":\"" H "\",\"size\":437074},"
                     "{\"name\":\"2160.jpeg\",\"codec\":\"jpeg\",\"w\":3840,\"h\":2160,\"file\":\"stills/epic/epic-01-king/2160.jpg\",\"sha256\":\"" H "\",\"size\":1287227}]}]}";
    t("found", ok, "a 1080.hevc", 0, "mp4");
    t("h264 is a video too", ok, "a 1080.h264", 0, "mp4");
    t("a still picture: jpg", ok, "epic-01-king 1080.jpeg", 0, "jpg");
    t("its 4K variant", ok, "epic-01-king 2160.jpeg", 0, "jpg");
    t("unknown id", ok, "b 1080.hevc", -1, NULL);
    t("unknown variant", ok, "a 2160.hevc", -1, NULL);
    t("no space", ok, "a", -1, NULL);
    t("path in the id", ok, "../a 1080.hevc", -1, NULL);
    t("slash in variant", ok, "a ../x", -1, NULL);
    t("upper case", ok, "A 1080.hevc", -1, NULL);
    t("other host", "{\"base\":\"https://example.org\",\"items\":[]}", "a 1080.hevc", -1, NULL);
    char b[600];
    t("bad hash", BASE "{\"id\":\"a\",\"variants\":[{\"name\":\"v\",\"codec\":\"hevc\",\"file\":\"f.mp4\",\"sha256\":\"zz\",\"size\":10}]}]}", "a v", -1, NULL);
    t("too big", one(b, sizeof b, "hevc", "f.mp4", "200000000"), "a v", -1, NULL);
    t("a fractional size", one(b, sizeof b, "jpeg", "f.jpg", "10.5"), "a v", -1, NULL);
    /* the kind comes from the codec, and the file must be of that kind */
    t("a jpeg whose file is an .mp4", one(b, sizeof b, "jpeg", "a/f.mp4", "10"), "a v", -1, NULL);
    t("a video whose file is a .jpg", one(b, sizeof b, "hevc", "a/f.jpg", "10"), "a v", -1, NULL);
    t("an unknown codec", one(b, sizeof b, "png", "a/f.jpg", "10"), "a v", -1, NULL);
    t("no codec", BASE "{\"id\":\"a\",\"variants\":[{\"name\":\"v\",\"file\":\"a/f.mp4\",\"sha256\":\"" H "\",\"size\":10}]}]}", "a v", -1, NULL);
    t("a jpeg named .jpeg", one(b, sizeof b, "jpeg", "a/f.jpeg", "10"), "a v", -1, NULL);
    t("a file outside the base", one(b, sizeof b, "jpeg", "../f.jpg", "10"), "a v", -1, NULL);
    t("an absolute file", one(b, sizeof b, "jpeg", "/etc/f.jpg", "10"), "a v", -1, NULL);
    t("a file with a query", one(b, sizeof b, "jpeg", "f.jpg?x=1.jpg", "10"), "a v", -1, NULL);
    t("a hidden file", one(b, sizeof b, "jpeg", ".f.jpg", "10"), "a v", -1, NULL);
    t("a double slash", one(b, sizeof b, "jpeg", "a//f.jpg", "10"), "a v", -1, NULL);
    t("upper case in the file", one(b, sizeof b, "jpeg", "A/f.jpg", "10"), "a v", -1, NULL);
    t("not json", "nope", "a v", -1, NULL);
    printf("wallpaper-find-test: %s\n", bad ? "FAILED" : "ok");
    return bad != 0;
}
