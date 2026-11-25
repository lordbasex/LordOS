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
 * result, the error's name, the devices' labels), the permission answers, and the Client Hints a site asked for.
 *
 * What never is: SDP, ICE candidates, addresses or ports, tokens, identifiers, phone numbers, names, message content.
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

/* a device label as it may be logged: letters, digits, space and ._()-:, at most 48 bytes, a stretch of digits,
 * spaces and dashes holding 5 or more digits (a serial number, a phone number) dropped; "" when nothing is left */
void webcall_label(const char *in, char *out, size_t n);

/* 1 while another line may be written at now_s (seconds), counting it; *dropped gets how many were dropped since the
 * last line that went out (to say so once) */
int webcall_rate(double now_s, unsigned *dropped);

/* the Client Hints a site asked for, as a line: "client hints asked by https://example.org: sec-ch-ua-arch, ..."
 * (an origin whose host is an IP address is written as "an address") */
size_t webcall_hints_line(const char *origin, unsigned mask, char *out, size_t n);

/* lord-web's side (webcall.c, not pure): write one line, rate limited */
void webcall_log(const char *line);
void webcall_log_hints(const char *origin, unsigned mask);

#endif
