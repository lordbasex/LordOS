/* hands_test.c — Hands (docs/HANDS.md): lord-server's policy (src/hands/hands.c), org.lordos.Hands1's arguments and its
 * callers (src/ui/handsbus.c), and the screenshot's fit (src/hands/handsimg.c). Built under ASan and UBSan by make test.
 *   - pure: client names, kinds, texts, refs, ids, paths, key chords (the physical keyboard's own refused), screenshot
 *     targets, rate buckets, the log's closed words, the screenshot's size;
 *   - the policy against a fake host: off, the prompt, a grant per connection (a second connection with the same name
 *     asks again; the verified program kept), Deny, Stop, the panic key, the person's input winning, the system layer,
 *     a covered ref, set-value's gate (a password field, a denied node, a covered ref), keys into a password field, an
 *     allowlisted screenshot (a region must lie inside an allowed window; other apps blacked out), rate limits;
 *   - a fuzz of every method's arguments (random signatures and bodies through dbus_msg_parse, handsbus_parse and the
 *     policy);
 *   - on a private dbus-daemon: a stand-in lord-server serving handsbus.c: the same uid served with the caller's real
 *     program, another uid refused with AccessDenied before anything runs.
 * Never touches the session's bus. */
#include "hands/hands.h"
#include "hands/handsimg.h"
#include "hands/handsmark.h"
#include "hands/mcp.h"
#include "sec/broker.h"
#include "ui/handsbus.h"
#include "vault/dbus.h"

#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails, passes;
#define CHECK(c, ...) do { if (c) passes++; else { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* handsbus.c's getuid, so the bus part can play another user (-Dgetuid=hands_test_getuid) */
static int uid_delta;
uid_t hands_test_getuid(void);
uid_t hands_test_getuid(void) { return (uid_t)(syscall(SYS_getuid) + uid_delta); }

/* ---- a fake host ---- */

static double clock_ms = 100000;
static int dialog_open, dialog_node_why, focus_secure_after, focus_calls;
static int place_why, node_why, same, keys_why, focus_secure, region_inside, ref_secure, black_n;
static int moves, buttons, keys, typed, shots, setvalues, last_black;
static char last_event[512];
static char *shell(const char *op, const char *json) {
    (void)json;
    char b[256];
    if (!strcmp(op, "resolve")) snprintf(b, sizeof b, "{\"node\":7,\"x\":100,\"y\":100,\"w\":40,\"h\":20,\"secure\":%s}", ref_secure ? "true" : "false");
    else if (!strcmp(op, "focus") && focus_secure_after && ++focus_calls > focus_secure_after) snprintf(b, sizeof b, "{\"secure\":true,\"allowed\":true}");
    else if (!strcmp(op, "focus")) snprintf(b, sizeof b, "{\"secure\":%s,\"allowed\":true,\"nodes\":[55],\"dialog\":%s,\"dialogNode\":%d}",
                                             focus_secure ? "true" : "false", dialog_open == 1 ? "true" : "false", dialog_open ? 77 : 0);
    else if (!strcmp(op, "target")) snprintf(b, sizeof b, "{\"allowed\":true}");
    else if (!strcmp(op, "region")) snprintf(b, sizeof b, "{\"inside\":%s}", region_inside ? "true" : "false");
    else if (!strcmp(op, "blackout")) snprintf(b, sizeof b, black_n ? "{\"nodes\":[41,42]}" : "{\"nodes\":[]}");
    else if (!strcmp(op, "window")) snprintf(b, sizeof b, "{\"node\":9,\"x\":10,\"y\":10,\"w\":300,\"h\":200}");
    else if (!strcmp(op, "setvalue")) { setvalues++; snprintf(b, sizeof b, "{\"ok\":true}"); }
    else if (!strcmp(op, "read")) snprintf(b, sizeof b, "{\"text\":\"window \\\"Notes\\\" [ref_1]\"}");
    else snprintf(b, sizeof b, "{\"text\":\"[]\"}");
    return strdup(b);
}
static void event(const char *j) { snprintf(last_event, sizeof last_event, "%s", j); }
static void size(int *w, int *h) { *w = 1280; *h = 800; }
static int plocked(void) { return 0; }
static int place(double x, double y, int *node) { (void)x; *node = y < 30 ? 99 : 7; return y < 30 ? HANDS_WHY_SYSTEM : place_why; }
static int same_target(int hit, int ref) { return same && hit == ref; }
static int node_place(int node) { return node == 77 ? dialog_node_why : node_why; }
static int keys_place(void) { return keys_why; }
static void move(double x, double y) { (void)x; (void)y; moves++; }
static void button(int b, int d) { (void)b; (void)d; buttons++; }
static void wheel(int dx, int dy) { (void)dx; (void)dy; }
static int key_refuse;
static int key(const HandsChord *c) { (void)c; if (key_refuse) return -2; keys++; return 0; }
static int type(const char *t, size_t n) { (void)t; typed += (int)n; return 0; }
static uint8_t *shot(int kind, uint32_t display, int node, double x, double y, double w, double h, int maxdim,
                     const int *black, int nblack, size_t *len, int *iw, int *ih, char *meta, size_t metan) {
    (void)kind; (void)display; (void)node; (void)x; (void)y; (void)w; (void)h; (void)maxdim;
    shots++;
    last_black = nblack == 2 && black[0] == 41 && black[1] == 42 ? 2 : nblack;
    *len = 4;
    *iw = *ih = 1;
    snprintf(meta, metan, "{}");
    return (uint8_t *)strdup("PNG");
}
static double now(void) { return clock_ms; }
static int clip_secret;
static int clip_sensitive(void) { return clip_secret; }
static const HandsHost host = { shell, event, size, plocked, place, same_target, node_place, keys_place, move, button, wheel, key, type, shot, now, clip_sensitive };

