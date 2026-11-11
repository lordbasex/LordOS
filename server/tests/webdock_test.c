/* webdock_test.c — what a web app puts on the Dock (docs/WEBAPPS.md):
 *   - SetBadge's checks (src/ui/webbadge.c): a new sender is asked about first and its count waits; only the bus's
 *     answer that it is that app and profile's lord-web shows it; a sender is one app and one profile for good; one
 *     refused stays refused; counts are clamped to 0..9999; bad ids are refused; a sender that leaves takes its
 *     count; a full table makes room only from refused senders;
 *   - the icon cache (src/ui/webicon.c): names from the app id only, a PNG or an SVG checked again, the other kind
 *     removed, no link followed, on a temporary home. */
#include "../src/ui/webbadge.h"
#include "../src/ui/webicon.h"
#include "../src/web/webreg.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_badges(void) {
    static WebBadges t;
    int slot = -1, show = 0;
    CHECK(webbadge_clamp(0) == 0 && webbadge_clamp(42) == 42 && webbadge_clamp(9999) == 9999 && webbadge_clamp(10000) == 9999 &&
          webbadge_clamp(0xffffffffu) == 9999, "clamped to 0..9999");

    /* a new sender: asked about, nothing shown yet */
    CHECK(webbadge_call(&t, ":1.5", "chat", "work", 3, &slot) == WB_ASK_PID && slot >= 0, "a new sender is asked about");
    int s5 = slot;
    webbadge_asked(&t, s5, 77);
    CHECK(webbadge_call(&t, ":1.5", "chat", "work", 120000, &slot) == WB_WAIT && slot == s5 && t.b[s5].count == 9999, "while asked: kept, clamped");
    CHECK(webbadge_call(&t, ":1.5", "chat", "personal", 1, &slot) == WB_REFUSE, "the same sender for another profile");
    CHECK(webbadge_call(&t, ":1.5", "mail", "work", 1, &slot) == WB_REFUSE, "the same sender for another app");
    CHECK(webbadge_answer(&t, 78, 1, &show) == -1 && !show, "an answer to another question");
    CHECK(webbadge_answer(&t, 77, 1, &show) == s5 && show && t.b[s5].state == WB_OK, "the bus says it is chat/work's lord-web: shown");
    CHECK(webbadge_answer(&t, 77, 1, &show) == -1, "answered once");
    CHECK(webbadge_call(&t, ":1.5", "chat", "work", 4, &slot) == WB_EMIT && slot == s5 && t.b[s5].count == 4, "then shown at once");
    CHECK(webbadge_call(&t, ":1.5", "chat", "personal", 4, &slot) == WB_REFUSE, "still one profile");

    /* an impostor: refused, and stays refused */
    CHECK(webbadge_call(&t, ":1.6", "chat", "work", 9, &slot) == WB_ASK_PID, "another sender for the same app");
    int s6 = slot;
    webbadge_asked(&t, s6, 80);
    CHECK(webbadge_answer(&t, 80, 0, &show) == s6 && !show && t.b[s6].state == WB_REFUSED, "not its lord-web: refused");
    CHECK(webbadge_call(&t, ":1.6", "chat", "work", 9, &slot) == WB_REFUSE, "refused again without asking");

    /* bad arguments */
    CHECK(webbadge_call(&t, ":1.7", "Chat", "work", 1, &slot) == WB_REFUSE && webbadge_call(&t, ":1.7", "chat", "../x", 1, &slot) == WB_REFUSE &&
          webbadge_call(&t, ":1.7", "", "work", 1, &slot) == WB_REFUSE && webbadge_call(&t, "", "chat", "work", 1, &slot) == WB_REFUSE &&
          webbadge_call(&t, NULL, "chat", "work", 1, &slot) == WB_REFUSE && webbadge_call(&t, ":1.7", NULL, "work", 1, &slot) == WB_REFUSE,
          "bad ids or no sender");
    CHECK(webbadge_call(&t, ":1.7", "a2345678901234567890123456789012345678901", "work", 1, &slot) == WB_REFUSE, "an id of 41 characters");

    /* a question that could not be sent frees its slot */
    CHECK(webbadge_call(&t, ":1.8", "notes", "personal", 1, &slot) == WB_ASK_PID, "asked");
    int s8 = slot;
    webbadge_asked(&t, s8, 0);
    CHECK(t.b[s8].state == WB_NEW, "no question went out: freed");

    /* leaving the bus */
    char app[WEBBADGE_ID_MAX + 1] = "", prof[WEBBADGE_ID_MAX + 1] = "";
    CHECK(webbadge_gone(&t, ":1.5", app, prof) == 1 && !strcmp(app, "chat") && !strcmp(prof, "work"), "a shown count goes with its sender");
    CHECK(webbadge_gone(&t, ":1.6", app, prof) == 0, "a refused one had nothing shown");
    CHECK(webbadge_gone(&t, ":1.9", app, prof) == 0, "an unknown one");
    CHECK(webbadge_call(&t, ":1.5", "chat", "personal", 2, &slot) == WB_ASK_PID, "a name is checked again after it left");

    /* full: only refused senders make room */
    static WebBadges f;
    char name[32];
    for (int i = 0; i < WEBBADGE_MAX; i++) {
        snprintf(name, sizeof name, ":2.%d", i);
        webbadge_call(&f, name, "chat", "work", 1, &slot);
        webbadge_asked(&f, slot, (uint32_t)(100 + i));
        webbadge_answer(&f, (uint32_t)(100 + i), i != 3, &show);
    }
    CHECK(webbadge_call(&f, ":3.1", "chat", "work", 1, &slot) == WB_ASK_PID && slot == 3, "full: a refused sender's place is taken");
    webbadge_asked(&f, slot, 999);
    CHECK(webbadge_call(&f, ":3.2", "chat", "work", 1, &slot) == WB_REFUSE, "full of checked or asked senders: refused");
}

