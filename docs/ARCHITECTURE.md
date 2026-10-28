# LordOS architecture

LordOS is a **GNU/Linux distribution based on Debian 13** that ships a **desktop environment of
its own** (on the level of KDE or GNOME) with a **real display server and Wayland compositor
written in C**, built from its fundamental components on top of Debian and Wayland. It is not a simulator or a web page: it takes the
graphics card and input directly from the Linux kernel. The interface (desktop, bar, dock,
native apps) is written in **JavaScript with React**, which draws on an engine of our own, just
like React Native but on top of our display server.

## Layers

```
 LordOS themes and apps ── React + JSX + CSS-like styles
        │                 <View style={{flexDirection:'row', borderRadius:12}}>
        ▼
 lord-ui (C, ours) ───── QuickJS (JS engine, vendored)
        │                + a host config for the stock react-reconciler + our own flexbox layout
        │                + our own 2D rasterizer + text (stb_truetype)
        ▼
 lord-server (C, ours)   display server: DRM/KMS + evdev, windows, focus,
        │                composition, theme decorations
        │                + Wayland protocol → GTK (GNOME), Qt (KDE) apps,
        │                  Firefox, Electron; X11 apps through Xwayland (0.8.3)
        ▼
 Linux kernel (minimal Debian 13, generic kernel with DRM)
```

