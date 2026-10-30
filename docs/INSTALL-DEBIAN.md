# Installing LordOS on Debian 13

LordOS can be added to a Debian 13 (trixie) system that is already installed, instead of booting the
ISO. The installer downloads the LordOS package from the GitHub release, installs it with apt, and
sets the system up the way the ISO does (`ISO.md`): LordOS's own login screen on tty1, its
services, and the user's groups.

```sh
curl -fsSL https://lordos.org/install.sh | sh
```

The script asks before changing anything. It reads the answers from the terminal (`/dev/tty`), so
this works even though the script itself comes through a pipe.

> **Status (2026-10-30):** the latest release is 0.8.6-beta, and the GitHub repository is now public, so
> the installer can look up and download its release files. Without access to GitHub, use `--deb` with a
> package you already have (see [Testing](#testing)).

## Requirements

| | |
|---|---|
| System | Debian 13 (trixie). A derivative based on trixie (`ID_LIKE=debian`) works, with a warning: only Debian itself is tested. |
| Architecture | `amd64` or `arm64`. On a Raspberry Pi 5 the [Raspberry Pi image](RASPBERRY.md) is the recommended path (tested kernel and boot setup); the installer says so and continues. |
| Rights | root, or a user who can use `sudo` |
| Disk | at least 2 GB free on `/var` (the package plus the dependencies apt pulls in) |
| Init | systemd running (Debian's default) |
| Network | to download the release and the Debian dependencies |

## What it does

1. **Checks** the points above and shows what it is going to do. Nothing changes until you answer `y`.
2. **Downloads** `SHA256SUMS` and `lordos_<version>_<arch>.deb` from the GitHub release
   (`https://github.com/lordbasex/LordOS/releases`), by default the latest one. The latest
   release is looked up through the GitHub API; since betas are marked as pre-releases, which
   `releases/latest` skips, it falls back to the newest release of any kind.
3. **Verifies** the `.deb`'s SHA-256 against `SHA256SUMS`. A mismatch stops everything.
   - **Signature:** before downloading the `.deb`, it checks `SHA256SUMS.sig`, the Ed25519
     signature of `SHA256SUMS` made with the LordOS release key (fingerprint
     `c7155b8e…53ea108e`, see `SIGNING.md`, "Release files"). A missing or invalid signature
     stops the install.
4. **Installs** with `apt-get install ./lordos_<version>_<arch>.deb`, so the dependencies come from
   your Debian mirrors. It also installs, if they are missing, what the ISO adds for sound, fonts
   and the user session (`dbus-user-session`, PipeWire with `pipewire-pulse`, `pipewire-alsa` and
   WirePlumber, `rtkit`, `alsa-ucm-conf`, the Noto and DejaVu fonts, `libthai-data`,
   `shared-mime-info`). `--no-extras` skips those. (LordOS's own package engine is for apps
   inside LordOS; a local `.deb` on Debian goes through apt, as usual.)
5. **Sets up LordOS** like the ISO:
   - creates the `lordos-greeter` system user (the login screen's user) if it doesn't exist;
   - adds your user (the one who ran `sudo`, or `--user NAME`) to whichever of these groups exist:
     `audio video input render lpadmin pipewire plugdev`. If the system has no administrator at all (a Debian
     installed with a root password), it also offers to make that user one, by adding them to `sudo`: the Store
     can't install or update without an administrator. Otherwise it does **not** add anyone to `sudo`;
   - generates the `en_US`, `es_ES` and `pt_BR` UTF-8 locales (the languages the interface ships).
     Your system language is not changed;
   - writes `/etc/lordos/session.conf` with an empty `AUTOLOGIN=`: the login screen, no automatic
     login (Settings → Users changes it). An existing file is kept;
   - enables `lord-netperm` (per-app network permissions) and `lord-fand` (fans);
   - **display manager:** if GDM, SDDM, LightDM or another display manager is set as
     `display-manager.service`, it asks whether to disable it. If you say yes, it is disabled (not
     removed), `lordos.service` is enabled and `getty@tty1` is masked: at the next boot LordOS shows
     its login screen on tty1. The other text consoles stay (Ctrl+Alt+F2). If you say no (or pass
     `--keep-display-manager`), LordOS is installed but not enabled at boot, and the final message
     shows the commands to switch later;
   - enables linger for your user, so their PipeWire starts with the system (as on the installed ISO);
   - enables the PipeWire user units globally (Debian's packages already do this).
6. **Doesn't touch:**
   - **the network.** NetworkManager, ifupdown and systemd-networkd keep their interfaces:
     `lord-netd` shows them as system interfaces and doesn't run DHCP or Wi-Fi on them
     (`NETWORK.md`). Only interfaces nobody manages get LordOS's DHCP;
   - **remote desktop**, which stays off (port 5673 closed; Settings → Sharing turns it on);
   - your home folder and your current desktop session. The display manager is only disabled for
     the next boot; nothing is stopped while you are using it;
   - **browsers.** The ISO and the Raspberry Pi image preinstall Chromium and Firefox ESR; the
     installer doesn't (decided in 0.8.4.1): it also runs on servers and VPSs, where a few hundred
     megabytes of browsers are unwanted, and on a desktop Debian (or Raspberry Pi OS) the browser you
     already have keeps working inside LordOS. Install one from the Store if the system has none.
     Without the image's defaults (`tools/iso/browser-defaults.sh`) the browsers start with the
     LordOS frame; Settings → Appearance → «Browsers: tabs in the title bar» switches them to their
     own single bar.
7. **Prints** what changed, how to start LordOS and how to uninstall.

What it changed is recorded in `/var/lib/lordos-installer/state`, so `--uninstall` undoes exactly
that. No telemetry: the only requests go to `api.github.com`, `github.com` and your apt mirrors.

### Starting LordOS

- **Reboot** (`sudo reboot`): the LordOS login screen appears.
- Or, if no display manager is running, from a text console: `sudo systemctl start lordos`. If GDM
  or another display manager is still running your session, reboot instead: both want tty1 and the
  graphics card.

## Options

```sh
curl -fsSL https://lordos.org/install.sh | sh -s -- [options]
```

| Option | |
|---|---|
| `-y`, `--yes` | don't ask (also `LORDOS_YES=1`). With it, the display manager **is** disabled; add `--keep-display-manager` to avoid that |
| `--version X` | install release `X` (`0.8.0-beta` or `v0.8.0-beta`) instead of the latest |
| `--deb FILE` | install a local `.deb`. If a `SHA256SUMS` is next to it, it is checked; otherwise the output says the checksum was not verified |
| `--user NAME` | the user to set up (default: the user who ran `sudo`; as plain root, who logged in, or the only regular user) |
| `--keep-display-manager` | leave the current display manager enabled; LordOS is not enabled at boot |
| `--no-extras` | don't install the extra sound and font packages |
| `--boot-splash` | set up the LordOS boot splash without asking (see "Boot splash (optional)") |
| `--no-boot-splash` | don't offer the boot splash |
| `--no-systemd-check` | continue without a running systemd (containers, chroots): services, display manager, getty and linger are skipped. Detected automatically in Docker/Podman |
| `--remote-desktop` | also turn on LordOS remote desktop, for servers and VPS without a screen (see "Servers and VPS"). It asks first, with a warning, unless `--yes` |
| `--remote-port N` | with `--remote-desktop`: the port, 1024–65535 (default 5673) |
| `--uninstall` | remove LordOS and restore the previous login setup; also removes the remote desktop configuration |
| `--purge` | with `--uninstall`: purge the package and its files in `/etc` |
| `-h`, `--help` | help |

## Boot splash (optional)

At the end the installer asks whether to show the LordOS boot splash (the crown and a progress bar,
`ISO.md`, "Boot splash and boot menu"). The default answer is no, and `--yes` skips it unless
`--boot-splash` is given. If you accept:

- `plymouth` and `plymouth-label` are installed and the theme is set to `lordos` with
  `plymouth-set-default-theme -R` (it rebuilds the initramfs). If you had picked a boot splash theme
  that isn't one of Debian's own, it asks before replacing it.
- **GRUB, only if nobody customised it for this:** when the system boots with GRUB, and its settings
  (`/etc/default/grub` and `/etc/default/grub.d/*.cfg`) don't already have `splash`/`nosplash` or a
  `GRUB_THEME`, and the terminal isn't `console`/`serial`, it writes its own drop-in
  `/etc/default/grub.d/lordos.cfg` (`splash` added to `GRUB_CMDLINE_LINUX_DEFAULT`,
  `GRUB_GFXPAYLOAD_LINUX=keep` and, on amd64, the LordOS boot menu as `GRUB_THEME`) and runs
  `update-grub`. `/etc/default/grub` itself is never edited. Without GRUB (systemd-boot, a
  Raspberry Pi), only the theme is set and the final message says to add `quiet splash` to the kernel
  command line.

`--uninstall` removes the drop-in (and runs `update-grub`) and puts the previous Plymouth theme
back; Plymouth itself stays installed.

## Servers and VPS

The same command works on a server or a VPS with no monitor, on **amd64 and arm64** (the release
has a `.deb` for each, `lordos_<version>_amd64.deb` and `lordos_<version>_arm64.deb`; the installer
picks the one for the machine). Add `--remote-desktop` to reach the desktop from a browser:

```sh
curl -fsSL https://lordos.org/install.sh | sh -s -- --remote-desktop
curl -fsSL https://lordos.org/install.sh | sh -s -- --remote-desktop --remote-port 8443
```

**What works headless.** LordOS's sessions don't need a screen (`SESSIONS.md`): each session is a
`lord-server --headless`, and the login screen runs headless too. Without a display manager the
installer enables `lordos.service` (as on any machine without one), so `lord-sessiond` starts at
boot and serves the login screen and the sessions over the remote desktop. Apps, Linux apps, the
firewall and the rest of the desktop work as on a PC; without a GPU everything is drawn on the CPU.
The remote desktop carries the session's sound (Opus over WebRTC) since 0.8.4.

**What `--remote-desktop` does:**

- In interactive mode it first warns that the machine will accept remote-desktop logins from the
  network on that port, and asks. "No" keeps the rest of the install and leaves remote desktop off.
  With `--yes`, the flag is the answer (the warning is still printed).
- It writes `/etc/lordos/remote/config` (`enabled=1`, the port, `allow=all`, `insecure_dev=0`) in
  the same format Settings → Sharing and the Raspberry Pi `remote-desktop` switch file use
  (`REMOTE.md`). Values saved there before (`allow=group`, a port) are kept unless
  `--remote-port` is given.
- If `lordos.service` is already running it is reloaded (it rereads the file). If it is enabled but
  not running, it is started right away when that takes the screen from nobody: no display manager
  running and nobody logged in on tty1. Otherwise it starts at the next boot.
- The final message gives the address, and warns when there is no account that can log in yet.

**Connecting.** Open `https://<the server's IP>:5673` (or your port) in a browser and log in with a
user of the machine that has a password (uid 1000 or above; `sudo adduser NAME` creates one; root
can't log in). The certificate is self-signed: the browser warns the first time; compare its
fingerprint with `/etc/lordos/remote/fingerprint` on the server before accepting it. Nothing has
to be installed on the other computer. See `REMOTE.md`.

**Security.** The port is protected by TLS and the account's password, and nothing else. On a
machine reachable from the internet:

- don't leave the port open to everyone: allow it only from your own addresses in the provider's or
  the host's firewall, or reach it through a VPN (WireGuard, OpenVPN) or an SSH tunnel
  (`ssh -L 5673:localhost:5673 server`, then `https://localhost:5673`);
- use strong passwords for every account that can log in, or limit remote login to the
  `lordos-remote` group (Settings → Sharing, `allow=group`);
- turn it off when you don't need it: Settings → Sharing, or `enabled=0` in
  `/etc/lordos/remote/config` and `sudo systemctl reload lordos`.

`--uninstall` removes `/etc/lordos/remote/config`, so the port is closed.

## Updating

The package adds the signed LordOS repository (`/etc/apt/sources.list.d/lordos.sources`, key
`/usr/share/keyrings/lordos-archive-keyring.gpg`), so new versions arrive in place: Store → Updates
shows "LordOS <version> is available", or `sudo apt update && sudo apt upgrade` does the same. Files,
settings, users and the login setup are kept, and the package applies whatever each version changes
in the system ([UPDATES.md](UPDATES.md)). Running `install.sh` again also upgrades in place; it is
the one-time step for LordOS 0.8.0, whose package does not have the repository yet. Uninstalling
removes the repository with the package.

## Uninstalling

```sh
curl -fsSL https://lordos.org/install.sh | sh -s -- --uninstall
```

It asks first, then:

- disables `lordos.service` and `lord-netperm` (the package's `prerm` gives the fans back to the
  kernel and disables `lord-fand`);
- unmasks `getty@tty1`;
- enables the previous display manager again. Without a record (LordOS installed by hand), it
  offers the first display manager it finds installed (`gdm3`, `sddm`, `lightdm`, …);
- removes the package (`apt-get remove`; `--purge` for `apt-get purge`);
- removes your user from the groups **the installer added** (not the ones you already had), turns
  off the linger it turned on, deletes `/etc/lordos/session.conf` and the `lordos-greeter` user if
  it created them;
- removes the remote desktop configuration (`/etc/lordos/remote/config`), if there is one.

Your home folder and `~/.config/lordos` are kept. The dependencies apt pulled in stay installed;
`sudo apt autoremove` removes them. Reboot to get your previous login screen back.

### Switching back without uninstalling

```sh
sudo systemctl disable lordos.service
sudo systemctl unmask getty@tty1.service
sudo systemctl enable gdm3          # or sddm, lightdm…
sudo reboot
```

## Troubleshooting

| Symptom | What to do |
|---|---|
| `no terminal to ask "Continue?"` | The script runs without a terminal (CI, `ssh host 'curl … \| sh'`). Use `ssh -t`, or `--yes`. |
| `could not find the latest LordOS release on GitHub` | No connection to `api.github.com`, the GitHub API rate limit (60 requests/hour per IP), or the repository is still private. Pass `--version 0.8.0-beta`, or download the files and use `--deb`. |
| `SHA-256 mismatch` | The download is corrupted or incomplete. Run it again; if it persists, report it: nothing was installed. |
| `the SHA256SUMS signature is NOT valid` | Don't install; report it (the release files may have been tampered with). |
| `apt-get could not install LordOS` | Usually a broken apt state or mirrors that aren't trixie. `sudo apt-get update && sudo apt-get -f install`, then run the installer again. |
| `LordOS needs Debian 13 (trixie)` | Debian 12 and testing/unstable are not supported: the package depends on trixie's library versions. |
| Black screen or back to text after the reboot | Switch to tty2 (Ctrl+Alt+F2), log in, and look at `journalctl -b -u lordos`. To go back to your desktop meanwhile, see "Switching back" above. |
| The previous desktop still appears | The display manager was kept (`--keep-display-manager` or "no" to the question). Disable it as shown in the final message. |
| No sound | Log out and in again (the new groups and linger apply on the next login), and check `systemctl --user status pipewire wireplumber`. |
| Network changed | It shouldn't: interfaces managed by NetworkManager, ifupdown or systemd-networkd are left alone. If an interface nobody managed now has a DHCP address, that is LordOS's DHCP (`NETWORK.md`); Settings → Network turns it off. |

## For maintainers

- **The canonical copy is `tools/install/install.sh`.** The website serves it at
  `https://lordos.org/install.sh`, and **that file must stay byte-identical to the one in the
  release tag** (`git show v0.8.0-beta:tools/install/install.sh | sha256sum` against
  `curl -fsSL https://lordos.org/install.sh | sha256sum`). Change the script here, tag, then copy it
  to the website, never the other way round.
- POSIX `sh` with `set -eu`; everything is inside `main()`, called on the last line, so a truncated
  download runs nothing. Commands that could read standard input (apt, dpkg) get `</dev/null`,
  because under `curl | sh` standard input is the script.
- `shellcheck -s sh tools/install/install.sh` must be clean (also `-s dash`, Debian's `/bin/sh`).
- Release layout the script expects: tag `vX`, assets `SHA256SUMS` (`sha256sum` format), the
  `lordos_<version>_<arch>.deb` files listed in it (the version inside the name doesn't have to match
  the tag), and `SHA256SUMS.sig` (required: without it the script stops).

### Testing

Test-only environment variables:

- `LORDOS_BASE_URL`: where the release files are, instead of
  `https://github.com/lordbasex/LordOS/releases/download/<tag>` (for example `file:///srv/rel`; use it
  with `--version` so the GitHub API is not asked). It lifts the https-only restriction.
- `LORDOS_PUBKEY_FILE`: a PEM public key used instead of `RELEASE_PUBKEY_PEM`, to exercise the
  signature check with a throwaway key.

Done on 2026-10-18 on the development Mac with Docker (`debian:trixie`, amd64) and the
`lordos_0.7.0_amd64.deb` built on VM 116 (sha256 `4684f1e9…147ffee`):

| Test | Result |
|---|---|
| `cat install.sh \| sh -s -- --yes --deb …` as root, no systemd | OK: apt pulled the dependencies; greeter user, groups, locales, `session.conf`; systemd steps skipped with a warning |
| The same as a sudo user (`bob`), with the user detected from `sudo` | OK |
| Download from a local release (`LORDOS_BASE_URL=file://…`, `--version 0.7.0-beta`) | OK; the output says the signature was not checked |
| Signature with a throwaway key: valid / signed by another key / no `.sig` | installs / stops before downloading the `.deb` / stops |
| `SHA256SUMS` with a wrong hash | stops: "SHA-256 mismatch", nothing installed |
| `--deb` with a `SHA256SUMS` next to it | checked |
| Latest release through the GitHub API (repository private) | clear error suggesting `--version`/`--deb` |
| Debian 12 `os-release` / a trixie-based derivative | refused / warning and continues |
| A `.deb` for the other architecture, an unknown option, no terminal without `--yes` | refused with a clear message |
| systemd as PID 1 in a privileged container, with a fake `lightdm.service` as `display-manager.service` | lightdm disabled, `lordos`/`lord-netperm`/`lord-fand` enabled, `getty@tty1` masked, linger on; `--uninstall` re-enabled lightdm (alias restored), unmasked getty, turned linger off, removed groups and the greeter user |
| The same with answers typed on a pseudo-terminal while the script comes from a pipe | "no" to the display manager leaves it enabled and `lordos.service` disabled; "no" to "Continue?" changes nothing |
| No display manager | `lordos` enabled, `getty@tty1` masked, the final message offers `systemctl start lordos` |
| `--uninstall --purge` with the state file deleted | still finds and re-enables lightdm, unmasks getty, keeps the greeter user (not created by the installer) |
| `shellcheck -s sh` and `-s dash` | clean |
| `--remote-desktop` (0.8.0, systemd in a privileged `debian:trixie` container, no display) | config written in the Settings → Sharing format, `lordos.service` started headless, port 5673 listening, login page served and a login accepted; `--remote-port 8443` while running moves it (reload); a bad port or `--remote-port` alone is refused; "no" to the question keeps the install and leaves it off; `--uninstall` removes the config and closes the port |
| Release files signed with the real key (`LORDOS_BASE_URL=file://…`, `--version 0.8.0-beta --remote-desktop`) | "valid Ed25519 signature", SHA-256 OK, installed, remote desktop listening |
| `--remote-desktop` on VM 120 (LordOS installed from the ISO) | reload path: port 5673 listening, login from another machine accepted |

### Still to test on a real Debian 13 machine

No free Debian 13 VM was available (the ISO work uses VMs 116, 117 and 120). Manual plan, on a fresh
Debian 13 VM with a desktop (GNOME, which brings GDM and NetworkManager) and one without (server
install, ifupdown):

1. `curl -fsSL https://lordos.org/install.sh | sh` (or `sh install.sh --deb lordos_…_amd64.deb`
   from a copy) as a sudo user, from inside the GNOME session. Answer "y" to both questions. The
   running GNOME session must keep working until the reboot.
2. `sudo reboot`: the LordOS login screen appears on tty1; log in with your Debian password. Check:
   sound (Settings → Sound, `pw-cli info 0`), network unchanged (`nmcli device` still shows
   NetworkManager managing the interface; no second DHCP client: `ps aux | grep -i dhcp`), Settings →
   Sharing shows remote desktop off, `ss -ltn | grep 5673` is empty, Ctrl+Alt+F2 gives a text login.
3. `systemctl status lordos lord-netperm lord-fand` are active; `journalctl -b -u lordos` has no errors.
4. On the server install: run the installer from tty1 over the console, then `sudo systemctl start
   lordos` without rebooting; then reboot and check ifupdown still owns `eth0`.
5. `curl … | sh -s -- --uninstall`, reboot: GDM comes back with GNOME; on the server, the text login
   on tty1.
6. Repeat 1–5 on arm64 (a Raspberry Pi 5 with Raspberry Pi OS trixie, or an arm64 VM), where the
   installer must warn that the image is the recommended path.