static int call(const char *bus, HandsReq *r, int *why) {
    char *out = NULL;
    uint8_t *png = NULL;
    size_t n = 0;
    int rc = hands_call(bus, r, &out, &png, &n, why);
    free(out);
    free(png);
    clock_ms += 1000;               /* past every rate limit */
    return rc;
}

static void pure(void) {
    char n[HANDS_NAME_MAX + 1];
    CHECK(!hands_clean_name("  Claude\x1b[31m Code\xe2\x80\xae  ", n, sizeof n) && !strcmp(n, "Claude [31m Code"), "a name cleaned: [%s]", n);
    CHECK(hands_clean_name("\x01\x02 ", n, sizeof n) == -1, "an empty name refused");
    CHECK(hands_kind_of("Claude Code") == HANDS_K_CLAUDE && hands_kind_of("lumo") == HANDS_K_LUMO && hands_kind_of("x") == HANDS_K_OTHER, "kinds");
    CHECK(hands_text_ok("héllo\nworld\t!", strlen("héllo\nworld\t!"), 100, 1) && !hands_text_ok("a\nb", 3, 100, 0), "lines only where allowed");
    CHECK(!hands_text_ok("a\x1b", 2, 10, 1) && !hands_text_ok("\xc3", 1, 10, 1) && !hands_text_ok("a\xe2\x80\xae" "b", 5, 10, 1), "controls, broken UTF-8 and bidi refused");
    CHECK(hands_ref_ok("") && hands_ref_ok("ref_12") && !hands_ref_ok("ref_") && !hands_ref_ok("ref_1a") && !hands_ref_ok("ref_1234567890"), "refs");
    CHECK(hands_id_ok("files#3") && hands_id_ok("lpk:com.x") && !hands_id_ok("a b") && !hands_id_ok("../x"), "ids");
    CHECK(hands_path_ok("/home/u/a.md") && !hands_path_ok("rel") && !hands_path_ok("/home/../etc") && !hands_path_ok("/a//b") && !hands_path_ok("/a/.") , "paths");
    HandsChord c;
    CHECK(!hands_chord("ctrl+s", &c) && c.mods == HANDS_M_CTRL && !strcmp(c.ch, "s"), "ctrl+s");
    CHECK(!hands_chord("Return", &c) && c.code && !c.mods, "Return");
    CHECK(!hands_chord("ctrl+shift+Left", &c) && c.mods == (HANDS_M_CTRL | HANDS_M_SHIFT), "ctrl+shift+Left");
    CHECK(!hands_chord("super", &c) && c.code && !c.mods, "a modifier alone");
    CHECK(!hands_chord("ctrl++", &c) && !strcmp(c.ch, "+"), "the plus key");
    CHECK(hands_chord("ctrl+alt+BackSpace", &c) && hands_chord("ctrl+alt+Escape", &c) && hands_chord("ctrl+alt+F2", &c) &&
          hands_chord("ctrl+alt+Delete", &c), "the physical keyboard's own chords refused");
    CHECK(hands_chord("ctrl+", &c) && hands_chord("s+ctrl", &c) && hands_chord("ctrl+ctrl+s", &c) && hands_chord("ctrl+\x01", &c), "bad chords");
    HandsChord cs[8];
    int k = 0;
    CHECK(!hands_chords("ctrl+a Delete", cs, 8, &k) && k == 2, "two chords");
    uint32_t d;
    char w[64];
    CHECK(hands_shot_target("display:42", &d, w, sizeof w) == HANDS_SHOT_DISPLAY && d == 42, "display:42");
    CHECK(hands_shot_target("window:notes", &d, w, sizeof w) == HANDS_SHOT_WINDOW && !strcmp(w, "notes"), "window:notes");
    CHECK(!hands_shot_target("display:x", &d, w, sizeof w) && !hands_shot_target("screen", &d, w, sizeof w), "bad targets");
    HandsBucket b = { 0 };
    int got = 0;
    for (int i = 0; i < 50; i++) got += hands_rate_take(&b, 1000, 2, 4);
    CHECK(got == 4, "a burst of 4: %d", got);
    CHECK(hands_rate_take(&b, 1600, 2, 4) == 1, "refilled");
    char line[160];
    hands_log_line(line, sizeof line, HANDS_K_CLAUDE, HANDS_C_POINTER, HANDS_P_LEFT, HANDS_E_REFUSED, HANDS_WHY_PASSWORD);
    CHECK(!strcmp(line, "hands: claude pointer left_click refused password"), "a log line: [%s]", line);
    int ow, oh;
    hands_fit(3840, 2160, 1568, 0, &ow, &oh);
    CHECK(ow == 1568 && oh == 882, "fit %dx%d", ow, oh);
    hands_fit(100, 50, 400, 1, &ow, &oh);
    CHECK(ow == 400 && oh == 200, "zoom grows %dx%d", ow, oh);
    uint32_t px[4] = { 0xff000000, 0xffffffff, 0xff000000, 0xffffffff };
    uint8_t *rgb = hands_scale_rgb(px, 2, 2, 2, 1, 1);
    CHECK(rgb && rgb[0] == 127, "an area average: %d", rgb ? rgb[0] : -1);
    free(rgb);
}

