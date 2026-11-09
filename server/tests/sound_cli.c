/* sound_cli.c — manual test of the sound client (sound/snd.c) against the real PipeWire.
 *
 *   sound-cli list                     full state (JSON)
 *   sound-cli volume sink 36 0.5       volume (also source, input, output; or a list: 0.5,0.8)
 *   sound-cli mute input 51 1
 *   sound-cli default sink <name>
 *   sound-cli port sink 36 <port>
 *   sound-cli move input 51 40
 *   sound-cli profile 57 a2dp-sink     a Bluetooth card's profile (state "cards")
 *   sound-cli meter <input> [seconds]
 *   sound-cli play pop|notify|error
 *   sound-cli watch [seconds]          prints every "state" (to see changes made by others) */
#include "../src/sound/snd.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int states, peaks;
static double peak_max;
static int watch;

static void on_event(const char *type, const char *json) {
    if (!strcmp(type, "state")) {
        states++;
        if (watch) { printf("state %s\n", json); fflush(stdout); }
    } else if (!strcmp(type, "peak")) {
        peaks++;
        double l = atof(strstr(json, "level\":") + 7);
        if (l > peak_max) peak_max = l;
        if (watch) printf("peak %s\n", json);
    }
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static void run_for(double secs) {
    double end = now() + secs;
    while (now() < end) {
        struct pollfd p = { snd_fd(), POLLIN, 0 };
        poll(&p, 1, 50);
        if (p.revents) snd_dispatch();
        snd_events(on_event);
    }
}

static int kind_of(const char *s) {
    return !strcmp(s, "sink") ? SND_SINK : !strcmp(s, "source") ? SND_SOURCE : !strcmp(s, "input") ? SND_INPUT : !strcmp(s, "output") ? SND_OUTPUT : -1;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: sound-cli list|volume|mute|default|port|move|profile|meter|play|watch ...\n"); return 2; }
    snd_start();
    for (int i = 0; i < 60 && !states; i++) run_for(0.05);
    if (!states) { fprintf(stderr, "no state (is PipeWire running?)\n"); return 1; }
    const char *cmd = argv[1];
    int r = 0;
    if (!strcmp(cmd, "list")) {
    } else if (!strcmp(cmd, "volume") && argc >= 5) {
        double v[32];
        int n = 0;
        for (char *p = argv[4]; n < 32 && *p;) { v[n++] = strtod(p, &p); if (*p == ',') p++; else break; }
        r = n == 1 ? snd_set_level(kind_of(argv[2]), (uint32_t)atoi(argv[3]), v[0])
                   : snd_set_volume(kind_of(argv[2]), (uint32_t)atoi(argv[3]), v, n);
    } else if (!strcmp(cmd, "mute") && argc >= 5) r = snd_set_mute(kind_of(argv[2]), (uint32_t)atoi(argv[3]), atoi(argv[4]));
    else if (!strcmp(cmd, "default") && argc >= 4) r = snd_set_default(kind_of(argv[2]), argv[3]);
    else if (!strcmp(cmd, "port") && argc >= 5) r = snd_set_port(kind_of(argv[2]), (uint32_t)atoi(argv[3]), argv[4]);
    else if (!strcmp(cmd, "profile") && argc >= 4) r = snd_set_profile((uint32_t)atoi(argv[2]), argv[3]);
    else if (!strcmp(cmd, "move") && argc >= 5) r = snd_move(kind_of(argv[2]), (uint32_t)atoi(argv[3]), (uint32_t)atoi(argv[4]));
    else if (!strcmp(cmd, "meter") && argc >= 3) {
        r = snd_meter((uint32_t)atoi(argv[2]));
        run_for(argc >= 4 ? atof(argv[3]) : 2);
        printf("peaks: %d, max %.4f\n", peaks, peak_max);
        snd_meter(0xffffffffu);
    } else if (!strcmp(cmd, "play") && argc >= 3) { r = snd_play(argv[2]); run_for(1); }
    else if (!strcmp(cmd, "watch")) { watch = 1; run_for(argc >= 3 ? atof(argv[2]) : 10); }
    else { fprintf(stderr, "invalid command\n"); return 2; }
    if (r) fprintf(stderr, "the command failed (%d)\n", r);
    run_for(0.4);
    char *j = snd_state_json();
    printf("%s\n", j);
    free(j);
    snd_stop();
    return r != 0;
}
