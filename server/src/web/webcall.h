/* webcall.h — the call log of a web app (docs/WEBAPPS.md, "Calls"), pure (tests/webua_test.c).
 *
 * So a failed voice or video call can be told apart (no WebRTC, no microphone, ICE that never connects, a codec), lord-web
 * writes short lines starting with "webcall: " to its own log (web/<app>-<profile>.log in the user's log folder, which
 * the Logs app and lordos-issue show). Always on, at a low volume: what happened, never what was said or who to.
 *
 * What is logged: whether the page has what calls need (crossOriginIsolated, SharedArrayBuffer, Atomics,
 * RTCPeerConnection: a messaging web app checks exactly these), each RTCPeerConnection the page creates and closes,
 * its ICE gathering, ICE connection, connection and signaling states, when it connects the codecs and the video's
 * size and frame rate of each direction and the kinds of ICE candidates of the pair in use (host, srflx, prflx,
 * relay), errors by their names (and the call that failed), getUserMedia and getDisplayMedia (audio, video, the
 * result, the error's name, the kind of each device in closed words), the permission answers, WebKit's capture
 * state, and the Client Hints the app's own site asked for.
 *
 * What never is: SDP, ICE candidates, addresses or ports, tokens, identifiers, phone numbers, names, device labels,
 * other sites' addresses, message content.
 * The page side (webcall.js) sends only a closed set of fields; this file checks each against its own closed list
 * or pattern again and drops anything else, so a page that forges messages can at most write well-formed lines about
 * calls into its own log, at most WEBCALL_RATE of them a minute. */
#ifndef LORD_WEB_WEBCALL_H
#define LORD_WEB_WEBCALL_H

#include <stddef.h>

#define WEBCALL_MSG_MAX 2048            /* a message from the page longer than this is dropped */
#define WEBCALL_RATE    60              /* lines a minute, per lord-web; the rest dropped (one line says how many) */

/* The line (without "webcall: " and the newline) for one message of webcall.js (JSON), into out: its length, 0 when the
 * message is dropped (not JSON, an unknown kind, a field out of its list or pattern). */
size_t webcall_line(const char *json, size_t len, char *out, size_t n);

/* The app's main document's Document-Isolation-Policy for the "page:" line, in closed words: "none" (no header, or
 * "none"), "isolate-and-require-corp", "isolate-and-credentialless" or "other" (anything else; never the value itself).
 * webcall_dip_word is pure; webcall_set_dip keeps the word of the main frame's last response (lord-web's response
 * policy decision), which webcall_line adds to the "page:" line as ", DIP <word>". The engine never tells the page its
 * own headers, so this comes from lord-web, not from webcall.js. */
const char *webcall_dip_word(const char *header);
void webcall_set_dip(const char *header);

/* the kind of device a captured track comes from, in closed words, never its label: "lordos-studio" (LordOS Studio's
 * virtual camera), "virtual", "bluetooth", "usb", "built-in", "other", "unnamed" (no label), and "screen" for
 * getDisplayMedia. The label (a device's name, which can be a person's name or a serial number) is only read here. */
const char *webcall_device_class(const char *op, const char *label);

/* 1 while another line may be written at now_s (seconds), counting it; *dropped gets how many were dropped since the
 * last line that went out (to say so once) */
int webcall_rate(double now_s, unsigned *dropped);

/* the Client Hints the app's own site asked for, as a line: "client hints asked by the app: sec-ch-ua-arch, ..." (no
 * origin: only the app's own origin is ever recorded, webhints_response) */
size_t webcall_hints_line(unsigned mask, char *out, size_t n);
/* Whether a main frame's response at uri may have its Accept-CH kept and logged: only the app's own site, an address
 * inside scope whose origin is app_origin's: 0 and that origin in out, else -1 (another site: nothing is kept or
 * written about it). */
int webcall_hints_site(const char *uri, const char *scope, const char *app_origin, char *out, size_t n);

/* lord-web's side (webcall.c, not pure): write one line, rate limited */
void webcall_log(const char *line);
void webcall_log_hints(unsigned mask);

#endif