static void policy(void) {
    hands_init(&host);
    HandsReq r = { .call = HANDS_C_READ };
    int why = 0;
    CHECK(hands_hello(":1.1", "Claude Code", "/usr/bin/lordos-mcp") == HANDS_E_OFF, "off until turned on");
    hands_serve(1, NULL, 0);
    CHECK(hands_hello(":1.1", "Claude Code", "/usr/bin/lordos-mcp") == HANDS_E_ASKING, "the prompt");
    CHECK(strstr(last_event, "\"exe\":\"/usr/bin/lordos-mcp\"") && strstr(last_event, "\"bus\":\":1.1\""), "the prompt shows the verified program: %s", last_event);
    CHECK(call(":1.1", &r, &why) == HANDS_E_ASKING, "nothing before the answer");
    hands_answer(":1.1", 1);
    CHECK(call(":1.1", &r, &why) == HANDS_OK, "granted: read");
    /* identity: the grant is the connection's, never the name's */
    CHECK(hands_hello(":1.2", "Claude Code", "/tmp/evil") == HANDS_E_ASKING, "another connection with the same name asks again");
    CHECK(call(":1.2", &r, &why) == HANDS_E_ASKING, "and gets nothing meanwhile");
    {
        char *st = hands_status_json();
        CHECK(st && strstr(st, "\"exe\":\"/tmp/evil\"") && !strstr(st, "system") && !strstr(st, "verified"), "the path as reported, no trust label: %s", st ? st : "");
        free(st);
    }
    hands_answer("Claude Code", 1);
    CHECK(call(":1.2", &r, &why) == HANDS_E_ASKING, "an answer by name grants nothing");
    hands_answer(":1.2", 0);
    CHECK(call(":1.2", &r, &why) == HANDS_E_DENIED, "Deny");
    CHECK(call(":1.9", &r, &why) == HANDS_E_DENIED, "no Hello, nothing");

    /* the person's input wins */
    hands_person_input();
    HandsReq p = { .call = HANDS_C_POINTER, .action = HANDS_P_LEFT, .x = 200, .y = 200 };
    CHECK(call(":1.1", &p, &why) == HANDS_E_ACTIVE, "user active");
    clock_ms += 4000;
    same = 1;
    CHECK(call(":1.1", &p, &why) == HANDS_OK && buttons == 2, "a click: %d", buttons);
    p.y = 10;
    CHECK(call(":1.1", &p, &why) == HANDS_E_REFUSED && why == HANDS_WHY_SYSTEM, "the system layer refused (a permission prompt)");
    p.y = 200;
    snprintf(p.ref, sizeof p.ref, "ref_3");
    same = 0;
    CHECK(call(":1.1", &p, &why) == HANDS_E_REFUSED && why == HANDS_WHY_OBSCURED, "a covered ref refused");
    same = 1;
    node_why = HANDS_WHY_PROTECTED;
    CHECK(call(":1.1", &p, &why) == HANDS_E_REFUSED && why == HANDS_WHY_PROTECTED, "a ref's own protected node refused");
    node_why = 0;

    /* set-value: the same gate as a click */
    HandsReq sv = { .call = HANDS_C_SETVALUE, .text = "hi", .text_len = 2 };
    snprintf(sv.ref, sizeof sv.ref, "ref_3");
    ref_secure = 1;
    CHECK(call(":1.1", &sv, &why) == HANDS_E_REFUSED && why == HANDS_WHY_PASSWORD && !setvalues, "set-value refuses a password field in C");
    ref_secure = 0;
    node_why = HANDS_WHY_DENIED_NODE;
    CHECK(call(":1.1", &sv, &why) == HANDS_E_REFUSED && why == HANDS_WHY_DENIED_NODE && !setvalues, "set-value refuses a handsDeny node");
    node_why = 0;
    same = 0;
    CHECK(call(":1.1", &sv, &why) == HANDS_E_REFUSED && why == HANDS_WHY_OBSCURED && !setvalues, "set-value refuses a covered ref");
    same = 1;
    CHECK(call(":1.1", &sv, &why) == HANDS_OK && setvalues == 1, "set-value");

    /* keys into a password field */
    HandsReq ty = { .call = HANDS_C_TYPE, .text = "secret", .text_len = 6 };
    focus_secure = 1;
    CHECK(call(":1.1", &ty, &why) == HANDS_E_REFUSED && why == HANDS_WHY_PASSWORD && !typed, "typing into a password field refused");
    focus_secure = 0;
    HandsReq ty2 = { .call = HANDS_C_TYPE, .text = "ab\tcd", .text_len = 5 };
    CHECK(call(":1.1", &ty2, &why) == HANDS_OK && typed == 4 && keys == 1, "a Tab typed as a key between the parts");
    /* the shell said nothing, but the node with the keyboard focus is in a handsDeny or protected node: refused natively */
    node_why = HANDS_WHY_DENIED_NODE;
    CHECK(call(":1.1", &ty, &why) == HANDS_E_REFUSED && why == HANDS_WHY_DENIED_NODE, "keys to a denied focused node refused in C");
    node_why = HANDS_WHY_PROTECTED;
    CHECK(call(":1.1", &ty, &why) == HANDS_E_REFUSED && why == HANDS_WHY_PASSWORD, "keys to a protected focused node refused in C");
    node_why = 0;
    /* a dialog that owns the keyboard and is the person's (the theme-trust sheet): Enter refused */
    HandsReq enter = { .call = HANDS_C_KEY, .text = "Return", .text_len = 6 };
    dialog_open = 1;
    CHECK(call(":1.1", &enter, &why) == HANDS_E_REFUSED && why == HANDS_WHY_DIALOG, "Enter on a protected dialog refused (the shell said so)");
    dialog_open = 2;
    dialog_node_why = HANDS_WHY_DENIED_NODE;
    CHECK(call(":1.1", &enter, &why) == HANDS_E_REFUSED && why == HANDS_WHY_DIALOG, "Enter on a protected dialog refused (its root checked in C)");
    dialog_node_why = 0;
    int k0 = keys;
    CHECK(call(":1.1", &enter, &why) == HANDS_OK && keys == k0 + 1, "Enter on an ordinary dialog goes");
    dialog_open = 0;
    /* F2: the whole key gate before every chord: "tab space" whose Tab lands in a denied control never presses it */
    HandsReq ts = { .call = HANDS_C_KEY, .text = "Tab space", .text_len = 9 };
    int kk = keys;
    focus_secure_after = 1;
    focus_calls = 0;
    CHECK(call(":1.1", &ts, &why) == HANDS_E_REFUSED && why == HANDS_WHY_PASSWORD && keys == kk + 1, "the Space after a Tab into a denied control is refused (%d keys)", keys - kk);
    HandsReq rp = { .call = HANDS_C_KEY, .text = "Tab", .text_len = 3, .repeat = 5 };
    kk = keys;
    focus_calls = 0;
    CHECK(call(":1.1", &rp, &why) == HANDS_E_REFUSED && keys == kk + 1, "every repeat gated too (%d keys)", keys - kk);
    HandsReq tt = { .call = HANDS_C_TYPE, .text = "a\tb", .text_len = 3 };
    int ty0 = typed;
    kk = keys;
    focus_calls = 0;
    CHECK(call(":1.1", &tt, &why) == HANDS_E_REFUSED && typed == ty0 + 1 && keys == kk, "typing: the gate runs before the Tab, not only after it");
    focus_secure_after = 0;
    /* F4: no paste of a secret: Ctrl+V, Shift+Insert and a middle click refused while the clipboard holds one */
    clip_secret = 1;
    HandsReq pv = { .call = HANDS_C_KEY, .text = "ctrl+v", .text_len = 6 };
    CHECK(call(":1.1", &pv, &why) == HANDS_E_REFUSED && why == HANDS_WHY_SENSITIVE_CLIP, "ctrl+v with a secret on the clipboard refused");
    HandsReq pi = { .call = HANDS_C_KEY, .text = "shift+Insert", .text_len = 12 };
    CHECK(call(":1.1", &pi, &why) == HANDS_E_REFUSED && why == HANDS_WHY_SENSITIVE_CLIP, "shift+Insert refused");
    HandsReq mc = { .call = HANDS_C_POINTER, .action = HANDS_P_MIDDLE, .x = 200, .y = 200 };
    CHECK(call(":1.1", &mc, &why) == HANDS_E_REFUSED && why == HANDS_WHY_SENSITIVE_CLIP, "a middle click refused");
    clip_secret = 0;
    /* the host refuses by the key it resolves to (a layout where "v" isn't KEY_V, or Ctrl+Insert…) */
    key_refuse = 1;
    HandsReq odd = { .call = HANDS_C_KEY, .text = "ctrl+q", .text_len = 6 };
    CHECK(call(":1.1", &odd, &why) == HANDS_E_REFUSED && why == HANDS_WHY_SENSITIVE_CLIP, "a paste the host resolved from its key code: refused");
    key_refuse = 0;
    CHECK(call(":1.1", &pv, &why) == HANDS_OK, "ctrl+v goes when the clipboard holds no secret");
    /* fail closed: after the agent's input, security-state actions refuse until the person's own click or key */
    CHECK(hands_active() && hands_agent_owns_input(), "just after the agent's key: the agent's");
    clock_ms += 600000;
    CHECK(hands_agent_owns_input(), "time passing never gives it back");
    hands_person_input();
    CHECK(hands_agent_owns_input(), "nor the person's motion");
    hands_person_press();
    CHECK(!hands_agent_owns_input(), "the person's own click or key: theirs again");
    clock_ms += 4000;                       /* past the pause the person's input asks for */
    CHECK(call(":1.1", &pv, &why) == HANDS_OK && hands_agent_owns_input(), "the agent's key again: the agent's");
    clock_ms += 4000;
    keys_why = HANDS_WHY_PROMPT;
    CHECK(call(":1.1", &ty, &why) == HANDS_E_REFUSED && why == HANDS_WHY_PROMPT, "an authentication prompt takes no agent keys");
    keys_why = 0;

    /* an allowlisted screenshot */
    const char *allow[] = { "notes" };
    hands_serve(1, allow, 1);
    HandsReq sh = { .call = HANDS_C_SHOT, .shot = HANDS_SHOT_DISPLAY, .maxdim = 800 };
    CHECK(call(":1.1", &sh, &why) == HANDS_E_REFUSED && why == HANDS_WHY_ALLOWLIST && !shots, "the whole screen refused with an allowlist");
    HandsReq rg = { .call = HANDS_C_SHOT, .shot = HANDS_SHOT_REGION, .x = 0, .y = 100, .w = 1000, .h = 400, .maxdim = 800 };
    region_inside = 0;
    CHECK(call(":1.1", &rg, &why) == HANDS_E_REFUSED && why == HANDS_WHY_ALLOWLIST && !shots, "a region over other windows refused");
    region_inside = 1;
    black_n = 1;
    CHECK(call(":1.1", &rg, &why) == HANDS_OK && shots == 1 && last_black == 2, "a region inside an allowed window, the others blacked out");
    HandsReq ww = { .call = HANDS_C_SHOT, .shot = HANDS_SHOT_WINDOW, .maxdim = 800 };
    snprintf(ww.window, sizeof ww.window, "notes");
    CHECK(call(":1.1", &ww, &why) == HANDS_OK && shots == 2 && last_black == 2, "a window, the others blacked out");
    hands_serve(1, NULL, 0);
    black_n = 0;

    /* rate */
    int limited = 0;
    for (int i = 0; i < 60; i++) {
        char *out = NULL;
        uint8_t *png = NULL;
        size_t n = 0;
        if (hands_call(":1.1", &r, &out, &png, &n, &why) == HANDS_E_RATE) limited++;
        free(out);
        free(png);
    }
    CHECK(limited > 30, "rate limited: %d", limited);
    clock_ms += 10000;

    /* Stop, a new connection, the panic key */
    hands_stop("stop");
    CHECK(call(":1.1", &r, &why) == HANDS_E_STOPPED, "stopped");
    CHECK(hands_hello(":1.1", "Claude Code", "") == HANDS_E_STOPPED, "a stopped connection stays stopped");
    CHECK(hands_hello(":1.3", "Claude Code", "") == HANDS_E_ASKING, "a new connection asks again");
    hands_panic();
    CHECK(!hands_on() && call(":1.3", &r, &why) == HANDS_E_OFF, "the panic key turns Hands off");
    hands_qa(1);
    CHECK(hands_hello(":1.4", "QA", "") == HANDS_OK, "the QA desktop grants with no prompt");
    hands_qa(0);
    hands_lost();
}

