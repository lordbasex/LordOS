/* vmpace_test.c — how often a machine's picture is taken (src/vm/vmpace.h, VD_PACE in vmproto.h): idle after a run of
 * unchanged frames, hidden, never in the helper for a Linux 3D picture, input ending idle, the helper's interval and
 * hold, and the message check */
#include "../src/vm/vmpace.h"
#include <stdio.h>

static int n, bad;
#define CHECK(c, ...) do { n++; if (!(c)) { bad++; fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

int main(void) {
    VmPace p = { 0, 0 };
    int ms = -1, kind = -1;
    CHECK(!vmpace_step(&p, 0, 0, &ms, &kind), "a new picture is unpaced and nothing is sent");
    for (int i = 0; i < VMPACE_IDLE_AFTER - 1; i++) vmpace_frame(&p, 0);
    CHECK(!vmpace_step(&p, 0, 0, &ms, &kind), "not idle before %d unchanged frames", VMPACE_IDLE_AFTER);
    vmpace_frame(&p, 0);
    CHECK(vmpace_step(&p, 0, 0, &ms, &kind) && kind == VDP_IDLE && ms == VMPACE_IDLE_MS, "idle after %d unchanged frames", VMPACE_IDLE_AFTER);
    CHECK(!vmpace_step(&p, 0, 0, &ms, &kind), "idle is told once");
    vmpace_frame(&p, 1);
    CHECK(vmpace_step(&p, 0, 0, &ms, &kind) && kind == VDP_NONE && ms == 0, "a changed frame ends idle");
    for (int i = 0; i < VMPACE_IDLE_AFTER; i++) vmpace_frame(&p, 0);
    CHECK(vmpace_step(&p, 0, 0, &ms, &kind) && kind == VDP_IDLE, "idle again");
    vmpace_input(&p);
    CHECK(p.kind == VDP_NONE && p.same_run == 0, "input ends idle (the helper ended it itself)");
    CHECK(!vmpace_step(&p, 0, 0, &ms, &kind), "nothing to send after input: the helper knows");
    CHECK(vmpace_step(&p, 1, 0, &ms, &kind) && kind == VDP_HIDDEN && ms == VMPACE_HIDDEN_MS, "hidden");
    vmpace_input(&p);
    CHECK(p.kind == VDP_HIDDEN, "input doesn't end hidden");
    for (int i = 0; i < 100; i++) vmpace_frame(&p, 0);
    CHECK(!vmpace_step(&p, 1, 0, &ms, &kind), "hidden stays hidden while unchanged");
    CHECK(vmpace_step(&p, 0, 0, &ms, &kind) && kind == VDP_IDLE, "shown again, still unchanged: idle");
    VmPace v = { 1000, 0 };
    CHECK(!vmpace_step(&v, 1, 1, &ms, &kind), "a Linux 3D picture is never paced in the helper, hidden or idle");
    VmPace w = { 0, VDP_HIDDEN };
    CHECK(vmpace_step(&w, 0, 1, &ms, &kind) && kind == VDP_NONE, "a picture that becomes 3D drops its pace");
    CHECK(vmpace_interval(60, 0) == 16 && vmpace_interval(0, 0) == 16 && vmpace_interval(144, 0) == 6, "the display's rate");
    CHECK(vmpace_interval(60, VMPACE_IDLE_MS) == VMPACE_IDLE_MS && vmpace_interval(1, 100) == 1000, "the longer of rate and pace");
    CHECK(vmpace_hold_ms(0, 250) == 250 && vmpace_hold_ms(VMPACE_HIDDEN_MS, 250) == VMPACE_HIDDEN_MS + 250, "hold: pace + the usual wait");
    CHECK(vmpace_hold_ms(800, 250) == VD_PACE_MAX_MS && VD_PACE_MAX_MS < 1000, "hold: always inside QEMU's 1 s");
    CHECK(VMPACE_HIDDEN_MS + 250 <= VD_PACE_MAX_MS, "the hidden pace is held whole");
    VdMsg m = vd_msg(VD_PACE, VMPACE_IDLE_MS, VDP_IDLE, 0, 0, 0, 0);
    CHECK(vd_msg_valid(&m, 1), "VD_PACE idle valid");
    m = vd_msg(VD_PACE, 0, VDP_NONE, 0, 0, 0, 0);
    CHECK(vd_msg_valid(&m, 1), "VD_PACE none valid");
    m = vd_msg(VD_PACE, 0, VDP_IDLE, 0, 0, 0, 0);
    CHECK(!vd_msg_valid(&m, 1), "VD_PACE idle without an interval refused");
    m = vd_msg(VD_PACE, 100, VDP_NONE, 0, 0, 0, 0);
    CHECK(!vd_msg_valid(&m, 1), "VD_PACE an interval without a reason refused");
    m = vd_msg(VD_PACE, VD_PACE_MAX_MS + 1, VDP_HIDDEN, 0, 0, 0, 0);
    CHECK(!vd_msg_valid(&m, 1), "VD_PACE over the limit refused");
    m = vd_msg(VD_PACE, 100, 3, 0, 0, 0, 0);
    CHECK(!vd_msg_valid(&m, 1), "VD_PACE unknown reason refused");
    m = vd_msg(VD_PACE, 100, VDP_IDLE, 0, 0, 0, 0);
    CHECK(!vd_msg_valid(&m, 0), "VD_PACE never from the helper");
    if (bad) { fprintf(stderr, "vmpace-test: %d of %d failed\n", bad, n); return 1; }
    printf("vmpace-test: %d checks passed\n", n);
    return 0;
}
