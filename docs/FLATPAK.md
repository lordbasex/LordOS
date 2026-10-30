# Flatpak and Flathub in LordOS: design review

**Status (2026-10-27): design review kept for the future; not planned for any release.** On 2026-10-27 the
maintainer decided not to implement Flatpak for now, to keep the system stable and avoid the complexity and
future problems it would add; this document stays as the reference if that is ever reconsidered. No code has
been written and nothing below is built. The roadmap item ([ROADMAP.md](ROADMAP.md), "Flatpak and Flathub (future, undecided; not planned for
any release)"; when it was written it was planned for 0.8.6) asks for a Flathub section in the
Store without giving up LordOS's security model. This document goes through the four problems the
roadmap lists, gives the options for each with their trade-offs against `AGENTS.md`, recommends
one, and lists exactly which new exceptions the maintainer has to approve before any code is
written. Every statement is marked as one of:

- **Checked** — read on 2026-10-27 from Flatpak's source at the tag Debian 13 ships (1.16.6), from
  Debian 13's package metadata, from Flathub's live repository, or from LordOS's own code and docs.
  Not run.
- **Design** — a proposal, not built.
- **Unverified** — an assumption that the test plan has to confirm on a VM before it can be claimed.

Nothing here has been run on a VM. No compatibility with any Flathub app is claimed.

## Summary

**Recommendation:**

- **Flatpak stays optional.** The Debian `flatpak` package is installed only when the user turns
  Flathub on in the Store, through the existing package engine (`pkg-install`, without Recommends),
  like Chrome or Spotify. LordOS never links libflatpak, libostree or bubblewrap. Its own isolation
  stays its own code: the root-owned app scope, `lord-netperm`'s kernel programs and the launcher's
  view. Flatpak's sandbox (bubblewrap) is an extra layer inside it, never the only one.
- **Identity:** a Flatpak app is started only by LordOS's launcher, in a root-owned
  `app-lordos-fp-<id>.scope`, which the firewall, the Monitor, the PipeWire guard and the
  notification server read as the app `flatpak:<id>`. `flatpak run` can't move itself out (its
  transient-scope call needs `/run/user/<uid>/systemd/private`, which the launcher's view doesn't
  contain; the call failing is not fatal in Flatpak). No identity is ever taken from a unit name
  under `user@`.
- **Launching:** the launcher never runs the `Exec` line of a Flatpak export. It takes a validated
  Flatpak app id of an installed app and builds the `flatpak run` command line itself in C: a fixed
  set of options plus LordOS's restrictions and a fixed list of things LordOS never grants. The
  command-line options win over the app's metadata and over the user's override files.
- **Permissions:** the Store shows what the app declares (its `[Context]`) and what LordOS changes.
  LordOS's existing restrictions (Settings → Privacy) apply to Flatpak apps through the same
  kernel paths as to Linux apps. No portal beyond the Settings portal LordOS already serves in
  0.8.6; a LordOS portal is a later phase with its own review.
- **Installing:** per user (`flatpak --user`), no root and no new admin action. The Flathub remote
  comes from a `.flatpakrepo` file LordOS ships (its GPG key pinned), the catalog is Flathub's
  signed appstream data that `flatpak` itself verifies, and every `flatpak` command lord-server runs
  is one of a closed list, run in its own firewall component `lordos:flathub`.
- **Seven approvals are needed** (see "Exceptions to approve"): the Flatpak launch path, the
  `flatpak:<id>` identity and scope name, the closed list of `flatpak` management commands with
  network, the dependency-policy wording, masking two Flatpak services, the `lord.fs` deny list,
  and a sandboxed parser for the catalog.

The largest limits, said up front: apps that start sub-sandboxes through Flatpak's portal (most
Chromium/Electron apps, through zypak) would lose those processes' per-app identity and probably
don't work in phase 1; file chooser dialogs show only what the app's sandbox already sees; and a
device-restricted Flatpak app has no D-Bus at all, as Linux apps don't today.

## What Debian 13 ships (Checked)

| Package | Version in trixie (sources.debian.org, 2026-10-27) | Notes |
|---|---|---|
| `flatpak` | 1.16.6-1~deb13u2 | Depends: `bubblewrap (>= 0.10)`, `fuse3`, `xdg-dbus-proxy`, `adduser` and its libraries (libostree among them). Recommends: `xdg-desktop-portal`, `xdg-desktop-portal-gtk`, `polkitd`, `libpam-systemd`… |
| `bubblewrap` | 0.12.0-1~deb13u1 | **not setuid**: Debian's README says the setuid mode "is no longer possible"; it needs unprivileged user namespaces (`kernel.unprivileged_userns_clone`, 1 by default) |

The `flatpak` package also installs (file list from packages.debian.org):

- `/usr/libexec/flatpak-session-helper` (`org.freedesktop.Flatpak` on the session bus, user unit
  `flatpak-session-helper.service`): host command execution (`flatpak-spawn --host`) for apps
  allowed to talk to that name, and the `/run/host/monitor` files;
- `/usr/libexec/flatpak-portal` (`org.freedesktop.portal.Flatpak`, user unit
  `flatpak-portal.service`): sub-sandboxes (`flatpak-spawn`, used by zypak for Chromium/Electron);
- `/usr/libexec/flatpak-system-helper` (system bus, `flatpak-system-helper.service`, root, polkit):
  system-wide installs;
- `/etc/profile.d/flatpak.sh`, `/etc/X11/Xsession.d/20flatpak` and
  `/usr/lib/systemd/user-environment-generators/60-flatpak`: add the export folders to
  `XDG_DATA_DIRS`. LordOS's launcher reads two fixed folders and ignores `XDG_DATA_DIRS`, so these
  change nothing there.

