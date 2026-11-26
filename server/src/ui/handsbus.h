/* handsbus.h — org.lordos.Hands1 on the session bus (docs/HANDS.md "The bus"), served by lord-server on portal.c's
 * connection for /usr/bin/lordos-mcp. Approved by the maintainer on 2026-11-26 (AGENTS.md).
 *
 *   Hello(s client) → (s state)                       "granted" | "asking" (the system layer's prompt is up)
 *   Status() → (s json)                               the displays, the allowlist and this client's state
 *   Windows() → (s json)                              windows_context
 *   ReadWindow(s window, s filter, u depth) → (s)     read_window: the UI tree as text with ref_N ids
 *   Find(s window, s query) → (s)                     find
 *   Pointer(s action, s ref, i x, i y, i x2, i y2) → (s json)   left_click, right_click, middle_click, double_click,
 *                                                     triple_click, hover, left_click_drag; at ref or (x, y)
 *   Scroll(s ref, i x, i y, s direction, u amount)    up, down, left, right; 1..30 steps
 *   Key(s chords, u repeat)                           "ctrl+s", "Return", "ctrl+a Delete" (hands.h hands_chord)
 *   Type(s text)                                      at most HANDS_TEXT_MAX bytes
 *   SetValue(s ref, s value)                          a text field, a checkbox, a switch, a list or a pop-up
 *   Screenshot(s target, i x, i y, i w, i h, u maxdim) → (ay png, s json)   "display[:id]", "window:<id>", "region"
 *   OpenApp(s app) → (s json)  OpenPath(s path) → (s json)  CloseWindow(s window) → (s json)
 *   AppLog(s app, s pattern, u max) → (s)
 *
 * Only for the user lord-server runs as: the bus says each caller's uid (GetConnectionUnixUser) before its first call is
 * served (GetConnectionCredentials: UnixUserID), and another uid gets AccessDenied (as org.lordos.Writing1, writebus.c).
 * The same answer's ProcessFD, a pidfd of the process that opened the connection, is kept for the connection's life:
 * its program is read through it (never a pid that may have been reused), a deleted program is refused, the prompt
 * shows the path and whether it is a system program (root's, in root's folders), and every later call first checks that
 * process still lives. A bus with no ProcessFD leaves the program unknown. A grant is the connection's alone (hands.c):
 * a new connection asks again, whatever name it gives. Every argument is checked here
 * (handsbus_parse) before hands.c's policy sees it; a call with any other signature is InvalidArgs. */
#ifndef LORD_UI_HANDSBUS_H
#define LORD_UI_HANDSBUS_H

#include "../hands/hands.h"
#include "../vault/dbus.h"

#define HANDSBUS_CALLERS 8

/* a call on HANDS_BUS_PATH */
void handsbus_call(DConn *c, const DMsg *m);
/* 1 if m answered one of our uid questions */
int  handsbus_reply(DConn *c, const DMsg *m);
/* that connection left the bus */
void handsbus_gone(const char *name);
/* the bus went away */
void handsbus_lost(void);

/* the program of the process a pidfd holds: 0 (path in out, *sys root's), -1 unknown, -2 deleted; tests/hands_test.c */
int  handsbus_exe_of_pidfd(int pidfd, char *out, size_t n, int *sys);

/* ---- pure, for tests (tests/hands_test.c and its fuzz) ---- */
/* The method's arguments into r (strings borrowed from m): 0, or -1 a wrong signature, method or value. */
int  handsbus_parse(const DMsg *m, HandsReq *r);

#endif
