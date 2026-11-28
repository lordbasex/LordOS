# Debian packages in LordOS

LordOS installs and removes Debian packages with its own code: it reads the apt sources,
downloads and verifies the signed indexes, resolves dependencies and downloads the `.deb` files.
**dpkg** does only the last step: unpacking and configuring. It doesn't use apt or libapt,
gpgv/sqv, curl, zlib or liblzma. The dpkg database (`/var/lib/dpkg`) is still written only by
dpkg, so `apt` and `dpkg` keep working the same on the same system.

The code is in `server/src/pkg/`:

| File | What it does |
|---|---|
| `crypto.c` | SHA-256, SHA-512, RSA PKCS#1 v1.5 verification (Montgomery) and Ed25519 (TweetNaCl arithmetic) |
| `pgp.c` | Binary or armored OpenPGP keyrings, subkeys, expirations and revocations; v4 signatures of clear-signed messages (`InRelease`) |
| `xz.c` | Our own xz/LZMA2 decompressor; gzip with `stb_image`'s inflate (already there for PNGs) |
| `deb.c` | dpkg version comparison, deb822 fields, relations (`a (>= 1) \| b`) |
| `idx.c` | The compact index that `pkg-update` builds and that is read with mmap |
| `status.c` | Reading `/var/lib/dpkg/status` (what is installed) |
| `resolve.c` | Dependency resolver and installation order |
| `pin.c` | apt's preferences and priorities (0.8.5, see "How it resolves") |
| `http.c` | HTTP/1.1 client, with a child process that runs as `nobody`; HTTPS through `lord-tls` (0.8.5, see "HTTPS") |
| `sources_admin.c` | `pkg-source-add`/`pkg-source-remove`: vendor repositories from LordOS's catalog (0.8.5) |
| `pkg.c` | Privileged helper actions (update, install, remove, local `.deb`) |
| `../ui/js_pkg.c` | `lord.pkg`, the queries for the UI (without root) |

## Security

- **Signatures.** An `InRelease` is only valid if it has at least one valid signature from a key
  in the source's keyring (`Signed-By`; if missing, those in `/etc/apt/trusted.gpg.d`). The key
  must be able to sign, and can't be expired or revoked. The signature can't be expired or come
  from the future. If a known key has an invalid signature, the whole file is rejected.
- **Rejections.** An `InRelease` is also rejected in these cases:
  - it expired (`Valid-Until`);
  - it is from another suite;
  - it is older than the one already stored (replay attack);
  - it carries data after the signature.
- **Hashes.** Every `Packages`, `Translation`, DEP-11 and `.deb` is verified against the SHA-256
  given in the signed index. Only then is it stored, read or passed to dpkg.
- **Keyring.** It is taken as trusted, as gpgv does: it is a root-owned file installed by
  `debian-archive-keyring`.
- **HTTPS since 0.8.5.** Until 0.8.4 LordOS had no TLS of its own and downloaded `https://` sources
  over `http://`, with the same path; since 0.8.5 they are downloaded over HTTPS through the sandboxed
  `lord-tls` helper (see "HTTPS" below), and `http://` sources as before. Either way security comes
  from the signatures and the SHA-256 hashes, as with apt; HTTPS adds privacy (someone watching the
  network no longer sees which packages are downloaded from an HTTPS source) and refuses an impostor
  server. A secure download is never redirected to plain HTTP. (A repository on a CDN should still
  serve plain HTTP as is, for systems not yet on 0.8.5: [UPDATES.md](UPDATES.md), "Serving it".)
- **Local repositories.** A `file:/path` source (a repository on a disk or a USB stick, like apt's
  `file:` method) is read by the same unprivileged child, as `nobody`: only world-readable files,
  checked exactly like a download. An HTTP redirect can never lead to a `file:` URL.
- **Its own firewall app** (0.8.5): before dropping root the download child moves itself into
  `lordos-net-store.scope` (`cgroup_enter_root`), so the Store and the updates are "Store and
  updates" (`lordos:store`) in Monitor → Network, with their own rules; blocking them asks first,
  because no security update arrives while they are blocked (`FIREWALL.md`, "LordOS components in
  their own scopes").
- **Downloads without root.** The process that talks to the network drops privileges before
  connecting: it becomes `nobody`, with no groups, with `no_new_privs` and no way back to root.
  It passes the bytes to the root process through a pipe. The root process only writes them into
  its folder, counts the size and computes the SHA-256: it doesn't interpret anything it
  receives.
- **Locks.** While dpkg works, LordOS takes dpkg's frontend lock
  (`/var/lib/dpkg/lock-frontend`, same as apt) and passes it `DPKG_FRONTEND_LOCKED=1`. If apt is
  running, LordOS waits its turn: they don't step on each other. LordOS operations among
  themselves are queued (`/var/lib/lordos/pkg/lock`).
- **Local `.deb`** (`pkg-install-file`):
  - the file must be in the user's folder;
  - it is opened with that user's permissions (`setfsuid`), walking the path component by
    component without following links (`O_NOFOLLOW`);
  - it is copied to a new root-owned file, and everything else (reading the control file,
    installing) is done on that copy, so the user can't change it midway.
- **Essential packages.** Packages with `Essential: yes` or `Protected: yes` are never removed.
- **Names.** They follow Debian's syntax (`[a-z0-9][a-z0-9+.-]+`). Arguments are validated in the
  helper, even though lord-server has already validated them.

## Files

| Path | Contents |
|---|---|
| `/var/lib/lordos/pkg/index.bin` | Index (packages, "provides", AppStream apps, sources, texts and long descriptions); world-readable |
| `/var/lib/lordos/pkg/lists/` | Verified `InRelease` files and the compressed indexes, named by their SHA-256 (what didn't change isn't downloaded again) |
| `/var/lib/lordos/pkg/icons/64/` | 64×64 icons of the desktop apps (DEP-11) |
| `/var/lib/lordos/pkg/auto` | Packages LordOS installed as dependencies |
| `/var/lib/lordos/pkg/sources` | SHA-256 of the list files the index was built from: when a component or source is added or removed, the next `pkg-update` rebuilds the index even if no signed release file changed |
| `/var/lib/lordos/pkg/notes` | The verified `main/notes/ReleaseNotes` of every source that has one (LordOS's; [UPDATES.md](UPDATES.md)); world-readable |
| `/var/cache/lordos/pkg/` | `.deb` files while they're being installed (deleted when done) |
| `/opt/google/chrome/initial_preferences` and the others of "Browser defaults" | LordOS's initial preferences for a Chromium-based browser, merged into the vendor's file if it ships one (0.8.8.9) |

## Helper actions (`admin.run`)

| Action | Argument |
|---|---|
| `pkg-update` | (none) |
| `pkg-install` | `name name… [--recommends] [--allow-remove]` |
| `pkg-remove` | `name name… [--cascade] [--purge] [--keep-orphans]` |
| `pkg-autoremove` | (none): removes dependencies that are no longer used |
| `pkg-upgrade` | `[--allow-remove]` |
| `pkg-install-file` | `/home/user/…/file.deb [--allow-remove]` |
| `pkg-source-add` | `<id>` (0.8.5): a catalog entry's vendor repository |
| `pkg-source-remove` | `<id>` (0.8.5) |
| `pkg-foreign-arch` | `i386` (0.8.8.9): lets dpkg install i386 packages, on amd64 only (see "Multiarch") |

All of them run in a separate process (`A_FORK`) and require an administrator user.

**Vendor repositories (0.8.5, `sources_admin.c`).** Store → Artificial Intelligence and Store →
Commercial add a maker's signed APT repository only when one of its apps is installed. Only the id comes
from the caller: the entry is read from LordOS's catalog (`<LordOS dir>/apt/catalog.json` and
`apt/keys/`, shipped root-owned by the lordos package), must be `"type": "apt", "delivery": "direct"`,
every field passes a strict character set, and the key file must hold a primary key with exactly the
catalog's fingerprint. The action writes `/etc/apt/sources.list.d/lordos-<id>.sources` (Signed-By that
key) and `/usr/share/keyrings/lordos-<id>.asc` as fixed-format files. A package that always writes its
own source (Cursor, Chrome, Edge) gets the vendor's own file names from the catalog's `files`, with the
same text, so the package's write changes nothing (two entries for one repository with different
Signed-By would make apt refuse every source); for Spotify, whose package adds `spotify.list` unless that
file exists, the catalog's `optout` makes `pkg-source-add` create it empty (only when missing) and
`pkg-source-remove` delete it only while it is still empty. Removing the last installed app of a
repository removes that repository.

