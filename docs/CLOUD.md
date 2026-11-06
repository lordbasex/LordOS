# Cloud

The design of **Cloud**, LordOS's layer over the person's cloud storage accounts: every account in one place, browsed and shared
from Files, set up and watched from Settings, backed up to by Backups, with files that live in the cloud and come down when they
are opened. The engine is [rclone](https://rclone.org) (MIT, the same license as LordOS); everything the person sees is ours.

Target release: **0.8.8.5**, all of it in one release (decided 2026-11-06). Phase 0 is measured (section 15); Phase 1
(the core, section 17.1, and the interface, section 18) is built and merged on branch `cloud`.

How to read it:

- **Measured** means checked on our own hardware. **Reported** means rclone's documentation or a public source says so and we
  haven't reproduced it. **To test** means nobody here has run it.
- Providers appear by the names rclone gives them, as plain text, only where the person has to pick one or sees which one an
  account is (Add Account, an account's provider line, "Open website"): the one exception to [the brand rule](../tools/brand-check.mjs)
  (17.2 item 2). No provider logos or trade dress; LordOS draws its own glyphs. Other operating systems' cloud features are
  never named anywhere else.

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

`lord-cloudd` is not a new program: it is `rclone rcd` started by `lord-server` for the session user, in its own scope
`app-lordos-lordos-cloud.scope`, firewall app `desktop:lordos-cloud` (reserved like `desktop:lordos-rclone`, `app_scope_reserved`).
It starts when the session has at least one account (or the provider list or a sign-in needs it), stops 2 minutes after the
last account goes, and ends with the session.

**As built (Phase 1):** not a systemd user unit but a child of `lord-server`'s cloud thread, the way Connect to Server starts
`rclone mount` (`server/src/cloud/cloud.c`, `daemon_start`): fixed argv and a fixed environment (HOME, PATH, LANG,
XDG_RUNTIME_DIR, USER), umask 077, `nice 10`, idle I/O, `PR_SET_PDEATHSIG` (rclone unmounts and ends if `lord-server` dies),
scope through lord-sessiond with `memory.high` 512 MB. Its log is `~/.cache/lordos/cloud.log` (NOTICE level; over 4 MB it
becomes `cloud.old` at the next start), which the Logs app shows. A unit's `ProtectSystem=`/`BindPaths=` sandbox (4.5) is not
there: the scope and the socket are the boundary.

- **Listens on a Unix socket only**: `$XDG_RUNTIME_DIR/lordos/cloud/rc.sock`, folder 0700, socket 0600. No TCP, not even
  loopback: any local program of any user can open a loopback port, and rclone's rc is all or nothing (4.1). Whether `rcd`
  listens on a Unix socket in the version we ship is the first thing Phase 0 checks (rclone's documentation shows the client's
  `--unix-socket`; the server side is To test). If it can't, the fallback is a loopback port chosen at random, with a 32-byte
  random user and password per start passed through a memfd, and the firewall allowing only `lord-server`'s connections to it.
- **Authentication even on the socket**: a random user and password per start, so `--rc-no-auth` is never used. As built:
  not `--rc-user`/`--rc-pass` (the command line is readable by every user) but `--rc-htpasswd /proc/self/fd/3`, a memfd with
  one bcrypt line (cost 4: the password is 32 random bytes); `lord-server` keeps the password in memory for Basic auth.
- **Configuration**: rclone's is in memory only (`--config ""`). `~/.config/lordos/cloud/rclone.conf` (0600) is LordOS's own
  record of each account's type and non-secret options, with **no secrets in it**: tokens and passwords are in the keyring
  (section 6) and given to rclone per account through `config/create` at each start, kept in its memory.
- **Restart**: Settings → Cloud → Service → **Restart** (and automatically after a crash, at most 5 times a minute, as Machines'
  passt). A restart remounts every account; open files show "Reconnecting…" in Files for the few seconds it takes.
- **Resources**: `--use-mmap`, `--buffer-size 16M`, transfers 4, checkers 8 by default; Settings → Cloud → **Bandwidth** limits
  upload and download (`core/bwlimit`). The unit has `MemoryHigh=` and `CPUWeight=` low, `Nice=10`, `IOSchedulingClass=idle`, so
  syncing never slows the desktop ("must not slow the OS").

### 3.3 Mounts

Each account is mounted with `mount/mount` at `~/Cloud/<Account name>` (a folder of the home, so every app and the file pickers
see it), `--vfs-cache-mode full`, the cache in `~/.cache/lordos/cloud/` (rclone's layout: `vfs/<id>/` for the data,
`vfsMeta/<id>/` for what is present). Section 7 covers how files on demand are built on it. The account's id is `c` and 8 hex
digits; its folder is its name, which can't be empty, start with a dot or hold a slash or a control character.

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
  and writes it back to the keyring. As built: rclone's configuration is in memory (`--config ""`), so there is no file for a
  refreshed token to land in; `lord-server` compares each OAuth account's secrets every 5 minutes and saves them when they
  changed. The keyring item is owner `cloud`, service `cloud:<id>`, its secret a JSON object of rclone's secret options as
  `config/get` gives them (passwords obscured by rclone), asked of `lord-vault` on `lord-server`'s own channel
  (`vc_request_wait`, `server/src/cloud/cloud_kr.c`), so JavaScript never sees it. Which options are secret comes from rclone's
  own schema (`IsPassword` or `Sensitive`), plus `token` and `client_secret`. If the keyring can't take them (locked) they wait
  in memory and are retried every 30 s; with no keyring at all the account says `cloud.noKeyring`.
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

As built (17.2 item 1): every backend the shipped rclone has, asked of rclone each time the sheet opens, under rclone's own
names, minus the ones that aren't a storage of their own; the groups below are derived from each backend's options. The
first design, kept for the groups:

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

Redirect address (decided 2026-11-06): the sign-in returns to rclone on `127.0.0.1:53682` on the person's own computer, the
loopback redirect that RFC 8252 recommends for native apps. A relay on our domain (auth.lordos.org or a page of lordos.org) is
not used: rclone's own apps only accept their registered loopback address; the code would pass through a server of ours (logs,
outages, a phishing target) for no gain; and auth.lordos.org is the blog's Cognito domain, which signs people in to the blog but
doesn't hand out a provider's file tokens. **Ruled out for good** by the maintainer for those risks: a provider that refuses
loopback addresses is simply not offered.

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
| **1, Core** | `server/src/cloud/` with `lord.cloud`, the unit, Settings → Cloud, accounts with sign-in, space, mounts, Files' Cloud section with status glyphs, Keep / Free Up Space, links, menu bar item | An account of each kind (OAuth drive, WebDAV, S3, SFTP) added, browsed, files opened offline, links made, on amd64 and the Pi. **Core built** (branch `cloud-core`): WebDAV end to end on both architectures (section 15); a real OAuth account, S3 and SFTP still to run; the interface is branch `cloud-ui` |
| **2, Backups** | Cloud destination, manifests, versions, crypt, restore | Backup, restore of a file and of everything, a kill in the middle recovered, encrypted names seen on the provider |
| **3, Release** | Docs, screenshots, translations, tests (section 16), THIRD_PARTY and About | `make test` and the release gates pass on both architectures |

## 15. Phase 0 results

Run on 2026-11-06 with rclone **v1.75.2** built from its tag's source with Go 1.26.10 (`CGO_ENABLED=0 go build -trimpath`; since
17.2 item 7 the package carries rclone's official release binary instead, the same version), on
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
| Token refresh written back to `--config` | Moot as built: `--config ""` keeps it in memory, and `lord-server` saves a changed token to the keyring (4.3). To test with a real OAuth account |

### Phase 1: the core (2026-11-06)

Built on branch `cloud-core`: `server/src/cloud/` (`cloud_pure.c` the closed method table and every check, `cloud_rc.c` the
client, `cloud.c` the thread, daemon and accounts, `cloud_files.c` status, eviction and Keep, `cloud_kr.c` the keyring),
`server/src/ui/js_cloud.c` (`lord.cloud`), `sdk/cloud.js`, `tools/iso/mkdeb-rclone.sh` (`lordos-rclone_1.75.2-1_amd64.deb`
and `_arm64.deb`, 21 MB and 18 MB, built on VM 116 in 4 min 17 s for both). Measured on VM 116 (amd64) and the Pi 5 (arm64):

| What | Result |
|---|---|
| `cloud_test` (unit, ASan and UBSan) | 1526 checks pass on both: the 21-method table and that 30 dangerous names (`core/command`, `config/dump`, `options/set`, `operations/delete`, `sync/sync`…) are not in it, header injection refused, HTTP answers (length, chunked, cut short), names and paths under `~/Cloud` (`..`, `//`, C1 controls, bad UTF-8, a sibling `~/Cloudy`), the provider and website tables (no other operating system's names; `https` only for an account's own address), `authorize`'s address and token, the status merge, eviction (LRU, never kept, changed, waiting or open), and on a real cache tree in a throwaway home: Free Up Space, a file open by a process refused, links and keep files read back without bad entries, files 0600 in a 0700 folder |
| `tests/cloud_integration.sh` against `rclone serve webdav` | 70 checks pass on amd64 and on the Pi: no TCP listener of `rcd`, the rc folder 0700 and socket for the user only, no password or `RCLONE_RC_*` on the command line or in the environment, 401 for `config/dump`, `core/command` and `operations/list` without credentials, another local user can't connect, `rclone.conf` and every file in `~/.config/lordos/cloud` without the password; add, mount, a 5 MB file read whole, Free Up Space while closed and refused while open, a write going up and refused as pending, Pause holding an upload 11 s and Resume sending it, Keep bringing a freed file down, WebDAV links answered `unsupported`, Restart and a `kill -9` restarted by itself (files read after both), add with the same name updating the account, rename, remove (unmounted, cache and keyring item gone, nothing deleted on the server), `rclone` gone with the session |
| Eviction while a file is open | Refused (`reason: open`): rclone's `vfs/stats` `inUse` > 0 makes `lord-server` look for the file among this user's open descriptors (`/proc/*/fd`), and also treats a file used in the last 10 s as open, because rclone keeps a closed file's handle 5 s (`HandleCaching`) |
| Pause per account | rclone has no per-remote pause: what waits in `vfs/queue` gets its expiry pushed to 10⁹ s (`vfs/queue-set-expiry`) every second while paused, and back to 0 on Resume. Downloads of files being opened are never paused; Keep's downloads are |
| A sign-in's address | `rclone authorize` prints it on stderr within milliseconds; `beginSignIn` waits for it up to 2 s, so `url` is there at once in practice |
| An own client ID | `RCLONE_<KIND>_CLIENT_ID`/`_CLIENT_SECRET` in `authorize`'s environment: **measured**, the provider's address carries that `client_id` |
| rclone's shared client ID for the largest drive | `config/create` asks `config_shared_client_id`: "rclone's shared … client_id is being retired and will stop working during 2026". Phase 1 answers yes; own client IDs (6.2) become necessary for that provider |

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

Built so far (section 15, "Phase 1: the core"): `server/tests/cloud_test.c` (in `make test`, `mk/cloud.mk`) and
`server/tests/cloud_integration.sh <build dir> <rclone binary>`, which drives `build/cloud-harness` (the same `src/cloud`
code with the keyring replaced by files and the browser by a line on stdout; `LORDOS_CLOUD_RCLONE` names the binary in that
test build only, `-DCLOUD_TEST`) against a local `rclone serve webdav`. MinIO, conflicts, Backups and the load tests are not
written yet.

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

### 17.1 As built (branch `cloud-core`)

`sdk/cloud.js` exports `cloud`, `useCloud()` and `cloudState`; `__lordCloud(state)` replaces the store's state. What the
contract above leaves open, and the few places the core differs from it:

- **Times** are milliseconds since the epoch (`expiresAt`, `recent[].at`, `links()[].made` and `expires`, `null` for never);
  sizes are bytes (`about`, `cacheBytes`, `keptBytes`, `up`/`down`.`bytes` and `done`, `free()`'s `freed`); `bwUp`/`bwDown`
  are KiB/s, 0 for no limit, at most 10 GiB/s; `freeBelowGb` 5 to 100.
- `providers()` is `[]` until rclone has listed its backends once in the session (it starts rclone if needed); a state push
  follows, so call it again from `useCloud()`. Fields: rclone's non-advanced options of that backend, minus the ones a preset
  sets (Nextcloud's `vendor`…); `label` is the first line of rclone's help, in English; OAuth kinds have `fields: []`.
- `beginSignIn` waits up to 2 s for rclone's address, so `url` is set at once in practice; if not, it is `null` and comes in
  `state.signin.url`. `lord-server` opens it in the default browser itself (the launcher's `x-scheme-handler/http` app);
  `openSignInPage(id)` opens it again and returns whether it could (`sdk/cloud.js` falls back to the session's opener).
  `signin.account` names the account once the token came. `cancelSignIn` of a finished sign-in clears `signin` to `null`.
- **Sign In Again / new credentials** (asked by the interface, 2026-11-06): `beginSignIn(kind, name)` or `add(kind, name,
  fields)` with the name of an existing account **of the same kind** (case doesn't matter) updates that account: same id, same
  folder, the new token or fields merged into its remote with `config/update`, unmounted and mounted again, the keyring item
  replaced. `add` answers `{ id }` with the existing id; required fields aren't required then, and an empty value keeps the
  old one. The name of an account of another kind is `cloud.nameTaken`.
- `link` and `unlink` reach the provider, so `lord.cloud.link/unlink` return `{ req }` and the answer comes through
  `__lordCloudReply(req, result)`; **`cloud.link()` and `cloud.unlink()` in the SDK return a Promise** of `{ url }`,
  `{ ok }` or `{ error }`. A provider without links answers `{ error: 'unsupported' }` (also at once when the account's
  `linksSupported` is already known to be false).
- `remove` with changes waiting answers `{ error: 'pending', files }`; `{ discardPending: true }` removes anyway.
- `status`: a folder is `up` or `down` when something inside moves, `kept` when kept, else `cloud`; `error` carries
  `error: 'cloud.transferFailed'` (the file's last transfer failed). Paths not under `~/Cloud/<account>` are left out.
- `free` refuses with `reason` in this order: `kept`, `pending` (changed here or in `vfs/queue`), `open`; `skipped` lists at
  most 200.
- **State**, besides §17: `available`; `service.error` (`cloud.unavailable`, `cloud.startFailed`, `cloud.crashed`,
  `cloud.noRuntimeDir`); `options.pauseAll` and `options.onBattery`; in each account `group`, `mounted`, `paused`,
  `ownClient` and `error` (a `cloud.*` code: `cloud.keyringLocked`, `cloud.noKeyring`, `cloud.signInAgain`, `cloud.offline`,
  `cloud.authFailed`, `cloud.mountFailed`, `cloud.folderNotEmpty`, `cloud.addFailed`…; `message` is rclone's own text).
  `about` has `used` always and the others only when the provider gives them.
- `pause(null)` / `resume(null)`: Pause All (`options.pauseAll`); `resume(null)` also resumes each account.
- `pauseOnMetered` is kept but does nothing yet: LordOS's network service doesn't say whether a network is metered.
- `showLog()` opens Logs on `~/.cache/lordos/cloud.log` (`wm.open('logs', { args: { file: 'cloud.log' } })`; Logs reads
  `args.file` since this branch).
- Not in the core: per-account options (`setAccountOptions`, `account.options`: rclone's bandwidth limit is one for the
  whole daemon, and there is no metered information to act on) and `State.conflicts`: rclone's VFS uploads the last version
  written and doesn't keep "name (this computer).ext" copies, so 7.2's conflict rule needs work of its own. The interface
  hides those rows when they're absent.

## 18. The interface (Phase 1)

Built from the approved mocks (Main, AddAccount, SignIn, Credentials, Account, Files, FilesMenu, ShareLink, GetInfo,
MenuBar, States; Backups is Phase 2), against section 17 only.

| Where | What |
|---|---|
| `sdk/cloud.js` | the thin wrapper of section 17: `cloud.*`, `useCloud()`, `cloudState`; `__lordCloud(state)` only stores the state |
| `sdk/cloudmodel.js` | everything that decides without a screen: status → glyph, space bars and rings, provider groups and search, names, menus, the sidebar, the menu bar glyph, notifications (`transitions`) |
| `sdk/cloudglyphs.js`, `sdk/cloudui.jsx` | the mocks' glyphs and rings drawn as pixels (the engine has no paths or conic gradients), cached so a row that doesn't change isn't uploaded again |
| `sdk/cloudstatus.jsx` | the menu bar item and its panel, the notifications from state changes, `startCloud()` in `startShell` |
| `apps/settings/Cloud.jsx` | Settings → Cloud, the account page and the sheets |
| `apps/files/cloud.jsx` | Files' Cloud section, status, menu items, Share Link, Get Info, the offline message (docs/FILES.md, "Cloud") |
| `sdk/cloudmock.js` | a development stand-in for `lord.cloud`, used only when `lord.cloud` is missing and settings.json has `"cloudDevMock": true` (or `"states"`, which a few seconds in turns on a sign-in request, a full account with changes waiting, a conflict and an offline account) |
| `server/tests/cloud_model_test.mjs` | 169 checks (with section 19's), in `run_suites.sh`: the model, the glyph pixels, the file dialog's places, the mock against section 17's shape, every text in the five catalogs, and that Cloud's screens use no one-sided physical edge (right to left) |

Decisions taken while building it:

- **Settings → Cloud** keeps the mock's three boxes (all accounts, the accounts, on this computer with the threshold and
  the service) and adds below them section 12's **Bandwidth** (upload and download limits, pause on battery, pause on
  metered networks) and **About rclone** (version, MIT license, rclone.org). **Show Log** sits next to Restart.
  **Add Account…** is in Settings' title bar, beside "Cloud", as in the mock.
- Sizes use LordOS's own `formatBytes` (1024-based, like the rest of the system), so the mocks' "612 GB" reads 570 GB.
- **Providers**: groups as in the mock (the encrypted folder with object storage, last). Each group shows the first
  five (browser), three (server) or two (object storage) in the order `providers()` gives, and a **More…** tile with
  the rest; a group with only one more shows it instead of a "1 more" tile. Search looks at the label, the hint and
  the kind, accents folded. The name proposed is Personal, then Work, then the provider's label (not changed once typed).
- **Credentials**: a field's label is ours for the usual rclone option names (`url`, `user`, `pass`, `endpoint`,
  `access_key_id`…, translated), else the provider list's own; a field with two to four examples (a preset, a service)
  is a segmented control, the rest text fields; secrets are masked. lord-server's error for a field outlines it.
- **Sign In Again**: an OAuth account calls `beginSignIn(kind, <its name>, { clientId?, clientSecret? })` again; any
  other opens its credentials form and calls `add(kind, <its name>, fields)`. **lord-server must treat a sign-in or an
  add for an existing account's name as updating that account**, not as a second one.
- **Files on demand** off is `keep([mount])`, on is `unkeep([mount])`; the ready sheet's "Keep everything on this
  computer" is the same, and its "Show in Files" switch is `settings.cloudHidden` (Files and the file dialog leave
  those accounts out).
- **Nothing comes down by browsing**: Files and the file dialog draw no thumbnail and no preview of a file that is
  only in the cloud or whose status isn't known yet. Opening, Quick Preview and Get Info's folder size are explicit.
- **Copy Link** copies the existing link, or makes one for 7 days. The account's Shared links are `links(id)`.
- Notifications go out as Settings' (sign-in, full) and Files' (conflict), so Settings → Notifications governs them.
- Right to left: every row uses logical edges (`insetInlineEnd`, `textAlign: 'start'`), the chevrons and the back
  button turn; the bars and rings fill left to right in every language, as LordOS's other gauges.

What the interface needs beyond section 17 (both optional: without them the interface works and says less):

- `cloud.setAccountOptions(id, { unmeteredOnly, bwUp })` and `account.options` with the same keys: the account page's
  **Only on unmetered networks** and **Upload limit** (the Account mock). The core doesn't have them (17.1), so those two
  rows are hidden.
- `State.conflicts: [{ path, kept, account, at }]`, newest first: the "Changed on another device too" notification
  (section 7.2). Without it there is no conflict notification.
- Times (`expiresAt`, `links().expires`, `recent[].at`, `conflicts[].at`) are read as seconds or milliseconds; the
  bandwidth menus write KiB/s (17.1), their MB/s being MiB/s.
- Error codes the interface itself uses: `cloud.unavailable`, `cloud.badId`, `cloud.badKind`, `cloud.uploadFailed`,
  `cloud.pending` (errors.json); `link()`'s `unsupported` is section 17's bare word.

## 19. Verified providers

rclone offers far more providers than anyone has tried on LordOS. The Add Account sheet says which were checked here and
asks people to check the rest. Decided 2026-11-06 with the maintainer.

### 19.1 The list

`assets/cloud/providers.json` names the kinds verified on LordOS, each with `since` (the release), `date` and `evidence`:
`"maintainer test: <what>"` or `"issue #N"` (a person's Check This Account report on GitHub). A kind goes in only when it
was really checked end to end. Today:

| Kind | Evidence |
|---|---|
| `webdav` | `cloud_integration.sh` and `cloud_providers_check.sh` against `rclone serve webdav`, VM 116 (amd64), 13/13 |
| `sftp` | `cloud_providers_check.sh` against the machine's own OpenSSH `sshd` on 127.0.0.1, a throwaway user and key (`key_pem`), 14/14; SFTP has no links |
| `s3` | `cloud_providers_check.sh` against `rclone serve s3` (one bucket, access key and secret), 13/13; links not confirmed: the stand-in's links are `http://` and `lord-server` keeps only `https` ones (`cloud_url_ok`), which a real S3 service gives |

`server/tests/cloud_providers_check.sh <build> <rclone> [webdav] [sftp] [s3]` runs Check This Account's steps through
`build/cloud-harness` (the same `src/cloud`), with the files checked on the server's side too: 40/40 on VM 116 with rclone
v1.75.2 on 2026-11-06. It needs FUSE and, for SFTP, `sudo` without a password (a throwaway user, removed at the end).
It isn't in `run_suites.sh` for that reason. The presets `nextcloud` and `owncloud` aren't marked: no real Nextcloud or
ownCloud server was tried.

### 19.2 Add Account

The verified providers come first, grouped by kind as before. Below them, **Not verified yet** (closed, with how many)
holds every other provider rclone offers, still addable, with the line "These work through rclone, but nobody has
confirmed them on LordOS yet. Using one? Check it and tell us, and we'll mark it verified." and **Tell Us on GitHub**.
Search looks in both parts and opens the second when it has matches.

For a browser sign-in provider not verified yet, the sheet asks before Continue: **Sign in with rclone's app** (the
default) or **Use my own app (client ID and secret)**, with the two fields (the secret masked), the line "Your own app on
the provider's developer site: its quota is yours alone, and the sign-in page shows your app's name. The redirect
address must be http://127.0.0.1:53682/" and **How to make one** (rclone's page for that backend, `https://rclone.org/<kind>/`;
`gcs` is `googlecloudstorage`, the WebDAV presets `webdav`: every page checked to answer 200). The ID and secret go to
`beginSignIn`'s `{ clientId, clientSecret }`, which the core keeps in the keyring (`account.ownClient` says so later). The
same choice comes with **Sign In Again** for such a provider (or any account already on its own app) and under the
account's **Advanced** for every browser sign-in account; a verified provider keeps the plain flow.

### 19.3 Check This Account

On the account's page: a box of its own for a provider not verified yet, under **Advanced** for a verified one (to report
a problem). `sdk/cloudcheck.js` runs it with `lord.fs` and `cloud.*` only, each step with its result and time:

1. **Space information**: `refreshAbout`, then the state's `about` (skipped when the provider tells none).
2. **List the account** (its root).
3. **Make a check folder** `.lordos-check-<random>` (in object storage the root lists buckets: inside the first one, or a
   new bucket `lordos-check-<random>` when there is none).
4. **Write 1 MiB** of random bytes.
5. **Send it up**: its status goes to on this computer (going up seen on the way), 2 minutes at most.
6. **Free Up Space** on it (tried again while rclone still holds it open).
7. **Bring it back, same contents**: read through the mount, compared by hash.
8. **Make a link** and stop sharing it, where the account has links.
9. **Delete the check folder**: `lord.fs` has no delete, so the folder moves out of the cloud (a move job) into a
   scratch folder of the home, which goes to the Trash and out of it. Runs even when an earlier step failed.
10. **Delete it in the cloud**: gone from a fresh listing and nothing of it waiting to go up, 1 minute at most.

Then a **report** with nothing personal: LordOS version and architecture, rclone's version, the provider's kind and
label, whether it is verified, **own app: yes/no** (no IDs), which `about` fields the provider gives (not their values),
whether it has links, each step's result, time and a fixed note word (never an error's text, which can hold a path), and
the totals. No account name, email, file names but the check's, links or tokens. **Copy Report** and **Tell Us on GitHub**.

### 19.4 GitHub

`.github/ISSUE_TEMPLATE/cloud-provider.yml` is an issue form (label `cloud-provider`): provider, does it work (yes, partly,
no), the report, LordOS version, anything else. **Tell Us on GitHub** opens
`https://github.com/lordbasex/LordOS/issues/new?template=cloud-provider.yml&title=Cloud provider: <label>&provider=<label>&version=…&report=…`
(every value URL-encoded) in the default browser; the report is cut at a line, with "[truncated]", so the address stays
under 7000 characters. From the Add Account sheet's line the same address goes without a report. A maintainer who
confirms a report adds the kind to `providers.json` with `"evidence": "issue #N"`.

### 17.2 Additions decided on 2026-11-06 (maintainer)

1. **Every provider the binary has.** `cloud.providers()` returns a **Promise** and asks rclone (`config/providers`) **every time**;
   nothing is cached and there is no fixed list of who is offered. The interface shows a spinner and blocks the sheet until it
   answers. Left out: backends that aren't a storage of their own (alias, archive, cache, chunker, combine, compress, hasher,
   http, local, memory, union, tardigrade and other aliases rclone marks as such). `crypt` stays as the encrypted folder. Each
   item: `{ kind, label, description, group: 'oauth'|'server'|'object'|'crypt', verified, website?, fields, questions? }`;
   `group` is derived from rclone's options (a `token` option → oauth; `access_key_id`/`endpoint`/bucket-style → object; else
   server). The old fixed table becomes an overlay only (presets like Nextcloud, the website address, verified evidence from
   `assets/cloud/providers.json`).