LordOS's package engine installs Recommends only when asked (`pkg-install … --recommends`,
[PACKAGES.md](PACKAGES.md)), so turning Flathub on would not pull `xdg-desktop-portal` in. That
matters: lord-server already owns `org.freedesktop.portal.Desktop` on the session bus for the
Settings portal (`server/src/ui/portal.c`), and `tests/fwlinux/screen.sh` depends on no screen-cast
portal being installed.

## Problem 1: identity and the firewall

### Facts

- **Checked (flatpak 1.16.6, `common/flatpak-run.c`, `flatpak_run_in_transient_unit`).** Before it
  starts the D-Bus proxy, `flatpak run` connects to `/run/user/<getuid()>/systemd/private` (the path
  is built from the uid, not from `XDG_RUNTIME_DIR`) and calls `StartTransientUnit` for
  `app-flatpak-<escaped id>-<instance id>.scope` with its own pid. If the socket isn't there or the
  call fails, it logs "Failed to run in transient scope" at info level and **goes on**: "nothing
  really depends on it".
- **Checked (LordOS).** The user manager has PID 1 do that move, which only checks the uid
  ([FIREWALL.md](FIREWALL.md), "Escape via `systemd-run --user`"). So a `flatpak run` started inside
  `app-lordos-<id>.scope` would leave it for `user@<uid>.service/app.slice/…`. There
  `lord-netperm` finds no app scope and applies the per-user union: restriction-only, so not a
  hole, but the app loses its own rules, its counters in the Monitor and its device restrictions,
  and its traffic isn't attributed.
- **Checked (flatpak-portal.c).** `flatpak-portal` (D-Bus activated, under `user@`) runs
  sub-sandboxes as its own children with `flatpak run --parent-pid=…`; they land under `user@` the
  same way, whatever LordOS does with the parent.
- **Checked (flatpak-run.c).** bubblewrap is started with `--unshare-user --disable-userns` (when
  unprivileged) and `--unshare-pid`. A process's cgroup doesn't change across namespaces, so
  `lord-netperm`'s programs (`bpf_get_current_ancestor_cgroup_id`) and `sec/appid.c` find a
  Flatpak app by its scope exactly as they find a Linux app, as long as it stays in it.

### Options

**A. Let Flatpak make its scope and recognise `app-flatpak-<id>-*.scope` under `user@` as
`flatpak:<id>`.** No launcher change. *Rejected:* it is an exemption by identity under `user@`,
which `AGENTS.md` forbids. The user (and any unsandboxed app of theirs) controls that manager:
`systemd-run --user --scope --unit=app-flatpak-org.mozilla.firefox-1.scope <cmd>` would give any
process another app's rules and take it out of the union. Two such attempts were already beaten
(`docs/SECURITY-REVIEW-0.8.0.md` #15 and #37).

**B. Start Flatpak apps in a root-owned LordOS scope, in a view without the systemd user socket.**
The launcher's child enters `app-lordos-fp-<id>.scope` through `lord-sessiond`'s placement service
(as every Linux app does today), then builds a view with LordOS's own code
(`sandbox_files_isolate`: user, mount, PID and IPC namespaces) whose runtime folder contains only
the sockets Flatpak is meant to use, never `systemd/`. `flatpak run`'s transient-scope call fails,
and the app, its D-Bus proxy and every process it forks stay in the root-owned scope, where the
user can't move them (every `cgroup.procs` there is root's). *Trade-offs:* every Flatpak launch
needs unprivileged namespaces and the view (the launcher already requires them for restricted
Linux apps); bubblewrap then nests its own user namespace inside LordOS's (allowed by the kernel,
**Unverified** under LordOS's view); a private runtime folder per launch means Flatpak's per-app
`$XDG_RUNTIME_DIR/.flatpak/<id>` state (shared `/tmp` between instances, `flatpak ps`) isn't seen
from outside, unless the launcher binds a per-app folder into the view (open question 4).

**C. Let Flatpak move the app, then have root move it back.** A new request to `lord-sessiond`'s
placement service ("reclaim this pid into that scope"), checked against the pid lord-server
started. *Rejected:* `AGENTS.md` says not to add requests to that service or place arbitrary
pids, and the app runs under `user@` until it is moved back.

**Recommendation: B.** The firewall id is `flatpak:<id>`; `fw_app_canon`, `lord-netperm`'s scope
scan (`netperm.c`), `sec/appid.c`, the PipeWire guard (`sec/pwguard.c`), the Monitor
(`ui/js_monitor.c`) and `sec/restrict.c` learn the `app-lordos-fp-` prefix the way they learned
`app-lordos-lpk-`. The launcher's `valid_id` already refuses desktop ids starting with `lpk-`; it
would refuse `fp-` too, so no `.desktop` can take a Flatpak app's identity.

The view (Design) for a Flatpak app:

| Item | Unrestricted Flatpak app | Device-restricted (no-mic, no-camera, no-usb, no-bluetooth) | no-files |
|---|---|---|---|
| runtime folder | Wayland socket, `pipewire-0`, `pulse/native`, the session `bus` | Wayland, `pipewire-0`, `pulse/native` (no bus, as Linux apps) | same as device-restricted |
| `systemd/`, other agents | never | never | never |
| home | as is (Flatpak exposes only what its arguments allow) | as is | empty tmpfs with only `~/.var/app/<id>` (rw), `~/.local/share/flatpak` (ro) and the opened file |
| `/tmp`, `/var/tmp`, `/dev/shm`, devpts | private | private | private |
| Landlock | scoping only (`sandbox_apply_scope`: abstract sockets, signals) | scoping only | scoping only, **not** the file rules (see below) |
| X display | only if the app declares `x11`/`fallback-x11` and isn't restricted | none | none |

