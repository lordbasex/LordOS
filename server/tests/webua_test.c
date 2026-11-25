/* webua_test.c — Chrome's Client Hints and navigator.userAgentData from one table (src/web/webua.h), the client hints
 * kept in the profile's state file (webpage.h) and the call log's lines (src/web/webcall.h).
 * Expected values: what Chrome 158 on Linux sent and answered, captured on 2026-11-25 against a local echo server. */
#include "../src/web/webua.h"
#include "../src/web/webpage.h"
#include "../src/web/webcall.h"
#include "../src/web/webreg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_version(void) {
    /* the string's major, the hints' and the script's come from WEBPAGE_CHROME_FULL */
    CHECK(atoi(WEBPAGE_CHROME_FULL) == WEBPAGE_CHROME_MAJOR, "WEBPAGE_CHROME_MAJOR %d is WEBPAGE_CHROME_FULL's major (%s)",
          WEBPAGE_CHROME_MAJOR, WEBPAGE_CHROME_FULL);
    int dots = 0;
    for (const char *s = WEBPAGE_CHROME_FULL; *s; s++) {
        if (*s == '.') dots++;
        else CHECK(*s >= '0' && *s <= '9', "the full version is digits and dots");
    }
    CHECK(dots == 3, "four numbers in the full version");
    char ua[256], want[64];
    CHECK(!webpage_user_agent(WEBREG_UA_CHROME, "x86_64", "0.9", ua, sizeof ua), "Chrome's string");
    snprintf(want, sizeof want, "Chrome/%d.0.0.0 Safari/537.36", WEBPAGE_CHROME_MAJOR);
    CHECK(strstr(ua, want) != NULL, "the string's major is the table's: %s", ua);
}

static void test_grease(void) {
    WebUaBrand b[3];
    /* Chrome 158's own list, captured */
    CHECK(!webua_brands(158, "158.0.8127.36", 0, b), "158");
    CHECK(!strcmp(b[0].brand, "Chromium") && !strcmp(b[0].version, "158"), "158: Chromium first (%s %s)", b[0].brand, b[0].version);
    CHECK(!strcmp(b[1].brand, "Not.A/Brand") && !strcmp(b[1].version, "24"), "158: Not.A/Brand 24 second (%s %s)", b[1].brand, b[1].version);
    CHECK(!strcmp(b[2].brand, "Google Chrome") && !strcmp(b[2].version, "158"), "158: Google Chrome third");
    CHECK(!webua_brands(158, "158.0.8127.36", 1, b), "158 full");
    CHECK(!strcmp(b[0].version, "158.0.8127.36") && !strcmp(b[1].version, "24.0.0.0") && !strcmp(b[2].version, "158.0.8127.36"),
          "158 full versions");
    /* other majors, as Chromium computes them: 120 -> "Not_A Brand" 8 (Chrome 120's), 124 -> "Not-A.Brand" 99 */
    CHECK(!webua_brands(120, "120.0.6099.71", 0, b), "120");
    CHECK(!strcmp(b[0].brand, "Not_A Brand") && !strcmp(b[0].version, "8") && !strcmp(b[1].brand, "Chromium") && !strcmp(b[2].brand, "Google Chrome"),
          "120: %s %s, %s, %s", b[0].brand, b[0].version, b[1].brand, b[2].brand);
    CHECK(!webua_brands(124, "124.0.6367.60", 0, b), "124");
    CHECK(!strcmp(b[0].brand, "Chromium") && !strcmp(b[1].brand, "Google Chrome") && !strcmp(b[2].brand, "Not-A.Brand") && !strcmp(b[2].version, "99"),
          "124: %s, %s, %s %s", b[0].brand, b[1].brand, b[2].brand, b[2].version);
    CHECK(webua_brands(0, "1", 0, b) && webua_brands(158, NULL, 0, b), "bad input refused");
}