/* random bodies for every method: nothing crashes, nothing parses that shouldn't */
static void fuzz(void) {
    static const char *const members[] = { "Hello", "Status", "Windows", "ReadWindow", "Find", "Pointer", "Scroll", "Key", "Type",
                                           "SetValue", "Screenshot", "OpenApp", "OpenPath", "CloseWindow", "AppLog", "Nope" };
    static const char *const sigs[] = { "", "s", "ss", "ssu", "ssiiii", "siisu", "su", "siiiiu", "u", "i", "as", "ay", "sss", "v" };
    static const char *const strs[] = { "", "ref_1", "left_click", "ctrl+s", "window:notes", "display", "/home/x", "all", "up",
                                        "notes", "\xff\xfe", "a\x01", "../..", "ref_99999999999", "interactive", "Return" };
    unsigned seed = 12345;
    int parsed = 0;
    hands_init(&host);
    hands_serve(1, NULL, 0);
    hands_qa(1);
    hands_hello(":9.9", "Fuzz", "");
    for (int it = 0; it < 20000; it++) {
        seed = seed * 1103515245u + 12345u;
        const char *mem = members[(seed >> 8) % (sizeof members / sizeof members[0])];
        const char *sig = sigs[(seed >> 13) % (sizeof sigs / sizeof sigs[0])];
        DBuf body;
        dw_init(&body);
        for (const char *t = sig; *t; t++) {
            seed = seed * 1103515245u + 12345u;
            if (*t == 's') dw_str(&body, strs[(seed >> 9) % (sizeof strs / sizeof strs[0])]);
            else if (*t == 'u') dw_u32(&body, (seed >> 7) % 3 ? (seed >> 11) % 40 : seed);
            else if (*t == 'i') dw_i32(&body, (int32_t)((seed >> 7) % 3 ? (seed >> 11) % 2000 : seed));
            else if (*t == 'a') { size_t m = dw_arr_begin(&body, 4); dw_str(&body, "x"); dw_arr_end(&body, m); t++; }
            else if (*t == 'v') { dw_variant(&body, "u"); dw_u32(&body, 1); }
        }
        DHdr h = { .type = DBUS_CALL, .path = HANDS_BUS_PATH, .iface = HANDS_BUS_IFACE, .member = mem, .dest = HANDS_BUS_NAME,
                   .sig = sig[0] ? sig : NULL, .serial = (uint32_t)it + 1 };
        DBuf raw;
        dw_init(&raw);
        if (dbus_build(&h, &body, &raw)) { dw_free(&raw); dw_free(&body); continue; }
        /* flip a byte of the body now and then: the strict reader refuses or the checks do */
        if (it % 7 == 0 && raw.n > 40) raw.p[16 + (seed % (raw.n - 16))] ^= (uint8_t)(seed >> 3);
        DMsg m;
        if (!dbus_msg_parse(raw.p, raw.n, &m)) {
            HandsReq r;
            if (!handsbus_parse(&m, &r)) {
                parsed++;
                int why = 0;
                char *out = NULL;
                uint8_t *png = NULL;
                size_t n = 0;
                if (r.call != HANDS_C_HELLO) hands_call(":9.9", &r, &out, &png, &n, &why);
                free(out);
                free(png);
                clock_ms += 1000;
            }
            dbus_msg_free(&m);
        }
        dw_free(&raw);
        dw_free(&body);
    }
    CHECK(parsed > 500, "the fuzz parsed some good calls: %d", parsed);
    hands_qa(0);
    hands_lost();
}

