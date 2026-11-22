/* pwguard_test.c — the PipeWire guard's pieces that don't need PipeWire (src/sec/pwguard.c): the
 * SPA POD reader on well-formed, truncated and lying input, and which nodes and streams each
 * restriction covers; the capture indicator's list and how it names what has no verified app (a process that left its
 * app's scope, a sandboxed pulse client, the sound system, an executable) from fixtures. The guard against a real
 * PipeWire: tests/fwlinux/mic.sh. */
#include "sec/pwguard.h"
#include "sec/appid.h"
#include "fw/rules.h"
#include "sec/broker.h"
#include "sec/perms.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

void brokersrv_revoke(const char *app, const char *perm) { (void)app; (void)perm; }
PermState perm_get(const char *app_id, Perm p) { (void)app_id; (void)p; return PERM_ASK; }

static int fails, total;
#define CHECK(c) do { total++; if (!(c)) { fails++; fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #c); } } while (0)

static size_t put(uint8_t *b, size_t o, uint32_t v) { memcpy(b + o, &v, 4); return o + 4; }

/* ---- who a process that left its app's scope belongs to (sec/appid.h, only to show it): a fixture process table ---- */
typedef struct { unsigned pid, ppid; const char *cg; int other_user; } FxProc;
static FxProc fx[32];
static int nfx;
static char cg_sess[128], cg_user[128];

static const FxProc *fx_find(unsigned pid) { for (int i = 0; i < nfx; i++) if (fx[i].pid == pid) return &fx[i]; return NULL; }
static int fx_cgroup(void *u, unsigned pid, char *out, size_t n) {
    (void)u;
    const FxProc *p = fx_find(pid);
    if (!p || strlen(p->cg) + 1 > n) return -1;
    memcpy(out, p->cg, strlen(p->cg) + 1);
    return 0;
}
static unsigned fx_parent(void *u, unsigned pid) { (void)u; const FxProc *p = fx_find(pid); return p ? p->ppid : 0; }
static int fx_mine(void *u, unsigned pid) { (void)u; const FxProc *p = fx_find(pid); return p && !p->other_user; }
/* as appid_session_procs: only the session's own subtree is listed */
static void fx_procs(void *u, void (*each)(void *, const char *, unsigned), void *ctx) {
    (void)u;
    for (int i = 0; i < nfx; i++) if (!strncmp(fx[i].cg, cg_sess, strlen(cg_sess))) each(ctx, fx[i].cg, fx[i].pid);
}
static char cgs[32][256];
static void fx_add(unsigned pid, unsigned ppid, const char *base, const char *rest, int other) {
    snprintf(cgs[nfx], sizeof cgs[nfx], "%s%s", base, rest);
    fx[nfx] = (FxProc){ pid, ppid, cgs[nfx], other };
    nfx++;
}
static const char *who(unsigned pid) {
    static char out[128];
    static const AppidView v = { fx_cgroup, fx_parent, fx_mine, fx_procs, NULL };
    if (appid_lineage_in(&v, pid, out, sizeof out)) return "-";
    return out;
}

