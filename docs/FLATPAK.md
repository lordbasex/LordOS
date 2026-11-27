# Flatpak and Flathub in LordOS: design review

**Status (2026-11-20): phase 1 released in LordOS 0.8.9.4** (x86-64 PCs; lordos.org/features/flatpak). Approved on 2026-11-19, built on branch `feat/flatpak`. On 2026-11-19 the maintainer approved
the seven exceptions E1–E7 below (recorded in `AGENTS.md`, "Flatpak as an app source") and decided: phase 1 on x86-64
PCs only (the Raspberry Pi 5 in phase 2); Flathub's `verified` subset by default, the rest behind a Store setting; apps
that need Flatpak's sub-sandboxes (Chromium/Electron through zypak) were marked "not yet supported" and run since 0.8.9.5 ("Sub-sandboxes" below); open question 11 as decided there (every Flatpak app in the view). No compatibility with any Flathub app is claimed until
the test plan has run. The history of the review follows.

**Update (2026-11-20, after the test plan): the maintainer asked for X11 the way GNOME and KDE give it, for the
sub-sandbox, and for the problems found to be fixed.** lord-server now serves `org.freedesktop.portal.Flatpak` itself
(E9, "Sub-sandboxes" below): GNOME 49+ apps that load images through glycin work (Meld, Emacs). Electron apps still
don't start (they stay marked) and Flatpak's own `flatpak-portal` stays masked.

**Update (2026-11-22): Chromium and Electron apps work, and the Store no longer marks them** (the maintainer: every app
blocked for Electron or Chromium must become installable and work). The Spawn of a Flatpak app now runs through the
app's own Spawn helper, forked inside its view and Landlock domain ("Sub-sandboxes" below), so a sub-sandbox can join
the app's user and PID namespaces as Chromium (QtWebEngine, CEF, the Chromium browser) and zypak (Electron) ask.
Checked on this machine with Whatsie (QtWebEngine), Chromium and FreeTube (Electron): `server/tests/fwflatpak/spawn.sh`.

