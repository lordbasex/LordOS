# LordOS Imager

Our own SD card and USB stick writer, with a first-run experience for LordOS on the Raspberry Pi 5:
choose the device, the system image and the drive, customise the first boot (computer name,
language, user, Wi-Fi, remote access, appearance, cooling), and write. Lumo gives a tip at every
step and celebrates at the end.

It comes in two forms that share the same interface and the same rules:

- **in LordOS**, as a native app (`apps/imager`), shipped with the distribution;
- **for other computers**, as a small desktop app built with Tauri (`desktop/imager`, about 6 MB):
  a Mac download, a Windows download and a Linux download.

It is not a web page: a browser can't write a whole disk.

| In LordOS | On another computer |
|---|---|
| ![Device](imager/native-01-device.png) | ![Device](imager/desktop-01-device.png) |
| ![Customise: cooling](imager/native-10-tab-cooling.png) | ![Customise: cooling](imager/desktop-07-cooling.png) |
| ![Writing](imager/native-15-writing.png) | ![Writing](imager/desktop-11-writing-dark.png) |
| ![Done](imager/native-18-done.png) | ![Done](imager/desktop-12-done-dark.png) |
| ![The Linux download running inside LordOS](imager/desktop-13-linux-in-lordos.png) | ![The Linux download, written through udisks2](imager/desktop-14-linux-done.png) |

## The steps