/* ---- on a private bus ---- */

static char status_path[PATH_MAX];
static void stand_in(const char *addr, int ready) {
    setenv("DBUS_SESSION_BUS_ADDRESS", addr, 1);
    hands_init(&host);
    hands_serve(1, NULL, 0);
    DConn c;
    int fd = dbus_connect_session();
    if (fd < 0 || dbus_open_fds(&c, fd, 3000)) _exit(1);      /* as the portal's: ProcessFD comes as an fd */
    DBuf b;
    dw_init(&b);
    dw_str(&b, HANDS_BUS_NAME);
    dw_u32(&b, 4);
    DHdr h = { .type = DBUS_CALL, .path = "/org/freedesktop/DBus", .iface = "org.freedesktop.DBus", .member = "RequestName",
               .dest = "org.freedesktop.DBus", .sig = "su" };
    DMsg m;
    if (dbus_wait_reply(&c, dbus_send(&c, &h, &b), &m, 3000) != 1) _exit(1);
    dbus_msg_free(&m);
    dw_free(&b);
    /* the portal's name too, as on the real connection: a call to it must never reach Hands */
    dw_init(&b);
    dw_str(&b, "org.freedesktop.portal.Desktop");
    dw_u32(&b, 4);
    if (dbus_wait_reply(&c, dbus_send(&c, &h, &b), &m, 3000) != 1) _exit(1);
    dbus_msg_free(&m);
    dw_free(&b);
    if (write(ready, "r", 1) != 1) _exit(1);
    close(ready);
    for (;;) {
        int r;
        while ((r = dbus_next(&c, &m)) == 1) {
            if ((m.type == DBUS_RETURN || m.type == DBUS_ERROR) && m.reply_serial) handsbus_reply(&c, &m);
            else if (m.type == DBUS_CALL && m.path && !strcmp(m.path, HANDS_BUS_PATH)) handsbus_call(&c, &m);
            dbus_msg_free(&m);
            char *st = hands_status_json();
            FILE *f = st ? fopen(status_path, "w") : NULL;
            if (f) { fputs(st, f); fclose(f); }
            free(st);
        }
        if (r < 0 || dbus_flush(&c) < 0) _exit(0);
        struct timespec ts = { 0, 5000000 };
        nanosleep(&ts, NULL);
    }
}

