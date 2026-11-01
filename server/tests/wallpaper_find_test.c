/* wallpaper_find_test.c — what the admin actions accept from the moving wallpapers catalog. */
#include "../src/hw/wallpaper_find.h"

#include <stdio.h>
#include <string.h>

#define H "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
static int bad;
static void t(const char *name, const char *json, const char *arg, int want) {
    char url[200] = "", sha[65] = "";
    unsigned long long size = 0;
    int r = hw_wallpaper_find(json, strlen(json), arg, url, sizeof url, sha, sizeof sha, &size);
    if (r != want) { printf("FAIL %s: %d\n", name, r); bad++; }
}
int main(void) {
    const char *ok = "{\"base\":\"https://raw.githubusercontent.com/lordbasex/x\",\"items\":[{\"id\":\"a\",\"variants\":[{\"name\":\"1080.hevc\",\"file\":\"a/1080.hevc.mp4\",\"sha256\":\"" H "\",\"size\":10}]}]}";
    t("found", ok, "a 1080.hevc", 0);
    t("unknown id", ok, "b 1080.hevc", -1);
    t("unknown variant", ok, "a 2160.hevc", -1);
    t("no space", ok, "a", -1);
    t("path in the id", ok, "../a 1080.hevc", -1);
    t("slash in variant", ok, "a ../x", -1);
    t("upper case", ok, "A 1080.hevc", -1);
    t("other host", "{\"base\":\"https://example.org\",\"items\":[]}", "a 1080.hevc", -1);
    t("bad hash", "{\"base\":\"https://raw.githubusercontent.com/lordbasex/x\",\"items\":[{\"id\":\"a\",\"variants\":[{\"name\":\"v\",\"file\":\"f\",\"sha256\":\"zz\",\"size\":10}]}]}", "a v", -1);
    t("too big", "{\"base\":\"https://raw.githubusercontent.com/lordbasex/x\",\"items\":[{\"id\":\"a\",\"variants\":[{\"name\":\"v\",\"file\":\"f\",\"sha256\":\"" H "\",\"size\":200000000}]}]}", "a v", -1);
    t("not json", "nope", "a v", -1);
    printf("wallpaper-find-test: %s\n", bad ? "FAILED" : "ok");
    return bad != 0;
}