**Earlier status (2026-10-27, rechecked 2026-11-19 against LordOS 0.8.9.1 and Debian 13's current packages): design review kept for the future; not planned for any release.** On 2026-10-27 the
maintainer decided not to implement Flatpak for now, to keep the system stable and avoid the complexity and
future problems it would add; this document stays as the reference if that is ever reconsidered (2026-11-15, GitHub #164: it may be looked at for 1.0.0, not before). No Flathub or Flatpak launcher integration has been built. Since 0.8.8.8 lord-server does recognise
Flatpak clients (security contexts, and the `/.flatpak-info` identity used for the shortcuts permission and for
drag and drop) and serves the portals listed below; that code is shipped but not yet checked with a real Flatpak. The roadmap item ([ROADMAP.md](ROADMAP.md), "Flatpak and Flathub (future, undecided; not planned for
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

## What changed since 2026-10-27 (recheck of 2026-11-19)

Read again from Debian's security tracker and package metadata, Flatpak's and Flathub's live sources, and LordOS 0.8.9.1's
code. Nothing was run. The design below still holds; these are the corrections and the new facts, each also written
into its section:

- **Debian 13's Flatpak is the security-patched `1.16.6-1~deb13u3`** (DSA-6524-1, 2026-09-28; the stable point release
  still has `~deb13u2`), and **xdg-dbus-proxy `0.1.6-1+deb13u3`** (DSA-6510-1, 2026-09-23, a filter bypass that could
  reach code on the host; the design relies on the proxy for bus filtering). The test plan requires these versions or
  newer. Nothing newer has shipped for flatpak, bubblewrap, xdg-dbus-proxy or ostree since 2026-10-27
  (security-tracker.debian.org).
- **Two fixes in `deb13u3` bear on the launcher** (Problem 2): exported `.desktop` and D-Bus `.service` files now go
  through an allowlist of fields (before, `SystemdService=` in an exported service file could start a host unit), and a
  sandboxed app could signal its parent's process group. LordOS's launcher starts `flatpak run` in its own session
  (`setsid()`) whatever the version, and never reads `exports/share/dbus-1/services` itself; the user bus does, through
  `XDG_DATA_DIRS`, which is why E5's masks and the session-bus rules matter.
- **Upstream moved to 1.18** (1.18.4, 2026-09-28); Debian 13 stays on 1.16.6 with backported fixes. Nothing here may
  rely on 1.17/1.18 features (conditional permissions such as `--device-if`, `--clear-env`, JSON output).
- **polkit is now on every LordOS image** (`polkitd` and `pkexec` since 0.8.8, GitHub #4; lord-server is the
  authentication agent, [PRIVACY.md](PRIVACY.md)). The earlier "LordOS doesn't install polkit" was wrong from then on:
  `flatpak-system-helper`'s polkit actions would be reachable, so E5's mask is now required, not only explicit.
- **lord-server already serves more portals** than the Settings portal: FileChooser v2, OpenURI v3, Inhibit and
  GlobalShortcuts v1, ScreenCast v4 and `org.freedesktop.ScreenSaver` (`server/src/ui/portal.c`); ScreenCast is checked
  on the VM with a browser ([DESKTOP-PARITY.md](DESKTOP-PARITY.md)). None identifies a Flatpak app yet (a caller under
  `user@` stays anonymous). The old argument against a ScreenCast frontend ("would break the no-screen-copy claim")
  no longer applies: LordOS hands screen streams to Linux apps itself, through its own chooser and the PipeWire guard.
- **bubblewrap already runs under a LordOS exception:** lord-web (0.8.8.8) runs WebKit's bubblewrap and xdg-dbus-proxy
  inside the root-owned `app-lordos-lordos-web-<app>.scope` under Landlock *scoping* only; with Landlock file rules
  bubblewrap fails ("Failed to make / slave", [WEBAPPS.md](WEBAPPS.md), checked on kernel 6.12, 2026-11-10). That
  confirms part of "Why scoping-only Landlock" (Problem 1) and is the precedent E4's wording builds on.
- **A new conflict with the launcher's view (Problem 1, option B).** The `/.flatpak-info` identity lord-server added in
  0.8.8.8 (`flatpak_host_ok`, `server/src/wl/kbinhibit.c`) reads `$XDG_RUNTIME_DIR/.flatpak/<instance>/` from
  lord-server's own runtime folder and checks `child-pid` in lord-server's pid namespace, and the security-context
  manager is offered only to clients in lord-server's namespaces (`may_secctx`, `server/src/wl/wl.c`). A `flatpak run`
  inside LordOS's private view (own user, mount and pid namespaces, private runtime folder) would get neither. Phase 1
  has to either run Flatpak apps without the private view when unrestricted, or teach both checks the view (a new
  open question, 11).
- **`flatpak:<id>` already exists as an identity** for the shortcuts permission (`permisos.json`,
  `server/src/sec/perms.h`), next to `program:<path>`; E2's firewall id shares that namespace. `apps_name_of_program`
  already reads `flatpak run … <id>` Exec lines, only to show a name (`server/src/apps.c`).
- **More bus names lord-server owns**, which a Flatpak app must never `--own-name`: also
  `org.freedesktop.ScreenSaver`, `org.kde.StatusNotifierWatcher` and `org.freedesktop.secrets`.
- **Two restrictions the table missed:** `no-incoming` and `no-clipboard` (`server/src/fw/rules.h`); `no-clipboard` is
  enforced today only for `.lpk` apps.
- **Facts now Checked:** Debian's flatpak build links libseccomp (`libseccomp-dev (>= 2.5.2)` in Build-Depends: open
  question 10 is answered); the appstream files are `<arch>/active/appstream.xml` and `appstream.xml.gz`, icons under
  `<arch>/icons`; Flatpak 1.16 tags its Wayland connections with `wp_security_context_v1` (engine `org.flatpak`, app id,
  instance id; since 1.15.6); since 1.16.1 the transient scope is named by instance id, not pid.
- **Flathub:** the same signing key (expires 2027-06-14; the summary carries no `xa.gpg-keys`, so no rotation is
  announced); the summary is 12.86 MB and also carries i386 compatibility refs; `summary.idx` has the `floss`,
  `verified` and `verified_floss` subsets for x86_64 and aarch64. zypak (v2025.09) is still how Chromium and Electron
  apps sandbox under Flatpak, so the sub-sandbox limit stands.
- **Package engine:** since 0.8.8.7 an upgrade installs the Recommends a new version adds; turning Flathub on still
  installs `flatpak` without Recommends. The engine already parses DEP-11 AppStream (`server/src/pkg/idx.c`), a
  starting point for E7.
- **Raspberry Pi 5:** still 16 KB pages; third-party reports show Chromium/Electron apps and jemalloc crashing on 16 KB
  kernels (**Unverified** for Flathub runtimes). User namespaces are on (`CONFIG_USER_NS=y`) and nothing in LordOS
  restricts them.

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
  kernel paths as to Linux apps. lord-server already serves Settings, FileChooser, OpenURI, Inhibit,
  GlobalShortcuts and ScreenCast to Linux apps, but none of them identifies a Flatpak app yet; doing that
  (identity by cgroup) is a later phase with its own review (E8).
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
| `flatpak` | 1.16.6-1~deb13u3 (trixie-security, DSA-6524-1; the point release has ~deb13u2) | Depends: `adduser, bubblewrap (>= 0.10.0~), fuse3, xdg-dbus-proxy (>= 0.1.0)` and its libraries (libostree among them); Build-Depends `libseccomp-dev`. Recommends: `xdg-desktop-portal`, `xdg-desktop-portal-gtk`, `polkitd`, `libpam-systemd`… |
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
Settings portal and its other frontends (`server/src/ui/portal.c`), and `tests/fwlinux/screen.sh` depends on no other
screen-cast portal being installed (LordOS serves its own ScreenCast).

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
(`ui/js_monitor.c`) learn the `app-lordos-fp-` prefix (identity comes from `fw_app_canon` and `appid.c`; `sec/restrict.c` doesn't parse scope names) the way they learned
`app-lordos-lpk-`. The launcher's `valid_id` already refuses desktop ids starting with `lpk-`, and `app_scope_reserved`
(`sec/cgroup.h`) reserves `lpk-`, `lordos-vm*`, `lordos-web*` and the Cloud and remote-desktop names; `fp-` would join
that list, so no `.desktop` can take a Flatpak app's identity.

The view (Design) for a Flatpak app:

| Item | Unrestricted Flatpak app | Device-restricted (no-mic, no-camera, no-usb, no-bluetooth) | no-files |
|---|---|---|---|
| runtime folder | Wayland socket, `pipewire-0`, `pulse/native`, the session `bus` | Wayland, `pipewire-0`, `pulse/native` (no bus, as Linux apps) | same as device-restricted |
| `systemd/`, other agents | never | never | never |
| home | as is (Flatpak exposes only what its arguments allow) | as is | empty tmpfs with only `~/.var/app/<id>` (rw), `~/.local/share/flatpak` (ro) and the opened file |
| `/tmp`, `/var/tmp`, `/dev/shm`, devpts | private | private | private |
| Landlock | scoping only (`sandbox_apply_scope`: abstract sockets, signals) | scoping only | scoping only, **not** the file rules (see below) |
| X display | only if the app declares `x11`/`fallback-x11` and isn't restricted | none | none |

While a Flatpak app is connected to the X display, no X app's drag of files reaches Files: the window
manager checks every X client's pid (X-Resource) before an X drag may hand LordOS paths, and one sandboxed
client is enough to refuse, since any X client can stand in for another's selection (`WAYLAND.md`, "Drag
and drop"). Drags between X and Wayland windows are unchanged.

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
should still be able to mount inside it. **Checked on 2026-11-19** (kernel 6.12.117, bubblewrap 0.12.0, the
maintainer's PC): `build/flatpak-view-probe` (`server/tests/flatpak_view_probe.c`) builds the launcher's keep-home view
with `sandbox_files_isolate` and `sandbox_apply_scope`, then runs `bwrap --unshare-user --disable-userns --unshare-pid
--ro-bind / / --proc /proc --dev /dev --tmpfs /tmp`, the namespaces `flatpak run` asks for. It starts, with and without
the session bus; `/run/user/<uid>/systemd/private` is hidden inside; the runtime folder holds only `bus`, `pipewire-0`,
`pulse` and the Wayland socket; and the process stays in the cgroup it was started in.

**`flatpak run` in the view, Checked on 2026-11-19** (the build VM: Debian 13, kernel 6.12.117, flatpak
`1.16.6-1~deb13u3`, xdg-dbus-proxy `0.1.6-1+deb13u3`, Flathub's `org.freedesktop.Platform//25.08` run with `--command=sh`;
no LordOS session and no app scope, so it shows the mechanics, not the identity):

| Run | Result |
|---|---|
| no view | the process moved itself to `user@1000.service/app.slice/app-flatpak-org.freedesktop.Platform-<n>.scope`: the escape this problem describes, seen |
| view, no session bus | runs; stays in the cgroup it was started in |
| view with the session bus | **fails**: `bwrap: Can't find source path /run/user/1000/.flatpak-helper/monitor`. With a bus, `flatpak run` asks `flatpak-session-helper` (D-Bus activated under `user@`, outside the view) for its monitor folder, which the helper creates in the real runtime folder |
| view with the session bus, `flatpak-session-helper.service` masked | runs; stays in its cgroup; `/etc/resolv.conf` and `/etc/localtime` are right without the helper |
| no view, helper masked | still moves itself to `user@`: the mask doesn't replace the view |

**The launch path itself, Checked on 2026-11-19** on the same VM with `build/launch-cli --flatpak <id>` (the real
`apps_launch_flatpak`: `server/src/apps.c` and `server/src/fpak.c`) and Flathub's `com.voxdsp.Tuxocide`, given the whole
home with `flatpak override --user --filesystem=home` to stand for an app that declares it:

| Restriction | What the app saw |
|---|---|
| none | its processes (bubblewrap, the app, its D-Bus proxy) stayed in the cgroup they were launched in; `~/Documents` readable (the grant); `~/.config/lordos` hidden; `~/.bashrc` readable but not writable (the override's home grant counts as well as the metadata's) |
| no-files | `~/Documents`, `~/.config/lordos` and `~/.bashrc` absent |
| no-mic (a device restriction) | no session bus in its runtime folder and no D-Bus proxy |

With the read-only home (same VM, the app given the whole home by an override, tried from inside its namespaces with
`nsenter`): writing `~/.bashrc`, creating a missing `~/.zshrc`, renaming `~/.config`, writing through a person's
folder that is a link to `~/.config`, and creating a file in the home's root were all refused; its `~/.var/app/<id>`
and the Documents folder of `user-dirs.dirs` were writable; the home stayed readable.

**In a LordOS session, Checked on 2026-11-19 (VM 127,** LordOS `0.8.9.2~ws72`, flatpak `1.16.6-1~deb13u3`): the real
launcher run from the session's `server` leaf asked lord-sessiond for the scope, and bubblewrap, `com.voxdsp.Tuxocide`
and its D-Bus proxy all ran in
`/system.slice/lordos.service/lordos-session-1000/app-lordos-fp-com.voxdsp.Tuxocide.scope`; the window opened on the
desktop with LordOS's frame and menu bar. Two writes of `flatpak run` itself needed the read-only home to keep them
writable: the user's installation `~/.local/share/flatpak` (OSTree locks) and its cache `~/.cache/flatpak` (created on
first use); `strace` found the second. Both are hidden inside the sandbox (`--nofilesystem`, a tmpfs over them): an app
given the whole home saw them empty, and a file it wrote there never reached the real cache (checked the same day). lord-sessiond places only processes whose executable is in LordOS's bin folder,
so the test tool was copied there for the run and removed after.

**From the desktop, Checked the same night (VM 127, `0.8.9.3~ws77`):** search (❖Space) found Tuxocide by name with its
icon and summary (`lord.apps.flatpak`), Enter opened it through `lord.apps.launchFlatpak` in its scope, its window
showed its own icon (lord-server names the window from the client's scope: the game's own `app_id` isn't its Flatpak
id), and Monitor → Processes listed `tuxocide`, three `bwrap`, `xdg-dbus-proxy` and `lordos-view` under "Tuxocide". Not
yet seen: a Flatpak app in Monitor → Network with traffic (the VM's firewall wasn't active).

So the launcher needs the view for every Flatpak app (open question 11) **and** `flatpak-session-helper.service`
masked as well (proposed as E5b, below). The mask also removes `org.freedesktop.Flatpak` (`flatpak-spawn --host`, any
command outside the sandbox) instead of only refusing it with `--no-talk-name`. The runtime with its locale and codecs
extensions took 2.0 GB.

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
`--filesystem=<that real path>` so the sandbox sees exactly that file (**Checked** on VM 127 on
2026-11-19 for a single file: GNOME's Audio Player, which declares no filesystem, opened from Files' "Open with" saw
in its home only `Documents/` holding that one file). Desktop actions are not offered in
phase 1 (their `Exec` lines are app arguments LordOS would have to validate).

**C. Run Flatpak apps from the Store only (no launcher, no Dock).** *Rejected:* the Dock, search
and "Open with" are where apps are opened; a second launch path just for the Store would be the
same exception with a worse user experience.

**Recommendation: B.** The Dock pins them as a new typed id (`flatpak:<id>`, `sdk/dockmodel.js`),
and `sdk/wayland.js` maps their windows (Wayland `app_id` is normally the Flatpak id) to the same
entry. The orphaned-grandchild, log, signal and fd hygiene of `launch()` stays as it is.

**Built on 2026-11-19 (phase 1 step 6):** `flatpak` is a Dock kind (`sdk/dockmodel.js`, the rules of `fpak_id_ok`),
so "Keep in Dock", dragging from the launcher and session restore take `flatpak:<id>` like `desktop:<id>`. Files' and
the Terminal's "Open with" list the Flatpak apps whose export declares the type, and `apps_launch_flatpak_with`
refuses an app whose export has no `MimeType` (as `launchWith` does for Linux apps). An app's other exports, named
after its id plus a suffix (LibreOffice's `org.libreoffice.LibreOffice.writer.desktop`), lend it their `MimeType` and
nothing else: they are no apps of their own, and the file opens in the app by its id (ws170 QA BUG-22). Not done in phase 1: Settings →
Default Apps doesn't offer Flatpak apps (`apps_default_for` reads only Linux apps), and MIME aliases aren't resolved
for them either (an app that declares `audio/wav` isn't offered for a `.wav`, which the system calls
`audio/vnd.wave`; the same holds for Linux apps).

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
  offers it, and lord-server does (0.8.8.8: `server/src/wl/secctx.c`, see
  "Security contexts" in `WAYLAND.md`). A Flatpak app then connects through a socket of its own,
  tagged `org.flatpak` with its app id and instance id: it counts as sandboxed without looking at
  `/.flatpak-info`, and doesn't see the globals only trusted clients get. An older Flatpak, or a
  launcher lord-server can't tell is unsandboxed, falls back to the normal socket, where the
  `/.flatpak-info` check still applies.

### What LordOS never grants (Design, fixed in C)

Whatever the app declares, the launcher always adds:

| Option | Why |
|---|---|
| `--nosocket=session-bus` | the unfiltered bus reaches the systemd user manager (`systemd-run --user`) and the session helper |
| `--no-talk-name=org.freedesktop.Flatpak` | `flatpak-spawn --host`: any command, unsandboxed, outside the scope |
| `--no-talk-name=org.freedesktop.systemd1` | units and transient scopes outside the scope |
| no `--own-name` of a name lord-server serves (`org.freedesktop.Notifications`, `org.freedesktop.portal.Desktop`, `org.freedesktop.ScreenSaver`, `org.kde.StatusNotifierWatcher`, `org.freedesktop.secrets`) | impersonating LordOS's services |
| `--nofilesystem` (hide) for `~/.config/lordos`, `~/.local/share/lordos`, `~/.cache/lordos`, `~/.local/share/applications`, `~/.config/autostart`, `~/.config/systemd`, `~/.config/environment.d`, `~/.local/bin` | the folders `fsops_protected()` and the launcher's grant rules keep from every app: rules, permissions, launchers, autostart, units |
| **the home folder is read-only in the launcher's view** (decided by the maintainer on 2026-11-19 after a security review): writable only `~/.var/app/<id>`, the person's folders from `~/.config/user-dirs.dirs` (real folders, not links, not protected) and an opened path `grant_ok` allows; whatever Flatpak grants (metadata, override files, `~/.bashrc` asked for by name), nothing can write, create or rename a file of the home elsewhere. The metadata is never parsed for this. "Open with" never lends the home folder itself or a folder holding a protected one, and lends a sensitive path read-only | startup files, keys and autostart entries run outside any sandbox at the next login. Protecting them one by one (a first version) could be sidestepped by renaming their folder, by creating a file that didn't exist yet, or through a link; `--nofilesystem=<file>` doesn't hide a single file inside a granted home (checked) |

`org.freedesktop.portal.Flatpak` (sub-sandboxes) can't be refused reliably by an option, so in
phase 1 LordOS masks `flatpak-portal.service` (see "Exceptions to approve", E5). Since 2026-11-20 lord-server owns that
name itself and runs a Spawn inside the caller's own scope and view (E9, "Sub-sandboxes"); the name is in the talk-only
list, so no app can own it. A user who unmasks Flatpak's portal in their own manager changes nothing while lord-server
holds the name.

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
| no screen capture | no capture protocol; the ScreenCast portal refuses the app and the PipeWire guard hides every shared screen from it; `/dev/fb*` denied | — |
| no notifications | `hw/notifyd.c`: the caller's pid is the app's `xdg-dbus-proxy`, which `flatpak run` starts inside the scope, so its cgroup is the app's | — |
| no access to your files | LordOS's view (home is a tmpfs with only `~/.var/app/<id>`), see Problem 1 | `--nofilesystem=home --nofilesystem=host` |
| no incoming connections | `lord-netperm` on the scope | — |
| no clipboard | enforced today only for `.lpk` apps (the broker); for Flatpak and Linux apps it needs the compositor to know the client's app, which the view conflict above affects | — |

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

LordOS runs no `xdg-desktop-portal`; lord-server serves these interfaces itself, on the
`org.freedesktop.portal.Desktop` name (`server/src/ui/portal.c`, Checked against the code). Today their
callers are Linux apps outside Flatpak; none of them identifies a Flatpak app yet (that is option B below).

| Interface | Status | What it does in LordOS |
|---|---|---|
| `org.freedesktop.portal.Settings` | served (0.8.6) | dark or light mode, accent, contrast, title bar buttons, GTK 3 theme, fonts and their antialiasing (since 0.8.9.5, "Fonts" below); `SettingChanged` live |
| `org.freedesktop.portal.FileChooser` | served (version 3 in development; 2 until 0.8.9.5-beta) | `OpenFile`, `SaveFile`: the shell's own file dialog, the answer only to the app that asked |
| `org.freedesktop.portal.OpenURI` | served (version 3; 0.8.8.8) | `OpenURI` (http and https to the default browser, only an address every URL parser reads the same way: printable ASCII, no backslash, no user info, no `%` in the host, a numeric host only as a dotted IPv4, so the question names the host the browser goes to; a `file://` address like `OpenFile`), `OpenFile` and `OpenDirectory` with the file as an fd, used only for its name: it must still name the same file, inside the home folder or the user's disks, outside `fsops_protected`, and opening never takes an executable or a `.desktop` file. Its device, inode and type are kept and checked again right before it opens (`portalcheck.c`), so a file swapped while the question was up is refused. Opened the way LordOS opens everything else (`sdk/sysopen.js`); an app that is not the one with the keyboard (the bus's pid against the focused Wayland client's, or the same app scope) makes the person confirm it first |
| `org.freedesktop.portal.Inhibit` | served (version 1; 0.8.8.8) | `Inhibit` with the idle or suspend flag counts as the compositor's idle inhibition (`wl_idle_inhibited()`, the screen-off, lock and sleep timers); released by `Request.Close` or when the caller leaves the bus. Logout and user-switch flags are accepted and do nothing; no `CreateMonitor` |
| `org.freedesktop.portal.GlobalShortcuts` | served (version 1; 0.8.8.8) | `CreateSession`, `BindShortcuts`, `ListShortcuts`; `Activated` and `Deactivated` (press and release, for push-to-talk) and `ShortcutsChanged`, sent only to the connection that opened the session and only while it is bound. Nothing is bound until the person answers LordOS's own sheet; never a ❖ combination or a key LordOS keeps; a bound key goes only to that app. Decisions kept per app scope in `~/.config/lordos/global-shortcuts.json` (`fsops_protected`), changed or forgotten in Settings → Keyboard ([SHORTCUTS.md](SHORTCUTS.md), "Linux apps' global shortcuts") |
| `org.freedesktop.ScreenSaver` (own bus name) | served (0.8.8.8) | `Inhibit`/`UnInhibit` with a cookie, the same hold, and `GetActive`; released when the caller leaves the bus |
| Notification | not a portal here | apps use `org.freedesktop.Notifications` (`server/src/hw/notifyd.c`) |
| `org.freedesktop.portal.ScreenCast` | served (version 4; 0.8.8.8) | `CreateSession`, `SelectSources` (monitor and window, one or several, the pointer hidden or drawn into the picture; the cursor as metadata isn't offered), `Start` and `OpenPipeWireRemote`, and the `Session` object (`Close`, `Closed`). `Start` shows LordOS's own chooser: the displays and the open windows with a picture of each and the name of the app asking; nothing is shared until the person picks, Cancel answers 1, and an app with "no screen capture" is answered 2 without a question. `persist_mode` gives an app LordOS verified a restore token kept only in memory, valid 10 s and once, revoked by Stop or a lock ("Screenshots and the screen-share token", below); nothing is ever written. The streams come from the sandboxed `lord-screencast` (`server/src/cast/`); the fd `OpenPipeWireRemote` returns is a PipeWire connection that can see only the session's streams. While it runs, the sharing indicator shows it with a Stop; Stop, a lock, the app's `Close` or the app leaving the bus end it. See `WAYLAND.md`, "Sharing a screen or a window" |
| Documents, Flatpak (sub-sandboxes) | not served | see below and E5 |
| Screenshot, Camera | never offered | they would hand out the screen or the camera (see A) |

**A. Install `xdg-desktop-portal` and write a LordOS backend** (`org.freedesktop.impl.portal.*`:
FileChooser, Screenshot, ScreenCast, Camera…). The frontend identifies Flatpak apps (from
`/.flatpak-info` of the caller), keeps the permission store and runs the document portal.
*Trade-offs:* a large GLib daemon under `user@` (outside any LordOS scope) that decides app
identity and brokers files; it wants the `org.freedesktop.portal.Desktop` name lord-server holds
(the Settings portal would move to a backend). (2026-10-27 this also said its ScreenCast frontend would break the "no
Linux app can copy the screen" claim; since 0.8.8 LordOS hands screen streams to Linux apps itself, through its own
chooser and the PipeWire guard, so that argument is gone. The others stand.)

**B. Serve the needed frontend interfaces in lord-server, on the name it already owns.**
FileChooser (LordOS's own dialog, `fs.openFile`-style), OpenURI, Notification, Inhibit, next to
Settings. The caller is identified as notifyd does it today: the bus's
`GetConnectionUnixProcessID`, then that pid's cgroup (the proxy's scope), never a file the app can
write. ScreenCast, Screenshot and Camera are not offered. *Trade-offs:* a new D-Bus surface in
lord-server with untrusted callers; FileChooser for a sandboxed app needs a way to expose the
chosen file after start, which is the document portal's FUSE file system (Debian's
`xdg-document-portal` is part of the `xdg-desktop-portal` package) or a LordOS equivalent.

**C. No new portals in phase 1.** Apps reach the portals lord-server already serves to Linux apps (Settings, and
FileChooser, OpenURI, Inhibit, GlobalShortcuts and ScreenCast, all anonymous for a Flatpak caller until E8).
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

**Built on 2026-11-19 (phase 1 step 5; `apps/store/flathub.js`, `flathubpages.js`, `sdk/flathubmodel.js`):** the
metadata is parsed in **JS** (`flathubmodel.parseMetadata`), not in C as planned above: it only feeds what the page
shows, and the launch never reads it (`fpak_argv` sets the never-granted options and the restrictions whatever an app
declares), so a parser bug can mislabel a row but never widen what an app gets. Each row is a permission key with its
LordOS change: never (session or system bus, `org.freedesktop.Flatpak` / `portal.Flatpak`, `systemd1`, `secrets`),
limited (home or host: read-only but for the person's folders, LordOS's folders hidden; X11 off under any restriction;
other bus names cut off under a device restriction) or blockable in Monitor → Firewall (network, microphone, camera,
USB, Bluetooth: restrictions are set there, not in Settings → Privacy). Unknown sockets, devices and features are
listed as "other". Until 2026-11-22 an app whose `[Application] base=` was `org.electronjs.Electron2.BaseApp`,
`org.chromium.Chromium.BaseApp` or `io.atom.electron.BaseApp` was "Not yet supported" and its Install disabled (open
question 2); since the Spawn helper ("Sub-sandboxes") they install like any other app.
`org.freedesktop.portal.Flatpak` is not a "never" row any more: it is LordOS's own Spawn, which only starts the same
app in a sub-sandbox. Checked on VM 127 with GNOME Calculator and MarkText.

**Display and toolkit (since 0.8.9.5; `apps/store/fhlook.js`, `flathubmodel.displayOf`/`toolkitOf`):**
an app's page also says, from the same metadata and before it is installed, how the app shows its windows and what it
is built with. Display follows Flatpak 1.16's own rule (Debian 13's; `common/flatpak-run-sockets.c`: with
`fallback-x11`, X11 only when the app got no Wayland): `wayland` alone is **Wayland**; `wayland` with `fallback-x11` is
**Wayland (X11 only without Wayland)**, and since LordOS always gives a Flatpak app its Wayland socket such an app gets
no X11 at all (Spotify's case under "Results"); `wayland` with `x11` gets **both** and its toolkit chooses; `x11` alone
(or `fallback-x11` without `wayland`) is **X11, through Xwayland**, and the page says it gets no X11, so no windows,
while its files or a device are blocked for it (the "X display" row of Problem 1's table). The toolkit comes from the
base app (`org.electronjs.Electron2.BaseApp`, `io.atom.electron.BaseApp` → Electron; `org.chromium.Chromium.BaseApp` →
Chromium) or the runtime (`org.gnome.Platform`, `io.elementary.Platform` → GTK; `org.kde.Platform` or an `io.qt.*`
base → Qt; anything else → "its own"), with the runtime's id and branch shown under it. Only the app's page shows it:
the catalog (AppStream) doesn't carry the metadata, and a column in the lists would mean one `remote-info` per app.
Tested in `server/tests/flathub_model_test.mjs` with the real metadata samples there.

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
  helper. The system helper (root, polkit) is used only for `--system`. (2026-10-27 this said LordOS doesn't install
  polkit; since 0.8.8 it does, so the helper must be masked, E5.)

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
| `remote-add --if-not-exists --from flathub /usr/lib/lordos/assets/flatpak/flathub.flatpakrepo` | none (the file ships in the `lordos` package, root-owned) |
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

**Built and checked on 2026-11-19** (`server/src/fpak.c` `fpak_cmd_argv`, `server/src/fpakrun.c`, `lord.flatpak` in
`server/src/ui/js_flatpak.c`; the shipped file is Flathub's own, keys `6E5C…7CAE` and subkey `54A6…7EE8` checked): on
VM 127, from the session's `server` leaf, remote-add, list, install (`cc.nift.nsm`), info and uninstall exited 0, the
running `flatpak` was in `lordos-net-flathub.scope`, and a ref ending in `--no-gpg-verify` was refused before anything
ran. Commands run one at a time. No subset is set on the remote: the maintainer's "verified by default" is a filter of
the catalog (phase 1 step 4), so the setting can show the rest without changing the remote.

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
`icons/64x64` and `icons/128x128`; **Checked** 2026-11-19 in `common/flatpak-dir.c`: `<arch>/active/appstream.xml` and
`appstream.xml.gz`, icons under `<arch>/icons`). lord-server doesn't parse it
(a 10+ MB XML file from the network): a sandboxed helper does, with `sandbox_apply_helper`, the
file given as an fd, returning a compact index in a sealed memfd (id, name and summary in the
user's language, categories, licence, `flathub::verification::verified`, content rating, icon
file name, sizes), which lord-server validates again before the Store reads it. Which helper
(a new mode of an existing one, or a small new binary) is open question 5; the rule is the same as
for every decoder. The website's API is never used for the catalog (only for the rankings, E10 below: app ids, nothing else).

**Built on 2026-11-19 (E7):** the helper is a new small binary, `lord-appstream` (open question 5: not a mode of
`lord-doc`, so it needs neither Poppler nor libarchive: only the C library), reading the uncompressed `appstream.xml`
Flatpak keeps next to the `.gz`. Its index line: id, branch, name, summary, icon (`64x64/` or `128x128/<file>.png`),
categories, license, developer, verified (`flathub::verification::verified`) and, since 2026-11-22, the newest release's
date ("The Store's home" below). The Store shows the verified ones by
default (maintainer's decision); the filter is the shell's, the index carries every app.

**Descriptions (since 0.8.9.5):** an app's page shows what it says about itself, AppStream's
`<description>`, like Flathub's site and other app centres do. The same helper, with nothing new (no syscall, no
network, still only the C library), takes the component's own `<description>` (never a release's) in the person's
language with the scoring of the name (`es-AR`: `es_AR`, then `es`, then the untranslated one; translations per
paragraph, `<p xml:lang>`, are followed too) and flattens its markup to plain text: `<p>` paragraphs apart by an empty
line, `<ul>`/`<ol>` items on their own line after `• ` or their number, `<em>`/`<code>` and anything else dropped,
entities decoded, spaces collapsed, control characters and bytes that aren't UTF-8 removed, at most 4 KB cut at a word
with `…`. It is written as a second line right after its app's, `#d<TAB>id<TAB>text`, with the text's line breaks and
backslashes escaped (`\n`, `\\`). lord-server checks it again (`fpakas_desc_line`: the id, only those two escapes, no
tab or control character, valid UTF-8, the size, no more than one empty line in a row) and keeps it only right after
its own app's line, once, and 24 MB of descriptions at most. The descriptions are kept out of `lord.flatpak.catalog()`,
which the Store reads at every load: the page asks for one app's with `lord.flatpak.description(id)`. Measured on
Flathub's catalog of 2026-11-22 (52 MB of XML, 3381 apps): the index grew from 0.6 MB to 2.5 MB, every app has a
description, four were cut at 4 KB, and the helper still reads the whole file in about 0.1 s, far under its 512 MB cap
(it keeps one 4 KB buffer per app being read, nothing more).

### The Store's home (since 0.8.9.5)

The maintainer asked on 2026-11-22 for a larger catalog the way Bazaar (GNOME's Flatpak app centre) shows it:
collections, categories and subcategories. The Store's Flathub section now opens on a home built only from the catalog
above, so nothing about where data comes from changed:

- **A banner** with the shown app released most recently (`spotlight`), the **ten main categories** as tiles with
  their counts, the **Recently updated** row (newest release first; **See all** opens the whole collection, endless
  scroll), and **Verified apps** while the unverified ones are shown too (otherwise every app shown is verified and
  the row would repeat the catalog); then every app, as before.
- **Subcategories**: a category page shows chips for the subcategories Flathub's catalog has most apps in under it
  (`SUBCATS` in `sdk/flathubmodel.js`, counted on the catalog of 2026-11-22; 60 in all, each a `flathub.sub.*` text),
  only those with apps, and a chip narrows the grid (`filterCatalog`'s `sub`).
- **A verified badge** on every card (**✓ Verified** / **Not verified**).
- The install rules are unchanged: the verified subset unless the setting shows every app, x86-64 only, and an app is
  installed only from its own page after its metadata was read.
- **The release date** comes from the same helper: `lord-appstream` writes each app's newest
  `<releases><release timestamp>` (a direct child of the component's `<releases>`; 1 to 11 digits, no sign or
  leading zero, else none) as the index line's tenth field, lord-server checks it again (`fpakas_line`) and
  `lord.flatpak.catalog()` gives it as `updated` (seconds). The model ignores a date before 2000, more than a day in
  the future or not a whole number. On the catalog of 2026-11-22 every one of the 3381 x86-64 apps has one.

### Settings in the section's second column (in development; the latest release is 0.8.9.5)

Asked by the maintainer on 2026-11-23: the shop opens straight on its banner. The section's description, **Only apps
verified by Flathub** (with how many it hides), Flathub's rankings, **Refresh** and **Turn off** moved to **Settings**,
the second column's item under Installed (`FlathubSettings` in `apps/store/flathubpages.js`). Refresh is also the ↻ icon
in the title bar, next to the search, whose field says "Search Flathub apps" and searches only Flathub's catalog; the
section is called **Flathub apps** in the sidebar, the titles and Installed's filter. Installed no longer repeats the
description.

### Installed in the section's second column (in development; the latest release is 0.8.9.5)

The maintainer asked on 2026-11-23 that each package system's section open on its shop and keep its installed apps
apart, the way the Store's own sidebar has Installed among its pages. The section's second column now starts with
**Installed** (with how many Flathub apps are installed), then **Categories** (All, the home above, and each
category). Installed shows the Flathub subtitle, **Update Flathub apps** (`F.updateAll`, the same job as Store →
Updates), the job's progress and the installed apps with **Remove** and **Open**, filtered by the toolbar's search
(name or id); with none, it says where to find them. The home and the search no longer list the installed apps
above the catalog (an installed app's card says **Open**). `FlathubInstalled` in `apps/store/flathubpages.js`; the
column is `apps/store/origincol.js` (`tests/store_origin_scene.mjs`). The general Store → Installed lists the Flatpak
apps with every other installed app and has a **Flathub** filter (`apps/store/installedmodel.js`,
`tests/store_installed_test.mjs`). Screenshots: `STORE_SCENE=installed tests/store_rankings_headless.sh OUT_DIR`
(`tests/store_scene/installed.js`; it seeds two Flatpak apps and two web apps, installs nothing).

### Rankings from Flathub's API (E10, approved by the maintainer on 2026-11-22; since 0.8.9.5)

Bazaar's Popular, Trending, Recently added and the app of the day/week come from Flathub's web API
(`https://flathub.org/api/v2/collection/{popular,trending,recently-added,recently-updated,verified}`,
`collection/category/<cat>[/subcategories?subcategory=<sub>]`, `app-picks/app-of-the-day/<date>`,
`app-picks/apps-of-the-week/<date>`; checked on 2026-11-22: a collection is a search result of up to 250 hits a page
with `app_id`, `installs_last_month`, `trending`, `added_at`, `updated_at` and the app's texts; a pick is
`{"app_id", "day"}` or `{"apps": [{"app_id", "position", …}]}`). Before E10 nothing in LordOS could reach that API:
lord-server has no HTTP client of its own, and `lord-fetch` had only its two approved jobs. The maintainer approved a
third `lord-fetch` job, E10, in this exact shape:

- **`FQ_FLATHUB` with a number only** (`server/src/fetch/flathub.h`): `{"list": N}` and no other key, N one of a closed
  list, `popular`, `trending`, `recently-added`, `recently-updated`, `app-of-the-day/<date>`, `apps-of-the-week/<date>`.
  The helper builds the path itself (`fh_path`); the picks' date is today's in UTC from the helper's own clock, by
  arithmetic (`fh_date`, no time zone file), never text from JS or lord-server. Any other number, a fraction, text,
  another key or no socket is `bad-request`. Flathub's `verified` and category collections are not in the list (the
  signed catalog already says which apps are verified and their categories).
- **One host, through its own connector:** lord-server connects to `flathub.org:443` only (`FH_HOST`) through the
  `flathub` connector (`sec/netconn.h`, `NC_FLATHUB`: `lordos-net-flathub.scope`, the firewall component
  `lordos:flathub` that Flatpak's own downloads already use, so one row in the Firewall and Monitor → Network blocks or
  shows both) and hands the connected socket to `lord-fetch`, which speaks TLS 1.2+ with the certificate checked against
  the system CA store and the host name (the same `conn_tls` as the other jobs, never relaxed). One GET, **no redirect
  followed** (any 3xx is the failure `redirect`), at most **1 MB** read (`FH_ANSWER_MAX`; a larger Content-Length,
  chunked body or body until close is `big`), 20 s to connect and 30 s for the whole answer (`FH_TIMEOUT_MS`).
- **Only app ids leave the helper** (`fh_parse`, strict `sec/json.h`: duplicate keys, more than 32 levels, `\u0000` or
  trailing bytes refuse the whole answer): a collection's `hits[].app_id`, the day's `app_id`, the week's `apps[].app_id`
  in `position` order (whole numbers 1 to 1000; only the first 1000 entries are looked at). Each must pass `fpak_id_ok`;
  members of another type, bad ids and duplicates are skipped one by one; at most 250 per list, Flathub's order kept.
  The answer is `{"ids": […]}` (`FR_FLATHUB`); no name, summary, icon address, number or any other text of Flathub's
  answer ever leaves the helper, and an error carries only its code. lord-server reads it again (`fh_from_compact`:
  nothing but `ids`, every id through `fpak_id_ok`, more than 250 refused).
- **At most once a day per list** (`server/src/fetch/fhsvc.c`): a list is fetched when its last good copy is from another
  UTC day; a failure waits 15 minutes, doubled after each failure in a row, at most a day. The lists are kept in
  `~/.cache/lordos/flathub/<list>.json` (`{"at", "tried", "fails", "ids"}`; each folder made `0700` and checked to be the
  user's own, no link followed, written to a new file and renamed), read back with every id checked again. A copy from
  an earlier day is still shown until the new one comes.
- **The Store** (`apps/store/flathubpages.js`, `sdk/flathub.js`, `sdk/flathubmodel.js`): `lord.flatpak.rankRequest()`
  asks for the stale lists while Flathub's home or a collection page is shown and the rankings are on, and
  `lord.flatpak.ranks()` gives the ids. The home gets an **App of the day** banner (else the latest release, as before),
  and rows **Apps of the week**, **Popular**, **Trending**, **Recently added**, **Recently updated** and (with every app
  shown) **Verified apps**, each with **See all**. Names, icons and summaries come only from the signed catalog
  (`rankedApps`): an id the catalog hasn't is dropped, and an unverified app only shows while the switch shows every app.
  **Recently updated** is Flathub's (its builds) when at least four of its apps are shown, else the catalog's (the
  newest `<release>` an app declares, which many apps don't keep up to date). The install rules don't change: x86-64
  only, the verified subset unless the switch, and installing only from the app's own page after its metadata was read.
  Without network, with the component blocked or on any failure, the rows are simply absent and the home is the one
  above.
- **A switch to turn it off**, on by default, in the Store's Flathub section under the verified-apps switch
  (`settings.flathub.rankings`; absent means on, so no migration). Off, the Store never asks and shows no rankings.
- **What it reveals:** that this computer uses Flathub's site at most once a day per list (its address and the time),
  nothing about the person's apps: no id, no language and no cookie is sent; the request is the same for everyone.
- **Tests:** `build/flathub-rank-test` (the closed list of paths, every number outside it refused, the date, the recorded
  answers of 2026-11-22 in `server/tests/data/flathub/api-2026-11-22/`, and hostile JSON: over 1 MB, 40 levels deep,
  wrong types, ids with bad characters, too long, duplicates, more than 250, duplicate keys);
  `build/flathub-fetch-test` (the exchange against a stand-in server: the exact GET path and Host, 301/302/303/307/308
  not followed, the 1 MB cap with a Content-Length, chunked and until close, other statuses, non-JSON; then
  `build/lord-fetch` itself refusing bad requests and failing on a peer without TLS); `build/fhsvc-test` (once a UTC
  day, the wait after failures, the cache read back strictly: bad ids, more than 250, not strict JSON, a link);
  `tests/flathub_model_test.mjs` (the rows on the recorded answers against the recorded catalog, hostile `ranks()`
  values, every new text in every language).


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

**Built on 2026-11-19 (phase 1):** no update detection yet (open question 8 stays open): Store → Updates has "Update
Flathub apps", which runs `update` (Flatpak finds what changed) then `uninstall --unused`; an app's page has Update for
that ref. The catalog is fetched again when the Store opens a day after the last fetch (`settings.flathub.appstreamAt`)
or on Refresh, instead of the hourly blocklist check. The page says an open app finishes updating when reopened.

## Dependency policy

`AGENTS.md` and [ARCHITECTURE.md](ARCHITECTURE.md) forbid "bubblewrap, flatpak, firejail or
libseccomp for isolation", and `.lpk` isolation must stay LordOS's own code. Since 0.8.8.8 one exception already lets
bubblewrap run: lord-web, where WebKit starts it (and xdg-dbus-proxy) inside a root-owned LordOS scope; E4 would be the
second, worded the same way (a third party's sandbox inside LordOS's, never instead of it). The argument for
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
installing Flathub adds no root service and no polkit actions are reachable. polkitd is installed on LordOS since 0.8.8,
so the mask is what keeps the helper's actions out of reach.

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
  migration (`packaging/migrations/`), not an admin action. Since 0.8.8 polkitd is on every LordOS image, so this mask
  is what keeps the system helper's polkit actions unreachable.
- **E5b (approved by the maintainer on 2026-11-19, after the test above). Mask `flatpak-session-helper.service`** (user) too, in the same
  way as E5: without it `flatpak run` can't start inside the view when the app has the session bus, and it is the
  service behind `flatpak-spawn --host`.
- **E6. `lord.fs` deny list**: `~/.local/share/flatpak` (installations, the trusted key ring,
  override files and exports) is added to `fsops_protected()`, since the system reads it to launch
  and install.
- **E7. A sandboxed parser for Flathub's appstream data** (`sandbox_apply_helper`, file as an fd,
  sealed memfd back, validated again in lord-server), whichever binary hosts it.
- **E10 (approved by the maintainer on 2026-11-22). A third `lord-fetch` job, `FQ_FLATHUB`, for the Store's rankings**:
  a number from a closed list of six paths on `flathub.org:443` only, through the `flathub` connector
  (`lordos-net-flathub.scope`, `lordos:flathub`), no redirect, 1 MB, once a day per list, only app ids back
  ("Rankings from Flathub's API" above).

Not requested now: **E8**, lord-server serving `org.freedesktop.portal.*` interfaces to Flatpak apps
(portal option B, identity by cgroup), which gets its own review in a later phase. OpenURI and Inhibit
are already served to Linux apps (table above), with no new privilege: no new helper, root action or
library.

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
3. Install, uninstall, update for user installations; Flathub in Store → Installed (with a Flathub filter), in the
   Flathub section's own Installed (since 2026-11-23) and in Updates.
4. The launch path (E1) with the scope and view (E2), the never-granted table, LordOS's restrictions
   mapped as above; Dock, search, launcher grid and "Open with".
5. Firewall, Monitor, Settings → Privacy for `flatpak:<id>` and `lordos:flathub`.
6. The test plan below, with a list of apps actually tried and how they behaved: only those are
   named on the website and in the paper.

**Phase 2 (0.8.7 or later):**

- LordOS's portal frontends (E8) for Flatpak apps: FileChooser with a document store, Notification;
  identity by cgroup. OpenURI and Inhibit are already served for Linux apps (see "Portals").
- Sub-sandboxes: served since 2026-11-20 (E9); Chromium's and zypak's EXPOSE_PIDS / SHARE_PIDS since 2026-11-22
  (the app's Spawn helper). Left: `sandbox-expose-fd(-ro)` (see "Sub-sandboxes").
- A session-bus filter for device-restricted apps (shared with Linux apps, already the "next step"
  in FIREWALL.md).
- Per-app Flatpak permission editor in Settings.
- `wp_security_context_v1` in the compositor, so Wayland clients from Flatpak are known by
  their sandbox engine and app id (shipped in 0.8.8.8, not yet checked with a real Flatpak).
- aarch64 / Raspberry Pi 5 (16 KB pages: some runtimes or apps may not run; **Unverified**).
- Option B of Problem 4 (LordOS's own signed summary and appstream reader).

## Test plan (VM, x86-64 Debian 13 with LordOS)

Each step records the commands, the output and a screenshot (`screendump`) before anything is
claimed; packages installed for it go into `VM-SETUP.md`. The VM runs Debian 13's security-patched versions or newer:
flatpak `1.16.6-1~deb13u3` and xdg-dbus-proxy `0.1.6-1+deb13u3` (2026-11-19).

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

### Results (VM 127, 2026-11-20)

LordOS `0.8.9.3~ws81` with a `lord-netperm` rebuilt with its BPF programs (see the first row), flatpak
`1.16.6-1~deb13u3`, kernel 6.12.116. The installs and updates ran through `build/fpak-cli`, the same closed commands
in `lordos-net-flathub.scope` that the Store runs; the launches through `build/launch-cli` (the real
`apps_launch_flatpak`) from the session's `server` leaf, and from desktop search where noted. The probes inside an
app ran with `nsenter` into its namespaces after moving the probe into the app's scope, so the firewall and the
devices program see the app; Landlock isn't inherited that way, which doesn't change these results (Flatpak apps
get only Landlock's scoping, never file rules). The two test tools were copied to LordOS's bin folder for the run and
removed after.

| Step | Result |
|---|---|
| — | **The firewall enforced nothing.** The VM's `lord-netperm`, like every release package from 0.8.8.6 to 0.8.9.2, was built without clang and so without BPF (`fw.bpfNotBuilt`). Fixed on its own branch (`fix/netperm-bpf`: `mkdeb.sh` refuses such a binary); the rows below used one built with BPF (11 programs attached). |
| 1 | `flatpak` brought `libappstream5`, `libmalcontent-0-0` and `libostree-1-1` (bubblewrap and xdg-dbus-proxy were already there): 10.3 MB. No `xdg-desktop-portal`. `flatpak-portal` and `flatpak-session-helper` are masked in `/etc/systemd/user`. |
| 2 | Flathub's key in the user's remote: `6E5C 05D9 79C7 6DAF 93C0 8135 4184 DD4D 907A 7CAE`. `update --appstream` ran in `lordos-net-flathub.scope`; Monitor → Network showed "Flathub" (311 MB after the installs). With `lordos:flathub` set to no internet: `denied out udp 8.8.8.8:53 (restricted no-internet)` and the fetch failed; lifted, it worked. Flatpak's message for that failure is "No such ref 'appstream2/x86_64'", which the Store shows as it is. |
| 3 | Checked on 2026-11-19 (above). |
| 4 | Every app launched in this run ran in its `app-lordos-fp-<id>.scope` with its `xdg-dbus-proxy`; nothing under `user@1000.service/app.slice/app-flatpak-*`. |
| 5 | Shortwave with no internet: `denied out udp 8.8.8.8:53` and `denied in udp` for its device discovery, "Unable to find radio-browser.info server"; Monitor → Network: 185 denied. From inside it: `org.freedesktop.systemd1`, `org.freedesktop.Flatpak` (`flatpak-spawn --host`: "ServiceUnknown") and `org.freedesktop.secrets` unknown through the proxy, no `$XDG_RUNTIME_DIR/systemd`, no system bus socket, no `systemd-run`. Per-domain rule (`*.radio-browser.info`, deny, out): DNS answered, then `denied out tcp 91.98.4.78:443 (rule)` and the same for its IPv6 address; Shortwave said "Unable to connect to de1.api.radio-browser.info". |
| 6 | Text Editor given `host`, `session-bus`, `system-bus` and `--talk-name=org.freedesktop.systemd1` by a user override: `~/.config/lordos`, `~/.local/share/applications`, `~/.local/share/lordos` and `~/.cache/lordos` are empty tmpfs (a file written there never reached the home); `~/.config/autostart` can't be created (`~/.config` read-only); `~/.bashrc` readable, not writable; Documents writable; the session bus is still only the proxy (`systemd1` unknown, the same filtered names on both socket paths); no system bus. The overrides had no effect beyond `host`. |
| 7 | Text Editor with no-files: its root is a tmpfs and only `~/.var/app/org.gnome.TextEditor` is real; no Documents, `~/.ssh`, `/media/alex` or other homes; no session bus. "Open with" lending exactly one file: checked on 2026-11-19 (Decibels). |
| 8 | Firefox (declares `devices=all`) with no camera, USB and Bluetooth: `open("/dev/bus/usb/001/00x")` → `EPERM` and `socket(AF_BLUETOOTH)` → `EPERM` inside its scope; the same namespaces outside the scope opened both. Camera, with `v4l2loopback` (`/dev/video9`): `EPERM` with no camera (`denied opening device 81:0 (no-camera)`), opened at once when lifted. |
| 9 | Playback: Decibels on 2026-11-19. The VM has no sound card, so a PipeWire virtual source stood in for a microphone. **Problem (fixed the same day, see "Fixes after the test plan"):** an unrestricted Flatpak app (Shortwave) didn't see it either, and the PipeWire guard logged `pwguard:  is restricted (no-mic): its capture stream … was closed` with an empty app. A Flatpak app's PulseAudio client reports a pid from the app's own pid namespace, so the guard can't verify it and treats it as restricted while any no-mic app runs ("unverified", `sec/pwguard.c`), and "runs" (`appid_running`) also counts an app scope that is empty after the app has exited, which stays until the session ends. So after one no-mic app has run, no Flatpak app can record until the next login. It fails closed (no restricted app gets a microphone) but it is wrong. ALSA capture: not tried (no card). |
| 10 | `notify-send` from inside Text Editor: shown. With no-notifications (set before login: lord-server reads `firewall.json` when the session starts and then only through its own pages): not shown, and the app still got an id (`1`). The notification shows a plain blue square, not the app's icon. |
| 11 | Firefox, device-restricted, started without a session bus and showed its first-run page (not accepted). |
| 12 | Text Editor taken back one commit, then `update` brought it to the newest; uninstalling KCalc and `uninstall --unused` removed the KDE 6.11 runtime, its locale and `KStyle.Adwaita` (7.1 → 6.2 GB). The Store still has no update detection (open question 8). |
| 13 | See the next table. |

| App (toolkit, what it declares) | What happened |
|---|---|
| GNOME Text Editor (GTK 4, libadwaita) | works: Dock, "Open with", the rows above |
| Decibels (GTK 4, GJS; sound) | works (2026-11-19) |
| Shortwave (GTK 4, Rust; network, sound) | the window and the station directory work; station covers didn't load until the sub-sandbox (glycin, below); playback fails with "Stream doesn't contain enough data", the same with a plain `flatpak run` outside LordOS, so not LordOS's |
| KCalc (Qt 6, KDE 6.11) | works, with LordOS's frame |
| Firefox (its own sandbox inside Flatpak; `devices=all`) | works, restricted or not |
| Meld (GTK 3; declares `host`) | works since E9 (2026-11-20); before it **crashed at start**: GTK asks gdk-pixbuf for an SVG, gdk-pixbuf asks glycin, and glycin runs its loader through `flatpak-spawn --sandbox`, a sub-sandbox that needs `flatpak-portal`, which LordOS masks; GTK aborts when the icon can't load |
| Emacs (X11 only) | works since 2026-11-20, through Xwayland with LordOS's frame (it can't create `~/.emacs.d`: the home is read-only); before it **didn't start** ("standard input is not a tty"), from the launcher and from search: the launcher never keeps X11 for a Flatpak app (`.x11 = 0` in `apps_launch_flatpak`, the view's `/tmp` has no X socket), although the table under Problem 1 and the Store's "Old-style X11 windows" row say it does when the app isn't restricted |
| Spotify (`extra-data`; declares `fallback-x11` and `wayland`) | installs (its extra data downloaded in the Flathub scope); exits at once with status 1 and nothing in its log. Found 2026-11-20, the same with a plain `flatpak run` outside LordOS: with a Wayland socket, Flatpak gives a `fallback-x11` app no X display, and Spotify's CEF starts on X11 only. With `--enable-features=UseOzonePlatform --ozone-platform=wayland` it showed its login page (on VM 127 also `--disable-gpu`, which has no GPU). A person can put those flags in `~/.var/app/com.spotify.Client/config/spotify-flags.conf`, which Spotify's own launcher reads; LordOS doesn't patch apps one by one |
| MarkText (Electron) | fails as expected: zypak needs `flatpak-portal` ("Unit flatpak-portal.service is masked"), then "Missing X server or $DISPLAY"; the Store already marks it. With E9 its Spawn reaches lord-server, and bubblewrap then stops: "creation of new user namespaces was not disabled as requested" (not looked into further) |

Smaller things seen: after an app closes, Monitor → Network names it by its raw id (`flatpak:de.haeckerfelix.Shortwave`)
with a letter for an icon, and the Dock showed a letter for Shortwave while it ran. Both came from installing it from a
terminal while those lists were open (a Store install refreshes them); fixed anyway (below).

### Fixes after the test plan (2026-11-20)

Checked on VM 127 with `0.8.9.3~ws82` and lord-server rebuilt from this branch:

- **The Store says when the Firewall stopped Flathub** (`apps/store/flathub.js`): with `lordos:flathub` blocked (no
  internet, no network, or an app rule "deny") it says so and where to allow it; with a connection a rule denied since
  the job started, it names the host. A catalog refresh that failed used to keep the last catalog without a word; now it
  says the catalog couldn't be updated.
- **Monitor and Settings take rules and blocks for Flatpak apps**: `firewall.enforceable` in `sdk/monitor.js` left
  `flatpak:` out, so Monitor offered no rule for them, although the firewall applies them. An exited app is named
  without its `flatpak:` prefix, and a Flatpak id the lists don't know yet reads the list again once.
- **Notifications**: lord-server's notification server adds the Flatpak app it found from the sender's scope
  (`"flatpak"`, never from the desktop-entry the caller writes); the shell shows that app's name and icon and files it
  under the same key Settings → Notifications lists. Checked: Text Editor's icon and name.
- **The microphone guard and Flatpak apps** (`sec/pwguard.c`, `sec/appid.c`): an exited app's empty scope no longer
  counts as running (`appid_running` reads `cgroup.events`); a Flatpak app's PulseAudio client is matched to its process
  through the app's own scope: pipewire-pulse's `pipewire.access.portal.app_id` names the app, then a process of that
  scope whose innermost pid (`NSpid`) is the one the client reported and which holds at least as many connections to
  pipewire-pulse as there are clients claiming them. Neither the property nor the pid is taken on its word. The
  connections are counted from pipewire-pulse's side, so an app with no network (another network namespace) is counted
  too. Checked: Shortwave recorded the virtual source while Decibels, with no microphone, ran and couldn't; after
  Decibels closed, recording kept working.
- **X11 like GNOME and KDE give it**: an unrestricted Flatpak app's view keeps the X socket of `$DISPLAY`
  (`/tmp/.X11-unix/X<n>`, bound into its private `/tmp`) and the session's X cookie (read-only, in its runtime
  folder), and the launcher keeps `DISPLAY` and `XAUTHORITY`; `flatpak run` then gives X only to an app that declares
  it. A restricted app gets none, as a restricted Linux app (X11 apps can watch each other, PRIVACY.md). The Store's
  row now says exactly when X is off. Checked: GNU Emacs (X11 only) from search, through Xwayland.
- **lord-server's D-Bus client**: a message with more fds than a connection takes (now 16, was 4) is answered with an
  error and the connection carries on; it used to drop the whole connection (an Electron Spawn took the portal down).
- **The build**: `server/Makefile`'s objects now depend on every header under `src/*/`: changing `vault/dbus.h` left
  objects with the old struct size and lord-server crashed at start (found on the VM, before any release).

### The automated run (`server/tests/fwflatpak/flatpak.sh`)

The plan's checks that need no person at the screen are a script, run in the test VM as the session user
(`make -C server build/launch-cli build/fpak-cli build/pwguard-cli && bash server/tests/fwflatpak/flatpak.sh`):
Flathub's firewall component, the app's identity, never-granted with a user override, no files, the sub-sandbox
(including a refused caller outside any Flatpak app), the X display with and without a restriction, no internet, and
the PipeWire guard with two Flatpak apps (through `pwguard-cli`, which rereads `firewall.json`, so no new login is
needed). On VM 127, 2026-11-20: **31 passed, 0 failed**. Its first runs found one more bug: lord-server's D-Bus client
refused any message whose signature has an fd (`h`) when it carried none, so a Spawn with no fds (an empty `a{uh}`)
closed the portal's connection; each fd index is now checked against the fds that came instead.

### Settings → Flatpak (built 2026-11-20)

Flatpak costs nothing until it is used: the package is installed only when Flathub is turned on in the Store, and
nothing of it runs in the background afterwards (checked on VM 127: no Flatpak process, timer or unit with no app open;
its three services are masked; lord-server's Spawn service exists only while `/usr/bin/flatpak` does, checked every
30 s, and gives its bus name up when the package goes). Settings → Flatpak (`apps/settings/Flatpak.jsx`) shows whether
it is installed, how many Flathub apps there are and how much the apps, runtimes and their own data take, Flathub shown
in the Store or not, and **Remove Flatpak completely**: every Flathub app, the runtimes left unused and the Flathub
remote through the closed list (`sdk/flathub.js` `removeAll`; one more command, `remote-delete --user --force flathub`,
E3), optionally each app's `~/.var/app/<id>` to the Trash, then `flatpak` and the libraries it brought
(`libostree-1-1`, `libappstream5`, `libmalcontent-0-0`, when nothing else needs them) through the package engine's
`pkg-remove --purge` (its `/etc/profile.d` and `Xsession.d` files too), with the plan checked first: a plan that
would remove anything else than those and the packages they pulled in that nothing needs any more (`pkg.why.auto`) is
refused. Last, `lord.flatpak.forget()` deletes Flatpak's own folders, `~/.local/share/flatpak` and `~/.cache/flatpak`
(closed to `lord.fs`, E6), and only once `/usr/bin/flatpak` is gone: never following a link, never into another
filesystem. The Store's Flathub jobs moved to the SDK for it (`sdk/flathub.js`, was `apps/store/flathub.js`).
The count is "—" until the installation was listed once, and follows installs made elsewhere (a terminal's
`flatpak install --user`, a `.flatpakref`): while the page (or the Store's Flathub updates on Installed) shows,
`watchInstalled` compares the exported apps' ids every 3 s and lists the installation again when they changed.

Checked on VM 127 (`0.8.9.3~ws84` and `ws85`), twice: with nine apps installed, then again after turning Flathub on
from the Store (which installed `flatpak` again, 6 packages, 11 MB): afterwards no `flatpak` binary, no Flathub
remote, no `libostree-1-1`, `libappstream5`, `libmalcontent-0-0` or the two libraries they had pulled in
(`libxmlb2`, `libstemmer0d`), no `~/.local/share/flatpak` (113 MB before `forget` existed) or `~/.cache/flatpak`, the
apps' data in the Trash, and lord-server no longer on the bus as `org.freedesktop.portal.Flatpak`. The first run left
`flatpak` in dpkg's "config files" state (its `/etc` files), hence `--purge` since.

### Security review of the branch (Claude Security, 2026-11-20)

A review of `feat/flatpak` against `main` (79 code files, high effort, 17 researchers, each candidate checked by a
three-lens panel) found four problems, all fixed the same day and checked again on VM 127 (`0.8.9.3~ws86`, the
automated plan 31/31, Meld's glycin sub-sandboxes):

- **HIGH — `sandbox-expose` through a link.** The Spawn turned the app's expose names into `--filesystem` grants under
  `~/.var/app/<id>/sandbox`, a folder the app writes: a link there to the session bus socket would have given the new
  instance the unfiltered bus (and through systemd any command outside every sandbox), or the whole home. Expose is no
  longer served (`fpak_spawn_ok` refuses it; glycin doesn't use it).
- **MEDIUM — wildcard bus names.** The never-granted names were cleared by exact name only; xdg-dbus-proxy has no deny
  rule, so an app declaring `org.freedesktop.*` (or `org.freedesktop.secrets.*`) would still have reached the Secret
  Service. Every wildcard of two or more elements that covers a never-granted or LordOS-served name is now cleared as
  well (`org.freedesktop.*`, `org.freedesktop.secrets.*`, `org.kde.*`…; `org.*` is no valid name to Flatpak, neither as
  an option nor in metadata). The first version cleared `org.*` and Flatpak refused to start any app: the automated plan
  caught it.
- **LOW — "No clipboard" offered for Flatpak apps.** Nothing enforces it for Wayland clients yet, as for Linux apps;
  Monitor and Settings no longer offer it for `flatpak:` ids.
- **LOW — the test plan's root helper ran `nsenter` with the tested app's environment** (its `PATH` or `LD_PRELOAD`
  would pick what root runs). It now runs `/usr/bin/nsenter` with a fixed environment and passes the command inside
  only a short list of the app's variables.

### Sub-sandboxes (E9, built 2026-11-20)

Flatpak's `flatpak-portal` serves `org.freedesktop.portal.Flatpak.Spawn` from the user's service manager, so what it
starts leaves the app's scope, its firewall rules and its view; LordOS masks it (E5). GNOME 49+ runtimes need it even
for ordinary apps: GTK asks gdk-pixbuf for an image, gdk-pixbuf asks glycin, and glycin runs each image loader as
`flatpak-spawn --sandbox`. Without it Meld crashed at start and Shortwave showed no images.

lord-server serves the name itself (`server/src/fpspawn.c`, the argument rules in `fpak.c`, `fpak_spawn_argv`):

- **Who**: the bus gives the caller's pid; it must be in a root-owned `app-lordos-fp-<id>.scope` (a Flatpak app's
  proxy runs there), and that process's `/.flatpak-info` must name the same app. Anything else is refused. The name
  is talk-only for apps (`--talk-name`), so no app can own it.
- **What**: `flatpak run` of the same app id, with the app's command and arguments after the id (never read as
  flatpak options), its environment as `--env=` (names checked), `--sandbox` and the sandbox flags as flatpak-portal
  maps them, `--die-with-parent`, and then LordOS's never-granted options and current restrictions, so they win.
  `usr-fd`, `app-fd` (but for Steam since 2026-11-22, see "Steam" below) and `sandbox-expose`/`sandbox-expose-ro` are
  refused (the last two since the security review below: the path is in a folder the app writes, and Flatpak follows a
  link there). `sandbox-expose-fd(-ro)` is read
  and left out (since 2026-11-22, as flatpak-portal does with an fd it can't resolve): Flatpak would be handed the
  fd's path, which the app can swap for a link between the check and the bind. The Chromium browser asks it for its
  `WidevineCdm` folder only, and runs without it (no DRM video).
- **Where (since 2026-11-22): the app's Spawn helper** (`server/src/fpspawner.c`). When the launcher builds a Flatpak
  app's view it applies Landlock's scoping and then, before `flatpak run` of the app, forks a small helper in the same
  process: it is in the app's scope, view namespaces and the very same Landlock domain, holds no capability (bounding
  set dropped too), is not dumpable, and talks only to lord-server over a socket pair lord-server made (the app's end
  closes at exec; the app's own processes are in nested namespaces and can't see it). lord-server checks the caller as
  before, builds `flatpak run`'s argv and environment, and sends them in a sealed memfd with the fds to place; the
  helper checks them again (only `/usr/bin/flatpak run`, at most 16 fds at 0-1023, the instance fd not on one of them),
  forks, places the fds and executes; it answers with the new process's pidfd (lord-server checks it is in the
  caller's scope) and later its wait status. Why a helper: `flatpak run --parent-pid` opens the app's
  `/run/.userns` and `/proc/<pid>/ns/pid`, which the kernel allows only from the app's own Landlock domain or one
  above it; the earlier waiter applied Landlock's scoping again (a new, sibling domain), bubblewrap then ran without
  the app's user namespace and stopped on "creation of new user namespaces was not disabled as requested", and
  Chromium's zygote never started (a CHECK failure: Whatsie died after showing its tray icon). lord-server no longer
  enters any namespace. SpawnExited goes to the caller alone; WATCH_BUS ends the process group when the caller leaves
  the bus; SpawnSignal only reaches the caller's own processes (through the pidfd). At most 32 per app. SpawnStarted
  waits (on each tick, up to 3 s) for bubblewrap's `bwrapinfo.json`, so the relative pid Chromium needs isn't 0.
- **How long it lives (since 2026-11-22)**: as long as anything of the app runs in its view. The view's PID 1 used to
  leave with the app's first process, and the kernel then killed the rest: VS Code's `code` starts Electron detached and
  exits, so VS Code opened a window and vanished. PID 1 now stays while anything the app started runs (as bubblewrap's
  PID 1 does) and, once only the helper is left, ends it (`sandbox_view_helper` tells PID 1 its pid; the helper has no
  parent-death signal any more, or it would die with `code` while Electron still asks). A stop (SIGTERM to the view)
  still ends everything once the app's first process leaves. Test: `build/sandbox-view-lifetime-test`.
- **Nothing in the background**: lord-server takes the name only while `flatpak` is installed (checked every 30 s, so
  turning Flathub on needs no restart). Without Flatpak nothing is on the bus and nothing runs.

Checked on VM 127: Meld and GNU Emacs start, with `glycin-svg` and `glycin-image-rs` in their own scopes; a running
`glycin-svg` had `NoNewPrivs: 1`, was two PID namespaces below the view, had its own network namespace, no session bus,
only `/dev/null`-class devices and an empty home. Electron (MarkText) reaches the Spawn and bubblewrap stops on
"creation of new user namespaces was not disabled as requested" when it joins the caller's namespaces
(`--parent-pid` with `--parent-expose-pids`); left for later, so Electron apps stay marked.

Fixed on 2026-11-22 with the app's Spawn helper (above). Checked on the maintainer's PC (LordOS 0.8.9.4, Debian 13,
kernel 6.12) with the branch's launcher and Spawn portal (`build/fpspawn-cli` on a private bus,
`server/tests/fwflatpak/spawn.sh`, 18 checks): Whatsie 6.2.0 (QtWebEngine 6.11.3), Chromium (org.chromium.Chromium) and
FreeTube (Electron, zypak) each start their zygote in a sandbox nested in the app's PID namespace, SpawnStarted gives
Chromium the right relative pid, renderers run with seccomp-bpf, a window draws, and nothing traps. Before: Whatsie
died at every start (`trap int3` in libQt6WebEngineCore).

### Shared runtime folders (decided 2026-11-22)

Open question 4, decided by the maintainer: a second launch of a running Flatpak app shares its runtime folders. Flatpak
gives all instances of an app the same `$XDG_RUNTIME_DIR/.flatpak/<id>` (its per-app `/tmp`, `tmp/` in it) and
`$XDG_RUNTIME_DIR/app/<id>` (bound as the app's `/run/user/<uid>`); in LordOS both were in each view's own tmpfs, so a
second launch (Open with, the Launcher, a Dock "New Window") couldn't reach the first instance's single-instance socket.

The launcher (`flatpak_share` in `apps.c`) makes `$XDG_RUNTIME_DIR/lordos/fpshare/<id>.<restrictions>/flatpak` and
`.../app` (the user's, 0700, each step a real folder) and the view binds them at `.flatpak/<id>` and `app/<id>` of its
runtime folder (`share_src`/`share_dst` of `sandbox_files_isolate`, opened again there with no link on the way). Only
views of the same app id, the same user and the same restrictions (the hex mask of `FW_R_*` in the name) share them: a
restriction added while an instance runs never puts a new instance in the same folders as the unrestricted one. Flatpak's
own instance folders (`.flatpak/<number>`) stay in each view.

### Lending a chosen file (decided 2026-11-22)

LordOS's equivalent of the document portal, without FUSE (`server/src/fplend.c`; the view's side is `lend_one` in
`sec/sandbox.c`). When a Flatpak app with the session bus gets a file from LordOS's file dialog
(`org.freedesktop.portal.FileChooser`), or a picture from the Screenshot portal, it gets
`/run/flatpak/app/<id>/doc/<grant>/<name>` instead of the real path (Flatpak 1.16 binds `$XDG_RUNTIME_DIR/app/<id>` there
in every instance), and exactly that is mounted in its running view:

| Dialog | What the app sees at `doc/<grant>/` |
|---|---|
| Open a file | that file, writable when the launcher's grant rules allow it (`apps_grant_ok`), else read-only |
| Choose a folder | that folder and what is in it (refused for the home, a folder holding its keys or startup files, a protected one) |
| Save | only the chosen file, writable (lord-server makes it empty first); nothing else of its folder |

- **How it reaches the running app.** The view's PID 1 (the only process with the view's user namespace's capabilities)
  mounts a small tmpfs at `$XDG_RUNTIME_DIR/.lordos-lend`, makes it shared and binds it read-only at
  `app/<id>/doc` before `flatpak run`. bubblewrap binds `app/<id>` (with what is mounted under it) at
  `/run/flatpak/app/<id>`, as a slave, so a grant PID 1 mounts later in its tmpfs appears in the running app. The app can't
  write where grants go (a read-only bind); another view of the same app has its own tmpfs.
- **Who asks.** The D-Bus caller's pid (the app's xdg-dbus-proxy) must be in a root-owned `app-lordos-fp-<id>.scope`;
  its view is the one whose PID 1 is that process's topmost ancestor in the scope, and lord-server talks to that PID 1
  only over the socket pair the launcher gave it (`FPLEND_FD`, closed in the app; PID 1 says hello with its pidfd).
- **What is lent.** Only a path written the canonical way (no empty, `.` or `..` element, no `/` at the end:
  `sec/pathcanon.h`): the policy compares strings, and `/home/u/./.config` or `/home/u//.ssh` (an app sets the dialog's
  starting folder) would match none of its entries (Claude Security F1/F2, 2026-11-22). The dialog never starts in such a
  folder (`startFolder`), the portal drops such a chosen path, and lord-server opens the path with no link on the way and
  goes on only when the kernel names what it opened (`/proc/self/fd`) by that very string; the policy
  (`fsops_protected_resolved`, `apps_grant_ok`, which also refuses any non-canonical path) is decided on it, and it sends
  its device and inode. PID 1 refuses a non-canonical path too, opens it again in its own mount namespace (`openat2`, no
  links), lends it only if it is that very file, copies its mount (`open_tree`), sets it nosuid, nodev and read-only
  unless writable, and moves it onto `<grant>/<name>` (a random 128-bit name, on PID 1's tmpfs, read-only to the app).
  Protected paths are never lent. A mount the kernel keeps read-only is lent read-only. At most 256 grants per view.
- **A save** lends the target file alone, exactly as an open does, writable: the grant folder holds that name and
  nothing else, so the app never sees the rest of the folder it pointed the dialog at (Claude Security F5, 2026-11-22;
  before, the whole folder was lent read-only). The name must pass `apps_grant_ok` (never a protected or sensitive path,
  a startup file, or a file with another name); the folder itself isn't checked, since nothing of it is lent.
- **Sensitive folders**: the launcher's (`sensitive` in `apps.c`: keys, startup, autostart and LordOS's folders) and, for
  lending only (`lend_sensitive`), credential stores, browser and mail profiles and other Flatpak apps' data: `~/.aws`,
  `~/.kube`, `~/.password-store`, `~/.local/share/keyrings`, `~/.docker`, `~/.netrc`, `~/.pki`, `~/.mozilla`,
  `~/.thunderbird`, `~/.config/google-chrome`, `~/.config/chromium`, `~/.config/BraveSoftware`,
  `~/.config/microsoft-edge`, `~/.var/app`. Never lent as a folder or writable, nor a save into them; a file there the
  person opens is lent read-only.
- **Hidden paths** (any segment starting with `.`: `~/.npmrc`, `~/.vimrc`, `~/.config/mimeapps.list`,
  `~/.config/git/…`, many of which run code later): never a save, never lent writable; a dot-file or hidden folder the
  person opens is lent read-only (`hidden_path` in `fplend.c`, 2026-11-22).
- **Revoked** with the view: when the app's last process leaves, its mount namespace and every grant go.
- A caller that isn't a Flatpak app LordOS started gets the real path, as before. A lend that fails is logged and the
  app gets the real path (which its own Flatpak permissions decide).
- Limit: an app that saves only by writing a temporary file next to the target and renaming it over it can't save
  there: the grant folder is read-only and holds no room for a temporary file, and the target is a mount point (a rename
  over it fails with `EBUSY`). "Write limited to that name" is the decision. An app that writes the file in place (most
  GTK and Qt apps through the portal's answer, `fpportal-probe`) saves normally. Not yet measured with a real app that
  renames.

Tested by `build/sandbox-lend-test` (a fake app, also inside bubblewrap with the grant made after it started) and
`build/portal-cast-test`; end to end with `build/fpportal-probe` inside a real Flatpak sandbox (below).

### Steam (decided 2026-11-22)

Steam's Flatpak runs its client and games in the Steam Runtime through pressure-vessel, which asks the Spawn portal for a
sub-sandbox with its own `/usr` and `/app` (`usr-fd`, `app-fd`, with SHARE_PIDS). Approved for the verified id
`com.valvesoftware.Steam` only (`read_spawn` in `fpspawn.c`), every other app's still refused:

- each fd must be a folder opened for reading (or `O_PATH`), inside Steam's data folder (`~/.var/app/com.valvesoftware.Steam`,
  where Steam keeps the runtime it deploys), Steam's own installed app, or the installed runtimes of the user's or the
  system's installation (`fpak_steam_fd_ok`): from the folder up through `..`, on the very same mount all the way (the
  statx mount id; a bind of another folder, or a link the app followed when it opened it, ends elsewhere), to one of
  those folders, opened by lord-server with no link on the way;
- lord-server places them like the app's own fds and adds `--usr-fd=N`/`--app-fd=M` (`fpak_spawn_argv`; never with an
  empty `/app` too) to `flatpak run` of Steam itself, which the Spawn helper starts in Steam's view and Landlock domain,
  with LordOS's never-granted options and restrictions after what Steam asked: the child never has more than Steam.

### Screenshots and the screen-share token (decided 2026-11-22)

- **ScreenCast restore token.** A session that asked to persist (`persist_mode` 1 or 2) of an app LordOS verified gets
  `restore_token` and `persist_mode` 1 in Start's answer: kept only in lord-server's memory, for the sources the person
  picked, valid 10 s from when it was given and once (`csm_token_issue`/`csm_token_take`). The same app's next session
  with it, within that time, starts on those sources without the chooser (Chromium asks twice for one share; OBS restarts
  its capture right after the choice); the indicator shows the share as always and its frames still wait for it; a
  restored session gets no token, so a token never leads to another. Never for an unknown program; never written.
  Revoked before it is spent (Claude Security F6/F7, 2026-11-22): the person's Stop in the indicator (or the share
  ending on its own) drops that app's tokens; a lock, a text console, a user switch, the session leaving the display or
  every share ending (`cast_end_all`) drops all of them, and none is honoured while the session is locked or away: the
  chooser, as always. Only the app's own `Close` keeps it (OBS restarting its capture).
- **Screenshot portal** (`org.freedesktop.portal.Screenshot` version 2: Screenshot and PickColor). The same chooser and
  gate as ScreenCast ("no screen capture" refuses; an unknown program is said so), always interactive whatever the app
  asks: a screen, a window, or "Select Area…" to drag over the live screen; PickColor shows only the overlay, a click
  picks the point. The picture is taken after the commit that removed the chooser and the overlay (an effect), painted as
  a share is (protected windows black, `cast_still`), encoded by lord-media into a new PNG in the person's Screenshots
  folder (named as the Screenshots app names its files, never over a file) and lent to a Flatpak app as above; others get
  its real path. The indicator shows "‹App› took a screenshot" (or picked a color) for 4 s. One request at a time.

### Checked end to end (2026-11-22, the maintainer's PC)

The branch's lord-server, headless (1600×900), in the session's server leaf with a private bus and runtime folder and a
test home whose `~/.local/share/flatpak` was the user's installation (bound in; `flatpak list --user --all` taken
before, Steam, ksnip and what they brought installed for the run and removed after, the list identical at the end), and
`build/fpportal-probe` run inside a real Flatpak sandbox (nsenter into the app's namespaces, the app's own filtered bus):

- **Lending** (Whatsie, which has the Documents folder but not the rest of the home): an Open of `~/Private/chosen.txt`
  answered `/run/flatpak/app/com.ktechpit.whatsie/doc/<grant>/chosen.txt`, readable; `~/Private/sibling.txt` was
  reachable neither at its real path nor next to the lent file. A Save of `~/Private/probe-saved.txt` was written through
  the lent name, and nothing else could be made next to it. (ksnip declares `host`, so it sees everything anyway.)
- **Shared runtime folders**: a second launch of Whatsie printed "another instance owns … running as secondary,
  forwarded commands to running instance" and left; before, it ran as a second copy.
- **Screenshot**: the chooser showed the screen and every window with pictures; "Select Area…" and a drag of 500 × 350
  gave a 500 × 350 PNG of exactly that part, with neither the chooser nor the overlay in it, lent to the app and readable
  there; PickColor answered a color. "Whatsie took a screenshot" in the log.
- **Steam**: with the update downloaded (483 MB) the client reached "Sign in to Steam"; `pv-adverb` ran with `/usr` and
  `/app` from `~/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamrt64/pv-runtime/…/var/tmp-*/`, `NoNewPrivs: 1`
  and seccomp. pressure-vessel's Spawn carries 82 environment variables: the per-Spawn limit went from 64 to 128 (the
  whole command line still fits the Spawn helper's, checked at build time). Whatsie's Spawn with a `usr-fd` was refused
  (`NotSupported`, "option not served: usr-fd").

### Fonts (fixed 2026-11-22)

GTK apps from Flathub looked out of place: their text was bigger, wider and softer than LordOS's. Checked offline with
CPU-X (`io.github.thetumultuousunicornofdarkness.cpu-x`, GTK 3.24.50 in `org.freedesktop.Platform//25.08`), nothing
shown on screen:

- Inside the sandbox, `gdbus call … org.freedesktop.portal.Settings.ReadAll "['org.gnome.*']"` gave only
  `color-scheme`, `accent-color`, `gtk-theme`, `gtk-im-module` and `button-layout`: no font keys.
- GDK's settings table in that runtime (`libgdk-3.so.0`) falls back to `font-name` = `Adwaita Sans 11` and reads
  `font-antialiasing`, `font-hinting`, `font-rgba-order`, `text-scaling-factor`, `icon-theme` and `cursor-*` from
  `org.gnome.desktop.interface` (older GTKs read the first three from `org.gnome.settings-daemon.plugins.xsettings`).
- `fc-match "Adwaita Sans"` and `fc-match sans` in the sandbox both give `DejaVu Sans`: the app drew DejaVu Sans 11
  (14.7 px), where LordOS draws Inter at 12–13 px. The sandbox sees the host's `/usr/share/fonts` (as
  `/run/host/fonts`), never `/usr/lib/lordos/assets/fonts`, so Inter wasn't there to ask for.

What changed:

- **The settings portal serves the text keys** (`server/src/ui/portal.c`), in `org.gnome.desktop.interface`:
  `font-name` and `document-font-name` `Inter 10` (13.3 px at 96 dpi), `monospace-font-name` `JetBrains Mono 10`,
  `font-antialiasing` `grayscale` (LordOS's own text is grayscale coverage, `ui/text.c`), `font-hinting` `slight`,
  `font-rgba-order` `rgb`, `text-scaling-factor` `1.0` and `icon-theme` `Adwaita`; and the same three antialiasing keys
  in `org.gnome.settings-daemon.plugins.xsettings` for older runtimes. When Inter or JetBrains Mono isn't in
  `/usr/share/fonts/truetype/lordos`, the names are fontconfig's `Sans 10` and `Monospace 10`; a change is sent with
  `SettingChanged`. The same keys go to the GSettings keyfile, for GTK 3 apps outside Flatpak (Debian's schema
  default, `Adwaita Sans 11`, isn't installed on LordOS either).
- **The lordos package installs Inter and JetBrains Mono in `/usr/share/fonts/truetype/lordos`** as well as in
  `/usr/lib/lordos/assets/fonts` (`tools/iso/mkdeb.sh`): copies, since a link to `/usr/lib/lordos` would not resolve in
  the sandbox. fontconfig's dpkg trigger refreshes the cache. New files, no migration.
- **Not served:** `cursor-theme` and `cursor-size`. GTK 3.24.50 and GTK 4 ask for cursors by shape
  (`cursor-shape-v1`), and LordOS draws them itself (`cursor.c`); it has no Xcursor theme to name.
- **Display scale** reaches GTK through the Wayland output, so `text-scaling-factor` stays 1 (more would scale the text
  twice). LordOS has no separate text-size setting today; if one is added, it goes here.

Tests: `build/portal-gs-test` reads every key with `ReadOne` and `ReadAll("org.gnome.*")` and checks the keyfile;
`tests/gtk_defaults_test.mjs` checks the font folder against `mkdeb.sh` and the three values against Debian's schema
enums. Still to check in a real session: CPU-X's text next to LordOS's, after installing the package that carries the
fonts.

## Open questions

1. **Long ids.** The scope name allows 62 bytes for the id part; Flatpak ids may be 255. Refuse
   longer ids in phase 1 (and count how many Flathub apps that excludes from the appstream data), or
   name the scope by a hash and keep a root-readable map?
2. **Sub-sandboxes.** Masking `flatpak-portal` likely breaks most Chromium/Electron apps (zypak).
   Accept that in phase 1 and mark them in the Store, or ship phase 2's own Spawn implementation
   first? **Decided 2026-11-19: mark them; 2026-11-20: LordOS's own Spawn built (E9) because glycin needs it;
   2026-11-22: the maintainer asked for every Chromium/Electron app to install and work: the app's Spawn helper, the
   marking removed.**
3. **`~/.ssh`, `~/.gnupg` and the `ssh-auth`/`gpg-agent` sockets.** Hide them from Flatpak apps that
   declare `home` by default (as the launcher's grant rules do for Linux apps with no-files), or
   honour the declaration and show it in the Store?
4. **Per-app runtime state.** Bind a persistent per-app `$XDG_RUNTIME_DIR/.flatpak/<id>` into the
   view (shared `/tmp` between an app's instances, `flatpak ps`), or keep it private per launch?
   **Measured 2026-11-22 (Flathub compatibility run):** private per launch breaks every app with a single-instance lock.
   Flatpak binds `$XDG_RUNTIME_DIR/.flatpak/<id>/tmp` as `/tmp` and `$XDG_RUNTIME_DIR/app/<id>` for all instances of an
   app; in LordOS both sit in the view's own tmpfs, so a second launch (Open with, the Launcher, a Dock "New Window")
   can't reach the first instance's socket and runs as a second copy on the same profile: Chromium showed "Profile
   error occurred" and its databases were locked, four VS Code copies corrupted `argv.json`. Session restore now
   launches a Flatpak app once (`launchesOnce`); the rest needs this decision. **Decided by the maintainer 2026-11-22:
   shared between the views of the same app id, user and restrictions ("Shared runtime folders" above).**
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
10. **Flatpak's own seccomp filter** (Flatpak installs one in every sandbox; Debian's build links libseccomp for it,
    **Checked** 2026-11-19: `libseccomp-dev (>= 2.5.2)` in Build-Depends): it is Flatpak's, inside the app, and LordOS
    never uses it; confirm the wording of E4 covers it.
11. **The launcher's view and lord-server's Flatpak checks** (new 2026-11-19). The `/.flatpak-info` identity
    (`flatpak_host_ok`) and the security-context manager (`may_secctx`) both assume the client shares lord-server's
    runtime folder and namespaces; inside LordOS's private view neither works. Run unrestricted Flatpak apps without
    the view (as unrestricted Linux apps run today) and keep the view for restricted ones with the checks taught to
    follow it, or change how the view handles `$XDG_RUNTIME_DIR/.flatpak`?
    **Decided by the maintainer (2026-11-19, corrected the same day):** every Flatpak app runs in the view, restricted or
    not. A first answer ran unrestricted apps without it, but then `flatpak run` reaches the user manager's socket and
    has it move the app into `app-flatpak-<id>-*.scope` under `user@` (Problem 1, Facts), out of its LordOS scope, its
    firewall rules and its Monitor row; only the view, which never contains `systemd/`, stops that. Unrestricted apps
    keep the session bus (reached only through Flatpak's filtering proxy), Wayland and sound in their view. lord-server
    knows a Flatpak client by its root-owned scope (`app-lordos-fp-<id>.scope`, read from the client's pid) instead of
    `/.flatpak-info` and the runtime folder, and offers the security-context manager to clients of those scopes.

## References

- Flatpak 1.16.6 source: `common/flatpak-run.c` (`flatpak_run_in_transient_unit`,
  `flatpak_run_add_environment_args`, `forward_file`), `common/flatpak-run-dbus.c`,
  `common/flatpak-context.c`, `common/flatpak-exports.c`, `common/flatpak-dir.c`,
  `portal/flatpak-portal.c`, `app/flatpak-builtins-run.c`, `app/flatpak-builtins-remote-add.c`
  (github.com/flatpak/flatpak, tag 1.16.6).
- Debian: sources.debian.org (flatpak 1.16.6-1~deb13u3, bubblewrap 0.12.0-1~deb13u1, xdg-dbus-proxy 0.1.6-1+deb13u3),
  security-tracker.debian.org (DSA-6524-1, DSA-6510-1, DSA-6472-1; rechecked 2026-11-19),
  `debian/control` of flatpak, `debian/README.Debian` of bubblewrap, packages.debian.org file list
  of `flatpak` (trixie, amd64).
- Flathub: `https://dl.flathub.org/repo/flathub.flatpakrepo`, `summary`, `summary.idx`
  (fetched 2026-10-27, again 2026-11-19).
- LordOS: [FIREWALL.md](FIREWALL.md) ("Linux apps", "Escape via `systemd-run --user`", "LordOS
  components in their own scopes", "Blocklists"), [PRIVACY.md](PRIVACY.md) ("What is isolated
  today", "Root-owned app scopes"), [WAYLAND.md](WAYLAND.md) ("The launcher"),
  [PACKAGES.md](PACKAGES.md), [ARCHITECTURE.md](ARCHITECTURE.md) (dependencies),
  `server/src/apps.c`, `server/src/sec/sandbox.h`, `server/src/sec/cgroup.h`,
  `server/src/remote/cgsup.c`, `server/src/sec/netconn.c`, `server/src/fetch/`,
  `server/src/ui/portal.c`.