static int hello_to(DConn *c, DMsg *m, const char *dest) {
    DBuf q;
    dw_init(&q);
    dw_str(&q, "Claude Code");
    DHdr h = { .type = DBUS_CALL, .path = HANDS_BUS_PATH, .iface = HANDS_BUS_IFACE, .member = "Hello", .dest = dest, .sig = "s" };
    int r = dbus_wait_reply(c, dbus_send(c, &h, &q), m, 3000);
    dw_free(&q);
    return r;
}
static char portal_err[128];
static int hello(DConn *c, DMsg *m) {
    /* first through the portal's name (a Flatpak's proxy lets that one through): refused before anything */
    DMsg pm;
    int pr = hello_to(c, &pm, "org.freedesktop.portal.Desktop");
    snprintf(portal_err, sizeof portal_err, "%s", pr == 0 && pm.error ? pm.error : "answered");
    if (pr >= 0) dbus_msg_free(&pm);
    return hello_to(c, m, HANDS_BUS_NAME);
}

static void bus_round(const char *dir, int delta, int *rc, char *state, size_t n, char *err, size_t en) {
    char conf[PATH_MAX], sock[PATH_MAX], addr[PATH_MAX + 16];
    snprintf(conf, sizeof conf, "%s/bus.conf", dir);
    snprintf(sock, sizeof sock, "%s/bus", dir);
    snprintf(addr, sizeof addr, "unix:path=%s", sock);
    snprintf(status_path, sizeof status_path, "%s/status.json", dir);
    unlink(sock);
    unlink(status_path);
    FILE *f = fopen(conf, "w");
    if (!f) return;
    fprintf(f, "<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN\" "
               "\"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n"
               "<busconfig><type>session</type><listen>%s</listen><auth>EXTERNAL</auth>"
               "<policy context=\"default\"><allow send_destination=\"*\"/><allow receive_sender=\"*\"/><allow own=\"*\"/><allow user=\"*\"/></policy></busconfig>\n", addr);
    fclose(f);
    pid_t daemon = fork();
    if (!daemon) {
        int null = open("/dev/null", O_RDWR);
        dup2(null, 1);
        char arg[PATH_MAX + 16];
        snprintf(arg, sizeof arg, "--config-file=%s", conf);
        execl("/usr/bin/dbus-daemon", "dbus-daemon", arg, "--nofork", "--nopidfile", (char *)NULL);
        _exit(127);
    }
    for (int i = 0; i < 100 && access(sock, F_OK); i++) { struct timespec ts = { 0, 20000000 }; nanosleep(&ts, NULL); }
    int rp[2];
    if (pipe(rp)) return;
    pid_t srv = fork();
    if (!srv) { close(rp[0]); uid_delta = delta; stand_in(addr, rp[1]); _exit(0); }
    close(rp[1]);
    char ch;
    CHECK(read(rp[0], &ch, 1) == 1, "the stand-in owns " HANDS_BUS_NAME);
    close(rp[0]);
    setenv("DBUS_SESSION_BUS_ADDRESS", addr, 1);
    DConn c;
    int fd = dbus_connect_session();
    *rc = -2;
    if (fd >= 0 && !dbus_open(&c, fd, 3000)) {
        DMsg m;
        *rc = hello(&c, &m);
        if (*rc == 1) { DRd d; dr_init(&d, &m); snprintf(state, n, "%s", dr_str(&d, NULL)); }
        if (*rc == 0) snprintf(err, en, "%s", m.error ? m.error : "");
        if (*rc >= 0) dbus_msg_free(&m);
        dbus_close(&c);
    }
    kill(srv, SIGTERM);
    kill(daemon, SIGTERM);
    waitpid(srv, NULL, 0);
    waitpid(daemon, NULL, 0);
    unlink(conf);
    unlink(sock);
}

static void on_bus(void) {
    if (access("/usr/bin/dbus-daemon", X_OK)) { printf("hands-test: bus part skipped: needs dbus-daemon\n"); return; }
    char dir[] = "/tmp/hands-test-XXXXXX";
    if (!mkdtemp(dir)) return;
    int rc;
    char state[64] = "", err[128] = "";
    bus_round(dir, 0, &rc, state, sizeof state, err, sizeof err);
    CHECK(rc == 1 && !strcmp(state, "asking"), "the same user: Hello answered (the prompt): rc %d [%s] %s", rc, state, err);
    CHECK(!strcmp(portal_err, "org.freedesktop.DBus.Error.UnknownObject"), "Hands1 through the portal's name: refused (%s)", portal_err);
    /* the program that opened the connection, from its pidfd: this test itself, in the build folder (no system one) */
    char self[PATH_MAX], st[2048] = "";
    ssize_t l = readlink("/proc/self/exe", self, sizeof self - 1);
    if (l > 0) self[l] = 0;
    FILE *f = fopen(status_path, "r");
    if (f) { size_t k = fread(st, 1, sizeof st - 1, f); st[k] = 0; fclose(f); }
    unlink(status_path);
    char want[PATH_MAX + 32];
    snprintf(want, sizeof want, "\"exe\":\"%s\"", self);
    if (strstr(st, "\"exe\":\"\"")) printf("hands-test: this dbus-daemon gives no ProcessFD: the program stays unknown\n");
    else CHECK(strstr(st, want) != NULL, "the caller's program through its pidfd: %s", st);
    state[0] = err[0] = 0;
    bus_round(dir, 1, &rc, state, sizeof state, err, sizeof err);
    CHECK(rc == 0 && !strcmp(err, HANDS_ERR_ACCESS), "another user refused before anything runs: rc %d [%s]", rc, err);
    rmdir(dir);
}