2. **Provider names as rclone uses them**, as plain text, in Add Account and as an account's provider line. This is the only
   exception to the brand rule (no logos, no trade dress, nowhere else). The generic "Office Drive" name goes away.
3. **rclone's own questions.** Some backends ask more during setup (a 2FA code for the phone maker's drive, a choice, a
   confirmation). `cloud.add(kind, name, fields)` may answer `{ question: { state, name, help, type, examples, isPassword,
   required, default } }`; `cloud.answer(state, value)` sends the answer (rclone's `config/create` with `opt.continue`,
   `state`, `result`) and answers the same way again, or `{ id }` when done, or `{ error }`. Generic: any backend's questions,
   not one provider's.
4. **Empty Trash** for providers that have it: `cloud.emptyTrash(id)` → Promise (`operations/cleanup`); `account.trashSupported`.
5. **Measure** space on demand for providers without a quota: `cloud.measure(id)` → Promise `{ bytes, count }`
   (`operations/size`, as an async job, cancellable); the result goes into `about.used` with `about.measuredAt`.
6. **Delete in the cloud.** Move to Trash on a path under `~/Cloud/<account>` must not create `.Trash-1000` in the cloud:
   `cloud.delete(paths)` → Promise removes them through the mount (the provider keeps its own trash where it has one); Files
   asks first ("Delete from <provider>? It goes to <provider>'s trash" / "It can't be undone" when the provider has none).