static void test_headers(void) {
    char v[256];
    const char *name;
    /* the low three, always, exactly Chrome 158's */
    CHECK(webua_header(0, 0, "x86_64", &name, v, sizeof v) == 1 && !strcmp(name, "sec-ch-ua"), "sec-ch-ua");
    if (WEBPAGE_CHROME_MAJOR == 158)
        CHECK(!strcmp(v, "\"Chromium\";v=\"158\", \"Not.A/Brand\";v=\"24\", \"Google Chrome\";v=\"158\""), "sec-ch-ua value: %s", v);
    CHECK(webua_header(1, 0, "x86_64", &name, v, sizeof v) == 1 && !strcmp(name, "sec-ch-ua-mobile") && !strcmp(v, "?0"), "mobile");
    CHECK(webua_header(2, 0, "x86_64", &name, v, sizeof v) == 1 && !strcmp(name, "sec-ch-ua-platform") && !strcmp(v, "\"Linux\""), "platform");
    /* the high ones only when asked */
    for (int i = WEBUA_LOW_N; i < WEBUA_LOW_N + WEBUA_HINT_N; i++)
        CHECK(webua_header(i, 0, "x86_64", &name, v, sizeof v) == 0, "high hint %d not sent unasked", i);
    struct { unsigned bit; const char *name, *x86, *arm; } want[] = {
        { WEBUA_HINT_FULL_VERSION_LIST, "sec-ch-ua-full-version-list", NULL, NULL },
        { WEBUA_HINT_FULL_VERSION, "sec-ch-ua-full-version", "\"" WEBPAGE_CHROME_FULL "\"", "\"" WEBPAGE_CHROME_FULL "\"" },
        { WEBUA_HINT_PLATFORM_VERSION, "sec-ch-ua-platform-version", "\"\"", "\"\"" },
        { WEBUA_HINT_ARCH, "sec-ch-ua-arch", "\"x86\"", "\"arm\"" },
        { WEBUA_HINT_BITNESS, "sec-ch-ua-bitness", "\"64\"", "\"64\"" },
        { WEBUA_HINT_MODEL, "sec-ch-ua-model", "\"\"", "\"\"" },
        { WEBUA_HINT_WOW64, "sec-ch-ua-wow64", "?0", "?0" },
        { WEBUA_HINT_FORM_FACTORS, "sec-ch-ua-form-factors", "\"Desktop\"", "\"Desktop\"" },
    };
    for (size_t k = 0; k < sizeof want / sizeof want[0]; k++) {
        int i = WEBUA_LOW_N + (int)k;
        CHECK(webua_header(i, WEBUA_HINT_ALL, "x86_64", &name, v, sizeof v) == 1 && !strcmp(name, want[k].name), "%s sent when asked", want[k].name);
        if (want[k].x86) CHECK(!strcmp(v, want[k].x86), "%s on x86_64: %s", want[k].name, v);
        CHECK(webua_header(i, want[k].bit, "aarch64", &name, v, sizeof v) == 1, "%s on aarch64", want[k].name);
        if (want[k].arm) CHECK(!strcmp(v, want[k].arm), "%s on aarch64: %s", want[k].name, v);
        CHECK(!strcmp(webua_hint_name(want[k].bit), want[k].name), "the bit's name");
    }
    CHECK(webua_header(WEBUA_LOW_N, WEBUA_HINT_ALL, "x86_64", &name, v, sizeof v) == 1, "full version list");
    if (WEBPAGE_CHROME_MAJOR == 158)
        CHECK(!strcmp(v, "\"Chromium\";v=\"158.0.8127.36\", \"Not.A/Brand\";v=\"24.0.0.0\", \"Google Chrome\";v=\"158.0.8127.36\""), "full list: %s", v);
    CHECK(webua_header(-1, 0, "x86_64", &name, v, sizeof v) == -1 && webua_header(99, 0, "x86_64", &name, v, sizeof v) == -1, "bad index");
    CHECK(webua_header(0, 0, "x86_64", &name, v, 8) == -1, "short buffer");
    CHECK(!strcmp(webua_arch("x86_64"), "x86") && !strcmp(webua_arch("aarch64"), "arm") && !strcmp(webua_arch("riscv64"), ""), "arch");
    CHECK(!strcmp(webua_bitness("x86_64"), "64") && !strcmp(webua_bitness("armv7l"), "32") && !strcmp(webua_bitness("weird"), ""), "bitness");
}