/* the program path of a pidfd (for the prompt, reported): this process, one that ended */
static void pidfds(void) {
    char exe[HANDS_EXE_MAX], self[PATH_MAX];
    int me = (int)syscall(SYS_pidfd_open, getpid(), 0);
    ssize_t l = readlink("/proc/self/exe", self, sizeof self - 1);
    if (l > 0) self[l] = 0;
    CHECK(me >= 0 && !handsbus_exe_of_pidfd(me, exe, sizeof exe) && !strcmp(exe, self), "this test's program path: [%s]", exe);
    close(me);
    pid_t p = fork();
    if (!p) { pause(); _exit(0); }
    int fd = (int)syscall(SYS_pidfd_open, p, 0);
    kill(p, SIGKILL);
    waitpid(p, NULL, 0);
    CHECK(fd >= 0 && handsbus_exe_of_pidfd(fd, exe, sizeof exe) == -1 && !exe[0], "a process that ended: nothing (its grant ends with it)");
    if (fd >= 0) close(fd);
    CHECK(handsbus_exe_of_pidfd(-1, exe, sizeof exe) == -1, "no pidfd, no path");
}

/* the screenshot's marks: fail closed */
static Node nodes[1200];
static Node *get(int id) { return id > 0 && id < 1200 ? &nodes[id] : NULL; }
static void add(Node *p, Node *c) { c->parent = p; if (p->last) { p->last->next = c; c->prev = p->last; } else p->first = c; p->last = c; }
static void marks(void) {
    for (int i = 0; i < 1200; i++) { memset(&nodes[i], 0, sizeof nodes[i]); nodes[i].id = i; }
    Node *root = &nodes[1], *base = &nodes[2], *sys = &nodes[3];
    add(root, base);
    add(root, sys);
    sys->s.z_index = 100;
    add(sys, &nodes[4]);                    /* a prompt */
    add(base, &nodes[5]);
    add(&nodes[5], &nodes[6]);
    nodes[6].hands_deny = 1;                /* Settings → AI → Hands */
    add(base, &nodes[7]);                   /* another app's window */
    int marked[HANDS_MARK_MAX + HANDS_BLACK_MAX], n = 0, black[] = { 7 };
    CHECK(!hands_mark_nodes(root, get, black, 1, marked, 8, &n) && n == 3 && nodes[4].protect && nodes[6].protect && nodes[7].protect && !nodes[5].protect,
          "the prompt, the denied node and the other window marked: %d", n);
    hands_unmark_nodes(get, marked, n);
    CHECK(!nodes[4].protect && !nodes[6].protect && !nodes[7].protect, "and put back");
    int unknown[] = { 5000 };
    CHECK(hands_mark_nodes(root, get, unknown, 1, marked, 8, &n) == -1 && !nodes[4].protect, "a window that can't be found to black out: refused");
    /* more prompts than room: nothing stays marked, the capture is refused */
    for (int i = 100; i < 1100; i++) add(sys, &nodes[i]);
    CHECK(hands_mark_nodes(root, get, black, 1, marked, HANDS_MARK_MAX, &n) == -1 && n == 0, "too many to hide: refused");
    int any = 0;
    for (int i = 0; i < 1200; i++) any |= nodes[i].protect;
    CHECK(!any, "and every mark undone");
}


/* lordos-mcp's pure parts: the tools, the errors' sentences, a screenshot's points, base64, the GIF */
/* F1, F5 and the rest: the security-state natives an agent's input never sets off */
static void native_guard(void) {
    const char *refused[] = { "keyring.request", "vault.request", "vault.fidoAttach", "term.fill", "term.saveSecret", "fw.setAppRule", "fw.addRule", "fw.setMode",
                              "fw.removeList", "perms.set", "admin.request", "sentinela.unblock", "themes.trust", "themes.remove",
                              "hands.serve", "hands.answer", "share.answer", "session.power" };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) CHECK(hands_native_guard(refused[i]) == HANDS_G_REFUSE, "lord.%s refused to an agent", refused[i]);
    CHECK(hands_native_guard("clipboard.get") == HANDS_G_CLIP, "lord.clipboard.get: empty while a secret");
    const char *free_[] = { "fs.list", "term.write", "fw.status", "fw.rules", "perms.get", "vaultx.y", "vault.poll", "vault.available", "admin.available", "hands.status", "themes.list", "term.fillx" };
    for (size_t i = 0; i < sizeof free_ / sizeof free_[0]; i++) CHECK(hands_native_guard(free_[i]) == HANDS_G_NONE, "lord.%s left alone", free_[i]);
    /* the broker: an isolated app's keyring, FIDO key and secret to the clipboard refuse on an agent's input, the
     * clipboard's secret while an agent is connected */
    CHECK(brokersrv_hands_refuses(BOP_KEYRING, 1, 1, 0) && brokersrv_hands_refuses(BOP_VAULT_FIDO, 1, 1, 0) &&
          brokersrv_hands_refuses(BOP_CLIP_SECRET, 1, 1, 0), "the broker refuses the keyring, the FIDO key and a secret on an agent's input");
    CHECK(!brokersrv_hands_refuses(BOP_KEYRING, 0, 1, 0), "the person's own keyring use goes, an agent connected or not");
    CHECK(brokersrv_hands_refuses(BOP_CLIP_GET, 0, 1, 1) && !brokersrv_hands_refuses(BOP_CLIP_GET, 0, 0, 1) && !brokersrv_hands_refuses(BOP_CLIP_GET, 1, 1, 0),
          "the clipboard's secret to an app: never while an agent is connected; its ordinary text: yes");
    CHECK(handsbus_sandboxed(-1) && handsbus_sandboxed(0), "an unknown caller is taken as sandboxed");
    CHECK(!handsbus_sandboxed(getpid()), "this test is not sandboxed");
    CHECK(hands_paste_chord(&(HandsChord){ .mods = HANDS_M_CTRL | HANDS_M_SHIFT, .ch = "v" }) && !hands_paste_chord(&(HandsChord){ .mods = HANDS_M_CTRL, .ch = "c" }),
          "paste chords");
}

