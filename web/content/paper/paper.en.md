## Abstract

LordOS is a free GNU/Linux distribution based on Debian 13 that ships a desktop environment of its own, built from its fundamental components on top of Debian and Wayland. Instead of GNOME or KDE, it has its own display server and Wayland compositor written in C (`lord-server`), its own user-interface engine (React running on the QuickJS JavaScript engine, with layout, painting and text in native C code), its own native apps, and its own security services: an app sandbox launcher, a permission broker, a per-app permission store, a kernel-enforced per-app firewall, package signing and an encrypted password vault. It reuses what is mature and widely reviewed: the Linux kernel, Debian and its packages, Mesa, PipeWire, the Wayland protocol, Xwayland for X11 apps, and a handful of libraries (FFmpeg, Poppler, libarchive, OpenSSL, libsrtp2, usrsctp, Monocypher) that are confined to small sandboxed helper processes or to the session manager.

This paper describes the design and the current state of **0.8.5 Beta**, released on 27 October 2026, for a technical reader who wants to check the project rather than take it on trust. **0.8.5 Beta is the latest release**; 0.8.0 (18 October) was the first public release, followed by 0.8.1, 0.8.2, 0.8.3, 0.8.4, the package-only 0.8.4.1 and 0.8.5, each listed in `CHANGELOG.md`. Every claim refers to a file in the public repository. Numbers are the ones measured in the project's test logs, and are labelled with where they were measured. Things that are designed but not built are marked as such. Since 0.8.5, many of the claims can also be checked on the reader's own computer with System Information's Checks and the Test Bench (§11), and `docs/PAPER-VALIDATION.md` lists every claim of this paper with how it is checked.

## 1. Vision

### 1.1 The problem

A modern Linux desktop is assembled from many independent projects: a display server or compositor, a toolkit, a session manager, a settings daemon, a keyring, a network manager, a firewall front end, and the apps. Each is good on its own terms, but the seams between them are where things get slow, inconsistent or insecure. Three seams in particular motivated LordOS:

- **Weight.** A desktop that draws its shell with a web engine, or loads several toolkits at once, needs hundreds of megabytes before the user opens anything. LordOS wants to be comfortable on modest machines and on a Raspberry Pi 5.
- **Trust in apps.** On a traditional Linux desktop, every program the user runs can read the whole home folder, record the microphone, open any network connection and read the clipboard. Sandboxing exists, but it is usually an add-on that the desktop itself doesn't understand.
- **Visibility.** Users rarely see which app is talking to which server, and even more rarely can they say "this app may not use the internet" in a way the app can't get around.

### 1.2 Principles

The project follows a short list of rules, written down in `AGENTS.md` and `docs/ARCHITECTURE.md`:

