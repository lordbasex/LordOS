/* pwguard.h — the firewall's "no microphone" and "no camera" for Linux apps inside PipeWire
 * (docs/FIREWALL.md, "Linux apps").
 *
 * Linux apps talk to PipeWire directly (natively, or through pipewire-pulse), so the kernel's
 * device rules don't cover the microphone (PipeWire opens the sound card, not the app) nor a
 * camera that PipeWire serves. lord-server keeps one connection of its own to the session's
 * PipeWire, on the manager socket ($XDG_RUNTIME_DIR/pipewire-0-manager), with our own
 * implementation of the native protocol (no libpipewire). For every PipeWire client it works out
 * the app (the PID's cgroup → the launcher's scope → desktop:<id>) and, if that app is
 * restricted (sec/restrict.h):
 *   - hides from that client every capture node: Audio/Source* and Audio/Duplex for "no
 *     microphone", Video/Source for "no camera", a shared screen or window (lord-screencast's
 *     Video/Source, media.role Screen) for "no screen capture", and their ports (permission 0, so it
 *     can neither see nor link them), and every other client (so it can't change anyone's permissions);
 *   - destroys every capture stream that client creates (Stream/Input/Audio, Stream/Input/Video),
 *     as soon as it appears.
 * The rules are lord.fw's store (the one Settings edits), read every second: a restriction set or
 * lifted on a running app applies within a second, without restarting it. Nothing is written to a
 * file the apps could change.
 *
 * Who a client is: PipeWire's own pipewire.sec.pid (from the socket's credentials) for native
 * clients. Apps using the PulseAudio API all reach PipeWire through pipewire-pulse's process, so
 * for those the PID is the application.process.id the app itself reported to pipewire-pulse (see
 * the documentation for what that means). */
#ifndef LORD_SEC_PWGUARD_H
#define LORD_SEC_PWGUARD_H

#include <stddef.h>
#include <stdint.h>

int  pwguard_start(void);       /* 0 connected; retried by pwguard_tick if PipeWire isn't up */
int  pwguard_fd(void);          /* -1 if not connected */
int  pwguard_wants_write(void);
void pwguard_dispatch(void);
void pwguard_tick(void);        /* every pass of the main loop; does work once a second */
/* The capture indicator (sdk/capturestatus.jsx): emit("capture-use", json) whenever what Linux apps capture through
 * PipeWire changes. A capture stream counts once a microphone, a camera or a shared screen is linked into it; its owner
 * is worked out as for the restrictions, a pulse client's own word always checked; "" an unknown program. lord-server's
 * own streams are left out (the shell shows them under the app they are for). With no verified app, it is still named
 * when the kernel can tell, only for the indicator (appid.h, appid_of_lineage; sandbox_owner): a process that left its
 * app's scope (Chrome's browser and audio service move into the user's manager), a pulse client in a sandbox of its own
 * (WebKit's in LordOS's web apps), the sound system's own processes ("sys":"sound"); else its executable ("exe"). */
void pwguard_on_use(void (*emit)(const char *kind, const char *json));

/* ---- for tests (tests/pwguard_test.c) ---- */
/* A SPA POD reader over buf: the n-th field of a Struct and its kind. */
typedef struct { const uint8_t *p; size_t n; } PwPod;
int  pwpod_struct_field(PwPod s, int index, PwPod *out);     /* 0 ok */
int  pwpod_int(PwPod p, int32_t *v);
int  pwpod_string(PwPod p, const char **s);
/* Which restriction makes a node a capture node to hide ("Audio/Source" → FW_R_NO_MIC...), 0 none. */
uint32_t pwguard_capture_flag(const char *media_class);
/* The same for a stream an app creates to capture ("Stream/Input/Audio" → FW_R_NO_MIC). */
uint32_t pwguard_stream_flag(const char *media_class);
/* A node's flag from its global's properties: a screen shared with an app (the ScreenCast portal's lord-screencast: a
 * Video/Source whose media.role is Screen, or named lordos-screencast-*) is FW_R_NO_SCREEN, not a camera; else as
 * pwguard_capture_flag. Any app's node that says so is hidden from no-screen apps, which only hides it more. */
uint32_t pwguard_node_flag(const char *media_class, const char *media_role, const char *node_name);
/* 1 for the capture stream of LordOS Studio's Noise Reduction filter (lordos-voice.conf: an audio capture stream named
 * "lordos_voice_capture"). Its link from the microphone isn't an app's use (the app reading the filter's source is);
 * pwguard.c only skips it when the node is the session's pipewire daemon's own, never an app's. */
int pwguard_voice_capture(const char *media_class, const char *node_name);
/* The capture indicator's JSON from (app, flags) pairs, one entry per app; with no app, one per "sys" (the sound
 * system's own processes: "sound") or "exe" (the executable of a program LordOS can't name): 0, or -1 when it doesn't
 * fit or a field isn't plain. */
typedef struct { char app[128]; uint32_t flags; char exe[32]; char sys[16]; } PwUse;
int pwguard_use_format(const PwUse *u, int n, char *out, size_t cap);
/* A sandboxed pulse client's app, only to name it (pwguard.c sandbox_owner): among the processes with the inner pid it
 * claims, the one app whose processes hold at least `claims` connections to pipewire-pulse. 0, or -1 when none or two
 * apps could be it. */
typedef struct { char app[128]; int conns; } PwCand;
int pwguard_sandbox_pick(const PwCand *c, int n, int claims, char *out, size_t cap);

#endif
