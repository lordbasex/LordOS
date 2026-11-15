/* webinspect.h — web apps' developer tools (docs/WEBAPPS.md, "Developer tools"): the pure parts, shared by lord-server
 * (web/webmgr.c) and lord-web (webmain.c, webengine.c). Tests: tests/webapp_test.c.
 *
 *   - Developer mode is one switch for all web apps (Settings → Web Apps), off by default, kept in
 *     ~/.config/lordos/web/developer.json as {"developerMode":true} (the folder is in fsops_protected: only lord.webapps
 *     writes it). Anything else, a missing file or one that isn't valid, is off.
 *   - WPE WebKit 2.48 has no inspector of its own in the window and can't turn remote inspection on for a view that
 *     already runs: its remote inspector starts only with the process, from WEBKIT_INSPECTOR_HTTP_SERVER=<ip>:<port>
 *     (read once, when the first web context is made; port 0 is refused). So "Inspect" starts that app and profile's
 *     lord-web again with --inspect (webargs.h), which lord-web accepts only while developer mode is on. It then binds
 *     a free port on 127.0.0.1 to learn its number, hands it to WebKit as 127.0.0.1:<port>, turns the view's developer
 *     extras on, and once that port listens from one of its own sockets (/proc/self/net/tcp against /proc/self/fd),
 *     opens http://127.0.0.1:<port>/ in the person's browser through OpenURI. It ends with the window.
 *   - WebKit's inspector server has no token or secret path: while it is on, any process on this computer that can
 *     reach 127.0.0.1 can connect to it. That is why it is per window, after asking, and ends with the window. */
#ifndef LORD_WEB_WEBINSPECT_H
#define LORD_WEB_WEBINSPECT_H

#include <stddef.h>

#define WEBINSPECT_FILE    "developer.json"             /* in ~/.config/lordos/web */
#define WEBINSPECT_FILE_MAX 256
#define WEBINSPECT_ENV     "WEBKIT_INSPECTOR_HTTP_SERVER"
#define WEBINSPECT_HOST    "127.0.0.1"

/* 1 when text is {"developerMode":true} (other keys are ignored), else 0 */
int webinspect_parse(const char *text, size_t len);
/* the file's text for on (0 or 1) into out; its length, or 0 when it doesn't fit */
size_t webinspect_write(int on, char *out, size_t n);

/* a port lord-web may hand to WebKit: 1024..65535 */
int webinspect_port_ok(unsigned port);
/* "127.0.0.1:<port>" (WEBINSPECT_ENV's value) and "http://127.0.0.1:<port>/" (what the browser opens); 0, or -1 for a
 * port that isn't webinspect_port_ok or a buffer too small */
int webinspect_address(unsigned port, char *out, size_t n);
int webinspect_url(unsigned port, char *out, size_t n);

/* Who listens on 127.0.0.1:port in /proc/net/tcp's text (len bytes)? inodes: the sockets of this process (n of them).
 *   1   a listening socket on that port, every one of them ours, and at least one on 127.0.0.1
 *   0   none listens on it yet
 *  -1   a socket that isn't ours listens on that port (on any address), or one of ours on another address than 127.0.0.1 */
int webinspect_listening(const char *tcp, size_t len, unsigned port, const unsigned long *inodes, int n);

#endif
