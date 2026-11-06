# Cloud

The design of **Cloud**, LordOS's layer over the person's cloud storage accounts: every account in one place, browsed and shared
from Files, set up and watched from Settings, backed up to by Backups, with files that live in the cloud and come down when they
are opened. The engine is [rclone](https://rclone.org) (MIT, the same license as LordOS); everything the person sees is ours.

Nothing here is built yet. Target release: **0.8.8.5**, all of it in one release (decided 2026-11-06).

How to read it:

- **Measured** means checked on our own hardware. **Reported** means rclone's documentation or a public source says so and we
  haven't reproduced it. **To test** means nobody here has run it.
- Providers appear by their plain names, as text, where the person has to pick one (the provider list, "Open website"). No
  provider logos or trade dress; LordOS draws its own glyphs. Other operating systems' cloud features are never named, in the app
  or here ([the brand rule](../tools/brand-check.mjs)).

Mocks of every screen: https://claude.ai/artifact/FosCD4rEoU5Jr8ZM5K8oyq (private to the maintainer).

## 1. Goals

1. **One place for every cloud.** Any number of accounts, of any provider rclone speaks to, side by side: a personal drive, a
   work drive, an S3 bucket, a WebDAV server, a NAS over SFTP.
2. **Native, not a front end.** No rclone window, no web UI, no terminal, no config file to edit. Adding an account is a sheet;
   the files are a section of Files; the status is an item in the menu bar.
3. **Files on demand.** Everything in an account shows in Files at once; a file comes down when it is opened. Each file says
   where it is (only in the cloud, coming down, on this computer, going up). **Keep on This Computer** and **Free Up Space** are
   one click on a file or a folder.
4. **Space at a glance.** Each account's used, free and total space, as a bar, with what takes it where the provider says.
5. **Sharing from Files.** A link to a file or folder, copied in one click, with an expiry where the provider has one, and
   **Stop Sharing**.
6. **Backups to the cloud.** The Backups app can put scheduled, encrypted backups in a folder of an account.
7. **Same security model as the rest of LordOS** (section 4): no open port, the interface can't run commands, each account's
   tokens stay in the keyring.

Non-goals for 0.8.8.5: two-way sync of a local folder such as Desktop or Documents (section 13), serving a folder to the local
network (section 4.4), and per-person sharing invitations (rclone has no API for them; **Open on Website** covers it).

## 2. What we already have

- `rclone` and `fuse3` are dependencies of `lordos` since 0.8.7: Files' Connect to Server mounts sftp, ftp, ftps and WebDAV with
  `rclone mount` (`server/src/lan/rconnect.c`, docs/FILES.md, "Connect to Server"). The configuration is a memfd, the password
  obscured, nothing on a command line or in the environment; rclone runs in scope `app-lordos-lordos-rclone.scope`, firewall app
  `desktop:lordos-rclone`.
- Files already treats `fuse.rclone` mounts as network volumes (`apps/files/network.jsx`, `sdk/filepickermodel.js`,
  `fsops_share_fs`), with an eject button, pins in Locations and reconnect on click.
- rclone is credited in `THIRD_PARTY.md` (MIT). Cloud adds it to Settings → General → About → Open source, with its copyright and
  license text.
- Backups has its engine in `lord-server` (`server/src/backup/`): hard-linked snapshots, retention, encryption, battery rules,
  restore (docs/SYSTEM.md, "Backups").

## 3. rclone: which one, and how it runs

### 3.1 Our own package

Debian 13 has rclone **1.60.1** (Reported: packages.debian.org, trixie 1.60.1+dfsg-4), from 2022. Too old for this design: the VFS
upload queue in rc, bisync over rc and several providers' current APIs are newer. rclone has no apt repository of its own.

**Decided:** a package of our own, `lordos-rclone`, built from the upstream release tag's source with Go (reproducible, `-trimpath`,
both architectures), signed and published in LordOS's apt repository like `lordos-hwdata`. It installs to
`/usr/lib/lordos/rclone/rclone`, so it never fights Debian's `/usr/bin/rclone`. `lordos` depends on it, so it is preinstalled and
comes with updates. Connect to Server moves to the same binary (`RCLONE` in `rconnect.c`). Which upstream version: the newest stable
release when 0.8.8.5 is built, at least the one that has every rc call in section 5 (To test, Phase 0).

