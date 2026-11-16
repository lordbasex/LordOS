# LordBridge (separate project): Windows, games and Android apps on LordOS

LordBridge lives in its own repository (`LordBridge/`, outside this tree). It detects what a program is, prepares an
isolated place for it, and launches it with the right engine: Wine for Windows programs, Proton (through umu-run) for
Windows games, Waydroid for Android apps. Engine versions sit side by side and switch like `nvm`
(`lordbridge runtime use wine 9.0`, a per-app pin). macOS apps are research only.

Status (2026-11-16): **phase 0** — `lordbridge detect`, `doctor` and the runtime layout exist; nothing is launched yet,
and nothing in LordOS depends on it.

## What LordOS already gives it

- **Xwayland on demand** (docs/WAYLAND.md): Wine draws through it today; WM_CLASS matching puts each program on the
  Dock.
- **Per-app scopes and the firewall's per-app rows** (docs/FIREWALL.md): a Windows program launched in its own scope
  gets its own firewall row: allow, ask or block the network per Windows app.
- **Session restore, Search, Dock** treat any program with a `.desktop` as a Linux app: LordBridge writes one per app.
- **Steam** already runs on LordOS; its own Proton games need nothing from LordBridge.
- On the maintainer's machine (`lordbridge doctor`): Xwayland, RADV Vulkan, i386 libraries, Steam, the
  `binder_linux` module (Android), user namespaces and Landlock are there; wine, umu-run, waydroid and lxc are not.

## What it would need from LordOS (each one a reviewed exception, AGENTS.md)

| Phase | Need | Kind |
|---|---|---|
| 1 · Wine | `lord.bridge.*` in lord-server: run `/usr/lib/lordos/bin/lordbridge` with fixed verbs (detect an fd, run/stop an app id, list runtimes), arguments checked in C, no shell; the Manager is a LordOS app | new JS→program exception |
| 1 · Wine | Debian's `wine` from the Store (no new code) | package |
| 2 · Versions | a signed catalog of engine builds (SHA-256 each) and a sandboxed downloader into the home folder | new network path |
| 4 · Android | load `binder_linux` with its devices, start/stop Waydroid's container | 2–3 admin.c actions |

Until those are approved, LordBridge runs only from a terminal, like any user program.

## Phases (LordBridge's docs/PHASES.md)

0 detect/doctor (done) · 1 Wine (high feasibility) · 2 versions side by side (high) · 3 Proton for games outside Steam
(medium-high) · 4 Android through Waydroid (medium-low: a root container, binder, its windows on lord-server) ·
5 ARM64 with FEX/Box64 (medium) · 6 macOS (research; GUI apps a no-go).

PlayOnLinux is not used (unmaintained); its install-recipe idea comes back as LordBridge's own recipes.