7. **`lordos-rclone` repackages rclone's official release binary**: `tools/iso/mkdeb-rclone.sh` downloads the release's
   `.zip` and `SHA256SUMS`, verifies the PGP signature with Nick Craig-Wood's key (fingerprint
   `FBF737ECE9F8AB18604BD2AC93935E02FF3B54FA`, pinned in the script, key file in `packaging/keys/`), checks the hash, and puts
   the unmodified binary in `/usr/lib/lordos/rclone/rclone`. It is published in LordOS's own apt repository with each release
   (rclone has no apt repository of its own), so Store → Updates and `apt` keep it current. No Go on the build host.

### 18.1 The interface of 17.2 (items 1 to 6)

- **Providers from rclone, every time.** Add Account asks `cloud.providers()` (a Promise) each time it opens: until it
  answers the sheet shows only a spinner, "Getting the list of providers…" and **Cancel**; a failure says so with
  **Try Again**. Groups follow each item's `group`; verified first (the item's `verified`, else
  `assets/cloud/providers.json`), the rest under **Not verified yet**; search looks at the label, the description, the
  hint and the kind. An account's page keeps the last list of the session (`providerCache`) for its provider's group and
  fields; Sign In Again for a server account asks rclone for the list when this session hasn't yet.
- **rclone's names.** The label shows exactly as rclone gives it, in the tiles, the credentials form and an account's
  provider line; the interface renames nothing. The names come at runtime: none is in a catalog, so brand-check still
  passes.