| Component    | Status      | Description                                                   |
|---------------|-------------|---------------------------------------------------------------|
| `server/src/` (kms, input, canvas) | **M1 done** | Display (KMS), input (evdev), 2D rasterizer |
| `server/src/ui/` | **M2 done** | Nodes, styles, flexbox, text, images, painting, damage, QuickJS |
| `runtime/`    | **M2 done** | React renderer (`@lordos/runtime`): a host config for the stock `react-reconciler` package, CSS → native, events |
| `themes/default/` | **M2 done** | Lord Flow (named Lord Default until 0.8.5): theme with a menu bar, a dock and an app launcher, with our own assets |
| `server/src/ui/js_sys.c` | **M3 done** | System APIs: fs (writes in the home folder and the user's mounts; since 0.8.5 reads the whole tree except `/proc`, `/sys`, `/dev`, `/run`), /proc, settings, themes, session |
| `sdk/` | **M3 done** | `@lordos/sdk`: windows, settings, tokens, controls, system hooks |
| `apps/` | **M3 done** | Apps as packages (`manifest.json` + `index.jsx`), independent of the theme |
| `themes/classic/` | **0.8.3 done** | Lord Classic (named Classic until 0.8.5): second full appearance (taskbar, start menu, snapping, task view, menus inside the window since 0.8.5) and a base for new themes (`THEMES-GUIDE.md`) |
| `server/src/term/` | **M4 done** | Our own terminal: xterm emulator, PTY, drawing, session holders (see `TERMINAL.md`) |
| `server/src/ui/otf.c`, `fonts.c` | **M4 done** | Our own OpenType (GSUB substitutions, CBDT emoji) and catalog of installed fonts: no HarfBuzz or fontconfig |
| `apps/terminal/` | **M4 done** | Native Terminal app (tabs, panes, profiles…) |
| `sdk/i18n.js`, `assets/locales/` | **M5 done** | Text, plurals and formats per language and region (see `LANGUAGES.md`) |
| `server/src/keyboard.c` | **M5 done** | Keyboard with libxkbcommon: every XKB layout, compose |
| `server/src/admin.c` | **M5 done** | Privileged helper with closed actions (region, time zone, language packs) |
| `server/src/ui/bidi.c`, `unicode.c`, `shape.c`, `textlayout.c` | **M5b done** | Full Unicode bidi, Arabic shaping and marks (GSUB/GPOS), per-script font fallback, CJK line breaking, mirrored interface (`direction`) |
| `server/src/ime.c`, `sdk/ime.js` | **M5b done** | Input methods: Korean, Japanese (SKK), Chinese (pinyin) |
| `server/src/app/`, `ui/js_broker.c`, `runtime/app-host.js` | **M8 done** | App mode: each `.lpk` as an isolated process (`lord-server --app <id>`), a Wayland client of the compositor, with a permission broker (see `PRIVACY.md`) |
| `server/src/sec/`, `ui/js_sec.c` | **M8 done** | Isolation of .lpk apps (Landlock+seccomp+cgroup), per-app permissions and Ed25519 signing (see `PRIVACY.md`, `SIGNING.md`) |
| `server/src/wl/`, `sdk/wayland.js` | **M6 done** | Our own Wayland compositor on libwayland-server: GTK and Qt apps as theme windows (see `WAYLAND.md`) |
| `server/src/tp/`, `wl/wl_input.c`, `js_input.c`, `sdk/trackpad.js` | **done (VM, uinput)** | Our own touchpad and touchscreen driver on evdev (no libinput): pointer acceleration, tap, finger clicks, palm rejection, kinetic scrolling, pinch, swipe; gestures and touches for Wayland apps (see `TRACKPAD.md`) |
| `server/src/doc/`, `js_doc.c`, `apps/pdf`, `apps/fonts`, `apps/characters` | **M9** | Documents: the sandboxed helper `lord-doc` (Poppler, libarchive, FreeType) and its apps: PDF Viewer, archives and Quick Preview in Files, Fonts, Emoji & Symbols (see `DOCUMENTS.md`) |
| `server/src/print/`, `js_print.c`, `sdk/printdialog.jsx`, `apps/scanner` | **M9** | Printing: our own IPP client against CUPS, our own mDNS, eSCL scanners, the system print dialog (see `PRINTING.md`) |
| `server/src/media/`, `js_media.c`, `sdk/media.js`, `apps/{photos,player,screenshots,camera,recorder}` | **M9** | Media: the sandboxed helper `lord-media` (FFmpeg, libva) and the Photos, Player, Screenshots, Camera and Recorder apps; `media` node, audio over shared memory with the PipeWire stream in lord-server, protected windows in screenshots, recording and remote desktop (see `MEDIA.md`) |
| `server/src/netperm/`, `server/src/fw/`, `js_fw.c` | **M8** | Per-app firewall: the root service `lord-netperm` enforces the rules with cgroup BPF in the sessions; Rule column in the Monitor and Trusted / Restricted apps in Settings (see `FIREWALL.md`) |
| `server/src/media/hwdec.c`, `sdk/video.js`, `assets/browser/video/` | **0.8.2** | Video in the browser without hardware decoding: per-boot VP9/AV1 decoding probe, the closed admin action `video-optimize` and LordOS's own Chromium extension that asks YouTube for H.264 and offers a lower quality when video stutters (see `VIDEO.md`) |
| `server/src/fan/`, `js_fan.c`, `sdk/cooling.jsx` | **M8** | Fans: the root service `lord-fand` drives them by temperature; Settings → Cooling and Monitor → Cooling (see `FANS.md`) |
| `server/src/monitor/`, `js_monitor.c`, `apps/monitor/` | **M8** | Monitor: CPU, memory, network, disks, processes and per-app connections with a map; its own thread only while the app is open, sock_diag and country by IP from a local database (see `MONITOR.md`) |
| `server/src/vault/`, `js_vault.c`, `sdk/vault.js`, `apps/passwords` | **M9** | Passwords and the system keyring (the Passwords UI runs as its own isolated process): the sandboxed helper `lord-vault` (Monocypher: XChaCha20-Poly1305, Argon2id), own `.lvault` format, FIDO2/CTAP2 client, session keyring opened at login, Secret Service on D-Bus with own code (see `PASSWORDS.md`) |
| `server/src/net/netd.c`, `remote/netsup.c` | **M9** | `lord-netd`: the network belongs to the system. A root service started and supervised by `lord-sessiond` at boot, serving the `net-*` actions for every session (see `NETWORK.md`) |
| `server/src/disk/`, `js_disk.c`, `sdk/disks.js`, `apps/disks` | **M9** | Disks: listing without root (sysfs, udev database, mountinfo), our own SMART reader (ATA PASS-THROUGH over SG_IO, NVMe health log), closed `disk-*` actions, bootable USB verified with SHA-256 (see `SYSTEM.md`) |
| `server/src/imager/`, `server/src/firstrun/`, `js_imager.c`, `apps/imager`, `desktop/imager` | **0.8.4** | LordOS Imager: verified image → removable drive (`imager-*` actions, `xz` and `curl` as the user), the Raspberry Pi first-run file and its applier `lordos-firstrun`, and the same UI as a Tauri app for other computers (see `IMAGER.md`) |
| `server/src/fw/fwlists.c`, `fwsub.c`, `server/src/fetch/`, `server/src/sec/netconn.c`, `apps/monitor/NetTree.jsx`, `Blocklists.jsx`, `sdk/netmodel.js`, `sdk/netstatus.jsx` | **0.8.5** | Monitor → Network, second generation: blocklists (the sandboxed downloader `lord-fetch`, compiled into lord-netperm's maps), the deny mode, LordOS's components in their own scopes (the `--net-connector` processes), the system services' traffic, the tree, chips and view menu, the menu bar network item (see `MONITOR.md`, `FIREWALL.md`) |
| `server/src/sysreport/`, `server/src/app/probe.c`, `apps/sysinfo`, `apps/sysprobe` | **0.8.5** | System Information: the hardware collector behind `lordos-sysreport report` and `lordos-fetch`, the app (Overview, Hardware, Compatibility, Checks, Report) and the Sandbox Probe system package (see `BENCHMARK.md`) |
| `server/src/testbench/` | **0.8.5** | Test Bench (`lordos-testbench`): a Wayland client that checks the display server from the outside (frame timing, presentation, keyboard, pointer, protocols, an optional GLES 3D view) (see `BENCHMARK.md`, `RESULTS.md`) |
| `server/src/tls/`, `server/src/pkg/sources_admin.c`, `apt/catalog.json` | **0.8.5** | The package engine's HTTPS (the sandboxed relay `lord-tls`), the signed featured banner, and vendor APT repositories from LordOS's catalog added and removed by the closed `pkg-source-add`/`pkg-source-remove` actions (Store → Artificial Intelligence and Commercial; see `PACKAGES.md`, `UPDATES.md`) |
| `server/src/mail/`, `js_mail.c`, `sdk/{mail,calendar,contacts}.js`, `apps/{mail,calendar,contacts,reminders}` | **M9** | Mail, Calendar, Contacts and Reminders (each app an isolated process since 0.8.2): the sandboxed helper `lord-maild` (IMAP with IDLE/CONDSTORE, SMTP, CalDAV/CardDAV, OpenSSL) with no sockets of its own — `lord-server` connects only to configured servers and hands it the fd; own MIME, HTML sanitizer, iCalendar, vCard and XML parsers (see `MAIL.md`) |
| `server/src/backup/`, `js_backup.c`, `sdk/backup.js`, `apps/backup` | **M9** | Backups: hard-link snapshots in a lord-server thread (nice 19, idle I/O), hourly/daily/weekly retention, optional encryption with the vendored Monocypher; no root and no `admin.c` actions (see `SYSTEM.md`) |
| `server/src/logs/`, `js_logs.c`, `sdk/logs.js`, `apps/logs` | **M9** | Logs: our own systemd journal reader (no libsystemd), LordOS logs, system logs for admins through a closed action, live follow with inotify (see `SYSTEM.md`) |
| `server/src/ui/tzif.c`, `js_help.c`, `sdk/{notify,alarms,statusitems,help}.js`, `apps/{clock,help}` | **M9** | Clock (our own TZif reader, session alarms with the app closed), session notifications, menu-bar status items, Help (markdown articles in es/en/pt, welcome tour, Search source) (see `SYSTEM.md`) |
| `server/src/ui/grab.c`, `cursor.c`, `sdk/windowshell.jsx` | **M8** | Windows: moving and resizing in C (one event per frame, no JS), a layer for the window being moved, edges on all 4 sides and corners, our own cursors and `cursor-shape-v1` (see `WINDOWS.md`) |
| `server/src/outputs.c`, `kms.c`, `hw/display.c`, `ui/paint.c` (`paint_view`) | **M9** | Several displays (extend and mirror, arrangement by EDID identity, hotplug) and sharp HiDPI at 100–200 % per display: one logical space, each display painted at its own resolution; one `wl_output` per display, xdg-output, fractional-scale-v1 (see `DISPLAYS.md`) |
| `server/src/hw/`, `ui/js_hw.c`, `sdk/hardware.jsx`, `sdk/defaults.js` | **M9** | Settings' hardware pages: displays (EDID, live modes with a 15 s revert, per-display persistence, software rotation and 200 % scale, Night Light on the gamma LUT or by software), Bluetooth (BlueZ over our own D-Bus client, pairing agent), battery and power profiles, idle (DPMS, lock, sleep), date and time, notifications from Linux apps (`org.freedesktop.Notifications`) and default apps (`mimeapps.list`); kernel uevents and D-Bus signals, no polling (see `SETTINGS.md`) |

### Flow of a frame

1. An event arrives (evdev) → `hit_test` finds the node under the cursor → `__lordEvent` in JS.
2. React updates state → the reconciler calls `lord.setStyle/appendChild/...` → nodes are marked.
   Only the style properties that changed are sent (`applyStyle` in `runtime/style.js`). Since 0.8.6
   (in development) `applyProps` in `runtime/index.js` also skips converting a node's style when its
   flattened style has the same keys, in the same order, with the same primitive values as last time
   (kept as a copy, so a style object changed in place is still seen; not when a converted value is
   NaN, which `applyStyle` re-sends every time). The calls are exactly the ones it sent before:
   `server/tests/runtime_props_test.mjs` checks it against the runtime with the skip turned off.
3. `lord-server` recomputes the layout, computes the damaged region and repaints only that region.
4. It copies the scene to the back buffer, draws the cursor, and page-flips on the next vblank.

## Dependency policy

Besides the kernel and libc, only these are allowed:

| Dependency          | Why                                                       | How       |
|----------------------|---------------------------------------------------------------|------------|
| kernel headers   | DRM / evdev ioctls                                            | system    |
| `libwayland-server`  | serializes the Wayland protocol that GTK/Qt speak (M6, in use) | system    |
| `libwayland-client`  | "app mode" (`lord-server --app <id>`): each isolated `.lpk` app is a Wayland client of the compositor (M8); same family and source package as `libwayland-server` | system    |
| `libxkbcommon`       | XKB keyboard layouts (M5) and what Wayland apps require (M6) | system    |
| Mesa: `libgbm`, `libEGL`, `libGLESv2` | GPU composition and dmabuf buffers from apps (games); optional: without a 3D GPU or without the `-dev` packages it builds and runs without Mesa (approved by the user on 2026-10-16, see `GPU.md`) | system |
| QuickJS              | ES2023 JavaScript engine, ~1 MB, written in C                  | vendored|
| `stb_truetype`, `stb_image` | fonts and images; public domain, a single header    | vendored|
| `stb_image_write`    | PNG for the remote desktop's lossless refresh of still areas and cursor images (`lord-sessiond`), and images written by the `lord-doc` and `lord-maild` helpers; public domain | vendored|
| OpenSSL (`libssl`, `libcrypto`) | TLS. **Only four binaries link it**: `lord-sessiond` (remote desktop on the LAN, `REMOTE.md`), `lord-maild` (IMAP/SMTP/CalDAV/CardDAV, certificates checked against the system CA store, `MAIL.md`) and, since 0.8.5, `lord-fetch` (the blocklist downloads, sandboxed, no sockets of its own, `FIREWALL.md` "Blocklists") and `lord-tls` (the package engine's HTTPS, sandboxed, `PACKAGES.md` "HTTPS"). Never `lord-server` | system |
| `libcrypt`           | verifying passwords against `/etc/shadow` (`lord-sessiond`, `admin.c`) | system |
| `libsrtp2`, `usrsctp` | the remote desktop's video (WebRTC, 0.8.4): SRTP and the data channels' SCTP, in `lord-sessiond`'s bridge only (as the user, after it dropped root); with OpenSSL's DTLS and our own ICE-lite, SDP and RTP code (`remote/rtc.c`, `remote/rtcwire.c`) because Debian 13 packages no libdatachannel. Required for `lord-sessiond` since the remote desktop shows the screen only over WebRTC (the image path was removed in 0.8.4). Never `lord-server` | system |
| FFmpeg (`libavcodec`, `libavformat`, `libavutil`, `libswscale`, `libswresample`) and `libva` (+ `libva-drm`) | Decoding and encoding video, audio and images; VA-API for hardware decoding (M9, approved by the user). **Only `lord-media` links them**, the sandboxed helper; the player, synchronization and interface are our own. Never `lord-server` (`MEDIA.md`) | system |
| Poppler (`libpoppler-glib`) and libarchive | PDF and archives (M9, approved by the user). **Only `lord-doc` links them**, the sandboxed helper; Poppler brings cairo, FreeType and fontconfig, which `lord-doc` also uses for font samples and for building PDFs. Never `lord-server` (`DOCUMENTS.md`) | system |
| `clang` (build time only) | the BPF programs of `lord-netperm` (`np.bpf.c`, `clang -target bpf`); they are embedded in the binary. Without clang (`BPF=0`) the service runs without BPF. Neither libbpf nor bcc is used | build machine |
| Monocypher 4.0.2 | XChaCha20-Poly1305, Argon2id/Argon2d, BLAKE2b, ChaCha20 for the Passwords vault and the KeePass importer (approved by the user; CC0/BSD-2, one C file). **Only `lord-vault` links it**; its official test vectors run in `make test` (`PASSWORDS.md`) | vendored |
| EFF word list, 10k common passwords | passphrase generator and the offline "breached" check (CC BY 3.0 US / MIT), embedded in `lord-vault` | `third_party/wordlists` |
| Inter, Plus Jakarta Sans, JetBrains Mono | interface, display and terminal fonts (OFL) | `assets/fonts` |
| Noto Sans Arabic, Devanagari, Thai and CJK SC, subset | LordBench's shaping workload (0.8.6, in development): Debian 13's fonts subset to its fixed paragraphs by `tools/gen-lbfonts.py` (fontTools, on the maintainer's machine only, not a build dependency) and built into `lord-bench` with Inter Regular, which opens no files (OFL; `BENCHMARK.md`) | `server/src/bench/fonts` |

**Xwayland** (Debian's `xwayland` package, 0.8.3) is not linked by anything: it is a separate
program that `lord-server` starts on demand for X11 apps (`WAYLAND.md`, "X11 apps (Xwayland)").
The X window manager inside `lord-server` speaks the X11 protocol itself (`server/src/wl/xwm.c`,
`x11wire.c`): no libxcb or libX11. Without the package, X11 apps simply don't open.

React (`react`, `react-reconciler`) is a dependency **of the theme**, not of the system: it is bundled
with esbuild inside the theme's `.js`. Since 0.7.0 it is React 19.3.1 with `react-reconciler` 0.34
(the default theme weighs 367 KB with all the apps; React 18 was ~40 KB less). Node.js is only
used on the development machine.

Besides `lord-server`, these programs and helpers run (root services, then sandboxed helpers):

- **`lord-netd`** (M9, root): the system network, started and supervised by `lord-sessiond` at boot;
  only the `net-*` actions of `admin.c`. libc only. See `NETWORK.md`.

- **`lord-sessiond`** (root, `lordos.service`): the login screen, the session list, the
  monitor and the remote desktop. See `SESSIONS.md` and `REMOTE.md`. The remote desktop shows the
  screen only as WebRTC video, from a bridge process that runs as the signed-in user: ICE over UDP
  and over TCP on the remote desktop port (the root connection child only routes an ICE-TCP socket
  to its bridge over `/run/lordos/rtc`), TURN over UDP, TCP or TLS, ICE restarts and reconnection.
- **`lord-server`**: one session per user, always without a monitor of its own (`--headless`); the
  monitor and the browsers look at it through its socket.
- **`lord-netperm`** (root, `lord-netperm.service`): per-app network permissions. It attaches its
  cgroup BPF programs to `lordos.service` and reads each user's rules. No network of its own and no
  execution. See `FIREWALL.md`.
- **`lord-fand`** (root, `lord-fand.service`): fans by temperature from the first boot (the
  Raspberry Pi 5 fan header and motherboard PWM chips). No capabilities, no network, executes
  nothing; sessions pick a profile over `/run/lordos/fand.sock`. See `FANS.md`.

- **`lord-doc`** (M9): the documents helper. `lord-server` launches it when needed, with a
  fixed path (next to its binary) and no arguments. It runs as the user, without network and sandboxed
  (Landlock + seccomp), and works only with the fds `lord-server` passes it. It opens PDFs,
  archives, fonts and images that come from outside. See `DOCUMENTS.md`.
- **`lord-media`** (M9): the media helper, just like `lord-doc`: fixed path, no arguments,
  as the user, no network or files (only the fds `lord-server` passes it, including the
  camera and the render node), in `sandbox_apply_helper`. It decodes and encodes with FFmpeg; the
  frames come back through sealed shared memory and the audio goes to a ring whose PipeWire
  stream belongs to `lord-server`. See `MEDIA.md`.

- **`lord-vault`** (M9): the Passwords and keyring helper. Same rules as `lord-doc`/`lord-media`
  (fixed path, no arguments, minimal environment, `sandbox_apply_helper`: no network, no new
  sockets, no exec), but it lives for the whole session: it holds the decrypted vaults in an
  `mlock`/`MADV_DONTDUMP` arena, receives the vault folder, picked files, the FIDO2 hidraw device
  and the already-connected session D-Bus socket as fds, and serves the Secret Service. See
  `PASSWORDS.md`.

- **`lord-maild`** (M9): the Mail, Calendar, Contacts and Reminders helper. Same rules as
  `lord-doc` (fixed path, no arguments, minimal environment, `sandbox_apply_helper`), lives while
  the session does (it sends mail later, keeps IMAP IDLE and computes alarms with the apps closed).
  It speaks IMAP, SMTP, HTTP/WebDAV and TLS (OpenSSL) but **cannot create sockets**: it asks
  `lord-server` (`mail/mailc.c`) for a connection, and `lord-server` connects only to the servers
  of the accounts file (or, after *Load Images*, to public addresses for remote images) and passes
  the connected fd. Its only writable folder is its cache. See `MAIL.md`.

- **`lord-fetch`** (0.8.5): the blocklist downloader. Same rules as `lord-maild` (fixed path, no
  arguments, minimal environment, `sandbox_apply_helper` after loading the CA bundle, no sockets of
  its own); it lives for one download job, gets each list's already-connected socket, speaks TLS
  and one HTTP GET with a size limit, parses the list strictly and returns normalized lines in a
  sealed memfd. See `FIREWALL.md`, "Blocklists".

- **`lord-tls`** (0.8.5): the package engine's HTTPS. The Store's download child (as `nobody`, in
  `lordos-net-store.scope`) connects the TCP socket itself and execs only `<lord-server dir>/lord-tls`
  (root's, not writable by others) with no arguments, an empty environment, the socket on fd 3 and a
  socketpair on fd 4. It reads one host line, loads the CA bundle, applies `sandbox_apply_helper`,
  verifies the certificate chain and the host name (TLS 1.2+, SNI) and then only relays bytes; it never
  parses HTTP or decides about packages (the signed index still does), and an https:// download is
  never redirected to http://. See `PACKAGES.md`, "HTTPS".

- **The components' connectors** (0.8.5): `lord-server --net-connector mail|print|lists`, the same
  binary re-executed with a fixed argv (like the terminal holders), each in its own
  `lordos-net-<name>.scope`, only creating and connecting sockets for lord-server so the kernel
  counts and decides them as that component (`sec/netconn.h`, `FIREWALL.md`, "LordOS components in
  their own scopes").

`lord-server` can still run on its own, with its own DRM and evdev, for development.

Data shipped with LordOS for the Monitor (`MONITOR.md`): `assets/monitor/geoip-country.mmdb.xz`
(DB-IP Lite Country, CC BY 4.0, read by our own MaxMind DB reader) and the Natural
Earth map (public domain). libmaxminddb is not used.

System data that is read (not linked): `xkb-data` (layouts), `libx11-data`
(only the compose tables in `/usr/share/X11/locale`), `locales` and `tzdata`.

System fonts (for example `fonts-noto-color-emoji` for emoji, or Fira Code) are
optional: if present, the Terminal offers them; HarfBuzz, FreeType and fontconfig are not linked.

Since M7, what LordOS does with its own code and what it delegates to a Debian program:

- **Our own code, no libraries:**
  - OpenPGP (RSA, Ed25519), SHA-256/512, xz, HTTP and the dependency resolver (`server/src/pkg`;
    since 0.8.5 HTTPS goes through the `lord-tls` relay above, which alone links OpenSSL);
  - the sandbox, permissions and signing of `.lpk` apps (`server/src/sec`): Landlock,
    seccomp, cgroup v2, Ed25519 (signing and verifying) and a JSON reader, with no external libraries;
  - the WebAssembly interpreter (`server/src/wasm`);
  - the PulseAudio protocol client (`server/src/sound`);
  - netlink, DHCP and WireGuard (`server/src/net`);
  - the CUPS IPP client, mDNS/DNS-SD and eSCL (`server/src/print`, M9): no libcups, avahi-client
    or SANE.
- **Debian services spoken to over their protocol:** CUPS (IPP over
  `/run/cups/cups.sock`; it manages the queue and the drivers, see `PRINTING.md`).
- **D-Bus services (Settings, `SETTINGS.md`):** BlueZ (`bluetoothd`), power-profiles-daemon and
  systemd-timedated on the system bus, with our own D-Bus client (`vault/dbus.c`); lord-server also
  serves `org.freedesktop.Notifications` on the session bus. No libdbus, sd-bus or libbluetooth.
- **Debian programs** (with a fixed path, never linked, launched by the `admin.c` helper):
  - `dpkg`: unpacking and configuring;
  - `mkfs.vfat`, `mkfs.exfat`, `mkfs.ext4`, `mkfs.ntfs` (optional) and their label tools: formatting
    and renaming in Disks (M9, `disk-*` actions);
  - `wpa_supplicant`: Wi-Fi;
  - `openvpn`;
  - `resolvectl` and `networkctl`.
  - `powerprofilesctl`, `systemctl suspend`/`reload systemd-logind` and `timedatectl` (Settings'
    closed `power-*` and `time-*` actions, `SETTINGS.md`);
  - `smartctl` (0.8.2, `smart-*` actions) and, on the Raspberry Pi 5, `rpi-eeprom-update` (0.8.4.1,
    Settings → Board firmware).
- **Sound server:** PipeWire, over the PulseAudio protocol.

apt, libapt, gpgv, zlib, liblzma, NetworkManager, wireguard-tools, libpulse,
libpipewire, libcups, avahi-client and SANE are not used. `curl` is used only by LordOS Imager's
`imager-*` actions (0.8.4), run as the user with a fixed path, never linked (`IMAGER.md`).

Forbidden: X11/Xorg (Xwayland is a separate program started on demand, see above), wlroots, libdrm, libinput, Mesa as a requirement (only as the optional GPU path), WebKit/Chromium,
GTK/Qt inside LordOS; bubblewrap, flatpak, firejail or libseccomp for isolation (it is
our own code using Landlock and seccomp-bpf directly). Vendored = copied as source code into `third_party/`.

## Platforms

| Architecture | Machines | Status | Image |
|---|---|---|---|
| x86-64 (amd64) | PC with UEFI or BIOS; AMD, Intel or Nvidia GPU, or no 3D GPU (CPU) | primary, tested on VM | live ISO with installer (`ISO.md`) |
| ARM64 (aarch64) | **Raspberry Pi 5** (the only supported board) | tested on the board (`RASPBERRY.md`, release tests in `ISO.md`) | SD/NVMe image, `tools/rpi/build-img.sh` (`RASPBERRY.md`) |

The code is the same. The differences:

- **Build:** the Makefile detects `ARCH` with `$(CC) -dumpmachine` and never uses
  `-march=native`.
  - On aarch64 the NEON loops in `canvas.c` (`NEON=0` turns them off) and `STBI_NEON` are enabled.
  - On x86-64 everything is portable C (stb uses SSE2 by itself).
- **Display and GPU:**
  - The card is chosen because it has connectors (`server/src/drmcard.h`), not because it is `card0`.
  - On ARM boards the display (vc4) and the GPU (v3d) are different devices. Mesa joins them
    (kmsro), lord-server advertises the GPU's render node to apps and only sends the display
    the modifiers the plane accepts (`IN_FORMATS`).
- **16K memory pages** (the Pi 5 kernel): nothing in LordOS assumes 4096.
- **Seccomp:** the allow list uses `#ifdef` for the syscalls arm64 doesn't have (`open`, `stat`,
  `epoll_wait`, `pipe`…).
- **Kernel:** on x86-64, Debian's; on the Raspberry Pi, the one from the Raspberry Pi archive (Debian
  13's doesn't handle the Pi 5). Mesa is Debian's on both.

## Why not a browser

WebKit or Chromium cost between 100 and 300 MB of RAM per process and are not our own.
LordOS's goal is to run on very low-end machines. With React on our own
engine, the theme writes JSX and styles, but it is rendered with a subset of CSS
(flexbox, colors, rounded borders, shadows, blur, transitions) that we do control.

## Theme model

A theme is a JS package that exports React components with a fixed contract:
`Desktop`, `MenuBar`, `Dock`, `Launcher`, `ControlCenter`, `WindowFrame` and its tokens
(colors, radii, shadows, fonts, wallpaper). The theme talks to the server only through
`@lordos/sdk` (`useWindows()`, `launch()`, `focus()`, `close()`…), never directly.
The default theme, Lord Flow, follows a familiar menu-bar-and-dock desktop layout, drawn with
our own icons and wallpapers; no third-party desktop assets are used or redistributed.

## JavaScript layers

```
themes/<theme>/  full desktop (bars, dock, window frames)               ─┐
apps/<id>/       content of each window, independent of the theme       ─┤ a single bundle
sdk/             @lordos/sdk: windows, settings, tokens, controls       ─┤ per theme
runtime/         @lordos/runtime: React renderer, CSS → native, events  ─┘ (esbuild)
```

The theme and the apps communicate through the token contract (`useTheme`) and the SDK's window
manager (`wm`). Switching themes discards the whole QuickJS context and loads the
other bundle; settings persist on disk.

## Security

- In a session, `lord-server` runs as the user, started by `lord-sessiond`, which lends it the DRM and
  input devices (`SESSIONS.md`). Run on its own for development, it starts with sudo only to open
  DRM, evdev and the console; right away it switches to the session user (`SUDO_USER` or `--user`), keeping only `CAP_SYS_TTY_CONFIG`.
- `lord.fs` writes only in the home folder and the user's mounts (after `realpath`), with a deny list
  of folders the system reads to launch or install something or that store a security decision;
  since 0.8.5 its reading calls reach the whole tree as the user, except `/proc`, `/sys`, `/dev` and
  `/run` (`FILES.md`).
- `lord.settings` only writes `~/.config/lordos/settings.json` (max. 64 KB, atomic write).
- `lord.themes.use` only accepts plain folder names in `themes/`; a theme that fails
  to load is banned to avoid loops.
- LordOS's own JavaScript (themes, login screen, app host, system apps) only runs if it matches
  `code-manifest.json`, signed with the release key and checked by lord-server at load time from the
  same read that is evaluated (`server/src/sec/codesig.c`, `SIGNING.md`, "Verified system code").
  A modified bundle is not run: another theme, or the built-in recovery screen.
- There is no API to run arbitrary commands. JS can start a process only through
  `lord.term.create()`, which launches the user's own login shell (the one in `/etc/passwd`)
  with no capabilities and no `SUDO_*` variables, just like any terminal emulator;
  `lord.apps.launch(id)` and its variants, which run only the `Exec` line of an installed `.desktop`
  without a shell (`WAYLAND.md`); and `lord.apps.launchPackage(id)`, which starts an installed `.lpk`
  as an isolated process (`PRIVACY.md`).
- The clipboard belongs to the session (in memory). Terminal programs can write it
  (OSC 52) but not read it.
- Manifest `permissions` are enforced since M8: each `.lpk` runs isolated and the broker checks every
  call against the user's decisions (`PRIVACY.md`).
- External Wayland apps (M6) only see their own surfaces.
- Documents that come from outside (PDFs, archives, fonts, scanner images) are opened
  only in `lord-doc`, sandboxed. A malicious file can bring that process down, but not
  `lord-server`. Extracting never writes outside the destination folder (`DOCUMENTS.md`, «Extracting
  without leaving the folder»).
- Media (videos, music, photos, the camera) are decoded only in `lord-media`, sandboxed and
  without the PipeWire socket. Screen, microphone and camera ask for permission (`sec/perms.c`, checked
  in C), and protected windows (`wm.setProtected`) come out black in screenshots, recordings and the
  remote desktop (`MEDIA.md`).
- Printers: managing queues (add, remove, default) is authorized by CUPS. Only
  users in the `lpadmin` group (the administrators) can do it, identified with PeerCred over
  the unix socket. There is no new root action in `admin.c` (`PRINTING.md`).