static void mcp(void) {
    CHECK(strstr(mcp_tools_json(), "\"read_window\"") && strstr(mcp_tools_json(), "\"computer\"") && !strstr(mcp_tools_json(), "javascript"),
          "the tools, and none that runs code");
    char b[256];
    CHECK(strstr(mcp_error_text(HANDS_ERR_ACTIVE, NULL, b, sizeof b), "user is active"), "user active");
    CHECK(strstr(mcp_error_text(HANDS_ERR_REFUSED, "password", b, sizeof b), "(password)"), "a refusal's word");
    CHECK(!strstr(mcp_error_text(HANDS_ERR_REFUSED, "a b\x01", b, sizeof b), "\x01"), "a word that isn't one is not echoed");
    McpShot s = { 0 };
    mcp_shot_from_json(&s, "{\"x\":100,\"y\":50,\"w\":1920,\"h\":1080,\"width\":1568,\"height\":882}");
    double x, y;
    mcp_map_point(&s, 784, 441, &x, &y);
    CHECK(s.valid && x > 1059 && x < 1061 && y > 589 && y < 591, "a screenshot's pixel back to the desktop: %.1f %.1f", x, y);
    char *e = mcp_base64((const uint8_t *)"hands", 5);
    CHECK(e && !strcmp(e, "aGFuZHM="), "base64 [%s]", e ? e : "");
    free(e);
    uint8_t rgb[16 * 8 * 3];
    for (int i = 0; i < (int)sizeof rgb; i++) rgb[i] = (uint8_t)(i * 7);
    McpFrame f[2] = { { rgb, 16, 8 }, { rgb, 16, 8 } };
    size_t n = 0;
    uint8_t *g = mcp_gif(f, 2, 50, &n);
    CHECK(g && n > 800 && !memcmp(g, "GIF89a", 6) && g[n - 1] == 0x3b, "a GIF of two frames");
    free(g);
    /* the export: a bare name in ~/Pictures/LordOS QA only, a new file, never through a link */
    CHECK(mcp_export_name_ok("qa run 1.gif") && !mcp_export_name_ok("../x.gif") && !mcp_export_name_ok("a/b.gif") &&
          !mcp_export_name_ok(".hidden.gif") && !mcp_export_name_ok("x.desktop") && !mcp_export_name_ok("/tmp/x.gif") &&
          !mcp_export_name_ok("a..gif"), "export names");
    char home[] = "/tmp/hands-export-XXXXXX", path[PATH_MAX + 128], sneaky[PATH_MAX];
    if (mkdtemp(home)) {
        CHECK(!mcp_write_export(home, "run.gif", (const uint8_t *)"GIF89a", 6, path, sizeof path) && strstr(path, "/Pictures/LordOS QA/run.gif"),
              "written in ~/Pictures/LordOS QA: %s", path);
        CHECK(mcp_write_export(home, "run.gif", (const uint8_t *)"x", 1, path, sizeof path) == -1, "never over an existing file");
        CHECK(mcp_write_export(home, "../../x.gif", (const uint8_t *)"x", 1, path, sizeof path) == -1, "never a path");
        snprintf(sneaky, sizeof sneaky, "%s/Pictures/LordOS QA/link.gif", home);
        CHECK(!symlink("/tmp/hands-export-target.gif", sneaky) && mcp_write_export(home, "link.gif", (const uint8_t *)"x", 1, path, sizeof path) == -1 &&
              access("/tmp/hands-export-target.gif", F_OK), "never through a link");
        unlink(sneaky);
        snprintf(sneaky, sizeof sneaky, "%s/Pictures/LordOS QA/run.gif", home);
        unlink(sneaky);
        snprintf(sneaky, sizeof sneaky, "%s/Pictures/LordOS QA", home);
        rmdir(sneaky);
        /* the folder itself a link (to ~/.config/autostart, say): refused */
        char target[PATH_MAX];
        snprintf(target, sizeof target, "%s/elsewhere", home);
        mkdir(target, 0700);
        CHECK(!symlink(target, sneaky) && mcp_write_export(home, "x.gif", (const uint8_t *)"x", 1, path, sizeof path) == -1, "never into a linked folder");
        unlink(sneaky);
        rmdir(target);
        snprintf(sneaky, sizeof sneaky, "%s/Pictures", home);
        rmdir(sneaky);
        rmdir(home);
    }
    char *q = mcp_quote("a\"b\n\x01");
    CHECK(q && !strcmp(q, "\"a\\\"b\\n\\u0001\""), "JSON quoting [%s]", q ? q : "");
    free(q);
}

int main(void) {
    pure();
    policy();
    fuzz();
    on_bus();
    pidfds();
    marks();
    mcp();
    native_guard();
    printf("hands-test: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