- **rclone's questions.** `cloud.add` (or `cloud.answer`) answering `{ question }` opens a step in the same sheet: rclone's
  help text, then an input by `questionInput()`: a switch for `bool`, a masked field for `isPassword`, a list of choices
  for 2 to 12 examples, a number field, else text. **Continue** sends `cloud.answer(state, value)` and the next answer is
  handled the same way, until `{ id }` (the account's page opens) or `{ error }` (shown in the step). Before the phone
  maker's drive (`iclouddrive`) the sheet and the form say, in our words from rclone's page, that the drive's extra
  end-to-end encryption must be off, web access to the drive on, and that it asks for a code now and about every 30 days.
- **Empty Trash** on the account's page when `account.trashSupported`: a confirmation ("can't be undone"), then
  `cloud.emptyTrash(id)` with a spinner and the result. Without it, a provider with trashed bytes keeps the website link.
- **Measure** where the provider gives no total: a button with "Measuring lists every file once"; while it runs a
  spinner and **Cancel** (`cloud.cancelMeasure(id)` when lord-server has it; otherwise the answer is just ignored),
  then "<used> used, measured <date time>" from `about.measuredAt`, also in the overview's account row.
- **Delete in the cloud.** Move to Trash in Files (the menu, ❖⌫ and Delete, dragging onto the Trash) splits what is
  chosen (`splitTrash`): local items go to the Trash as before; each account's items are asked about ("Delete from
  <provider>?" with "It goes to its trash on <provider>." or "<provider> has no trash: it can't be undone.") and go to
  `cloud.delete(paths)`, never `lord.fs.trash`, so no `.Trash-<uid>` folder is made in the cloud and Restore isn't
  offered. An account's own folder is never deleted this way. Check This Account (19.3) still moves its own folder out
  through `lord.fs`; with `cloud.delete` in every server it can use that instead.
- The dev mock (`sdk/cloudmock.js`) answers all of it: providers after 0.9 s (`"cloudDevMock": "providersFail"` to see
  the failure), a 6-digit code asked when adding the phone maker's drive, Empty Trash, a 4 s Measure that can be
  stopped, delete.

### 18.2 Sheets in the window, the sign-in inline (fixed 2026-11-06 after the Pi test)

On the Pi the sign-in sheet was drawn in the shell's modal layer, so it floated over the browser and took its input, and
the sign-in page opened more than once. Now:

- Every Cloud sheet (Add Account, credentials, rclone's questions, Sign In Again, Remove, Empty Trash, the folder picker for
  Keep) is drawn by `CloudSheets` at the Settings window's root, over that window's content only; none uses `Layer`.
  A window above Settings keeps its input, and Escape closes a sheet only while Settings is in front. Files' cloud
  dialogs were already inside its window.
- The sign-in waits **inline** at the top of the Cloud pane (`SignInCard`): the time left, **Open Sign-in Page Again**,
  **Cancel**, rclone's line; then "is ready" with Show in Files, Keep everything, **Done** and **Open in Files**. The Add
  Account sheet closes when the sign-in starts, so Settings can go behind the browser.
- One start: every sign-in goes through `startSignIn`, which ignores a second start while one runs, shows again a sign-in
  already waiting for the same account instead of starting another, and cancels one for another account first. Nothing
  in the interface opens the page on start (lord-server does); `cloud.openSignInPage` runs only from its button, and the
  SDK's fallback opener only when lord-server couldn't open it.

### 17.2 As built (branch `cloud-core`, 2026-11-06)

1. **Providers.** `lord.cloud.providers()` answers `{ req }` and the list comes through `__lordCloudReply`; `cloud.providers()`
   is a Promise. Each call runs `config/providers` (rclone is started if it isn't running). Left out: `alias`, `archive`,
   `cache`, `chunker`, `combine`, `compress`, `hasher`, `http`, `local`, `memory`, `union`, `tardigrade`, every backend rclone
   marks `Hide`, and every name another backend lists in its `Aliases`. `kind` is rclone's `Prefix` (`gcs`, `oos`), and
   `config/create` gets its `Name`. `label` is rclone's `Description`, first line; `description` (and `hint`) is ours, from
   the overlay, often empty. `group`: the overlay's when it has one (`cloud_pure.c`), else `cloud_derive_group`: `token` with
   `client_id` and `client_secret` → `oauth` (a `token` alone, as Linkbox or Jottacloud have, is not a browser sign-in);
   `access_key_id`, `secret_access_key`, `endpoint`, `sas_url`, `storage_url`, `bucket`, `bucket_name` or
   `application_key` → `object`; `crypt` → `crypt`; else `server`. `verified`: the kind is under `verified` in
   `assets/cloud/providers.json`. `website`: from the overlay. Fields as before, plus `help` (rclone's whole help). The
   variants (Nextcloud, ownCloud on WebDAV) follow their backend in the list. rclone 1.75.2 gives 59 kinds this way (14 oauth, 33 server, 11 object, crypt), Nextcloud and ownCloud included.
   Accounts keep their kind, rclone type, group and provider name in `accounts.json`, so an account of a backend with no
   overlay comes back after a restart before the list is asked again.
2. **Names.** rclone's, everywhere a provider is named (the list, `account.provider`, the keyring item's label). The overlay's
   labels are only a fallback before rclone has answered, and the variants' own.
3. **Questions.** `add` always answers through `{ req }` (a Promise in the SDK) when its arguments pass: `{ id }`, `{ question:
   { state, name, help, type, examples: [{ value, help }], isPassword, required, exclusive, default } }` or `{ error, message
   }`. `state` is `"<account id>:<rclone's state>"`; `answer(state, value)` runs `config/update` with `opt.continue`,
   `state`, `result` (and `obscure`) and answers the same three ways. LordOS answers only `config_refresh_token` and
   `config_is_local` itself; every other question goes to the person. While a new account waits for an answer it is in the
   state with `configuring: true`; if its setup ends in an error it is removed (`config/delete`), and an existing one being
   updated stays as it was with the error. Secret answers that rclone keeps as options go to the keyring with the rest
   (`split_remote`). Sign-in in the browser still answers rclone's later questions with their defaults. **Measured:** Jottacloud
   asks `config_type` (standard, traditional, legacy), then for a login token; a wrong token ends in `{ error }` and the
   half-made account is gone (`cloud_integration.sh`, both architectures).
4. **Empty Trash.** `emptyTrash(id)` → Promise `{ ok }` | `{ error: 'unsupported' }` | `{ error }`: `operations/cleanup` as an
   `_async` rclone job, followed with `job/status` every second. `account.trashSupported` comes from `operations/fsinfo`'s
   `CleanUp` feature; while it runs `account.emptyingTrash` is true, and `about` is asked again after it. WebDAV answers
   `unsupported` (measured); a provider whose trash really empties is not tested yet.
5. **Measure.** `measure(id)` → Promise `{ bytes, count }`: `operations/size` as an `_async` job; `cancelMeasure(id)` →
   Promise `{ ok }` (`job/stop`), and the measure then answers `{ error: 'cloud.cancelled' }`. One rclone job per account at
   a time (`cloud.busy`). The result is kept in `accounts.json` and shows as `about.used` when the provider gives none, with
   `about.measuredAt` (ms), `measuredBytes` and `measuredCount`; `account.measuring` while it runs. **Measured** on WebDAV.
6. **Delete.** `delete(paths)` → Promise `{ ok, deleted, failed: [{ path, error, errno }] }` or, at once, `{ error, path }`:
   every path must be strictly inside a mounted account's folder (not the folder itself), with no `.`, `..` or `//`, and its
   parent's real path must be its own text (no link on the way); one bad path refuses the whole request. A thread of its own
   removes through the mount: a file or link with `unlink`, a folder with everything in it (`nftw` with `FTW_PHYS` and
   `FTW_MOUNT`: links are not followed, and it never leaves the mount). **Measured:** a file and a folder with a subfolder
   gone on the WebDAV server, no `.Trash` folder made, a path outside `~/Cloud`, the account's folder and a `..` path refused.
7. **`lordos-rclone`** repackages `rclone-v1.75.2-linux-<arch>.zip` from downloads.rclone.org: `SHA256SUMS` must carry a good
   signature by exactly `FBF737ECE9F8AB18604BD2AC93935E02FF3B54FA` (`packaging/keys/rclone-release-<fingerprint>.asc`, from
   keys.openpgp.org, the same key as rclone.org/KEYS), the zip must have the hash it lists, the binary goes in unmodified
   (`cmp` equal to the zip's), and on the build's own architecture it must say `rclone v1.75.2`. The copyright file is
   rclone's MIT license plus every linked Go module's license, kept in `packaging/rclone/copyright-v1.75.2` (generated once
   from a build of the tag). Both packages build in 39 s on VM 116 with no Go; the same release gives the same bytes; a
   changed `SHA256SUMS` or zip is refused.

The closed rc method table has 25 methods now: `operations/cleanup`, `operations/size`, `job/status` and `job/stop` were added.

**Later the same day (fixes from the Pi):**

8. **A locked keyring, then unlocked.** After an automatic login the session keyring stays locked: it is wrapped by the login
   password (Argon2id), which an automatic login never types, so nothing can open it without asking; Mail and Remote Desk
   behave the same (Mail marks its accounts as needing the password, Remote Desk asks on connect). An account whose secrets
   are behind it shows `state: 'locked'` and `error: 'cloud.keyringLocked'` (`cloud.noKeyring` when there is none) while
   `service.running` stays true, and an action on it (measure, links, Empty Trash, delete) answers `cloud.keyringLocked`
   instead of `cloud.notRunning`; an account that isn't connected for another reason answers `cloud.notConnected`.
   `lord-server` hears lord-vault's own `{"ev":"unlocked","v":"login"}` event (`vc_set_event_hook`, `cloud_kr_watch`) and
   connects the waiting accounts at once, with no polling; `cloud.retry(id|null)` does the same on demand (call it after
   unlocking from the interface), and `refreshAbout(id)` of an account that isn't connected also retries it. The only timer
   left is a fallback every 10 minutes. **Measured** (`cloud_integration.sh`, the keyring stand-in locked): the account says
   locked with the service running, `measure` and `delete` answer `cloud.keyringLocked` and remove nothing, and after
   unlocking, `retry` mounts it without a restart.
9. **Busy and stale mounts.** Unmounting retries `mount/unmount` for 1.5 s, then detaches the folder lazily
   (`fusermount3 -u -z`): a program that keeps a file open there no longer leaves the folder mounted (the
   "Device or resource busy" in cloud.log). Before every mount a mount point that answers `ENOTCONN` ("Transport endpoint is
   not connected", an rclone that died) or is still in `/proc/self/mounts` is detached first. **Measured:** Restart with a file
   open in the mount, and `kill -9` of rclone with a file open, both end with exactly one mount at the folder.
10. **Sign In Again and Add after a rename.** The account to update is found by kind name, not by pointer (the overlay's
    entry and rclone's are the same kind; before, after the provider list had been asked, the same kind and name answered
    `cloud.nameTaken`), with the name compared without case. `beginSignIn(kind, name, { accountId })` and `add(kind, name,
    fields, { accountId })` update exactly that account whatever the name (an unknown id is `cloud.notFound`, another
    kind `cloud.badKind`); the account page should pass its id. Accounts loaded from `accounts.json` share the session's one
    entry per kind.
11. **Rename** unmounts the old folder (lazily if busy), moves it, or makes the new one and removes the empty old one, then
    mounts at the new name. **Measured:** renamed with a file open in it: mounted at the new name, the old folder unmounted
    and gone; in a new session the account comes back mounted under its new name, `add` with the same kind and the name in
    another case updates it, and `add` with its id updates it under any name.

### 18.3 After the Pi test (2026-11-06)

- **The credentials form** shows the fields a kind needs (`formLayout`): SFTP Server, Port, User, then **Sign in with**
  Password or SSH key (a key file chosen with the file dialog, read once and sent as `key_pem`, so rclone never needs the
  file, plus its passphrase); WebDAV Address, preset, User, Password; S3 Provider, Endpoint, Region, Access key, Secret key;
  FTP and SMB alike; any other kind its required options and those without a default. The rest is under **Advanced
  options** (closed): a short label (the first clause of rclone's help), the control, rclone's whole help dimmed and the
  option's name. Only a password is masked (`isPassword`, which lord-server now gives per field with its `type`; rclone's
  `Sensitive` host and user stay plain text), yes/no options are switches, rclone's choices menus. The form's body scrolls
  inside the Settings window between its title and Back / Connect.
- **The account page** uses the width: Settings gives Cloud a 980-pixel column; two balanced columns from 900 pixels
  (space, kept folders, shared links and Check This Account / options, status, Sign In Again and Remove), one below that,
  and **Advanced** full width at the bottom with the two sign-in cards side by side. The folder name is a field on click:
  Enter, Save or a click elsewhere renames (`cloud.rename`), Escape leaves it, a problem shows under it.
- **A locked keyring** (`cloud.keyringLocked`, after an automatic login) is said once, in the status box, with
  **Unlock Keyring…** (the system's keyring prompt, `askKeyringUnlock` in `sdk/vault.js`) and "Your sign-ins are kept in the
  keyring, which locks when you log in without a password."; the same button is on the overview's row and in the menu bar
  panel. After unlocking, `cloud.retry(null)` connects the accounts at once.
- **Errors**: an account's space without an answer says "The space shows once the account is connected" (no Measure: that
  is only for a provider that answered without a total); Measure and Empty Trash show their own error's text, and
  `cloud.notRunning` is read as the account's reason while the service runs (`explainError`).
- **Sign In Again** passes `{ accountId }` to `beginSignIn` and `add` (17.2 item 10).



### 18.4 After the end-to-end run (2026-11-06)

- **Trust questions** come in the same steps as rclone's questions: `{ question: { kind: 'hostKey', host, port, fingerprint } }`
  asks "Is this the right server?" with the SHA256 fingerprint in monospace and **Trust**; `kind: 'certificate'` asks "This
  server's certificate isn't trusted by the system" with fingerprint, issued to, issued by and **Trust This Certificate**.
  Both answer `cloud.answer(state, 'trust')`; Cancel goes back to the credentials with what was typed.
- **Errors with details**: `cloud.hostKeyChanged` shows both fingerprints (`old`, `new` of the answer), `cloud.cantRevoke` the
  date the link stops working (`expires`), and `cloud.certUntrusted`, `cloud.unreachable`, `cloud.badAddress`,
  `cloud.notConnected`, `cloud.keyringLocked`, `cloud.noKeyring`, `cloud.notRunning`, `cloud.notFound`, `cloud.badKind` their
  own texts (`errorVars`, `errorMessage`). An error of `add` or of a later step shows under the credentials form, the field it
  belongs to outlined (lord-server's `field`, else by the code: an address for unreachable and bad address, the password
  for a failed sign-in), with everything typed kept.
- **Provider names**: an account's line uses the provider list's short label (asked once in the background when the pane
  opens), else rclone's description cut before "including"; it is cut with an ellipsis where it doesn't fit.
- **Measure** shows for a connected account whose provider gives no total, `about` null included (S3).

**From the end-to-end tests (2026-11-06, B3–B9):**

12. **Options JS may set (B5, 4.2).** Only an allow-list per kind: rclone's non-advanced, non-hidden options of that
    backend, minus every option `cloud_option_refused` catches, which is applied again to whatever goes to `config/create`:
    names ending `_command`, `_file`, `_path`, `_socket`, `_dir`, `_cmd`, `_exec` or `_agent`; `ssh` (sftp's external ssh
    command), `server_command`, `key_use_agent`, `ssh_agent`, `unix_socket`, `known_hosts_file` (LordOS sets it, item 14),
    `env_auth`, `use_msi`, `use_az`, `use_cli`, the proxies, `auth_url`, `token_url`, `client_credentials`, every `config_*`;
    and every option whose type isn't plain data (string, int, bool, size, duration, tristate, float, comma list, encoding,
    bits, time): sftp's `ssh` is a `SpaceSepList`. Keys go as content (`key_pem`). `cloud.add('sftp', …, { ssh: 'sh -c …' })`
    answers `{ error: 'cloud.badField', field: 'ssh' }` and runs nothing; of rclone 1.75.2's 59 kinds and 187 offered
    fields none matches those patterns (`cloud_integration.sh`).
13. **A new account must answer (B4).** Before `{ id }`, its top folder is listed (`operations/list`, folders only, 20 s). On
    failure the account is removed whole (rclone's remote, the trust files; nothing was saved yet) and the answer is
    `cloud.authFailed`, `cloud.unreachable`, `cloud.badAddress`, `cloud.certUntrusted`, `cloud.hostKeyChanged` or
    `cloud.addFailed` with rclone's `message`, and `field` where one is to blame (`url`, `endpoint`, `host`, `pass`,
    `key_pem`). An update keeps the account and its old credentials (from the keyring) and reports the same. A mounted account
    is listed again after mounting, every 10 minutes (2 while it fails) and on `retry(id)`, and its `state`/`error` follow
    (B6: a password changed on the server shows `cloud.authFailed` without a restart).
14. **Who the server is (B9, B6; `cloud_trust.c`).** As Connect to Server: SFTP keys read with `ssh-keyscan`, a new one
    answered as `{ question: { state, kind: 'hostKey', host, port, fingerprint } }`; `answer(state, 'trust')` keeps it in
    `~/.config/lordos/cloud/known_hosts/<id>` (0600) and rclone always gets `known_hosts_file` pointing there (no more "No host
    key validation"); a changed key answers `{ error: 'cloud.hostKeyChanged', old, new }`. An https address (WebDAV, S3's
    endpoint…) whose certificate the system doesn't trust answers `{ question: { state, kind: 'certificate', host, port,
    fingerprint, subject, issuer } }`; trusted, it is kept in `~/.config/lordos/cloud/certs/<id>.pem` (0600) and given to that
    account alone as rclone's `override.ca_cert` (measured: another account on the same server is asked again;
    `no_check_certificate` is never used); a changed one answers `{ error: 'cloud.certChanged', old, new }`; one that doesn't
    name the host is `cloud.certUntrusted`. Any other answer than `trust` is `cloud.notTrusted`. `ssh-keyscan`, `ssh-keygen`
    and `openssl` run through `rconnect_run` in Cloud's scope (read-only, 15 s, no TCP for the offline ones).
15. **Rename in a mount (B7).** FUSE (rclone's mounts, also Connect to Server's) refuses `RENAME_NOREPLACE` with `EINVAL`:
    `fsops_rename` then checks that nothing is at the target and does a plain `rename`; another program creating the
    target in those microseconds would be replaced, only on those filesystems.
16. **Links (B8).** Addresses up to 4096 characters (presigned links). Stop Sharing on S3-style backends (`s3`,
    `oracleobjectstorage`, `qingstor`) answers `{ error: 'cloud.cantRevoke', expires }` (ms, or null) and drops the link from
    `links.json`: a presigned address can't be withdrawn before it expires. Their `trashSupported` is false (CleanUp there
    removes unfinished uploads, not a trash).
17. **`add` before `providers()`** asks rclone for the list itself instead of answering `cloud.notReady`.
18. **Recents and search (B3).** `fs_recent` and `fs_search` skip every mount point under the home folder by path (from
    `/proc/self/mounts`, before any `lstat`) and stay on the home's filesystem; `fs_du` and the Trash's size stay on theirs.
    Measured: with the WebDAV server of a mounted account stopped (`SIGSTOP`), Recents and search answer at once and list
    nothing from `~/Cloud`.

### 18.4 After the end-to-end run (2026-11-06)

- **Trust questions** come in the same steps as rclone's questions: `{ question: { kind: 'hostKey', host, port, fingerprint } }`
  asks "Is this the right server?" with the SHA256 fingerprint in monospace and **Trust**; `kind: 'certificate'` asks "This
  server's certificate isn't trusted by the system" with fingerprint, issued to, issued by and **Trust This Certificate**.
  Both answer `cloud.answer(state, 'trust')`; Cancel goes back to the credentials with what was typed.
- **Errors with details**: `cloud.hostKeyChanged` shows both fingerprints (`old`, `new` of the answer), `cloud.cantRevoke` the
  date the link stops working (`expires`), and `cloud.certUntrusted`, `cloud.unreachable`, `cloud.badAddress`,
  `cloud.notConnected`, `cloud.keyringLocked`, `cloud.noKeyring`, `cloud.notRunning`, `cloud.notFound`, `cloud.badKind` their
  own texts (`errorVars`, `errorMessage`). An error of `add` or of a later step shows under the credentials form, the field it
  belongs to outlined (lord-server's `field`, else by the code: an address for unreachable and bad address, the password
  for a failed sign-in), with everything typed kept.
- **Provider names**: an account's line uses the provider list's short label (asked once in the background when the pane
  opens), else rclone's description cut before "including"; it is cut with an ellipsis where it doesn't fit.
- **Measure** shows for a connected account whose provider gives no total, `about` null included (S3).

### 18.5 The e2e first pass, interface items (U3, U4, U6, U7)

- **U3**: a file that was coming down keeps "coming down" (at 100 %) for up to 8 s when an answer says "in the cloud"
  right after (`mergeStatusHeld`), so it no longer flickers back before "on this computer"; Free Up Space still turns a
  file to "in the cloud" at once.
- **U4**: "All accounts" says "<used> of <total>" only when every account has a quota, else "<used> used" (a measured
  `used` is never a total). An account without a quota that nobody measured (`used` 0, no `measuredAt`) says "Space not
  measured" in Settings and nothing in Files' status bar.
- **U6**: WebDAV's presets list plain WebDAV ("other") first and choose it by default (`presetField`).
- **U7**: while `service.running` is false every account says "Not connected: the cloud service isn't running" (dim with
  "!" in Files' sidebar), and the overview starts with the service's own message, **Restart** and **Show Log**.
- The server form no longer promises a fingerprint prompt; the trust step (18.4) shows only when lord-server asks.
19. **Offline (B10).** rcd runs with `--contimeout 5s --timeout 30s --low-level-retries 1`: with the server refusing, a read of a
    file not on this computer fails in about 8.6 s instead of more than 200 s (measured; uploads are retried by the VFS queue
    anyway). A transfer that fails for the network lists the account at once; a listing that can't reach the server sets
    `error: 'cloud.unreachable'` and `state: 'offline'`, retried every 30 s, and `ok` again when it answers. On the interface's
    thread `lord.fs.read` of a cloud file not on this computer, of an account offline, locked, not mounted or with the
    service down, fails at once with `cloud.offline` (`cloud_read_would_block`); what is here still opens. `lord.fs.du` on a
    cloud, network or FUSE folder answers at once with `remote: true` and no size (the UI shows the provider's), and
    `lord.fs.search` there finds nothing for now (N2 is to make it work off the interface's thread). `lord.fs.list` of a folder
    rclone hasn't listed lately can still wait for rclone's timeouts (about 8 s) while offline: not changed.
20. **Logout (B11).** `lord-server` stops Cloud after closing the apps: every account unmounted (a busy one lazily), rclone
    asked to quit, up to 15 s, and anything still mounted detached. Measured: the session ended with a file open in the
    mount leaves nothing in `/proc/self/mounts`.
21. **Messages (U5)** shown under Get Info are rclone's words with HTML tags dropped, white space collapsed, at most 200 bytes.
22. **Service down (U7):** while rclone isn't running (after `cloud.crashed`, a failed start…) every account says
    `state: 'error'` with the service's error (`cloud.crashed`, `cloud.startFailed`, or `cloud.notRunning`).
23. **Conflicts (7.2, as built).** Before a changed file goes up, it is held and the cloud's object is looked at
    (`operations/stat`). If it changed since this copy came down (rclone's fingerprint in `vfsMeta`: size, plus modtime and
    hash where the backend has them; a modtime within a second counts as the same), or it appeared in the cloud meanwhile, the
    cloud's version is first copied in the cloud to **"name (cloud copy).ext"** (`operations/copyfile`, inside the account
    only, added to the closed table), then ours goes up under the name. `State.conflicts: [{ path, kept, account, at }]`
    (newest first, at most 20) names both: `path` is ours, `kept` the cloud's copy. **This differs from 7.2's naming**
    ("name (this computer).ext" for ours): renaming our file in the mount moves the cloud's object with it in rclone's VFS, so
    the cloud's version would have been overwritten (measured), and rclone has no way to drop a pending upload. If the copy
    fails the file stays held and is tried again. Limits: a server change that keeps the size and gives no modtime or hash
    (plain WebDAV) can't be seen; a change made in the second between the check and the upload is lost.
24. **lord.fs.list** stops at 5000 entries per folder without saying so (`js_sys.c` `f_fs_list`): not changed here; it is a
    bound of the interface's thread, not Cloud's.

The closed rc method table has 27 methods now (`operations/list`, `operations/stat`, `operations/copyfile` added).