static void lineage_tests(void) {
    unsigned u = (unsigned)getuid();
    snprintf(cg_sess, sizeof cg_sess, "/system.slice/lordos.service/lordos-session-%u", u);
    snprintf(cg_user, sizeof cg_user, "/user.slice/user-%u.slice/user@%u.service", u, u);
    char other_sess[128];
    snprintf(other_sess, sizeof other_sess, "/system.slice/lordos.service/lordos-session-%u", u + 1);
    nfx = 0;
    /* Chrome launched by LordOS: its zygote stays in the app's scope, the browser process moved itself into the user's
     * manager (Chromium's own systemd scope), and the audio service it started afterwards is there too */
    fx_add(1168, 1, cg_user, "/app.slice/app-com.google.Chrome-1168.scope", 0);
    fx_add(1192, 1168, cg_sess, "/app-lordos-google-chrome.scope", 0);
    fx_add(1193, 1168, cg_sess, "/app-lordos-google-chrome.scope", 0);
    fx_add(2514, 1168, cg_user, "/app.slice/app-com.google.Chrome-1168.scope", 0);
    /* the user's manager and a process it runs (systemd-run --user, D-Bus activation) */
    fx_add(900, 1, cg_user, "/init.scope", 0);
    fx_add(901, 900, cg_user, "/app.slice/run-u12.service", 0);
    /* a unit of the user's own named like an app's scope: never an app */
    fx_add(902, 900, cg_user, "/app.slice/app-lordos-google-chrome.scope", 0);
    fx_add(903, 902, cg_user, "/app.slice/app-lordos-google-chrome.scope", 0);
    /* a process that left OBS's scope while its parent stayed */
    fx_add(1300, 1, cg_sess, "/app-lordos-com.obsproject.Studio.scope", 0);
    fx_add(1301, 1300, cg_user, "/app.slice/obs-helper.scope", 0);
    /* a process with children in two apps' scopes: can't be told */
    fx_add(1400, 1, cg_user, "/app.slice/x.scope", 0);
    fx_add(1401, 1400, cg_sess, "/app-lordos-a.scope", 0);
    fx_add(1402, 1400, cg_sess, "/app-lordos-b.scope", 0);
    /* lord-server, its own helper's scope, the Terminal, a machine's QEMU */
    fx_add(1078, 1, cg_sess, "/server", 0);
    fx_add(1079, 1078, cg_sess, "/lordos-screencast.scope", 0);
    fx_add(1766, 1078, cg_sess, "/lordos-net-terminal.scope", 0);
    fx_add(6250, 1078, cg_sess, "/app-lordos-lordos-vm-1b2b4eb6-77dd-42a8-933c-3cf7eac71aa2.scope", 0);
    /* an app process that left its scope at once: its parent is lord-server, never "LordOS" for it */
    fx_add(1500, 1078, cg_user, "/app.slice/quick.scope", 0);
    /* Chrome started from the Terminal: its browser process moved out, its zygote stayed in the Terminal's scope */
    fx_add(1600, 1766, cg_user, "/app.slice/app-com.google.Chrome-1600.scope", 0);
    fx_add(1601, 1600, cg_sess, "/lordos-net-terminal.scope", 0);
    /* another user's process, another session's scope */
    fx_add(1700, 1, cg_user, "/app.slice/y.scope", 1);
    fx_add(1701, 1700, cg_sess, "/app-lordos-c.scope", 0);
    fx_add(1800, 1, cg_user, "/app.slice/z.scope", 0);
    fx_add(1801, 1800, other_sess, "/app-lordos-d.scope", 0);
    /* a parent loop (a table that lies) ends */
    fx_add(1900, 1900, cg_user, "/app.slice/loop.scope", 0);

    CHECK(!strcmp(who(1168), "desktop:google-chrome"));          /* the browser process: its zygote's scope */
    CHECK(!strcmp(who(2514), "desktop:google-chrome"));          /* the audio service: up to the browser process */
    CHECK(!strcmp(who(1192), "desktop:google-chrome"));          /* still in its scope */
    CHECK(!strcmp(who(901), "-"));                               /* the manager's own child: nobody's */
    CHECK(!strcmp(who(900), "-"));                               /* the manager itself */
    CHECK(!strcmp(who(903), "-"));                               /* a name under user@ is never trusted */
    CHECK(!strcmp(who(1301), "desktop:com.obsproject.Studio"));  /* its parent's scope */
    CHECK(!strcmp(who(1400), "-"));                              /* children in two apps */
    CHECK(!strcmp(who(1078), "lordos"));                         /* lord-server itself */
    CHECK(!strcmp(who(1079), "lordos"));                         /* its helper's scope */
    CHECK(!strcmp(who(1766), "lordos:terminal"));                /* the Terminal's scope */
    CHECK(!strcmp(who(6250), "desktop:lordos-vm-1b2b4eb6-77dd-42a8-933c-3cf7eac71aa2"));   /* a machine */
    CHECK(!strcmp(who(1500), "-"));                              /* never LordOS through a parent */
    CHECK(!strcmp(who(1600), "lordos:terminal"));                /* started from the Terminal */
    CHECK(!strcmp(who(1700), "-"));                              /* another user's process */
    CHECK(!strcmp(who(1801), "-") && !strcmp(who(1800), "-"));   /* another session's scope is nobody's here */
    CHECK(!strcmp(who(1900), "-"));                              /* a loop ends */
    CHECK(!strcmp(who(4242), "-"));                              /* gone */
    char small[8];
    static const AppidView v = { fx_cgroup, fx_parent, fx_mine, fx_procs, NULL };
    CHECK(appid_lineage_in(&v, 1168, small, sizeof small) == -1 && !small[0]);   /* doesn't fit: nothing */

    /* the owner of a session cgroup */
    char o[128], p[256];
    snprintf(p, sizeof p, "%s/app-lordos-lpk-com.x.scope", cg_sess);
    CHECK(appid_session_owner(p, o, sizeof o) == 0 && !strcmp(o, "lpk:com.x"));
    snprintf(p, sizeof p, "%s/lordos-net-nothing.scope", cg_sess);
    CHECK(appid_session_owner(p, o, sizeof o) == 2 && !strcmp(o, "lordos"));     /* not a component: LordOS's own */
    snprintf(p, sizeof p, "%s/app-lordos-bad id.scope", cg_sess);
    CHECK(appid_session_owner(p, o, sizeof o) == -1);                             /* an app-lordos-* that isn't an app */
    snprintf(p, sizeof p, "%sX/server", cg_sess);
    CHECK(appid_session_owner(p, o, sizeof o) == -1);                             /* another session's name, longer */
    CHECK(appid_session_owner(cg_user, o, sizeof o) == -1);

    /* the executable's name */
    char e[32];
    CHECK(appid_exe_base("/opt/google/chrome/chrome", e, sizeof e) == 0 && !strcmp(e, "chrome"));
    CHECK(appid_exe_base("/tmp/rec (deleted)", e, sizeof e) == 0 && !strcmp(e, "rec"));
    CHECK(appid_exe_base("/tmp/a\"b\\c\n", e, sizeof e) == 0 && !strcmp(e, "abc"));
    CHECK(appid_exe_base("/usr/bin/", e, sizeof e) == -1);
    CHECK(appid_exe_base("/usr/bin/qemu-system-x86_64-with-a-very-long-name", e, 8) == 0 && !strcmp(e, "qemu-sy"));
}