| Step | What it does |
|---|---|
| **Device** | Raspberry Pi 5 (the only supported board; Pi 500/CM5 and Pi 4/400 are listed as "not supported yet", Pi 3 and Zero 2 as "not planned") or **PC (USB stick)**, which writes the amd64 live ISO. For the Pi, the memory advice: 4 GB minimum, 8 GB comfortable, 16 GB for the best performance. |
| **System image** | The latest LordOS release (version, date, download size, "Signed by LordOS"), older releases on request (since 0.8.5 point releases such as `v0.8.4.1-beta` are listed too: the tag is `v` and three numbers, or four for a point release, plus an optional suffix; `apps/imager/core/releases.js` and the desktop app's core), or **an image file of your own** (`.img`, `.img.xz`, `.iso`) with a clear warning that nobody vouches for it. Downloads are cached and resume after an interruption. |
| **Storage** | Only removable drives (USB, SD cards): model, size, kind, labels, what is mounted. Internal disks and the disk the computer runs from are never listed, and "show all drives" doesn't exist. Too small, write-protected and over-2-TiB drives are shown disabled with the reason; a very large removable drive gets a warning. Erasing is spelled out before anything happens. |
| **Customise** (Pi only) | A switch, then seven tabs: **General** (computer name, language, time zone, keyboard layout, Wi-Fi country), **User** (full name, user name, password with confirmation and a strength hint, automatic login), **Wi-Fi** (network name, password or open network, hidden network, a scan on LordOS), **Remote access** (SSH off / keys only / password and keys, keys pasted or from a `.pub` file; LordOS remote desktop on or off), **Appearance** (Lord Flow or Lord Classic, light or dark, accent, wallpaper), **Cooling** (case with a fan, active cooler, passive case, nothing; a profile; a mild overclock only with a fan), **More** (Bluetooth, automatic update checks, and the statement that LordOS collects nothing). Profiles save and load everything except secrets. |
| **Write** | A summary, then "Erase and write" asks once more. Download (if needed) → release signature → image check → write → read back and compare → first-run file → eject. Progress, speed and time left; Cancel at any time (the drive is wiped so it never looks half-written). |
| **Done** | Lumo celebrates; next steps: put the card in the Pi 5, the first boot takes about a minute, then it is `hostname.local` on the network (`ssh user@hostname.local`, `https://hostname.local:5673`). A sound and a notification when it finishes. |

**App options:** sound when finished, eject when finished, check for app updates (the desktop app;
in LordOS the imager updates with the system), language and appearance (the desktop app; in
LordOS they follow Settings), the download cache (where, how big, clear; the desktop app can move
it). No telemetry: the imager only contacts the LordOS releases on GitHub.

More screenshots: the native app in [`imager/native-*.png`](imager/) (every step and tab, the
custom image warning, the confirmation, cancelling, the options), the desktop app in
[`imager/desktop-*.png`](imager/) (light and dark).

## One interface, two platforms

```
apps/imager/
  index.jsx, ui/*.jsx      the interface: steps, Lumo's tips, dialogs (React)
  state.js                 the wizard state and the write pipeline
  core/                    validators, the first-run file writer, profiles, the release list (plain JS)
  data/                    time zones and keyboard layouts (Debian 13's tzdata and xkb-data)
  platform.js              LordOS: lord.disks, lord.imager, the admin actions, lord.fs
desktop/imager/
  web/sdk.jsx              DOM versions of the few @lordos/sdk primitives the UI uses
  web/platform.js          the same functions as apps/imager/platform.js, over Tauri commands
  build-web.mjs            builds apps/imager against those two files (esbuild)
  core/                    lordos-imager-core (Rust): signature, .xz index, write and verify, drives, secrets
  src-tauri/               the Tauri shell and its commands
```

The interface imports only View, Text, Image, ScrollView, TextInput, Button, Lumo, useTheme,
useKeyboard, t, typeScale, spacing and a store from `@lordos/sdk`; in the desktop build those come
from `web/sdk.jsx`, which draws them with HTML and Lord Flow's colors (light, dark or automatic).
The texts are the same catalogs (`assets/locales/<lang>/imager.json`, plus errors, status, mascot
and countries), so both apps speak English, Spanish and Portuguese identically.

The writing rules exist twice, in C for LordOS (`server/src/imager/`) and in Rust for the desktop
(`desktop/imager/core/`), and are tested against the same fixtures (a real signed `SHA256SUMS`, the
same `.xz` files, the same password and Wi-Fi vectors).

## Security model

### In LordOS

`lord-server` runs as the user and never executes anything. Everything that needs root is one of
four closed admin actions in `server/src/imager/imager_admin.c` (table `IMAGER_ADMIN_ACTIONS`):

| Action | Argument | What it does |
|---|---|---|
| `imager-fetch` | `meta`, or `file\n<tag>\n<name>\n<size>` | `/usr/bin/curl` **as the user** (setresuid, no new privileges, no curlrc, https only also after redirects) fetches `https://api.github.com/repos/lordbasex/LordOS/releases?per_page=10` or `https://github.com/lordbasex/LordOS/releases/download/<tag>/<name>` into `~/.cache/lordos/imager` (resumable `.part`; the tag and name are validated; only images and the two signature files). Root never parses what arrives. |
| `imager-fetch-cancel` | none | stops this user's download |
| `imager-cache-clear` | none | deletes the imager's cache, as the user, one level of release folders |
| `imager-write` | `<disk>\n<size>\n<model>\n<serial>\nofficial\|custom\n<image>[\n<SHA256SUMS>\n<SHA256SUMS.sig>]` | writes the image to a whole disk (below) |

`imager-write`, all checked on the root side:

1. **The drive:** a whole disk (the `disk-*` rules: a real block device whose `/dev` node matches
   sysfs), never the system disk, **only removable** (USB or a card; refused even for an
   administrator), not read-only, at most 2 TiB, and its size, model and serial are the ones the
   user picked.
2. **The image:** opened with the user's permissions, only from their home or their mounts, never
   from the disk being written; `.img`, `.img.xz` or `.iso`; the `.xz` index gives its uncompressed
   size, which must fit.
3. **Official images:** `SHA256SUMS.sig` must verify with the release key compiled into LordOS
   (`codesig_release_key`, fingerprint `c7155b8e…53ea108e`, docs/SIGNING.md) and the file's SHA-256
   must be the listed one — **before anything is erased**.
4. **Right before opening:** the identity is checked again, mounts under `/media` are unmounted
   (anything mounted elsewhere is refused), the device is opened `O_EXCL | O_DIRECT`, and the
   opened descriptor must be that very device (major:minor, `BLKGETSIZE64`).
5. **Writing:** a raw image is copied; an `.img.xz` is decompressed by `/usr/bin/xz` running **as
   the user** with no privileges, fed through pipes, so root never decodes compressed data. The
   file's bytes are hashed again while writing; if they no longer match the signed hash, the write
   fails.
6. **Verifying:** the drive is read back with `O_DIRECT` and its SHA-256 compared with what was
   written.
7. **Failing safely:** on any error or a cancel after the first write, the first and last MiB are
   zeroed, so a half-written card never looks bootable. Cancelling is `disk-cancel <disk>`.

The first-run file then goes onto the card's boot partition the way any file goes onto a USB stick:
`disk-mount` mounts it for the user under `/media/<user>`, `lord.fs.write` writes the file, and
`disk-unmount` and `disk-eject` finish. The board validates the file again (below).

`lord.imager` (`server/src/ui/js_imager.c`) only computes: `hashPassword` (yescrypt with libcrypt,
Debian's default), `wifiPsk` (PBKDF2-HMAC-SHA1, 4096 rounds), `checkSums` (the signature check, so
the UI can show it before writing) and `imageInfo` (size and uncompressed size of a file in the
user's folders). It writes nothing and runs nothing.

### On other computers

- **Downloads** go only to the LordOS releases on GitHub over HTTPS (Rust, `reqwest` with rustls),
  into the platform's cache folder, resumable. What is trusted is decided by the signature, never by
  the transport.
- **Drives:** only removable, non-system drives are listed and accepted; the identity is checked
  again right before opening; nothing of the drive stays mounted.
  - Mac download: `diskutil` (external, physical disks; the boot disk and internal ones are left
    out). The raw device is opened by the system's authorized-open helper (`/usr/libexec/authopen`),
    which asks for an administrator's approval in the system's own dialog and hands back only that
    one open device; the app itself never runs as root.
  - Linux download: `lsblk` for the list; the device is opened through udisks2 (`OpenDevice`, which
    polkit approves like a desktop's "Format disk"); the boot partition is mounted and the drive
    powered off through udisks2 too. The `.deb` depends on `udisks2`.
  - Windows download: `Get-Disk` (USB, SD and MMC bus types; never a boot or system disk); the
    `.exe` carries an administrator manifest (`requireAdministrator`); every volume on the disk is
    locked and dismounted before writing.
- **The same pipeline** as in LordOS: signature and hash before erasing, decompression while
  writing with the file hashed again, read back and compare, wipe on failure or cancel.
- **Secrets** are derived in Rust: the password as SHA-512 crypt with 200 000 rounds (`$6$`, accepted
  by every Linux PAM and by `lordos-firstrun`), the Wi-Fi key as the WPA2 PSK.
- **Test devices:** `LORDOS_IMAGER_TEST_DEVICES=<folder>` also lists disk images (Mac download) or loop
  devices (Linux) whose backing file is inside that folder, and nothing else; disk images attached
  for any other purpose are never listed. `LORDOS_IMAGER_TEST_STATE=<file.json>` opens the window at
  a given step with given choices, for tests and documentation screenshots; it can start a write only
  to such a test device. Neither is ever set by the app.

## The first-run file

`lordos-firstrun.txt` on the boot partition (FAT, `bootfs`), written by both imagers
(`apps/imager/core/firstrun.js`) and read once by the Pi on its first boot.

- UTF-8 text, LF or CRLF, a BOM allowed, at most 64 KiB, no NUL or control characters.
- Lines are empty, comments (`# …`) or `key=value`. The first setting must be `version=1`.
- **Strict:** an unknown key, a repeated key (except `ssh_key`), a value out of range, a space
  around a value, or a broken rule between keys makes the whole file invalid, and **nothing** of it
  is applied (the board starts in setup mode, as without a file).
- **No secrets in clear:** the password is a crypt(3) hash, the Wi-Fi password is replaced by its
  derived key.
- Only what was chosen is written; everything else keeps the image's defaults.

| Key | Value | Rule |
|---|---|---|
| `version` | `1` | first |
| `hostname` | RFC 1123 label | `[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?`, not only digits, not `localhost` |
| `lang` | `en` \| `es` \| `pt` | the languages LordOS ships |
| `timezone` | `Area/City` | `[A-Za-z0-9_+-/]`, starts with a capital, no `..`; must exist in the image's zoneinfo |
| `keyboard`, `keyboard_variant` | xkb layout and variant | `[a-z]{2,10}`; variant `[A-Za-z0-9_-]{0,32}`; the layout must exist in the image |
| `wifi_country` | two capital letters | `cfg80211.ieee80211_regdom=` in `cmdline.txt` |
| `user` | user name | `^[a-z_][a-z0-9_-]{0,31}$`, not a reserved system name |
| `fullname` | UTF-8 | up to 128 bytes, no control or bidi-override characters, no `: , = \` |
| `password_hash` | `$6$[rounds=N$]salt$…` or `$y$…` | required with `user` |
| `autologin` | `0` \| `1` | needs `user` |
| `wifi_ssid_hex` | the network name's bytes in lower-case hex | 1–32 bytes, no NUL |
| `wifi_psk` | 64 lower-case hex digits | the WPA2 key; without it the network is open |
| `wifi_hidden` | `0` \| `1` | needs `wifi_ssid_hex` |
| `ssh` | `off` \| `password` \| `keys` | `keys` needs at least one `ssh_key`; on needs `user` |
| `ssh_key` | an OpenSSH public key line | up to 16; ed25519, ECDSA, security keys or RSA ≥ 2048 bits; the blob's type must match; no `command=` or other options |
| `remote_desktop` | `0` \| `1` | needs `user` |
| `bluetooth` | `0` \| `1` | |
| `theme` | `default` \| `classic` | |
| `mode` | `light` \| `dark` | |
| `accent` | a LordOS accent name | `azul`, `violeta`, `rosa`, `naranja`, `verde`, `grafito`, … |
| `wallpaper` | a wallpaper id | `[a-z0-9-]{1,48}` |
| `cooler` | `case-fan` \| `active-cooler` \| `passive` \| `none` | |
| `performance` | `balanced` \| `performance` \| `quiet` \| `conservative` | needs `cooler`; `performance` and `quiet` need a fan |
| `overclock` | `0` \| `1` | `1` needs a fan |
| `auto_updates` | `0` \| `1` | |

Example (what the imager writes):

```
version=1
hostname=pi-living
lang=en
timezone=America/Argentina/Buenos_Aires
keyboard=us
keyboard_variant=
wifi_country=AR
user=ana
fullname=Ana Perez
password_hash=$y$j9T$szwfNUJQrRIXau020U3AQ.$JWQYK9BLxED/3ePLXhQZSPEoVcZhisZzAZ.B5GpXuk3
autologin=0
wifi_ssid_hex=43617361204c6f7264
wifi_psk=d98e0bb94693bd28702424c1dfe34e440968a185eee961785e000f7b50c2b825
wifi_hidden=0
ssh=keys
ssh_key=ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIF8zdvpQ/Ic++n9TOGaDGEe9/+IRgtRBf1H86da41yJ8 ana@laptop
remote_desktop=1
bluetooth=1
theme=default
mode=dark
accent=azul
wallpaper=lumo-celestial
cooler=active-cooler
performance=performance
overclock=1
auto_updates=1
```

### On the board

`lordos-rpi-firstboot.service` (`/usr/lib/lordos/rpi/firstboot`, docs/RASPBERRY.md, "First boot")
runs `/usr/lib/lordos/bin/lordos-firstrun apply /boot/firmware/lordos-firstrun.txt` on the first
boot, before the user section. `lordos-firstrun` (`server/src/firstrun/`) parses the file with the
strict parser above (`fr_parse`, pure), and only if it is valid applies it, as root:

| Part | How |
|---|---|
| Computer name | `/etc/hostname`, the `127.0.1.1` line of `/etc/hosts` |
| Time zone, language, keyboard | `/etc/localtime` → zoneinfo, `/etc/timezone`, `/etc/locale.conf` (Debian 13's `/etc/default/locale` points there), `/etc/default/keyboard` |
| Wi-Fi country | `cfg80211.ieee80211_regdom=` in `cmdline.txt` (one line kept) |
| Wi-Fi | `/etc/lordos/net/wifi/<ssid hex>.json` and `.key` (0600, `psk=<hex>`), the format Settings → Network uses; lord-netd now accepts a derived key there (WPA2; a WPA3-only network needs the password itself) |
| User | `useradd` and `usermod -p <hash>` (fixed paths, the values as arguments, never a shell), groups `sudo audio video input render` (and `pipewire plugdev lpadmin bluetooth netdev` when they exist), linger, the home folders in the chosen language, `~/.config/lordos/settings.json` (language, keyboard, theme, mode, accent, wallpaper, `updates.autoCheck`) and `~/.config/locale.conf`; the setup account is then removed by `firstboot` |
| Automatic login | `AUTOLOGIN=<user>` (or empty) in `/etc/lordos/session.conf` |
| SSH | `systemctl enable` + `start --no-block ssh.service`; with keys only, `~/.ssh/authorized_keys` (0600 in a 0700 folder) and `/etc/ssh/sshd_config.d/05-lordos-keys-only.conf` (`PasswordAuthentication no`, `KbdInteractiveAuthentication no`); root never logs in (10-lordos.conf) |
| Remote desktop | `/etc/lordos/remote/config` with `enabled=1`, `port=5673`, `allow=all`, `net=lan` (local network only) — the same file Settings → Sharing writes, with the 0.8.3 defaults (TLS, the lockout after 3 failed sign-ins) |
| Bluetooth off | a soft block of every Bluetooth radio through `/dev/rfkill` (`RFKILL_OP_CHANGE_ALL`), kept across boots by systemd-rfkill; Settings → Bluetooth turns it back on |
| Cooling | `/etc/lordos/fan.json` with the profile (and `PROFILE` to `lord-fand` if it is already running); performance: `/etc/tmpfiles.d/lordos-cpu-governor.conf` sets the `performance` governor at boot; a block of our own in `config.txt` between `# >>> lordos-firstrun` and `# <<< lordos-firstrun`: `arm_freq=2600` for the mild overclock (fan only), `arm_freq=2000` for the conservative profile without cooling |

The file is then **shredded** (overwritten with random bytes, then zeros, `fsync`, deleted) whatever
the outcome; `firstboot` deletes it too if anything is left. On an SD card the flash controller may
keep old copies of blocks, which is why the file never holds a secret in clear in the first place.

Exit status: 0 applied with a user (the board goes to the login screen, or logs in automatically),
2 applied without a user (setup mode), 1 invalid file (nothing applied), 3 the user could not be
created. `--root DIR --boot DIR` apply into a mounted image for tests (`useradd`/`usermod`/`systemctl
--root`; nothing is started).

### Cooling profiles

| Cooler | Profiles | What they set |
|---|---|---|
| Case with a fan, active cooler | **Balanced** (default), Performance, Quiet; optional mild overclock | lord-fand's curve (docs/FANS.md): Balanced 55 °C → 30 %, Performance 45 °C → 40 % plus the `performance` CPU governor, Quiet 60 °C → 30 %; overclock 2.6 GHz instead of 2.4 GHz with no extra voltage |
| Passive case | Balanced (default), Conservative | no fan to drive; Conservative caps the clock at 2.0 GHz |
| Nothing | **Conservative** (default), Balanced | 2.0 GHz so a bare board doesn't reach the 85 °C throttle within minutes |

The overclock warns that not every board is stable and how to undo it (delete our block in
`config.txt` from another computer). It is refused by the parser without a fan.

## Accessibility

- **Keyboard:** Tab and Shift+Tab move a visible focus ring (2 px in the accent color) through
  every button, card, switch, tab and field of the step; Enter or Space activates; Escape goes back.
  In the desktop app the browser's own focus does the same and every control has a screen-reader
  label, role and state (`aria-label`, `aria-checked`, `aria-selected`, `role="progressbar"`…).
  LordOS itself has no screen reader yet; the labels are in place for when it does.
- **Texts:** every string is in English, Spanish and Portuguese (`node tools/i18n-check.mjs`).
- **Never color alone:** problems carry text, the steps carry numbers and checkmarks.

## Building the desktop downloads

Everything is built from `desktop/imager`. The interface is built first (`node build-web.mjs` →
`dist/`), then the Rust shell bundles it.

### Mac download

On the development machine (the Mac download is built on one), with Rust (rustup) and Node:

```sh
desktop/imager/scripts/build-mac.sh
# → desktop/imager/target/release/bundle/macos/LordOS Imager.app (6.3 MB)
#   desktop/imager/target/universal-apple-darwin/release/bundle/dmg/lordos_imager_0.8.6_universal.dmg (7.5 MB)
```

**Signing (since 2026-10-19):** the download is built for both processor families
(`--target universal-apple-darwin`), signed with the maintainer's code-signing identity (team
`UR4X327NVD`, hardened runtime, timestamp), notarized and stapled, so it opens without warnings.
`scripts/build-mac.sh` does all of it when `IMAGER_SIGN_IDENTITY` is set and a notarization profile
named `lordos-notary` exists in the keychain (created once by the maintainer with
`xcrun notarytool store-credentials lordos-notary --apple-id <id> --team-id UR4X327NVD`; the
password never goes into the repository or a script). The LordOS release key
(`~/.config/lordos-release/`) signs release files, not apps, and is not used here. Without
`IMAGER_SIGN_IDENTITY` the build stays unsigned and the first launch needs a right click → Open.

### Linux download

In a Debian 13 container (Docker), so the host needs nothing but Docker:

```sh
desktop/imager/scripts/build-linux.sh
# → desktop/imager/target/linux/lordos_imager_0.8.6_amd64.deb (4.1 MB, depends on udisks2 and WebKitGTK 4.1)
#   desktop/imager/target/linux/lordos-imager (7.6 MB)
```

### Windows download

Cross-built in the same container with `cargo-xwin` (clang, lld and the MSVC runtime headers that
xwin downloads), NSIS for the installer:

```sh
desktop/imager/scripts/build-windows.sh
# → desktop/imager/target/windows/lordos_imager_0.8.6_x64_setup.exe (3.3 MB)
#   desktop/imager/target/windows/lordos-imager.exe (6.2 MB, requireAdministrator)
```

The installer is unsigned (Tauri signs only when it builds on that platform itself, or with a custom sign command).
`.github/workflows/imager-desktop.yml` builds all three on hosted runners; it is in the tree but
was never pushed or run.

The containers keep their own `node_modules` and cargo target folders in named volumes
(`lordos-imager-*`), so the host's are never mixed in; `docker volume rm` them to start clean.

## Tests

| Test | What | Result |
|---|---|---|
| `build/firstrun-test` (`make -C server test`) | validators, a full file, 55 broken files, cross-key rules, 16/17 keys, a 64 KiB+1 file, 30 000 mutations | 166 checks |
| `build/firstrun-fuzz` | the same with 300 000 mutations under ASan and UBSan | 0 errors |
| `build/imager-test` (ASan/UBSan) | the real 0.8.1 `SHA256SUMS` signature: good, without the newline, tampered list, truncated list, tampered signature, missing, short, not base64, other key; the key's fingerprint; `SHA256SUMS` lookups; names and tags; the `.xz` index on one stream, many blocks, two streams and stream padding, every byte of the last 64 flipped, 40 truncations, 8 000 random edits | 461 checks |
| `tests/imager_core_test.mjs` | the shared JS: validators, the first-run writer (and every file it writes parsed by the C parser), profiles without secrets, the release list (with hostile names and tags) | 79 checks |
| `desktop/imager/core` (`cargo test`) | the Rust core: the same signature fixtures and the 0.8.0 signature over the 0.8.1 list, the key fingerprint, the `.xz` index, the IEEE 802.11i PSK vector, glibc's SHA-512 crypt vectors, write + verify of raw and two-stream `.xz` images to a file standing in for a device, refusals (too large, a changed file wipes the device), cancel in the middle wipes the device | 7 tests, on the development machine and on Linux |
| `server/tests/imager_write_test.sh` (root, VM 116) | `imager-write` against a virtual USB stick (the kernel's USB gadget over a file): 17 refusals (internal disk even for an administrator, system disk, partition, wrong size/model/serial, tampered list, another release's signature, missing signature, unlisted image, too large, broken `.xz`, a link to `/etc/shadow`, a file outside the home, not an image, missing signature lines, a control character), a custom image and the official 0.8.1 image written and read back independently, cancel in the middle (first MiB wiped), another user can't cancel | 24/24 |
| `server/tests/firstrun_image_test.sh` (root, VM 116) | the real 0.8.3 Raspberry Pi image written by `imager-write`, the first-run file put on its boot partition as the user, then `lordos-firstrun --root --boot`: every file it should write, owners and modes, nothing else changed in `cmdline.txt`/`config.txt`, no Wi-Fi password anywhere, the file shredded, and a broken file applies nothing | 25/25 |
| Native app on VM 116 | the whole flow with the 0.8.3 image to the virtual stick: written and verified; the first-run file it wrote parses, its yescrypt hash verifies with `crypt(3)` and its PSK matches `pbkdf2_hmac` | ✓ |
| Desktop app (Mac download, on the development machine) | the whole flow through the app's own commands (`LORDOS_IMAGER_TEST_STATE`, no simulated input) to a 16 GB disk image: download of 0.8.3 from GitHub, signature, image check, write, read back (the same SHA-256 as the native write, `b623f7a8…`), the first-run file on the boot partition (it parses, glibc accepts its SHA-512 hash, the PSK matches) | ✓ |
| Linux download inside LordOS (VM 116) | the `.deb`'s binary as a Wayland app in the LordOS session: lists only the virtual stick (2 disks hidden), downloads 0.8.3 again, signature, image check, write and read back through udisks2's `OpenDevice` (the same SHA-256); without a polkit agent udisks2 answers "not authorized" and nothing on the drive is touched, with a test rule standing in for the agent the write goes through | ✓ |
| Downloads on VM 116 | `imager-fetch meta`, the two signature files, the 558 MB image cancelled at 335 MB and resumed from the `.part` | ✓ |

## Not done or not verified yet

- **Not booted on a real Raspberry Pi:** the first-run file was applied with `--root` to the real
  0.8.3 image (arm64) from an amd64 build, and the image's own `firstboot` script runs `lordos-firstrun`
  but wasn't executed (no board was reflashed, as asked). Since 0.8.4 the `lordos` package ships
  `lordos-firstrun`, so the Pi image published with 0.8.4.1 reads the file (a 0.8.4 image build
  applied a file from the Imager's writer with `--root/--boot`: 26/26, `ISO.md`, "Release test (0.8.4
  Beta, October 19–25)"); images up to 0.8.3
  ignore it and start in setup mode. A real first boot from a card the Imager prepared is still not
  recorded.
- **Not tested with real drives:** only virtual ones (the USB gadget on VM 116, a disk image on the
  development machine). The authorized-open prompt of the Mac download, udisks2's `OpenDevice` with
  polkit and the Windows download were not run against a real stick (the test opened its own disk
  image directly; the Linux and Windows downloads were built and the core tested on Linux, not run
  with a window).
- **Wi-Fi scan** only on LordOS (the desktop app types the name).
- **App updates** on the desktop: the option looks for a newer LordOS Imager among the files of the
  LordOS releases it already reads; no release carries one yet, so it has never found one.
- **WPA3-only networks** need the password on the board (only the WPA2 key is stored).
- **Secrets in the host keychain:** profiles never keep secrets at all; storing them in the
  keychain was left out.