**A vendor package that owns its own source (0.8.8.9; Steam).** Valve's `steam-launcher` ships its
source as one of its own files: the conffile `/etc/apt/sources.list.d/steam-stable.list`
(`deb [arch=amd64,i386 signed-by=/usr/share/keyrings/steam.gpg] https://repo.steampowered.com/steam/ stable steam`) and
the keyring `/usr/share/keyrings/steam.gpg`. LordOS can't write those names first (dpkg would find files it doesn't own),
so the catalog entry says the package owns them: `"files": {"list": "steam-stable.list", "keyring": "steam.gpg",
"ownedBy": "steam-launcher"}`. `pkg-source-add steam` writes the usual `lordos-steam.sources` and `lordos-steam.asc`
as a bootstrap for the first installation (or nothing, when the package is already installed with its own source).
After every `dpkg` run of the engine and before `pkg-update` reads the sources, `pkg_sources_handover()`
(`sources_admin.c`) removes LordOS's two bootstrap files once, and only once, the owning package is installed, its list
is a root-owned file not writable by others with an active `deb` line for the catalog's URL and suite, and its keyring
holds a primary key with the catalog's fingerprint; so apt never sees two entries for one repository with different
`Signed-By`. `pkg-source-remove steam` removes the bootstrap files if they are still there, and the vendor's list only
when the package is gone (dpkg keeps a removed package's conffile while its keyring goes with it) and the list names
nothing but that repository. Every name and field comes from the root-owned catalog and is checked root-side
(`good_listname`: `[a-z0-9._-]`, ending in `.list`, never `lordos*`; the keyring as for `files`; the package a Debian
name); nothing comes from the caller but the id. A catalog source may also say `"multiarch": ["i386"]` (the only value,
only for an amd64-only entry): its `.sources` then lists `Architectures: amd64 i386`. One-line `.list` files are now
read with `strtok_r`, so options after `arch=` (`signed-by=` in Valve's line) are no longer lost. The catalog itself is described in `UPDATES.md`, "Mirrored apps
(catalog)".

### Browser defaults (0.8.8.9)

Debian's Chromium starts new profiles with its own bar and the GTK theme because LordOS writes
`/etc/chromium/master_preferences` (WAYLAND.md, "The browsers by default"). The Chromium-based browsers
from the Store and the vendors' repositories don't read `/etc/chromium`: Chromium looks for its initial
preferences only next to its binary (`InitialPreferences::Path`: `<binary's directory>/initial_preferences`,
or `master_preferences` when that one isn't readable), and there is no `/etc` location and no policy for the
GTK theme. So after `dpkg` installs or upgrades one of them, the same `pkg-install`, `pkg-upgrade` or
`pkg-install-file` action writes LordOS's two keys there (`server/src/pkg/browserprefs.c`, called from
`apply_plan` in `pkg.c`):

| Package | Directory |
|---|---|
| `google-chrome-stable`, `google-chrome-beta`, `google-chrome-unstable` | `/opt/google/chrome`, `chrome-beta`, `chrome-unstable` |
| `brave-browser`, `brave-browser-beta`, `brave-browser-nightly` | `/opt/brave.com/brave`, `brave-beta`, `brave-nightly` |
| `microsoft-edge-stable`, `microsoft-edge-beta`, `microsoft-edge-dev` | `/opt/microsoft/msedge`, `msedge-beta`, `msedge-dev` |

- **Closed list.** The table is compiled in; a package name only selects a row and nothing else comes from
  the caller. Debian's `chromium` is not in it (it has `/etc/chromium`), nor Vivaldi (its interface is its
  own, and neither key changes it).
- **What is written.** `"browser": {"custom_chrome_frame": true}` and
  `"extensions": {"theme": {"system_theme": 1}}` (1 = GTK), the same as Chromium's file. With no file, a new
  `initial_preferences` (0644) with exactly those keys. A file the vendor ships (`initial_preferences`, or
  `master_preferences` when that is the one the browser reads, so LordOS never creates a file that would
  hide it) is **merged**: it must be a JSON object (`json_parse`), the vendor's keys and values stay, and
  each of LordOS's keys is inserted as text only where it is absent; a key the vendor already sets, or a
  parent that isn't an object, is the vendor's and is kept. A file that isn't JSON, a symlink or any other
  type is left alone, and so is a directory that is a symlink or doesn't exist (the browser isn't
  installed: nothing is created). The write is a temporary file in the same directory renamed over the
  old one; running it again changes nothing.
- **Why after every upgrade.** A file the package doesn't own survives upgrades, but a vendor may start
  shipping one, which dpkg would put back without LordOS's keys.
- **Never fails an installation.** A file left alone or a failed write is a line in the helper's log
  (`pkg: <package>: …`), not an error.
- **Only new profiles.** These are initial preferences: Chrome, Brave and Edge copy them into a profile
  when it is created. Home folders are never touched; an existing profile changes in the browser's
  Settings → Appearance → Theme → GTK (and «Use system title bar and borders» off). Settings → Appearance's
  browser switch (`hw/brframe.c`) writes Chromium's and Brave's profiles, not Chrome's or Edge's.
- **Already installed.** Migration `009-0.8.8.9-chromium-family` runs the same code once for every browser
  of the list that is installed, as root from the lordos package's postinst, through
  `/usr/lib/lordos/bin/lord-browser-prefs` (`src/pkg/browserprefs_tool.c`): a separate tool built from
  `browserprefs.c` and `sec/json.c` only (libc), with no arguments (it refuses any), no environment, nothing
  executed; it always exits 0. lord-server, which runs without root, has no entry point for it.
- **Tests.** `build/browserprefs-test` (the merge rule over vendor files, and the files in a fixture root:
  not installed, a new file, `master_preferences` only, both files, not JSON, symlinks, a second run) and
  `tests/migration_chromium_family_test.sh` (migration 009 over a fake system tree, and `lord-browser-prefs` refusing an argument). Not yet checked with
  the real browsers on the VM.

### Confirmations

