/* cli.c — `lordos-sysreport` and `lordos-fetch`, System Information's commands (see
 * docs/BENCHMARK.md). Ordinary user commands: lord-server never runs them. The report is the same as
 * the app's, so one can be made on a machine without a session (a headless Pi, a VM over SSH, CI).
 *
 *   lordos-sysreport report [--json | --markdown]
 *   lordos-sysreport bench [--quick | --full] [--json]
 *   lordos-fetch [--share] [--color=auto|always|never]      (also: lordos-sysreport fetch …)
 *
 * lordos-fetch is a link to this binary. --root <dir> reads a fake tree instead of this machine
 * (tests and fixtures only).
 *
 * bench runs LordBench through lord-server's own code (bench/lbrun.c): the same sandboxed lord-bench
 * and lord-media, which must be next to this binary (it is installed in the LordOS folder; /usr/bin has
 * links), the same guards and the same result file as System Information → Performance. Without a session
 * the helper gets no cgroup scope and the desktop test can't run. */
#include "sysreport.h"
#include "fetch.h"
#include "lord_version.h"
#include "../bench/lbproto.h"
#include "../bench/lbrun.h"
#include "../sec/sandbox.h"
#include "../sec/json.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int usage(int fetch) {
    if (fetch)
        fprintf(stderr, "usage: lordos-fetch [--share] [--color=auto|always|never]\n"
                        "  Shows the crown and a summary of this computer. --share leaves out the user and\n"
                        "  host names, for a screenshot you will post.\n");
    else
        fprintf(stderr, "usage: lordos-sysreport report [--json | --markdown]\n"
                        "       lordos-sysreport bench [--quick | --full] [--json]\n"
                        "       lordos-sysreport fetch [--share] [--color=auto|always|never]\n"
                        "  report prints this computer's hardware and what the system made of it (no serial\n"
                        "  numbers, addresses, host or user names). Markdown by default.\n"
                        "  bench measures it with LordBench, as System Information → Performance does: --quick\n"
                        "  (default, about half a minute) or --full (adds storage and 3 minutes of sustained load,\n"
                        "  4 to 6 minutes). The result is also saved for the app. Nothing is sent anywhere.\n");
    return 2;
}

static volatile sig_atomic_t interrupted;
static void on_signal(int sig) { (void)sig; interrupted = 1; }

static int json_has_try(const JVal *tries, const char *name) {
    for (const JVal *t = tries->kid; t; t = t->next) { const char *id = json_str(t, "id"); if (id && !strcmp(id, name)) return 1; }
    return 0;
}

/* One line per measurement and per skipped one, from the result JSON lbrun built. Returns how many of the
 * helper-sandbox self-test's tries were not refused (a missing one counts; the file tries on a kernel without
 * Landlock don't), 0 for a result without the test. */
static int bench_print(const char *json) {
    JDoc d;
    char err[96];
    if (json_parse(json, strlen(json), &d, err, sizeof err)) { puts(json); return 0; }
    const JVal *r = json_get(d.root, "result");
    const char *status = json_str(r, "status"), *stop = json_str(r, "stopReason");
    printf("LordBench %d, %s test: %s in %.0f s (LordOS %s, %s, %.0f threads)\n", (int)json_num(r, "lordbench", 0),
           json_str(r, "mode") ? json_str(r, "mode") : "?", status ? status : "?", json_num(r, "seconds", 0),
           json_str(r, "lordos") ? json_str(r, "lordos") : "?", json_str(r, "arch") ? json_str(r, "arch") : "?", json_num(r, "threads", 0));
    if (stop && stop[0]) printf("stopped: %s\n", stop);
    const JVal *res = json_get(r, "results");
    for (const JVal *m = res && res->type == J_OBJ ? res->kid : NULL; m; m = m->next) {
        const char *unit = json_str(m, "unit");
        printf("  %-18s %10.4g %-8s ±%.3g\n", m->key, json_num(m, "value", 0), unit ? unit : "", json_num(m, "spread", 0));
    }
    const JVal *sk = json_get(r, "skipped");
    int only_full = 0, said = 0;
    for (const JVal *m = sk && sk->type == J_OBJ ? sk->kid : NULL; m; m = m->next) {
        if (m->type == J_STR && !strcmp(m->str, "mode")) { only_full++; continue; }     /* not part of this test */
        if (!said++) printf("skipped:\n");
        printf("  %-18s %s\n", m->key, m->type == J_STR ? m->str : "");
    }
    if (only_full) printf("(%d more with --full)\n", only_full);
    /* the helper-sandbox self-test lord-bench made after locking itself in: every one of the LB_SBX_COUNT tries
     * must be there and refused, as the Stability verdict checks (apps/sysinfo/perf.js) */
    const JVal *sb = json_get(r, "sandboxTries");
    int leaks = 0;
    if (sb && sb->type == J_ARR) {
        int denied[LB_SBX_COUNT] = { 0 };
        for (int i = 0; i < LB_SBX_COUNT; i++)
            for (const JVal *t = sb->kid; t; t = t->next) {
                const char *id = json_str(t, "id");
                if (id && !strcmp(id, lb_sbx_name(i))) { denied[i] = json_bool(t, "denied", 0); break; }
            }
        /* without Landlock in the kernel (no SB_LANDLOCK_FS in the mask) the two file tries are expected to work:
         * said, not counted, as the verdict only warns for them */
        int no_landlock = !((int)json_num(r, "sandbox", 0) & SB_LANDLOCK_FS), refused = 0;
        for (int i = 0; i < LB_SBX_COUNT; i++) refused += denied[i];
        printf("helper sandbox: %d of %d tries refused\n", refused, LB_SBX_COUNT);
        for (int i = 0; i < LB_SBX_COUNT; i++) {
            if (denied[i]) continue;
            int file_try = i == LB_SBX_NEWFILE || i == LB_SBX_READ;
            if (no_landlock && file_try && json_has_try(sb, lb_sbx_name(i))) printf("  allowed without Landlock: %s\n", lb_sbx_name(i));
            else { printf("  NOT REFUSED: %s\n", lb_sbx_name(i)); leaks++; }
        }
    }
    json_free(&d);
    return leaks;
}

