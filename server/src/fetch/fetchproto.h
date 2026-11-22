/* fetchproto.h — lord-server ↔ lord-fetch, the blocklist downloader (0.8.5, docs/FIREWALL.md,
 * "Blocklists"). The channel and the framing are lord-doc's (doc/wire.h: header + JSON + fds over a
 * SOCK_SEQPACKET pair on fd 3).
 *
 *   FQ_GET   {"host":"example.org","path":"/list.txt","format":"hosts|domains|ips","sub":0|1}
 *            + 1 fd: a TCP socket lord-server already connected (the blocklists' connector, in
 *            lordos-net-lists.scope) to host:443
 *   FQ_WEATHER {"lat":-34.6,"lon":-58.45} + 1 fd: a TCP socket lord-server already connected (the
 *            weather connector, lordos-net-weather.scope) to api.open-meteo.com:443 (0.8.8.10, the
 *            weather widget; fetch/weather.h). The helper builds the path from the two numbers itself.
 *   FQ_FLATHUB {"list":N} + 1 fd: a TCP socket lord-server already connected (the flathub connector,
 *            lordos-net-flathub.scope) to flathub.org:443 (E10, the Store's rankings; fetch/flathub.h). N picks one
 *            of a closed list of paths; the helper builds the path (and the picks' date, from its clock) itself.
 *   FQ_END   {}: the helper exits
 *
 *   FR_DONE      {"domains":N,"nets":N,"skipped":N,"bad":N,"dups":N,"truncated":0|1,"bytes":N,
 *                 "lastModified":"…"} + 1 sealed memfd with the normalized lines (fw/fwlists.h)
 *   FR_REDIRECT  {"location":"https://…"} (lord-server checks it and connects again; at most 3)
 *   FR_WEATHER   the forecast in weather.h's compact JSON (numbers only); no redirects followed
 *   FR_FLATHUB   {"ids":["org.example.App",…]}: Flatpak app ids only (fpak_id_ok, at most 250, Flathub's order);
 *                no redirects followed, nothing else of the answer
 *   FR_ERR       {"error":"tls|cert|http|status|big|nul|line|format|empty|oom|redirect|bad-request","detail":"…"}
 *
 * lord-fetch never opens a socket or a file: it loads the CA bundle, locks itself in with
 * sandbox_apply_helper (no socket/connect/exec, no files), speaks TLS 1.2+ (certificate checked
 * against the system CA store and the host name, never relaxed) and HTTP/1.1 GET with a size
 * limit, and parses what came strictly (fwl_parse_raw). lord-server validates every line again
 * before it compiles the lists. */
#ifndef LORD_FETCH_PROTO_H
#define LORD_FETCH_PROTO_H

enum { FQ_GET = 1, FQ_END = 2, FQ_WEATHER = 3, FQ_FLATHUB = 4 };
enum { FR_DONE = 101, FR_REDIRECT = 102, FR_ERR = 103, FR_WEATHER = 104, FR_FLATHUB = 105 };

#endif
