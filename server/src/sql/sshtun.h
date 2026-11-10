/* sshtun.h — the SSH tunnel of a Databases favorite (docs/DATABASES.md, "SSH tunnel"), the part without a process:
 * what lord-server checks and builds before it starts /usr/bin/ssh, and how it reads ssh's errors. Pure (tests/sqlc_test.c).
 *
 * The tunnel is `ssh -W <db host>:<db port>` to the favorite's SSH server: its stdin and stdout are the stream lord-sql
 * speaks MySQL over. ssh never reads the user's ~/.ssh/config (-F none: a ProxyCommand there would run anything) and
 * accepts only the server key kept in the favorite (ssh.hostKey, written to a private known_hosts file for this one
 * connection). The first connection has no key yet: a probe run that stops before authenticating fetches it, the person
 * sees its SHA-256 fingerprint, and only a key they accept is kept. */
#ifndef LORD_SQL_SSHTUN_H
#define LORD_SQL_SSHTUN_H

#include <stddef.h>

#define SSH_BIN          "/usr/bin/ssh"
#define SSH_KEY_MAX      1000          /* "ssh-ed25519 AAAA…": a type, a space and base64 */
#define SSH_ARGV_MAX     80

/* 1 if s is a server key as kept in a favorite: one of the key types ssh offers, one space, base64 of a blob that
 * starts with that same type name. */
int ssh_hostkey_ok(const char *s);
/* "SHA256:<base64 without padding>" of the key's blob, as ssh prints fingerprints. 0, or -1 for a bad key. */
int ssh_fingerprint(const char *key, char *out, size_t cap);
/* The known_hosts line for host:port and the key ("host key" or "[host]:port key"). 0, or -1 if it doesn't fit. */
int ssh_known_line(const char *host, int port, const char *key, char *out, size_t cap);
/* The key out of a known_hosts line written by ssh for one host (the type and the base64, without the host part):
 * 0, or -1 when the line has none. */
int ssh_key_of_line(const char *line, char *out, size_t cap);

typedef struct {
    const char *ssh_host, *ssh_user;   /* the SSH server */
    int ssh_port;
    const char *db_host;               /* the database, as the SSH server sees it */
    int db_port;
    const char *key_path;              /* the private key, already resolved and checked */
    const char *known_hosts;           /* this connection's known_hosts file */
    int probe;                         /* 1: only fetch the server's key (accept-new, no authentication, no -W) */
} SshTun;
/* ssh's argv into argv (NULL-terminated, pointing into buf): the fixed options, then "--" and the server. -1 when an
 * argument doesn't fit or a host could pass for an option. */
int ssh_argv(const SshTun *t, char *buf, size_t cap, char **argv, int max);

/* What went wrong, from ssh's stderr: "hostkey" (the server's key changed or is not the kept one), "sshauth" (the key
 * was refused), "dns", "connect", "sshforward" (the server would not open the database's port), "sshkey" (the private
 * key could not be used) or "ssh". */
const char *ssh_err_code(const char *stderr_text);

#endif