The package builds with the upstream release's own flags; nothing is patched. Its `copyright` file carries rclone's MIT license
and the licenses of the Go modules it vendors (generated from `go mod`), and `tools/release/sign-release.sh` treats it like any
other `.deb` of the release.

### 3.2 One daemon per session, no port

`lord-cloudd` is not a new program: it is `rclone rcd` started by `lord-server` for the session user, as one systemd user unit
(`lordos-cloud.service`), in its own scope `app-lordos-lordos-cloud.scope`, firewall app `desktop:lordos-cloud` (reserved like
`desktop:lordos-rclone`, `app_scope_reserved`). It starts when the session has at least one account and stops with the session.

- **Listens on a Unix socket only**: `$XDG_RUNTIME_DIR/lordos/cloud/rc.sock`, folder 0700, socket 0600. No TCP, not even
  loopback: any local program of any user can open a loopback port, and rclone's rc is all or nothing (4.1). Whether `rcd`
  listens on a Unix socket in the version we ship is the first thing Phase 0 checks (rclone's documentation shows the client's
  `--unix-socket`; the server side is To test). If it can't, the fallback is a loopback port chosen at random, with a 32-byte
  random user and password per start passed through a memfd, and the firewall allowing only `lord-server`'s connections to it.
- **Authentication even on the socket**: `--rc-user`/`--rc-pass`, random per start, so `--rc-no-auth` is never used.
- **Configuration**: `--config` points at `~/.config/lordos/cloud/rclone.conf` (0600) with **no secrets in it**: tokens and
  passwords are in the keyring (section 6) and given to rclone per account through `config/create` at start, kept in its memory.
- **Restart**: Settings → Cloud → Service → **Restart** (and automatically after a crash, at most 5 times a minute, as Machines'
  passt). A restart remounts every account; open files show "Reconnecting…" in Files for the few seconds it takes.
- **Resources**: `--use-mmap`, `--buffer-size 16M`, transfers 4, checkers 8 by default; Settings → Cloud → **Bandwidth** limits
  upload and download (`core/bwlimit`). The unit has `MemoryHigh=` and `CPUWeight=` low, `Nice=10`, `IOSchedulingClass=idle`, so
  syncing never slows the desktop ("must not slow the OS").

### 3.3 Mounts

Each account is mounted with `mount/mount` at `~/Cloud/<Account name>` (a folder of the home, so every app and the file pickers
see it), `--vfs-cache-mode full`, the cache in `~/.cache/lordos/cloud/<id>/`. Section 7 covers how files on demand are built on it.

## 4. Security

### 4.1 Why no network address

rclone's documentation says it plainly (Reported, rclone.org/rc): access to the rc API is equivalent to a shell as the user
running rclone; there is no per-call permission; `core/command` runs rclone with any arguments; `config/dump` hands out every
stored credential. An rc port reachable from the network, even with a password, gives anyone who gets it every file and every
token of every account, plus code execution. So there is **no switch** to listen on `0.0.0.0` or any other address: the
maintainer's first idea (an on/off switch, loopback by default) was replaced by the Unix socket and by 4.4.

### 4.2 The interface never talks to rclone

JavaScript has no way to reach the socket. `lord-server` (`server/src/cloud/`) is the only client and exposes `lord.cloud` with a
**closed list** of operations, each with checked arguments:

| `lord.cloud` | rc call | Arguments checked |
|---|---|---|
| `providers()` | `config/providers` | none; filtered to the list in 6.1 |
| `add(kind, name, fields)` | `config/create` | kind in the list; field names in that provider's schema; values without control characters |
| `remove(id)` | `config/delete` + unmount | an existing id |
| `about(id)` | `operations/about` | an existing id |
| `link(id, path, expire)` / `unlink` | `operations/publiclink` | path inside the account, no `..` |
| `stats()` | `core/stats`, `vfs/stats`, `vfs/queue` | none |
| `pause(id)`, `resume(id)`, `bwlimit(up, down)` | `core/bwlimit`, VFS flags | numbers in range |
| `keep(id, path)`, `free(id, path)` | section 7 | path inside the account's mount |
| `restart()` | systemctl restart of the unit | none |