static void test_accept_ch(void) {
    /* the echo server's own header, as Chrome read it */
    unsigned m = webua_accept_ch("Sec-CH-UA-Full-Version-List, Sec-CH-UA-Platform-Version, Sec-CH-UA-Arch, Sec-CH-UA-Bitness, Sec-CH-UA-Model, Sec-CH-UA-WoW64, Sec-CH-UA-Form-Factors");
    CHECK(m == (WEBUA_HINT_ALL & ~WEBUA_HINT_FULL_VERSION), "the captured Accept-CH: %#x", m);
    CHECK(webua_accept_ch("sec-ch-ua-arch") == WEBUA_HINT_ARCH, "lowercase");
    CHECK(webua_accept_ch("  Sec-CH-UA-Bitness ,,Device-Memory, Sec-CH-UA ,DPR") == WEBUA_HINT_BITNESS, "spaces, unknown names and low hints ignored");
    CHECK(webua_accept_ch("") == 0 && webua_accept_ch(NULL) == 0, "nothing");
    CHECK(webua_accept_ch("Sec-CH-UA-Arch-Extra, Sec-CH-UA-Ar") == 0, "only whole names");
}

static void test_origin(void) {
    char o[256];
    CHECK(!webua_origin("https://Web.WhatsApp.com/app?x=1#y", o, sizeof o) && !strcmp(o, "https://web.whatsapp.com"), "https: %s", o);
    CHECK(!webua_origin("https://example.org:443/", o, sizeof o) && !strcmp(o, "https://example.org"), "default port dropped");
    CHECK(!webua_origin("https://example.org:8443/", o, sizeof o) && !strcmp(o, "https://example.org:8443"), "port kept");
    CHECK(!webua_origin("http://127.0.0.1:18799/", o, sizeof o) && !strcmp(o, "http://127.0.0.1:18799"), "http on localhost");
    CHECK(!webua_origin("http://localhost/x", o, sizeof o), "localhost");
    CHECK(webua_origin("http://example.org/", o, sizeof o), "plain http: no hints");
    CHECK(webua_origin("ftp://example.org/", o, sizeof o) && webua_origin("about:blank", o, sizeof o) && webua_origin(NULL, o, sizeof o), "other schemes");
    CHECK(webua_origin("https://user@example.org/", o, sizeof o) && webua_origin("https:///x", o, sizeof o), "userinfo, no host");
    CHECK(webua_origin("https://exa mple.org/", o, sizeof o), "bad host");
}

static void test_sites(void) {
    WebUaSites s = { .n = 0 };
    CHECK(webua_sites_set(&s, "https://a.example", WEBUA_HINT_ARCH) && webua_sites_get(&s, "https://a.example") == WEBUA_HINT_ARCH, "set, get");
    CHECK(!webua_sites_set(&s, "https://a.example", WEBUA_HINT_ARCH), "same again: no change");
    CHECK(webua_sites_set(&s, "https://a.example", 0) && s.n == 0, "an empty Accept-CH forgets");
    for (int i = 0; i < WEBUA_SITES + 3; i++) {
        char o[64];
        snprintf(o, sizeof o, "https://s%d.example", i);
        webua_sites_set(&s, o, WEBUA_HINT_MODEL);
    }
    CHECK(s.n == WEBUA_SITES && !webua_sites_get(&s, "https://s0.example") && webua_sites_get(&s, "https://s18.example"), "the oldest go");
    /* kept in state.json and read back */
    WebPageState st = { .own_sound = 1 }, back;
    webua_sites_set(&st.hints, "https://web.example", WEBUA_HINT_ARCH | WEBUA_HINT_FULL_VERSION_LIST);
    char text[WEBPAGE_STATE_MAX];
    size_t n = webpage_state_write(&st, text, sizeof text);
    CHECK(n && strstr(text, "\"https://web.example\": [\"sec-ch-ua-full-version-list\", \"sec-ch-ua-arch\"]"), "written: %s", text);
    webpage_state_parse(text, n, &back);
    CHECK(back.own_sound == 1 && webua_sites_get(&back.hints, "https://web.example") == (WEBUA_HINT_ARCH | WEBUA_HINT_FULL_VERSION_LIST), "read back");
    const char *bad = "{ \"ownSound\": false, \"clientHints\": { \"http://evil.example\": [\"sec-ch-ua-arch\"], \"https://ok.example\": "
                      "[\"sec-ch-ua-arch\", \"x-other\", 3], \"https://OK2.example\": [\"sec-ch-ua-model\"], \"https://n.example\": 5 } }";
    webpage_state_parse(bad, strlen(bad), &back);
    CHECK(back.hints.n == 1 && webua_sites_get(&back.hints, "https://ok.example") == WEBUA_HINT_ARCH,
          "plain http, a non-canonical origin, unknown names and wrong types left out (%d)", back.hints.n);
    WebPageState plain = { .own_sound = 0 };
    n = webpage_state_write(&plain, text, sizeof text);
    CHECK(n && !strcmp(text, "{ \"ownSound\": false }\n"), "no hints: the file as before: %s", text);
}

