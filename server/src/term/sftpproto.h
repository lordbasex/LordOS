/* sftpproto.h — lord-server ↔ lord-sftp, the Terminal's Files mode helper (docs/TERMINAL.md "Files
 * mode"). The channel and the framing are lord-doc's (doc/wire.h: header + JSON + fds over a
 * SOCK_SEQPACKET pair on fd 3). fd 4 is what the server sends (the ssh channel's stdout), fd 5 what
 * it reads (its stdin): an `sftp` subsystem lord-server opened on the tab's own ssh connection
 * (sshmux.c). lord-sftp never opens a socket or a file: a download's or an upload's local file is
 * an fd lord-server opened and passes with the request. It applies sandbox_apply_helper before
 * reading a byte from the server.
 *
 *   SP_LIST     {"path"}                → {"path" (canonical),"entries":[{name,kind,size,mtime,perm,uid,gid,owner,group}],"truncated"}
 *   SP_STAT     {"path"}                → {"path","kind","size","mtime","perm","uid","gid"}
 *   SP_MKDIR    {"path","perm"?}        → {}
 *   SP_REMOVE   {"path","dir"?}         → {}    (dir: an empty folder)
 *   SP_RENAME   {"from","to"}           → {}
 *   SP_CHMOD    {"path","perm"}         → {}
 *   SP_CHOWN    {"path","uid","gid"}    → {}
 *   SP_GET      {"path"} + fd (write)   → {"bytes"}   SE_PROGRESS {"done","total"} while it runs
 *   SP_PUT      {"path","size","perm"?} + fd (read) → {"bytes"}
 *   SP_END      {}: exit
 *
 *   SE_OK       the reply's JSON;  SE_ERR {"error":"nofile|denied|failed|closed|bad|toolarge|unsupported","message"}
 *   SE_PROGRESS {"done","total"} (at most every 100 ms)
 *
 * One request at a time, answered in order. Paths are absolute, checked here and again by the
 * helper (sf_path_ok). Everything the server sends is untrusted: names with '/' or control
 * characters are left out of a listing, owner and group names are checked, sizes are bounded. */
#ifndef LORD_TERM_SFTPPROTO_H
#define LORD_TERM_SFTPPROTO_H

enum { SP_LIST = 1, SP_STAT = 2, SP_MKDIR = 3, SP_REMOVE = 4, SP_RENAME = 5, SP_CHMOD = 6, SP_CHOWN = 7,
       SP_GET = 8, SP_PUT = 9, SP_END = 10 };
enum { SE_OK = 101, SE_ERR = 102, SE_PROGRESS = 103 };

#define SFTP_LIST_MAX   20000          /* entries in one listing, then "truncated" */

#endif
