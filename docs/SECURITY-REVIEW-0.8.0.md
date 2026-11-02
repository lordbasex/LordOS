# Security review before 0.8.0-beta

An adversarial review of everything that changed since the 0.7.0 release commit
(`389abc94e0821d2f1f7e611fc6ceed05ea8843c0..847914f`, about 40 commits). It was done on
2026-10-18 and checked on VM 118.

The attackers considered:
- a user with a normal account;
- a malicious `.lpk` package;
- a malicious Linux app or `.desktop` file, restricted with any of the firewall's restrictions;
- crafted DNS answers;
- a hostile release mirror.

Every finding is listed below with its status. Findings rated high or medium are fixed; the
low-rated ones are fixed where that was cheap and listed otherwise.

Tests:
- `make -C server test` stays green with 0 warnings;
- `npverify-test` loads every BPF program into the running 6.12 kernel (38/38);
- the end-to-end scripts in `server/tests/fwlinux/` are listed per finding.

**Severity:** critical, high, medium, low, info.

## Findings

| # | Severity | Area | Finding | Status | Commit |
|---|---|---|---|---|---|
| 1 | High | Launcher (`apps.c`, `sec/sandbox.c`) | The working directory was entered before the tmpfs went over the home and runtime folders, so it still pointed at the real ones. A `.desktop` with `Path=/run/user/<uid>` reached the session bus by a relative `connect("bus")`, then `systemd-run --user` ran anything outside the sandbox. Without `Path=`, sockets in the home were reachable (ssh ControlMaster, gpg-agent). Reproduced on VM 118 against 0.7.0+main. | Fixed: the directory is entered again inside the view (`cfg.cwd`, else the home). Test: `files.sh` `cwd_relative_bus`. | 594399c |
| 2 | High | Launcher, cgroups | `app_scope_enter()` returned void, and the launcher ran the app even when placing it in its scope failed. That happened for ids with `+` or over about 62 characters, ids with `..`, and when lord-sessiond was unreachable. The app then ran in axia-server's own leaf as `lordos`: no network rules, no device, Bluetooth, PipeWire or notification restrictions. An `lpk-<x>` desktop id also took the `lpk:<x>` identity. | Fixed: desktop ids are limited to `[A-Za-z0-9._-]`, at most 62 characters, no `..` and no `lpk-` prefix. `app_scope_enter` returns int, and inside a LordOS session a failed placement never execs (`_exit(126)`). | 594399c |
| 3 | High | PipeWire guard (`sec/pwguard.c`) | It failed open in three cases: a full client table (256) left a new client untracked, so its stream was never destroyed; a full node table (1024) let a stream go unseen; and a permission list over 16 KB was logged and never sent, leaving the client every permission. | Fixed: a client the guard can't follow is disconnected, an unstorable capture stream or node of a restricted or unknown owner is destroyed, and permission lists go in chunks of 400 entries (PipeWire merges them). | c1f12c7 |
| 4 | High | PipeWire guard and pipewire-pulse | A restricted app (no-mic, even with no-files) could load `module-loopback` from the microphone into a pipe or a TCP server through pipewire-pulse. The module's stream belongs to the pipewire-pulse daemon, which counted as unrestricted. Reproduced on VM 118. Modules could also open network connections (tunnel, RTP) or create a FIFO at any path of the home. | Fixed: module loading is off (`/etc/pipewire/pipewire-pulse.conf.d/lordos-no-modules.conf`, shipped by the package and the ISO). The daemon's own capture streams are closed while a restricted app runs. Test: `mic.sh` 8. | c1f12c7, 81e1398 |
| 5 | High | Session-side restrictions | `kernel.yama.ptrace_scope` was 0 (Debian's default), so a Linux app restricted without no-files could `ptrace` axia-server, PipeWire or another app of the same user and act through it, defeating every user-space restriction and the network rules. | Fixed: the package ships `/usr/lib/sysctl.d/50-lordos-ptrace.conf` (`kernel.yama.ptrace_scope = 1`) and applies it in postinst. Only descendants can be traced, and `/proc` reads (Monitor, the guard) are unaffected. | fb02854 |
| 6 | Medium | Launcher | An "Open with" folder grant could include protected folders below it. Opening `~/.config` or `~/.local/share` read-write handed the app `autostart/`, `firewall.json`, `applications/` and `dbus-1/`. Reproduced on VM 118. | Fixed: no writable grant on the home itself, or on a folder that contains or lies inside `.ssh`, `.gnupg`, `systemd`, `dbus-1`, `environment.d`, `user-tmpfiles.d`, `~/.local/bin`, `~/bin`, `autostart`, `applications` or the `lordos` folders. More startup files are added (`.bash_aliases`, `.bash_logout`, `.zlogout`, `.xinitrc`, `.Xresources`), and a hard-linked file is granted read-only. Test: `files.sh` 3b. | 594399c |
| 7 | Medium | Launcher | Landlock grants `/dev` read-write, and `/dev/shm` and `/dev/pts` were the session's: other apps' shared memory and the user's terminals (keystrokes, a sudo password). | Fixed: a new IPC namespace, a private tmpfs on `/dev/shm`, and a new devpts instance with `/dev/ptmx` bound to it. Each fails closed. Test: `files.sh` `read_session_shm`, `session_pts_hidden`. | 594399c |
| 8 | Medium | Launcher | Shared caches (fontconfig, Mesa and radv shader caches, the GStreamer registry) were writable. A restricted app could poison what unrestricted apps load as trusted. | Fixed: read-only. Test: `files.sh` `write_shared_cache`. | 594399c |
| 9 | Medium | Launcher | The own-folder deny list missed folders other programs execute from: `dbus-1` (service activation), `user-tmpfiles.d`, terminals', browsers' and editors' config. | Fixed: the list is extended. | 594399c |
| 10 | Medium | Launcher | Time-of-check to time-of-use on "Open with": the path was checked with `realpath()` in axia-server but bound (and given to Landlock) later by path, following symlinks. A running instance could swap a component for a symlink to `~/.ssh`. | Fixed: the opened file is reopened with `openat2(RESOLVE_NO_SYMLINKS)` and must still be the inode that was checked (dev and inode), both for the bind and for the Landlock rule (also on `/media`). Otherwise the grant is dropped. | 594399c |
| 11 | Medium | Launcher | Without `XDG_RUNTIME_DIR` no tmpfs went over the runtime dir, so the bus stayed reachable. | Fixed: it falls back to `/run/user/<uid>`, or refuses to launch. | 594399c |
| 12 | Medium | axia-netperm (root) | No-mic denied only the ALSA capture minors seen at the last scan (every 5 s). A USB microphone plugged in was open to a restricted app in between, and the app keeps the fd afterwards. | Fixed: in the app's own scope every ALSA minor not classified as non-capture is denied (fail closed; `np_snd` classifies every node). Test: `mic.sh` 6, with an unknown minor. | bd09ae7 |
| 13 | High | axia-netperm, `user@` union, launcher | The union's no-mic denied the capture nodes to the user's own PipeWire and WirePlumber: while any no-mic app ran, the microphone disappeared for the whole desktop and did not come back (reproduced on VM 118). Taking no-mic out of the union reopened the escape (a no-mic app without no-files could `systemd-run --user` and open `/dev/snd` directly). Two exemptions for "the real sound services" were tried and rejected (see "Design change" below). | Fixed by design: no process under `user@` is exempted for who it is. Every app restricted with no-mic, no-camera, no-usb or no-bluetooth is started in its own view (no session bus, no user manager, Landlock scoping), so it can't escape its scope, and no-mic stays out of the union. Tests: `files.sh` 3c (for each of the four restrictions: no bus, no `systemd/private`, no `systemd-run`, home kept, PipeWire reachable), `mic.sh` 6 (ALSA capture → EPERM in the scope) and "once lifted, the desktop still records". | 00b2c95, see below |
| 14 | Medium | PipeWire guard | A process escaped to `user@` (`systemd-run --user pw-record`) was not restricted. | Mitigated: escape is closed for launcher-started apps (#13). While a restricted app runs, the guard also holds `user@` clients as restricted, except PipeWire, WirePlumber and pipewire-pulse (exact unit path, alone in it, root-owned binary). That exemption is best effort: it decides only what the guard hides, never a kernel decision. Test: `mic.sh` 9. | c1f12c7, 99a0c7d |
| 15 | Medium | PipeWire guard (reported by the automated reviewer) | The first fix for #14, and the existing pipewire-pulse check, matched the cgroup path by suffix. The user owns `user@`, so a nested `…/x.scope/session.slice/pipewire.service` posed as the sound server. Reproduced on VM 118 against the suffix version (it recorded 286 KB). | Fixed: exact path built from the uid, sole process in the unit, root-owned executable (same inode). Test: `mic.sh` 9 (nested fake pipewire and wireplumber units). Other cgroup matching was checked: `appid.c` (now anchored, #18), netperm `scope_app` (reads only root-owned directories), `cgroup.c` (exact `strcmp`). | 99a0c7d |
| 16 | Medium | PipeWire guard | The pulse identity check counted connections but didn't tie them to the claimant. A process in another PID namespace (a no-files app) reports its inner PID, which left slack that a no-mic app could claim with the outer PID. | Fixed: a claimed PID must live in our PID namespace (`NSpid`). Test: `mic.sh` 7 (claim of a namespaced recorder's outer PID). | c1f12c7 |
| 17 | Medium | lord-sessiond (root), pre-existing | On a switch the leaving session's framebuffers stayed alive until it handled RP_UNDISPLAY; a stuck or hostile one never does. The next DRM master can read them (`GETFB2` + map): the last frame of another user's unlocked desktop. The new keep path made the window longer. | Fixed: `take_back()` removes every framebuffer of the lent file (`GETRESOURCES` + `RMFB`) before `DROP_MASTER`. | 913113f |
| 18 | Low | `sec/appid.c` | The match on `/lordos-session-` was not anchored. A cgroup the user creates under `user@` with the same names posed as an app (attribution only). | Fixed: only `/system.slice/lordos.service/lordos-session-<own uid>/`. | c1f12c7 |
| 19 | Low | lord-sessiond | Keep (leave the boot splash) could be sent again on a later lend if RP_SHOWN was lost, so the previous picture stayed until the new session drew. | Fixed: keep only on the first lend of the process; a failure to create `/run/lordos/first-frame` is logged. | 913113f |
| 20 | Low–Medium | `tools/install/install.sh` | The verified `.deb` stayed in a user-owned folder until `sudo apt-get install` ran. Another process of the user could swap it in between, and root would run its maintainer scripts. | Fixed: the package is copied into a root-owned folder, checked again there (SHA-256, package fields) and installed from there. | 6e2b66f |
| 21 | Low | `install.sh` | An empty or non-PEM key override (`LORDOS_PUBKEY_FILE`) skipped the signature with a warning. | Fixed: there is no skip path; a missing or bad key is an error. | 6e2b66f |
| 22 | Low | `install.sh` | An old, validly signed release served as "latest" could downgrade an install. | Fixed: refuses to downgrade unless `--version` is given. A signed version line in SHA256SUMS would be stronger (later). | 6e2b66f |
| 23 | Low | `tools/web/deploy.sh` | The release key was uploaded without being compared with the repository's copy. | Fixed. | 6e2b66f |
| 24 | Low | `tools/release/sign-release.sh` | The file list is word-split. | Fixed: unexpected file names are refused. | 6e2b66f |
| 25 | Low | `hw/notifyd.c` | If asking the bus who sent a Notify failed, the notification was shown (fail open). | Fixed: dropped. | c1f12c7 |
| 26 | Low | Website | Table alignment used inline `style=` (dropped by a CSP without `unsafe-inline`); the team LinkedIn link accepted any scheme. | Fixed: classes; `https://` only. | 0c6a344 |
| 27 | Low | axia-netperm | `RELOAD` can be sent in a loop by any local user (CPU only: 4 connections per uid, each reload rereads a file of up to 4 MB). | Open: rate-limit per uid. | — |
| 28 | Low | axia-netperm | The hidraw major is read once at startup. If hidraw is a module loaded later, no-usb misses `/dev/hidraw*`. Debian builds it in (`CONFIG_HIDRAW=y`). | Open. | — |
| 29 | Low | lord-sessiond | The session socket's peer isn't checked to be `axia-server` before a lend. The framebuffer leak it enabled is closed by #17. | Open: check `SO_PEERCRED` pid's `/proc/<pid>/exe`. | — |
| 30 | Low | notifyd | `CloseNotification` and `replaces` act on any id, so one app can close or replace another's banner. App name and desktop-entry are the caller's word. | Open: record the owner of each id. | — |
| 31 | Low | PipeWire guard | The manager socket's peer isn't verified, so an unsandboxed app of the user could replace `pipewire-0-manager`. This is the same class as "unsandboxed apps can edit firewall.json". | Open. | — |
| 32 | Low | Restrictions at startup | `fwNotifyBlocked` in `sdk/notify.js` returns false on an exception. The rule store is NULL for a moment before `js_fw_register`. A corrupt `firewall.json` starts with no rules. | Open: fail closed for apps that were restricted. | — |
| 33 | Low | Launcher | The abstract unix socket scoping (`LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET`) depends on the build headers. It is present on Debian 13. | Open. | — |
| 34 | Low | Launcher | `/run/dbus` (system bus), `/run/cups` and `/run/lordos/*.sock` stay reachable from a no-files view. Each was checked: `cgroup.sock` requires the session leaf and a trusted binary, `netperm.sock` takes only RELOAD, `fand.sock` gives STATUS only. | Accepted. | — |
| 35 | Info | PipeWire guard | A new client has full permissions for milliseconds before the first `update_permissions`, and a stream can get a few milliseconds of audio before it is destroyed. | Documented (FIREWALL.md). | — |
| 36 | Info | Escapes | An app not started from the launcher, or already running when it was restricted, may have escaped to `user@`; the ALSA capture nodes stay open there. An app restricted without no-files can write its own home (startup files, user units, `firewall.json`), which takes effect later, outside the restriction. | Documented (FIREWALL.md); Settings says "restart the app". | — |
| 37 | High | axia-netperm (reported by the automated reviewer) | The `np_sndsvc` exemption (the exact unit cgroup, its sole process's tgid, the packaged binary, no user unit files or drop-ins) was still forgeable. The user controls their own manager: `systemd-run --user --unit=pipewire.service` passed the file checks (reproduced on VM 118: `sndsvc.sh` 3 and 4 got the exemption), and so would type-wide `service.d` drop-ins, `set-environment` (`LD_PRELOAD`) and PipeWire's or WirePlumber's own config (modules are code). | Removed: replaced by the design in #13. The intermediate commits 30d4e8a and the uncommitted hardening are superseded. | see below |
| 38 | Medium | `sec/sandbox.c` (reported by the automated reviewer) | Landlock's abstract unix socket scoping failed open: with ABI < 6, or built without the scoping constants, the files sandbox went on without it, so a restricted app could reach the session's abstract sockets. | Fixed: both the files sandbox and the device view require ABI 6, otherwise the app isn't started. The launcher checks this before forking and reports `apps.restrictKernel` (a localized notification). The read-only remount of a read-only grant now fails closed too. Test: `files.sh` 3d (`LORD_SANDBOX_MAX_ABI=5`, which can only lower the ABI). | see below |

## Design change: no exemption by identity under `user@`

The user owns their systemd user manager, so anything that runs in `user@<uid>.service` can be
made to look like anything:
- a nested cgroup with the same names;
- a process moved into the real unit's cgroup;
- a unit file, drop-in or type-wide `service.d` drop-in;
- a transient unit with the same name (`systemd-run --user --unit=pipewire.service`);
- the manager's environment (`set-environment`, `environment.d`, `LD_PRELOAD`);
- PipeWire's and WirePlumber's own config, which loads modules.

Two attempts to exempt the user's real PipeWire from the union's no-mic were rejected:
1. matching by cgroup path suffix (#15);
2. the exact cgroup, its sole process's tgid, the packaged binary's inode and the absence of user
   unit files (#37).

Both were beaten on VM 118. The design now:
1. **No process under `user@` is trusted by axia-netperm** for who or what it claims to be.
2. **The escape is closed at the source.** A Linux app restricted with no-files, no-mic,
   no-camera, no-usb or no-bluetooth is started by the launcher in its own view:
   - user, mount, PID and IPC namespaces;
   - a runtime dir with only the Wayland socket, `pipewire-0` and `pulse/native`, so no session
     bus, no `systemd/private` and no agents;
   - private `/tmp`, `/var/tmp`, `/dev/shm` and devpts;
   - Landlock scoping of abstract sockets and signals.

   Only no-files also hides the home folder. Such an app can't create or change a unit, or start
   anything outside its scope, so the device program and the PipeWire guard always find it by its
   scope.
3. **No-mic stays out of the `user@` union.** The union keeps covering the network and the other
   devices for processes that escaped earlier.
4. **The cost:** these apps lose the session bus (notifications, portals). A restriction set while
   the app runs needs a restart to close the escape (the Settings note says so).

## Checked and clean

**axia-netperm (root):**
- **BTF reader (`bpfs_btf_func_id`):**
  - header fields are checked without overflow, and the loop requires `pos + 12 <= len`;
  - only the 12-byte headers are read, and `12 * vlen` can't overflow;
  - string offsets are checked before comparing, and unknown kinds abort;
  - the read is capped at 64 MB, and a big-endian kernel fails safe.
- **`/proc/devices` and `/sys/class/sound`:** fixed buffers, bounded `sscanf` (`%63s`), only the character-device section, NUL-terminated reads, and `O_NOFOLLOW` on the sound `dev` files.
- **Device program:** only the listed majors, only for an app with that restriction, and never `lordos`.
- **Bluetooth LSM program:** never touches kernel sockets.
- **Host names in notices:** they come from the normalized DNS map (`[a-z0-9-_.]`, at most 253 characters). `json_quote` escapes the quote, the backslash and control characters, and the buffers hold the worst case.
- **`firewall.json` read:** fd-relative, `O_NOFOLLOW`, an owner check at each step, a regular file of at most 4 MB.
- **RELOAD-only socket:** any other frame drops the client. The peer comes from `SO_PEERCRED`, with a limit per uid.

**lord-sessiond:**
- `RP_SHOWN` (7) collides with no other type; the flags byte is read only when present; `mirror_shown` goes to seat clients only.
- `/run/lordos/first-frame` is created with `O_CREAT|O_NOFOLLOW`, no truncation, 0644, in a root-owned 0755 folder. A session that fakes `RP_SHOWN` only ends the splash early.
- Plymouth script, `lordos-splash`, and the quit and plymouth-quit units: fixed paths, bounded waits, no user input.

**Launcher:**
- **Ids:** taken only from the file name; never from `Exec`, `Name` or `StartupWMClass`.
- **Namespaces and mounts:** `MS_REC|MS_PRIVATE`; `OPEN_TREE_CLONE` without `AT_RECURSIVE`; nosuid and nodev tmpfs; a fresh procfs; identity uid and gid maps with setgroups denied; mounts are locked in the less privileged namespace.
- **Landlock:** blocks ptrace of PID 1.
- **Fail closed:** every error path refuses to exec.
- **ABI handling:** REFER is unhandled under ABI 1 and TRUNCATE is handled from ABI 3.

**PipeWire guard:**
- POD parsing (`pod_at`, `pwpod_struct_field`, alignment), message size caps, SCM_RIGHTS fds closed, `dict_get` capped at 4096 entries, and the `sock_diag` iteration bounded.

**notifyd:**
- Only replies from `org.freedesktop.DBus` are matched, by serial. Pending table of 32, with a 3 s timeout. Bounded markup stripping and JSON.

**Broker:**
- `restrict.c`, `brokersrv.c`, `brokercam.c`: the restriction is checked before the permission on every new path, revoking closes running devices, and the id is the verified package id.

**`js_fw.c`:**
- `updateRule` and `ruleApps`: every field is validated (app ids, destinations, ports, enums, restriction names), the old rule is copied before any JS getter runs, and frame lengths are bounded. `lord.fw` exists only in the theme context.

**Installer:**
- The signature is checked before anything in SHA256SUMS is used, and HTTPS with TLS 1.2 or later is enforced (redirects too).
- Quoting is correct, and the GRUB drop-in has a fixed path and fixed content, removed on failure and on uninstall.

**Release tooling:**
- No `set -x`. The private key is never printed or copied, and the signature is verified against the public key.
- `make-sbom.sh` uses `mktemp` with a trap, `python3 -I` and data in environment variables.
- `build-kernel.sh` is unchanged in this range.

**Website:**
- `md.mjs` passes no raw HTML, escapes attributes and allow-lists link schemes.
- `diagram.mjs` escapes every field.
- The FAQ's ld+json escapes `<`, and the post-build check refuses inline scripts and `on*=` attributes.
- `site.js` uses no `innerHTML`.
- `deploy.sh` is strict and quoted, and leaves the key and `install.sh` alone on `--delete`.

## Not exercised on the VM

- **The framebuffer removal on a real user switch (#17).** The new lord-sessiond was installed on VM 118 and the greeter's first-frame hand-off works ("first frame on the monitor"), but no second user session was switched to.
- **Restarting `lordos.service` left the old greeter process running** until it was killed (systemd: "remains running after unit stopped"). The unit is unchanged in this review, and whether 0.7.0 does the same was not checked. It needs a look before the ISO; the new sessiond lent the monitor only to its own new greeter.
