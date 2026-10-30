# Updating LordOS in place

From 0.8.1, a LordOS system moves to a new version without reinstalling: the Store shows
"LordOS <version> is available" with the release notes, installs it with the same package engine
it uses for Debian packages, applies whatever the new version changes in the system (the
migrations below) and asks for a restart. Files, settings and accounts are kept. It works the same
on a PC installed from the ISO, on the Raspberry Pi 5 image, on Debian 13 with `install.sh` and on
a server or VPS, and `apt` sees the same repository.

| Piece | Where |
|---|---|
| The signed repository | `https://lordos.org/apt`, suite `stable`, component `main`, amd64 and arm64 |
| Its source on every system | `/etc/apt/sources.list.d/lordos.sources` (deb822, `Signed-By:`) |
| Its key on every system | `/usr/share/keyrings/lordos-archive-keyring.gpg` (in the `lordos` package) |
| Building, signing and uploading it | `tools/release/apt-repo.sh` (on the maintainer's Mac) |
| The Store's side | `apps/store/debian.js` (`Updates`), `sdk/updates.js`, `lord.pkg.releaseNotes` and `lord.pkg.rebootRequired` (`server/src/ui/js_pkg.c`) |
| Version-to-version changes | `packaging/migrations/` (system, as root) and `MIGRATIONS` in `sdk/settings.js` (each user's settings) |

## The repository

```
https://lordos.org/apt/
  dists/stable/InRelease               signed (clear-signed Release)
  dists/stable/Release, Release.gpg    the same, with a detached signature
  dists/stable/main/binary-amd64/Packages(.xz, .gz)
  dists/stable/main/binary-arm64/Packages(.xz, .gz)
  dists/stable/main/notes/ReleaseNotes(.xz)   release notes of each lordos version (below)
  pool/main/l/lordos/lordos_<version>_<arch>.deb
  lordos.sources                       the same file the package installs
https://lordos.org/keys/lordos-archive-keyring.gpg (.asc)   the public key
https://lordos.org/apt-staging/        the same layout, for tests before anything goes live
```

- **One suite, `stable`,** with every published release (until 1.0 every release is a beta, and the
  betas are what users run). Origin and Label are `LordOS`. Test builds go to `apt-staging`, never to
  `apt`. A `beta` suite can be added later for opt-in previews.
- **The pool only grows.** A published `.deb` is never replaced (`apt-repo.sh` refuses a version that
  already exists with other contents); the previous versions stay, so `apt install lordos=<version>`
  can go back. Versions follow dpkg's order, so a test build named `0.8.1~staging1` sorts after
  `0.8.0` and before `0.8.1` (checked by `build/pkg-test`).
- **Built with Debian's tools.** `apt-ftparchive` writes `Packages` and `Release` (in a
  `debian:trixie` container on the Mac, or natively on a Debian host); `gpg` signs on the Mac.
- **Release notes.** `main/notes/ReleaseNotes` is a deb822 file covered by the signed `Release`:
  one stanza per lordos version with `Package`, `Version` and `Notes` (the version's section of
  [CHANGELOG.md](../CHANGELOG.md) as Markdown, through `tools/release/release-notes.sh` without its
  footer; continuation lines start with a space, ` .` is an empty line). Our package engine downloads
  it in `pkg-update`, checks its SHA-256 against `InRelease` like any index and keeps it in
  `/var/lib/lordos/pkg/notes`; `lord.pkg.releaseNotes(name, version)` returns the Markdown. `apt`
  ignores the file.

### Serving it (CDN)

`lordos.org` is S3 behind CloudFront. Two things the website's defaults do would break APT, so the
repository paths need their own CloudFront cache behaviors, `apt/*` and `apt-staging/*`:

1. **Plain HTTP must be served, not redirected.** Until 0.8.4 our package engine had no TLS of its
   own and read `https://` sources over `http://`, exactly like `http://deb.debian.org`: what protects
   the files is the signature and the SHA-256 of every file, not the transport. Since 0.8.5 it speaks
   HTTPS through `lord-tls` ([PACKAGES.md](PACKAGES.md), "HTTPS") and sources written as `https://`
   stay HTTPS, but systems updating from 0.8.4.1 or older still read the repository over `http://`
   until they have 0.8.5, and `apt` itself uses HTTPS. Viewer protocol policy: **HTTP and HTTPS**
   (`allow-all`).
2. **No URL rewriting.** The site's `lordos-router` function (viewer request) turns a path without
   an extension into `<path>/index.html`, and APT's files `InRelease`, `Release` and `Packages` have
   none. The two behaviors have **no function association**.

Everything else is copied from the default behavior: origin `s3-lordos.org`, GET/HEAD, the same
cache policy and response headers policy. `keys/*` needs nothing special (HTTPS, files with an
extension). `tools/web/cdn-apt-behaviors.sh <admin-profile>` makes this change (once, by the
account's administrator: the deploy profile cannot change the distribution). The website's deploy
script (`tools/web/deploy.sh`) never touches `apt/`, `apt-staging/` or `keys/`.
`apt-repo.sh verify <target>` checks all of this from the outside.

## The keys

| Key | Private part | Public part |
|---|---|---|
| **APT archive key** (signs `InRelease`/`Release.gpg`): OpenPGP v4, Ed25519, sign only, `LordOS Archive Signing Key`, fingerprint `1DDA F390 107D F264 FA8A  0918 3972 CC6D E4DA 2336`, expires 2031-10-18 | the maintainer's Mac only: GnuPG home `~/.config/lordos-release/apt-gnupg` (0700) and an armored export `~/.config/lordos-release/apt-archive-signing.secret.asc` (0600); backup copies of both in `~/.ssh/ssh/lordos-release/`, the same place as the release key's | `packaging/keys/lordos-archive-keyring.gpg` (and `.asc`), installed as `/usr/share/keyrings/lordos-archive-keyring.gpg`, published at `https://lordos.org/keys/` |
| **Release key** (signs `SHA256SUMS` of a GitHub release; raw Ed25519) | `~/.config/lordos-release/release-ed25519.key.pem` | see [SIGNING.md](SIGNING.md) |

APT needs OpenPGP, so the repository has its own key instead of reusing the raw Ed25519 release key.
Neither private key is ever copied to a VM, a build machine, a server or the repository. Keep an
offline copy of `~/.config/lordos-release/` (the whole folder) as well.

**Before 2031-10-18**, extend the key's expiry on the Mac (`gpg --quick-set-expire <fingerprint> 5y`
with `GNUPGHOME=~/.config/lordos-release/apt-gnupg`), export it again into `packaging/keys/` and ship
it in a release some months before the old date: a system that has not updated by then needs
`install.sh` again. To replace the key, ship the new one in the keyring file next to the old one for
at least one release before signing with it.

## How a system gets the repository

- **The `lordos` package** carries the source as `/usr/lib/lordos/apt/lordos.sources`. Its
  `postinst` copies it to `/etc/apt/sources.list.d/lordos.sources` when that file is missing, and
  its `postrm` removes it with the package (otherwise `apt update` would fail on a repository whose
  key is gone). It is not a conffile: an administrator's change, such as `Enabled: no` to stop
  LordOS updates, is kept on every upgrade.
- **New installs** from the 0.8.1 ISO, the Raspberry Pi image and `install.sh` get it with the
  package; none of them needs a step of its own.
- **LordOS 0.8.0** (one time): its package has no repository yet. Either run the installer again,
  which upgrades in place (it keeps the login setup, the users and their settings). A system
  installed from the 0.8.0 ISO has no `curl`, so install it first:

  ```sh
  sudo apt update && sudo apt install -y curl
  curl -fsSL https://lordos.org/install.sh | sh
  ```

  or add the repository by hand and upgrade with apt (or with Store → Updates afterwards):

  ```sh
  sudo curl -fsSLo /usr/share/keyrings/lordos-archive-keyring.gpg https://lordos.org/keys/lordos-archive-keyring.gpg
  gpg --show-keys /usr/share/keyrings/lordos-archive-keyring.gpg    # 1DDA F390 107D F264 FA8A  0918 3972 CC6D E4DA 2336
  sudo curl -fsSLo /etc/apt/sources.list.d/lordos.sources https://lordos.org/apt/lordos.sources
  sudo apt update && sudo apt install lordos
  ```

  That file is byte for byte the one the package would write, so the package keeps it as it is.

```
# /etc/apt/sources.list.d/lordos.sources
Types: deb
URIs: https://lordos.org/apt
Suites: stable
Components: main
Signed-By: /usr/share/keyrings/lordos-archive-keyring.gpg
```

## Store → Updates

| | |
|---|---|
| ![LordOS update available](store-update-available-light.png) | ![LordOS update available, dark](store-update-available-dark.png) |
| "LordOS <version> is available", with the release notes | The same in dark mode |
| ![Updating](store-update-progress-light.png) | ![Restart required](store-update-restart-dark.png) |
| Updating (the plan, then the progress of `pkg-install lordos`) | Installed: "Restart required" |
| ![The daily check's notification](store-update-notification-dark.png) | |
| The daily check found an update and says so once (here a Debian update; after the restart, About shows the new version) | |

(Screenshots from the staging test on VM 120: the test builds are called `0.8.1~staging5` and
`~staging6`, and their notes are 0.8.1's unreleased changelog.)

- **"Check for updates"** runs `pkg-update`: every source's signed `InRelease`, Debian's and
  LordOS's, then the indexes and the release notes.
- **"LordOS <version> is available"** appears when the candidate of the `lordos` package is newer
  than the installed one: the installed version, the download size, **What's new** (the release
  notes, the first lines with "Show all") and **Update**. Update shows the plan first
  (`lord.pkg.plan({ install: ['lordos'] })`: lordos and any new dependency), then runs
  `pkg-install lordos` through the administration helper, with the same progress bar as any
  install. Only administrators (the `sudo` group) can update.
- **Debian updates** are listed below with "Update all" (`pkg-upgrade`, which also takes lordos if
  it is pending).
- **"Restart required"** appears when an installed update needs it: the running `lord-server` is
  not the installed `lordos` version, or `/run/reboot-required` exists (the Debian convention: the
  lordos `postinst` writes it on an upgrade, and its kernel hook
  `/etc/kernel/postinst.d/zz-lordos-reboot-required` on every new kernel). "Restart Now…" opens the
  session's Restart dialog (with its countdown and the open apps; [SESSIONS.md](SESSIONS.md)) when
  the session can restart; otherwise it says to log out and choose Restart on the login screen.
  "Later" hides it. Nothing restarts on its own: the update replaces the files under the running
  session, which keeps working with what it already loaded until the restart.
- **Check for updates automatically** (on by default, per user, `settings.json` → `updates.autoCheck`):
  five minutes after login and then every hour, if the lists are more than a day old and the user is
  an administrator, the session runs `pkg-update` quietly and, when the set of available updates is
  new, shows one notification ("LordOS 0.8.2 is available", or "3 updates are available") that
  opens Updates. It never installs anything. When nothing changed it costs a few small requests
  (`If-Modified-Since`, 304). LordOS has no metered-connection setting yet; turning the check off
  is the way to avoid it on a metered link.
- **Local repositories.** The engine also reads `file:/path` sources (a copy of the repository on a
  disk or USB stick, for machines without internet), checked the same way.

The whole path is our own engine ([PACKAGES.md](PACKAGES.md)): the `InRelease` signature is checked
by `server/src/pkg/pgp.c` (Ed25519 v4, SHA-512) against the keyring named in `Signed-By`, every
file by its SHA-256, and dpkg installs. `apt` and the Store share dpkg's lock and see the same
state, so either can be used.

## Migrations

What changes between versions in the installed system (not in the files the package ships) is a
migration. Two kinds:

**System migrations** (root), `packaging/migrations/NNN-<version>-<name>.sh`, installed in
`/usr/lib/lordos/migrations/` and run by `/usr/lib/lordos/migrate` from the package's `postinst`:

- each one runs **once**, in `NNN` order; `/var/lib/lordos/migrations/<name>` records it (the time
  it ran). So 0.8.1 → 0.8.2 → 0.8.3 and 0.8.0 → 0.8.3 in one step run the same migrations and end
  in the same state;
- it gets the version it upgrades from in `LORDOS_OLD_VERSION` (empty on a new install, where
  most migrations have nothing to do) and runs under `sh -eu`;
- it must be **idempotent** and safe without systemd (a chroot building an image), must not touch
  the users' home folders, and must keep what an administrator changed;
- if one fails, the ones after it don't run, `dpkg` leaves lordos unconfigured and says so, and the
  next `dpkg --configure -a` or Store update runs it again.

Today's migrations:

| Migration | What it does |
|---|---|
| `001-0.8.1-network-permissions` | Enables `lord-netperm.service` (per-app network permissions) where `lordos.service` is enabled. The ISO, the Pi image and `install.sh` already did; a system set up with `apt` alone had it off. Runs once: disabling it later sticks. |
| `002-0.8.1-root-ssh-keys` | 0.8.1 adds `PermitRootLogin no` (`/etc/ssh/sshd_config.d/10-lordos.conf`). On an **upgrade** from 0.8.0 with SSH installed and keys in `/root/.ssh/authorized_keys` (a VPS run as root), it writes `09-lordos-root-keys.conf` with `PermitRootLogin prohibit-password`, so root keeps logging in with keys (never passwords) instead of being locked out at the next restart. New installs never get it; delete the file to apply LordOS's default. |
| `003-0.8.2-drivetemp` | 0.8.2 ships `/usr/lib/modules-load.d/lordos-drivetemp.conf` (SATA disk temperatures in Monitor → Disks, read at boot). On a running system it loads `drivetemp` once, so the temperatures show without a restart. Does nothing without systemd (an image being built) or where the kernel has no `drivetemp`. |
| `004-0.8.3-smartd-off` | 0.8.3 ships a drop-in that keeps Debian's smartd (`smartmontools.service`) off unless `/etc/lordos/smartd-enabled` exists ([SYSTEM.md](SYSTEM.md), "smartd"). A condition is only checked at start, so a smartd already running from an earlier smartmontools install is stopped once. Does nothing without systemd, without smartmontools or where an administrator opted in. |
| `005-0.8.4-browser-frames` | 0.8.4 makes the preinstalled browsers draw their own bar with LordOS's window buttons (`WAYLAND.md`, "The browsers by default"). Where `/etc/chromium/master_preferences` still has the 0.8.3 ISO's `"custom_chrome_frame": false` it becomes `true` and the GTK theme is added (new Chromium profiles only); where `/etc/firefox-esr/lordos.js` is still exactly the 0.8.3 file it becomes `inTitlebar` 1 (every Firefox profile whose user didn't change «Title Bar»). Anything an administrator changed is kept; home folders are not touched. |

**Settings migrations** (each user), in `sdk/settings.js`: `MIGRATIONS` is a list of
`[schema, (settings) => newSettings]` applied in order when the session loads
`~/.config/lordos/settings.json`; the file records the last one in `schema`. 0.8.1 starts recording
it (schema 1, nothing changes). A setting renamed or reshaped in a release gets an entry here, so a
user who skips versions still converges.

Things that only need new files (a new unit, a new default config) ship in the package as usual; a
new unit that must be enabled on upgraded systems needs a migration, since `postinst` only enables
`lord-fand`.

## Raspberry Pi firmware packages

Since 0.8.4.1 the repository also carries, **for arm64 only**, the Raspberry Pi bootloader updater
and exactly what its `Depends` need that Debian 13 doesn't have, copied unchanged from Raspberry
Pi's archive (`archive.raspberrypi.com/debian`, trixie, main):

| Package | Why |
|---|---|
| `rpi-eeprom` | the bootloader images (`/usr/lib/firmware/raspberrypi/bootloader-2712/…`) and `rpi-eeprom-update`, used by Settings → Board firmware (`SETTINGS.md`) |
| `raspi-utils`, `raspi-utils-core`, `-dt`, `-eeprom`, `-otp`, `raspinfo`, `libgpiolib0`, `libdtovl0` | `rpi-eeprom` depends on `raspi-utils` (`vcgencmd`, used to read the running bootloader), which depends on these |
| `kms++-utils`, `libkms++0` | `raspinfo` depends on them |

Their recommendations (`flashrom`, `rpieepromab`) are not mirrored: without them a Pi 5 stages the
update for the bootloader to apply at the next boot. Raspberry Pi's whole archive is never added to a
system by LordOS's packages.

- **Pinned and verified.** `tools/release/rpi-mirror.list` names each package, version, architecture
  and SHA-256 (with its licence). `apt-repo.sh build <target> --rpi …` fetches Raspberry Pi's
  `InRelease`, verifies it with `gpgv` against `tools/release/raspberrypi-archive-keyring.pgp` (the
  key from Raspberry Pi's `raspberrypi-archive-keyring` package; the script checks its fingerprint,
  `CF8A 1AF5 02A2 AA2D 763B  AE7E 82B1 2992 7FA3 303E`), checks `main/binary-arm64/Packages.gz`
  against it, and takes a file only when its SHA-256 matches both the index and the pinned line.
  Verified files are kept in `~/.local/share/lordos-apt/rpi-cache`, so a pinned version can still be
  published after Raspberry Pi's archive keeps only a newer one (it keeps one version per package).
  `apt-repo.sh rpi-fetch <target>` only fetches and verifies. A new upstream version is a new line
  in the list, reviewed like code; the pool only grows.
- **Only on arm64.** They live under `pool/rpi/` and only the `binary-arm64` index lists them (most
  are `Architecture: all`, which would otherwise appear on every PC's Store).
- **Licences** (each package's `debian/copyright`, shipped inside it): the scripts and `raspi-utils`
  are BSD-3-Clause; the bootloader images are under Raspberry Pi's binary licence (redistribution in
  unmodified binary form, for use with Raspberry Pi devices, with the notice: the `.deb` is copied
  as is); `kms++` is MPL-2.0, and its source, Raspberry Pi's `kms++` source package of the pinned
  version, is published next to the binaries: https://lordos.org/apt/pool/rpi/source/k/kms++/ (the
  `.dsc`, `.orig.tar.xz` and `.debian.tar.xz`, pinned in `rpi-mirror.list` with architecture `source`
  and checked against Raspberry Pi's signed `Sources` index; not indexed for `apt source`)
  ([COMPLIANCE.md](COMPLIANCE.md)).
- **Which source wins.** On the LordOS Pi image, which also lists Raspberry Pi's archive with
  priority 990 for these packages (`/etc/apt/preferences.d/raspberrypi`), apt takes the newest
  version among the 990 sources; ours mirrors the same files, so it only matters when a newer version
  is in one of them. No pin is needed for our copy: on a board without Raspberry Pi's archive it is
  the only candidate (priority 500), and our package engine (which follows apt's preferences since
  0.8.5) takes the highest version among equal priorities. A newer `rpi-eeprom` reaches a board through Store → Updates or `apt upgrade` like any
  other package.

## Mirrored apps (catalog)

LordOS offers a curated list of third-party apps (today nine AI apps, and three free-of-charge
proprietary apps in a "commercial" section: Google Chrome, Microsoft Edge and Spotify) through the
Store with its normal package engine. The list is `apt/catalog.json` in the repository (schema,
update steps and trust rules: `apt/README.md`); the vendors' signing keys are in `apt/keys/`,
LordOS's build recipes in `apt/recipes/`. An app reaches a system in one of two ways:

| Delivery | Apps | How |
|---|---|---|
| **direct** | Claude Code (`claude-code`), Claude Desktop (`claude-desktop`), Visual Studio Code (`code`), Cursor (`cursor`); commercial: Google Chrome (`google-chrome-stable`, amd64 and arm64), Microsoft Edge (`microsoft-edge-stable`, amd64), Spotify (`spotify-client`, amd64) | the vendor has a signed APT repository: the user's system adds it (a deb822 `.sources` with the key pinned in the catalog) and installs and updates from the vendor. Nothing is copied to lordos.org. |
| **mirror** | `llama.cpp`, `whisper.cpp`, `codex-cli` (built by LordOS); `goose`, `opencode` (the vendor's .deb) | no vendor repository: verified on the maintainer's Mac and published in `pool/catalog/` of lordos.org/apt, signed with the LordOS archive key with everything else. |

The flow, on the maintainer's Mac only (like the rest of the repository):

1. `apt/build.sh check` validates the catalog and, for every vendor repository, that its key file
   holds exactly the pinned fingerprint, that `InRelease` verifies with it (`gpgv`, `VALIDSIG` by that
   fingerprint, suite and `Valid-Until`), that `Packages` matches `InRelease`, and that the package
   exists for each architecture (it prints the newest version).
2. `tools/release/apt-repo.sh build <target> --catalog …` runs `apt/build.sh fetch`: every pinned
   download over https, checked against its SHA-256 (and codex-cli against its sigstore signature with
   `cosign`); every vendor .deb checked again for its `Package`, `Version` and `Architecture`; the
   recipes run in `debian:trixie` containers of each architecture (amd64 natively, arm64 under qemu)
   with no network. The results go to `pool/catalog/<letter>/<name>/` and into the `binary-amd64` and
   `binary-arm64` indexes like `pool/main`, then the whole repository is signed as before.
3. Everything verified or built is kept in `~/.local/share/lordos-apt/catalog-cache`: a version that
   disappears upstream can still be published, and a built package is never rebuilt for the same
   version (the pool is append-only; `apt-repo.sh` refuses a different file with a published name).

What LordOS builds, and how:

- **llama.cpp** (the local model runner; `llama-cli -hf <user>/<model>-GGUF` and `llama-server -hf …`
  download models from Hugging Face themselves, over HTTPS with OpenSSL) and **whisper.cpp**: built
  from the upstream source tarball of a pinned tag, portable (`GGML_NATIVE=OFF`, every CPU variant of
  the architecture built as a backend module and the best one loaded at run time), programs in
  `/usr/bin`, libraries in `/usr/lib/<name>` (RUNPATH). llama.cpp also has the Vulkan backend
  (`GGML_VULKAN`) as one more module, `libggml-vulkan.so`, loaded only when a Vulkan device exists;
  `libvulkan1` (the loader, about 0.5 MB) is a dependency and `mesa-vulkan-drivers` a recommendation.
  Checked on 2026-10-27 in `debian:trixie` containers (amd64, and arm64 under qemu), with no GPU: with
  no Vulkan driver `llama-cli --list-devices` lists none and the CPU backends are used; with Mesa's
  software driver (lavapipe) the Vulkan backend loads and, with `GGML_VK_VISIBLE_DEVICES=0`, lists
  `Vulkan0: llvmpipe` (ggml skips CPU-type Vulkan devices unless asked). On real AMD, Intel or NVIDIA
  hardware the GPU is picked by default; that path has not been tested on hardware yet.
  whisper.cpp is CPU only. llama-server's web UI is upstream's CI build of the same commit (a pinned
  input), because the build has no network; whisper.cpp's `talk-llama` example (which clones llama.cpp
  at configure time) is left out. No model is downloaded at build time.
- **codex-cli**: OpenAI's static musl binary as `/usr/bin/codex`, with the Apache-2.0 `LICENSE` and
  `NOTICE`; it recommends `bubblewrap` (Codex's Linux sandbox) and `ripgrep`.

**Vendor maintainer scripts.** LordOS never modifies a vendor package. The package engine runs `dpkg`
with `DEBIAN_FRONTEND=noninteractive` and no terminal, without preseeding debconf, so this is what each
does on installation (read from the packages of 2026-10-26: code 1.143.1, claude-desktop 3.6556.2,
cursor 3.27.14, claude-code 2.1.308, goose 1.56.0, opencode 1.19.4; of 2026-10-27: google-chrome-stable
156.0.8078.47-1, microsoft-edge-stable 156.0.4314.50-1, spotify-client 1:1.2.95.453.g0eeebbed):

| Package | On install, unattended |
|---|---|
| `claude-code` | no maintainer scripts. |
| `code` | debconf `code/add-microsoft-repo` defaults to true and there is no terminal, so it writes `/etc/apt/sources.list.d/vscode.sources` (Signed-By `/usr/share/keyrings/microsoft.gpg`, created with `gpg --dearmor`) **unless** a source file already has `URIs: https://packages.microsoft.com/repos/code`; it writes the keyring in both cases. It also links `/usr/bin/code`, registers `/usr/bin/editor` (priority 0) and, with `code/add-copilot-command` defaulting to true, links `/usr/bin/copilot` to VS Code's Copilot CLI shim (which offers to install Copilot CLI when run). postrm deletes `vscode.sources` and the keyring. |
| `claude-desktop` | writes `/usr/share/keyrings/claude-desktop-archive-keyring.asc` always, and `/etc/apt/sources.list.d/claude-desktop.list` plus `/etc/apt/apt.conf.d/50claude-desktop` (unattended-upgrades origin) **unless** another source already lists `downloads.claude.ai/claude-desktop/apt/stable` (or `/etc/default/claude-desktop` has `CLAUDE_DESKTOP_ADD_REPO=false`). It also writes an unconfined AppArmor `userns` profile when `/etc/apparmor.d/abi/4.0` exists, a GNOME Shell search provider, MIME types and `/usr/bin/ccd`. `chrome-sandbox` is setuid root. It recommends `qemu-system-x86`, `ovmf` and `virtiofsd`. |
| `cursor` | debconf `cursor/add-cursor-repo` defaults to true and it does **not** look for an existing source: it always writes `/etc/apt/sources.list.d/cursor.sources` (Signed-By `/usr/share/keyrings/anysphere.gpg`, created with `gpg --dearmor` if missing). Ships `/etc/apparmor.d/cursor-sandbox` (loaded if AppArmor is active) and `/etc/sysctl.d/50-cursor.conf` (comments only); `chrome-sandbox` setuid root; `/usr/bin/cursor`, `/usr/bin/editor` (priority 0). postrm deletes `cursor.sources` and the keyring. |
| `goose` | no maintainer scripts; `chrome-sandbox` is shipped setuid root. |
| `google-chrome-stable` | always writes `/usr/share/keyrings/google-chrome.gpg` (Google's key EB4C 1BFD … D38B 4796 with its three current signing subkeys). On a new install (no `/etc/default/google-chrome`), or when `google-chrome.sources` already exists, it writes `/etc/apt/sources.list.d/google-chrome.sources` (`URIs: https://dl.google.com/linux/chrome-stable/deb/`, `Architectures:` the machine's, Signed-By that keyring) and `/etc/default/google-chrome` (`repo_add_once="false"`, `repo_reenable_on_distupgrade="true"`); it migrates and removes a legacy `google-chrome.list` and `/etc/apt/trusted.gpg.d/google-chrome.gpg`. The daily `/etc/cron.daily/google-chrome` rewrites the key and, while `google-chrome.sources` exists, the source; it doesn't recreate a removed one. An `/etc/default/google-chrome` with `repo_add_once="false"` and no `google-chrome.sources` stops it from adding one. Also `x-www-browser`/`gnome-www-browser` alternatives (priority 200), icons, `defaults.list`, and an AppArmor profile when `/etc/apparmor.d/abi/4.0` exists. postrm cleans the source, key and defaults file only on purge. |
| `microsoft-edge-stable` | the same scripts as Chrome's: `/usr/share/keyrings/microsoft-edge.gpg` (Microsoft's key BC52 8686 … BE12 29CF, the same as VS Code's), `/etc/apt/sources.list.d/microsoft-edge.sources` (`URIs: https://packages.microsoft.com/repos/edge-stable`, the same index as `repos/edge`; the file carries `Architectures: amd64` twice, which apt accepts), `/etc/default/microsoft-edge`, `/etc/cron.daily/microsoft-edge`; purge-only cleanup. |
| `spotify-client` | copies its key (`spotify-2025-11-21-5384CE82BA52C83A.gpg`, E109 6BCB … BA52 C83A) into `/etc/apt/trusted.gpg.d/` (trusted for every source without Signed-By), and writes `/etc/apt/sources.list.d/spotify.list` (`deb https://repository.spotify.com stable non-free`, no signed-by) unless a `.list` file already names that repository or `spotify.list` exists. It only looks at `.list` files, not `.sources`. Icons and the menu entry through `xdg-icon-resource`/`xdg-desktop-menu`; no postrm. The vendor-documented opt-out is an empty `spotify.list`. |
| `opencode` | `/usr/bin/ai.opencode.desktop` through update-alternatives; `chrome-sandbox` made setuid only when unprivileged user namespaces don't work; an AppArmor profile copied to `/etc/apparmor.d/ai.opencode.desktop` when AppArmor is enabled. It ships electron-updater's `app-update.yml` (GitHub releases). |

So that the vendor scripts and LordOS's own source agree, a direct app's source should be written
**before** its package is installed, with `Types: deb` and the `URIs` exactly as in the catalog (VS
Code and Claude Desktop then skip their own entry), and for Cursor, which always writes its own, the
system's file should be exactly Cursor's: `/etc/apt/sources.list.d/cursor.sources` with
`Signed-By: /usr/share/keyrings/anysphere.gpg` (the pinned key, dearmored). Two entries for the same
repository with different `Signed-By` make `apt update` fail ("Conflicting values set for option
Signed-By … The list of sources could not be read", checked with Debian 13's apt in a `debian:trixie`
container on 2026-10-26).

That is what `pkg-source-add` does (`server/src/pkg/sources_admin.c`): the Store adds the source before
installing, and an entry with `"files": {"sources": "cursor", "keyring": "anysphere.gpg"}` is written
under the vendor's own names (the key dearmored because the name ends in `.gpg`) instead of
`lordos-<id>.sources` and `lordos-<id>.asc`; `pkg-source-remove` removes the same files. Both names
come only from the root-owned catalog and pass a strict character set (`lordos*` source names are
refused).

The commercial entries, checked in `debian:trixie` containers on 2026-10-27 with Debian 13's apt (source
written as `pkg-source-add` writes it, then `apt-get install`, `apt-get update` and, for Chrome and Edge,
their cron job):

- **Google Chrome** uses `"files": {"sources": "google-chrome", "keyring": "google-chrome.gpg"}` and the
  URL the package itself writes (`dl.google.com/linux/chrome-stable/deb`): the package replaces the
  file with its own text for the same repository and key, and `apt-get update` is clean. With
  `lordos-<id>` names and the documented `linux/chrome/deb` URL the system would keep two sources for
  the same packages (different URLs, so apt doesn't refuse them; both appear in `apt-cache policy`).
- **Microsoft Edge** is the same with `microsoft-edge` / `microsoft-edge.gpg` and `repos/edge-stable`.
- **Spotify** has no `files`: its package writes a `.list` whatever `.sources` exist. With LordOS's
  `lordos-spotify.sources` and the package's `spotify.list` apt only warns ("Target Packages … is
  configured multiple times") and `apt-get update` succeeds; with an empty `spotify.list` created
  before the install (Spotify's documented opt-out) there is no warning, but `pkg-source-add` doesn't
  write that file yet. Spotify's key goes into `trusted.gpg.d` in both cases.

## The Store's banner

Store → Discover opens with a banner LordOS can change between releases. Its source is in the
repository: `apt/featured/featured.json` and `apt/featured/banner.png` (1400 × 340 works; the picture
is drawn with `objectFit: cover` under a shade on the text side). The JSON:

```json
{
  "schema": 1,
  "catalog": "llama.cpp",          // a catalog app id (apt/catalog.json), or "package": "<Debian package>"
  "kicker": { "en": "…", "es": "…", "pt": "…" },
  "title":  { "en": "…", "es": "…", "pt": "…" },
  "text":   { "en": "…", "es": "…", "pt": "…" },
  "colors": ["#1c1638", "#3a2f8a"] // the gradient under the picture
}
```

`tools/release/apt-repo.sh build` checks both (a schema 1 object of at most 64 KB; a PNG of at most
2 MB) and publishes them as `dists/stable/main/featured/Featured` and `…/Banner`, listed in the
signed Release. The package engine (`server/src/pkg/pkg.c`, `featured_valid`/`banner_valid`) fetches
them on each list update only from a source whose signed Release says `Origin: LordOS`, checks their
hashes like every index, parses the JSON and the PNG signature, and writes
`/var/lib/lordos/pkg/featured.json` and `featured.png`; `lord.pkg.featured()` reads them, with no
arguments. Without them the Store shows a skeleton until the first list update, and a catalog app
when the repository publishes no banner. To change it: edit the two files, rebuild and publish the
repository; machines see it at their next list update.

## The Raspberry Pi kernel

The LordOS kernel for the Pi 5 (`linux-image-<version>-lordos-rpi-2712`, from
`tools/rpi/build-kernel.sh`) is **not** in the repository yet: kernel updates on the Pi come with a
new image for now. Its package installs exactly like Raspberry Pi's own (`raspi-firmware`'s hook
copies the newest kernel to `kernel_2712.img`), with no `tryboot` fallback, so a broken kernel would
leave the board unbootable with no way back from the Store. It goes into the repository when the
image boots new kernels through the firmware's `tryboot` first and keeps the previous one as the
fallback. The `lordos` package itself updates on the Pi like anywhere else (arm64).

Since `6.18.57-3+lordos` (0.8.4) each kernel revision is its own release and package
(`linux-image-6.18.57-lordos.3-rpi-2712`), so a new one installs next to the previous one instead of
over it, and the previous one stays bootable from its own folder on `bootfs` (`RASPBERRY.md`,
"Switching kernels on a board"). That is the package side of the repository path; the automatic
`tryboot` first boot is still missing, so the kernel stays out of the repository (staging and live)
for now.

## Release tests

- **0.8.6 (2026-10-30):** 0.8.5 → 0.8.6 with `apt` against `lordos.org/apt-staging` on a live-session
  amd64 VM (no restart is possible there) and on the Raspberry Pi 5 with a restart: no migration ran,
  the settings files are unchanged, and the Pi came back with 0.8.6-beta build 20261030, the first frame
  7.63 s after the kernel, in a release build with signed code. The Store's own package engine and the
  Store → Updates screen were not exercised, and a second reinstall was not run.
- **0.8.5 (2026-10-27):** 0.8.4.1 → 0.8.5 with `apt` on a VMware machine and 0.8.4 → 0.8.5 with `apt`
  on the Raspberry Pi 5, both tested before the release with the same package built as `0.8.5~rc2`,
  then a restart; no migration runs (`/var/lib/lordos/migrations` unchanged from 0.8.4) and the
  settings are kept. The VMware machine was also updated from 0.8.4.1 to the 0.8.5 package with
  LordOS's package engine against `lordos.org/apt-staging` and measured afterwards
  (`results/0.8.5-beta/vmware-fusion/`, [RESULTS.md](RESULTS.md)).
- **0.8.4.1 (2026-10-25):** 0.8.4 → 0.8.4.1 from the Store and with `apt` on VM 120 (no migration,
  nothing kept back) and with `apt` on the Raspberry Pi 5; the Raspberry Pi packages of the new
  `pool/rpi/` resolve on a Pi with only Debian and LordOS sources ([ISO.md](ISO.md), "Release test
  (0.8.4.1 Beta)").
- **0.8.4 (2026-10-25):** 0.8.3 → 0.8.4 from the Store and with `apt` on VM 120, and with `apt` on the
  Raspberry Pi 5: migration 005 only, the two new libraries (`libsrtp2-1`, `libusrsctp2`) installed
  as dependencies, settings byte-identical; `apt-get upgrade` keeps `lordos` back (new
  dependencies). A staging package re-published under the same version was refused by the Store's
  size check while the CDN still served the old file: staging is append-only too, and a re-upload
  needs a CloudFront invalidation of its path ([ISO.md](ISO.md), "Release test (0.8.4 Beta)").

## Tests (2026-10-18)

- **Staging** (`https://lordos.org/apt-staging`, built and signed on the Mac with `apt-repo.sh`):
  lordos 0.8.0 (amd64, arm64, from the 0.8.0 release, checked against its signed `SHA256SUMS`) and
  test builds `0.8.1~staging*`.
- **Signatures:** `InRelease` verifies with LordOS's own code (`build/pkg-test pgp
  packaging/keys/lordos-archive-keyring.gpg InRelease`: 1 key, 1 valid signature) and with Debian
  13's `apt` (`sqv`); one changed byte is rejected (`pkg.pgp.badSignature`).
- **apt, VM 120 (0.8.0 installed from the ISO, user logged in):** the 0.8.0 one-time step, then
  `apt install lordos`: 0.8.0 → 0.8.1~staging in 29 s with the session open; migration 001
  applied; `lordos`, `lord-netperm`, `lord-fand` active; `settings.json` byte for byte the same;
  `/run/reboot-required.pkgs` = `lordos`; after the restart the login screen came back with the new
  version. That run found the `PermitRootLogin no` lock-out (the VM is administered as root with a
  key), which migration 002 now prevents.
- **`install.sh` on an installed LordOS** (VM 120 back at 0.8.0, session running): with the
  staging package (`--deb`) it upgraded in place to 0.8.1~staging5, kept `session.conf`, the users,
  groups and settings, ran migrations 001 and 002, and reported "LordOS is running and was upgraded
  in place"; running it again reinstalls the same version and changes nothing else.
- **Store → Updates, VM 120 at 0.8.1~staging5,** the repository as a `file:` source (the same
  signed tree as staging): Check for updates verified LordOS's `InRelease` with our own code
  (73,103 packages from 4 sources), showed "LordOS 0.8.1~staging6 is available" with its notes and
  liblzma5 under Debian updates; Update showed the plan (1 to update, 6.0 MB) and installed it with
  progress; "Restart required" appeared; after the restart the session came back on staging6 with
  the user's settings (and `schema: 1` recorded). **Daily check:** with the lists aged two days,
  five minutes after login the session ran `pkg-update` on its own and showed "1 update is
  available" once (`updates.notified` recorded in `settings.json`). **Second upgrade:** a build with a third
  migration ran only that one (`old version: 0.8.1~staging3`); `apt install --reinstall lordos` and
  running `/usr/lib/lordos/migrate` again applied nothing.
- **Raspberry Pi 5 (arm64, lordos 0.8.0):** `apt-get update` against a copy of staging in a
  temporary `Dir::State::Lists` (nothing on the board changed; removed afterwards): signature good,
  `binary-arm64/Packages` read, `lordos 0.8.0 arm64` listed with the right `Filename` and SHA-256.
- `make -C server test` (21 suites passed, 5 skipped for missing tools; `pkg-test` 83/83 with the
  release-notes reader and pre-release version order), `node tools/i18n-check.mjs`, the brand
  check and `shellcheck` on every new script.
- **Not yet:** the CDN change above, so `https://lordos.org/apt-staging` is uploaded and signed but
  `InRelease` still answers 404 (the router) and http 301; `apt-repo.sh verify staging` passes
  only after `tools/web/cdn-apt-behaviors.sh` is run by the account administrator. (Done since: the 0.8.2
  release test updated a 0.8.1 system from `https://lordos.org/apt`, `ISO.md`.)