Files themselves are read and written through the mount, with `lord.fs` and its usual rules (`fsops_allowed`), not through rc.
`core/command`, `config/dump`, `config/show`, `operations/*` outside the list, `rc/*` and `options/set` are never called; a test
proves the C client can't send them (the method name comes from a fixed table, never from JS).

### 4.3 Tokens and passwords

- OAuth tokens, passwords and keys go to the keyring (`lord.keyring`, the same store as Mail and Remote Desk), one item per
  account. `rclone.conf` holds only the account's type and non-secret options.
- At each start `lord-server` reads them from the keyring and gives them to rclone with `config/create`/`config/update` over the
  socket. When rclone refreshes an OAuth token, `lord-server` reads the new one (`config/get` for that one remote, over the socket)
  and writes it back to the keyring, then removes it from rclone's file if rclone wrote it there (To test: whether rcd writes
  refreshed tokens to `--config`; if it does, the file stays 0600 and is rewritten without secrets after each refresh).
- A locked keyring (no auto-unlock) means Cloud waits and Files shows the accounts dim with "Unlock the keyring to connect".

### 4.4 Using a cloud from another computer

The safe form of "reach it from the network" is a separate, later feature: **Share Folder on Network**, `rclone serve webdav` on
one chosen folder, its own password, TLS with a certificate shown as a fingerprint, optionally read-only, off by default, its own
firewall row. Not in 0.8.8.5.

### 4.5 Sandbox

rclone needs the network and the user's files under `~/Cloud` and its cache, nothing else. The unit runs with
`ProtectSystem=strict`, `ProtectHome=tmpfs` plus `BindPaths=` for `~/Cloud`, `~/.cache/lordos/cloud`, `~/.config/lordos/cloud` and
the runtime folder, `PrivateTmp`, `NoNewPrivileges`, `RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6 AF_NETLINK`, and
`SystemCallFilter=@system-service`. FUSE needs `/dev/fuse` and `fusermount3` (setuid); To test which of these a user unit can
keep while mounting.

## 5. rc calls used

`config/providers`, `config/create`, `config/update`, `config/delete`, `config/get`, `mount/mount`, `mount/unmount`,
`mount/listmounts`, `operations/about`, `operations/publiclink`, `core/stats`, `core/bwlimit`, `vfs/stats`, `vfs/queue`,
`vfs/refresh`, `vfs/forget`, `sync/copy` and `sync/sync` with `_async`, `job/status`, `job/stop`, `core/quit`. Phase 0 checks each
exists in the shipped version and records its answers.

## 6. Accounts

### 6.1 Providers offered

The add sheet shows a short list first, then **More…** with the rest rclone supports, searchable:

- **With sign-in in the browser** (OAuth): the large consumer and work drives (personal and business), a photo-and-file drive,
  Box, Dropbox, pCloud, Yandex Disk, Zoho WorkDrive, and others rclone marks as OAuth.
- **With a user and password or key**: WebDAV (with presets for Nextcloud and ownCloud), SFTP, FTP, SMB, Mega, Proton Drive,
  Koofr, Storj, and Seafile.
- **Object storage**: S3 and every S3-compatible service rclone lists (access key, secret, region, endpoint), Backblaze B2, Azure
  Blob, Google Cloud Storage, Swift.
- **Encrypted folder** (rclone `crypt`) on top of any account: section 10.

Providers rclone supports that need a command line or a local browser on another machine, or that rclone marks experimental, are
not offered.

### 6.2 Signing in with the browser

1. The person picks a provider and names the account ("Work", "Personal"; the name becomes the folder in `~/Cloud`).
2. `lord-server` starts `rclone authorize <type> --auth-no-open-browser` in the cloud scope, reads the authorization URL from its
   log, and opens it in the default browser itself; the token comes back on stdout and goes to `config/create` as `config_token`
   (section 15: the rc state machine asks `config_is_local` first, so this path is simpler). Settings shows "Waiting for you to sign in
   in the browser…" with the time left (5 minutes), **Open Again** and **Cancel**. The redirect comes back to rclone's listener on
   `127.0.0.1:53682`, which only lives during the sign-in.