Giving the session bus to an unrestricted app's `flatpak run` is safe only because the app itself
reaches it through Flatpak's filtering proxy (`xdg-dbus-proxy`), never directly: LordOS always adds
`--nosocket=session-bus` (see "Never granted"). A restricted app has no bus, so `flatpak run` starts
no proxy (Checked: `flatpak_run_add_session_dbus_args` returns without one when there is no bus
socket), and the app has no portals, notifications or other D-Bus services, exactly the cost Linux
apps pay today.

**Why scoping-only Landlock.** Landlock's file rules forbid `mount` and `pivot_root` for any domain
that handles file access, and bubblewrap needs both, so `sandbox_apply_files` would make every
Flatpak app fail to start. A domain with only Landlock's scoping (ABI 6) handles no file access,
and Landlock's documentation ties the mount restriction to file-system restrictions, so bubblewrap
should still be able to mount inside it. **Unverified** (the kernel's `security/landlock/fs.c` was
not read for this review): the first thing to test.

## Problem 2: the launcher

### Facts

- **Checked.** `flatpak install` writes a rewritten `.desktop` to
  `~/.local/share/flatpak/exports/share/applications` (user) or
  `/var/lib/flatpak/exports/share/applications` (system). Its `Exec` is `/usr/bin/flatpak run
  --branch=… --arch=… --command=… [--file-forwarding] <id> [@@ %f @@]`.
- **Checked.** LordOS's launcher (`server/src/apps.c`) lists `.desktop` files only from
  `~/.local/share/applications` and `/usr/share/applications`, runs their `Exec` without a shell,
  and refuses files written from JS (`lord.fs` denies those folders). The user's export folder is
  in the home folder: anything running as the user can write a file there with any `Exec`.
- **Checked (flatpak-run.c, `forward_file`).** `--file-forwarding` hands a file to the app through
  the document portal (`org.freedesktop.portal.Documents`, a FUSE file system). Without it,
  `flatpak run` logs "Can't get document portal" and passes the path unchanged, which the app can
  open only if its sandbox already sees it.

### Options

**A. Add the two export folders to the launcher's list and run their `Exec` lines.** Smallest
change. *Rejected:* a third and fourth folder whose `Exec` is run as is, one of them user-writable
and unknown to `lord.fs`'s deny list; the command line would be whatever the file says, so LordOS's
arguments (identity, restrictions, never-granted permissions) couldn't be guaranteed.

**B. A Flatpak launch path that takes only an app id and builds the command line in C.**
`lord.apps.launchFlatpak(id)` and `lord.apps.launchFlatpakWith(id, path)` (names to settle). The
id must:

- pass Flatpak's own name rules (at least three dot-separated elements of `[A-Za-z0-9_-]`, not
  starting with a digit, at most 255 bytes) and LordOS's scope name limit (`cgroup_scope_name_ok`:
  62 bytes for the id part; see open question 1);
- be an installed app: `~/.local/share/flatpak/app/<id>/current/active/metadata` (user) or
  `/var/lib/flatpak/app/<id>/current/active/metadata` (system, read-only use), opened without
  following links outside that tree.

The argv is fixed in C: `/usr/bin/flatpak` (root-owned, not writable by others, as the Xwayland
rule), `run`, `--arch=<native>`, `--branch=<the active branch read from the installation>`,
LordOS's generated context options (next sections), the id, and with "Open with" one path after
it. The `.desktop` export is read only for the name, icon, `MimeType` and `Categories`, never for
`Exec`. "Open with" follows `launchWith`'s rules (realpath in the home folder or the user's
mounts, only apps with `MimeType`) and, instead of the document portal, adds
`--filesystem=<that real path>` so the sandbox sees exactly that file (**Unverified** for single
files: to test). Desktop actions are not offered in
phase 1 (their `Exec` lines are app arguments LordOS would have to validate).

**C. Run Flatpak apps from the Store only (no launcher, no Dock).** *Rejected:* the Dock, search
and "Open with" are where apps are opened; a second launch path just for the Store would be the
same exception with a worse user experience.

**Recommendation: B.** The Dock pins them as a new typed id (`flatpak:<id>`, `sdk/dockmodel.js`),
and `sdk/wayland.js` maps their windows (Wayland `app_id` is normally the Flatpak id) to the same
entry. The orphaned-grandchild, log, signal and fd hygiene of `launch()` stays as it is.

## Problem 3: permissions

### Facts (Checked, `common/flatpak-context.c`)

An app's `metadata` `[Context]` group declares:

| Key | Values in 1.16 |
|---|---|
| `shared` | `network`, `ipc` |
| `sockets` | `x11`, `wayland`, `fallback-x11`, `pulseaudio` (since 1.8 it also bind-mounts `/dev/snd`), `session-bus`, `system-bus`, `ssh-auth`, `pcsc`, `cups`, `gpg-agent`, `inherit-wayland-socket` |
| `devices` | `dri`, `all`, `kvm`, `shm`, `input`, `usb` |
| `features` | `devel`, `multiarch`, `bluetooth`, `canbus`, `per-app-dev-shm` |
| `filesystems` | `home`, `host`, `host-os`, `host-etc`, `xdg-*` folders, `~/path`, `/path`, each with `:ro`, `:rw` or `:create` |
| `[Session Bus Policy]`, `[System Bus Policy]` | `<name>=see|talk|own` |
| `[Environment]` | variables |

- The same options exist on `flatpak run` (`--share`, `--unshare`, `--socket`, `--nosocket`,
  `--device`, `--nodevice`, `--allow`, `--disallow`, `--filesystem`, `--nofilesystem`,
  `--talk-name`, `--no-talk-name`, `--own-name`, `--system-talk-name`, `--system-no-talk-name`,
  `--env`, `--unset-env`…). `flatpak_run_app` merges the metadata, then the system and user
  override files, then **the command line last**: LordOS's options win over everything, including
  `~/.local/share/flatpak/overrides/<id>`, which any unsandboxed process of the user can write.
