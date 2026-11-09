/* sqlc.h — lord-server's side of lord-sql, the MySQL / MariaDB helper (sqlproto.h,
 * docs/DATABASES.md).
 *
 * lord-server never takes a host from JS: `open` names a favorite id, and host, port, socket and
 * TLS settings are read here from ~/.config/lordos/databases/favorites.json (in fsops_protected;
 * written only by sql_favorites_save, which refuses anything that could hold a secret). A thread
 * per connection resolves and connects the socket, opens the favorite's CA / client certificate
 * files, starts lord-sql (fixed path next to this binary, no arguments, LANG only, channel on fd
 * 3, the socket on fd 4), hands it the password in memory and then reads its answers. The
 * compositor never waits: replies and events queue up and JS reads them with sql_poll()
 * (lord.sql.poll). One query per connection at a time (busy), at most SQL_MAX_CONNS connections;
 * close kills the helper. */
#ifndef LORD_SQL_SQLC_H
#define LORD_SQL_SQLC_H

#include <stddef.h>

#define SQL_MAX_CONNS      16
#define SQL_FAV_FILE_MAX   (1u << 20)
#define SQL_FAV_MAX        500         /* favorites, and groups */

int  sql_available(void);                       /* 0 if lord-sql is there and safe to run */
const char *sql_favorites_path(void);           /* ~/.config/lordos/databases/favorites.json */
/* The favorites file (malloc'd), or the empty one when it is missing or not valid. */
char *sql_favorites_read(size_t *len);
/* 1 if json is a favorites file we accept; why (optional) says what is wrong otherwise. */
int  sql_favorites_valid(const char *json, size_t len, char *why, size_t wn);
/* Validates and writes the file (0600, atomic). 0, 1 if refused, 2 if it could not be written. */
int  sql_favorites_save(const char *json, size_t len);

typedef struct {
    char id[129], type[8];
    char host[256], user[260], database[260], socket[256];
    int  port;
    char tls[12];                               /* off | preferred | required | verify */
    char ca[1024], cert[1024], key[1024];
    char trust[160];
    char iam[160];                              /* cloud.iamProfile */
} SqlFav;
/* Finds favorite id in a (valid) favorites file. 0 found, -1 not. Pure: used by the tests. */
int  sql_fav_find(const char *json, size_t len, const char *id, SqlFav *f);
/* May lord-server connect to this Unix socket (already resolved with realpath)? Not the session's
 * or the system's own sockets (runtime dir, D-Bus, systemd, X11, LordOS). */
int  sql_socket_ok(const char *real, const char *runtime_dir);

/* A request {"op":…} from JS. → request id (its answer comes through sql_poll), or -1 when
 * lord-sql is not available. */
int  sql_request(const char *json, size_t len);

typedef struct { int id; int type; char *data; size_t len; } SqlItem;   /* type: SQL_OK, SQL_ERR, SQL_EVT */
enum { SQL_OK = 0, SQL_ERR = 1, SQL_EVT = 2 };
/* Takes every queued reply and event (malloc'd array; free with sql_items_free). */
int  sql_poll(SqlItem **items);
void sql_items_free(SqlItem *items, int n);

#endif