int main(void) {
    /* Struct { Int 7, String "Audio/Source", Int -1 } */
    uint8_t b[128] = { 0 };
    size_t o = 8;
    o = put(b, o, 4); o = put(b, o, 4); o = put(b, o, 7); o += 4;
    o = put(b, o, 13); o = put(b, o, 8); memcpy(b + o, "Audio/Source", 13); o += 16;
    o = put(b, o, 4); o = put(b, o, 4); o = put(b, o, 0xffffffffu); o += 4;
    put(b, 0, (uint32_t)(o - 8)); put(b, 4, 14);
    PwPod s = { b, o }, f;
    int32_t v;
    const char *str;
    CHECK(!pwpod_struct_field(s, 0, &f) && !pwpod_int(f, &v) && v == 7);
    CHECK(!pwpod_struct_field(s, 1, &f) && !pwpod_string(f, &str) && !strcmp(str, "Audio/Source"));
    CHECK(!pwpod_struct_field(s, 2, &f) && !pwpod_int(f, &v) && v == -1);
    CHECK(pwpod_struct_field(s, 3, &f) != 0);                          /* past the end */
    CHECK(!pwpod_struct_field(s, 1, &f) && pwpod_int(f, &v) != 0);     /* a string is not an int */
    PwPod cut = { b, o - 10 };
    CHECK(pwpod_struct_field(cut, 0, &f) != 0);                        /* the struct says more than there is */
    uint8_t lie[64];
    memcpy(lie, b, sizeof lie);
    put(lie, 8, 400);                                                  /* a field longer than the struct */
    PwPod l = { lie, o };
    CHECK(pwpod_struct_field(l, 0, &f) != 0);
    uint8_t nonul[40] = { 0 };
    put(nonul, 0, 24); put(nonul, 4, 14); put(nonul, 8, 4); put(nonul, 12, 8); memcpy(nonul + 16, "abcd", 4);
    PwPod nn = { nonul, 32 };
    CHECK(!pwpod_struct_field(nn, 0, &f) && pwpod_string(f, &str) != 0); /* string without its 0 */

    CHECK(pwguard_capture_flag("Audio/Source") == FW_R_NO_MIC);
    CHECK(pwguard_capture_flag("Audio/Source/Virtual") == FW_R_NO_MIC);
    CHECK(pwguard_capture_flag("Audio/Duplex") == FW_R_NO_MIC);
    CHECK(pwguard_capture_flag("Video/Source") == FW_R_NO_CAMERA);
    CHECK(pwguard_capture_flag("Audio/Sink") == 0);                    /* playback stays */
    CHECK(pwguard_capture_flag(NULL) == 0);
    CHECK(pwguard_stream_flag("Stream/Input/Audio") == FW_R_NO_MIC);
    CHECK(pwguard_stream_flag("Stream/Input/Video") == FW_R_NO_CAMERA);
    CHECK(pwguard_stream_flag("Stream/Output/Audio") == 0);
    /* a shared screen or window (lord-screencast) is hidden from no-screen apps, not from no-camera ones */
    CHECK(pwguard_node_flag("Video/Source", "Screen", "lordos-screencast-0") == FW_R_NO_SCREEN);
    CHECK(pwguard_node_flag("Video/Source", NULL, "lordos-screencast-1") == FW_R_NO_SCREEN);
    CHECK(pwguard_node_flag("Video/Source", "Screen", NULL) == FW_R_NO_SCREEN);
    CHECK(pwguard_node_flag("Video/Source", "Camera", "v4l2_input.pci") == FW_R_NO_CAMERA);
    CHECK(pwguard_node_flag("Video/Source", NULL, NULL) == FW_R_NO_CAMERA);
    CHECK(pwguard_node_flag("Audio/Source", "Screen", "lordos-screencast-0") == FW_R_NO_MIC);   /* only a video source is a screen */
    CHECK(pwguard_node_flag("Audio/Sink", NULL, NULL) == 0);
    /* LordOS Studio's Voice Isolation: only its audio capture stream, by that exact name (and pwguard.c also requires
     * the daemon's own client); its source is an ordinary microphone, hidden from no-mic apps and counted when read */
    CHECK(pwguard_voice_capture("Stream/Input/Audio", "lordos_voice_capture") == 1);
    CHECK(pwguard_voice_capture("Stream/Input/Audio", "lordos_voice_capture2") == 0);
    CHECK(pwguard_voice_capture("Stream/Input/Video", "lordos_voice_capture") == 0);
    CHECK(pwguard_voice_capture("Audio/Source", "lordos_voice_capture") == 0);
    CHECK(pwguard_voice_capture(NULL, "lordos_voice_capture") == 0 && pwguard_voice_capture("Stream/Input/Audio", NULL) == 0);
    CHECK(pwguard_node_flag("Audio/Source", NULL, "lordos_voice_isolation") == FW_R_NO_MIC);

    /* the capture indicator's list: one entry per app, its flags merged; "" an unknown program; a non-plain id refused */
    {
        char out[512];
        PwUse u[4] = { { "desktop:obs", FW_R_NO_MIC, "", "" }, { "", FW_R_NO_CAMERA, "", "" }, { "desktop:obs", FW_R_NO_SCREEN, "", "" },
                       { "flatpak:x", 0, "", "" } };
        CHECK(pwguard_use_format(u, 4, out, sizeof out) == 0);
        CHECK(!strcmp(out, "[{\"app\":\"desktop:obs\",\"mic\":1,\"camera\":0,\"screen\":1},{\"app\":\"\",\"mic\":0,\"camera\":1,\"screen\":0}]"));
        CHECK(pwguard_use_format(u, 0, out, sizeof out) == 0 && !strcmp(out, "[]"));
        PwUse q[1] = { { "desktop:a\"b", FW_R_NO_MIC, "", "" } };
        CHECK(pwguard_use_format(q, 1, out, sizeof out) == -1);
        CHECK(pwguard_use_format(u, 4, out, 20) == -1);                 /* doesn't fit */
        /* with no app: the sound system's own processes, a program known by its executable (one entry each) */
        PwUse s[5] = { { "", FW_R_NO_MIC, "", "sound" }, { "", FW_R_NO_MIC, "chrome", "" }, { "", FW_R_NO_CAMERA, "chrome", "" },
                       { "", FW_R_NO_MIC, "qemu-system-x86", "" }, { "desktop:obs", FW_R_NO_MIC, "obs", "sound" } };
        CHECK(pwguard_use_format(s, 5, out, sizeof out) == 0);
        CHECK(!strcmp(out, "[{\"app\":\"\",\"sys\":\"sound\",\"mic\":1,\"camera\":0,\"screen\":0},"
                           "{\"app\":\"\",\"exe\":\"chrome\",\"mic\":1,\"camera\":1,\"screen\":0},"
                           "{\"app\":\"\",\"exe\":\"qemu-system-x86\",\"mic\":1,\"camera\":0,\"screen\":0},"
                           "{\"app\":\"desktop:obs\",\"mic\":1,\"camera\":0,\"screen\":0}]"));   /* an app's entry says only the app */
        PwUse e[1] = { { "", FW_R_NO_MIC, "a\"b", "" } };
        CHECK(pwguard_use_format(e, 1, out, sizeof out) == -1);
    }

    /* a sandboxed pulse client (WebKit's bubblewrap in a web app: inner pid 2, "org.webkit.app-<hash>"): named only when
     * one app's processes with that inner pid hold as many connections to pipewire-pulse as clients claim them */
    {
        char app[128];
        PwCand one[1] = { { "desktop:lordos-web-whatsapp", 1 } };
        CHECK(pwguard_sandbox_pick(one, 1, 1, app, sizeof app) == 0 && !strcmp(app, "desktop:lordos-web-whatsapp"));
        CHECK(pwguard_sandbox_pick(one, 1, 2, app, sizeof app) == -1 && !app[0]);        /* more claims than connections */
        CHECK(pwguard_sandbox_pick(one, 1, 0, app, sizeof app) == -1);                   /* nobody claims: nothing to name */
        PwCand two[2] = { { "desktop:lordos-web-whatsapp", 1 }, { "desktop:lordos-web-gmail", 2 } };
        CHECK(pwguard_sandbox_pick(two, 2, 1, app, sizeof app) == -1);                   /* two apps could be it */
        CHECK(pwguard_sandbox_pick(two, 2, 2, app, sizeof app) == 0 && !strcmp(app, "desktop:lordos-web-gmail"));
        PwCand same[2] = { { "desktop:lordos-web-gmail", 1 }, { "desktop:lordos-web-gmail", 1 } };
        CHECK(pwguard_sandbox_pick(same, 2, 1, app, sizeof app) == 0 && !strcmp(app, "desktop:lordos-web-gmail"));
        CHECK(pwguard_sandbox_pick(NULL, 0, 1, app, sizeof app) == -1);
    }

    lineage_tests();
    printf("pwguard-test: %d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