- `--nofilesystem=<path>` for a path not granted becomes a tmpfs over it
  (`flatpak_exports_add_path_expose_or_hide`, mode NONE), so a folder can be hidden inside a
  granted `home`. Flatpak itself always hides `~/.local/share/flatpak` and other apps'
  `~/.var/app/*`.
- Every app may call any `org.freedesktop.portal.*` name through its proxy
  (`--call=org.freedesktop.portal.*=*` in `flatpak-run-dbus.c`). Whether `--no-talk-name` overrides
  that wildcard is **Unverified**; LordOS doesn't rely on it.
- The Wayland side: Flatpak creates a `wp_security_context_v1` listener when the compositor
  offers it. LordOS's compositor doesn't (checked: no reference in `server/src/wl`), so Flatpak
  apps connect to the normal Wayland socket like any Linux app.

### What LordOS never grants (Design, fixed in C)

Whatever the app declares, the launcher always adds:

| Option | Why |
|---|---|
| `--nosocket=session-bus` | the unfiltered bus reaches the systemd user manager (`systemd-run --user`) and the session helper |
| `--no-talk-name=org.freedesktop.Flatpak` | `flatpak-spawn --host`: any command, unsandboxed, outside the scope |
| `--no-talk-name=org.freedesktop.systemd1` | units and transient scopes outside the scope |
| no `--own-name` of a name lord-server serves (`org.freedesktop.Notifications`, `org.freedesktop.portal.Desktop`) | impersonating LordOS's services |
| `--nofilesystem` (hide) for `~/.config/lordos`, `~/.local/share/lordos`, `~/.cache/lordos`, `~/.local/share/applications`, `~/.config/autostart`, `~/.config/systemd`, `~/.config/environment.d`, `~/.local/bin` | the folders `fsops_protected()` and the launcher's grant rules keep from every app: rules, permissions, launchers, autostart, units |
| `--filesystem=<file>:ro` for `~/.bashrc`, `~/.profile`, `~/.bash_profile`, `~/.zshrc` (those that exist) | shell startup files run outside any sandbox at the next login |

`org.freedesktop.portal.Flatpak` (sub-sandboxes) can't be refused reliably by an option, so in
phase 1 LordOS masks `flatpak-portal.service` (see "Exceptions to approve", E5): with no owner and
no activation the name can't be called. A user who unmasks it in their own manager only loses
per-app identity for those sub-sandboxes (they fall under the `user@` union), never a restriction.

The table is **Design**: whether each hidden path really disappears inside a sandbox granted
`home` or `host` is part of the test plan.

### LordOS's restrictions on a Flatpak app (Design)

The existing restrictions of Settings → Privacy apply to `flatpak:<id>` with the same enforcement
as for Linux apps, because the app stays in its scope. Where Flatpak has a matching option LordOS
adds it as well, as defence in depth (the kernel path is the boundary, not bubblewrap):

| Restriction | Enforced by (unchanged LordOS code) | Also added to `flatpak run` |
|---|---|---|
| network rules, no-internet, no-LAN, no-network | `lord-netperm` cgroup programs on the scope | `--unshare=network` for no-network |
| no camera | `cgroup/dev` (V4L2) + PipeWire guard | — |
| no USB | `cgroup/dev` | `--nodevice=usb` |
| no Bluetooth | `cgroup/lsm_socket_create` | `--disallow=bluetooth` |
| no microphone | `cgroup/dev` (ALSA capture nodes) + PipeWire guard | — (`--nosocket=pulseaudio` would also take the sound output) |
| no screen capture | no capture protocol, no screen-cast portal, `/dev/fb*` denied | — |
| no notifications | `hw/notifyd.c`: the caller's pid is the app's `xdg-dbus-proxy`, which `flatpak run` starts inside the scope, so its cgroup is the app's | — |
| no access to your files | LordOS's view (home is a tmpfs with only `~/.var/app/<id>`), see Problem 1 | `--nofilesystem=home --nofilesystem=host` |

