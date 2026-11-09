# Databases

Databases (`apps/databases/`, 0.8.8.7 in development) is the MySQL / MariaDB client: favorites, a table's
structure, content, relations, triggers and information, a query editor, a console, server processes, users,
import and export. Its screens follow the approved mockups (Connect, Structure, Content, Query, Relations,
Triggers, Table Info, Users, Console, Processes, Export, Import, Sheets, Preferences). It reads Sequel Ace's and
Sequel Pro's favorites from a Mac disk or home folder (see "Importing").

## How it connects

```
apps/databases  ──lord.sql.request/poll──▶  lord-server (ui/js_sql.c, sql/sqlc.c)
                                              │ reads the favorite by id from favorites.json
                                              │ resolves it; the sql connector (lordos:sql in the
                                              │ firewall) makes the TCP socket; a Unix socket is local
                                              ▼
                                           lord-sql (sql/lordsql.c), one process per connection,
                                           sandboxed, no sockets of its own: gets the connected fd,
                                           speaks the MySQL protocol, TLS (OpenSSL), auth plugins
```

- **lord-server never takes a host from JS.** `open` names a favorite id; lord-server reads host, port, socket
  and TLS settings from `~/.config/lordos/databases/favorites.json`. Quick Connect saves a favorite marked
  `quick: true` first. The folder is in `fsops_protected`, and the file is written only by
  `lord.sql.saveFavorites`, which validates it and refuses any key that could hold a secret.
- **lord-sql** is started like `lord-maild`: fixed path (`<lord-server dir>/lord-sql`, owned by root or the
  user, not writable by others), no arguments, `LANG` only, channel on fd 3, the connected socket on fd 4. It
  loads the CA bundle and calls `sandbox_apply_helper` (scope `lordos-sql`; no socket, connect, bind or exec;
  no files) before it reads a byte from the server. It links OpenSSL (TLS starts in the middle of the MySQL
  handshake, and `caching_sha2_password` without TLS needs RSA), which makes it the fifth binary to do so.
- **One connection, one process.** Closing or cancelling kills it; a crash only closes that tab.
- **Passwords** live in the keyring (`databases:<favorite id>`, account = user), reach lord-sql in memory in
  the `open` request, and are never written to disk, the console or logs.
- **Certificates** are checked against the system store (or the favorite's CA file, opened by lord-server)
  and the host name. A failure is an event; only an explicit Trust stores that sha256 in the favorite.
- **The compositor never waits**: every call is asynchronous; results are polled.
- **Not in the broker**: isolated `.lpk` apps get no `lord.sql`.

## lord.sql (native)

| Call | |
| --- | --- |
| `available()` | lord-sql is installed |
| `favorites()` | the favorites file as a JSON string (`{"version":1,"groups":[],"favorites":[]}` when missing) |
| `saveFavorites(json)` | validates and writes it (0600, atomic); throws on a bad file |
| `request(json)` | → request id, or -1; `json` is `{"op": …, …}` |
| `poll()` | → `[{ id, type: 'ok'\|'err'\|'evt', data }]` |

Requests:

| op | arguments | reply |
| --- | --- | --- |
| `open` | `fav`, `password?`, `database?` | `{ conn, server: { version, flavor: 'mariadb'\|'mysql' }, threadId, tls: { version, cipher, verified } \| null, db }` |
| `query` | `conn`, `sql`, `maxRows?` (default 1000, at most 200000) | `{ results: [Result], ms }` |
| `use` | `conn`, `db` | `{}` |
| `ping` | `conn` | `{ ms }` |
| `close` | `conn` | `{}` |

`Result` is one per statement: `{ columns: [Column], rows: [[string \| null]], affected, insertId, warnings,
info, truncated }`. A statement without rows has `columns: []`. `Column` is `{ name, orgName, table, orgTable,
db, type (protocol number), typeName ('INT', 'VARCHAR', …), length, decimals, charset, flags: { notNull, pk,
unique, multiKey, unsigned, zerofill, binary, autoInc, blob } }`. Binary values (charset 63) come as
`"\u0000hex:<hex>"`. A result over 64 MB or `maxRows` stops reading, sets `truncated` and discards the rest.

Errors (`data.error`): `unavailable`, `notfound` (favorite), `dns`, `connect`, `tls`, `cert`, `auth`, `sql`
(with `errno`, `sqlstate`, `message`), `closed`, `busy` (a query is running), `toolarge`, `unsupported`.

Events: `{ ev: 'closed', conn, reason }`, `{ ev: 'cert', conn, fav, sha256, subject, issuer, problem }`.

One query runs per connection at a time (`busy` otherwise); at most 16 connections.

## favorites.json

```json
{ "version": 1,
  "groups": [{ "id": "g1", "name": "Clients", "parent": null, "expanded": true }],
  "favorites": [{
    "id": "prod-shop", "name": "prod-shop", "group": "g1", "color": 0,
    "type": "tcp", "host": "db.example.net", "port": 3306, "user": "shop_admin", "database": "shop",
    "socket": "", "ssh": { "host": "", "port": 22, "user": "", "key": "" },
    "tls": { "mode": "preferred", "ca": "", "cert": "", "key": "" },
    "cloud": { "service": "", "region": "", "iamProfile": "" },
    "readOnly": false, "production": true, "timeZone": "", "trust": "",
    "imported": { "from": "sequel-ace", "id": "…" }, "quick": false }] }
```

`type`: `tcp`, `socket`, `ssh`, `cloud`. `tls.mode`: `off`, `preferred` (encrypt when the server offers it),
`required`, `verify` (required, and the certificate and host name must check). `color`: 0–6 (red, orange,
yellow, green, blue, purple, gray) or null. Phase 1 opens `tcp`, `socket` and `cloud` with a password;
`ssh` and IAM tokens answer `unsupported` until phase 2.

## Importing

The app looks for Sequel Ace (`Library/Containers/com.sequel-ace.sequel-ace/Data/Library/Application
Support/Sequel Ace/Data/Favorites.plist`) and Sequel Pro (`Library/Application Support/Sequel
Pro/Data/Favorites.plist`) under the home folder and under `/media/<user>/*/Users/*/`, and offers to import
what it finds on first run (and from File > Import Connections). The XML plist is parsed in JS
(`apps/databases/plist.js`, tested): groups (`Favorites Root` → `Children`), name, host, port, user, database,
socket, type (0 standard, 1 socket, 2 SSH), SSH host/user/port/key, SSL files, color, AWS IAM profile and
region. Passwords stay in the Mac's keychain, which can't be read here: the first connection asks for each one
and offers to keep it in Passwords.

## Phases

1. lord-sql and `lord.sql`; the app: Connect (Standard, Socket, Cloud with a password), favorites and groups,
   the import, the table list, Structure (read, and field edits shown as ALTER before applying), Content
   (filter, sort, pages, edit a cell, add and delete rows), Query (run current, run all, results, messages,
   history), Table Info, Console.
2. SSH tunnel (`ssh -W` run by lord-server), IAM tokens, Relations and Triggers editing, Users, Processes,
   Export and Import, query favorites and history from Sequel Ace, Preferences.
