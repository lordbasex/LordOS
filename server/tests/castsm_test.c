/* castsm_test.c — the ScreenCast portal's sessions without a bus (src/cast/castsm.c): every call in the state it
 * belongs to and refused in the others, a session only its own app can reach, Cancel and the app's own Close of its
 * Start, the limits, every session of an app that leaves the bus, and who may get the chooser (csm_gate). */
#include "cast/castsm.h"

#include <stdio.h>
#include <string.h>

static int ok, bad;
#define CHECK(c) do { if (c) ok++; else { bad++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define APP ":1.42"
#define OTHER ":1.43"
#define REQ "/org/freedesktop/portal/desktop/request/1_42/r1"

static CsSess *made(CastSm *m, const char *sender, const char *token) {
    CsSess *s = NULL;
    return csm_create(m, sender, token, &s) == CSM_OK ? s : NULL;
}

int main(void) {
    CastSm m;
    csm_init(&m);
    char p[CSM_PATH_MAX];

    /* paths: the spec's form; a token with other characters is cut, an empty one is ours */
    csm_path("/o", "session", ":1.42", "tok_1", 7, p, sizeof p);
    CHECK(!strcmp(p, "/o/session/1_42/tok_1"));
    csm_path("/o", "session", ":1.42", "a/../b-c", 7, p, sizeof p);
    CHECK(!strcmp(p, "/o/session/1_42/abc"));
    csm_path("/o", "request", ":1.42", "", 7, p, sizeof p);
    CHECK(!strcmp(p, "/o/request/1_42/lordos7"));

    /* the whole way: create, select, start, pick, started */
    CsSess *s = made(&m, APP, "s1");
    CHECK(s && s->st == CS_CREATED && !strcmp(s->path, "/org/freedesktop/portal/desktop/session/1_42/s1"));
    CHECK(csm_find(&m, APP, s->path) == s);
    CHECK(csm_find(&m, OTHER, s->path) == NULL);            /* another app's session is not found */
    CHECK(csm_start(&m, s, REQ) == CSM_E_STATE);            /* Start before SelectSources */
    CHECK(csm_may_remote(s) == 0);
    CHECK(csm_select(&m, s, 0, 0, 0, 0) == CSM_OK && s->types == CSM_TYPE_MONITOR && s->cursor == CSM_CURSOR_HIDDEN);
    CHECK(csm_select(&m, s, CSM_TYPE_WINDOW, 0, 0, 0) == CSM_E_STATE);      /* once */
    CHECK(csm_start(&m, s, REQ) == CSM_OK && s->st == CS_CHOOSING && csm_choosing(&m) == s);
    CHECK(csm_start(&m, s, REQ) == CSM_E_STATE);
    uint32_t mon = CSM_TYPE_MONITOR, win = CSM_TYPE_WINDOW, two[2] = { CSM_TYPE_MONITOR, CSM_TYPE_MONITOR };
    CHECK(csm_chosen(&m, s, &win, 1) == CSM_E_ARGS && s->st == CS_CHOOSING);  /* a window when only monitors were asked */
    CHECK(csm_chosen(&m, s, two, 2) == CSM_E_ARGS);          /* two when multiple is false */
    CHECK(csm_chosen(&m, s, &mon, 1) == CSM_OK && s->st == CS_STARTING);
    CHECK(csm_may_remote(s) == 0);
    CHECK(csm_started(s) == CSM_OK && s->st == CS_STARTED && !s->req[0]);
    CHECK(csm_may_remote(s) == 1);
    CHECK(csm_started(s) == CSM_E_STATE);
    CHECK(csm_start(&m, s, REQ) == CSM_E_STATE);            /* a running session isn't started again */

    /* the options SelectSources takes */
    CsSess *o = made(&m, APP, "s2");
    CHECK(o && csm_select(&m, o, 4, 0, 0, 0) == CSM_E_ARGS);              /* virtual: not offered */
    CHECK(csm_select(&m, o, CSM_TYPES, 1, CSM_CURSOR_METADATA, 0) == CSM_E_ARGS);   /* metadata: not offered */
    CHECK(csm_select(&m, o, CSM_TYPES, 1, CSM_CURSOR_HIDDEN | CSM_CURSOR_EMBEDDED, 0) == CSM_E_ARGS);   /* one mode */
    CHECK(csm_select(&m, o, CSM_TYPES, 1, 0, 3) == CSM_E_ARGS);
    CHECK(o->st == CS_CREATED);
    CHECK(csm_select(&m, o, CSM_TYPES, 1, CSM_CURSOR_EMBEDDED, 2) == CSM_OK && o->persist == 2 && o->multiple);

    /* Cancel: back to SELECTED, and Start may be called again */
    CHECK(csm_start(&m, o, "/r/2") == CSM_OK);
    CsSess *x = made(&m, OTHER, "s1");
    CHECK(x && csm_select(&m, x, 0, 0, 0, 0) == CSM_OK);
    CHECK(csm_start(&m, x, "/r/3") == CSM_E_BUSY);          /* one chooser on screen at a time */
    CHECK(csm_chosen(&m, o, NULL, 0) == CSM_OK && o->st == CS_SELECTED && !o->req[0] && !csm_choosing(&m));
    CHECK(csm_start(&m, x, "/r/3") == CSM_OK);
    uint32_t both[2] = { CSM_TYPE_WINDOW, CSM_TYPE_MONITOR };
    CHECK(csm_chosen(&m, x, both, 2) == CSM_E_ARGS);        /* x selected monitors only, one */
    CHECK(csm_chosen(&m, x, NULL, 0) == CSM_OK);

    /* the app's own Request.Close of its Start: only that app, only that request */
    CHECK(csm_start(&m, o, "/r/4") == CSM_OK);
    CsSess *got = NULL;
    CHECK(csm_request_close(&m, OTHER, "/r/4", &got) == CSM_E_NOTFOUND);
    CHECK(csm_request_close(&m, APP, "/r/5", &got) == CSM_E_NOTFOUND);
    CHECK(csm_request_close(&m, APP, "/r/4", &got) == CSM_OK && got == o && o->st == CS_SELECTED);
    CHECK(csm_start(&m, o, "/r/6") == CSM_OK);
    CHECK(csm_chosen(&m, o, both, 2) == CSM_OK && o->st == CS_STARTING);
    CHECK(csm_request_close(&m, APP, "/r/6", &got) == CSM_OK && o->st == CS_SELECTED);   /* while the streams were made */
    /* the streams couldn't be made: back to SELECTED; only from STARTING */
    CHECK(csm_failed(o) == CSM_E_STATE);
    CHECK(csm_start(&m, o, "/r/7") == CSM_OK && csm_chosen(&m, o, &win, 1) == CSM_OK);
    CHECK(csm_failed(o) == CSM_OK && o->st == CS_SELECTED && !o->req[0]);
    CHECK(csm_started(o) == CSM_E_STATE);

    /* the same token twice, the limits */
    CHECK(made(&m, APP, "s1") == NULL);
    CHECK(made(&m, APP, "s3") != NULL);
    CHECK(made(&m, APP, "s4") != NULL);
    CHECK(made(&m, APP, "s5") == NULL);                      /* CSM_PER_APP */
    CHECK(csm_create(&m, "", "t", &got) == CSM_E_ARGS);

    /* the app leaves the bus: all of its sessions, none of the other's */
    int ids[CSM_MAX];
    int n = csm_gone(&m, APP, ids, CSM_MAX);
    CHECK(n == 4);
    for (int i = 0; i < n; i++) { CsSess *e = csm_by_id(&m, ids[i]); CHECK(e && !strcmp(e->sender, APP)); if (e) csm_end(e); }
    CHECK(csm_gone(&m, APP, ids, CSM_MAX) == 0);
    CHECK(csm_find(&m, OTHER, x->path) == x);
    CHECK(csm_by_id(&m, ids[0]) == NULL);
    csm_end(x);

    /* every slot can be used again, and no more than CSM_MAX at once */
    int made_n = 0;
    char who[16];
    for (int i = 0; i < CSM_MAX + 4; i++) {
        snprintf(who, sizeof who, ":2.%d", i);
        if (made(&m, who, "t")) made_n++;
    }
    CHECK(made_n == CSM_MAX);

    /* who asked: a verified app by its restriction; one LordOS can't tell fails closed while a restricted app runs */
    CHECK(csm_gate(1, 0, 0) == CSM_GATE_APP);
    CHECK(csm_gate(1, 0, 1) == CSM_GATE_APP);               /* another app's restriction isn't this one's */
    CHECK(csm_gate(1, 1, 0) == CSM_GATE_REFUSE);
    CHECK(csm_gate(1, 1, 1) == CSM_GATE_REFUSE);
    CHECK(csm_gate(0, 0, 0) == CSM_GATE_UNKNOWN);           /* the chooser, saying it is an unknown program */
    CHECK(csm_gate(0, 0, 1) == CSM_GATE_REFUSE);            /* it may be the restricted app, out of its scope */
    CHECK(csm_gate(0, 1, 1) == CSM_GATE_REFUSE);

    printf("castsm-test: %d/%d\n", ok, ok + bad);
    return bad ? 1 : 0;
}