**No access to your files is the one restriction whose enforcement changes.** For a Linux app it
is the view plus Landlock's file rules; for a Flatpak app Landlock's file rules can't be used
(bubblewrap must mount), so it is the view alone: a mount namespace whose home is an empty tmpfs,
built by LordOS's code, plus Flatpak's own options inside. Paths outside the home (`/media`,
`/run/media`, other users' homes, `/mnt`) are also hidden in the view by tmpfs mounts (Design),
because without Landlock nothing else would deny them. This is weaker than the Linux-app version
in one respect: Landlock would also have denied files reachable through an fd passed in or a bind
the view forgot. Settings → Privacy says so for Flatpak apps.

**The PipeWire guard and pid namespaces (Unverified).** A Flatpak app's native PipeWire clients
are identified by `pipewire.sec.pid` (socket credentials, translated to PipeWire's namespace), so
the guard finds the scope. Its PulseAudio clients go through `pipewire-pulse`, where the guard
trusts `application.process.id` only for a process in its own PID namespace, and a Flatpak app
reports the pid from inside bubblewrap's namespace. The guard would then treat it as unidentified:
restricted while a restricted app runs (fail closed), which could take the microphone from an
unrestricted Flatpak app in that time. The ALSA capture nodes are still denied in the kernel either
way. To measure on the VM (test 9).

### Portals

LordOS runs no `xdg-desktop-portal`; lord-server serves `org.freedesktop.portal.Settings` itself.

**A. Install `xdg-desktop-portal` and write a LordOS backend** (`org.freedesktop.impl.portal.*`:
FileChooser, Screenshot, ScreenCast, Camera…). The frontend identifies Flatpak apps (from
`/.flatpak-info` of the caller), keeps the permission store and runs the document portal.
*Trade-offs:* a large GLib daemon under `user@` (outside any LordOS scope) that decides app
identity and brokers files; it wants the `org.freedesktop.portal.Desktop` name lord-server holds
(the Settings portal would move to a backend); its ScreenCast and Camera frontends hand out
PipeWire streams, which would break the "no Linux app can copy the screen" claim the moment a
backend exists.

**B. Serve the needed frontend interfaces in lord-server, on the name it already owns.**
FileChooser (LordOS's own dialog, `fs.openFile`-style), OpenURI, Notification, Inhibit, next to
Settings. The caller is identified as notifyd does it today: the bus's
`GetConnectionUnixProcessID`, then that pid's cgroup (the proxy's scope), never a file the app can
write. ScreenCast, Screenshot and Camera are not offered. *Trade-offs:* a new D-Bus surface in
lord-server with untrusted callers; FileChooser for a sandboxed app needs a way to expose the
chosen file after start, which is the document portal's FUSE file system (Debian's
`xdg-document-portal` is part of the `xdg-desktop-portal` package) or a LordOS equivalent.

**C. No new portals in phase 1.** Apps reach only the Settings portal (dark mode, accent, fonts).
GTK 4 and Qt fall back to their own in-process file dialogs, which show only what the sandbox sees
(**Unverified** per toolkit).

**Recommendation when this was written: C for 0.8.6, B later** with its own review (E8 below is not requested now). The
Store says which apps need portals to be useful (declares no `home`/`host` file access and expects
FileChooser).

### Showing them: the Store and Settings (Design)

- **Before installing**, an app's page lists its declared `[Context]` in plain language (network,
  your files: home / whole computer / specific folders, devices: GPU / all devices / USB / webcam via
  `all`, sound, X11, D-Bus names grouped as "talks to other apps"), and next to each one what LordOS
  changes ("never granted on LordOS", "hidden: LordOS settings and launchers"). The metadata comes
  from `flatpak remote-info --show-metadata` (one of the closed commands below), parsed in C with a
  strict key-file reader into a fixed set of flags; unknown keys are shown as "other".
- Flathub's `verified` flag and licence come from the appstream data (see Problem 4). An app that
  asks for `org.freedesktop.Flatpak`, the unfiltered session bus or `host` says so in red before
  install, and that LordOS removes it (so it may not work).
- **Settings → Privacy** lists Flatpak apps with the same restrictions as Linux apps, labelled
  "Flatpak". A per-app editor for Flatpak's own permissions (turning a declared one off) is a later
  phase: it would add options from a validated table, never free text.
- **Monitor** shows `flatpak:<id>` with the app's appstream name and icon; its processes appear as
  `bwrap` and the app's binaries inside the scope.

## Problem 4: installing

### Facts