- Recommended packages are not installed by default. `--recommends` adds them, and if one of them
  can't be installed it doesn't stop the rest. The Store never passes it (it would also take the Recommends of every
  new dependency, recursively); a catalog app that needs some of its packages' Recommends names them in its `"with"`
  instead (Steam's 32-bit Vulkan drivers, see "Multiarch").
- (0.8.8.7) **An upgrade brings the Recommends the new version adds**, as apt's full upgrade does. This holds for
  Update All (`pkg-upgrade`) and for upgrading one package. A group is added only if none of its names were
  recommended by the installed version, so a recommended package the user removed stays removed. They are optional:
  one that can't be installed doesn't stop the upgrade. This is how `whiptail` and `zstd`, added to the lordos
  package's Recommends in 0.8.8.7, reach systems that are already installed.
- If a request forces removing something that wasn't asked for, the action fails with
  `ERR pkg.removeAlso names=a,b` ("these must also be removed"). For example:
  - installing `postfix` removes `exim4`, because they are two mail programs and can't coexist;
  - removing a library removes what depends on it.

  The UI shows the plan (`lord.pkg.plan`) and, if the user accepts, repeats the request with
  `--allow-remove` (when installing) or `--cascade` (when removing).
- **Orphans.** When removing, the dependencies LordOS installed (recorded in
  `/var/lib/lordos/pkg/auto`) that nobody uses anymore after that change are removed too, as
  `apt autoremove` would do afterwards. They appear in the plan with `reason: 'auto'` and do
  **not** need `--cascade`. `--keep-orphans` keeps them. `pkg-autoremove` cleans up the ones that
  were already left over.

### Progress

While running, the action sends `LOG` lines:

```
LOG plan install featherpad 1.6.1-1 603084        (one per package: install|upgrade|remove, version, bytes)
LOG plan held libfoo                               (upgrades that can't be done now)
LOG plan total 4034148 24981                       (bytes to download, KB the installed size changes by)
LOG progress download 1048576 4034148              (phases: lists, index, download, unpack, configure, remove)
LOG Unpacking featherpad (1.6.1-1) ...             (everything else: text for the log)
OK pkg.done installed=3 upgraded=0 removed=0
```

(The final `OK` line is a code line, as every helper reply: the UI shows it translated, "3 installed,
0 upgraded, 0 removed".)

The `progress <phase> <done> <total>` lines are for a progress bar.

### Notifications

When finished, the helper sends an unrequested notification:
- `EVENT pkg index`, after updating the list;
- `EVENT pkg changed`, after installing or removing.

The UI receives them with `admin.onEvent()`.

## The Store

`apps/store`. In "Discover" it shows the AppStream desktop apps by category, with their translated
names and descriptions and their icon. Search returns apps first and then the other packages. A
package's page, before installing or removing, shows the plan: what gets installed, upgraded or
removed, how much is downloaded and how much disk space it takes. It also shows the task's
progress. "Updates" lists what can be upgraded and upgrades everything together. From Files, a
`.deb` opens in the Store (`pkg-install-file`).

| | |
|---|---|
| ![debian](store-debian-packages.png) | ![plan](store-install-plan.png) |
| Debian apps by category | The GNOME Sudoku plan: the same 9 packages as `apt-get -s install` |
| ![sudoku](store-installed-app.png) | |
| Installed and opened from the Store (GTK 4 + libadwaita, in Spanish, with its AppStream icon in the Dock) | |

**The Store since 0.8.5.** A sidebar in three groups (Store: Discover, For this computer, Installed,
Updates; Categories: Artificial Intelligence, Web apps (0.8.8.8), Commercial, Remote access and
Third-party apps when the catalog has any, and Debian's ten categories, each with its count; LordOS
apps), the page's title, a back button and the search in a toolbar, and apps as cards in a grid (icon,
name, category, two lines of summary, the download size and a Get, Open or View button). Discover opens
with a featured banner, then the AI shelf and a shelf per category; an app's page has a row of facts
(version, size, category, distribution). The hardware suggestions moved to their own page, **For this
computer** (below). Store → Installed shows each native LordOS app's own version.

**The origins' shape (in development; the latest release is 0.8.9.5; asked by the maintainer on 2026-11-23).** Every
package system's section (GNU/Linux apps (Debian), Flathub apps, Web apps (PWA), LordOS apps: one name each, in the
sidebar, the titles, the search fields and Installed's filters) opens on its shop with nothing above it. Its second
column is **Installed** (that system's installed apps), **Settings** (`apps/store/sectionsettings.js` and each system's
own: what the system is, its switches, turning it off, the package lists' last check with Check for updates and the
automatic check) and its **Categories**. The title bar has an update icon next to the search where there is something
to fetch (Flathub's catalog; the package lists for GNU/Linux apps and LordOS apps), and the search field searches only
that section ("Search Flathub apps"); Discover and Installed search every system, and the other Store pages send a
search to Discover. Sections are `src:debian`, `src:flathub`, `src:web` and `src:lordos`, with `sub` null (the shop),
a category, `installed` or `settings` (`apps/store/installedmodel.js`).

**LordOS apps, LordOS's own store (in development; the latest release is 0.8.9.5).** Its shop lists LordOS's own
installable apps and components as cards, from `assets/store/lordos-apps.json` (`apps/store/lordosmodel.js` reads and
checks it: a known `kind`, an id, a package name, an `open` that is an app id or a Store page; a new native app is an
entry and its two texts, `store.native.<id>.name` and `.summary`, in every language). Kinds: `deb` (a package of
LordOS's repository: the usual package page, the plan and `pkg-install`), `catalog` (an `apt/catalog.json` item with its
own page: Disk Formats, licenses first) and `settings` (installed from Settings: the voices). The first four:
`lordos-studio-camera`, `lordos-disk-formats`, `lordos-web` and the `lordos-voice-*` models. Studio Camera stays a
hardware suggestion in For this computer, and Disk Formats stays in Commercial; hardware drivers (NVIDIA and the rest)
stay in For this computer only, and `lordos-rclone` and `lordos-hwdata` aren't offered (every install has them). Its
Installed lists the installed items (View, Open) and the installed `.lpk` apps (trust, Restore, Share, Remove, Open);
the `.lpk` files in the home folder are listed under the shop's cards. Store → Installed lists the items too, under
the LordOS apps filter. Links open an item's page with `{ page: 'lordos', native: '<id>' }`.

**Web apps in the Store (0.8.8.8).** The Web apps section lists the web app catalog
(`assets/webapps/catalog.json`, 37 apps), and Discover shows a web apps banner and shelf after the AI
shelf; search and each category's page show the matching web apps too. Every card has a "Web app" tag.
The first web app installed also installs `lordos-web` (the LordOS Web Runtime) through the Store's own
package job; an app's page shows the version, the day LordOS tested it, its data and memory, the
permissions and the profiles. Details in [WEBAPPS.md](WEBAPPS.md), "The catalog" and "The Store:
Discover, the first install and an app's page".

**Store → Installed (0.8.8.8).** Every installed app, with a filter: **All** or
**Deactivated**.

- *Removing a Linux app.* "Remove" shows for a Linux app that a Debian package brought. The package is
  found first through the index (AppStream names the app's `.desktop` file), then through dpkg's own
  file lists: `lord.pkg.ownerOfApp(desktopId)` (below), so a `.deb` installed from a file (ZapZap:
  `/usr/share/applications/com.rtosta.zapzap.desktop` belongs to `zapzap`) or an app the index doesn't
  name (VLC) can be removed too. The button opens the same plan sheet as a package's page: what else
  would be removed and how much space comes back, then Remove (`pkg-remove`, the existing action; an
  administrator only). It is never offered for an Essential, Protected or `Priority: required`
  package, and when the plan would remove the `lordos` package itself the sheet says so and has no
  Remove button (and the app's button goes for the rest of the session).
- *Deactivating a LordOS app.* A built-in app LordOS can do without has **Deactivate** (and, once
  deactivated, **Activate**); the core keeps "System app — part of LordOS". The lists are in
  `sdk/appsoffmodel.js` (tested by `server/tests/apps_off_model_test.mjs`, which also checks that every
  app in `apps/` is in exactly one of them):

  | Deactivatable | Core (never) |
  |---|---|
  | Remote Desktop, Machines, Databases, Imager, Scanner, Voice Recorder, Camera, Characters, Calculator, Clock, Calendar, Contacts, Mail, Notes, Reminders, Photos, Player, PDF Viewer, Fonts, Cleaner, Backups | Files, Settings, Store, Terminal, Monitor, Logs, Disks, System Information, Force Quit, Screenshots, Help, Passwords, Text Editor, About, About this app, the installer, the sandbox probe |

  The choice is per user, in `settings.json` → `appsOff` (a new key with a default: no migration;
  only deactivatable ids are ever read from it). A deactivated app stays installed and verified with the
  `lordos` package, but: it leaves the launcher, the start menu, the Dock (unpinned; pinned again on
  Activate), Spotlight (Notes' notes too), Files' "Open With" and Settings → Default Apps. A file type
  whose default is a deactivated app opens with the next capable app; a file only that app opens gives
  the notice instead. `launchApp` and `wm.open` refuse it (`setOpenGate` in `sdk/windows.js`) and show
  a small notice, "<App> is deactivated", with **Activate** (which then opens what was asked); session
  restore leaves it closed. What it did in the background stops (`sdk/appsoffactions.js`):
  - **Mail**: lord-maild stops syncing the mail accounts (`"mailPaused": true` in the mail config;
    `accounts_apply_config` stops their workers without dropping what they keep on disk), and there are
    no new-mail notices. With **Calendar, Contacts and Reminders** all off, the calendar-and-contacts
    accounts pause too (`"davPaused"`).
  - **Calendar**, **Reminders**: their alarms don't ring. **Clock**: its alarms and timer stop, and its
    item by the clock goes.
  - **Backups**: no scheduled backups (the schedule becomes "manual" and comes back on Activate; a backup
    running now is cancelled; refused while it restores or deletes one).
  - **Cleaner**: its item by the clock goes.
  - **Machines**: refused while a machine is running, paused, starting or stopping, with a message; a
    running VM is never stopped by this.
  - The others only work in their window, which is closed first (one with unsaved changes stops it).

- **Artificial Intelligence** lists LordOS's catalog of AI apps (`UPDATES.md`, "Mirrored apps
  (catalog)"), each with its kind, vendor and licence, dimmed on an architecture it doesn't support. An
  app from its vendor's repository first says whose repository it adds and its key fingerprint, then
  adds it (`pkg-source-add`) and fetches the lists; an app LordOS builds that the lists don't have yet
  offers to fetch them again.
- **Commercial** lists free-of-charge apps whose source code isn't public (Google Chrome, Microsoft
  Edge, Spotify), from their makers' signed repositories with the key LordOS pins. The page says before
  anything is installed that LordOS can't review or fix what they do. Tested on the VMware machine:
  the three sources added, the three apps installed, `apt update` and the engine's update clean
  afterwards. **Steam** (0.8.8.9; amd64) comes from Valve's signed repository
  (`https://repo.steampowered.com/steam`, suite `stable`, component `steam`) with Valve's 2025 key pinned
  (`DB23 C56B F0F3 77FD 0297 0A2A 8470 6F31 879D 59CB`, `apt/keys/steam.asc`). Its client is 32-bit, so its page first
  says that it needs 32-bit system libraries and that LordOS will enable them; **Enable and continue** runs
  `pkg-foreign-arch i386`, `pkg-source-add steam` and `pkg-update`, and the usual plan then installs `steam-launcher`,
  `steam-libs-amd64` and `steam-libs-i386:i386` (about 70 i386 libraries from Debian 13) together, with Debian's
  `mesa-vulkan-drivers:i386`, `libasound2-plugins:i386` and `mesa-vulkan-drivers` (see "Steam's 32-bit Vulkan
  drivers" under "Multiarch"); removing Steam removes them all but the amd64 `mesa-vulkan-drivers` (the catalog's
  `"keep"`: the system's own Vulkan driver). Steam's entry also says `"categories": ["Game"]`, so Store → Games lists it (and counts it) next
  to Debian's games. Tried on one LordOS system so far, the maintainer's PC (Intel Core i9-10900, AMD Radeon RX 580, Vulkan through RADV, Mesa 25.0.7), with 0.8.8.9 test builds
  (see "Multiarch").
- **Third-party apps** (until 0.8.8.3 "Third-party suites") lists open source apps and suites from
  other projects: ArtCraft (0.8.8.1, its maker's `.deb` with a pinned SHA-256), and since 0.8.8.4
  VSCodium (#38) and Brave Browser (#39), both from their makers' signed repositories (amd64 and arm64)
  with the key LordOS pins, added the same way as the AI apps' (`UPDATES.md`, "Mirrored apps (catalog)").
  Tested on VM 116 (amd64): both sources added, verified and installed by the engine.
- **Remote access** (0.8.8.6) lists third-party remote access apps: Tailscale (#68, from Tailscale's
  signed repository with the key LordOS pins), amd64 and arm64. It installs a system service that
  starts with the computer and isn't held to the per-app firewall; the page says so. What it does
  on LordOS, and what it can't do: `UPDATES.md`, "Remote access: Tailscale". Not installed on a
  LordOS system yet. RustDesk (#66) was offered here from 0.8.8.6 and is removed from the catalog in
  0.8.8.9: LordOS doesn't redistribute it; people who want it can get it from its maker.
- **Disk Formats** (0.8.7, `lordos-disk-formats`, `tools/iso/mkdeb-diskformats.sh`) is one item in the Commercial group,
  from the LordOS repository: it depends on `ntfs-3g`, `exfatprogs` and `dosfstools`, and on amd64 suggests `apfs-dkms`,
  offered as a switch on its page. Those drivers are free software ([FILES.md](FILES.md), "Disks' file systems").
- **The featured banner** comes from the LordOS repository (`apt/featured/featured.json` and
  `banner.png`, published under `main/featured/` and listed in the signed InRelease). The engine takes
  them only from a source whose signed Release says `Origin: LordOS`, checks their hashes, that the
  JSON is an object of at most 64 KB and that the PNG has a PNG signature and at most 2 MB, and keeps
  them in `/var/lib/lordos/pkg/` (`lord.pkg.featured()`, read-only). Until the picture is there the
  Store draws a skeleton of the banner (it pulses, unless Reduce motion is on); a repository without a banner shows
  a catalog app instead.

**Search (0.8.8.4, #37).** The query is split into words; case and accents don't matter (`camara`
finds «Cámara»), the words can be in any order, and they are also tried joined, so `vs code` finds the
`vscode` packages and `vscode` finds an app called «VS Code». A result with every word comes first,
ranked by where the words are: the whole name, the start of the name, the start of a word, inside a
word, then the summaries (and a bonus when the words appear together in that order). With two words or
more, packages with only some of them follow, after all of those, when one of the words starts a word
of their name (`vs code` also lists `code-aster`); a word only in a summary isn't enough.
`lord.pkg.search` does this in C (`server/src/pkg/match.c`, tested in `build/pkg-test`), and the
Store's own lists in JS with the same rules (`apps/store/search.js`, tested in
`server/tests/store_search_test.mjs`). While searching, Discover first shows **From the LordOS
catalog**: the catalog apps that match (`apt/catalog.json`, every section), which aren't in the package
lists until their maker's repository is added; they match by name, id, package name, the catalog's
`keywords` (`"vscode"`, `"vs code"`, nicknames) and, with less weight, the summary in the user's
language and in English, the vendor and the kind. The same search filters the catalog pages,
categories, Installed and LordOS apps. Spotlight asks for `every: true`: only the packages with every
word. On VM 116 (Debian 13, 75,016 packages) a search takes 24–33 ms, the same as before (24–31 ms with
the old substring search, measured with the same queries).

![search](store-search-vs-code.png)

*"vs code": Visual Studio Code first, then VSCodium and Cursor (VS Code in their keywords or summary), then
the catalog apps with only "code", then the packages.*

**What `lordos` depends on since 0.8.7.** `lordos-hwdata`, the signed tested-hardware snapshot
([UPDATES.md](UPDATES.md), "The tested-hardware snapshot"); `libfreerdp3-3`, `libfreerdp-client3-3` and `libwinpr3-3` for
Remote Desk; `rclone`, `fuse3` and `openssh-client` for Connect to Server; `openssl`. An update brings them in.
Since 0.8.8.5 `lordos-rclone` (rclone's official release binary v1.75.2, unmodified, at `/usr/lib/lordos/rclone/rclone`; `tools/iso/mkdeb-rclone.sh`
checks the release's signed `SHA256SUMS` against rclone's pinned release key and the zip's hash, one package per architecture,
published in LordOS's own apt repository with each release since rclone has none) replaces Debian's `rclone` as the dependency: Cloud needs a newer rclone than Debian 13's 1.60, and
Connect to Server runs the same binary (Debian's only if ours is missing). Debian's `rclone` stays installed on an upgrade; nothing runs it.

## Suggested for your computer

Store → **For this computer** (until 0.8.5 the top of Discover) shows **Suggested for your computer**: Debian packages for the hardware
LordOS finds, each card saying why it appears, what it installs and, for software that isn't free,
that it isn't (with an orange label; such cards never have a highlighted button, and the plan
repeats the notice). **Install** shows the usual plan and runs `pkg-install` through the admin
helper; **Don't suggest again** stores the card's id in `settings.json` (`suggestions.dismissed`).
When an installable suggestion is new, the Store's Discover item shows a count until Discover is
opened, and the session shows one notification (`sdk/suggestions.js`: 3 minutes after login and
every 6 hours; never again for the same suggestion, `suggestions.notified`).

| | |
|---|---|
| ![light](store-suggested-light.png) | ![dark](store-suggested-dark.png) |
| Test data (fixture, labeled "Test data"), VM 119: the Wi-Fi firmware card expanded, with its plan | Dark: the NVIDIA card with what changes and its plan (38 packages, with `contrib` and `non-free` enabled) |
| ![notification](store-suggested-notify.png) | |
| The one-time notification, 3 minutes after login | |

### Rules

The rules are data, `assets/store/suggestions.json`, evaluated by `sdk/suggestengine.js` (pure
functions; `server/tests/suggest_engine_test.mjs`). Every package is looked up in the package
index first, and an installable card also needs a plan that can be made (`lord.pkg.plan`): a
package that isn't in the configured sources, or that depends on one that isn't (the NVIDIA driver
needs `contrib` as well as `non-free`), is never offered as installable.

| Detected | How | Suggests | Notes |
|---|---|---|---|
| VMware virtual machine | DMI `sys_vendor` "VMware, Inc." or a PCI device of vendor `15ad` | `open-vm-tools` | Preinstalled since 0.8.1, so it normally shows as installed. `open-vm-tools-desktop` is not suggested: LordOS doesn't start its `vmware-user` agent (no XDG autostart), and only part of its clipboard works through Xwayland (`WAYLAND.md`, "VMware Tools"); resize was not tested |
| QEMU/KVM guest with a guest agent channel | a virtio port named `org.qemu.guest_agent.0` (only present when the host enabled the agent) | `qemu-guest-agent` | Clean shutdown from the host, frozen disks for backups, the guest's address. Preinstalled in the live ISO from the next image build (0.8.9.5 had no new images; then a LordOS machine in Machines has its agent from the first boot, `ISO.md`), so it normally shows as installed there |
| VirtualBox virtual machine | DMI product "VirtualBox" or PCI `80ee:cafe` | `virtualbox-guest-utils` | **Not in Debian 13** (main, contrib or backports): the card explains that, and that the kernel already has the guest drivers (display with VMSVGA, shared folders) |
| NVIDIA graphics card (x86_64) | PCI vendor `10de`, class `0300`/`0302`/`0380` | `linux-headers-amd64`, `nvidia-driver`, `firmware-misc-nonfree` | **non-free**. The card says the free `nouveau` driver is in use and stays the default; what changes: DKMS builds the module for each kernel, Secure Boot needs its key enrolled at the next restart, cards older than about 2014 need the free driver, and the LordOS desktop hasn't been tested with this driver yet. `nvidia-driver` also depends on packages in `contrib` (`nvidia-installer-cleanup`): without both components in the sources the card explains that instead (the LordOS ISO enables both) |
| USB printer | an interface of class `07`, by USB vendor ID: HP `03f0` → `hplip`, Epson `04b8` → `printer-driver-escpr`, Brother `04f9` → `printer-driver-brlaser`, Samsung `04e8` → `printer-driver-splix`, Canon `04a9` → `printer-driver-gutenprint` | the vendor's Debian driver | Vendors not in the table get nothing: most printers work driverless with CUPS |
| USB printer with IPP over USB | interface `07/01/04` | `ipp-usb` | Driverless printing and scanning: the device shows up like a network printer |
| USB scanner | udev's `libsane_matched=yes` for the device (`/run/udev/data/c189:N`; needs libsane's udev rules installed), by vendor: HP → `libsane-hpaio` | the vendor's SANE backend | Other vendors' backends are already in `libsane1` |
| Raspberry Pi 5 (and 500, CM5) | device tree `compatible` | `rpi-eeprom`, `raspi-utils`, `gpiod` (those that exist) | `rpi-eeprom` and `raspi-utils` come from the Raspberry Pi archive the LordOS image configures, and since 0.8.4.1 also from the LordOS repository (`UPDATES.md`, "Raspberry Pi firmware packages"); `gpiod` from Debian |
| A device that asked for missing firmware | the firmware loader's "Direct firmware load for X failed" / "firmware: failed to load X" lines in the kernel log, minus files present now | the package that ships X (`iwlwifi-*`, `intel/ibt-*` → `firmware-iwlwifi`; `rtl_bt/`, `rtw88/`, `rtw89/`, `rtlwifi/`, `rtl_nic/` → `firmware-realtek`; `ath*k/`, `ar3k/`, `qca/`, `htc_*` → `firmware-atheros`; `brcm/brcmfmac*` → `firmware-brcm80211`; `brcm/BCM*` → `bluez-firmware`; `mediatek/`, `mt76*` → `firmware-mediatek`; `mrvl/`, `libertas/` → `firmware-libertas`; `amdgpu/`, `radeon/` → `firmware-amd-graphics`; `i915/`, `xe/` → `firmware-intel-graphics`; `nvidia/` → `firmware-nvidia-graphics`) | **non-free-firmware**, one card per package (Wi-Fi, Bluetooth, network adapter, graphics). The prefixes were checked against trixie's `non-free-firmware` Contents. If the package is installed and a file is still missing, nothing is offered. Files no prefix matches (e.g. `regulatory.db`, which a VM without Wi-Fi also asks for) get no card |

Cards are ordered: installable, then explained (not available), then installed.

### Detection

`lord.hwinfo.scan()` (`server/src/hw/hwscan.c`, exposed in `ui/js_hw.c`) runs in `lord-server`,
**without root**, and only reads world-readable kernel files: `/sys/class/dmi/id` (vendor, product,
board, chassis; never the serial numbers, which are root-only), `/proc/device-tree/model` and
`compatible`, `/sys/bus/pci/devices/*` (vendor, device, class, bound driver),
`/sys/bus/usb/devices/*` (IDs and interface classes, root hubs skipped), `/sys/class/virtio-ports/*/name`,
udev's database entry of each USB device (only the `libsane_matched` flag) and `/dev/kmsg`. Values
are reduced to hex IDs and printable ASCII before they reach JS. Nothing runs, nothing is written,
nothing goes over the network.

The kernel log is readable by the session only with `kernel.dmesg_restrict=0` (as on the LordOS
Raspberry Pi kernel). With Debian's default (`1`) the Store asks the admin helper's fixed action
**`hw-firmware-missing`**: no arguments, any user, read-only; it reads `/dev/kmsg` as root and returns
only `LOG fw <file> <driver>` lines whose file name passes `hwscan_fw_name_valid()` (`[A-Za-z0-9._+,/-]`,
no `..`, no leading `/`) and is still missing, never other log text.

`LORD_HWSCAN_ROOT=/path` (development and screenshots only; an absolute path without `..`) makes
`lord-server` read a fake tree instead, with the kernel log in `<root>/kmsg`; the Store then labels the
section "Test data". `build/hwscan-test fixture <dir> demo` writes such a tree (also `vmware`,
`virtualbox`, `nvidia`, `printer`, `scanner`, `rpi5`, `qemu`, `firmware`); `build/hwscan-test dump`
prints the facts of the machine it runs on.

### Tests

- `build/hwscan-test` (`make test`): the facts over fake sysfs/DMI/PCI/USB/udev/kmsg trees for every
  kind, the kernel-log parser (kmsg and dmesg formats, Debian's "failed to load" line, duplicates,
  unsafe names such as `../../etc/shadow`), files already present (`.zst`, `updates/`), and this
  machine's facts as valid JSON.
- `node tests/suggest_engine_test.mjs` (`run_suites.sh`): every rule against fixtures and a fake index
  (installed, installable, not in the sources, dismissed, wrong architecture, other devices of the
  same vendor), the firmware mapping, ordering, the root helper's lines, notify-once, and that each
  card's texts exist in en, es and pt.
- **VM 119 (QEMU, real path):** `hw-firmware-missing` as root returned `fw regulatory.db platform`
  (no card: unmapped), as a user the kernel refuses (`hw.noKernelLog`), and an argument is refused.
  The Store showed one card, the guest agent, as installed (`qemu-guest-agent` was already there).
- **VM 119 (fixture `demo`):** NVIDIA, HP printer with IPP over USB, a Broadcom Wi-Fi and a Realtek
  Bluetooth firmware miss and the guest agent. With `non-free` and `non-free-firmware` temporarily
  enabled, **Review and install** on the Wi-Fi card planned `firmware-brcm80211` (5.1 MB) and
  installed it through `pkg-install`; the card went away and the others stayed. **Don't suggest
  again** on the printer card was stored and the card hidden. Sources, index and package were
  restored afterwards.
- **Raspberry Pi 5 (read-only):** device tree `raspberrypi,5-model-b`, no DMI, two PCI functions
  (the BCM2712 bridge and RP1), no USB devices, no firmware misses in the kernel log
  (`dmesg_restrict=0` there, so no root action is needed); `rpi-eeprom`, `raspi-utils` and `gpiod`
  are installed and have candidates in its sources. Fed those facts and that package state, the
  engine gives one card, `rpi5Tools`, as installed (the Store itself wasn't run on the board).

**Needs real hardware:** an NVIDIA card (the driver install, DKMS, Secure Boot enrollment and the
LordOS desktop on it), a USB printer and scanner of each vendor in the table, Wi-Fi and Bluetooth
adapters without firmware (that the miss is logged as expected and the device works after the
install), and VMware and VirtualBox guests.

## `lord.pkg` (lord-server, without root)

The queries read the index with mmap and `/var/lib/dpkg/status`, and reload by themselves when
these change. `lang` (`'es'`, `'pt'` or omitted) picks the AppStream name and description.

```js
lord.pkg.available()            // true if there is already an index (only its header is read until a query opens it)
lord.pkg.lastUpdate()           // seconds since 1970 (0: never)
lord.pkg.sources()              // [{ uri, suite, component, codename, label, date, validUntil, priority, packages }]
lord.pkg.search(q, { limit = 50, apps = false, every = false, section, lang })   // [Summary], sorted by relevance
lord.pkg.info(name, { lang })   // Summary + { description, homepage, arch, source, depends, preDepends,
                                //   recommends, suggests, provides, conflicts, breaks, priority, suite,
                                //   origin, appDescription, versions: [{ version, suite, size }] } | null
lord.pkg.installed({ lang })    // [Summary] of everything installed
lord.pkg.upgrades()             // [{ name, from, to, summary, suite, size }]
lord.pkg.upgradeCountAsync()    // (0.8.9.6.2) req: upgrades().length counted on an fsasync.c worker with its own index
                                //   and dpkg status, answered as __lordFsReply(req, { count }) (sdk/appcontrol.js storeUpdates)
lord.pkg.plan({ install: [], remove: [], recommends, allowRemove, cascade, upgrade, keepOrphans })
    // { ok, error, items: [{ action: 'install'|'upgrade'|'remove', name, version, oldVersion, size,
    //   requested, reason }], held: [], download, installedDelta }   (bytes)
    // with remove, it includes the orphans: { action: 'remove', reason: 'auto', requested: false }
lord.pkg.orphans()              // [{ name, version, summary, installedSize }]: what pkg-autoremove would remove
lord.pkg.apps({ category, type = 'desktop-application', limit = 100, offset = 0, lang })
    // [App + { package, installed, version, size }], sorted by name
lord.pkg.categories()           // { AudioVideo: 210, Development: 180, … } (desktop apps)
lord.pkg.icon(name)             // path of the 64×64 PNG or null
lord.pkg.releaseNotes(name, version)   // Markdown release notes a source published for that version, or null
lord.pkg.featured()             // (0.8.5) the Store's banner from the signed LordOS repository, or null
lord.pkg.rebootRequired()       // null, or the packages that asked for a restart (/run/reboot-required.pkgs)
lord.pkg.multiarch()            // (0.8.8.9) { native, supported (amd64), i386 (enabled in dpkg's list) }; read-only
lord.pkg.ownerOfApp(desktopId)  // (0.8.8.8) { name, essential } | null: the installed package whose files include
                                //   that launcher app's .desktop file. Only the id comes from JS: lord-server finds the
                                //   file the way the launcher does (apps.c apps_system_file, /usr/share/applications
                                //   only, never ~/.local) and reads dpkg's /var/lib/dpkg/info/*.list for an exact path
                                //   match (status.c dpkg_owner: only lines in /usr/share/applications or
                                //   /usr/local/share/applications are kept, cached until the info folder changes,
                                //   bounded). essential: Essential, Protected or Priority: required.

// Summary = { name, version (candidate), summary, section, size, installedSize (bytes),
//             installed (version or null), upgradable, essential, auto, app: App | null }
// App     = { id, type, name, summary, icon (path or null), categories: [] }
```

To make changes:

```js
await admin.run('pkg-update')
const plan = lord.pkg.plan({ install: ['featherpad'] })      // show it and ask for confirmation
await admin.run('pkg-install', 'featherpad', (line) => …)    // line: "progress download 123 456"…
```

## How it resolves

- **Candidate.** The same version apt would choose (`server/src/pkg/pin.c`, `pkg_candidate` in
  `resolve.c`; since 0.8.5). Each version gets a priority:
  - from **apt's preferences** (`/etc/apt/preferences`, then `/etc/apt/preferences.d/*` in C sort
    order, only names of `[A-Za-z0-9_.-]` with no extension or `.pref`): the first *specific* pin
    naming the package (globs, `/regex/` or `src:name`) decides for the versions its `Pin:` matches
    (`origin <host>`, `release a=,n=,c=,o=,l=`, `version <glob>`); otherwise the first *general*
    pin (`Package: *`) matching its source. A malformed stanza is skipped; a `release` key we can't
    check (`v=`, `b=`) never matches;
  - otherwise from its source: 500 for normal ones, 100 for `NotAutomatic` + `ButAutomaticUpgrades`
    (backports), 1 for `NotAutomatic` only (experimental).

  Then, as apt does, the versions are taken from the newest down and only a strictly higher priority
  replaces the one chosen; the installed version counts at least 100; an older version than the
  installed one only with 1000 or more; a negative priority never. So what was installed from
  backports keeps being upgraded from there, and on the Raspberry Pi image Raspberry Pi's `+rpt`
  builds of Debian packages are not taken. The preferences are re-read when they change, without a
  new index (`idx_refresh`). Tested in `pkg-test` with the Pi image's own pin files.
- **Sources.** One-line `.list` files and deb822 `.sources`; since 0.8.5 `Architectures:` may be
  separated by commas as well as spaces (`amd64,arm64`, as Cursor's package writes it), as apt accepts.
- **Dependencies.** It walks those of what changes: `Pre-Depends` and `Depends`, and `Recommends`
  with `--recommends` (on an upgrade, the Recommends the new version adds, without it).
- **Choices.** Alternatives (`a | b`) and virtual packages are tried in order. Among providers of
  a virtual package, the highest `Priority` is preferred, then the name. If a path fails, it
  backtracks, with a maximum of 4000 attempts.
- **Conflicts.** It checks `Conflicts` and `Breaks` in both directions. If it clashes with
  something installed:
  1. it upgrades that package, if its new version no longer clashes;
  2. otherwise it removes it, with confirmation.
- **Orphans.** Marking starts from the roots: what was installed manually, what is essential and
  what is being installed now. `Pre-Depends`, `Depends` and `Recommends` are followed, because apt
  also counts recommended packages, and so are all present alternatives or providers. Whatever in
  `auto` is left unmarked is removed. Only dependencies LordOS installed are touched: what apt
  installed as automatic is not included.
- **Neighbors.** When something is upgraded or removed, it rechecks what depended on it.
  Installed packages left without what they need are first upgraded, and if that isn't possible
  they are removed (with confirmation). It repeats until nothing else changes.
- **Order for dpkg.** It sorts topologically by dependencies and uses one `dpkg --install` per
  batch. A `Pre-Depends` on something in the same batch splits it, because that has to be
  configured first.
- **Breaks inside one run (0.8.8.9).** dpkg unpacks a run's archives in the order given and checks each
  `Breaks` and `Conflicts` when it unpacks, against what is installed at that moment; `Depends` wait for the
  configure step, after every unpack. So inside a batch, a package whose installed version another one in the
  batch breaks is unpacked first, even when it depends on that one. `lordos` has
  `Breaks: lordos-web (<< its version)` and `lordos-web` has `Depends: lordos (= its version)`
  (`tools/iso/mkdeb.sh`, `mkdeb-web.sh`): dependency order alone put `lordos` first, and dpkg refused it
  ("installing lordos would break lordos-web, and deconfiguration is not permitted"), leaving `lordos-web`
  unpacked and unconfigured. Updating `lordos` alone brings `lordos-web` (the conflict rule above), and Update all
  does the same. With no newer `lordos-web` published, updating `lordos` asks to remove `lordos-web` and Update all
  holds `lordos` back: never a half-configured `lordos-web`.
  The batches also pass `--auto-deconfigure`, as apt does: a cycle, or a package broken by one in an earlier
  batch, is deconfigured and configured again with its new version. A local `.deb` installs without it, so
  `lordos` from a file over an older `lordos-web` is refused with nothing changed.
  By hand: `sudo dpkg -i lordos-web_V.deb lordos_V.deb` (lordos-web first) or
  `sudo dpkg --auto-deconfigure -i lordos_V.deb lordos-web_V.deb`; `dpkg -i lordos_V.deb` alone over an older
  `lordos-web` is refused before anything changes. Tested in `pkg-test` and `tests/pkg_breaks_dpkg_test.sh`
  (two dummy packages with the build scripts' relationships, against the real dpkg in a throwaway
  `dpkg --root`).

## Tests

```sh
make -C server build/pkg-test
build/pkg-test                         # unit tests: versions, deb822, SHA, RSA, Ed25519, xz/gz,
                                       # index and resolver over a toy repository, dpkg_owner over a
                                       # fake /var/lib/dpkg/info (exact matches, architectures, cache)
build/pkg-test pgp /usr/share/keyrings/debian-archive-keyring.gpg InRelease
build/pkg-test plan /var/lib/lordos/pkg /var/lib/dpkg/status gimp     # the plan, without installing
sudo build/pkg-test run pkg-update     # the real actions, without lord-server
sudo build/pkg-test run pkg-install "cowsay sl"
```

## Real tests (VM, Debian 13.7, 2026-10-16)

- **Unit tests:** `pkg-test` passes 78/78, including the orphan tests.
- **Real signature:** the real trixie `InRelease` was verified with `debian-archive-keyring.gpg`.
  It has three signatures: two RSA-4096 and one Ed25519, the Stable Release Key 13, and all three
  are valid. With one byte changed it is rejected.
- **`pkg-update` against deb.debian.org**, with 4 sources: trixie, trixie-updates,
  trixie-backports and trixie-security.

  | Situation | Downloaded | Time | Peak memory |
  |---|---|---|---|
  | First time | 11 indexes, 29 MB | 4.9 s | 122 MB, in the child process that lives as long as the action |
  | No changes in the lists (304 responses) | nothing | 0.12 s | 3 MB |

  Result: 74,362 packages and 2,100 icons. On disk, `/var/lib/lordos/pkg` takes 98 MB:
  60 MB of index and 29 MB of compressed lists.
- **Plans against `apt-get -s install --no-install-recommends`:**

  | Package | Packages in the plan |
  |---|---|
  | cowsay | 1 |
  | sl | 2 |
  | postfix | 3 |
  | gnome-calculator | 13 |
  | thunderbird | 30 |
  | inkscape | 48 |
  | libreoffice-writer | 79 |
  | vlc | 110 |
  | gimp | 121 |
  | blender | 197 |
  | krita | 237 |
  | kdenlive | 437 |

  In all 12 cases the names and versions match apt's.
  - With recommended packages, gimp gives 188 packages, same as apt.
  - `pkg-upgrade` gives 1 upgrade (libexpat1, from trixie-security), same as
    `apt-get -s upgrade`.
  - Removing `libqt6core6t64` gives the same 18 removals as `apt-get -s remove`.
- **Real installations:**
  - `cowsay sl`: 3 packages in 1.7 s; `cowsay` works.
  - `gnome-calculator`: 13 packages, 3.9 MB, in 4.9 s.
  - `hello` from a `.deb` downloaded to the user's folder: works.
  - Removals: `sl cowsay`, `gnome-calculator` and the 13 dependencies recorded in `auto`.
  - Rejections:
    - removing `dpkg`, because it is essential;
    - removing `libgtk-4-1` without `--cascade`, which lists what would have to be removed;
    - a name `../etc`;
    - a `.deb` that is a link to `/etc/shadow`;
    - a path through a link to `/root`;
    - a file outside the home folder.

  - Orphans: `pkg-autoremove` removed the 8 left over from a Store test
    (libadwaita-1-0, libqqwing2v5…).
  - Installing `gnome-sudoku` (9 packages) and then `pkg-remove gnome-sudoku`, without
    `--cascade`, removes all 9. With `keepOrphans` the plan has 1.

  After each test, `apt-get check` and `dpkg --audit` came back clean. The VM was left as it was.
- **`lord.pkg`**, with `build/pkg-js-test`:

  | Query | Time |
  |---|---|
  | `search` over 74 thousand packages | 27–42 ms |
  | `info` | 0.1 ms |
  | `apps` by category | 0.6 ms |
  | `plan` for gimp | 7 ms |
  | `installed` (648 packages) | 3 ms |

  The process's peak memory is 37 MB, including the pages of the mapped index touched while
  searching; the kernel can reclaim them.

## Pending

- `Suggests` don't count for orphans (apt does by default); apt's `extended_states` is not
  touched.
- **Other foreign architectures.** Since 0.8.8.9 the engine does i386 next to amd64 (below); no other pair
  (armhf on arm64, say) is fetched or offered, though such packages already installed are read and kept.
- **Versions or suites on request.** `package=version` or `package/backports` can't be requested
  yet.
- **Flat repositories.** Those whose suite ends in `/` are not accepted.
- **`control.tar.zst`** in local `.deb` files (Ubuntu); Debian's use xz.
- **Incremental index diffs** (`Packages.diff`). Today the whole index is downloaded when it
  changes.

## Multiarch (0.8.8.9)

Steam's client is 32-bit, so the engine installs i386 packages next to the amd64 ones, as apt does after
`dpkg --add-architecture i386`. Only that pair: i386 is the one foreign architecture, and only on amd64.

- **Enabling it.** The admin action `pkg-foreign-arch i386` (administrators only) takes exactly `i386`, refuses any
  other value and any native architecture but amd64 (`pkg.foreignArchUnsupported`), and runs only the fixed
  `/usr/bin/dpkg --add-architecture i386`, holding the engine's lock and dpkg's frontend lock; with i386 already in
  dpkg's list (`/var/lib/dpkg/arch`) it does nothing (`pkg.foreignArchPresent`). There is no action to remove it.
  `lord.pkg.multiarch()` says whether it is on.
- **Which lists.** With i386 enabled, `pkg-update` also fetches `<component>/binary-i386/Packages`, verified against
  the signed `InRelease` like every list, but only from a source that names i386 in its architectures (`arch=` or
  `Architectures:`, as Valve's does) or, naming none, is Debian's own archive (`Origin: Debian`) whose signed Release
  lists i386: so Debian's trixie, updates and security serve the ~70 libraries, and a vendor repository the catalog
  adds (it always names its architectures) never brings i386 unasked. The i386 list's `Architecture: all` records are
  left out (they come with the native list).
- **The index and dpkg's status.** An i386 package is its own entry, named `name:i386` (`libc6:i386`), in the index
  (`idxb_packages_foreign`) and in what is installed (`StPkg.key`, from the status `Architecture`), so the two
  architectures never mix; Provides keep their plain names. Searches leave the i386 libraries out; Installed and
  Updates list them, labelled "(32-bit)" in the Store. apt's pins name them as `libc6` (or `libc6:i386`).
- **The resolver** (`resolve.c`) reads every relation in its owner's architecture, as dpkg does: an unqualified
  Depends is satisfied by the same architecture, or by any architecture's package that is `Multi-Arch: foreign`
  (so `steam-libs-i386:i386` uses the installed amd64 D-Bus, and amd64 `steam-launcher` can recommend the i386-only
  `steam-libs-i386`); `name:any` also by a `Multi-Arch: allowed` one; `name:native` and `name:i386` name one
  architecture; a foreign owner takes a native `Multi-Arch: foreign` package first. Unqualified Conflicts and Breaks
  apply to every architecture, except the owner's own package in the other one. A request may name `name:i386`.
- **`Multi-Arch: same` pairs.** The two architectures of one package can be installed together only when both are
  `Multi-Arch: same` (else `pkg.multiarchConflict`), and then always at the same version: installing or upgrading one
  moves the other to the same version in the same plan (reason "Same version as …"); a new i386 package takes the
  installed amd64 version when its candidate is older; the other one is never moved down; and a plan that would leave
  a changed pair at two versions is refused (`pkg.multiarchSplit`). Update All upgrades `libc6` and `libc6:i386`
  together, or holds both when the i386 version isn't in the lists (i386 packages installed but no i386 lists): never
  one without the other. `plan_order` puts the two side by side, never across a Pre-Depends cut, so one
  `dpkg --install` unpacks both.
- **dpkg.** Installs pass the `.deb` files; removals pass `name:i386`, and `name:amd64` for a native package whose
  i386 one is installed too (dpkg refuses an ambiguous name).
- **Steam's 32-bit Vulkan drivers** (found on the maintainer's PC, 2026-11-15, with a 0.8.8.9 test build): Valve's
  `steam-libs-i386` only *recommends* `mesa-vulkan-drivers` and `libasound2-plugins`, and an install takes no
  Recommends (see "Confirmations"), so the Store installed `libvulkan1:i386` and `libgl1-mesa-dri:i386` (Depends) but
  no 32-bit Vulkan driver: 32-bit games and DXVK fell back to Lavapipe (software), as the Monitor showed for Steam's
  processes, while 64-bit Proton games (with the amd64 driver) were fine. The resolver itself was right: with
  `--recommends`, an i386 package's unqualified Recommends resolve in i386 (`mesa-vulkan-drivers:i386`, never the amd64
  one). The fix is in the catalog: Steam's `"with"` names `mesa-vulkan-drivers:i386` and `libasound2-plugins:i386`
  (Debian's own i386 list, `Multi-Arch: same`) and `mesa-vulkan-drivers` (amd64; the ISO installs it, a Debian system
  set up with `tools/install/install.sh` may not have it), and `"keep": ["mesa-vulkan-drivers"]` leaves the amd64
  driver in place when Steam is removed. The `Multi-Arch: same` rules keep each pair at one version: a new
  `mesa-vulkan-drivers:i386` moves the installed amd64 driver up to its version in the same plan, never down.
  **Steam installed before** (0.8.8.9 test builds): Update All doesn't bring them (the installed `steam-libs-i386`
  already recommended them, so they aren't "new" Recommends), but Steam's Store page, which comes with 0.8.8.9's
  catalog, shows **Update** while any of its `"with"` packages is missing; the plan installs
  only the missing ones. `apt/build.sh check` accepts a `"with"` package that the vendor's index for its
  architecture lacks when Debian trixie's `main` index has it (InRelease verified with Debian's archive keyring, the
  index with its SHA-256; an i386 one must be built for i386 and `Multi-Arch: same`, `foreign` or `allowed`).
- **Tests** (`build/pkg-test`): request names; `:any`/`:native`/`:i386` qualifiers; dpkg's architecture list; an index
  from an amd64 and an i386 list (the i386 `all` records left out, Provides of both architectures); status keys;
  Steam's libraries resolved with their i386 dependencies and the native D-Bus; a native Recommends of an i386-only
  foreign package; `python3:any` by `python3:i386`; `python3` (allowed) refused twice; Update All moving the libc6
  pair together and holding a pair whose i386 version is missing; libc6 alone bringing libc6:i386; a split refused;
  removing `libc6:i386` without touching libc6; unqualified Conflicts across architectures; a `Multi-Arch: no`
  package refused in the other architecture; a new `libc6:i386` at the installed amd64 version; no i386 lists at all;
  the dpkg order of a pair; the vendor source handover over a fixture tree; and Steam's 32-bit Vulkan drivers (a
  Debian and a Valve source, each with amd64 and i386 lists): the old three-package install leaves the i386 driver
  out, `--recommends` resolves `steam-libs-i386`'s Recommends in i386, the catalog's `"with"` brings
  `mesa-vulkan-drivers:i386`, `libasound2-plugins:i386` and their i386 dependencies with the `mesa-vulkan-drivers` and
  `libvulkan1` pairs at one version and `libc6` untouched, Update All leaves an old Steam without them, and asking for
  the missing ones (the Store's Update) adds them with the amd64 driver moved up to the same version. Valve's real `InRelease` verifies with
  `apt/keys/steam.asc`. **Tried on one LordOS system**, the maintainer's PC (Intel Core i9-10900, AMD Radeon RX 580, Vulkan through RADV, Mesa 25.0.7), with 0.8.8.9 test builds (2026-11-15): i386
  enabled from the Store, Steam installed from the Store, and Lost Ark, a Windows game, running through Proton. No
  other machine has run it, and Update All moving the pair and removing Steam have not been reported on it.

## HTTPS (0.8.5)

The engine downloads `https://` sources over TLS and `http://` ones as before. The download child (nobody,
in `lordos-net-store.scope`) resolves and connects the TCP socket itself; for `https://` it starts
`lord-tls` (`server/src/tls/tlsproxy.c`, next to `lord-server`, root's) with the socket on fd 3 and a
socketpair on fd 4. lord-tls loads the system's CA bundle, takes the helpers' sandbox (no sockets, no exec,
no files), does a TLS 1.2+ handshake with SNI, checks the chain and the host name, answers `1` (or `0`,
which the engine reports as `pkg.http.tlsFailed`) and then relays plain HTTP, so the HTTP code is the
same for both schemes. Redirects may go from `http://` to `https://`, never back (`pkg.http.downgrade`).
TLS adds privacy and refuses an impostor server; what is installed is still decided by the signed
index and each file's SHA-256, exactly as over HTTP. Sources written as `https://` are no longer
rewritten to `http://`.

A `+` in a file's path is sent as `%2B`, as apt does (0.8.5): lordos.org's CDN reads a bare `+` as a
space, so packages such as `kms++-utils` and `libkms++0` failed with 404 before.

Tested against real servers (`server/tests/pkg_https_test.c`, run as root with network, not part of
`make test`): an HTTPS-only vendor repository, lordos.org over both schemes, an HTTP→HTTPS redirect, and
self-signed, wrong-host and expired certificates refused (7/7).