static void test_script(void) {
    char *js = webua_script("F", "x86_64");
    CHECK(js && !strncmp(js, "F({", 3), "the script's call");
    if (js && WEBPAGE_CHROME_MAJOR == 158) {
        CHECK(strstr(js, "\"brands\":[{\"brand\":\"Chromium\",\"version\":\"158\"},{\"brand\":\"Not.A/Brand\",\"version\":\"24\"},{\"brand\":\"Google Chrome\",\"version\":\"158\"}]") != NULL, "brands: %s", js);
        CHECK(strstr(js, "\"fullVersionList\":[{\"brand\":\"Chromium\",\"version\":\"158.0.8127.36\"},{\"brand\":\"Not.A/Brand\",\"version\":\"24.0.0.0\"}") != NULL, "full list");
    }
    CHECK(js && strstr(js, "\"architecture\":\"x86\"") && strstr(js, "\"bitness\":\"64\"") && strstr(js, "\"platform\":\"Linux\"")
          && strstr(js, "\"vendor\":\"Google Inc.\"") && strstr(js, "\"uaFullVersion\":\"" WEBPAGE_CHROME_FULL "\""), "values");
    free(js);
    js = webua_script("F", "aarch64");
    CHECK(js && strstr(js, "\"architecture\":\"arm\""), "arm");
    free(js);
    CHECK(webua_script(NULL, "x86_64") == NULL, "no script, nothing");
}

static void line_is(const char *json, const char *want) {
    char out[600];
    size_t n = webcall_line(json, strlen(json), out, sizeof out);
    if (want) CHECK(n && !strcmp(out, want), "%s -> '%s', want '%s'", json, n ? out : "(dropped)", want);
    else CHECK(!n, "%s should be dropped, got '%s'", json, out);
}

