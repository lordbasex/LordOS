/* fpipe.c — see fpipe.h. */
#include "fpipe.h"

#include <string.h>

void fpipe_init(FPipe *p, const char *env) {
    memset(p, 0, sizeof *p);
    p->mode = env && !strcmp(env, "0") ? FPIPE_OFF : env && !strcmp(env, "2") ? FPIPE_ALWAYS : FPIPE_AUTO;
}

void fpipe_note(FPipe *p, double now_ms, double cpu_ms, double period_ms) {
    if (p->mode != FPIPE_AUTO || period_ms <= 0) return;
    int idle = p->last > 0 && now_ms - p->last > FPIPE_IDLE_MS;
    p->last = now_ms;
    if (idle) { p->on = 0; p->heavy = p->light = 0; }
    if (!p->on) {
        p->heavy = cpu_ms > period_ms * 0.6 ? p->heavy + 1 : 0;
        if (p->heavy >= 2) { p->on = 1; p->heavy = p->light = 0; }
        return;
    }
    p->light = cpu_ms < period_ms * 0.4 ? p->light + 1 : 0;
    if (p->light >= 60) { p->on = 0; p->heavy = p->light = 0; }
}

int fpipe_engaged(const FPipe *p) { return p->mode == FPIPE_ALWAYS || (p->mode == FPIPE_AUTO && p->on); }

int fpipe_step(int eligible, int pending, int prep, int needs, int paint) {
    if (!pending) {
        if (prep == FPIPE_READY) return FPIPE_SUBMIT;
        return needs || paint ? FPIPE_BUILD : FPIPE_WAIT;
    }
    return eligible && prep == FPIPE_NONE && paint ? FPIPE_PREPARE : FPIPE_WAIT;
}

int fpipe_blocked(int eligible, int pending, int prep) { return pending && (!eligible || prep != FPIPE_NONE); }

int fpipe_hold_callbacks(int prep, int asked_again) { return prep == FPIPE_EMPTY && asked_again; }