/* a Full run's sustained load, the same 3 minutes as System Information's Full (SUSTAINED_S in
   apps/sysinfo/performance.jsx), so a terminal run and the app's are the same test */
#define CLI_SUSTAINED_S 180

static int bench(int full, int json) {
    struct sigaction sa = { 0 };
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    if (lbrun_start(full ? "full" : "quick", full ? CLI_SUSTAINED_S : 0, "{\"cli\":true}")) {
        fprintf(stderr, "lordos-sysreport: LordBench did not start: another run is going on, or the lord-bench next to this program is missing or writable by others\n");
        return 1;
    }
    char last[96] = "";
    for (;;) {
        struct timespec ts = { 0, 250 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        if (interrupted) { lbrun_cancel(); interrupted = 0; }
        char *p = lbrun_poll();
        if (!p) return 1;
        if (!strstr(p, "\"state\":\"waiting\"") && !strstr(p, "\"state\":\"running\"")) {
            int leaks = 0;
            if (json) puts(p);
            else leaks = bench_print(p);
            int ok = strstr(p, "\"status\":\"done\"") != NULL;
            free(p);
            return !ok ? 1 : leaks ? 3 : 0;     /* 3: finished, but the helper sandbox let a try through */
        }
        const char *st = strstr(p, "\"step\":\"");
        if (st && isatty(2)) {                      /* progress on the terminal only */
            st += 8;
            const char *e = strchr(st, '"');
            int n = e ? (int)(e - st) : 0;
            if (n > 0 && n < (int)sizeof last && (strncmp(last, st, (size_t)n) || last[n])) {
                snprintf(last, sizeof last, "%.*s", n, st);
                fprintf(stderr, "  %s…\n", last);
            }
        }
        free(p);
    }
}

int main(int argc, char **argv) {
    const char *root = "", *mode = "markdown", *color = "auto";
    const char *base = strrchr(argv[0], '/');
    base = base ? base + 1 : argv[0];
    int fetch = !strcmp(base, "lordos-fetch"), report = 0, share = 0, bnch = 0, full = 0, json = 0;
    for (int i = 1; i < argc; i++) {
        if (!fetch && !report && !bnch && !strcmp(argv[i], "report")) report = 1;
        else if (!fetch && !report && !bnch && !strcmp(argv[i], "fetch")) fetch = 1;
        else if (!fetch && !report && !bnch && !strcmp(argv[i], "bench")) bnch = 1;
        else if (bnch && (!strcmp(argv[i], "--quick") || !strcmp(argv[i], "--full"))) full = argv[i][2] == 'f';
        else if (bnch && !strcmp(argv[i], "--json")) json = 1;
        else if (report && !strcmp(argv[i], "--json")) mode = "json";
        else if (report && !strcmp(argv[i], "--markdown")) mode = "markdown";
        else if (fetch && !strcmp(argv[i], "--share")) share = 1;
        else if (fetch && !strncmp(argv[i], "--color=", 8)
                 && (!strcmp(argv[i] + 8, "auto") || !strcmp(argv[i] + 8, "always") || !strcmp(argv[i] + 8, "never"))) color = argv[i] + 8;
        else if (!strcmp(argv[i], "--root") && i + 1 < argc) root = argv[++i];
        else return usage(fetch);
    }
    if (bnch) return root[0] ? usage(0) : bench(full, json);
    if (!report && !fetch) return usage(0);

    SysReport *r = malloc(sizeof *r);
    if (!r) return 1;
    sysreport_collect(r, root, NULL, NULL);
    snprintf(r->sw.lordos, sizeof r->sw.lordos, "%s%s%s", LORD_VERSION, LORD_CHANNEL[0] ? "-" : "", LORD_CHANNEL);
    char *out;
    if (fetch) {
        FetchLive l;
        fetch_live(&l, root, share);
        out = fetch_render(r, &l, fetch_color_mode(color, isatty(1)));
    } else {
        out = !strcmp(mode, "json") ? sysreport_json(r) : sysreport_markdown(r);
    }
    free(r);
    if (!out) { fprintf(stderr, "%s: out of memory\n", base); return 1; }
    fputs(out, stdout);
    free(out);
    return ferror(stdout) ? 1 : 0;
}