- **Checked (live, 2026-10-27).** `https://dl.flathub.org/repo/flathub.flatpakrepo` gives
  `Url=https://dl.flathub.org/repo/` and a `GPGKey` whose primary key is `rsa4096`, fingerprint
  `6E5C 05D9 79C7 6DAF 93C0 8135 4184 DD4D 907A 7CAE`, "Flathub Repo Signing Key
  <flathub@flathub.org>", **expiring 2027-06-14** (signing subkey `54A6 CDDD 8919 FB20 4200 D8AC
  5627 02E9 E3ED 7EE8`, same expiry).
- **Checked (live).** The repository serves `summary` + `summary.sig` (12.7 MB, x86_64 refs only,
  including `appstream/x86_64` and `appstream2/x86_64`) and the indexed `summary.idx` +
  `summary.idx.sig` whose sub-summaries include `x86_64`, `aarch64` and the **subsets** `floss-*`,
  `verified-*` and `verified_floss-*`.
- **Checked (flatpak-dir.c, remote-add).** `flatpak remote-add --subset=<name>` restricts a remote
  to a subset, and the appstream branch then becomes `appstream2/<subset>-<arch>`. A signed summary
  may carry `xa.gpg-keys`, which `flatpak_dir_update_remote_configuration` uses to update the
  remote's keys: Flathub can roll its key forward through a summary signed with the current one.
- **Checked.** OSTree's summary and commits are GPG-signed (detached OpenPGP signatures inside
  GVariant), objects are content-addressed by SHA-256. Flatpak verifies them through libostree and
  GPGME. LordOS's package engine has its own OpenPGP and RSA code (`pkg/pgp.c`, `pkg/crypto.c`) for
  clear-signed `InRelease` files, but no GVariant or OSTree reader.
- **Checked.** `flatpak --user` installs into `~/.local/share/flatpak` with no root and no system
  helper. The system helper (root, polkit) is used only for `--system`; LordOS doesn't install
  polkit.

### Options

**A. lord-server runs `flatpak --user` for a closed list of commands, in its own firewall
component.** Flatpak does the network, TLS, OSTree and signature checks itself (libostree +
GPGME). LordOS controls which commands run, their arguments, their scope and what it reads back.
*Trade-offs:* a third-party program doing network and parsing as the user, not inside
`sandbox_apply_helper` (it must write `~/.local/share/flatpak` and runs bubblewrap for
`apply_extra` and triggers); its trust store (`~/.local/share/flatpak/repo/flathub.trustedkeys.gpg`)
is user-writable, as is everything else in the home folder.

**B. LordOS's own OSTree client in a sandboxed helper** (GVariant parser, detached OpenPGP
signatures in `pgp.c`, object fetch and checkout), Flatpak used only to run. *Trade-offs:* the
strongest control and no third-party network code, but several weeks of work, and installing
still has to produce a layout `flatpak run` accepts. Not for 0.8.6.

**C. System-wide installs through the admin helper.** *Rejected:* the roadmap asks for per-user
installs and no new admin action; it would also need the system helper or root-run `flatpak`.

**Recommendation when this was written: A for 0.8.6, B kept as the later way to make the catalog independent of
Flatpak's code.** The closed list (all with `--user --noninteractive`, argv built in C, no shell,
minimal environment, stdin `/dev/null`, output to a log or parsed strictly):

| Command | Arguments from the UI |
|---|---|
| `remote-add --if-not-exists --from flathub /usr/share/lordos/flatpak/flathub.flatpakrepo` | none (the file ships in the `lordos` package, root-owned) |
| `update --appstream flathub` | none |
| `remote-info --show-metadata flathub <ref>` | a ref |
| `install flathub <ref>` | a ref |
| `update [<ref>]` | optionally a ref |
| `uninstall <ref>`, `uninstall --unused` | a ref |
| `list --app --columns=application,branch,version,origin` | none |

A ref is validated in C: `app/<id>/<native arch>/<branch>`, the id as in Problem 2, the branch
`[A-Za-z0-9._-]{1,64}`. Runtimes are never named by the UI; Flatpak resolves them.

Each command is started like a connector: lord-server forks, the child enters a root-owned
`lordos-net-flathub.scope` (the firewall component `lordos:flathub`, added to
`fw_component_scope`'s fixed list) and execs `/usr/bin/flatpak`. In the Monitor its traffic is
"Flathub", with its own rules, and blocking it says that no Flatpak update arrives meanwhile (as
`lordos:store`). Downloads of `extra-data` (proprietary apps that fetch from their vendor during
install) go through the same scope.

**The pinned key.** The `.flatpakrepo` shipped by LordOS holds Flathub's key above. Because the
key expires on 2027-06-14 and Flathub can roll it through `xa.gpg-keys`, LordOS also ships a new
`.flatpakrepo` in a `lordos` update when Flathub publishes one, with a migration that runs
`remote-modify --gpg-import` for users who have the remote (a per-user `MIGRATIONS` entry in
`sdk/settings.js`; never by hand). Whether to accept key updates delivered by the summary or only
LordOS's is open question 6.

**Which subset.** Design: the default is the whole of Flathub, and the Store marks apps that are
neither verified nor free software. Offering `verified_floss` as a setting is open question 7.

### The catalog (Design)

After `update --appstream flathub`, the signature-checked appstream data sits in
`~/.local/share/flatpak/appstream/flathub/<arch>/active/` (`appstream.xml`, icons in
`icons/64x64` and `icons/128x128`; exact file names **Unverified**). lord-server doesn't parse it
(a 10+ MB XML file from the network): a sandboxed helper does, with `sandbox_apply_helper`, the
file given as an fd, returning a compact index in a sealed memfd (id, name and summary in the
user's language, categories, licence, `flathub::verification::verified`, content rating, icon
file name, sizes), which lord-server validates again before the Store reads it. Which helper
(a new mode of an existing one, or a small new binary) is open question 5; the rule is the same as
for every decoder. The website's API is never used.

### Updates (Design)

- Store → Updates lists Flatpak updates beside Debian ones (`flatpak --user remote-ls --updates`
  would be one more closed command; alternatively compare the installed commit with the appstream
  release, open question 8) and applies them with `update`.
- Runtimes left over are removed with `uninstall --unused` after an update, with the space freed
  shown.
- The hourly check that already exists for blocklists (`sdk/netstatus.jsx`) can drive a daily
  `update --appstream`; no new timer or daemon.
- An app running during an update keeps its old deployment until restarted (Flatpak's behaviour);
  the Store says "restart to finish".

## Dependency policy

`AGENTS.md` and [ARCHITECTURE.md](ARCHITECTURE.md) forbid "bubblewrap, flatpak, firejail or
libseccomp for isolation", and `.lpk` isolation must stay LordOS's own code. The argument for
allowing Flatpak as an **app source**:

1. LordOS doesn't link it: no libflatpak, libostree or bubblewrap code in any LordOS binary.
   lord-server only executes `/usr/bin/flatpak` with fixed argv (E1, E3).
2. It isn't installed by default; it arrives as a Debian package the user chooses in the Store,
   like the Commercial apps, and the page says what it adds (about the size: **Unverified**,
   measured in test 1; Flathub runtimes add hundreds of MB each).
3. LordOS's boundaries for a Flatpak app are LordOS's: the root-owned scope, `lord-netperm`'s
   programs, the PipeWire guard, notifyd and the launcher's view. If bubblewrap failed open, the app
   would still be held by every LordOS restriction the owner set, exactly like an unsandboxed Linux
   app. Flatpak's sandbox adds to that; LordOS's guarantees don't depend on it.
4. `.lpk` apps, sysapps and helpers keep `sec/sandbox.c` and nothing changes for them.

What it costs that the policy was protecting: a large amount of third-party C (Flatpak, OSTree,
GPGME/GnuPG, xdg-dbus-proxy, bubblewrap) runs as the user when Flathub is on, and Flatpak's network
code runs outside LordOS's helper sandbox (Problem 4). The wording to approve is E4.

The root parts of the package are not used: LordOS masks `flatpak-system-helper.service` (E5), so
installing Flathub adds no root service and no polkit actions are reachable (polkitd isn't installed;
the mask makes that explicit).

## Exceptions to approve

Each needs the maintainer's approval before code, in the same way as the existing launcher and
network exceptions:

- **E1. The Flatpak launch path** (Problem 2, option B). `lord.apps.launchFlatpak(id)` /
  `launchFlatpakWith(id, path)`: only a validated id of an installed Flatpak app (user or system
  installation), only `/usr/bin/flatpak` (root-owned, not writable by others), argv built in C
  from a fixed set (`run`, `--arch`, `--branch` read from the installation, LordOS's restriction and
  never-granted options from fixed tables, the id, and for `With` one real path in the home folder
  or the user's mounts with its `--filesystem` grant). Never an `Exec` line from an export, never
  caller arguments, no shell. The child enters its scope and builds the view (E2) before exec;
  failing either, it doesn't launch.
- **E2. The identity `flatpak:<id>` and the scope name `app-lordos-fp-<id>.scope`**, created by the
  existing placement request (no new request type), and a launcher view variant that keeps the
  session bus socket (for unrestricted Flatpak apps only) and never contains `systemd/`. Desktop
  ids starting with `fp-` are refused.
- **E3. The closed list of `flatpak --user` management commands with network** (Problem 4,
  option A) in `lordos-net-flathub.scope` (`lordos:flathub`): the table above and nothing else; refs
  validated in C; no URL, remote name, key, file or option from the UI. Flatpak's own TLS and
  signature checks are not relaxed by any option (no `--no-gpg-verify`, no `--from` other than the
  shipped file).
- **E4. Dependency policy wording.** "Flatpak (and with it bubblewrap, OSTree and
  xdg-dbus-proxy) may be installed from the Store as an app source and is run only through E1 and
  E3; it is never linked, never installed by default and never used for LordOS's own isolation."
- **E5. Masking Flatpak's services**: the `lordos` package ships `/etc/systemd/system/
  flatpak-system-helper.service` and `/etc/systemd/user/flatpak-portal.service` as links to
  `/dev/null` (harmless while Flatpak isn't installed). This is a packaging change with a
  migration (`packaging/migrations/`), not an admin action.
- **E6. `lord.fs` deny list**: `~/.local/share/flatpak` (installations, the trusted key ring,
  override files and exports) is added to `fsops_protected()`, since the system reads it to launch
  and install.
- **E7. A sandboxed parser for Flathub's appstream data** (`sandbox_apply_helper`, file as an fd,
  sealed memfd back, validated again in lord-server), whichever binary hosts it.

Not requested now: **E8**, lord-server serving more `org.freedesktop.portal.*` interfaces (portal
option B), which gets its own review in a later phase.

## Device restrictions, the session bus and `no-files` (Design, summary)

- A device-restricted Flatpak app (no-mic, no-camera, no-usb, no-bluetooth) gets the view without a
  session bus, like a Linux app: `flatpak run` starts no D-Bus proxy, so the app has no portals,
  no notifications, no tray, no MPRIS and no a11y bus. Some apps will refuse to start (as Xfce's
  Ristretto does for Linux apps). Settings → Privacy says it, as it does today.
- `no-files` uses the view's empty home with `~/.var/app/<id>` (rw) and `~/.local/share/flatpak`
  (ro, needed by `flatpak run`), plus `--nofilesystem=home --nofilesystem=host`. Not Landlock's
  file rules (see Problem 1).
- A restriction set while the app runs applies at once for everything the kernel enforces, and at
  the next launch for the view (files, bus), as for Linux apps.
- Without Landlock ABI 6 a restricted Flatpak app is not started (`apps.restrictKernel`), as for
  Linux apps. Without unprivileged user namespaces no Flatpak app can start at all (bubblewrap
  needs them), and the Store says so before installing Flatpak.

## Phased plan

**Phase 1, planned at the time for 0.8.6 (if E1–E7 are approved):**

1. Store → "Flathub" switch: installs `flatpak` (no Recommends), runs `remote-add` from the shipped
   file. Masks per E5. x86-64 only.
2. Catalog: `update --appstream`, the sandboxed parser, a Flathub section in Discover and search,
   app pages with declared permissions and LordOS's changes.
3. Install, uninstall, update for user installations; Flathub in Store → Installed and Updates.
4. The launch path (E1) with the scope and view (E2), the never-granted table, LordOS's restrictions
   mapped as above; Dock, search, launcher grid and "Open with".
5. Firewall, Monitor, Settings → Privacy for `flatpak:<id>` and `lordos:flathub`.
6. The test plan below, with a list of apps actually tried and how they behaved: only those are
   named on the website and in the paper.

**Phase 2 (0.8.7 or later):**

- LordOS's portal frontends (E8): FileChooser with a document store, OpenURI, Notification,
  Inhibit; identity by cgroup.
- Sub-sandboxes (`org.freedesktop.portal.Flatpak` served by LordOS so `flatpak-spawn` children
  enter the same scope), which Chromium/Electron apps need.
- A session-bus filter for device-restricted apps (shared with Linux apps, already the "next step"
  in FIREWALL.md).
- Per-app Flatpak permission editor in Settings.
- `wp_security_context_v1` in the compositor, so Wayland clients from Flatpak are known by
  their sandbox engine and app id.
- aarch64 / Raspberry Pi 5 (16 KB pages: some runtimes or apps may not run; **Unverified**).
- Option B of Problem 4 (LordOS's own signed summary and appstream reader).

## Test plan (VM, x86-64 Debian 13 with LordOS)

Each step records the commands, the output and a screenshot (`screendump`) before anything is
claimed; packages installed for it go into `VM-SETUP.md`.

1. Install `flatpak` through the Store (no Recommends): which packages, how much disk; check
   `xdg-desktop-portal` was not installed and `screen.sh`'s portal check still passes; the two masked
   units show as masked.
2. `remote-add` from the shipped file: the remote's key fingerprint equals the pinned one;
   `update --appstream` runs in `lordos-net-flathub.scope` (its row in Monitor → Network); blocking
   `lordos:flathub` makes it fail with a firewall denial.
3. Bubblewrap under LordOS's view: `flatpak run` of a small app (e.g. a GTK 4 app from Flathub)
   inside the view with scoping-only Landlock starts; the same with `sandbox_apply_files` fails (to
   confirm the reasoning in Problem 1).
4. Identity: after launch, `/proc/<pid>/cgroup` of the app, its `xdg-dbus-proxy` and its children
   is `…/app-lordos-fp-<id>.scope`; `flatpak run --verbose` logs the failed transient scope; nothing
   appears under `user@<uid>.service/app.slice/app-flatpak-*`.
5. Firewall: "no internet" on `flatpak:<id>` blocks the app's connections (`denied out tcp …`),
   per-domain rules apply, the Monitor shows its bytes; an escape attempt from inside the app
   (`systemd-run --user`, `flatpak-spawn --host`, `gdbus call` to `org.freedesktop.systemd1`) fails.
6. Never-granted: an app declaring `home` can't read `~/.config/lordos/firewall.json`, can't write
   `~/.local/share/applications` or `~/.config/autostart`, and sees `~/.bashrc` read-only; an app
   declaring `host` likewise. A user override file granting `session-bus` has no effect.
7. no-files: the app sees only `~/.var/app/<id>`; `/media/$USER` and other homes aren't visible;
   "Open with" a file in Documents opens exactly that file.
8. Devices: v4l2loopback camera, a USB device and Bluetooth sockets denied for a restricted app,
   as `tests/fwlinux/devices.sh` and `bluetooth.sh` do for Linux apps.
9. Sound: playback works for an unrestricted app; with no-mic, `parecord` inside the app gets no
   capture and ALSA capture is denied; while a restricted Flatpak runs, check whether an
   unrestricted Flatpak app's microphone is affected (the PipeWire guard question).
10. Notifications: a Flatpak app's notification shows; with no-notifications it is dropped
    (notifyd resolves the proxy's pid to the app's scope).
11. Device-restricted app: starts without a bus; record which apps refuse.
12. Updates: an app with a pending update shows in Store → Updates, updates, and `uninstall --unused`
    frees the old runtime.
13. Apps to try and record (a mix of toolkits and permission profiles): a GTK 4/libadwaita app, a
    Qt/KDE app, an X11-only app (Xwayland), Firefox (its own sandbox inside Flatpak), one Electron app
    (expected to fail or degrade without the Flatpak portal), an app declaring `host`, an app with
    `extra-data`.

## Open questions

1. **Long ids.** The scope name allows 62 bytes for the id part; Flatpak ids may be 255. Refuse
   longer ids in phase 1 (and count how many Flathub apps that excludes from the appstream data), or
   name the scope by a hash and keep a root-readable map?
2. **Sub-sandboxes.** Masking `flatpak-portal` likely breaks most Chromium/Electron apps (zypak).
   Accept that in phase 1 and mark them in the Store, or ship phase 2's own Spawn implementation
   first?
3. **`~/.ssh`, `~/.gnupg` and the `ssh-auth`/`gpg-agent` sockets.** Hide them from Flatpak apps that
   declare `home` by default (as the launcher's grant rules do for Linux apps with no-files), or
   honour the declaration and show it in the Store?
4. **Per-app runtime state.** Bind a persistent per-app `$XDG_RUNTIME_DIR/.flatpak/<id>` into the
   view (shared `/tmp` between an app's instances, `flatpak ps`), or keep it private per launch?
5. **Which binary parses appstream** (E7): a mode of `lord-doc` (which already decodes untrusted
   files), or a small new helper?
6. **Key roll-over.** Accept Flathub's key updates delivered in its signed summary (`xa.gpg-keys`,
   Flatpak's default behaviour), or pin strictly to the key LordOS ships and update it only in
   LordOS releases? The current key expires on 2027-06-14.
7. **Subsets.** Whole Flathub by default, or `verified_floss` by default with the rest behind a
   setting?
8. **Update detection.** One more closed command (`remote-ls --updates`) or LordOS's own comparison
   of installed commits against the appstream data?
9. **Disk use on low-end machines.** Runtimes are large; should the Store warn below a free-space
   threshold, and should Flathub be offered at all on the Raspberry Pi in phase 1?
10. **Flatpak's own seccomp filter** (Flatpak installs one in every sandbox; whether Debian's
    build links libseccomp for it is **Unverified**): it is Flatpak's, inside the app, and LordOS
    never uses it; confirm the wording of E4 covers it.

## References

- Flatpak 1.16.6 source: `common/flatpak-run.c` (`flatpak_run_in_transient_unit`,
  `flatpak_run_add_environment_args`, `forward_file`), `common/flatpak-run-dbus.c`,
  `common/flatpak-context.c`, `common/flatpak-exports.c`, `common/flatpak-dir.c`,
  `portal/flatpak-portal.c`, `app/flatpak-builtins-run.c`, `app/flatpak-builtins-remote-add.c`
  (github.com/flatpak/flatpak, tag 1.16.6).
- Debian: sources.debian.org (flatpak 1.16.6-1~deb13u2, bubblewrap 0.12.0-1~deb13u1),
  `debian/control` of flatpak, `debian/README.Debian` of bubblewrap, packages.debian.org file list
  of `flatpak` (trixie, amd64).
- Flathub: `https://dl.flathub.org/repo/flathub.flatpakrepo`, `summary`, `summary.idx`
  (fetched 2026-10-27).
- LordOS: [FIREWALL.md](FIREWALL.md) ("Linux apps", "Escape via `systemd-run --user`", "LordOS
  components in their own scopes", "Blocklists"), [PRIVACY.md](PRIVACY.md) ("What is isolated
  today", "Root-owned app scopes"), [WAYLAND.md](WAYLAND.md) ("The launcher"),
  [PACKAGES.md](PACKAGES.md), [ARCHITECTURE.md](ARCHITECTURE.md) (dependencies),
  `server/src/apps.c`, `server/src/sec/sandbox.h`, `server/src/sec/cgroup.h`,
  `server/src/remote/cgsup.c`, `server/src/sec/netconn.c`, `server/src/fetch/`,
  `server/src/ui/portal.c`.