static int write_file(const char *p, const void *b, size_t n) {
    FILE *f = fopen(p, "wb");
    if (!f) return -1;
    size_t w = fwrite(b, 1, n, f);
    fclose(f);
    return w == n ? 0 : -1;
}

static void test_icons(void) {
    char home[] = "/tmp/webdock-test-XXXXXX";
    if (!mkdtemp(home)) { CHECK(0, "a temporary home"); return; }
    char n[64], p[PATH_MAX], q[PATH_MAX];
    CHECK(!webicon_name("whatsapp", 1, n, sizeof n) && !strcmp(n, "whatsapp.png"), "a PNG's name: %s", n);
    CHECK(!webicon_name("my-chat", 2, n, sizeof n) && !strcmp(n, "my-chat.svg"), "an SVG's name: %s", n);
    CHECK(webicon_name("../x", 1, n, sizeof n) && webicon_name("Chat", 1, n, sizeof n) && webicon_name("a.b", 1, n, sizeof n) &&
          webicon_name("chat", 3, n, sizeof n) && webicon_name("chat", 1, n, 8), "bad ids, kinds or a short buffer");
    CHECK(webicon_cached(home, "chat", p, sizeof p), "nothing cached yet");

    static const unsigned char png[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R',
                                         0, 0, 0, 64, 0, 0, 0, 64, 8, 6, 0, 0, 0, 0, 0, 0, 0 };
    static const char svg[] = "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 1 1\"><rect width=\"1\" height=\"1\"/></svg>";
    CHECK(webreg_icon_kind(png, sizeof png) == 1, "the test PNG is one");
    CHECK(!webicon_store(home, "chat", png, sizeof png, p, sizeof p), "a PNG kept");
    snprintf(q, sizeof q, "%s/.cache/lordos/web/icons/chat.png", home);
    CHECK(!strcmp(p, q), "at ~/.cache/lordos/web/icons/<app>.png: %s", p);
    struct stat st;
    CHECK(!stat(q, &st) && (st.st_mode & 0777) == 0600, "mode 0600");
    snprintf(q, sizeof q, "%s/.cache/lordos/web/icons", home);
    CHECK(!stat(q, &st) && (st.st_mode & 0777) == 0700, "its folder 0700");
    CHECK(!webicon_cached(home, "chat", p, sizeof p) && strstr(p, "/chat.png"), "found");

    CHECK(!webicon_store(home, "chat", (const unsigned char *)svg, sizeof svg - 1, p, sizeof p) && strstr(p, "/chat.svg"), "an SVG replaces it");
    snprintf(q, sizeof q, "%s/.cache/lordos/web/icons/chat.png", home);
    CHECK(access(q, F_OK) != 0, "the PNG is gone");
    CHECK(!webicon_cached(home, "chat", p, sizeof p) && strstr(p, "/chat.svg"), "the SVG is the one found");

    static const char html[] = "<html><body>not an icon</body></html>";
    static const char script[] = "<svg><script>alert(1)</script></svg>";
    CHECK(webicon_store(home, "chat", (const unsigned char *)html, sizeof html - 1, NULL, 0), "not an image: refused");
    CHECK(webicon_store(home, "chat", (const unsigned char *)script, sizeof script - 1, NULL, 0), "an SVG with a script: refused");
    CHECK(webicon_store(home, "../../etc", png, sizeof png, NULL, 0), "a path as the id");
    CHECK(webicon_store("relative", "chat", png, sizeof png, NULL, 0) && webicon_store(NULL, "chat", png, sizeof png, NULL, 0), "no real home");
    unsigned char *big = calloc(1, WEBREG_ICON_BYTES + 1);
    if (big) { memcpy(big, png, sizeof png); CHECK(webicon_store(home, "big", big, WEBREG_ICON_BYTES + 1, NULL, 0), "over 1 MB"); free(big); }

    /* a link in the cache is not an icon, and is not followed */
    snprintf(q, sizeof q, "%s/.cache/lordos/web/icons/linked.png", home);
    CHECK(!symlink("/etc/passwd", q) && webicon_cached(home, "linked", p, sizeof p), "a link is not a cached icon");
    /* a file put there that is too big is not one either */
    snprintf(q, sizeof q, "%s/.cache/lordos/web/icons/empty.png", home);
    CHECK(!write_file(q, "", 0) && webicon_cached(home, "empty", p, sizeof p), "an empty file");

    /* a profile's icon: the app's PNG with the color's dot and its initial */
    CHECK(webicon_color_rgb("orange") == 0xff9f0a && webicon_color_rgb("none") == -1 && webicon_color_rgb("pink") == -1 && webicon_color_rgb(NULL) == -1, "tag colors");
    CHECK(!webicon_profile_name("chat", "work", n, sizeof n) && !strcmp(n, "chat.work.png"), "a profile icon's name: %s", n);
    CHECK(webicon_profile_name("chat", "../w", n, sizeof n) && webicon_profile_name("ch.at", "work", n, sizeof n), "bad ids");
    unsigned char *px = webicon_compose(NULL, 0, 0x0a84ff, 'W');
    if (px) {
        const unsigned char *dot = px + ((size_t)(WEBICON_SIDE - 15) * WEBICON_SIDE + (WEBICON_SIDE - 24)) * 4;
        const unsigned char *corner = px;
        const unsigned char *ring = px + ((size_t)(WEBICON_SIDE - 15) * WEBICON_SIDE + (WEBICON_SIDE - 29)) * 4;
        CHECK(dot[0] == 0x0a && dot[1] == 0x84 && dot[2] == 0xff && dot[3] == 255, "the dot in the color: %d %d %d", dot[0], dot[1], dot[2]);
        CHECK(corner[0] == 0x8e && corner[3] == 255, "no base: a gray tile");
        CHECK(ring[0] == 255 && ring[1] == 255 && ring[2] == 255, "a white ring");
        int white = 0;
        for (int y = WEBICON_SIDE - 22; y < WEBICON_SIDE - 8; y++)
            for (int x = WEBICON_SIDE - 20; x < WEBICON_SIDE - 10; x++) white += px[((size_t)y * WEBICON_SIDE + x) * 4] == 255;
        CHECK(white > 20, "the initial is drawn (%d white pixels)", white);
        free(px);
    } else CHECK(0, "composed");
    snprintf(q, sizeof q, "%s/cfg", home);
    mkdir(q, 0700);
    CHECK(!webicon_profile_write(q, "chat", "work", NULL, 0, 0xff9f0a, 'T'), "written");
    snprintf(p, sizeof p, "%s/cfg/icons/chat.work.png", home);
    size_t got = 0;
    unsigned char *file = (unsigned char *)webreg_read_file(p, WEBREG_ICON_BYTES, &got);
    CHECK(file && webreg_icon_kind(file, got) == 1, "a PNG at icons/<app>.<profile>.png");
    free(file);
    CHECK(!webicon_profile_write(q, "chat", "work", NULL, 0, -1, 'T') && access(p, F_OK) != 0, "no color: removed");
    CHECK(webicon_profile_write(q, "chat", "../../x", NULL, 0, 0xff9f0a, 'T') && webicon_profile_write("rel", "chat", "work", NULL, 0, 1, 'T'), "bad ids or folder");

    webicon_remove(home, "chat");
    CHECK(webicon_cached(home, "chat", p, sizeof p), "removed");

    /* the cache folder itself a link: nothing written there */
    char home2[] = "/tmp/webdock-test2-XXXXXX";
    if (mkdtemp(home2)) {
        snprintf(q, sizeof q, "%s/.cache", home2);
        CHECK(!symlink(home, q) && webicon_store(home2, "chat", png, sizeof png, NULL, 0), "a ~/.cache that is a link: refused");
        unlink(q);
        rmdir(home2);
    }
    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", home);
    if (system(cmd)) {}
}

int main(void) {
    test_badges();
    test_icons();
    printf("webdock-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