1. **Own the parts the user touches; reuse the foundations.** The display server, the compositor, the UI engine, the apps and the security services are LordOS's code. The kernel, Debian's packages, Mesa, PipeWire and the Wayland protocol are reused.
2. **A strict dependency policy.** X11, wlroots, libdrm, libinput, WebKit, GTK and Qt are not used inside LordOS. Libraries that parse untrusted data are linked only by sandboxed helpers, never by the compositor.
3. **The interface can't run commands.** JavaScript in LordOS has no API to execute arbitrary programs. The exceptions are few, narrow and each approved and documented in `AGENTS.md`: the Terminal's login shell; launching an installed `.desktop` app (optionally with one file the user opened, or one of the app's own named actions); starting an installed `.lpk` app in its sandbox; Xwayland, started by `lord-server` itself with a fixed command line only when an X11 app connects; the components' network connectors and the blocklist downloader, which re-execute LordOS's own binaries with fixed arguments; and the Store's download child, which places itself in its own scope. None takes text from the interface.
4. **Root only through closed doors.** Everything that needs root goes through a fixed table of validated actions or a small dedicated service with a closed protocol.
5. **The CPU path must always work.** GPU composition is an optimization, never a requirement.
6. **The compositor never waits.** Helpers, apps and services talk to `lord-server` over non-blocking sockets; a hung app hangs only itself.
7. **Verify on a real system.** Every milestone is checked on a virtual machine with real screenshots before it is marked done, and compatibility that wasn't tested isn't claimed.

### Source files

- [AGENTS.md](https://github.com/lordbasex/LordOS/blob/main/AGENTS.md)
- [docs/ARCHITECTURE.md](https://github.com/lordbasex/LordOS/blob/main/docs/ARCHITECTURE.md)

## 2. Architecture

### 2.1 Layers

[[diagram:layers]]

From the bottom up:

- **The Linux kernel** provides DRM/KMS for displays, evdev for input, and the security features LordOS builds on: Landlock, seccomp-bpf, cgroups v2 and eBPF. On PCs it is Debian's kernel; on the Raspberry Pi 5 it is LordOS's build of the Raspberry Pi kernel with Landlock enabled (§7.4).
- **Debian 13** provides the userland: `dpkg` and the package archive, systemd, Mesa, PipeWire, `libwayland`, `libxkbcommon`, CUPS, BlueZ and the rest.
- **`lord-server`** is the display server and Wayland compositor. It drives displays and input directly, composites, and serves the Wayland protocol to Linux apps.
- **The UI engine** lives inside `lord-server`: QuickJS runs the React interface, and native C code does layout, painting and text.
- **Apps** come in three kinds: built-in apps and the theme, which run in `lord-server`'s JavaScript context; installed `.lpk` apps, each in its own sandboxed process with its own engine instance; and Linux apps, which connect to the compositor as ordinary Wayland clients.

### 2.2 Processes and privileges

[[diagram:processes]]

A running LordOS system has a small number of processes with clearly separated roles:

| Process | Runs as | Role |
|---|---|---|
| `lord-sessiond` | root | login screen, sessions, lending the display and input to the visible session, remote desktop |
| `lord-netd` | root | the system network: DHCP, Wi-Fi, VPN; only the `net-*` actions |
| `lord-netperm` | root (limited capabilities) | per-app network rules, and the device and Bluetooth restrictions of Linux apps, enforced with cgroup and LSM eBPF programs |
| `lord-fand` | root (no capabilities) | fan control by temperature |
| `lord-server` | the user | one per session: compositor, UI engine, broker |
| `lord-server --app <id>` | the user, sandboxed | one per running `.lpk` app, and the system apps that run isolated: Passwords, Mail, Calendar, Contacts, Reminders, and System Information's Sandbox Probe |
| `lord-doc`, `lord-media`, `lord-maild`, `lord-vault`, `lord-fetch`, `lord-bench` | the user, sandboxed | helpers that process untrusted data or secrets (`lord-fetch` downloads the firewall's blocklists; `lord-bench`, since 0.8.6, runs LordBench's workloads) |
| `lord-tls` | `nobody`, sandboxed, in the Store's download scope | HTTPS for the package engine's downloads (0.8.5): checks the certificate and the host name, then only relays bytes |
| `lord-server --net-connector mail\|print\|lists` | the user, in its own scope | opens network sockets for one LordOS component, so the firewall can see and block it separately |
| `Xwayland` | the user, own scope and network namespace | only while an X11 app is connected, started by `lord-server` with a fixed command line |
| login screen | `lordos-greeter` | draws the login UI; never verifies passwords itself |

Each root process has a narrow, documented interface. §6.3 describes them.

### 2.3 What is ours and what is reused

| Ours (in this repository) | Reused |
|---|---|
| Display server and Wayland compositor (C) | Linux kernel |
| UI engine: native nodes, flexbox layout, rasterizer, text shaping, bidi, input methods | Debian 13 and its packages; `dpkg` |
| React renderer (host configuration for `react-reconciler`) | React 19.3.1, `react-reconciler` 0.34 (bundled in the theme) |
| Native apps and themes | QuickJS 2025-04-26 (vendored) |
| Sandbox, broker, permissions, signing, firewall service, vault | Mesa (optional), PipeWire, `libwayland`, `libxkbcommon` |
| Session manager, network service, fan service | systemd, systemd-resolved, BlueZ, CUPS, wpa_supplicant, OpenVPN, strongSwan's daemon |
| Debian package engine (OpenPGP, xz, HTTP and HTTPS, resolver) | FFmpeg, Poppler, libarchive, OpenSSL, Monocypher (in helpers only) |
| Touchpad driver, terminal emulator, WebAssembly interpreter | stb (fonts and images), fonts under the OFL |
| Remote desktop over WebRTC: ICE-lite, DTLS-SRTP set-up, RTP packetization, bandwidth control (encoding in `lord-media`) | libsrtp2 and usrsctp (in `lord-sessiond`'s bridge), libx264, libvpx, Opus and VA-API (in `lord-media`) |
| System Information's collector, `lordos-sysreport`, `lordos-fetch`, the Test Bench | Xwayland (Debian's package, on demand), smartmontools, `pci.ids` and `usb.ids` |

LordOS speaks to several Debian services over their own protocols with its own clients: D-Bus (BlueZ, power profiles, time and date), IPP (CUPS), the PulseAudio protocol (PipeWire), and the Secret Service. libdbus, sd-bus, libcups, libpulse, libbpf and libapt are not used.

### 2.4 System services

- **Network (`lord-netd`).** The network belongs to the system, not to a session. `lord-sessiond` starts `lord-netd` at boot with a fixed path and no arguments, so the machine has an address before anyone logs in. It runs only the closed `net-*` actions: its own DHCP client and netlink code, Wi-Fi through Debian's `wpa_supplicant`, WireGuard through LordOS's own netlink code, OpenVPN, IPsec/IKEv2 through strongSwan's `charon` driven over VICI with LordOS's own client, and optionally OpenConnect, PPTP and Fortinet-compatible tunnels. It talks only to the sessions' root helpers, over a socket that accepts only uid 0.
- **Fans (`lord-fand`).** A root service that reads the CPU temperature every 2 s and writes fan duty from a profile, from early boot. It drives only fans its own scan found: the Raspberry Pi 5 fan header and allow-listed motherboard controllers. Laptop, GPU and unknown fans are never written. On stop, or after a crash, it restores the original kernel settings. It runs no programs, has no network, no capabilities and no device nodes.
- **Hardware settings.** Displays (modes from KMS with EDID names, a 15-second "keep these settings?" revert, per-display persistence, rotation, Night Light), Bluetooth through BlueZ, battery and power profiles, idle and lid handling, date and time, notifications for Linux apps and default apps (in 0.8.6, in development: when you haven't chosen one, a file opens with the system's default or the first installed app that declares its type, shown as "Automatic"). Changes arrive as kernel uevents and D-Bus signals; nothing polls. On a Raspberry Pi 5, Settings → Board firmware shows and updates the bootloader in the board's EEPROM through two closed admin actions (0.8.4.1).
- **Updates.** Installed systems update in place from LordOS's signed APT repository on lordos.org, from Store → Updates or with `apt`; changes that are more than new files run as numbered, idempotent migrations (0.8.1). The Store's package engine follows apt's priorities (`/etc/apt/preferences.d`, 0.8.5), so on the Raspberry Pi image "Update all" chooses the same versions as `apt` and the pinned kernel stays blocked, and it accepts sources written as `Architectures: amd64,arm64` as apt does. The update from 0.8.4.1 or 0.8.4 to 0.8.5 runs no migration; it was tested with the release package built as 0.8.5~rc2 on a VMware machine on 0.8.4.1 and a Raspberry Pi 5 on 0.8.4, both with `apt`, then a restart.

### Source files

- [server/src/main.c](https://github.com/lordbasex/LordOS/blob/main/server/src/main.c)
- [server/src/admin.c](https://github.com/lordbasex/LordOS/blob/main/server/src/admin.c)
- [server/src/net/netd.c](https://github.com/lordbasex/LordOS/blob/main/server/src/net/netd.c)
- [server/src/fan/fand.c](https://github.com/lordbasex/LordOS/blob/main/server/src/fan/fand.c)
- [server/src/hw](https://github.com/lordbasex/LordOS/tree/main/server/src/hw)

## 3. The compositor

### 3.1 Displays without libdrm

`lord-server` sets display modes with the kernel's DRM ioctls directly (`kms.c`): it enumerates connectors, CRTCs and planes, allocates buffers, and page-flips on vertical blank. It picks the card that has connectors, not simply `card0`, which matters on boards where the display controller and the GPU are separate devices (on the Raspberry Pi 5, `vc4` and `v3d`).

### 3.2 Two ways to build a frame

- **CPU path.** The UI and app windows are copied into a canvas, and a "dumb" buffer is presented with a page flip. It needs nothing but the kernel. It is the default when there is no 3D-capable card, and `LORD_GPU=0` forces it (the boot menu's safe mode uses it). Since 0.8.5, heavy frames paint on a pool of up to 8 threads (`par.c`), only while painting takes more than 70 % of a frame at the target rate (in 0.8.6, in development, also while the whole frame takes more than 60 %): the backdrop blur in phases and the row-local primitives split by bands of rows, with the same pixels as one thread (§7.2).
- **GPU path.** Mesa's GBM, EGL and GLES2 (`server/src/gpu/gpu.c`). The UI becomes one texture, and only the changed region is uploaded each frame. App windows are not copied into the UI canvas: the UI leaves a transparent hole where each window goes, and window textures are drawn underneath, so an app that only changes its own content doesn't cause a UI repaint. Apps can hand over GPU buffers (`linux-dmabuf`) with no copies, and a fullscreen app with a suitable buffer is scanned out directly. The cursor uses the hardware cursor plane.

The path is chosen at start-up: a card without 3D (simpledrm, bochs, virtio-gpu without virgl, and others) uses the CPU without even loading Mesa; a software OpenGL renderer also falls back to the CPU, because composing with it costs more than LordOS's own CPU path. If a GPU frame fails, the GPU is turned off and the CPU path takes over.

**Status:** the GPU path was exercised with Mesa's software renderer in virtual machines. Testing on real AMD and Nvidia cards is pending.

### 3.3 Windows and input

- **Window management in C.** Moving and resizing run in C with one event per frame and no JavaScript, and a window being moved is drawn from its own layer. All four edges and corners resize, with LordOS's own cursors.
- **Input without libinput.** Keyboards, mice and tablets are read from evdev. Keyboard layouts come from `libxkbcommon` (every XKB layout, variants and compose).
- **Touchpad and touchscreen.** LordOS has its own multitouch driver (`server/src/tp`): pointer acceleration, tap and two-finger click, palm rejection, kinetic and natural scrolling, pinch, and three-finger swipes. Gestures reach both the LordOS interface and Linux apps (`zwp_pointer_gestures_v1`). It is tested with recorded gestures in `make test`.

### 3.4 Wayland compatibility

The compositor (`server/src/wl/wl.c`) uses only `libwayland-server` and code generated from the protocol XML. It offers: `wl_compositor`, `wl_subcompositor`, `wl_shm`, `wl_output` (one per display), `xdg_wm_base` with popups, `wl_seat` (pointer, keyboard, touch), `wl_data_device_manager` (a clipboard shared with LordOS), `zwp_primary_selection_device_manager_v1` (middle-click paste, 0.8.3), `zxdg_decoration_manager_v1`, the KDE server-decoration protocol (so GTK 3 apps such as Firefox get the theme's frame), `zxdg_output_manager_v1`, `wp_fractional_scale_v1`, `wp_viewporter`, `zwp_linux_dmabuf_v1` v4, `wp_presentation`, `zwp_relative_pointer_manager_v1`, `zwp_pointer_constraints_v1`, `zwp_pointer_gestures_v1`, `wp_cursor_shape_manager_v1` and `zwp_idle_inhibit_manager_v1` (a video player keeps the screen on, 0.8.5). The compositor honors the frame an app asks for, also when it changes while the window is open (0.8.4).

**Checked from the outside.** The Test Bench (`lordos-testbench`, 0.8.5) is a plain Wayland client that binds each of these at the advertised version and exercises it: shm formats, outputs and their modes, xdg-output, fractional scale, a subsurface with a viewport, a popup, maximize and fullscreen states, the dmabuf feedback, clipboard and primary-selection round trips, idle-inhibit, cursor shape, relative pointer and gestures. Its first run found that the compositor didn't send a new selection back to the client that set it, as the protocol requires, and that idle-inhibit was missing; both were fixed the same day, and the run now gives 16 passed, 0 failed on VM 116 (`docs/PAPER-VALIDATION.md`).

Linux windows enter the SDK's window manager like any other, with title, focus, maximize, minimize and a Dock icon. Apps that ask for server-side decoration get the theme's frame; apps that draw their own (GTK 4) keep it, and their shadow is clipped. GTK 4 and libadwaita apps follow light/dark mode, the accent and the button order through an `org.freedesktop.portal.Settings` implementation that `lord-server` serves itself on the session bus; GTK 3 follows through the GSettings keyfile.

**X11 apps (0.8.3).** When an X11-only app connects to the session's X socket, `lord-server` starts Debian's `/usr/bin/Xwayland` with a fixed command line, rootless, in its own scope and its own network namespace, with `no_new_privs`; the compositor is the X window manager (`server/src/wl/xwm.c`), so X11 windows get the same frame, Dock entry, per-app identity, firewall rules and restrictions as Wayland ones, and the clipboard works both ways. Isolated `.lpk` apps and Linux apps restricted from devices get no `DISPLAY`. Debian's `xwayland` package must be installed.

Still pending: drag and drop, LordOS input methods inside Linux apps (`text-input` v3), activation (`xdg-activation`), explicit sync, single-pixel buffers, tearing control, content type, drawing tablets and SVG app icons. No screen-capture protocol is offered to apps.

### 3.5 Several displays and HiDPI

Every connected display is driven at once, extended or mirrored, in one logical space, with an arrangement remembered by EDID identity and hotplug handling. Each display is painted at its own scale (100, 125, 150, 175 or 200 %), so text and vectors are rasterized at its real resolution. Linux apps get one `wl_output` per display and fractional scaling. Tested with two virtual heads (1280×800 and 2560×1600) on both paths; real monitors, laptop panels and DisplayPort hubs are untested.

Since 0.8.5, Settings → Displays numbers each display (the main one is 1) and **Identify** shows that number large on each monitor; dragging a display in the arrangement snaps it edge to edge and aligns it with the others, a drop never leaves two displays overlapping or apart, and the server keeps a position that fits exactly. A window can be sent to another display from the Window menu, and an app can be assigned to a display from its Dock item, kept by the connector name (HDMI-A-1, DP-2…) so two monitors of the same model are told apart (verified on VM 118). The sign-in and lock screen goes on the main display only, with the wallpaper on every display, and follows displays plugged in or out (VM 118, two displays).

### 3.6 Sessions and login

[[diagram:sessions]]

`lord-sessiond` is LordOS's own session manager:

- **The login screen** is a `lord-server` running as the unprivileged `lordos-greeter` user, with a special theme. It sends login requests to `lord-sessiond`; **passwords are always verified by the root process** against `/etc/shadow`, never by the greeter's JavaScript. Failed attempts are answered at a fixed 1.25 s, so timing reveals nothing. Since 0.8.5 it follows the kernel's display events, so a monitor plugged in after boot shows it, and it no longer stays running behind the user's session: it closes 10 seconds after a session takes the screen (about 53 MB of its own, 78 MB counting shared pages, measured in a VMware VM; the whole system went from 603 to 548 MB in use) and starts again in a few milliseconds on a lock, a user switch or a log-out. On a lock the screen goes dark first and only then is the login screen started, so the unlocked desktop is never left visible meanwhile.
- **The user's session bus first (0.8.5).** `lord-sessiond` starts `user@<uid>` and waits for the user's session bus socket (up to 5 s) before starting the session. With automatic login it used to start a few milliseconds too early, so the Secret Service of `lord-vault` and the notification server never got on the bus (§6.10).
- **One session per user**, all able to live at once (fast user switching). Each is a `lord-server` that starts as root and drops privileges right away.
- **Display lending.** `lord-sessiond` opens the graphics card and the input devices as root and lends them to the session that is on the monitor, over a Unix socket. On a switch it revokes them: `EVIOCREVOKE` makes the old session's input file descriptors stop working, and `DROP_MASTER` takes display control away, before anything is given to the next session. Since 0.8.5 access is still cut at once, but the devices are closed on a short-lived thread, because the last close of a USB mouse waits for its transfers to stop: from the lock to the sign-in screen went from 903 ms to 141 ms on a VMware Fusion VM, and a slow hand-off is logged with the time of each step. The visible session keeps composing on the GPU without ever being root. A session off the monitor draws in memory, and only if someone is watching it remotely.
- **Locking really takes the session off the monitor**; it is not a layer drawn on top. If the user's session hangs, the login screen still works.
- **Boot.** A LordOS GRUB theme draws the boot menu, and a LordOS Plymouth theme draws the splash (the crown with a progress bar). The hand-off has no black frame and no console text: Plymouth stops drawing and gives up display control while its last frame stays on screen, `lord-sessiond` takes display control and sets the first real frame of the login screen, and only then does Plymouth quit, keeping the splash on screen until it is replaced. Since 0.8.5 the splash shows the time since it started, About LordOS → Startup time shows how long this boot took from the kernel's start to the login screen (or the desktop, with automatic login), and every boot is logged with the LordOS version and build (`journalctl -b -u lordos | grep "first frame"`) and kept in `/var/lib/lordos/boot-times.jsonl` (the last 200 boots).

**Remote desktop.** The same service offers the desktop to a browser at `https://<address>:5673` (5673 spells "LORD" on a phone keypad). It is **off by default**, is turned on in Settings → Sharing, and accepts the local network only by default (0.8.3; a list of allowed networks or any address can be chosen, and LordOS's own VPN interfaces count as local). It uses TLS 1.2 or later with a self-signed P-256 certificate whose fingerprint Settings shows for comparison, a strict content security policy, same-origin checks and a session cookie that is `Secure`, `HttpOnly` and `SameSite=Strict`; the login page never lists user accounts. **Three failed sign-ins block the source address** for 15 minutes, then 1 hour, then 24 hours (IPv6 per /64), persisted by `lord-sessiond` alone, with manual block and allow lists; failures are answered at a fixed 1.25 s. Everything from the network goes through strict parsers with unit tests and a fuzz run under ASan and UBSan (0.8.3).

Since 0.8.4 the screen travels as **WebRTC video**: H.264 (VP8 when the browser lacks H.264) with the session's sound in Opus, encoded once per session by `lord-media` (VA-API on the graphics card when it can, x264 otherwise) for every viewer, with still areas made pixel-perfect by lossless tiles. Keyboard, mouse and clipboard go over a WebRTC data channel. Besides UDP, the video connects over TCP on port 5673 itself, so a firewall or VPN that lets only that port through is enough; the page reconnects by itself after an outage, and says why when it really can't connect. STUN servers and a TURN relay (UDP, TCP or TLS) can be added for other networks. The process that serves a connection drops root as soon as it knows whose session it is and keeps only four descriptors. Since 0.8.5 the video follows the browser screen's refresh rate (60, 120, 144 Hz…) instead of a fixed 30, the bandwidth ramps up in seconds instead of minutes, and a top bar shows the frame rate, the encoder and whether the screen is composed on the GPU or the CPU (§7.2). Restart and Shut Down are offered only in the session on the monitor. Internet access is not intended; a VPN is recommended.

**Remote login (SSH).** Settings → Sharing also turns on Debian's OpenSSH server (0.8.1) through closed admin actions. Since 0.8.5 an **advanced, off-by-default option lets root log in over SSH**, with a public key only or with a key or a password, for tests and administration. Only administrators can change it, and every change carries the administrator's own password, checked as root against `/etc/shadow`. The modes write only fixed text to `/etc/ssh/sshd_config.d/05-lordos-root.conf` (read before the shipped `10-lordos.conf`, which keeps `PermitRootLogin no`), checked with `sshd -t`. The only user text written is one public key line validated in C (a known type, a blob that matches it, a printable comment, no options) into `/root/.ssh/authorized_keys`; keys are listed with their SHA-256 fingerprints, and root's password goes to `chpasswd` on standard input. A warning stays on the page while it is on. Tests: `build/ssh-root-test` (17 checks, the same fingerprints as `ssh-keygen -l`) and an end-to-end run as root on VM 116 (wrong password refused, a key added and accepted from another computer, a duplicate refused, the key removed, the mode off, `ssh root@` refused).

### Source files

- [server/src/kms.c](https://github.com/lordbasex/LordOS/blob/main/server/src/kms.c), [server/src/gpu/gpu.c](https://github.com/lordbasex/LordOS/blob/main/server/src/gpu/gpu.c), [server/src/outputs.c](https://github.com/lordbasex/LordOS/blob/main/server/src/outputs.c)
- [server/src/input.c](https://github.com/lordbasex/LordOS/blob/main/server/src/input.c), [server/src/tp](https://github.com/lordbasex/LordOS/tree/main/server/src/tp), [server/src/ui/grab.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/grab.c)
- [server/src/wl/wl.c](https://github.com/lordbasex/LordOS/blob/main/server/src/wl/wl.c), [server/src/ui/portal.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/portal.c)
- [server/src/remote/sessiond.c](https://github.com/lordbasex/LordOS/blob/main/server/src/remote/sessiond.c), [server/src/remote/monitor.c](https://github.com/lordbasex/LordOS/blob/main/server/src/remote/monitor.c), [server/src/remote/sess.c](https://github.com/lordbasex/LordOS/blob/main/server/src/remote/sess.c)
- [tools/boot/plymouth](https://github.com/lordbasex/LordOS/tree/main/tools/boot/plymouth), [tools/iso/grub](https://github.com/lordbasex/LordOS/tree/main/tools/iso/grub), [docs/SESSIONS.md](https://github.com/lordbasex/LordOS/blob/main/docs/SESSIONS.md)

## 4. The UI engine

### 4.1 Not a browser

LordOS's interface is written in JavaScript with React, but there is no web engine underneath. WebKit or Chromium would cost 100 to 300 MB of RAM per process and would not be LordOS's code. Instead, the theme and the apps use a React renderer (`runtime/index.js`) that creates and updates **native nodes** in C: `View`, `Text`, `Image`, plus surfaces for Wayland windows and media. Styles are a subset of CSS (flexbox, colours, rounded and per-side borders, shadows, blur, transforms, transitions) applied by C code that the project controls. The model is close to how a mobile framework renders React to native views, except that the "native" side here is LordOS's own.

### 4.2 Pieces

- **JavaScript:** QuickJS 2025-04-26, a small ES2023 engine in C, vendored as source and embedded in `lord-server`.
- **React:** React 19.3.1 with `react-reconciler` 0.34, bundled with esbuild into each theme. The default theme with every app weighs about 1.8 MB (540 KB compressed) in 0.8.5.
- **Layout:** LordOS's own flexbox implementation (`layout.c`), with wrapping, logical start/end edges and right-to-left mirroring.
- **Painting:** LordOS's own 2D rasterizer with antialiased rounded rectangles, cached shadows, reduced-resolution blur and damage tracking: only the changed region is repainted. On ARM, optional NEON loops speed up blits and masks, verified bit-identical to the C code.
- **Text:** glyphs are rasterized with `stb_truetype`. Shaping is LordOS's own OpenType code (GSUB substitutions and GPOS positioning, colour emoji), with full Unicode bidi, Arabic joining, Indic shaping and the Universal Shaping Engine (409 of 409 test sentences identical to HarfBuzz in `tests/hb_compare.py`), CJK line breaking, Thai word breaking, and per-script font fallback. HarfBuzz, FreeType, fontconfig, fribidi and ICU are not linked by `lord-server`.
- **Input methods:** Korean (hangul), Japanese (kana and SKK) and Chinese (pinyin), in `server/src/ime.c`, with no ibus or fcitx.

[[diagram:frame]]

### 4.3 How JavaScript reaches the system

The JavaScript side calls native services through `lord.*` APIs implemented in C (`server/src/ui/js_*.c`): `lord.fs` (files), `lord.settings`, `lord.net`, `lord.pkg`, `lord.media`, `lord.mail`, `lord.vault`, `lord.fw`, `lord.monitor`, `lord.fan` and others. Every call validates its arguments in C. The rules are strict:

- `lord-server` runs without root, and **no API executes arbitrary commands**.
- `lord.fs` writes only in the home folder and the user's own mounts, has no general "delete" (only moving to the Trash), and refuses the folders the system reads to launch or install something or that store security decisions: `~/.local/share/applications`, `~/.config/autostart`, the installed app folder, `settings.json`, the permissions file, the firewall rules, the blocklists, the mail accounts file and the vault folder. Since 0.8.5 its reading calls reach the whole system read-only as the user (Files → This Computer; the kernel's `/proc`, `/sys`, `/dev` and `/run` stay out, and the system's own permissions decide the rest); writing never widened, and isolated apps keep only their own data folder.
- Anything that needs root goes through `admin.c`: a closed table of actions with arguments validated on the root side, fixed program paths and configuration generated by LordOS, never user text passed to a shell. Passwords go through standard input, never on a command line.
- Long work never blocks a frame: helpers and services are polled from the main loop without waiting.

### 4.4 Languages

The interface ships in English (the default), Spanish and Portuguese, and more languages are added as language packs without code changes. Text goes through the SDK's `t('namespace.key')`, with plurals and locale formats. The whole interface mirrors for right-to-left languages, and the region (`LANG`/`LC_*`) is passed to Linux apps. `node tools/i18n-check.mjs` checks that every key exists in every language and that no third-party brand names appear in the interface.

### Source files

- [runtime/index.js](https://github.com/lordbasex/LordOS/blob/main/runtime/index.js), [sdk/index.js](https://github.com/lordbasex/LordOS/blob/main/sdk/index.js)
- [server/src/ui/js.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/js.c), [server/src/ui/node.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/node.c), [server/src/ui/layout.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/layout.c), [server/src/ui/paint.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/paint.c)
- [server/src/ui/shape.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/shape.c), [server/src/ui/otf.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/otf.c), [server/src/ui/bidi.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/bidi.c), [server/src/ime.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ime.c)
- [server/src/ui/js_sys.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/js_sys.c), [server/src/fsops.c](https://github.com/lordbasex/LordOS/blob/main/server/src/fsops.c)

## 5. Applications

### 5.1 Native apps

Thirty-one native apps ship with LordOS, all React apps on the same engine and SDK: Files, Settings, Terminal, Store, Text Editor, Notes, Calculator, Monitor, PDF Viewer, Fonts, Emoji & Symbols, Scanner, Photos, Player, Screenshots, Camera, Recorder, Passwords, Disks, Backups, Logs, Clock, Help, Mail, Calendar, Contacts, Reminders, LordOS Imager (0.8.4), System Information (0.8.5), About and the installer. Since 0.8.5 each app's manifest carries its own version (all start at 0.8.5; the build refuses an app without a valid one), shown in Store → Installed and in the app's About window, so the apps can later be released apart from LordOS. Some highlights:

- **Files:** since 0.8.5 the sidebar has **This Computer**, to browse the whole system read-only as the user (the system's own permissions decide; `/proc`, `/sys`, `/dev` and `/run` are shown with a lock and not opened), and every disk with a file system: one that isn't mounted shows dimmed and a click mounts and opens it (an internal disk asks for an administrator, as in Disks), and a removable one can be ejected. Folders the user can't write show no actions that would change them (§4.3).

- **Terminal:** LordOS's own xterm-compatible emulator in C, with tabs, split panes, profiles, inline images and Sixel, and sessions that survive a restart of `lord-server`.
- **Monitor:** CPU, memory, disks (with temperature and SMART health, 0.8.2), processes (stop a process, Get Info, 0.8.1), cooling and per-app network connections on a world map, with country lookup from a local database. Monitor → Network's second generation (0.8.5) is a tree from each app to the sites, addresses and ports it talks to, with bytes sent and received, an allow/block switch on every destination, a search with `app:`, `domain:`, `ip:`, `port:`, `proto:` and `country:` chips, and a traffic history from one hour to 30 days. It lists TCP, UDP, ICMP (ping), raw IP and packet sockets under the app that opened them (a `ping` run in the Terminal counts under the Terminal), and says when a program can see or forge traffic below TCP and UDP. One pop-up button groups the view by app, domain, address, port or country and filters it by direction, protocol and rule state, remembered per user. On the map each app has its own color, arcs to a country stay visible, moving dots show the direction of any traffic (orange out, green in), and the map follows the search and the filters. Its sampling thread exists only while the app is open.
- **System Information** (0.8.5, About LordOS → System Report…): every piece of hardware and what LordOS makes of it; the graphics card shown layer by layer (driver, display output, render node, GPU context, composition, direct scanout) with the exact reason when the screen is composed on the CPU; a Compatibility panel; memory modules from the firmware's tables through a read-only admin action that never reads serial numbers; a Report tab with the exact text before it is copied or saved (nothing is sent anywhere); and Checks, with **Run tests** for the Sandbox Probe (§6.4, §11.4). `lordos-sysreport report` prints the same report in a terminal (Markdown or `--json`, with no serial numbers, addresses or names), and `lordos-fetch` shows the LordOS crown beside a summary of the computer and how apps are isolated (`--share` leaves out the user and host names).
- **Test Bench** (`lordos-testbench`, 0.8.5): a test-pattern window, a plain Wayland client, that checks the display server from the outside: frame timing and presentation, every key, the mouse and wheel, the Wayland protocols offered (§3.4), a 3D scene that says whether a real GPU or the processor renders it, generated video at 24, 30 or 60 frames a second and pointer lock. Keys LordOS keeps for itself are outlined; its last report appears in System Information → Checks.
- **LordOS Imager** (0.8.4): writes a signature-checked LordOS image to an SD card or USB stick, never to an internal disk, reads it back to compare, and prepares the Raspberry Pi 5's first boot (name, language, keyboard, account with only a password hash, Wi-Fi with only the derived key, SSH, remote desktop, theme and cooling). The same app exists for macOS, Linux and Windows.
- **Disks:** LordOS's own SMART reader, the full SMART report with smartmontools, formatting, and bootable USB sticks verified with SHA-256.
- **Backups:** hard-link snapshots with optional encryption, running at low priority.
- **The Dock** (0.8.1) is arranged by the user, with pinned apps and the apps' own actions; the default theme gained window snapping, a task view and the **Lord Classic** theme became a second full appearance (0.8.3, §8).

### 5.2 Installable `.lpk` apps

Third-party native apps are packaged as `.lpk`: a gzip-compressed tar with a manifest, the bundled `app.js`, a required icon, optional translations and assets. An app doesn't carry React or the SDK; it uses the system's, which keeps it small (the example Fractal app weighs 5 KB). They install without root into `~/.local/share/lordos/apps/<id>/`. The installer rejects links, devices, absolute paths, `..`, odd characters, more than 2,000 files or 64 MB, and replaces the previous version atomically. Each one runs isolated (§6.4).

### 5.3 The Store and Debian packages

The Store (redesigned in 0.8.5) has a sidebar in three groups (Discover, For this computer, Installed and Updates; Artificial Intelligence and Debian's ten categories with their counts; LordOS apps) and shows apps as cards in a grid that fills the window: icon, name, category, two lines of summary, the download size and a Get, Open or View button. Discover opens with a featured banner, then the AI shelf and a shelf per category; an app's page has a row of facts (version, size, category, distribution). It shows Debian's apps by category with search, `.lpk` apps and local `.deb` files, suggestions for this computer's hardware (missing firmware, printer and scanner drivers, guest tools, 0.8.2; their own page, For this computer, since 0.8.5), and LordOS's own updates (0.8.1), and always shows a plan before installing or removing. Debian packages are handled by **LordOS's own package engine** (`server/src/pkg`): it reads the apt sources, verifies `InRelease` signatures with its own OpenPGP code (RSA and Ed25519), refuses expired, replayed, wrong-suite or trailing-data indexes, checks every index and `.deb` against SHA-256 values from the signed index, and resolves dependencies (plans identical to apt's in 12 test cases). Only the last step is Debian's: `dpkg` unpacks and configures, and remains the only writer of its database, so `apt` keeps working. Since 0.8.5, `https://` sources are fetched over HTTPS through `lord-tls`, a sandboxed helper that checks the certificate chain and the host name and only relays bytes (§6); before, they were fetched over `http://`. Integrity still rests on the signatures and hashes either way; HTTPS adds privacy (the package names downloaded are no longer visible on the network) and refuses a server that isn't who it claims, and a secure download is never redirected to plain HTTP. `lord-tls` is executed by the download child (running as `nobody` in its own download scope) with a fixed path, no arguments, an empty environment and the already-connected socket; it applies the helpers' sandbox before it reads anything, uses TLS 1.2 or later with SNI, never parses HTTP and never decides about packages. The Store's update and installs used it on VM 116 and on a VMware machine. Since 0.8.5 the engine also sends a `+` in a package name as `%2B`, as apt does, so packages such as `kms++-utils` download from lordos.org's CDN.

**Artificial Intelligence (0.8.5).** A Store category built from a curated catalog in the repository (`apt/catalog.json`, with the vendors' keys in `apt/keys/` and LordOS's build recipes in `apt/recipes/`): nine apps for amd64 and arm64, each with its kind, vendor, licence, summary and a pinned source; an app is dimmed on an architecture it doesn't support. Claude Code, Claude Desktop, Visual Studio Code and Cursor install from their vendor's own signed APT repository ("direct"; `apt/build.sh check` verifies the pinned key fingerprint, the signed `InRelease`, the index hash and the package for each architecture): the Store first says whose repository it adds and its key fingerprint, then an administrator action (`pkg-source-add`) writes it with the vendor's key, which must have exactly the fingerprint pinned in the catalog, and fetches the lists; only the app's id comes from the interface, never a URL, key, file name or field. The `.sources` file (with `Signed-By` that key) and the keyring are fixed-format files named `lordos-<id>`, or the vendor's own names when its package always writes its own source (Cursor's `cursor.sources`, because a second entry for the same repository with another `Signed-By` stops apt reading any source); `pkg-source-remove` removes the repository with the app unless another installed catalog app uses it. Vendor packages are never modified; what their maintainer scripts do when installed (VS Code, Claude Desktop and Cursor add their own APT source) is documented in `docs/UPDATES.md`. llama.cpp 0.7.0, whisper.cpp 1.9.5 and codex-cli 0.164.0 are built by LordOS from verified upstream sources in network-less `debian:trixie` containers (codex-cli is OpenAI's static binary with its sigstore signature checked with cosign), and goose 1.56.0 and opencode 1.19.4 are the vendors' packages copied after checking a pinned SHA-256; all five are published in LordOS's own signed repository, built and signed on the maintainer's machine. An app LordOS builds that the local lists don't have yet offers to fetch them again. llama.cpp is built with every CPU variant (the best one is chosen at run time) and a Vulkan backend loaded only when a Vulkan device exists, and downloads models from Hugging Face on first run (`-hf`); no model is bundled. Tested on a VMware machine: llama.cpp installed from the Store and `llama-server -hf ggml-org/Qwen3-0.6B-GGUF:Q8_0` answered over its HTTP API on the CPU at about 33 tokens/s (no Vulkan device); on a GPU it has not been tested on real hardware yet (only the CPU path and the backend loading with software Vulkan were checked). On VM 116, Cursor's and VS Code's sources were added, Cursor installed (its own script rewrote the same file), `apt update` and the Store's own update stayed clean, and it was removed.

**Commercial (0.8.5).** Free-of-charge apps without public source code, from their makers' signed APT repositories with the key LordOS pins: Chrome (amd64 and arm64), Edge and Spotify (amd64). The page says before anything is installed that LordOS can't review or fix what they do. Their packages write their own APT source, so the catalog names the files Chrome and Edge write (`google-chrome.sources`, `microsoft-edge.sources` and their keyrings) and LordOS writes exactly those; for Spotify, which adds `spotify.list` unless that file exists, a catalog field `optout` makes `pkg-source-add` create it empty (only when missing; `pkg-source-remove` deletes it only while it is still empty). Tested on a VMware machine with the admin action and the package engine: the three sources added, the three apps installed (Chrome 156.0.8078.47, Edge 156.0.4314.50, Spotify 1.2.95), `apt update` with no error or warning and the engine's update clean afterwards, and Chrome's window opening in the session. Spotify's package also copies its key into `/etc/apt/trusted.gpg.d/`, which apt trusts for any source without `Signed-By` (Debian's own sources have one). Steam needs i386 multiarch, which the package engine doesn't do yet.

**The featured banner (0.8.5).** Discover's banner can change without a release: `apt/featured/featured.json` (a schema 1 JSON object of at most 64 KB) and `banner.png` (a PNG of at most 2 MB) are checked by `apt-repo.sh` and published signed with the indexes (`main/featured/`). The package engine takes them only from a source whose signed Release says `Origin: LordOS`, checks their hashes, parses the JSON and the PNG signature, and keeps them in `/var/lib/lordos/pkg/`; the interface reads them with `lord.pkg.featured()`, read-only. While the image isn't there the Store draws a skeleton of the banner (it pulses unless Reduce motion is on), and a repository without a banner shows a catalog app instead. Tested on VM 116 against lordos.org/apt-staging.

### 5.4 Linux apps

Linux apps with a `.desktop` file appear in the app launcher. Launching goes through `lord.apps.launch(id)`: the interface can only name an installed `.desktop` file; `lord-server` re-reads it, checks it, builds `argv` from its `Exec` line by the specification and runs it with `execve`, without a shell or caller arguments, inside its own root-owned cgroup scope. Chromium and Firefox are preinstalled in the ISO and the Raspberry Pi image; since 0.8.4 they draw a single bar with their tabs and LordOS's window buttons (one switch in Settings → Appearance brings back the LordOS frame), and an optional extension keeps video smooth without hardware decoding (0.8.2). X11-only apps run through Xwayland (§3.4). **Linux apps are not sandboxed by default**: they run as the user, as on any Debian system. They get attribution, the per-app firewall and the per-app restrictions the owner sets (§6.6), including an optional "no access to your files" view applied by the launcher. Since 0.8.5 a `.desktop` file whose name has a space or another unusual character (the LordOS Imager's is `LordOS Imager.desktop`) gets an id with those characters turned into `-` and is launched from its real file, and **Force Quit…** (Lord Flow's crown menu) ends a Linux or isolated app's own process only, through `lord.wayland.kill`, which accepts only a Wayland client of the same user (an X11 app is asked to close instead).

### 5.5 WebAssembly

LordOS includes its own WebAssembly interpreter in C (`server/src/wasm`), exposed as the standard `WebAssembly` global, so a native app can carry modules compiled from C, C++, Rust or Zig. It covers WebAssembly 2.0 without SIMD (the MVP plus sign extension, saturating conversions, multi-value, bulk memory and reference types), passes 26,290 of 26,290 assertions of the official test suite on the VM, and turns every error into a trap rather than a crash. On the project's example it ran 3 to 5 times faster than the same code in JavaScript.

### 5.6 Installing LordOS

- **Live ISO** for 64-bit PCs, with a graphical installer (UEFI and BIOS).
- **Raspberry Pi 5 image** for SD card or NVMe (§7.4), written and prepared with the LordOS Imager, which also writes a first-run file the board applies once at its first boot (strictly validated; nothing is applied if any part is wrong).
- **On an existing Debian 13**: `curl -fsSL https://lordos.org/install.sh | sh`. The script asks before changing anything, downloads `SHA256SUMS` and verifies its Ed25519 signature with the release key embedded in the script, refuses to continue without a valid signature, checks the `.deb`'s SHA-256, installs it with apt and sets the system up like the ISO. The release files are published on GitHub Releases.

### Source files

- [apps](https://github.com/lordbasex/LordOS/tree/main/apps), [server/src/lpk.c](https://github.com/lordbasex/LordOS/blob/main/server/src/lpk.c), [tools/lpk.mjs](https://github.com/lordbasex/LordOS/blob/main/tools/lpk.mjs)
- [server/src/pkg](https://github.com/lordbasex/LordOS/tree/main/server/src/pkg), [server/src/apps.c](https://github.com/lordbasex/LordOS/blob/main/server/src/apps.c)
- [server/src/wasm/wasm.c](https://github.com/lordbasex/LordOS/blob/main/server/src/wasm/wasm.c), [server/src/ui/js_wasm.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/js_wasm.c)
- [tools/install/install.sh](https://github.com/lordbasex/LordOS/blob/main/tools/install/install.sh)

## 6. Security

### 6.1 Threat model

LordOS is designed against these threats:

- **A malicious or compromised installed app** that wants the user's files, keys, microphone, camera, clipboard or network.
- **Malicious documents and messages**: PDFs, archives, fonts, images, video and mail crafted to exploit a parser.
- **A Linux app that wants to send data out**, or to escape the rules set for it.
- **An attacker on the local network** trying to use the remote desktop or to tamper with downloads.
- **Tampered packages and releases.**

Outside the model, for now: a compromised kernel or root account; physical attacks; side channels; and Linux apps that the owner hasn't restricted, which run as the user, as on any Debian system, and can read the user's files. Linux apps are confined only by the restrictions the owner sets for them (§6.6), and only when they are started from the app launcher.

### 6.2 Least privilege

- `lord-server` runs as the user. In a managed session it never holds the display or input devices itself: `lord-sessiond` lends them and revokes them.
- Libraries that parse untrusted data are linked only by helpers: FFmpeg, libva and the video encoders by `lord-media`; Poppler and libarchive by `lord-doc`; OpenSSL by `lord-maild`, `lord-fetch`, `lord-tls` (HTTPS for the package engine's downloads: it verifies the certificate and the host name and only relays bytes; the signed index still decides about packages) and `lord-sessiond`; libsrtp2 and usrsctp by `lord-sessiond`, used only in the bridge that runs as the user after sign-in; Monocypher by `lord-vault`.
- Helpers receive files and devices as file descriptors opened by `lord-server`; they never open paths.
- The compositor never waits for a helper or an app, so a crash or a hang stays contained.

### 6.3 The root processes and their closed interfaces

| Process | Accepts | Never does |
|---|---|---|
| `admin.c` helper | a fixed table of actions, arguments validated as root | generic actions, user text in a shell |
| `lord-sessiond` | login requests from the greeter; short session JSON (≤ 4 KB) | let the greeter verify passwords; read pixels as root |
| its cgroup placement child | `enter <scope> <limits>` for the calling process only, from a session's `server` leaf, running a binary from `lord-server`'s directory | place arbitrary processes; hand the tree back to the user |
| `lord-netd` | `net-*` actions from root peers only | non-network actions; run anything from a session |
| `lord-netperm` | `RELOAD` with no data | run programs; take arguments; open internet sockets |
| `lord-fand` | `STATUS` for anyone; `PROFILE`, `CURVE`, `FORCE` for administrators | name paths or devices; run programs; use the network |
| the Store's download child | moves its own pid into its root-owned download scope, from its session's `server` leaf, then drops to `nobody` | place any other process |

The admin helper's table gained, in 0.8.5, only closed actions of the same kind: `pkg-source-add` and `pkg-source-remove <id>` (§5.3: only a catalog id from the interface; the entry, its key's fingerprint and every field are checked as root against the root-owned catalog the lordos package ships), `ssh-root`, `ssh-root-key`, `ssh-root-password` and the read-only `ssh-root-status` (§3.6: administrators only, each change with the administrator's own password checked as root), and the read-only `hw-memory-modules` (System Information: SMBIOS memory devices parsed strictly in C, never serial numbers, asset tags or part numbers).

### 6.4 What is isolated, and the app sandbox

Not every program on LordOS is sandboxed. This is exactly what applies to each kind:

| Kind | Process | Sandbox | cgroup scope | Broker and permissions | Firewall and restrictions (§6.6) |
|---|---|---|---|---|---|
| `.lpk` apps, and Passwords, Mail, Calendar, Contacts and Reminders (0.8.2) | own (`lord-server --app <id>`) | Landlock, seccomp, no capabilities | own, root-owned, with limits | every call checked in C; the four PIM apps have only their own allow-listed mail requests | network rules and all eight non-network restrictions, in the broker |
| Other built-in apps | inside `lord-server` | no (they are part of the desktop) | `lord-server`'s | no broker; camera, microphone and screen are checked in C | as "LordOS": network rules; camera, microphone, screen and notifications |
| Linux apps from the app launcher | own | none by default; with "no access to your files", their own user, mount and PID namespaces plus Landlock, from the next launch | own, root-owned | no | network rules; camera, microphone, USB, Bluetooth sockets, files and notifications; not the clipboard |
| Terminal programs | children of the Terminal's shell | no | the Terminal's own component scope (0.8.5) | no | as "Terminal": network rules, with the programs' names in the Monitor |
| X11 apps | under Xwayland | as their launcher entry | own, like Wayland apps | no | the same as Linux apps |

A program started some other way, outside the app launcher, is in no app scope and gets no per-app rules. Flatpak and bubblewrap are not used.

Each `.lpk` app, and the Passwords app, runs as `lord-server --app <id>`: the same binary in "app mode", launched by `lord-server` with a validated package id as its only argument. The process connects to the compositor as a Wayland client and opens the broker socket, then applies the sandbox, and only then loads the app's code (`server/src/sec/sandbox.c`):

- **Landlock** (ABI up to 6): read-only access to the package, the shared app runtime, fonts and libraries; read-write only to `~/.local/share/lordos/appdata/<id>`; scoping of abstract Unix sockets and signals.
- **seccomp-bpf**: an allow list of system calls on every thread (TSYNC), without `ptrace`, `mount`, `kexec`, `bpf`, `perf_event_open`, `keyctl`, module loading or `execve`. New Unix sockets and `socketpair` are refused, because Landlock up to ABI 6 doesn't filter `connect` to named Unix sockets; this keeps the app away from the session bus, the print service and the Wayland socket.
- **No privileges**: `no_new_privs`, an empty capability set and a dropped bounding set.
- **cgroup v2**: `app-lordos-lpk-<id>.scope`, created by root, with `memory.high` 192 MB, `memory.max` 512 MB and one CPU core.
- **Network**: without the `network` permission (declared **and** allowed), seccomp refuses internet sockets and Landlock refuses TCP connect and bind. Network namespaces are not used, because creating one needs privileges `lord-server` doesn't have.

If a kernel feature is missing, the app still starts and the log says which part is missing. The deliberately malicious test app (`tests/lpk-evil`) fails at everything it tries; `build/sandbox-test` passes 14 of 14 checks for apps and 14 of 14 with network. Measured on a VM: about 13.7 MB of RSS per isolated app, a median start of 106 ms, and 0.89 ms to apply the sandbox.

**On the user's own computer (0.8.5).** System Information → Checks → **Run tests** starts the hidden system package `sysprobe` the way any isolated app starts; in app mode, for that package only, `server/src/app/probe.c` takes the same sandbox and tries what must fail: the home folder, `/etc/passwd`, the root cgroup's `cgroup.procs`, an internet and a Unix socket, `ptrace`, a user namespace, `mount` and `exec` (its own data folder must work, as a control). A child forked before the sandbox takes the decoding helpers' profile and tries a socket, `exec`, `fork` and files; and the probe asks its broker for what must be refused (a path outside its folder, the clipboard without permission, the security key, mail). `lord-server` adds what only it can read: the process's `NoNewPrivs`, `Seccomp` and `CapEff` and its cgroup's `memory.max`. On VM 116 every try was refused (9 + 4 + 4), with seccomp mode 2, no new privileges and no capabilities. The probe never gets a permission or a network grant.

### 6.5 The broker, permissions and the file dialog

[[diagram:permission]]

The isolated app talks to `lord-server` only over one `SOCK_SEQPACKET` socket. Every request is checked in C: file access only inside the app's own data folder; clipboard reads only with the `clipboard` permission; permission requests answered from the stored decision or, if none, by a **system-drawn prompt** (*Allow / Don't Allow*); camera and microphone only through `lord-server`.

- **Decisions** are stored per user in `~/.config/lordos/permisos.json`, outside the app's reach. The lookup is O(1).
- **The file dialog is LordOS's own powerbox** (`sdk/powerbox.jsx`), not `xdg-desktop-portal`: `lord-server` draws it, opens the chosen file and passes only that descriptor. The only desktop portal interface LordOS implements is `org.freedesktop.portal.Settings`, for GTK theming.
- **Microphone:** `lord-server` owns the PipeWire stream and gives the app a read-only shared ring.
- **Camera:** `lord-server` opens the device and gives it only to `lord-media`; the app gets a read-only view of the decoded frames and must acknowledge each one, and `lord-server` validates every acknowledgement.
- **Revocation:** denying the camera or the microphone in Settings stops it at once (the app's `lord-media` is killed; the PipeWire stream is closed). The `network` permission is decided at launch.
- **Keyring:** an isolated app's keyring requests carry an owner that `lord-server` sets from the verified app id.

**Restrictions beat permissions.** A firewall restriction (§6.6) wins over any stored decision: the request is refused without a prompt, even if the app was allowed before, and restricting a running app takes its camera and microphone away at once, like revoking.

**Limits.** There is no "allow once". Some declared permissions (Bluetooth, launching apps, notifications) have no broker call yet, so isolated apps can't use those capabilities at all; listening on a port is governed by the network permission. A Linux app running as the user could edit the permissions file, unless it is restricted with "no access to your files".

### 6.6 The per-app firewall, its restrictions and the Monitor

[[diagram:firewall]]

`lord-netperm` enforces per-app rules in the kernel. Linux apps opened from the app launcher run in `app-lordos-<id>.scope`, `.lpk` apps in `app-lordos-lpk-<id>.scope`, and the rest of the session counts as `lordos`. All scopes are root-owned. The service loads seven network programs (`connect4/6`, `sendmsg4/6`, `bind4/6`, `sock_create`), a device program (`cgroup/dev`) and a per-cgroup LSM program on `socket_create` into the cgroup of `lordos.service`, finds each process's app by its cgroup ancestor, and returns allow or deny (`EPERM`). The programs are compiled into the binary and loaded with LordOS's own loader over the `bpf()` system call, which also reads the kernel's BTF to find the LSM hook; libbpf and bcc are not used. If the kernel refuses the device or the LSM program, the network programs still work and the service says so.

- **Rules** support exact domains, wildcards, IPv4/IPv6 networks, `lan`, `internet`, ports, protocols and direction. Restrictions (no internet, no LAN, no network, no incoming) come first; then the most specific rule wins, with deny beating allow at equal specificity; then trusted apps; then the mode (silent, the default, or off). `npcompile.c` precomputes this into hash and longest-prefix-match maps. A test runs the kernel's decision code against the reference decision maker over 480,000 random connections: 0 differences.
- **Domain names** come only from the system resolver: systemd-resolved's monitor interface or, without it, replies from port 53 of the configured servers read by a receive-only packet socket. Answers an app received on its own sockets are never trusted. So an app that resolves names itself with encrypted DNS (DNS over HTTPS inside a browser, for example) isn't matched by domain rules: an "allow only example.com" rule still blocks it, but a "block example.com" rule doesn't stop it. Address and network rules, the app's default and its restrictions still apply, and Settings says so under the rules.
- **Rules in the interface.** Settings → Privacy → Network rules by app shows each app's default ("allow all" or "block all except rules") and its rules in the order they win, with when each one last matched; the sheet that adds a rule checks what is typed as it is typed. In Monitor → Network, "Allow / Block this domain" on one of an app's destinations opens the same sheet, prefilled with the name the system resolver gave for that address.
- **Escapes.** The cgroup tree is root-owned, so an app can't move itself. A process that asks the user's systemd manager for a new unit (`systemd-run --user`) lands under `user@<uid>.service`; `lord-netperm` attaches the same programs there and holds such processes to the strictest union of that user's running restricted apps. Tested: all escape attempts denied, other users unaffected.
- **Cost**: about 0.5 µs per UDP connect, TCP within noise, no change in throughput, since nothing runs per packet.
- **The Monitor** shows each app's connections (from `sock_diag`, plus ICMP, raw and packet sockets from `/proc/net`) and the destinations the firewall reported recently, bytes, destinations by country from a local database, and a per-app Rule switch. No address ever leaves the machine for lookups: no reverse DNS, no online geolocation.
- **Open connections (0.8.3).** Rules apply to connections already open, not only new ones: the programs also drop packets a rule now denies, and `lord-netperm` destroys those sockets over `sock_diag` (`SOCK_DESTROY`), so a blocked app sees its connections end within a fraction of a second (0.04–0.37 s for TCP and 0.08 s for QUIC on a Pi 5 with the LordOS kernel).
- **LordOS's own components (0.8.5).** Mail and calendar sync, the Store and updates, printers and scanners, the Terminal and the blocklist updates each run in a scope of their own, so each can be seen and blocked separately; their sockets are opened by a small connector that `lord-server` re-executes with fixed arguments, which enters the component's scope and hands back only a connected socket. A connector (`lord-server --net-connector mail|print|lists`) has a fixed argument list and environment, `no_new_privs`, isn't dumpable, answers only fixed-size requests (family, type, address, port) with no names, paths or execution, and fails closed: if it can't enter its scope it exits and the request fails. The Store's download child enters its own scope in the same spirit before dropping to `nobody` (§6.3). Name resolution, time synchronization, the network service and VPN tunnels are shown and never blocked; blocking the Store asks first, because security updates stop.
- **Blocklists and "deny silently" (0.8.5).** Public lists of advertising, tracker, malware and attacking sites (each with its source and licence) are downloaded over HTTPS once a day by `lord-fetch`, a helper with no sockets of its own and the certificate checked against the system's authorities, and every line is validated again; a rule the user writes for a site always wins over a list. A new firewall mode, "deny silently", lets only what a rule allows connect and records the rest, while the computer's own services keep working. An optional menu bar item shows the traffic rates and what was recently denied.

**Restrictions beyond the network.** Besides the four network restrictions (no internet, no LAN, no network, no incoming connections), the owner can restrict an app from the camera, the microphone, the screen, USB, files, the clipboard, Bluetooth and notifications. Each one is enforced where the resource is handed out, checked against the app's scope and never against a name the app gives itself, and it beats any permission granted in `permisos.json`.

[[diagram:restrictions]]

| Restriction | `.lpk` apps and Passwords | Built-in apps ("LordOS") | Linux apps from the app launcher |
|---|---|---|---|
| camera | broker (`brokercam.c`) | `js_media.c` | `cgroup/dev` on V4L2 nodes, and the PipeWire guard |
| microphone | broker | `js_media.c` | the PipeWire guard, and `cgroup/dev` on ALSA capture nodes |
| screen | broker (no capture API in app mode) | `js_media.c` | by design: no capture protocol, no X server, no screen-cast portal; `/dev/fb*` denied |
| USB | broker (the security key of Passwords) | not offered | `cgroup/dev`: raw USB, hidraw and USB serial nodes |
| files | broker (file picker, camera saves) | not offered | the launcher: own namespaces, a tmpfs home and Landlock, from the next launch |
| clipboard | broker | not offered | not available |
| Bluetooth | broker (no Bluetooth API in app mode) | not offered | BPF LSM program on `socket_create`: no `AF_BLUETOOTH` sockets |
| notifications | the `notifications` permission | `sdk/notify.js` | `notifyd`, by the caller's verified identity |

For isolated apps the broker (`sec/restrict.c`) is the single enforcement point: a restricted request is refused without a prompt, and restricting a running app takes its camera and microphone away at once (`tests/broker_restrict_test.c` covers every restriction, with and without it). The "LordOS" row is the desktop itself (Files, copy and paste, Disks, Bluetooth settings), so it doesn't offer files, USB, clipboard or Bluetooth; individual apps are restricted instead. For Linux apps:

- **Devices.** In cgroup v2 the device controller is a BPF program. `lord-netperm`'s refuses `open()` and `mknod()` of the device classes the app is restricted from (V4L2, raw USB, hidraw, USB serial, ALSA capture nodes, `/dev/fb*`) and allows everything else. It applies to the next `open`; a descriptor already open stays usable.
- **Bluetooth sockets.** The inet socket hook never sees `AF_BLUETOOTH`, so a per-cgroup LSM program on `socket_create` refuses them.
- **Files.** Right before `execve`, the launcher's child enters its own unprivileged user, mount and PID namespaces. The home folder is an empty tmpfs holding only the app's own folders (derived from its desktop id, never from fields a `.desktop` file controls) and the file it was opened with; the runtime folder holds only the Wayland and sound sockets, so the session bus and the user's systemd are out of reach; then Landlock limits the rest. It applies at the next launch. GTK apps, Firefox and Chromium run without their D-Bus integrations.
- **Notifications.** `notifyd` asks the bus for the caller's PID (only an answer the bus itself sent counts), finds its app scope, and drops the notification of a restricted app; an error or a late answer drops it too (fail closed).
- **Microphone and camera through PipeWire.** The PipeWire guard (`sec/pwguard.c`), `lord-server`'s own PipeWire client, identifies each client by the PID PipeWire took from the socket's credentials and, for a restricted app, hides capture nodes and destroys its capture streams within a second, a recording already running included. While a restricted app runs, a client whose identity can't be established is treated as restricted.

**Limits.** "Ask" mode is designed but behaves as silent today. ICMP is blocked only by "no network"; connections to an already-open port aren't checked. The restrictions for Linux apps don't cover:

- BlueZ over D-Bus: a Bluetooth-restricted app can still ask `bluetoothd` on the system bus to scan, pair and connect;
- the system bus, which stays reachable even with "no access to your files";
- the clipboard;
- apps started outside the app launcher, which are in no app scope;
- side effects of the `user@` union: while a restricted app runs, the user's own services are held to the same union (a headset's call audio, which PipeWire opens per call, stops for that time);
- a window of a few milliseconds in which a new PipeWire client of a restricted app has full permissions, or in which a capture stream can receive audio before it is destroyed.

### 6.7 Helpers that process untrusted data

[[diagram:helpers]]

`lord-doc` (PDF, archives, fonts, scanner images), `lord-media` (video, audio, photos, camera), `lord-maild` (mail, calendars, contacts), `lord-vault` (secrets) and, since 0.8.5, `lord-fetch` (the firewall's blocklists) and `lord-tls` (HTTPS for the package engine) share one hardened sandbox, `sandbox_apply_helper()`:

- no `socket`, `socketpair`, `connect`, `bind`, `listen`, `accept`; no `execve`; `clone` only for threads; seccomp on every thread, and if seccomp can't be applied the helper exits;
- files only through descriptors passed by `lord-server`; ioctls only by class (DRM, V4L2, DMA-BUF) where needed;
- results returned in sealed shared memory, validated by `lord-server`.

`lord-media` doesn't even receive the PipeWire socket; audio flows through a shared ring whose stream belongs to `lord-server`. `lord-maild` can't create sockets: it asks `lord-server`, which connects only to servers in the accounts file (and, after the user asks to load images, only to public addresses on ports 80/443 for ten minutes) and hands over the connected socket. `lord-fetch` and `lord-tls` have no sockets of their own either: each receives one socket already connected (by the `lists` connector, or by the Store's download child), checks the server's certificate against the system's authorities and its host name with TLS 1.2 or later, and is never relaxed; `lord-fetch` makes one HTTP GET of at most 16 MB, parses the list strictly and sends redirects back to `lord-server` (at most three, https only), which validates every line again. `build/sandbox-test helper` passes 17 of 17 checks.

### 6.8 Package signing and trust levels

`.lpk` packages carry a manifest of every file's SHA-256 and an Ed25519 signature over it. SHA-256 and Ed25519 are LordOS's own code (Ed25519 following TweetNaCl's arithmetic), tested against NIST and RFC 8032 vectors (55 of 55). Verification happens in C at install and at load. The Store shows the trust level: **LordOS** (official key, compiled into the binary so no file can add one), **Verified author** (listed in the signed catalog), **Signed by <name>**, or **Unsigned** with a warning. Updates must be signed with the same key. A signed catalog carries a revocation list. If you edit an installed app, LordOS marks it "Modified by you", can restore the original, or can share your version signed with your own key, with credit to the original.

**Status:** official `.lpk` packages are still signed with a development key, until the final key replaces it. A signed LordOS app repository for `.lpk` apps is not built yet; LordOS itself updates from a signed APT repository (§6.9).

### 6.9 Release signing

Every release file (the `.deb`, the ISO and the Raspberry Pi image) is listed with its SHA-256 in `SHA256SUMS`, and `SHA256SUMS` is signed with the LordOS release key (Ed25519). The public key is `tools/release/lordos-release.pub.pem`, also embedded in the Debian installer. Its fingerprint (SHA-256 of the DER public key) is:

`c7155b8e6bbbb180f907381994d06af76d333016cd6f3facf67cadd053ea108e`

The private key lives only on the maintainer's machine, never in the repository or on a build machine. Each release also carries an SPDX 2.3 software bill of materials (SBOM) per architecture, listed in `SHA256SUMS` and so covered by the signature, and its release notes come from `CHANGELOG.md`, kept in the Keep a Changelog format. §11 shows how to check a download.

- **The APT repository (0.8.1).** `lordos.org/apt` is a signed APT repository built and signed only on the maintainer's machine; installed systems update from it. On the Raspberry Pi 5 it also mirrors the bootloader updater and the few Raspberry Pi packages it needs, copied only after their signature and every file's SHA-256 match a pinned list (0.8.4.1).
- **LordOS's own code is verified before it runs (0.8.2).** Every JavaScript bundle LordOS runs as its own code (the themes, the app host, the system apps) is loaded only through `codesig_load`, which checks it against `code-manifest.json` and its Ed25519 signature by the release key. A modified bundle is not loaded and the user sees why; System Information's Checks report it (§11.4). Development builds without a signed manifest say so.
- **System apps can't be removed** from the Store (0.8.2).

### 6.10 The vault

`lord-vault` keeps passwords and the session keyring in `.lvault` files: one random 32-byte vault key, wrapped once per unlock method (Argon2id password with a cost calibrated per machine, FIDO2 `hmac-secret` with LordOS's own CTAP2 client, key file, password plus key file, recovery key); content sealed with XChaCha20-Poly1305 with the whole header as associated data; attachments sealed separately; atomic saves with a backup. Every flipped byte and every truncation is detected (1,165 of 1,165 in the test). Secrets live in an `mlock`ed arena excluded from core dumps and wiped on free, in a process that is not dumpable. The Passwords window runs in its own sandboxed process, so revealed values never enter the desktop's JavaScript. The keyring opens at login with the password `lord-sessiond` verified, and serves Linux apps through LordOS's own Secret Service implementation. Since 0.8.5 the session starts only once the user's session bus exists (§3.6), so `org.freedesktop.secrets` and `org.freedesktop.Notifications` belong to the LordOS session from the first second; and when an app pulls in gnome-keyring, a drop-in shipped by the lordos package (`gnome-keyring-daemon.service.d/lordos.conf`) keeps only its PKCS#11 component, so LordOS's keyring keeps the Secret Service and no second "choose a password for the new keyring" window appears. Tested on the VMware machine after a reboot with automatic login: both names belong to the LordOS session, and Chrome asks through LordOS's own keyring prompt. When several apps ask at once, one answer in the keyring prompt now goes to all of them. Real hardware security keys are not yet tested.

### 6.11 Protected windows

A window marked protected (Passwords is) is painted black in every path that copies pixels out: screenshots, recordings, thumbnails, the remote desktop, and Wayland menus and fullscreen. The real monitor shows it normally. Linux apps have no screen-capture protocol to begin with. Since 0.8.2, keys typed into LordOS's own prompts (the keyring, administrator passwords) never reach the Linux app in front.

### 6.12 Disclosure and standards

Vulnerabilities are reported as described in `SECURITY.md`, with a machine-readable contact at `https://lordos.org/.well-known/security.txt` (RFC 9116). LordOS's processes are **aligned with, not certified against**, ISO/IEC 29147 and ISO/IEC 30111 (vulnerability disclosure and handling), ISO/IEC 5230 (OpenChain license compliance: the inventory in `THIRD_PARTY.md`, the license notices and the SBOM) and ISO/IEC 18974 (OpenChain security assurance: the SBOM plus Debian's security updates for the packages LordOS depends on). lordos.org publishes the legal and trust pages (security policy, code signing, licenses, privacy, support lifecycle, standards) and says what is still missing: no formal self-certification, no automated matching of the SBOM against vulnerability databases, no SBOM for the whole images, and no third-party audit.

### Source files

- [server/src/sec/sandbox.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/sandbox.c), [server/src/sec/brokersrv.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/brokersrv.c), [server/src/sec/perms.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/perms.c), [server/src/sec/cgroup.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/cgroup.c)
- [server/src/netperm/np.bpf.c](https://github.com/lordbasex/LordOS/blob/main/server/src/netperm/np.bpf.c), [server/src/netperm/netperm.c](https://github.com/lordbasex/LordOS/blob/main/server/src/netperm/netperm.c), [server/src/fw/rules.c](https://github.com/lordbasex/LordOS/blob/main/server/src/fw/rules.c)
- [server/src/sec/restrict.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/restrict.c), [server/src/sec/pwguard.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/pwguard.c), [server/src/hw/notifyd.c](https://github.com/lordbasex/LordOS/blob/main/server/src/hw/notifyd.c), [server/src/apps.c](https://github.com/lordbasex/LordOS/blob/main/server/src/apps.c), [server/tests/broker_restrict_test.c](https://github.com/lordbasex/LordOS/blob/main/server/tests/broker_restrict_test.c)
- [server/src/sec/sign.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/sign.c), [server/src/pkg/crypto.c](https://github.com/lordbasex/LordOS/blob/main/server/src/pkg/crypto.c), [tools/release/sign-release.sh](https://github.com/lordbasex/LordOS/blob/main/tools/release/sign-release.sh), [tools/release/make-sbom.sh](https://github.com/lordbasex/LordOS/blob/main/tools/release/make-sbom.sh)
- [server/src/vault/vault.c](https://github.com/lordbasex/LordOS/blob/main/server/src/vault/vault.c), [server/src/vault/vfile.c](https://github.com/lordbasex/LordOS/blob/main/server/src/vault/vfile.c)
- [server/src/media/capture.c](https://github.com/lordbasex/LordOS/blob/main/server/src/media/capture.c)
- [SECURITY.md](https://github.com/lordbasex/LordOS/blob/main/SECURITY.md), [docs/COMPLIANCE.md](https://github.com/lordbasex/LordOS/blob/main/docs/COMPLIANCE.md), [CHANGELOG.md](https://github.com/lordbasex/LordOS/blob/main/CHANGELOG.md)

## 7. Performance

All figures below were measured by the project and are documented in the files cited. Most come from virtual machines; real-hardware measurements are noted as such.

### 7.1 Memory and idle

**Two different magnitudes.** The first rows measure the **whole system**: the "used" column of `free`, which is everything the kernel, Debian's services (systemd, journald, udev, the network, PipeWire, CUPS…), LordOS's root services and the user's session hold, without the page cache the kernel can give back. The other rows are the **resident memory (RSS) of one process**, as `ps` or `/proc/<pid>/status` report it. An RSS is not the whole system's consumption, and RSS figures can't be added up: pages of shared libraries are counted in every process that maps them. Comparing LordOS with another desktop means comparing `free`'s "used" on the same machine, after the same time idle, with the same apps open; a fair split of the shared pages per process is PSS (`/proc/<pid>/smaps_rollup`). How to repeat both, and the results of each release, are in §11.5.

| Measurement | Value | Where |
|---|---|---|
| Whole system, live ISO, idle after a minute (4 GB, 4 cores) | 465 MB used, CPU 99 % idle | VM 117, `docs/ISO.md` |
| Whole system, installed from the ISO, idle | 487 MB used, CPU 99 % idle | VM 120, `docs/ISO.md` |
| Boot, live ISO with UEFI | 16.3 s (4.9 s kernel + 11.4 s userspace) | VM 117, `docs/ISO.md` |
| `lord-server` without a 3D card (Mesa not loaded) | about 32 MB RSS | `docs/GPU.md` |
| A user session (`lord-server`), remote desktop tests | about 50 MB; login screen 41 MB | VM 118, `docs/REMOTE.md` |
| One isolated `.lpk` app | 13.7 MB RSS | VM 122, `docs/PRIVACY.md` |
| Whole system, 0.8.5~rc1 updated from 0.8.4.1, a minute after sign-in (VMware VM, 4.7 GB, virtual GPU) | 606–622 MB used; PSS of all processes 316 MB, of which the session's `lord-server` 110 MB and the login screen's 78 MB (in that build it stayed for switching users and locking) | `results/0.8.5~rc1-beta/vmware-fusion` |
| The login screen closed after sign-in (0.8.5 fix, same kind of VMware VM) | whole system 603 → 548 MB used | `CHANGELOG.md`, 0.8.5 |
| Whole system, 0.8.5-beta updated from 0.8.4.1, about five minutes after a restart with automatic login (same VMware VM, GPU composition on SVGA3D; Chrome, VS Code, Spotify and llama.cpp installed, not running) | 592 MB used | `results/0.8.5-beta/vmware-fusion` |
| Boot, the same 0.8.5-beta machine | first frame on the monitor 7.4 s after the kernel started (`systemd-analyze`: 3.3 s kernel + 4.3 s userspace) | `results/0.8.5-beta/vmware-fusion` |

### 7.2 Drawing

- **Software vblank (0.8.5).** A virtual card that completes page flips at once (VMware's `vmwgfx`) made the compositor and its apps draw 500–580 frames a second on a 60 Hz display; LordOS now detects it and paces frames to the refresh on a fixed grid: 59.8 frames a second, p99 18.4 ms, no frame over budget (`results/0.8.5~rc1-beta/vmware-fusion`). On the released 0.8.5-beta, the Test Bench with its 3D view on the same machine measured 59.83 frames a second over 1,796 frames, p99 17.9 ms, one frame over budget, 1,795 presented (`results/0.8.5-beta/vmware-fusion`). The same grid also paces sessions without a monitor, so waiting in whole milliseconds no longer turns 60 frames a second into 57.
- **Dragging on the GPU path (0.8.5 fix).** A dragged window is painted from a cached layer; on the GPU path every fully transparent pixel of that layer was taken for a Linux window's hole, and now only pixels inside an app surface are. In 0.8.5-beta small black squares could still show at a dragged window's rounded corners, on the CPU and GPU paths: the layer's corner restore took the saved pixel as opaque. In 0.8.6 (in development) it mixes both pixels alpha included, so the layer stays transparent outside the curve; a test compares each corner with a full repaint on both paths (§10.2).
- **A cheaper drag frame (0.8.6, in development).** A dragged window's frame now paints only the part of its layer that changed and only the changed part of what is below it, with the same pixels: 2,417 recorded frame hashes per path must match, and the one approved change brings rounded corners below the window one level closer to a full repaint. On the GPU path the window itself can be composed by the GPU with the CPU's exact arithmetic, after a self-test of every pixel combination ("GPU drag"), and the next frame can be painted while the previous flip is pending; both were measured on the Raspberry Pi 5 and are off by default. Results on the Raspberry Pi 5 are in §7.4.
- Dragging a window on one 1280×800 display on the CPU path: about 6.7–6.9 ms of work per frame, around 120 frames per second (VM 118, `docs/DISPLAYS.md`).
- The same window on a 2560×1600 display at 150 %: about 5.1 ms per frame.
- Video: painting straight from the decoded frame when only an opaque video changed brought `lord-server` from 42 % to 13 % of a core for a windowed 1080p video, and from 92 % to 15 % fullscreen (`docs/ROADMAP.md`, media).
- The Monitor closed costs nothing: its sampling thread doesn't exist until opened (`docs/MONITOR.md`).
- **The CPU path in 0.8.5** (VM 116, no GPU, screen composed on the CPU, viewed through the remote desktop from a browser with a 60 Hz screen; `docs/REMOTE.md`, "Measurements (0.8.5, CPU reference)"): the backdrop blur of a window (radius 20) went from 27.7 ms to 6.0 ms on one thread and 2.7 ms on eight, with identical pixels; a 1280×800 test scene from about 18 to 55–58 frames a second; a 1920×1080 session playing a 60 fps clip from 45 (one core at 100 %) to 55–58 frames a second on the thread pool. A 1080p, 24 fps video in the browser played in real time with no frame dropped by the player over 17 s. The software H.264 encoder takes 12–13 ms a frame at 1280–1568 px wide, so it tops out near 75–80 frames a second. AV1 at 60 fps remains the hard case without a GPU: the browser decodes it in software on the same machine that composes the screen.

### 7.3 GPU

On the VMs only a software renderer was available. With it, the CPU path is cheaper than "GPU" composition, which is why LordOS uses the CPU path when the only renderer is a software one. The GPU path's real benefit (composition and present close to free, zero-copy fullscreen) is expected on real cards, but **real AMD and Nvidia hardware has not been measured yet**. Since 0.8.5 two tools report it on any machine: System Information shows each GPU layer and the exact reason when the screen is composed on the CPU, and the Test Bench draws a 3D scene with EGL and says whether the renderer is a hardware GPU, a virtual one or software (on VM 116: `llvmpipe`, software), with its draw time and whether its frames reach the screen without a copy.

### 7.4 The Raspberry Pi 5

The Raspberry Pi 5 is the only supported board. Results on a real board (16 GB, SD card, without a monitor unless stated), from `docs/RASPBERRY.md`:

| Measurement | Value |
|---|---|
| Building every LordOS binary on the board | 1 min 02 s, 0 warnings |
| `make -C server test` | 23 passed, 0 failed, 0 skipped |
| NEON blit and mask vs. plain C | 9.90 → 4.33 ms and 11.09 → 5.49 ms per screen (2.0–2.3×), bit-identical |
| Idle at the login screen, headless | `lord-server` 36 MB RSS, 0.6 % CPU; system 450 MB of 16 GB; 38–43 °C, no throttling |
| Building the SD image on the board | 8 min 18 s; 503 MB compressed |
| Building the LordOS kernel on the board | about 50 min per flavour |

**The LordOS kernel.** The Raspberry Pi's own kernel doesn't build Landlock. Without it, the document-helper hostile test fails 5 of its 25 filesystem checks and the sandbox test can't run. LordOS builds the Raspberry Pi kernel from its own source and default configuration, changing only three options (Landlock on, the LSM list, the version suffix), and pins it so a newer archive kernel can't replace it. The image also enables the memory cgroup, which Raspberry Pi turns off at boot.

**Fans.** On the Pi 5 in a test case, `lord-fand` kept the fan off at 50–53 °C during a 4-thread load (the aluminium case acts as a heatsink), drove it to 55 % (4,500 RPM) and full speed (7,400 RPM) with test curves, and restored the kernel's own settings after `SIGTERM` and `kill -9` (`docs/FANS.md`).

**Monitor and image.** Since 0.8.4.1 the Raspberry Pi 5 image shows the desktop on an HDMI monitor at the first boot (the display driver line in `config.txt` now comes after the firmware's auto-detection lines, and LordOS waits up to 30 s for the driver), sets the clock from the network, and ships Chromium and Firefox ESR. Since 0.8.4 the LordOS Pi kernel (`6.18.57-lordos.3-rpi-2712`) also destroys the connections of a blocked app. **Measured on a monitor (LordBench, 0.8.6 in development):** the 0.8.5 image booted with a 1920×1080 60 Hz monitor on either HDMI port reaches the login screen 5.99 s after the kernel starts (systemd: 2.1 s kernel, 4.2 s userspace). With GPU composition (V3D) a window moved by the compositor keeps 59.4 frames per second, with a frame built in 10.9 ms (median) and 13.0 ms (99th percentile) and 1.0 % of frames late; under an all-core load at the lowest priority it falls to 24 frames per second with 96.5 % late, a problem LordBench found: the compositor's cgroup weighed the same as the load's, so the load's low priority did not count. Giving the compositor ten times the CPU weight of an app raised the same test, run without the monitor, from 43–45 to 56–57 frames per second (31–33 % → 7–8 % late) and brought a pointer move to the screen in 22 ms instead of 32 ms (median); with the monitor it was measured only together with the real-time priority below. The compositor's main and paint threads now also run at the lowest real-time priority, which lord-sessiond allows and lord-server drops before it starts anything, so no app inherits it; on VM 116 under load that took late frames from 2.7–2.9 % to 0.28 % and the slowest 1 % of pointer moves from 31–32 to 25 ms, as without load, with the load getting the same CPU time. On the Pi 5 with its monitor the two changes took the loaded test from 24 to 29–30 frames per second and the compositor no longer waits for a CPU, but under the load a frame takes 15–16 ms to build instead of 7, so it misses the 60 Hz refresh; that cause is still being looked for. LordBench's scene had a square meant to turn that stood still, because it was set through a style key the engine lacks; it now turns, which makes the test heavier: with it, that code showed 53–58 frames per second alone and 24–25 under the load. In development for 0.8.6, a dragged window's frame paints only what changed (the changed part of the window's layer and of what is below it), with the same pixels, checked frame by frame against recorded hashes; with that, the same test shows 59.8–60 frames per second alone (99th percentile 11.3–11.6 ms) and 30 under the load, where a frame is built in 12.9 ms (median) instead of about 20. Under the load nearly every frame is still late. A final run of the development build (2026-10-29) measured the two remaining changes. Composing the dragged window on the GPU passes its self-test on the Pi's GPU and gave the CPU's exact pixels in all 138 frames compared, but no measurable gain (30.8–31.6 frames per second under the load, against 30.5–31.1 without it). Painting the next frame while the previous one waits for the flip gave 33–36 frames per second under the load, but a pointer move reached the screen 17 ms later alone (42 instead of 25.5 ms, median) and 29 ms later under the load (78 instead of 49 ms). Both stay in the code and are off by default. With the final defaults the Pi 5 shows 59.9 frames per second alone and still goes from 60 to about 31 (30.5–31.1) under an all-core load. The remaining cost is the GPU's time per frame, which goes from 1.5 ms alone to 14–16 ms under the load, while the clocks and the temperature (36.7–47.2 °C) show no clock or thermal throttling; memory contention between the GPU and the busy cores is the likely cause, not yet proven. Sending and composing fewer pixels per frame, finding what slows the GPU under load, and composing the dragged window on the GPU for the whole drag (also where the Dock or the menu bar overlap it) moved to 0.8.7. The board's under-voltage alarm is present and read 0 with the official power supply. In a 3-minute all-core load with the fan the board kept 5.9 GFLOP/s at 55.6 °C at most, and it decodes 1080p video in software at 258 (H.264), 246 (HEVC), 204 (VP9) and 159 (AV1) frames per second.

### 7.5 Modern PCs

There are no measurements on real PCs in the repository yet. The ISO is tested on virtual machines (UEFI and BIOS); testing on a real AMD machine from the live USB is planned. LordBench (0.8.6) is designed to measure memory and CPU at idle, boot time, frame times, video and app start in a repeatable way, and community hardware reports (0.8.7) to collect them from real machines (`docs/BENCHMARK.md`). In development for 0.8.6 (unreleased; the latest release is 0.8.5-beta), its first part is built: a sandboxed `lord-bench` helper that measures the CPU, memory, the GPU offscreen, storage and a sustained load, stops near the temperature limit or when the processor throttles, and records the conditions of each run, shown in System Information → Performance as "×N" of reference machines; it also measures software video decoding and the remote desktop's encoders, the desktop's frame times and the time from a pointer move to the screen, alone and under a low-priority load, and boot time over several restarts (run on the Pi 5 with 3 boots of 7.6–7.7 s, started without the app's button); it also runs from a terminal (`lordos-sysreport bench`). Also in development, it times LordOS's own interface work with the engine's real code (the layout and paint of a fixed full-HD desktop scene, and the shaping of paragraphs in Latin, Arabic, Devanagari, Thai and Chinese with fonts built into the helper, which opens no files; on the reference virtual machine 37 frames per second on one core and 40 with the paint threads, 619 shaping runs per second on one core and 3,595 on eight; on the Raspberry Pi 5, in a full run that is now part of its reference, 39 and 45 frames per second and 628 and 2,514 shaping runs per second on one and four cores, within 0.6 % of two earlier quick runs), compared with a reference only when it was measured with the same LordOS version, and `lord-bench` checks its own sandbox before any work: six things it must not be able to do (open a local socket or a network connection, run a program, start a process, create a file, read a system file) were all refused on that machine, and a run where one gets through is marked unstable. On a Raspberry Pi it watches the board's firmware during the whole run: the under-voltage alarm, the only flag the LordOS Pi kernel exposes, the firmware's full throttling flags where an older kernel has them, and 80 °C, where the firmware starts slowing the processor down, which stops a peak test as throttled; System Information's Raspberry Pi section reads the same alarm and flags and leaves out what the kernel doesn't show. This was tested on simulated system files and on the Pi 5, where the alarm is present and reads 0 with the official power supply; an actual under-voltage was not provoked. The references are a virtual machine and a Raspberry Pi 5 (§7.4); hardware video decoding and a real PC reference are not built yet and moved to 0.8.7.

### Source files

- [docs/ISO.md](https://github.com/lordbasex/LordOS/blob/main/docs/ISO.md), [docs/RASPBERRY.md](https://github.com/lordbasex/LordOS/blob/main/docs/RASPBERRY.md), [docs/FANS.md](https://github.com/lordbasex/LordOS/blob/main/docs/FANS.md), [docs/DISPLAYS.md](https://github.com/lordbasex/LordOS/blob/main/docs/DISPLAYS.md)
- [tools/rpi/build-kernel.sh](https://github.com/lordbasex/LordOS/blob/main/tools/rpi/build-kernel.sh), [tools/rpi/build-img.sh](https://github.com/lordbasex/LordOS/blob/main/tools/rpi/build-img.sh)
- [server/src/canvas.c](https://github.com/lordbasex/LordOS/blob/main/server/src/canvas.c)

## 8. Visual experience

- **A design guide.** Apps follow a shared guide (`docs/DESIGN.md`): an 8 px spacing grid, a small type scale, consistent control heights and one accent-coloured action per view. The tokens are exported by the SDK (`spacing`, `typeScale`, `controlSize`), and every app went through documented before-and-after polish passes in light, dark, right-to-left and small windows.
- **Themes.** A theme is a complete desktop written in React: menu bar, Dock, app launcher, Control Center and window frame, plus its tokens. LordOS ships **Lord Flow**, with a menu bar and a Dock, and since 0.8.3 **Lord Classic** as a second full appearance: a taskbar, a start menu with search, a tray, quick settings, a notification panel, window snapping with layouts, and its own restyling of every app. Both have window snapping and a task view. Themes switch live; settings persist. In 0.8.5 the two themes took these names (formerly "Default" and "Classic"; the user's choice is kept), Settings → Appearance → Desktop style became the one place that lists every installed theme, and if a theme is refused the last theme that loaded stays the fallback.
- **Menus and windows (0.8.5).** A fuller Window menu in both themes: Minimize, Zoom, Fill, Center; Move & Resize to halves, quarters and thirds, or arrangements of two or four windows; Move to another display; Bring All to Front; Return to Previous Size; and the open windows. In Lord Flow, clicking the app's name in the menu bar opens its app menu (About the app with its own version, its Settings, Hide, Hide Others, Show All, Quit), and the crown menu adds the Store with the number of waiting updates, Recent Items and Force Quit… (§5.4). Lord Classic, which has no menu bar, shows an app's own menus in a strip under its title bar. A Dock item can open its app at sign-in and, with several displays, be assigned to one (§3.5). Transparent apps such as the Terminal get an opaque border and title-bar line.
- **Light and dark, accents.** Both modes and the accent colour apply live to native apps, isolated apps and, through the portal and GSettings, GTK apps.
- **Right to left.** The whole interface mirrors for Arabic, Hebrew and similar scripts, including isolated apps, which follow the session's language live.
- **Lumo.** LordOS's mascot is Lumo, a small celestial fox, used in welcome screens, empty states and brand material, with a fixed set of poses defined in the brand guide.
- **Accessibility today:** a "reduce motion" setting that turns off decorative animations, keyboard shortcuts throughout, and scalable displays up to 200 %. A screen reader, zoom and high contrast are on the roadmap and **not built yet**.

### Source files

- [docs/DESIGN.md](https://github.com/lordbasex/LordOS/blob/main/docs/DESIGN.md), [docs/LORDOS-BRAND.md](https://github.com/lordbasex/LordOS/blob/main/docs/LORDOS-BRAND.md)
- [themes](https://github.com/lordbasex/LordOS/tree/main/themes), [sdk/controls.jsx](https://github.com/lordbasex/LordOS/blob/main/sdk/controls.jsx), [sdk/i18n.js](https://github.com/lordbasex/LordOS/blob/main/sdk/i18n.js)

## 9. Extensibility

- **The SDK** (`@lordos/sdk`) is the only thing apps import: windows, settings, theme tokens, controls, files, media, notifications, keyring, printing and more. Apps don't import from themes, so any app works in any theme.
- **Writing an app** means a folder with `manifest.json` and `index.jsx`. `node tools/lpk.mjs <folder>` bundles it without React or the SDK, adds the icon and translations, and signs it if a key is given; `keygen`, `sign`, `verify` and `catalog` cover the rest. Apps can include WebAssembly modules and draw raw pixels with `<Image pixels>`.
- **Themes** follow a fixed contract (`Desktop`, `MenuBar`, `Dock`, `Launcher`, `ControlCenter`, `WindowFrame`, tokens), documented in `themes/README.md`, with the Lord Classic theme as a template. Since 0.8.5 `docs/THEMES-GUIDE.md` explains, for a React developer new to LordOS, a theme's parts (theme.json, tokens, the shell, window frames, menus, layers), right-to-left, light and dark, several displays and the performance rules, with a checklist, and `examples/theme-minimal/` is a small working theme to start from. A theme LordOS didn't sign doesn't load on a release build yet: there is no theme package format or user trust decision.
- **Linux interoperability** is through standards: Wayland, the Settings portal, the Secret Service, the notifications service, `mimeapps.list` default apps and `.desktop` files.
- **Automation:** there is no scripting or automation API beyond the SDK today. Since 0.8.5 the Store installs AI apps (§5.3), but they are ordinary Linux apps with the same rules as any other; integrations of AI assistants with the desktop itself (for example through the Model Context Protocol) **do not exist in LordOS**; they are only a possible future direction and would have to respect the same rule that JavaScript can't execute commands.

### Source files

- [sdk](https://github.com/lordbasex/LordOS/tree/main/sdk), [apps/README.md](https://github.com/lordbasex/LordOS/blob/main/apps/README.md), [themes/README.md](https://github.com/lordbasex/LordOS/blob/main/themes/README.md)
- [docs/NATIVE-APPS.md](https://github.com/lordbasex/LordOS/blob/main/docs/NATIVE-APPS.md), [docs/WASM.md](https://github.com/lordbasex/LordOS/blob/main/docs/WASM.md), [examples/apps/fractal](https://github.com/lordbasex/LordOS/tree/main/examples/apps/fractal)

## 10. Roadmap and limitations

### 10.1 Where things stand

0.8.0 Beta was the first public release (18 October 2026); 0.7.0 was an internal build and was never published. Milestones M1 to M9 are complete: the display server, the UI engine, the SDK and themes, the terminal, languages and complex scripts, Linux app compatibility, packages and system services, sessions, isolation and the firewall, and the M9 app suites (documents and printing, media, system apps, passwords, hardware settings, mail and calendar). Since then: 0.8.1 (updates from the Store, the Dock, SSH), 0.8.2 (verified system code, isolated PIM apps, disk health, traffic history), 0.8.3 (X11 apps, Lord Classic, remote desktop hardening, open connections cut), 0.8.4 (remote desktop as WebRTC video with sound, LordOS Imager), 0.8.4.1 (Raspberry Pi bootloader from Settings, Pi image with HDMI fixed), and 0.8.5 Beta, released on 27 October 2026 (System Information with Checks and the Sandbox Probe, the Test Bench, versioned results, Monitor → Network's second generation, blocklists, the "deny silently" mode, components in their own scopes, Files → This Computer, a faster CPU path, paced frames on virtual cards, HTTPS in the package engine, a redesigned Store with Artificial Intelligence and Commercial categories whose vendor repositories install with the key LordOS pins and a signed featured banner, root over SSH as an advanced option, a fuller Window menu, the login screen closed after sign-in, the session started once its session bus exists). The Changelog below lists each release.

### 10.2 Known limitations

- Real hardware: AMD and Nvidia GPUs, real monitors on PCs, laptops (battery, backlight, lid), Bluetooth controllers, printers and scanners, FIDO2 keys and real mail providers are untested.
- The Raspberry Pi 5 is the only supported board, with LordOS's build of its kernel with Landlock; its 0.8.5 image was booted and used on a 1920×1080 monitor on both HDMI ports; other monitors and resolutions are untested.
- Linux apps are not sandboxed by default. X11 apps need Debian's `xwayland` package.
- Firewall: no ask mode; domain rules don't see apps that resolve names with their own encrypted DNS.
- Restrictions for Linux apps don't cover BlueZ over D-Bus, the system bus, the clipboard or apps started outside the app launcher; the `user@` union also holds the user's own services while a restricted app runs; a new PipeWire client has a window of a few milliseconds before the guard restricts it (§6.6).
- The `.lpk` signing key is still a development key, and there is no signed repository for `.lpk` apps yet.
- Permissions: no "allow once"; camera and microphone use by Linux apps isn't shown.
- Secure Boot must be turned off; signing with a shim is pending.
- The package engine doesn't do i386 multiarch yet, so Steam isn't offered.
- AI apps on a GPU: llama.cpp's Vulkan backend is untested on real AMD, NVIDIA and Intel hardware.
- Dragging a window in 0.8.5-beta: small black squares can show at its rounded corners while it moves, on the monitor and in the remote desktop's frames (fixed for 0.8.6, in development, §7.2).
- On the Raspberry Pi 5 an app keeping every core busy still halves the desktop's frame rate (about 31 instead of 60 frames a second in LordBench's test): the GPU's time per frame grows under the load, and the two changes measured for it in 0.8.6 (in development) gave too little and are off by default (§7.4).
- Legacy BIOS boot: a black rectangle (GRUB's terminal area while it loads the kernel and the initrd) shows for a few seconds between the boot menu and the splash.
- Wayland: no drag and drop, no LordOS input methods inside Linux apps (`text-input`), no `xdg-activation`, explicit sync, tearing control or tablets (§3.4).
- Without a GPU, AV1 video at 60 fps is heavy, since the browser decodes it in software on the same machine.
- Accessibility (screen reader, zoom, high contrast) is not built.

### 10.3 Next

Next, as planned in `docs/ROADMAP.md` and `docs/BENCHMARK.md`: **0.8.6, LordBench**, repeatable measurements of CPU, memory, GPU, the compositor, video and storage; **0.8.7, community hardware reports**, published from System Information with the user's consent; and a connection refused by the per-app firewall checked on the user's own computer. Xwayland and the Lord Classic theme, planned here for 1.0 in 0.8.0, shipped in 0.8.3. Flatpak and Flathub are not planned for any release: a design review is kept in `docs/FLATPAK.md` for the future, but to keep the system stable it is not being built.

After 1.0: a desktop virtualization app built on KVM and QEMU; an Android container with LineageOS and apps from LordOS's Store; the firewall's ask mode; accessibility; and a full demo video.

### Source files

- [docs/ROADMAP.md](https://github.com/lordbasex/LordOS/blob/main/docs/ROADMAP.md), [CHANGELOG.md](https://github.com/lordbasex/LordOS/blob/main/CHANGELOG.md)

## 11. How to verify the project yourself

### 11.1 Build it

On a development machine with Node.js, bundle the themes, SDK and apps:

```
npm install && npm run build
```

On Debian 13, build the binaries and run the tests:

```
sudo apt-get install -y --no-install-recommends build-essential
make -C server -j4
make -C server test        # every self-contained suite
make -C server test-full   # adds the helpers, the sandbox and the HarfBuzz comparison
```

`make test` fails if any suite fails; a suite whose tool or data isn't installed reports "skipped" rather than passing silently. The sandbox tests need a kernel with Landlock. The ISO and the Raspberry Pi image are built as described in `docs/ISO.md` and `docs/RASPBERRY.md`. On VM 116 with the 0.8.5 tree: 23 passed, 0 failed, 10 skipped (the Node.js suites, which run on the development machine).

### 11.2 What the tests cover

- **Terminal and text:** the VT emulator, OpenType shaping against HarfBuzz, Unicode bidi against the official test file, input methods, Thai word breaking against libthai.
- **Security:** the sandbox (app, network, helper), signing (NIST and RFC 8032 vectors), package state, archive path rules against malicious archives, `lord.fs` protections, the firewall rules (with 20,000 JSON mutations and 30,000 random operations against a linear reference), the DNS reader (200,000 random packets), the kernel-map compiler (480,000 connections), and the kernel integration test (18 of 18 on a VM).
- **Parsers:** mail, MIME, HTML sanitizer, iCalendar, vCard and XML (30,953 checks with mutation fuzzing), the vault format, importers, QR, CBOR, D-Bus marshalling, the country database reader, journal, TZif and IPP.
- **Engine and hardware:** layout and painting, NEON vs. C, the touchpad driver with recorded gestures, fans on fake sysfs trees, display arrangement, sound, and the WebAssembly specification suite. Since 0.8.6 (in development; the latest release is 0.8.5 Beta), every frame of random scenes, and of a Monitor scene with a translucent, blurred Terminal minimized into the Dock, is compared with a full repaint through a copy of the compositor's damage and double-buffer bookkeeping; that comparison found and fixed seven partial-repaint errors, and the same check on a running desktop on a virtual machine (the 0.8.2 scene, 22 minimize and restore cycles, 54 comparisons per run, before and after, with one damaged box and with several) found no stale area. A blurred window hidden whole behind an opaque one is no longer repainted with every change near it: a translucent Terminal behind Monitor's moving map made each frame repaint 46 % of the screen, now 11–15 % with one damaged box and 3–4 % with several on that virtual machine, checked the same way with no stale area.
- **System Information (0.8.5):** the collector over five fake machines (an AMD desktop, an Intel laptop, an Nvidia desktop, a Raspberry Pi 5, a virtual machine) filled with serial numbers, UUIDs, MAC addresses and names that must not reach a report (none does, 460 checks), the SMBIOS memory reader against crafted and malformed tables, and the Checks and Sandbox Probe evaluation (56 checks).

On the final ISO build: `make -C server test` reported 23 passed, 0 failed, 1 skipped (bidi data not installed).

### 11.3 Check a release

Download `SHA256SUMS`, `SHA256SUMS.sig`, the release files and the public key (`tools/release/lordos-release.pub.pem`). Check that the key's fingerprint is `c7155b8e6bbbb180f907381994d06af76d333016cd6f3facf67cadd053ea108e`, then, with OpenSSL 3:

```
base64 -d SHA256SUMS.sig > SHA256SUMS.sig.bin
openssl pkeyutl -verify -pubin -inkey lordos-release.pub.pem -rawin -in SHA256SUMS -sigfile SHA256SUMS.sig.bin
sha256sum -c --ignore-missing SHA256SUMS
```

The SBOM of each architecture (`lordos_<version>_<arch>.spdx.json`) is listed in `SHA256SUMS` too, so the same check covers it.

### 11.4 Check a running system

- `systemctl status lord-netperm lord-fand lordos` shows the services; the ISO test confirmed the seven network programs attached. If the kernel refuses the device or the LSM program, `lord-netperm` reports `"devices":false` or `"bluetooth":false`.
- `cat /proc/<pid>/status | grep -E 'Seccomp|NoNewPrivs'` on a `lord-server --app` process shows the sandbox.
- `systemd-cgls` shows the root-owned `lordos-session-<uid>` tree with one scope per app.
- Remote desktop: `ss -ltn` shows nothing on port 5673 until you enable it in Settings → Sharing.
- **System Information → Checks (0.8.5)** does these checks for you and says whether each was found configured or actually tried: the display server without root, Landlock, the per-app firewall with its kernel pieces, every bundle of LordOS's own code verified this session, the remote desktop's port closed when it is off (read from `/proc/net`), the session's cgroup tree owned by root, composition on the GPU, and **Run tests** for the Sandbox Probe (§6.4).
- **`lordos-testbench --auto`** prints a JSON report of the Wayland checklist, frame and presentation timing, the 3D renderer and input-to-screen latency; `lordos-sysreport report --json` prints the hardware report without serial numbers, addresses or names.

### 11.5 Versioned results

Each release publishes its measurements as files in `results/<version>/<machine>/` (and attached to the GitHub release): the hardware report, the Test Bench's report, whole-system memory with PSS per process, and the boot time, each folder with its conditions and the exact commands that produced it (`tools/results/collect.sh`, `docs/RESULTS.md`). From 0.8.6 LordBench's results join them. A number in this paper that has such a file can be checked against it, and repeated.

## 12. License and credits

LordOS's code is released under the **MIT license**. Third-party components keep their own licenses, listed in `THIRD_PARTY.md`: QuickJS and stb (vendored), Monocypher (CC0/BSD-2), the EFF word list (CC BY 3.0 US), the common-password list (MIT), DB-IP Lite country data (CC BY 4.0), Natural Earth (public domain), and the Inter, JetBrains Mono, Plus Jakarta Sans and Noto fonts (OFL; Noto as subsets built into `lord-bench`). Each release carries an SPDX SBOM of what the package bundles and depends on (`docs/COMPLIANCE.md`).

LordOS is created by the **iPERFEX** group: **Pablo Miranda** and **Federico Pereira**.

## 13. Glossary

- **Broker:** the single socket through which an isolated app asks `lord-server` for anything that touches the user's things; every request is checked in C.
- **cgroup (v2):** a kernel mechanism that groups processes to account for and limit resources. LordOS gives each app its own group, a "scope", and uses it to identify the app in the firewall.
- **Compositor:** the program that combines the windows of all apps into the final image on screen.
- **DRM/KMS:** the kernel interfaces for graphics devices and for setting display modes.
- **eBPF:** small verified programs that run inside the kernel; LordOS attaches them to cgroups to decide network connections, device opens and Bluetooth sockets.
- **evdev:** the kernel interface for input devices.
- **LSM (BPF LSM):** the kernel's security hooks; with the `bpf` LSM, a small program attached per cgroup can refuse an operation, such as creating a Bluetooth socket.
- **Namespace:** a kernel feature that gives a process its own view of something (users, mounts, process ids); LordOS uses unprivileged ones for Linux apps restricted from the user's files.
- **Helper:** a small LordOS process that handles untrusted data or secrets in a stricter sandbox (`lord-doc`, `lord-media`, `lord-maild`, `lord-vault`, `lord-fetch`, `lord-tls`, `lord-bench`).
- **Landlock:** a kernel feature that lets an unprivileged process restrict which files and network ports it, and its children, may use.
- **`.lpk`:** LordOS's package format for installable native apps.
- **`.lvault`:** LordOS's encrypted vault file format.
- **PipeWire guard:** `lord-server`'s own PipeWire client that hides capture devices from restricted Linux apps and stops their capture streams.
- **Powerbox:** a file dialog drawn by the system that hands the app only the file the user chose.
- **QuickJS:** a small JavaScript engine written in C.
- **SBOM:** a software bill of materials, the machine-readable list of the components a release contains, in the SPDX format.
- **Scope:** a cgroup for one app, such as `app-lordos-lpk-<id>.scope`.
- **seccomp-bpf:** a kernel filter that restricts which system calls a process may make.
- **Secret Service:** the freedesktop D-Bus standard Linux apps use to store secrets.
- **Wayland:** the protocol between graphical apps and the compositor.

## 14. Where to read more

| Chapter | Documents in `docs/` |
|---|---|
| 1. Vision | ARCHITECTURE.md, LORDOS-BRAND.md |
| 2. Architecture | ARCHITECTURE.md, NETWORK.md, FANS.md, SETTINGS.md, SYSTEM.md |
| 3. Compositor | GPU.md, DISPLAYS.md, WINDOWS.md, TRACKPAD.md, WAYLAND.md, SESSIONS.md, REMOTE.md |
| 4. UI engine | ARCHITECTURE.md, LANGUAGES.md, TERMINAL.md |
| 5. Applications | NATIVE-APPS.md, PACKAGES.md, UPDATES.md, WASM.md, FILES.md, MEDIA.md, DOCUMENTS.md, MAIL.md, ISO.md, INSTALL-DEBIAN.md |
| 6. Security | PRIVACY.md, FIREWALL.md, MONITOR.md, SIGNING.md, PASSWORDS.md, MEDIA.md, REMOTE.md, COMPLIANCE.md, and `SECURITY.md` |
| 7. Performance | ISO.md, RASPBERRY.md, FANS.md, DISPLAYS.md, GPU.md, REMOTE.md, BENCHMARK.md |
| 8. Visual experience | DESIGN.md, LORDOS-BRAND.md, LANGUAGES.md, DOCK.md |
| 9. Extensibility | NATIVE-APPS.md, WASM.md, THEMES-GUIDE.md, and `apps/README.md`, `themes/README.md` |
| 10. Roadmap | ROADMAP.md, RELEASING.md, UPDATES.md and `CHANGELOG.md` |
| 11. Verification | PAPER-VALIDATION.md, RESULTS.md, BENCHMARK.md, IMAGER.md |

## Changelog

Every release, with its full notes on GitHub (they come from `CHANGELOG.md`, in the Keep a Changelog format). This paper is updated with every change.

| Version | Date | What it brought |
|---|---|---|
| **0.8.5 Beta** | 2026-10-27 | System Information (hardware, the GPU by layers, Compatibility, Report, Checks and the Sandbox Probe), `lordos-sysreport`, `lordos-fetch`, the Test Bench, versioned results, Monitor → Network's second generation (a tree by app, destination and port; ICMP, raw and packet sockets; a color per app), LordOS's components in their own scopes, blocklists downloaded by `lord-fetch`, the "deny silently" mode, Files → This Computer and every disk, the redesigned Store with Artificial Intelligence and Commercial categories and a signed featured banner, HTTPS downloads through `lord-tls`, root over SSH as an advanced option, a CPU path on up to 8 threads, frames paced on virtual cards, remote desktop video at the viewer's refresh rate, a fuller Window menu and Force Quit, display numbers and snapping, the login screen closed after sign-in, faster locking, the session bus and keyring fix, idle-inhibit and the clipboard returned to its owner. [Release notes](https://github.com/lordbasex/LordOS/releases/tag/v0.8.5-beta) |
| **0.8.4.1 Beta** | 2026-10-25 | Raspberry Pi 5: Settings → Board firmware (the bootloader), its updater in LordOS's signed repository, a new image with HDMI fixed at first boot, network time, browsers and the ISO's system packages. [Release notes](https://github.com/lordbasex/LordOS/releases/tag/v0.8.4.1-beta) |
| **0.8.4 Beta** | 2026-10-19 | Remote desktop as WebRTC video with sound, over UDP or TCP on its own port, reconnecting by itself, with STUN and TURN; the LordOS Imager (in LordOS and for macOS, Linux and Windows) with the Pi's first-run file; browsers with a single top bar; blocked apps' connections closed on the Pi 5. [Release notes](https://github.com/lordbasex/LordOS/releases/tag/v0.8.4-beta) |
| **0.8.3 Beta** | 2026-10-19 | X11 apps through Xwayland; the primary selection; Lord Classic as a second full appearance; window snapping and the task view; remote desktop allow and block lists, a lockout after 3 failed sign-ins and a security review; firewall rules applied to open connections. [Release notes](https://github.com/lordbasex/LordOS/releases/tag/v0.8.3-beta) |
| **0.8.2 Beta** | 2026-10-19 | LordOS verifies its own code before running it; Mail, Calendar, Contacts and Reminders isolated; disk temperature and SMART health; suggestions for this computer; traffic history; per-destination switches; smooth browser video without hardware decoding. [Release notes](https://github.com/lordbasex/LordOS/releases/tag/v0.8.2-beta) |
| **0.8.1 Beta** | 2026-10-19 | Updates from the Store and a signed APT repository, migrations between versions, a Dock you arrange, remote login (SSH), Restart and Shut Down in the crown menu, stopping processes from the Monitor, Bluetooth on the Pi 5 image. [Release notes](https://github.com/lordbasex/LordOS/releases/tag/v0.8.1-beta) |
| **0.8.0 Beta** | 2026-10-18 | The first public release: milestones M1 to M9. [Release notes](https://github.com/lordbasex/LordOS/releases/tag/v0.8.0-beta) |
