/* sqlproto.h — lord-server ↔ lord-sql, the MySQL / MariaDB helper (docs/DATABASES.md). The channel
 * and the framing are lord-doc's (doc/wire.h: header + JSON + fds over a SOCK_SEQPACKET pair on
 * fd 3). One lord-sql per connection: fd 4 is the TCP or Unix socket lord-server already connected
 * to the favorite's server (sql/sqlc.c). lord-sql never opens a socket or a file.
 *
 *   SQ_OPEN   {"user","password","database","host","tls":"off|preferred|required|verify","trust",
 *              "ca":0|1,"cert":0|1,"key":0|1} + the fds flagged with 1, in that order (files
 *              lord-server opened). The first message, and the only one before the sandbox: the
 *              helper loads the CA bundle (or "ca"), applies sandbox_apply_helper and only then
 *              reads the server's greeting. Answered with SR_OK {server, threadId, tls, db} or
 *              SR_ERR; a certificate that fails in verify mode first sends SR_EVT (cert).
 *   SQ_QUERY  {"sql","maxRows"} → SR_OK {"results":[Result],"ms"} (Result as in docs/DATABASES.md)
 *   SQ_USE    {"db"}            → SR_OK {}
 *   SQ_PING   {}                → SR_OK {"ms"}
 *   SQ_END    {}: COM_QUIT and exit
 *
 *   SR_OK     the reply's JSON
 *   SR_BIG    the reply's JSON in a sealed memfd (fd 0; past DOC_INLINE_MAX; up to SQL_REPLY_MAX)
 *   SR_ERR    {"error":"tls|cert|auth|sql|closed|toolarge|unsupported|…", errno, sqlstate, message}
 *   SR_EVT    {"ev":"cert","sha256","subject","issuer","problem"} (lord-server adds conn and fav)
 *
 * Requests are answered one at a time, in order; lord-server sends the next one only after the
 * answer (busy otherwise). While idle the helper also watches the server: anything it says then
 * (a timeout's ERR, the end of the connection) ends the helper, and lord-server reports closed. */
#ifndef LORD_SQL_PROTO_H
#define LORD_SQL_PROTO_H

enum { SQ_OPEN = 1, SQ_QUERY = 2, SQ_USE = 3, SQ_PING = 4, SQ_END = 5 };
enum { SR_OK = 101, SR_BIG = 102, SR_ERR = 103, SR_EVT = 104 };

#define SQL_REPLY_MAX   ((size_t)66 << 20)     /* 64 MB of results + the envelope */
#define SQL_RESULT_MAX  ((size_t)64 << 20)     /* the JSON of a query's results, then truncated */
#define SQL_ROWS_DEFAULT 1000
#define SQL_ROWS_MAX     200000
#define SQL_TEXT_MAX    ((size_t)16 << 20)     /* the text of one query (doc/wire's limit) */

#endif
