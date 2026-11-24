/* cgroup_restart_test.c — lordos.service restarts with live sessions on systemd 258 and later (GitHub #140):
 * cgroup_release_at() notes the sessions' limits and turns the controllers off (sessions first, then the service
 * root); cgroup_restore_at() puts the limits back. On a directory tree that stands in for cgroupfs: a write to
 * cgroup.subtree_control replaces the file, so "-cpu -memory -pids" there means "all turned off" (the kernel
 * would read back an empty line).
 *   make -C server build/cgroup-restart-test */
#include "../src/sec/cgroup.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static char root[256];

static void put(const char *rel, const char *text) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    FILE *f = fopen(p, "w");
    if (!f) { perror(p); exit(1); }
    fputs(text, f);
    fclose(f);
}
static void dir(const char *rel) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    if (mkdir(p, 0755)) { perror(p); exit(1); }
}
static const char *get(const char *rel) {
    static char buf[4096];
    char p[512];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    FILE *f = fopen(p, "r");
    if (!f) return "(missing)";
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return buf;
}

int main(void) {
    snprintf(root, sizeof root, "%s/cgroup-restart.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(root)) { perror("mkdtemp"); return 1; }

    /* lordos.service as lord-sessiond left it, with two sessions alive */
    put("cgroup.subtree_control", "cpu memory pids\n");
    dir("supervisor");
    put("supervisor/cgroup.subtree_control", "\n");
    dir("netd");
    put("netd/cgroup.subtree_control", "\n");
    dir("lordos-session-1000");
    put("lordos-session-1000/cgroup.subtree_control", "cpu memory pids\n");
    dir("lordos-session-1000/server");
    put("lordos-session-1000/server/cpu.weight", "1000\n");
    put("lordos-session-1000/server/memory.max", "max\n");
    dir("lordos-session-1000/app-lordos-lpk-notes.scope");
    put("lordos-session-1000/app-lordos-lpk-notes.scope/memory.high", "268435456\n");
    put("lordos-session-1000/app-lordos-lpk-notes.scope/memory.max", "536870912\n");
    put("lordos-session-1000/app-lordos-lpk-notes.scope/cpu.max", "50000 100000\n");
    put("lordos-session-1000/app-lordos-lpk-notes.scope/cpu.weight", "100\n");
    dir("lordos-session-1000/app-lordos-featherpad.scope");
    put("lordos-session-1000/app-lordos-featherpad.scope/memory.max", "max\n");
    put("lordos-session-1000/app-lordos-featherpad.scope/cpu.max", "max 100000\n");
    dir("lordos-session-1000/not-ours");              /* a name lord-sessiond never makes: not noted */
    put("lordos-session-1000/not-ours/memory.max", "1\n");
    dir("lordos-session-1001");
    put("lordos-session-1001/cgroup.subtree_control", "memory pids\n");
    dir("lordos-session-1001/server");
    put("lordos-session-1001/server/cpu.weight", "1000\n");

    int svc = open(root, O_RDONLY | O_DIRECTORY);
    char saved_path[300];
    snprintf(saved_path, sizeof saved_path, "%s.saved", root);
    int out = open(saved_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK(svc >= 0 && out >= 0, "open");
    CHECK(cgroup_release_at(svc, out) == 0, "release reports controllers still on");
    close(out);

    /* every controller off: the sessions' and then the root's (the root last: the kernel refuses it before) */
    CHECK(!strcmp(get("cgroup.subtree_control"), "-cpu -memory -pids"), "root: %s", get("cgroup.subtree_control"));
    CHECK(!strcmp(get("lordos-session-1000/cgroup.subtree_control"), "-cpu -memory -pids"), "session 1000: %s", get("lordos-session-1000/cgroup.subtree_control"));
    CHECK(!strcmp(get("lordos-session-1001/cgroup.subtree_control"), "-memory -pids"), "session 1001: %s", get("lordos-session-1001/cgroup.subtree_control"));
    CHECK(!strcmp(get("supervisor/cgroup.subtree_control"), "\n"), "an empty one is left alone");

    /* the limits that differ from the defaults, nothing else */
    FILE *f = fopen(saved_path, "r");
    char saved[4096] = "";
    size_t n = f ? fread(saved, 1, sizeof saved - 1, f) : 0;
    if (f) fclose(f);
    saved[n] = 0;
    const char *want[] = {
        "lordos-session-1000 server cpu.weight 1000\n",
        "lordos-session-1000 app-lordos-lpk-notes.scope memory.high 268435456\n",
        "lordos-session-1000 app-lordos-lpk-notes.scope memory.max 536870912\n",
        "lordos-session-1000 app-lordos-lpk-notes.scope cpu.max 50000 100000\n",
        "lordos-session-1001 server cpu.weight 1000\n",
    };
    int lines = 0;
    for (const char *p = saved; *p; p++) lines += *p == '\n';
    for (size_t i = 0; i < sizeof want / sizeof *want; i++) CHECK(strstr(saved, want[i]), "not noted: %s", want[i]);
    CHECK(lines == (int)(sizeof want / sizeof *want), "%d lines noted:\n%s", lines, saved);
    CHECK(!strstr(saved, "not-ours") && !strstr(saved, "featherpad"), "noted too much:\n%s", saved);

    /* a second run (nothing on any more) has nothing to do and succeeds */
    CHECK(cgroup_release_at(svc, -1) == 0, "second release");

    /* Two releases with no restore between (lord-sessiond failed before cgroup_service_init): the second can't see
     * the limits any more (their files went with the controllers), so its notes must not replace the first's. */
    {
        static const char *gone[] = { "lordos-session-1000/server/cpu.weight", "lordos-session-1000/app-lordos-lpk-notes.scope/memory.high",
                                      "lordos-session-1000/app-lordos-lpk-notes.scope/memory.max", "lordos-session-1000/app-lordos-lpk-notes.scope/cpu.max",
                                      "lordos-session-1001/server/cpu.weight" };
        char p[512];
        for (size_t i = 0; i < sizeof gone / sizeof *gone; i++) { snprintf(p, sizeof p, "%s/%s", root, gone[i]); unlink(p); }
        char fresh_path[300];
        snprintf(fresh_path, sizeof fresh_path, "%s.fresh", root);
        int fo = open(fresh_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
        CHECK(cgroup_release_at(svc, fo) == 0, "release without the limit files");
        char fresh[4096] = "";
        ssize_t fn = pread(fo, fresh, sizeof fresh - 1, 0);
        fresh[fn > 0 ? fn : 0] = 0;
        close(fo);
        unlink(fresh_path);
        CHECK(fresh[0] == 0, "the second release saw limits: %s", fresh);
        static char merged[CG_NOTES_MAX];
        CHECK(cgroup_merge_notes(svc, saved, fresh, merged, sizeof merged) == 0, "merge");
        CHECK(!strcmp(merged, saved), "the first notes were lost:\n%s", merged);
        /* a fresher value wins; a note for a leaf that is gone is dropped; a note that doesn't parse is dropped */
        const char *older = "lordos-session-1000 app-lordos-lpk-notes.scope memory.max 1\n"
                            "lordos-session-1000 app-lordos-gone.scope memory.max 7\n"
                            "lordos-session-1000 server cgroup.procs 1\n"
                            "lordos-session-1001 server cpu.weight 1000\n";
        const char *newer = "lordos-session-1000 app-lordos-lpk-notes.scope memory.max 536870912\n";
        CHECK(cgroup_merge_notes(svc, older, newer, merged, sizeof merged) == 0, "merge 2");
        CHECK(!strcmp(merged, "lordos-session-1000 app-lordos-lpk-notes.scope memory.max 536870912\n"
                              "lordos-session-1001 server cpu.weight 1000\n"), "merged:\n%s", merged);
        char tiny[16];
        CHECK(cgroup_merge_notes(svc, saved, "", tiny, sizeof tiny) == -1, "an overflow must refuse (old file kept)");
    }

    /* a limit that is refused counts as failed (the notes file stays for the next start) */
    {
        dir("lordos-session-1001/app-lordos-x.scope");
        dir("lordos-session-1001/app-lordos-x.scope/memory.max");     /* a directory: the write fails */
        int failed = -1;
        int put = cgroup_restore_at(svc, "lordos-session-1001 app-lordos-x.scope memory.max 1\n"
                                         "lordos-session-1001 app-lordos-gone.scope memory.max 1\n", &failed);
        CHECK(put == 0 && failed == 1, "put %d failed %d (a gone leaf is no failure)", put, failed);
    }

    /* back: the files are gone with the controllers on cgroupfs; here they are emptied */
    put("lordos-session-1000/server/cpu.weight", "100\n");
    put("lordos-session-1000/app-lordos-lpk-notes.scope/memory.high", "max\n");
    put("lordos-session-1000/app-lordos-lpk-notes.scope/memory.max", "max\n");
    put("lordos-session-1000/app-lordos-lpk-notes.scope/cpu.max", "max 100000\n");
    put("lordos-session-1001/server/cpu.weight", "100\n");
    int put_failed = -1, put_back = cgroup_restore_at(svc, saved, &put_failed);
    CHECK(put_back == 5 && put_failed == 0, "%d values put back, %d failed", put_back, put_failed);
    CHECK(!strcmp(get("lordos-session-1000/server/cpu.weight"), "1000"), "cpu.weight %s", get("lordos-session-1000/server/cpu.weight"));
    CHECK(!strcmp(get("lordos-session-1000/app-lordos-lpk-notes.scope/memory.high"), "268435456"), "memory.high");
    CHECK(!strcmp(get("lordos-session-1000/app-lordos-lpk-notes.scope/memory.max"), "536870912"), "memory.max");
    CHECK(!strcmp(get("lordos-session-1000/app-lordos-lpk-notes.scope/cpu.max"), "50000 100000"), "cpu.max");
    CHECK(!strcmp(get("lordos-session-1001/server/cpu.weight"), "1000"), "session 1001 cpu.weight");

    /* lines that don't name a session leaf and a known limit with a plain value are skipped */
    const char *hostile =
        "lordos-session-1000 ../../etc memory.max 1\n"
        "lordos-session-1000 server cgroup.procs 1\n"
        "lordos-session-1000 server memory.max 1;rm\n"
        "other.service server memory.max 1\n"
        "lordos-session-1000 server memory.max\n"
        "lordos-session-1000 gone.scope memory.max 1\n"
        "lordos-session-1000x server memory.max 1\n";
    CHECK(cgroup_restore_at(svc, hostile, NULL) == 0, "a bad line was written");
    CHECK(!strcmp(get("lordos-session-1000/server/memory.max"), "max\n"), "memory.max changed: %s", get("lordos-session-1000/server/memory.max"));
    CHECK(access(saved_path, F_OK) == 0, "saved");

    /* (docs/VIRTUALIZATION.md 3.7) the machines' slice: which names go into it and which noted children are accepted */
    CHECK(cgroup_scope_in_machines("app-lordos-lordos-vm-6f1c0d2e-1111-4222-8333-444455556666.scope"), "a machine's scope");
    CHECK(cgroup_scope_in_machines("app-lordos-lordos-vm-tor-6f1c0d2e-1111-4222-8333-444455556666.scope"), "a Tor gateway's");
    CHECK(!cgroup_scope_in_machines("app-lordos-firefox.scope") && !cgroup_scope_in_machines("lordos-machines.slice")
          && !cgroup_scope_in_machines("app-lordos-lordos-vm-.scope") && !cgroup_scope_in_machines("app-lordos-lordos-vm-x/../y.scope")
          && !cgroup_scope_in_machines("app-lordos-lordos-vm-x.slice") && !cgroup_scope_in_machines(NULL), "not a machine's");
    CHECK(!cgroup_scope_name_ok("lordos-machines.slice"), "nobody may ask to enter the slice itself");
    CHECK(cgroup_session_child_ok("server") && cgroup_session_child_ok("app-lordos-x.scope")
          && cgroup_session_child_ok("lordos-machines.slice/app-lordos-lordos-vm-6f1c0d2e.scope"), "children");
    CHECK(!cgroup_session_child_ok("lordos-machines.slice") && !cgroup_session_child_ok("lordos-machines.slice/server")
          && !cgroup_session_child_ok("lordos-machines.slice/app-lordos-firefox.scope")
          && !cgroup_session_child_ok("lordos-machines.slice/../server") && !cgroup_session_child_ok("other.slice/app-lordos-lordos-vm-1.scope")
          && !cgroup_session_child_ok("lordos-machines.slice/app-lordos-lordos-vm-1.scope/qemu"), "not children");
    {
        dir("lordos-session-1002");
        put("lordos-session-1002/cgroup.subtree_control", "cpu io memory pids\n");
        dir("lordos-session-1002/lordos-machines.slice");
        put("lordos-session-1002/lordos-machines.slice/cgroup.subtree_control", "cpu io memory pids\n");
        put("lordos-session-1002/lordos-machines.slice/cpu.weight", "50\n");
        dir("lordos-session-1002/lordos-machines.slice/app-lordos-lordos-vm-6f1c0d2e.scope");
        put("lordos-session-1002/lordos-machines.slice/app-lordos-lordos-vm-6f1c0d2e.scope/memory.max", "4429185024\n");
        put("lordos-session-1002/lordos-machines.slice/app-lordos-lordos-vm-6f1c0d2e.scope/cpu.max", "450000 100000\n");
        dir("lordos-session-1002/lordos-machines.slice/app-lordos-firefox.scope");     /* never made there: not noted */
        put("lordos-session-1002/lordos-machines.slice/app-lordos-firefox.scope/memory.max", "1\n");
        char sp[300];
        snprintf(sp, sizeof sp, "%s.slice", root);
        int so = open(sp, O_RDWR | O_CREAT | O_TRUNC, 0600);
        CHECK(cgroup_release_at(svc, so) == 0, "release with a slice");
        char notes[4096] = "";
        ssize_t sn = pread(so, notes, sizeof notes - 1, 0);
        notes[sn > 0 ? sn : 0] = 0;
        close(so);
        unlink(sp);
        CHECK(!strcmp(get("lordos-session-1002/lordos-machines.slice/cgroup.subtree_control"), "-cpu -io -memory -pids"), "the slice first: %s",
              get("lordos-session-1002/lordos-machines.slice/cgroup.subtree_control"));
        CHECK(!strcmp(get("lordos-session-1002/cgroup.subtree_control"), "-cpu -io -memory -pids"), "then its session");
        CHECK(strstr(notes, "lordos-session-1002 lordos-machines.slice/app-lordos-lordos-vm-6f1c0d2e.scope memory.max 4429185024\n")
              && strstr(notes, "lordos-session-1002 lordos-machines.slice/app-lordos-lordos-vm-6f1c0d2e.scope cpu.max 450000 100000\n"), "noted:\n%s", notes);
        CHECK(!strstr(notes, "firefox") && !strstr(notes, "lordos-machines.slice cpu.weight"), "noted too much:\n%s", notes);
        put("lordos-session-1002/lordos-machines.slice/app-lordos-lordos-vm-6f1c0d2e.scope/memory.max", "max\n");
        int ff = -1;
        CHECK(cgroup_restore_at(svc, notes, &ff) >= 2 && ff == 0, "put back %d", ff);
        CHECK(!strcmp(get("lordos-session-1002/lordos-machines.slice/app-lordos-lordos-vm-6f1c0d2e.scope/memory.max"), "4429185024"), "a machine's memory.max back");
        CHECK(cgroup_restore_at(svc, "lordos-session-1002 lordos-machines.slice/app-lordos-firefox.scope memory.max 9\n"
                                     "lordos-session-1002 lordos-machines.slice/../server memory.max 9\n", NULL) == 0, "a hostile slice line was written");
    }

    close(svc);
    char cmd[700];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' '%s'", root, saved_path);
    if (system(cmd)) { /* best effort */ }
    if (fails) { fprintf(stderr, "cgroup-restart: %d failure(s)\n", fails); return 1; }
    printf("cgroup-restart: ok\n");
    return 0;
}