static void test_call_log(void) {
    line_is("{\"k\":\"env\",\"coi\":true,\"sab\":false,\"atomics\":true,\"rtc\":true,\"gum\":true}",
            "page: crossOriginIsolated yes, SharedArrayBuffer no, Atomics yes, RTCPeerConnection yes, getUserMedia yes");
    line_is("{\"k\":\"pc\",\"id\":1,\"ev\":\"created\"}", "peer connection 1 created");
    line_is("{\"k\":\"state\",\"id\":1,\"what\":\"connection\",\"state\":\"connected\"}", "peer connection 1: connection connected");
    line_is("{\"k\":\"state\",\"id\":2,\"what\":\"ice-connection\",\"state\":\"failed\"}", "peer connection 2: ice-connection failed");
    line_is("{\"k\":\"media\",\"id\":1,\"dir\":\"out\",\"kind\":\"video\",\"codec\":\"video/VP8\",\"w\":640,\"h\":480,\"fps\":30}",
            "peer connection 1: out video video/VP8 640x480 30 fps");
    line_is("{\"k\":\"media\",\"id\":1,\"dir\":\"in\",\"kind\":\"audio\",\"codec\":\"audio/opus\"}", "peer connection 1: in audio audio/opus");
    line_is("{\"k\":\"pair\",\"id\":1,\"local\":\"host\",\"remote\":\"relay\",\"proto\":\"udp\"}", "peer connection 1: candidate pair host -> relay over udp");
    line_is("{\"k\":\"error\",\"id\":3,\"op\":\"setRemoteDescription\",\"name\":\"OperationError\"}", "peer connection 3: setRemoteDescription failed: OperationError");
    line_is("{\"k\":\"error\",\"id\":0,\"op\":\"RTCPeerConnection\",\"name\":\"NotSupportedError\"}", "RTCPeerConnection failed: NotSupportedError");
    line_is("{\"k\":\"gum\",\"op\":\"getUserMedia\",\"audio\":true,\"video\":true,\"result\":\"granted\",\"labels\":[\"Built-in Audio Analog Stereo\",\"HD Pro Webcam C920 (046d:082d)\"]}",
            "getUserMedia audio+video: granted (Built-in Audio Analog Stereo, HD Pro Webcam C920 (046d:082d))");
    line_is("{\"k\":\"gum\",\"op\":\"getUserMedia\",\"audio\":true,\"video\":false,\"result\":\"failed\",\"name\":\"NotAllowedError\"}",
            "getUserMedia audio: failed: NotAllowedError");
    /* what is never written: anything outside the closed lists and patterns */
    line_is("{\"k\":\"state\",\"id\":1,\"what\":\"connection\",\"state\":\"192.168.1.10\"}", NULL);
    line_is("{\"k\":\"state\",\"id\":1,\"what\":\"sdp\",\"state\":\"new\"}", NULL);
    line_is("{\"k\":\"media\",\"id\":1,\"dir\":\"in\",\"kind\":\"audio\",\"codec\":\"a=candidate:1 1 udp 2122260223 10.0.0.2 51234\"}", NULL);
    line_is("{\"k\":\"error\",\"id\":1,\"op\":\"setRemoteDescription\",\"name\":\"v=0 o=- 4611 2 IN IP4 127.0.0.1\"}", NULL);
    line_is("{\"k\":\"error\",\"id\":1,\"op\":\"eval\",\"name\":\"Error\"}", NULL);
    line_is("{\"k\":\"pair\",\"id\":1,\"local\":\"10.0.0.2\",\"remote\":\"relay\",\"proto\":\"udp\"}", NULL);
    line_is("{\"k\":\"pc\",\"id\":-1,\"ev\":\"created\"}", NULL);
    line_is("{\"k\":\"pc\",\"id\":1.5,\"ev\":\"created\"}", NULL);
    line_is("{\"k\":\"note\",\"text\":\"hello\"}", NULL);
    line_is("not json", NULL);
    line_is("[]", NULL);
    /* a label keeps no phone number, serial or markup */
    char l[64];
    webcall_label("Camera +54 9 11 5555 1234 <b>x</b>", l, sizeof l);
    CHECK(!strstr(l, "5555") && !strchr(l, '<') && !strchr(l, '+'), "label: %s", l);
    webcall_label("USB Camera SN 1234567890", l, sizeof l);
    CHECK(!strcmp(l, "USB Camera SN"), "serial dropped: '%s'", l);
    webcall_label("", l, sizeof l);
    CHECK(!l[0], "empty");
    /* client hints line; an IP host is not written */
    char out[700];
    CHECK(webcall_hints_line("https://web.example", WEBUA_HINT_ARCH | WEBUA_HINT_BITNESS, out, sizeof out)
          && !strcmp(out, "client hints asked by https://web.example: sec-ch-ua-arch, sec-ch-ua-bitness"), "hints line: %s", out);
    CHECK(webcall_hints_line("http://127.0.0.1:8080", 0, out, sizeof out) && !strcmp(out, "client hints asked by an address: none"), "ip: %s", out);
    /* the rate */
    unsigned dropped = 0;
    int ok = 0;
    for (int i = 0; i < WEBCALL_RATE + 10; i++) ok += webcall_rate(1000.0, &dropped);
    CHECK(ok == WEBCALL_RATE, "at most %d a minute (%d)", WEBCALL_RATE, ok);
    CHECK(webcall_rate(1061.0, &dropped) && dropped == 10, "a minute later, and how many were dropped (%u)", dropped);
}

int main(void) {
    test_version();
    test_grease();
    test_headers();
    test_accept_ch();
    test_origin();
    test_sites();
    test_script();
    test_call_log();
    printf("webua-test: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
