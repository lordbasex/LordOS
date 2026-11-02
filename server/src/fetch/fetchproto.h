/* fetchproto.h — lord-server ↔ lord-fetch, the blocklist downloader (0.8.5, docs/FIREWALL.md,
 * "Blocklists"). The channel and the framing are lord-doc's (doc/wire.h: header + JSON + fds over a
 * SOCK_SEQPACKET pair on fd 3).
 *
 *   FQ_GET   {"host":"example.org","path":"/list.txt","format":"hosts|domains|ips","sub":0|1}
 *            + 1 fd: a TCP socket lord-server already connected (the blocklists' connector, in
 *            lordos-net-lists.scope) to host:443
 *   FQ_END   {}: the helper exits
 *
 *   FR_DONE      {"domains":N,"nets":N,"skipped":N,"bad":N,"dups":N,"truncated":0|1,"bytes":N,
 *                 "lastModified":"…"} + 1 sealed memfd with the normalized lines (fw/fwlists.h)
 *   FR_REDIRECT  {"location":"https://…"} (lord-server checks it and connects again; at most 3)
 *   FR_ERR       {"error":"tls|cert|http|status|big|nul|line|format|empty|oom|bad-request","detail":"…"}
 *
 * lord-fetch never opens a socket or a file: it loads the CA bundle, locks itself in with
 * sandbox_apply_helper (no socket/connect/exec, no files), speaks TLS 1.2+ (certificate checked
 * against the system CA store and the host name, never relaxed) and HTTP/1.1 GET with a size
 * limit, and parses what came strictly (fwl_parse_raw). lord-server validates every line again
 * before it compiles the lists. */
#ifndef LORD_FETCH_PROTO_H
#define LORD_FETCH_PROTO_H

enum { FQ_GET = 1, FQ_END = 2 };
enum { FR_DONE = 101, FR_REDIRECT = 102, FR_ERR = 103 };

#endif