3. The token goes to the keyring (4.3), the account is mounted, and Files shows it.

Client IDs (decided 2026-11-06): **0.8.8.5 uses rclone's own built-in OAuth client IDs.** `lordos-rclone` is upstream rclone,
unpatched, so the person is signing in to rclone, exactly as with Debian's or any distribution's rclone package. Nothing hides it:
the provider's consent page names rclone, and the sign-in sheet says so ("Signing in uses rclone, the open source engine LordOS
uses for Cloud. The provider's page will ask you to allow rclone."). rclone's IDs are shared by every rclone user, and some
providers rate-limit them (Reported: rclone's docs recommend your own ID for the largest drive). So each account has, under
Advanced, **Use my own client ID** (client ID and secret, to the keyring), and later LordOS can register its own apps (owner
account iPERFEX, identity lordos.org: name LordOS, home page and privacy policy on lordos.org) and ship them in
`assets/cloud/oauth.json`, read by `lord-server`, without changing anything else.

### 6.3 Account settings

Settings → Cloud → an account: name (renames the folder), **Open Website** (the provider's web page for the account, from a fixed
table per provider; S3 and WebDAV open the endpoint's address only if it is https), space (section 8), **Keep on This Computer**
folders, **Pause** syncing, **Sign In Again** (expired token), **Remove Account** (unmounts, deletes the cache after uploads
finish or after the person confirms discarding them, removes the keyring item; never deletes anything in the cloud).

## 7. Files on demand

### 7.1 What the person sees

In Files every file of a cloud account has a status, as a small glyph in the icon views and a column in the list view:

| Status | Glyph | Means |
|---|---|---|
| In the cloud | cloud outline | listed, nothing on this computer |
| Downloading | cloud with arrow down and a progress ring | coming down because it was opened or kept |
| On this computer | check in a circle outline | in the cache, can be freed |
| Kept on this computer | check in a filled circle | pinned: never freed automatically, synced |
| Uploading | arrow up with progress | changed here, going up |
| Error | exclamation | the last upload or download failed; Get Info says why |

Context menu on files and folders of an account: **Keep on This Computer** / **Free Up Space**, **Copy Link** / **Share Link…**
/ **Stop Sharing**, **Open on Website**, **Get Info** (where it is, size in the cloud and here, last synced, link).

### 7.2 How it is built

- rclone's VFS with `--vfs-cache-mode full` already fetches on open and keeps what was read; uploads changed files after
  `--vfs-write-back 5s`.
- **LordOS decides what stays**, not rclone: the mount runs with `--vfs-cache-max-age 100y` and no `--vfs-cache-max-size`, so
  rclone never evicts on its own. `lord-server`'s cloud manager evicts instead:
  - **Free Up Space** on a file or folder: for each clean file (not open, not in `vfs/queue`, not kept), the cached copy is
    dropped. How: rclone has no evict call (Reported); Phase 0 tests dropping the cache file and its metadata in
    `vfsMeta` while the mount runs and reading it again, against `vfs/forget`. If it isn't safe, eviction happens by restarting
    that account's mount with a size limit for one pass.
  - **Automatic**: when the disk holding the home has less than the threshold free (Settings → Cloud → "Free up space when this
    computer has less than 10 GB free", 5 to 100 GB, on by default), the least recently used clean, not kept files go first.
  - **Keep on This Computer**: the path goes in `~/.config/lordos/cloud/keep.json`; the manager reads the files once to bring them
    down (low priority, paused on battery like Backups), watches the folder for new ones (`vfs/refresh` + polling the remote
    every 5 minutes, or the provider's change notifications where rclone has them), and never evicts them.
- **Status per file**: `lord-server` reads the VFS cache metadata (which ranges of a file are present) and `vfs/queue` (what waits
  to upload), and Files gets the status with `lord.cloud.status(paths)` for the visible rows only, pushed again on change.
- **Offline**: with no network, cached and kept files open; the rest say "Not on this computer, and the cloud can't be reached".
  Changes wait in the queue and go up when the network comes back; the menu bar item says how many wait.
- **Conflicts**: a file changed here and in the cloud while offline: the cloud's version keeps the name, ours is saved next to it
  as "name (this computer).ext" and a notification says so, the same rule as Calendar's conflicts.

## 8. Space

Settings → Cloud lists the accounts, each with a bar: used, free, total (`operations/about`: `total`, `used`, `free`, `trashed`,
`other` where the provider gives them), the trash's share in its own color with **Open Website** to empty it, and below all of them
**On this computer**: what the cache takes per account and **Free Up Space** for everything not kept. Providers with no quota
(most S3 and WebDAV) show only what is used, measured from the listing in the background once a day (`operations/size` is not in
the list in 4.2: it would read the whole account; To decide whether to offer it on demand).

## 9. Sharing

**Share Link…** opens a sheet: the link (`operations/publiclink`), **Copy**, an expiry where the provider supports one (`expire`:
1 day, 7 days, 30 days, never), and **Stop Sharing** (`unlink`) where the provider supports it. Providers without public links
show **Open on Website** instead. The links made from LordOS are listed in Settings → Cloud → the account → **Shared Links**, from
`~/.config/lordos/cloud/links.json` (rclone can't list links), each with Copy and Stop Sharing.

## 10. Backups to Cloud

The Backups app gets a third kind of destination, **Cloud**: an account and a folder in it.

- Clouds have no hard links, so the snapshot layout of docs/SYSTEM.md can't be used. The cloud layout:

```
<account>:<folder>/LordOS-Backups/<host>-<user>/
├── current/                  the home folder as of the last backup
├── versions/2026-11-06-020000/   what that run replaced or deleted, as it was before (rclone --backup-dir)
├── manifests/2026-11-06-020000.json.zst   every path of that snapshot: size, mtime, mode, hash
└── encryption.json           salt and key check, as for disks
```

- A run is one `sync/sync` job from the home (same exclusions as today) to `current/` with `backupDir` set to that run's
  `versions/<stamp>/`, at low priority, paused on battery, after a fresh manifest of the home is written. The Backups tab lists
  snapshots from `manifests/`; restoring a file at a date takes it from the oldest `versions/` folder newer than that date that has
  it, else from `current/` (the manifest says which, by hash).
- **Encryption**: the destination is wrapped in an rclone `crypt` remote whose password is derived from the Backups passphrase
  (the same `encryption.json` scheme as disks, Argon2id), so names and contents are encrypted before they leave the computer.
- **Retention**: the same `bk_retention` rules decide which manifests stay; a `versions/` folder goes when no kept manifest needs
  it.
- Errors, battery, notifications and the menu bar item work as for disks.

## 11. The menu bar item

A cloud glyph by the clock while at least one account exists: still when idle, with a moving arrow while transferring, with a
badge for errors or waiting changes. Its panel: each account's state (up to date, syncing with the file and percentage, paused,
needs sign-in), the last changes (file, account, up or down, time) with **Show in Files**, **Pause All** / **Resume**, and
**Cloud Settings…**. Following the Lord Classic rule (#29), the panel opens toward the middle of the screen.

## 12. Settings → Cloud

In Settings' second group, next to Sharing, glyph `☁`, color `#0a84ff`:

1. **Accounts**: one row per account with its glyph, name, provider, a space bar and its state; **+** adds one (the provider sheet,
   6.1). A row opens the account (6.3).
2. **On this computer**: cache use per account, the free-space threshold (7.2), **Free Up Space**.
3. **Bandwidth**: upload and download limits, "Pause on battery", "Pause on metered networks".
4. **Service**: state (running, its version, uptime, memory), **Restart**, **Show Log** (opens the Logs app on the unit),
   and a line "Only LordOS can reach it: it listens on no network address."
5. **About rclone**: version, MIT license, link to rclone.org.

## 13. Later

Two-way sync of chosen local folders (Desktop, Documents) with `bisync`; **Share Folder on Network** (4.4); photo upload from the
Camera and Photos; a cloud's shared-with-me folder where the provider exposes it; per-person sharing through each provider's API.

## 14. Phases (all in 0.8.8.5)

| Phase | Ships | Done when |
|---|---|---|
| **0, Proof** | `lordos-rclone` built on both architectures; `rcd` on a Unix socket; every rc call in section 5 answered; eviction while mounted (7.2); OAuth through `config/create` states; token refresh and the keyring | Each To test above is Measured, in section 15 |
| **1, Core** | `server/src/cloud/` with `lord.cloud`, the unit, Settings → Cloud, accounts with sign-in, space, mounts, Files' Cloud section with status glyphs, Keep / Free Up Space, links, menu bar item | An account of each kind (OAuth drive, WebDAV, S3, SFTP) added, browsed, files opened offline, links made, on amd64 and the Pi |
| **2, Backups** | Cloud destination, manifests, versions, crypt, restore | Backup, restore of a file and of everything, a kill in the middle recovered, encrypted names seen on the provider |
| **3, Release** | Docs, screenshots, translations, tests (section 16), THIRD_PARTY and About | `make test` and the release gates pass on both architectures |

## 15. Phase 0 results

Run on 2026-11-06 with rclone **v1.75.2** built from its tag's source with Go 1.26.10 (`CGO_ENABLED=0 go build -trimpath`), on
VM 116 (Debian 13.7, amd64) and the Raspberry Pi 5 (arm64, LordOS 0.8.8.4). v1.76.0 came out the same day; 0.8.8.5 takes the newest
patch release when it is built. Probe: `tools/cloud/phase0.sh` (a local `rclone serve webdav` stands in for the cloud). Same
results on both architectures unless a number says otherwise.

| Question | Result |
|---|---|
| `rcd` on a Unix socket | **Measured, works**: `--rc-addr unix:///…/rc.sock`. No TCP listener at all (`ss -ltnp`). The socket is created with the umask (0775 here), so the 0700 folder is what protects it; `lord-server` also sets umask 077 for the unit |
| Auth on the socket | **Measured**: without `--rc-user`/`--rc-pass` a call that touches a remote gets 401 |
| Every rc call in section 5 | **Measured, all present**, plus `vfs/list` and `sync/bisync`. `core/command` and `config/dump` are present too, as expected: 4.2's closed table is what keeps them out |
| `config/create` for WebDAV with `obscure` | **Measured**: the password lands obscured in `rclone.conf`; 4.3 still moves it to the keyring |
| `mount/mount` over rc with `CacheMode full`, no age or size limit | **Measured, works**; first read of a 5 MB file 63 ms (amd64), 36 ms (Pi) over loopback |
| Eviction while mounted (7.2) | **Measured, works**: removing a clean file's cache data and its `vfsMeta` entry while the mount runs, then reading it again, fetches it again with the right content (sha256 equal), also after `vfs/forget`. Phase 1 adds the cases this didn't cover: a file open while evicted (must be refused by the manager: `vfs/stats` `inUse`), and eviction racing a write |
| Upload queue | **Measured**: a write through the mount shows in `vfs/queue` (name, size, delay, tries) and reaches the server after `WriteBack` (5 s) |
| `operations/about` | WebDAV without quota answers `{}`: section 8's "used only" case |
| `operations/publiclink` | WebDAV: "doesn't support public links", the error 9 turns into **Open on Website** |
| OAuth through rc `config/create` (non-interactive) | Answers a state machine whose first question is `config_is_local`. **Decided instead:** `lord-server` runs `rclone authorize <type> --auth-no-open-browser` in the cloud scope, reads the URL from its log line ("Please go to the following link: http://127.0.0.1:53682/auth?state=…"), opens it in the default browser itself, and gets the token from stdout; then `config/create` with `config_token`. **Measured** up to the URL with Dropbox; the token step needs a real account (Phase 1) |
| `core/bwlimit` | **Measured**: up and down separately (`1M:2M`) |
| Memory | `rcd` with one mount: RSS 77 MB (amd64), 81 MB (Pi) |
| Binary | 86 MB (amd64), 79 MB (arm64) with every backend. To decide in packaging: build tags to leave out backends 6.1 doesn't offer |
| Token refresh written back to `--config` | To test with a real OAuth account (Phase 1) |

## 16. Tests

- **Unit** (`cloud_test`): the closed rc method table (no other name can be sent), argument checks, the status merge from cache
  metadata and the queue, eviction choice (LRU, never kept, never dirty), manifest and restore choice for Backups, provider
  presets, the link list.
- **Integration** (VM 116 and the Pi): a local WebDAV server (rclone `serve webdav`) and MinIO as S3 stand-ins: add, mount, open,
  edit offline, reconnect, conflict, free up space while a file is open, kill rclone mid-upload and restart, Backups run, kill and
  restore.
- **Security**: nothing listens on any TCP port while Cloud runs (`ss -ltnp`), the socket is 0600, `rclone.conf` holds no secret,
  a second local user can't connect, a JS caller can't reach `core/command` or `config/dump`.
- **Load**: 100 000 files in one account listed in Files without stalling the desktop; a 4 GB upload with the desktop at 60 FPS.

## 17. The `lord.cloud` contract (Phase 1)

The interface and `lord-server` meet here; Settings, Files, the menu bar item and Backups use only this. Calls are synchronous
and return at once (long work runs in `lord-server`'s cloud thread); every change is pushed as one state object through
`__lordCloud(state)` (an eventfd in the main loop, at most twice a second, like `__lordBackup`), wrapped by `sdk/cloud.js` as
`useCloud()` and `cloud.*`.

```
cloud.available()            → bool (lordos-rclone installed)
cloud.state()                → State (the last one pushed)
cloud.providers()            → [{ kind, label, group: 'oauth'|'server'|'object'|'crypt', hint,
                                  fields: [{ name, label, secret, required, default, examples: [..] }] }]
cloud.beginSignIn(kind, name, { clientId?, clientSecret? })   → { id, url, expiresAt } | { error }
cloud.openSignInPage(id)     → opens url in the default browser again
cloud.cancelSignIn(id)
cloud.add(kind, name, fields)                                 → { id } | { error, field? }   (server, object, crypt kinds)
cloud.remove(id, { discardPending })                          → { ok } | { error: 'pending', files }
cloud.rename(id, name)       → { ok } | { error }
cloud.refreshAbout(id)
cloud.websiteUrl(id)         → string | null
cloud.status(paths)          → { [path]: { st: 'cloud'|'down'|'here'|'kept'|'up'|'error', pct?, error? } }  (paths under ~/Cloud)
cloud.keep(paths) / cloud.unkeep(paths)
cloud.free(paths)            → { freed, skipped: [{ path, reason: 'open'|'pending'|'kept' }] }
cloud.link(path, expire)     → { url } | { error: 'unsupported'|… }   (expire: '1d'|'7d'|'30d'|'never')
cloud.unlink(path)           → { ok } | { error }
cloud.links(id)              → [{ path, url, expires, made }]
cloud.pause(id|null) / cloud.resume(id|null)
cloud.setOptions({ bwUp, bwDown, pauseOnBattery, pauseOnMetered, autoFree, freeBelowGb })
cloud.restart()
cloud.showLog()              → opens Logs on the service

State = {
  service: { running, version, uptimeS, rssKb, restarts, error? },
  options: { bwUp, bwDown, pauseOnBattery, pauseOnMetered, autoFree, freeBelowGb },
  accounts: [{ id, name, kind, provider, mount, state: 'ok'|'syncing'|'paused'|'signin'|'offline'|'error'|'locked', message?,
               about: { total?, used, free?, trashed?, other? } | null, cacheBytes, keptBytes,
               up: { files, bytes, done, name? }, down: { files, bytes, done, name? }, kept: [path], linksSupported }],
  signin: { id, kind, name, phase: 'waiting'|'done'|'failed', url, expiresAt, error? } | null,
  recent: [{ path, account, dir: 'up'|'down', at }]   (newest first, at most 20)
}
```

Errors are codes (`errors.json` keys `cloud.*`), never rclone's text, except `message` in an account, which is shown under Get Info.
