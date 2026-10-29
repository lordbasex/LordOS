# LordOS roadmap

Every milestone is verified on the `lordos-dev` VM (see `VM-SETUP.md`) with real
screenshots (QEMU `screendump`) and documented.

## M1 — Minimal display server ✅ (2026-10-15)

- [x] Our own modesetting with DRM ioctls (no libdrm), double buffering + page flip.
- [x] Our own input from evdev (no libinput): keyboard, relative mouse, absolute tablet, `EVIOCGRAB`.
- [x] Our own 2D rasterizer: alpha, antialiased rounded rectangles, shadows.
- [x] Test scene: wallpaper, menu bar, dock with magnification, draggable window, cursor.
- [x] Measured: 1280×800, full CPU render ≈ 13 ms/frame on the VM.

## M2 — UI engine with React ✅ (2026-10-15)

- [x] QuickJS 2025-04-26 vendored in `third_party/quickjs`, embedded in `lord-server`.
- [x] Native node tree (`View`, `Text`, `Image`) + styles (a CSS subset, see `themes/README.md`).
- [x] Our own flexbox layout (`server/src/ui/layout.c`).
- [x] Text with `stb_truetype` and the Inter font (OFL); PNG/JPEG images with `stb_image`.
- [x] React 18 renderer (`runtime/`, `react-reconciler` 0.29) that creates and updates native nodes.
- [x] Mouse events with bubbling (enter/leave/click), keyboard, timers, `requestAnimationFrame`.
- [x] "Lord Flow" theme: menu bar with blur, dock with magnification and tooltip, draggable
      windows with traffic lights, Files/Settings/About apps, light/dark mode and live accents.
- [x] Cached shadows, reduced-resolution blur, damage tracking.

Measured on the VM (1280×800, CPU, no GPU):

| Situation | Render |
|---|---|
| First frame (full screen) | ~65 ms |
| Menu hover | 0.3–0.6 ms |
| Dock hover with magnification | 2–4 ms |
| Window drag | ~5 ms |
| Total server RAM (with React theme) | ~24 MB |
| Theme load (126 KB, React included) | ~28 ms |

Left for later: per-corner radii, `transform: scale`, text
input (keyboard → characters per layout), copying only the damaged region to the back buffer.

## M3 — SDK, apps as packages and swappable themes ✅ (2026-10-15)

- [x] Security: `lord-server` drops root after opening the display/input; it runs as the user with
      only `CAP_SYS_TTY_CONFIG` (verified in `/proc/<pid>/status`).
- [x] Native APIs: `lord.fs` (read-only, confined to `$HOME`), `lord.system` (/proc),
      `lord.settings` (`~/.config/lordos/settings.json`), `lord.themes`, `lord.quit`.
- [x] Input: right click, wheel, double click; keyboard with `latam` and `us` layouts,
      accents with dead keys, keypad.
- [x] `@lordos/sdk` (`sdk/`): window manager, persistent settings, token contract
      (`ThemeProvider`/`useTheme`), `TextInput`, `ScrollView`, controls, system hooks.
- [x] Apps as packages (`apps/<id>/manifest.json`): Files (real FS), Settings, About
      (real data), Calculator, Terminal (M4 notice).
- [x] "Lord Flow" theme: app launcher with search, Control Center (live CPU/RAM),
      drop-down menus, context menu, 4 wallpapers of our own.
- [x] "Lord Classic" theme as a template (taskbar + start menu, a single file).
- [x] Live theme switching from Settings, persistent, with protection against broken themes.

Measured on the VM: server RAM 24–26 MB with Lord Flow, 11 MB with Lord Classic; theme
switch ~30 ms + first frame ~60 ms.

Left over from M3 and moved to later: volume and brightness (no audio or backlight on the VM),
real per-app isolation by permissions, a development environment in the development machine's browser.

## M4 — Native terminal ✅ (2026-10-15)

Moved ahead of Wayland at the project's request: the LordOS terminal is our own, not a Linux
app. Full details, shortcuts and the parity table with popular terminal emulators in `TERMINAL.md`.

- [x] C engine (`server/src/term/`): xterm-256color emulator (VT500 parser, direct color,
      alternate screen, mouse, bracketed paste, synchronized output), scrollback with
      reflow, unprivileged PTY, drawing with per-row damage, hand-drawn boxes/blocks/powerline.
- [x] OSC 1337 extensions: inline images (`imgcat`), badges, marks, bash
      integration (OSC 133/7), Sixel, OSC 8 hyperlinks, OSC 52, OSC 9 notifications.
- [x] React app: tabs, split panes, profiles with 16 color schemes, transparency and
      blur, regex search, toolbelt, status bar, hotkey window (F12),
      copy mode, Instant Replay, triggers, automatic profile switching, saved
      arrangements, undo close, composer, autocomplete, command and paste history.
- [x] Platform: several windows per app, dynamic title, app menus in the theme's
      bar, confirmation on close, arguments when opening apps, window resizing,
      `<Terminal>` component, session clipboard, `Stepper` and `Slider`.

- [x] Parity with popular terminal emulators completed: font choice and "non-ASCII" font,
      ligatures (our own GSUB engine, `server/src/ui/otf.c`), color emoji (CBDT), combining
      characters and graphemes (mode 2027), zsh and fish integration, configurable shortcuts and
      keys, profile switching by user@host, highlight/mark/title triggers,
      sessions that survive a server restart (holders, `term/hold.c`),
      `tmux -CC` with native tabs and panes, and **profile import from other terminal emulators**
      (JSON, `.itermcolors`, XML and binary plist).

Measured on the VM: `seq 1 1000000` in 2.1 s with transparency; server RAM ~39 MB with
3 terminals, ~43 MB with 6 sessions + 3 tmux panes + emoji; ~2.2 MB per holder.
What is left out (background image, COLR/SVG emoji, GPOS) is in `TERMINAL.md`.

## M5 — Language and region (system-wide) ✅ (2026-10-15)

Details in `LANGUAGES.md`. It comes before Wayland so GTK/Qt apps receive the same language.

- [x] Per-user system region (`~/.config/locale.conf`) inherited by the session and the
      shells; Debian messages (gettext) in the chosen language (verified with `ls`, `date`, `printf`).
- [x] Interface: catalogs per namespace (`assets/locales/<language>/*.json`), `t()`/`useLocale()`,
      plurals (our own CLDR rules), dates and numbers per region; live switching without losing
      windows or terminals; English fallback. 391 keys.
- [x] Spanish, English and Portuguese complete; `tools/i18n-check.mjs` and `tools/i18n-pack.mjs`;
      test French pack installed from Settings.
- [x] Language packs: LordOS catalogs (user, no root) + glibc region + the language's Debian
      packages (manpages, hunspell, aspell, hyphen, mythes, fonts for the script).
- [x] Keyboard with libxkbcommon + xkb-data: every layout and variant, up to 4 at
      once, dead keys and compose.
- [x] System time zone and region from Settings.
- [x] Minimal privileged helper (`server/src/admin.c`) with closed actions, approved.

## M5b — Complex scripts ✅ (2026-10-15)

Details in `LANGUAGES.md` → "Complex scripts". No HarfBuzz, fribidi, ICU, ibus or fcitx.

- [x] Full Unicode bidi (UAX #9): 91,707/91,707 cases of `BidiCharacterTest.txt`.
- [x] Arabic contextual forms, ligatures, marks, cursive (nastaliq), Indic shaper
      (9 scripts) and Thai/Lao: 217/217 sentences identical to HarfBuzz.
- [x] Per-script font fallback in the interface and the terminal (Noto; CJK by language).
- [x] Line breaking for Chinese and Japanese.
- [x] Mirrored interface for RTL languages (`direction` as in CSS, `textAlign` start/end),
      tested in Arabic with both themes.
- [x] Input methods: Korean (hangul), Japanese (romaji → kana + SKK, phrase
      conversion with segments), Chinese (pinyin with abbreviations); in the terminal and in text fields.
- [x] Terminal: Arabic and Hebrew in visual order and shaped complex scripts (per-profile option).
- [x] Khmer and Myanmar shapers and the Universal Shaping Engine (Tibetan, Sinhala, Balinese,
      Javanese, Mongolian and ~80 more scripts): with the above, 409/409 sentences identical to HarfBuzz
      (2026-10-16).
- [x] Thai word-based line breaking with the libthai-data dictionary and our own
      reader (identical to libthai on 705/705 test paragraphs); Khmer and Myanmar by syllable (2026-10-16).
- [ ] Pending: Lao by words, vowel constraints with ◌, Japanese with frequencies.

## M6 — Linux app compatibility (Wayland) ✅ (2026-10-16)

Details in `WAYLAND.md`.

- [x] `libwayland-server` + `wl_compositor`, `wl_shm`, `wl_subcompositor`, `xdg_shell`
      (windows and popups), `wl_seat` (pointer and XKB keyboard), `wl_output`.
- [x] GTK4 (own frame) and Qt6 (theme frame) app windows inside both themes:
      title, focus, move, resize, maximize, minimize, close, Dock.
- [x] Clipboard shared with LordOS in both directions, `xdg-decoration`.
- [ ] Launching `.desktop` apps from the app launcher (needs an approved exception to "the server
      doesn't execute commands"), `linux-dmabuf`, app cursors, drag and drop,
      `text-input`, Xwayland. (Later: the launcher in M7, `linux-dmabuf` with the GPU path, app
      cursors and `cursor-shape-v1` in M8, Xwayland in 0.8.3; drag and drop and `text-input` are
      still missing.)

## M7 — Apps, packages and system services ✅ (2026-10-16)

Defined with the user on 2026-10-16: our own store (Debian apps and native LordOS apps),
our own installer, and network, VPN, sound and clipboard with our own interface.

- [x] Launcher for installed Linux apps (`.desktop`) in the app launcher, the start menu and the
      Dock, with the approved `lord.apps.launch(id)` exception (`WAYLAND.md`, "The launcher").
- [x] Installable native `.lpk` apps (no root), with React and the SDK shared with the theme,
      `<Image pixels>` and `loadWasm` (`NATIVE-APPS.md`); example `examples/apps/fractal`.
- [x] Our own WebAssembly interpreter and the `WebAssembly` global (`WASM.md`): 26,290/26,290
      assertions of the official wg-2.0 suite; 3–5× faster than the same code in JS.
- [x] Our own Debian package engine (`PACKAGES.md`): our own OpenPGP (RSA/Ed25519), xz, HTTP and
      resolver, `dpkg` at the end; plans identical to apt in 12 cases; orphans.
- [x] Store: Debian apps by category (AppStream), search, plan before installing or
      removing, updates, local `.deb` and `.lpk`.
- [x] App launcher and start menu with categories (freedesktop), AppStream icons for apps
      that only ship SVG.
- [x] Settings grouped as in TangoOS (search, user, General, Network, Sound,
      Updates…).
- [x] Clipboard: images between Linux apps that survive the source app, history (❖V).
- [x] Sound on PipeWire with our own client (`SOUND.md`): Control Center and Settings.
- [x] Our own networking (netlink, DHCP, Wi-Fi with wpa_supplicant) and VPN (WireGuard over our own netlink,
      OpenVPN) in Settings → Network (`NETWORK.md`).
- [x] React 19.3.1 (`react-reconciler` 0.34): adapted renderer (`runtime/index.js`), tested with
      both themes, live theme switching, installed apps (.lpk) and Linux apps. lord-server: 34 MB RSS.
- [x] Files (`FILES.md`): tabs, four views, freedesktop
      Trash, Quick Preview, Get Info, background copy/move (`fsops.c`),
      drag and drop, tags, search, mounted disks and «Open With» (`launchWith`).
- [ ] Left over from M7: OpenConnect and IPsec (strongSwan), WPA3/Enterprise, real per-app
      permissions (done in M8, `PRIVACY.md`), a signed LordOS app repository. (OpenConnect and IPsec/IKEv2
      came in M9, `NETWORK.md`; WPA-Enterprise is still missing.)
- [x] `flexWrap` in our own layout engine (`layout.c`).
- [x] Notes: sticky notes on the desktop (widgets), markdown with reading view and index, images,
      attachments and emoji, saved automatically in `~/Notes` (`NOTES.md`).

## M8 — Session and system

- [x] Our own session: `lord-sessiond` with a login screen, lock (❖L), fast user
  switching, parallel sessions and Settings → Users (`SESSIONS.md`).
- [x] Remote desktop on the LAN from the browser (port 5673, off by default;
  `REMOTE.md`). Audio came with WebRTC in 0.8.4 (Opus; see "Remote desktop over WebRTC" below).
- [x] Privacy and signing of `.lpk` apps (`PRIVACY.md`, `SIGNING.md`):
  - [x] Our own sandbox (`server/src/sec/sandbox.c`): Landlock (ABI≤6), seccomp-bpf (allow
    list), no network without permission, `no_new_privs`, dropped capabilities and per-app cgroup v2;
    tested on the VM (`build/sandbox-test`, 7/7). No bubblewrap/flatpak/firejail.
  - [x] per-app permissions: prompts drawn by the system, decisions stored where
    the app can't write (`permisos.json`), Settings → Privacy and the green/orange
    camera/microphone dots in both themes.
  - [x] Our own Ed25519 signing (SHA-256/Ed25519, verify and sign), `contenido.json` with hashes,
    trust levels, signed catalog with revocation, and «Modified by you» / «Restore
    original» / «Share my version». Tests `build/sign-test` (55/55, RFC 8032 + NIST) and
    `build/pkgstate-test` (11/11).
  - [x] `tools/lpk.mjs`: `keygen`, signing when packaging, `sign`, `verify` and `catalog`.
  - [x] Each `.lpk` as an isolated process (`lord-server --app <id>`): a Wayland client with the
    theme's frame, in the Dock and the remote desktop; a broker that checks every `lord.*`; system
    prompts requested by the app. The malicious app (`tests/lpk-evil`) fails at everything at
    run time; the legitimate one (`tests/lpk-legit`) works after consent.
  - [x] Per-session delegated cgroup (`Delegate=yes`, `lord-sessiond` in the `supervisor` leaf):
    `app-lordos-lpk-<id>.scope` and `app-lordos-<id>.scope` (Linux apps) under
    `lordos-session-<uid>`; tested with fast user switching and remote desktop.
  - [x] `sandbox_apply_helper` hardened for lord-media and lord-doc (no sockets or exec, threads
    only, TSYNC, folders by fd, ioctl by class, our own kcmp): `sandbox-test helper` 17/17.
  - [x] Graphical powerbox (a dialog drawn by `lord-server` that hands over only the fd), the
    camera as a read-only frame ring and the microphone stream for the app, live
    tokens/language/RTL and menus for the isolated app (`PRIVACY.md`).
- [x] **Per-app firewall**, steps 1 to 4 (`FIREWALL.md`), tested on VM 122:
  - [x] rule store, precedence, `dnsmap` and JSON (`server/src/fw`, step 1);
  - [x] `lord-netperm`, the root service that enforces the rules with cgroup BPF (connect, sendmsg,
    bind, sock_create) in the sessions' subtree, with our own loader (no libbpf), rules
    compiled to LPM/hash maps (`build/npcompile-test`: 480,000 connections against `fw_decide`,
    0 differences) and names only from the system resolver (systemd-resolved monitor);
  - [x] kernel test `tests/netperm_integration.sh` 18/18: deny/allow, "only
    example.com", LAN only, no incoming, no network, off;
  - [x] Monitor: the Rule column is a per-app switch (`lord.fw.setAppRule`, enforced in the
    kernel) with allowed/denied counters (`MONITOR.md`, "Phase 2"); Settings → Privacy: mode,
    Trusted apps and Restricted apps;
  - [x] non-network restrictions (camera, microphone, screen, USB, files, clipboard, Bluetooth,
    notifications) enforced for LordOS apps in the broker, `js_media.c` and `sdk/notify.js`, with
    live revocation (`FIREWALL.md`, "Non-network restrictions"); Linux apps: network only (later
    also devices, files and notifications, `FIREWALL.md`, "Linux apps");
  - [x] root-owned scopes: `lord-sessiond` places every app process and a Linux app can't move
    itself to another cgroup (VM 122). Then still open, closed since: `systemd-run --user --scope`
    (PID 1 moves it into `user@<uid>.service`; `lord-netperm` now holds it to the per-user union and
    device-restricted apps get their own view, `FIREWALL.md`, «Escape via `systemd-run --user`»).
  - [ ] "ask" mode (designed). (Per-destination switches: done in 0.8.2, `MONITOR.md`.)
- [x] **Monitor** app, phase 1 (`MONITOR.md`), tested on VM 123:
  - [x] Overview, CPU, Memory, Network, Disks and Processes tabs with the look of TangoOS's
    Monitor;
  - [x] metrics read by `lord-server` in its own thread, at 4 Hz and only while the app is open
    (closed, it costs nothing);
  - [x] per-app connections with sock_diag, from process to app through the `.desktop` files;
  - [x] our own vector map with zoom and arcs, in the style of a per-app network monitor;
  - [x] country by IP with our own MaxMind DB reader and DB-IP Lite (CC BY 4.0), no network;
  - [x] item in Settings → Privacy.
  - [x] Phase 2, firewall: see "Per-app firewall" above.
  - [ ] Phase 2, the rest: connection alerts ("ask"), camera and microphone in use (`monitor.deviceUsers()`)
    and usage alerts. (Host names from the system resolver, without reverse DNS, and the traffic
    history: done in 0.8.2, `MONITOR.md`.)
- [ ] VT switching. (GPU sessions: done, `lord-sessiond` lends the card to the visible session,
  `SESSIONS.md`; multiple monitors: done in M9, `DISPLAYS.md`.)
- [ ] A native browser. (The text editor and the image viewer are done: `EDITOR.md`, Photos in
  `MEDIA.md`.)
- [x] Installable image based on minimal Debian 13, with LordOS as `.deb` packages: the live ISO
  with its installer (`ISO.md`), and since 0.8.1 updates from the signed APT repository (`UPDATES.md`).
- [ ] **ARM64 / Raspberry Pi 5 as an official platform** (`RASPBERRY.md`):
  - [x] Portability audit of `server/` for aarch64: unsigned char, 16K pages,
    arm64 seccomp, alignment and `ARCH` in the Makefile (no `-march=native`).
  - [x] Optional per-architecture NEON in `canvas.c` (blit and masks), bit-identical to C
    (`build/canvas-test`); `STBI_NEON`.
  - [x] Separate display and GPU (vc4 + v3d):
    - the card with connectors (`drmcard.h`), in lord-server and lord-sessiond;
    - `IN_FORMATS` of the primary plane and direct scanout only with accepted modifiers;
    - the EGL render node in the dmabuf feedback.
  - [x] `tools/rpi/build-img.sh`: `lordos-<version>-rpi5-arm64.img.xz` for Raspberry Pi Imager:
    - Debian 13 arm64 with the Raspberry Pi kernel and firmware;
    - the same `.deb` as the ISO;
    - the partition grows by itself;
    - first boot with `lordos-user.txt`/`userconf.txt` (login screen) or setup
      mode.
  - [x] LordOS build of the Raspberry Pi kernel with Landlock (`tools/rpi/build-kernel.sh`,
    `6.18.57-lordos-rpi-2712`), pinned over the archive kernel.
  - [x] Switch files on the boot partition: `ssh`, `remote-desktop`, Wi-Fi in `lordos-user.txt`;
    `tools/rpi/verify-board.sh` after flashing.
  - [x] Run `build-img.sh` and test on the 16 GB Pi 5 (test list in `RASPBERRY.md`; the image is
    a release file since 0.8.0; results in `RASPBERRY.md` and the release tests in `ISO.md`).
  - [ ] Hardware video on the Pi 5 (HEVC via V4L2 stateless; H.264 in software) in
    `lord-media`.
  - [ ] A «create user» step on the login screen, instead of setup mode.

## M9 — Documents and printing ✅ (2026-10-17)

Details in `DOCUMENTS.md` and `PRINTING.md`. Tested on VM 116 with screenshots (`archives-*`, `pdf-*`, `print-*`, `fonts-*` and related `.png` files).

- [x] **`lord-doc`, the sandboxed helper:**
  - it is the only binary with Poppler (glib interface) and libarchive;
  - it is launched on demand with a fixed path and no arguments, and works only with the fds
    lord-server passes it;
  - it locks itself in with `sandbox_apply_helper`: no network or sockets, Landlock with writes only
    under the destination, seccomp, cgroup and a memory cap;
  - pages come back in sealed memfds;
  - the compositor never waits: there are deadlines, and if the helper crashes nothing else happens.
- [x] **PDF Viewer:**
  - continuous and single-page scrolling, thumbnails, outline;
  - search with highlighting, zoom and fit, rotate, select and copy, go to page, links;
  - annotations (highlight, note, freehand) saved as a copy, forms;
  - inverted dark page;
  - print.
- [x] **Files:**
  - looking inside archives as if they were a folder;
  - Extract Here (zip, tar.gz/xz/zst/bz2, 7z, bare gz) without leaving the folder, with
    `arcpath-test` and malicious archives: 31/31;
  - Compress to `.zip`;
  - Quick Preview for PDFs, fonts, archives and what is inside an archive;
  - Quick Preview for audio and video with `media.poster` when available.
- [x] **Fonts:** samples, character table by block, information, installing and removing
  user fonts.
- [x] **Emoji & Symbols:**
  - global picker (❖.) with search in es/en/pt (CLDR), categories, recents and skin tone;
  - it types through the input method path;
  - an app with Unicode symbols by block.
- [x] **Printing:**
  - our own IPP client for CUPS (no libcups), our own mDNS;
  - Settings → Printers & Scanners: add driverless printers (IPP Everywhere), manual URI,
    default, queue and cancel, administration through `lpadmin` + PeerCred;
  - the system print dialog with preview, used by the PDF Viewer, the Editor and
    Notes (Photos has the API ready).
- [x] **Scanner:** our own eSCL client, no SANE: preview, area, resolution, color, and
  saving as PDF/PNG/JPEG. Tested with a test scanner (`tests/escl_server.py`).
- [ ] **Pending:**
  - PDF thumbnails in icon view;
  - encrypted archives;
  - PDF signatures;
  - scanners and `ipps` over TLS from lord-server;
  - complex-script shaping in printed text;
  - ZWJ emoji in the text engine;
  - testing with real printers and scanners.

## M9 — Native media apps ✅ (2026-10-17)

Details in `MEDIA.md`. Tested on VM 119 with screenshots (`photos-*`, `music-*`, `video-*`, `recorder-*`, `screenshots-*` and related `.png` files).

- [x] **`lord-media`, the sandboxed helper:**
  - it is the only binary with FFmpeg (libavcodec, libavformat, libswscale, libswresample) and libva;
  - it is launched on demand with a fixed path and no arguments, and works only with the fds
    lord-server passes it (files, camera, render node);
  - `sandbox_apply_helper`: no sockets, exec or network, ioctl by class (DRM, V4L2) only when
    needed, TSYNC; `media-test sandbox` verifies it;
  - frames come back through three sealed shared-memory slots; if it crashes, only the
    window finds out (tested with a real segfault).
- [x] **Audio over shared memory:** the PipeWire stream belongs to lord-server; the PCM goes through a
  ring and the video clock through a seqlock on the same page (no round trips). Mean A/V offset
  0.5–1.9 ms. The microphone, the other way around.
- [x] **Player:**
  - video with synchronized audio, time bar, volume, `.srt` and in-track subtitles;
  - fullscreen and picture in picture (`wm.setFullscreen`/`wm.setPip` in the SDK);
  - music library by artist and album with cover art, queue, shuffle and repeat;
  - media keys and the Control Center tile.
- [x] **Photos:** photo library from the pictures folder (`~/Pictures` in English) with thumbnails in `~/.cache/lordos/thumbs`, viewer with zoom,
  panning, rotation, fullscreen, slideshow, animated GIF, HEIC/AVIF/WebP/BMP, basic
  editing saved as a copy, and printing.
- [x] **Screenshots:**
  - screen, window or area, and a timer;
  - to the clipboard or to `~/Pictures/Screenshots`;
  - floating thumbnail and annotation editor;
  - screen recording to MP4 (H.264 + AAC from the microphone);
  - shortcuts ❖⇧3/❖⇧4/❖⇧5 and Print Screen.
- [x] **Protected windows** (`wm.setProtected`): black in screenshots, recordings, the remote
  desktop and thumbnails; also Wayland apps and their menus.
- [x] **Camera:** our own V4L2, JPEG photo, H.264 + AAC video, mirror, camera choice (tested with
  v4l2loopback).
- [x] **Recorder:** microphone over PipeWire, waveform, rename, trim and export to Opus,
  MP3 or WAV.
- [x] **Files:** «Open With» Photos and Player, video thumbnails, cover art and WebP/HEIC/AVIF;
  `media.poster` for Quick Preview.
- [x] **Performance:** painting straight from the video when only an opaque frame changed: lord-server with
  a 1080p video went from 42 % to 13 % of a core windowed and from 92 % to 15 % fullscreen.
- [ ] **Pending:**
  - measure VA-API on the AMD PC (the VM has no driver);
  - zero-copy dmabuf → EGL on the GPU path;
  - HEVC via `v4l2request` on the Raspberry Pi 5;
  - image-based subtitles;
  - recording an area and the system audio;
  - saving as WebP.

## M9 — System apps and system network ✅ (2026-10-18)

Details in `SYSTEM.md` (apps), `NETWORK.md` (network) and `SESSIONS.md` (user folders). Tested on VM 119
with screenshots (`backup-*`, `disks-*`, `logs-*`, `clock-*`, `help-*` `.png` files).

- [x] **The network belongs to the system (`lord-netd`):** a small root service started and
  supervised by `lord-sessiond` at boot. DHCP, saved Wi-Fi, saved configs and DNS come up before
  anyone logs in (IP on the greeter, remote desktop reachable); sessions forward their `net-*`
  actions over `/run/lordos/netd.sock`. It survives two users logging in and out, its own crash
  (the DHCP lease is resumed without dropping the address) and a `lord-sessiond` restart (adopted
  through a pidfd).
- [x] **User folders:** the installer and Settings → Users create Escritorio, Documentos,
  Descargas, Música, Imágenes and Videos in the user's language plus `user-dirs.dirs`
  (`users/homedirs.c`, with `build/homedirs-test`).
- [x] **Disks** (`apps/disks`): disks and partitions with usage, our own SMART reader (ATA over
  `SG_IO`, NVMe health log; `build/smart-test`), mount/unmount/eject, format FAT32/exFAT/ext4/NTFS,
  rename, erase, bootable USB from an ISO/IMG verified with SHA-256; closed `disk-*` actions that
  never touch the system disk.
- [x] **Backups** (`apps/backup`): scheduled hard-link snapshots of the home, hourly/daily/weekly
  retention, browse by date and restore, exclusions, optional encryption with Monocypher; a
  lord-server thread at nice 19 and idle I/O, paused on battery (`build/backup-test`).
- [x] **Logs** (`apps/logs`): our own systemd journal reader, the user's own journal and LordOS
  logs, system logs for admins through a closed action, filters, search, live follow and export
  (`build/journal-test`).
- [x] **Clock** (`apps/clock`): world clock with our own TZif reader (`build/tzif-test`), alarms that
  fire with the app closed, timer and stopwatch, a menu-bar status item.
- [x] **Help** (`apps/help`): searchable markdown articles in es/en/pt, a skippable welcome tour on
  first login, links to Settings pages, indexed by Search.
- [x] **Session notifications** (`sdk/notify.js`) and **menu-bar status items**
  (`sdk/statusitems.js`) for any theme.
- [ ] **Pending:**
  - install a new ISO to disk and repeat the network-before-login and user-folder checks there;
  - test `lord-netd` with real NICs and Wi-Fi; a per-VPN "connect at boot" option;
  - Disks: GPT for disks over 2 TiB, manual partitioning, LUKS/LVM, real NVMe;
  - Backups: test pausing on a real laptop battery, store the passphrase in the Passwords keyring;
  - Logs: zstd-compressed journal fields;
  - RTL screenshots of the five apps.

## M9 — Passwords and the system keyring ✅ (2026-10-18)

Details in `PASSWORDS.md`. Tested on VM 116 with a real `lord-sessiond` session
(`passwords-*.png`).

- [x] **`lord-vault`**, sandboxed helper (no network, no sockets, no exec, fd-only files); the only
  binary with Monocypher; decrypted data in an mlock/MADV_DONTDUMP arena, wiped on lock.
- [x] **Own `.lvault` format:** XChaCha20-Poly1305, vault key wrapped per unlock method (Argon2id
  tuned per machine, BLAKE2b), attachments sealed separately, atomic save with `.bak`; every flipped
  byte and every truncation detected (`vault-test`, 74/74).
- [x] **Unlock:** master password, FIDO2 `hmac-secret` key (own CTAP2 client), key file on a USB
  stick, password + key file, printable recovery key. Auto-lock on idle, ❖L and user switch.
- [x] **App:** sidebar (All, Favorites, Archived, 2FA codes, Wi-Fi, Secure
  notes, Cards, Security), KeePassX-style category tree with 16 built-in templates and user
  categories with their own fields, tags, history, trash, attachments, rotation reminders, Wi-Fi QR,
  TOTP with countdown (also from a QR image), generator, security report (offline breach check),
  import (own KDBX 4/3.1 reader, Bitwarden, Chrome, Firefox, CSV from other password managers) and export. Protected window.
- [x] **Clipboard:** secrets marked sensitive (out of the history, `x-kde-passwordManagerHint` for
  Wayland clients), cleared after 30 s.
- [x] **Session keyring** (`login.lvault`) opened with the login password via `lord-sessiond`;
  `keyring` API per app with sharing; Settings → Network stores Wi-Fi/VPN secrets there;
  `<AutofillButton>`.
- [x] **Secret Service** with our own D-Bus code (plain + DH sessions): `secret-tool` and Chromium
  work without gnome-keyring.
- [x] **The Passwords UI runs as its own sandboxed process** (`lord-server --app passwords`, a
  system package built into `assets/sysapps/`): its own channel to `lord-vault`
  (`vc_attach_app`), files through the powerbox, FIDO2 and secret clipboard through the broker
  (`BOP_VAULT_FIDO`, `BOP_CLIP_SECRET`), protected Wayland window. Revealed values never enter
  `lord-server`'s JavaScript heap. In-process fallback kept (see `PASSWORDS.md`).
- [x] Keyring for isolated `.lpk` apps: a broker op (`BOP_KEYRING`, `sec/brokerkr.c`) that sets
  the owner from the verified app id (`PASSWORDS.md`, "Keyring for isolated .lpk apps").
- [ ] Test with real FIDO2 keys (only the uhid software authenticator was available).

## M9 — Settings: hardware pages ✅ (2026-10-18)

Details in `SETTINGS.md`. Tested on VM 118 (`settings-*.png`), Bluetooth with a virtual controller
(`btvirt`).

- [x] **Displays:** outputs, resolutions and refresh rates from KMS with EDID names; live modeset
  with the system's 15-second "Keep these display settings?"; saved per display (EDID identity) and
  used from the first frame; main display choice and move on unplug (DRM uevents); software rotation
  and a doubled 200 % scale (CPU path); brightness through a closed action; Night Light on the gamma
  LUT or by software, scheduled sunset-to-sunrise from the time zone's coordinates or custom hours.
- [x] **Bluetooth:** BlueZ over our own D-Bus client: power, discovery, pair (system-drawn PIN,
  passkey and confirmation prompts), connect, disconnect, forget; headsets in Sound through
  PipeWire.
- [x] **Energy:** battery state, time and health from sysfs, low-battery notices, percentage in the
  menu bar, power profiles (power-profiles-daemon or the cpufreq governor), display off / lock /
  sleep after N minutes, power button and lid (logind drop-in).
- [x] **Notifications:** per-app allow, style, sound; Do Not Disturb with a schedule; grouping;
  `org.freedesktop.Notifications` for Linux apps; packages use the sandbox permission.
- [x] **Date & Time:** NTP (timesyncd) or manual time, manual time zone (never from the network),
  12/24 h, menu bar clock format.
- [x] **Default Apps:** `mimeapps.list` and `xdg-terminals.list`; Files' "Open With" and
  `lord.apps.launchWith('', path)` honor them.
- [ ] **Pending:** Night Light on the GPU path without a LUT; real laptops (battery, backlight, lid)
  and real Bluetooth controllers; notifications on the lock screen and a notification center.
  (Done since: extend/mirror across displays and sharp HiDPI at 100–200 %, `DISPLAYS.md`; Wayland
  idle inhibit in 0.8.5.)

## M9 — Mail, Calendar, Contacts and Reminders ✅ (2026-10-18)

Details in `MAIL.md`. Tested on VM 123 against local Dovecot, Postfix and Radicale with a test CA
(`mail-*.png`, `calendar-*.png`, `contacts*.png`, `reminders*.png`).

- [x] **`lord-maild`**, sandboxed helper with no sockets of its own: `lord-server` connects only to
  the configured servers (or public image hosts after *Load Images*) and passes the fd. OpenSSL,
  certificates verified against the system CA store, explicit per-account trust otherwise.
- [x] **IMAP** (IDLE, CONDSTORE, MOVE, UIDPLUS, SPECIAL-USE, modified UTF-7), **SMTP** 465/587,
  local cache and offline prefix search; a 10,000-message folder syncs in ~2 s and opens from the
  cache in ~20 ms; virtualized list.
- [x] **Mail app:** three panes, toolbar, unified inbox, conversations, flags, six categories (IMAP
  `$label1…6`), attachments through the system's default apps, signatures, rich text / markdown /
  plain compose, server drafts, send later, new-mail notifications.
- [x] **HTML** through our own sanitizer into native views (no web engine), remote images blocked
  until asked, tracking pixels dropped, deceptive links flagged; plain-text fallback.
- [x] **Accounts:** any IMAP/SMTP; popular providers (Gmail, Yahoo and others) from our own table; app
  passwords in the keyring; OAuth prepared and disabled ("needs your client ID").
- [x] **Calendar** (day / week / month, colored calendars, local and CalDAV, recurrence),
  **invitations** from Mail (Accept / Maybe / Decline with iTIP REPLY), **Reminders** (lists, due
  dates, priorities, flags, smart lists, VTODO over CalDAV), **Contacts** (local and CardDAV,
  groups, photo; Mail autocompletes from it). Alarms ring with the apps closed.
- [x] **Isolated apps (0.8.2):** Mail, Calendar, Contacts and Reminders run as their own sandboxed
  processes (`lord-server --app <id>`), with `BOP_MAIL` and per-app allow lists in the broker
  (`sec/brokermail.c`, `MAIL.md`).
- [x] **Tests:** `mail-test` 30,953 checks (IMAP, MIME, charsets, HTML sanitizer, iCal, vCard, XML,
  mutation fuzzing), `mailnet-test` 138, `mailc-test` 69; ASan/UBSan target.
- [ ] Real providers (Gmail, Yahoo and the others in the table) and OAuth2 with a registered client ID.
- [ ] Edit a single occurrence of a repeating event; create calendars/address books on the server.
- [ ] CJK multibyte charsets; S/MIME and OpenPGP; QRESYNC.

## Before the final ISO ✅ (all done; shipped in 0.8.0)

- [x] **Mail, Calendar, Contacts and Reminders** (see "M9 — Mail, Calendar, Contacts and
  Reminders" above and `MAIL.md`).
- [x] **Missing Settings sections:** Displays, Bluetooth, Energy, Notifications, Date &
  Time, Default Apps (see "M9 — Settings: hardware pages" above and `SETTINGS.md`).
- [x] **Chromium and Firefox** preinstalled (Linux apps, not native; verified in the final ISO, see
  `docs/ISO.md`, "Final ISO test").
- [x] **Camera in isolated apps** (`.lpk`) and **several windows per isolated app** (VM 122,
  `PRIVACY.md`): read-only frame ring from `lord-media` with validated ACKs, photo/video through
  the powerbox, instant revoke; one Wayland toplevel per window, grouped in the Dock, «New
  window» from the app and the Dock.
- [x] **Visual polish, app by app** (before the ISO; done 2026-10-18, see `docs/polish/`): review every native app
  (Settings, Files, Terminal, Store, Editor, Notes, Calculator, Monitor, PDF, Fonts,
  Emoji, Scanner, Photos, Player, Screenshots, Camera, Recorder, Passwords, Disks, Backups,
  Logs, Clock, Help, Mail…) following our design guide, modeled on widely used desktop human-interface conventions (`DESIGN.md`): 8 px grid,
  uniform margins and spacing, alignment, typographic hierarchy, sidebars,
  toolbars and sheets as in the design guide, controls with the same size and height, nothing overlapping or
  clipped; in light, dark, RTL and in small windows; with before and after screenshots.
- [x] **Final ISO** with everything (check that `lord-doc` and `lord-media` are inside) and a **Raspberry
  Pi 5 image** (`tools/rpi/build-img.sh`), tested on the real Pi: released as 0.8.0 (`ISO.md`,
  `RASPBERRY.md`).

## Release 0.8.5-beta ✅ (2026-10-27)

Released 2026-10-27 (`CHANGELOG.md`, "0.8.5-beta"; ISO `lordos-live-0.8.5-amd64.iso` and the
Raspberry Pi 5 image `lordos-0.8.5-rpi5-arm64.img.xz`). Updated in place from 0.8.4.1 and 0.8.4 with
no migration (`UPDATES.md`). What it delivered, each item described in its own document:

- [x] Monitor → Network, second generation, the menu bar network item, blocklists and the "Deny
  silently" mode; LordOS's own components (mail, Store, printing, Terminal, blocklists) in their own
  firewall scopes (`MONITOR.md`, `FIREWALL.md`; item below).
- [x] The Store: a new look, the Artificial Intelligence catalog and the Commercial page (vendor
  repositories added with `pkg-source-add`), a signed featured banner, downloads over HTTPS
  (`lord-tls`) and apt's priorities (`PACKAGES.md`, `UPDATES.md`, "Mirrored apps (catalog)").
- [x] System Information with its Sandbox Probe, `lordos-sysreport`, `lordos-fetch`, the Test Bench,
  the boot time on the splash and on record, and versioned results (`BENCHMARK.md`, `PRIVACY.md`,
  `RESULTS.md`).
- [x] Files: This Computer and every disk (item below; `FILES.md`, `SYSTEM.md`).
- [x] Root over SSH as an advanced option (`REMOTE.md`, "Root over SSH").
- [x] Settings → Displays: numbers, Identify and snapping; one place to choose the desktop style;
  the themes renamed Lord Flow and Lord Classic; the app menu and Force Quit, a fuller Window menu,
  menus inside the window in Lord Classic, Dock Open at Login and Assign To (`SETTINGS.md`,
  `WINDOWS.md`); a guide to building an appearance (`THEMES-GUIDE.md`); a version per native app
  (`NATIVE-APPS.md`).
- [x] The login screen runs only while it is shown, follows a monitor plugged in after boot, and lock
  and user switch are faster (`SESSIONS.md`); software vblank pacing on drivers that complete flips at
  once (`CHANGELOG.md`, `RESULTS.md`); idle-inhibit for Linux apps (`WAYLAND.md`).

Flatpak and Flathub are not planned for any release (decided by the maintainer on 2026-10-27, item below).

## Release 0.8.6 (in development)

LordBench and a desktop that stays smooth under load. Each item is described in its own document.

**LordBench** (`BENCHMARK.md`; the item under "Before 1.0" has the details):
- [x] The sandboxed `lord-bench` helper: CPU, memory, GPU, storage and sustained workloads; the thermal, throttle
  and battery guards; versioned results and System Information → Performance.
- [x] Video workloads (software decoding, the remote desktop's encoders), the desktop-impact mode, the
  pointer-to-screen latency, the N-boot test, `lordos-sysreport bench` and the Pi 5 reference.
- [x] LordOS's own interface work as workloads (`cpu.ui`, `cpu.shape`, fonts built into the helper) and the
  helper-sandbox self-test inside `lord-bench` (2026-10-28).
- [ ] The interface workloads measured on the Pi 5 for its reference. *Two Quick runs from a terminal (2026-10-29):*
  layout and paint 39.3–39.4 fps on one core, 45.4 fps with the paint threads; shaping 630–632 runs/s on one core,
  2,522–2,527 on four (`BENCHMARK.md`); the reference waits for a Full run.
- The x86 PC reference and hardware video decoding moved to 0.8.7 (decided by the maintainer on 2026-10-29;
  "Release 0.8.7 (planned)" below).
- [x] Detecting the Pi 5 firmware's throttling. *Built (2026-10-28/29), checked on the Pi 5 on 2026-10-29:* the
  LordOS kernel (`rpi-6.18.y`) only exposes the firmware's under-voltage alarm (`rpi_volt`'s `in0_lcrit_alarm`, the
  last 2 s), not its capped, throttled or soft-limit bits, so LordBench watches that alarm and `get_throttled` where
  an older kernel has it, and counts 80 °C on a Pi as the firmware's limit (peak tests stop as throttled); System
  Information's Raspberry Pi rows read the same way through the shared `hw/rpifw.c`. Tested on fake sysfs trees, and
  on the Pi 5: the `rpi_volt` alarm is present and reads 0 with the official supply (`piFirmware.voltageAlarm` true,
  `underVoltage` false), `get_throttled` absent. An actual under-voltage was not provoked.

**The desktop under load** (`BENCHMARK.md`, "The desktop under load"; `GPU.md`). On the Pi 5 with its monitor the
desktop fell to 24 fps under an all-core load:
- [x] The compositor's cgroup CPU weight and its real-time priority (main and paint threads at `SCHED_RR` 2): the
  main thread no longer waits for a CPU; 24 → 29–30 fps on the Pi 5.
- [x] Black squares at a dragged window's rounded corners (also in 0.8.5, CPU and GPU paths, the remote desktop's
  frames too): the corner restore on the drag layer now mixes both pixels alpha included (`mix_px` in `canvas.c`),
  checked against a full repaint by `test_drag_corners` on both paths (`WINDOWS.md`).
- [x] The frame-cost work (`WINDOWS.md`, `GPU.md`, `BENCHMARK.md` "The drag frame's cost"): steps 0–11, pixel-identical
  (golden hashes of 2,417 drag frames per path) except the approved 1-level corner change of step 9; LordBench's
  square and the Cooling fans turn (the `rotate` key); a transformed node repaints when its own style changes; GPU
  composition without `GL_EXT_unpack_subimage`; the dragged window composited on the GPU ("GPU drag", step 10) with
  its self-test, `LORD_GPU_DRAG=0` and `LORD_GPU_VERIFY`; LordBench's per-frame phase timing (CPU-late or GPU-late per
  frame, GPU drag's refusals by cause); GPU drag's textures catching up in slices between frames; the paint pool
  turning on for slow whole frames; `lord.stats()` in O(1) (a QuickJS patch, `THIRD_PARTY.md`). Pi 5 with its monitor,
  GPU drag off: alone 53–58 → 59.8–60 fps, under load 24–25 → 30 fps; with GPU drag on it stalled (52 fps alone; that
  run predates the slices and the phase timing).
- [x] Build the next frame while the previous flip is pending (the frame pipeline, `fpipe.c`, `LORD_PIPELINE`; `GPU.md`,
  "The frame pipeline"): one display with real page flips, pixels unchanged (`fpipe-test`, `engine-test`'s pipelined
  drag runs). Estimated 30 → 52–58 fps under load; measured on the Pi 5, 30.8 → 33–36 fps for 17–29 ms more pointer
  latency, so it is off by default (`LORD_PIPELINE=1` while frames are heavy, `2` always).
- [x] Measure the frame-cost work, GPU drag and the frame pipeline on the Pi 5 with its monitor and the phase timing
  (`BENCHMARK.md`, "What was run on the Pi 5"), with `LORD_GPU_VERIFY=1` once and a real screenshot (2026-10-29).
  Both off (the defaults now): 59.9 fps alone, 30.5–31.1 fps under load, pointer to screen p50 25.7–25.9 / 48.5–49.0
  ms; GPU drag on: 59.9 and 30.8–31.6 fps; the pipeline while frames are heavy: 60.0 and 33.0–36.2 fps, pointer
  42 / 78 ms. GPU drag's self-test passes on V3D and 138 verified frames had 0 differences; the dragged window's
  corners are clean on the monitor. Under load 320 of 374 frames are GPU-late (the GPU's time per frame 1.5 → 14–16
  ms, no clock or thermal throttling).
- [x] Decide GPU drag's default after that run: **off** (`LORD_GPU_DRAG=1` turns it on): correct on V3D, no
  measurable gain.
- [ ] Step 12: GPU drag for the whole drag, also where the Dock or the menu bar overlap the window.
- [ ] Next on the Pi 5 under load: fewer pixels uploaded and composed per frame (a buffer-age scissor so the GPU
  composes only the damage, PBO uploads or a linear canvas) and finding what slows V3D under an all-core CPU load
  (14–16 ms of GPU a frame instead of 1.5 with no clock change; memory contention is the likeliest cause, not proven:
  read a memory-bandwidth counter). Optional: a 1000 Hz kernel tick for the Pi 5.
- [x] Merge `perf-frame` (2026-10-29), with CHANGELOG, `BENCHMARK.md`, `GPU.md`, `WINDOWS.md`, `FANS.md`, the paper,
  the FAQ, the features page and `THIRD_PARTY.md` (the QuickJS `JS_GetMallocSize` patch), with the final Pi 5 run's
  numbers and the frame pipeline and GPU drag off by default.

**Release:**
- [ ] Re-measure the references after the release (results compare only within one LordOS version).
- [ ] Release cleanup, packages, the Pi 5 image and the ISO, as for 0.8.5.

## Release 0.8.7 (planned)

Community hardware reports (the item under "Before 1.0", `BENCHMARK.md`), and what LordBench left for after 0.8.6:
- [ ] The x86 PC reference for LordBench (a modest PC still to be chosen; moved from 0.8.6 on 2026-10-29).
- [ ] Hardware video decoding (VA-API on PCs; the Pi 5's HEVC decoder needs FFmpeg's `v4l2request`, which Debian's
  FFmpeg lacks), in `lord-media` and LordBench's video workloads (moved from 0.8.6 on 2026-10-29).

## Before 1.0

- [x] **Monitor → Network, second generation (0.8.5)** — done 2026-10-19 on VM 118, see
  `MONITOR.md` ("Network, second generation", "The menu bar network item", "Blocklists") and
  `FIREWALL.md` ("LordOS components in their own scopes", "The essential system services",
  "Blocklists", "The modes"). Our own design; same function as the
  best per-app firewalls, no copied look or names.
  - **Native apps as their own rows:** every built-in component that uses the network gets a row
    with its name and icon, backed by its own process and cgroup so rules are enforced by the
    kernel: Mail (`lord-maild`, also the Calendar, Contacts and Reminders sync), Store and
    updates (the package engine), Remote desktop, Printers, VPN, and the Terminal (its programs in
    their own scope, shown as "Terminal" with the program's name). Apps that never touch the
    network (Calculator) don't appear. Essential system traffic (DNS, time, DHCP) is shown but
    can't be blocked; blocking Store and updates warns that security updates stop. Goal: the
    "LordOS" row near zero; anything left gets its own process.
  - **A tree with traffic everywhere:** app → its domains and IPs (and ports under them), each row
    with sent and received bytes, a small up/down activity meter and its allow/deny switch
    (domain rules from the row, e.g. deny one tracker and keep the site). "N more…" folds long
    lists.
  - **Left column view selector** (a pop-up button): group by app, domain, IP, port or country;
    direction filter (all, incoming, outgoing); protocol filter (all, TCP, UDP/QUIC); status
    filter (all, allowed, denied, with rules); sort by name, most traffic, least traffic, most
    recent, most connections. Remembered per user.
  - **Search with tokens:** typed search turns into chips for app, domain, IP/CIDR, port,
    protocol, country and city, combinable (an app chip plus a domain chip).
  - **Time range:** the traffic histogram (sent up, received down) with zoom and back/forward by
    hour, day and week; the list follows the selected range.
  - **Menu bar network item** (optional, off by default): live up/down rate in the status area;
    its panel shows the sent/received graph of the last minutes, recent network activity by app,
    "Recently denied" with a count, quick controls (firewall mode: ask, allow silently, deny
    silently; rule profiles such as Home/Public) and a link to the Monitor.
  - **Blocklists:** a curated catalog of public lists by topic (malware, advertising, trackers),
    domain- and IP-based, each with its licence checked before inclusion; enable with a checkbox,
    entry counts shown, daily updates downloaded over HTTPS by an unprivileged fetcher with size and
    format limits, parsed strictly, compiled into the existing domain/IP rule maps; custom list
    URLs allowed. Lists never override a user's explicit allow rule.
  - Performance budget measured as in 0.8.3 (no visible cost with thousands of list entries).
  - **Done as described, except:** no city chip and no grouping by city (no city-level IP database
    with a compatible licence); a history window lists apps only (the history never keeps
    destinations); the switches are on destinations, not on single ports; the menu bar item offers
    allow silently / deny silently / off (the ask mode doesn't exist yet) and no rule profiles
    (0.8.5); "Recently denied" counts the last ten minutes.
  - [ ] Test Mail's and the printers' connectors against a real mail server and a network printer
    (the same connector was tested live with the blocklists); a laptop's real DHCP and VPN rows.
  - [ ] The per-packet cost of the per-port accounting (+0.6 µs at 104,000 packets/s): find it.

- [ ] **Firewall rules manager (its own release after 0.8.5)** — a "Rules" section in the Monitor (our own design):
  - **Sidebar:** All rules, Active, Denied, Recently modified, Recently used, Temporary, Unapproved
    (rules created silently, waiting for review, with a count); then Rule groups, Blocklists and
    Profiles, each with an on/off checkbox.
  - **Table:** process (with icon; "Any process" for global rules) → its rules, each with an
    enable checkbox, allow/deny icon, a readable sentence ("allow outgoing connections to domain
    example.org"), lock for system rules, a tag for the group it belongs to; detail panel with
    the app's identity (path, package, signature check), priority, owner, the rule's usage
    statistics (times applied, last used) and an editor.
  - **Rule scope:** app → servers, domains, IP/CIDR, ports and port ranges, protocol (TCP, UDP,
    ICMP), direction (outgoing, incoming, both), local network only; durations (forever, until
    the app quits, until restart, once, for N minutes); actions beyond allow/deny: notify, play a
    sound (optionally scaled by traffic volume).
  - **Identity that survives updates:** Linux apps identified by their Debian package and
    `.desktop` id (not a versioned path); system code by LordOS's signed manifest; rules don't
    break when an app updates.
  - **Rule groups:** local groups by topic, and subscriptions to remote rule-group files over
    HTTPS (validated, size-limited, unprivileged fetcher, never able to allow what a local deny
    rule denies), switched on/off together.
  - **Profiles:** sets of rules and groups (Home, Work, Public…), switched by hand, from the menu
    bar item, or automatically by Wi-Fi network / connection; "effective in all profiles" rules.
  - **Tidy up:** flags invalid rules (app no longer installed), redundant ones (covered by a
    broader rule), never-used ones, and suggests new rules from what was recorded silently.
  - **Quick filters** in the search field (deny rules only, temporary, from a group) and
    import/export of rules as JSON.
  - **Encrypted DNS:** DNS over HTTPS / TLS for the system resolver, with a short list of
    privacy-respecting resolvers to choose from and a custom one (Settings → Network, shown in
    the Monitor).
  - **Command line:** `lordos-fw` to list, add and remove rules, switch profiles, export, and log
    allow/deny events, through the same validated API as the Monitor (no root shortcut).

- [x] **System Information (released in 0.8.5-beta, 2026-10-27; the open sub-items remain)** — design in `BENCHMARK.md`. A native app (`apps/sysinfo`,
  also About LordOS → System Report…) with every piece of hardware detected and what LordOS uses:
  each GPU as separate layers (device, driver, DRM, render node, EGL context, composition, scanout,
  video decode and encode), the services, a Compatibility panel, and the paper's claims checked on
  this machine, each marked *configured* or *effective* (a probe app tries what must fail). JSON
  and Markdown reports with preview, built from an allow-list of fields. The logic lives in
  `server/src/sysreport/`, shared by the app, the `lordos-sysreport` command and tests.
  - [x] collector, `pci.ids`/`usb.ids`/EDID readers, fake-sysfs fixtures, anonymization test,
    `lordos-sysreport report` (2026-10-26);
  - [x] the GPU's live layers (EGL, composition, scanout) through `lord.sysinfo` (2026-10-26);
  - [x] the read-only `hw-memory-modules` admin action (2026-10-26);
  - [x] Overview, Hardware and Compatibility tabs (en, es, pt; 2026-10-26; right to left not yet checked on screen);
  - [x] Checks part 1: what lord-server sees itself, configured and effective (2026-10-26);
  - [x] Checks part 2: the Sandbox Probe tries what must fail (app sandbox, helper sandbox, broker, scope; 2026-10-26);
  - [ ] a connection refused by the per-app firewall (an app with network access and a temporary rule);
  - [x] reports: the Report tab with the exact text, Copy and Save to Documents (2026-10-26);
  - [ ] first real reports from the RX 580 PC, and a full report from the Pi 5 with a monitor (LordBench has measured
    the Pi 5 on its monitor, `BENCHMARK.md`; System Information's report from it is not published yet).
  - [ ] from the live USB without installing: a *Check this computer* boot entry, reports marked
    live, nothing written to the computer's disks, saved to a removable drive; the installer shows
    the Compatibility summary before installing.
- [x] **Files: this computer and every disk (released in 0.8.5-beta, 2026-10-27; approved 2026-10-25)** — a "This computer"
  place named after the machine: the system's root folder, **read-only and as the user** (what the
  user can already read; folders they can't read show a lock; writing stays limited to the home
  folder and the user's mounts), and every disk and partition, mounted or not, with size and free
  space; a click mounts it through the existing `disk-mount` action (internal disks ask for an
  administrator, removable ones don't), plus unmount and eject. **No "as root" mode** (decided: it
  would be generic root file access from the interface); "Open in Terminal here" for `sudo` work.
  *Implemented (not yet checked on screen):* the place, read-only `lord.fs` outside the writable
  roots (`fsops_readable`, without `/proc`, `/sys`, `/dev`, `/run`), the lock, unmounted disks that
  mount on a click, ⏏ to eject or unmount. *Left:* "Open in Terminal here" would need a folder
  argument for `lord.term.create()`, one of the approved launcher exceptions, so it waits for a
  decision; size and free space per disk in the sidebar.
- [ ] **Files: network shares (own release after 0.8.5, approved 2026-10-25)** — SMB and NFS shares
  found with mDNS or typed ("Connect to server…", `smb://host/share`), mounted by the kernel through
  one new closed admin action, `share-mount` (host, share and user validated; the password on
  standard input, never in argv; fixed options `nosuid,nodev` and the user's uid; only under
  `/media/<user>/`), the password in the keyring, the mount's traffic as its own firewall app.

- [ ] **Flatpak and Flathub (future, undecided; not planned for any release)** — a Flathub section in the Store. On
  2026-10-27 the maintainer decided not to implement it for now: the priority is the system's stability, and Flatpak
  would add complexity (bubblewrap, OSTree, portals, seven new exceptions to `AGENTS.md`) and future problems. The
  design review is kept as documentation (`FLATPAK.md`); nothing is built until the maintainer decides whether to
  do it at all.
  2026-10-27) and waits for approval. Today LordOS neither uses nor integrates
  Flatpak, and its rules keep isolation as LordOS's own code (no bubblewrap/flatpak for `.lpk`). What
  has to be solved so Flatpak apps follow LordOS's security model:
  - **Identity and the firewall:** `flatpak run` moves the app into its own transient
    `app-flatpak-<id>-*.scope` under `user@`, where `lord-netperm` treats it as an escape and applies
    the per-user union, not that app's rules. Flatpak apps must start through LordOS's launcher in
    LordOS's app scope (or a root-owned `lordos-flatpak-<id>` scope) so Monitor, the firewall and the
    device restrictions see them as themselves; device-restricted apps have no session bus, which
    `flatpak run` needs today.
  - **The launcher:** their `.desktop` files live in `/var/lib/flatpak/exports/share/applications`
    and `~/.local/share/flatpak/exports/share/applications`; adding a launch path is an `AGENTS.md`
    exception that needs its own review.
  - **Permissions:** each app declares its own (often the whole home, X11, every device) and asks
    through xdg-desktop-portal, which LordOS doesn't run; they would need to be shown and limited in
    Settings → Privacy, and a LordOS portal backend would route file, screen, camera and microphone
    requests to LordOS's own prompts.
  - **Installing:** per user (`--user`, no root), so no new admin action; the Flathub remote pinned by
    its key, and its signed appstream data (not the website's API) as the Store's catalog, through a
    network path that needs approval like `lord-fetch`.

- [ ] **Steam in the Store's Commercial section (needs multiarch)** — Debian 13's `steam-installer`
  (contrib) depends on `steam-libs-i386`: 72 i386 packages with `--no-install-recommends`, after
  `dpkg --add-architecture i386` (checked in a `debian:trixie` container, 2026-10-27). LordOS's package
  engine is native-architecture only today (`binary-<native>` and `binary-all` indexes, foreign
  packages skipped, `:i386` qualifiers dropped). Needs: a closed admin action for the one fixed value
  (`pkg-foreign-arch i386`), an index and status keyed by (name, arch), the Multi-Arch rules in the
  resolver (same arch by default; `foreign`, `allowed` with `:any`, explicit `:i386`), co-installable
  `Multi-Arch: same` packages and dpkg called with `name:arch`.

- [ ] **LordBench (0.8.6)** — `BENCHMARK.md`. Workloads in the sandboxed `lord-bench` helper
  (CPU and LordOS's own work, memory, GPU, compositor, video, storage, sustained load); peak
  performance and desktop impact measured apart; frame times against the refresh rate actually in
  use; stops on the hardware's trip points and throttling; every result records its conditions;
  three verdicts (performance against several reference machines, compatibility, stability), no
  single score; results versioned.
  *In development (2026-10-27):* the `lord-bench` helper, CPU, memory, GPU, storage and sustained workloads,
  the thermal, throttle and battery guards, the boot history and System Information → Performance are built
  (`BENCHMARK.md`, "Built so far"), and so are the video workloads (software decoding, the remote desktop's
  encoders), the desktop-impact mode (the compositor's frame times alone and under a low-priority load), the N-boot
  test, the pointer-to-screen latency, `lordos-sysreport bench` and the Pi 5 reference. *(2026-10-28)* Also built:
  LordOS's own interface work as workloads (layout and paint of a fixed 1920×1080 scene with the engine's real
  code, one core and with the paint threads, and shaping fixed paragraphs in five scripts, with the fonts built
  into the helper; VM 116: 37.0 and 39.9 fps, 619 and 3,595 runs/s), and the helper-sandbox self-test inside
  `lord-bench` (six tries that must fail, checked by the Stability verdict and `lordos-sysreport bench`). Left:
  a Full run of the Pi 5 with the interface workloads for its reference
  (two Quick runs measured them on 2026-10-29), and the rest of what it found on the Pi 5 (the desktop fell to 24 fps under load;
  the compositor's CPU weight and its real-time priority fix it on VM 116; on the Pi 5 with its monitor they take it
  to 29–30 fps; the frame-cost work cuts a drag frame's build under load to 12.9 ms and keeps 60 fps alone, but
  under load the frames still miss the refresh, at about 31 fps, now on the GPU's side (its time per frame 1.5 → 14–16
  ms under load, no throttling); the frame pipeline and GPU drag, measured on 2026-10-29, gave too little and are off
  by default: `BENCHMARK.md` "The desktop under load" and "The drag frame's cost"). The firmware throttling
  detection was checked on the Pi 5 (the under-voltage alarm present, reading 0). Hardware video decoding and the x86 PC
  reference moved to 0.8.7 (2026-10-29, "Release 0.8.7 (planned)").
- [ ] **Community hardware reports (0.8.7)** — `BENCHMARK.md`. Share on GitHub with the
  `hardware-report` template (the user submits it, LordOS sends nothing), schema validation and
  labels in a workflow, and a tested-hardware catalogue on lordos.org that the Compatibility panel
  reads as a signed snapshot.

- [x] **Remote desktop over WebRTC (0.8.4)** — see `REMOTE.md`, "Video (WebRTC)". Until 0.8.3 the
  browser got PNG/JPEG tiles over a TLS WebSocket, and a connection switched the session to CPU
  composition. Now:
  - [x] **Media:** H.264 (constrained baseline) or VP8 video and Opus audio as WebRTC media tracks
    (RTP/SRTP), keyboard, mouse, clipboard and a lossless refresh of still areas (sharp text) over two
    data channels. **Stack:** Debian 13 doesn't package libdatachannel (nor libjuice), so it is our
    own small peer in C (`remote/rtc.c`, `remote/rtcwire.c`) on what Debian does ship: OpenSSL's
    DTLS (already in `lord-sessiond`), libsrtp2 and usrsctp.
  - [x] **WebRTC only (0.8.4):** the image path (PNG/JPEG tiles over the WebSocket) and its «images
    only» switch are removed, not kept as a fallback. Instead: ICE over TCP on the remote desktop
    port (passive candidates; the root connection child hands the socket to the bridge its check
    names), TURN over UDP, TCP or TLS (pinned fingerprint optional), ICE restarts on the same DTLS
    association, a new peer and a new WebSocket with backoff without signing in again, and a page
    that explains each failure (no route, DTLS, lost, offer, media, server) with what to do, or the
    requirements for a browser without WebRTC or H.264/VP8 (`REMOTE.md`, "Reconnecting" and "When it
    can't connect").
  - [x] **Rendering once:** the GPU keeps composing while someone watches (the composed frame is read
    back once; only protected windows on screen, or screen recording, still need the CPU path); the
    same frame feeds the encoder (`lord-media` MK_STREAM: VA-API when the card encodes H.264, libx264
    otherwise; frames only when something changed, and fewer when the encoder is behind).
  - [x] **Per session:** one encoder per session and codec, shared by every viewer (only the
    encryption is per connection); two users means two sessions, two encoders, each as its user.
  - [x] **ICE:** ICE-lite with a built-in STUN server per connection (LAN works with no outside
    server), plus optional external STUN and TURN in Settings → Sharing (off by default). The TURN
    password is kept in `/etc/lordos/remote/turn.key` (root 0600) rather than a keyring, because
    `lord-sessiond` needs it before any user signs in and for every user; it never leaves
    `lord-sessiond`'s bridge. Signaling only over the existing authenticated WebSocket; DTLS-SRTP runs
    in the bridge that already dropped root, and the browser's DTLS certificate must match the
    fingerprint of the offer that came over that WebSocket. Protected windows stay black.
  - [x] Measured against 0.8.3's numbers, and again after the image path was removed (`REMOTE.md`,
    "Measurements (0.8.4, video)": the same CPU while the video plays; video over TCP where UDP is
    blocked).
  - [ ] later: VA-API encoding tested on real hardware (only the software path ran, on a VM without
    a video encoder); a GPU's concurrent-encode limit (today a session falls back to software only
    when the encoder can't be opened); sessions without a monitor composing on the GPU off-screen;
    stereo sound; bandwidth estimation from transport-wide feedback (today: receiver reports, NACK
    and REMB); an external security review of the new network surface.

- [ ] **Two-step sign-in (TOTP) for the remote desktop** — proposal (0.8.3 security review,
  `REMOTE.md`): an optional one-time code from an authenticator app after the password, enabled
  per user in Settings → Users (secret stored by `lord-sessiond` in a root-only file, QR shown once,
  recovery codes), checked in the same constant-time path, and counted by the same lockout. Off by
  default; required for "Any address".

- [ ] **Immutable edition (A/B system images)** — 0.8.2 published the design study and an
  experimental, VM-verified prototype (`IMMUTABLE.md`, `tools/immutable/`): read-only `/usr` with
  dm-verity in two slots, one boot image per version, automatic fallback, `/etc` and `/home` kept.
  Still to do, in phases: the full desktop as an image with an "Immutable (experimental)" installer
  option and Store updates (phase 1), Secure Boot and the TPM (phase 2), the Raspberry Pi 5 with
  `tryboot` (phase 3), Debian packages as system extensions and delta updates (phase 4), `/etc` and
  execution hardening (phase 5). The classic edition stays as it is.

- [x] **Xwayland** (X11 app compatibility) — 0.8.3, see `WAYLAND.md` ("X11 apps (Xwayland)") and
  `PRIVACY.md` ("X11 apps"):
  - [x] `Xwayland` on demand (rootless), started by the first connection to the session's X socket
    and stopped 10 s after the last X app; our own X window manager in C (map, stacking, focus,
    move and resize, titles, `WM_CLASS` → `.desktop`, override-redirect menus and tooltips,
    fullscreen, minimize, close, our window frame and cursor);
  - [x] clipboard and primary selection bridged both ways (and `zwp_primary_selection_v1` for
    Wayland apps); drag and drop X11 ↔ Wayland: not yet (LordOS has no Wayland drag and drop yet);
  - [x] HiDPI policy (scale 1, scaled by the compositor) and positions across displays; not tested on
    a HiDPI display;
  - [x] the same per-app identity as Wayland apps (scope, firewall, Dock grouping); restricted apps
    get no X display;
  - [x] X11 apps never see Wayland apps, protected windows or keys typed into them; documented in
    `PRIVACY.md`;
  - [x] tests with real X11 apps (XTerm, xeyes, a Swing app, Neverball), CPU and GPU paths.
  - [ ] later: a separate or "untrusted" X display for restricted apps; X11 ↔ Wayland drag and drop;
    sharp X11 apps on HiDPI displays.
- [x] **Turn the "Lord Classic" theme into a second full appearance** (0.8.3; Settings → Appearance → Desktop style, see `DESIGN.md`, "Lord Classic"), for people coming from the
  most common desktop layout: a centered taskbar with a start menu, pinned and running apps, a
  system tray with quick settings and a notification panel, snap layouts for windows, rounded
  corners and translucent materials. Until 0.8.3 `themes/classic` was only a template (taskbar and
  start menu); it became a complete, polished theme with the same name:
  - **same apps, different skin:** no app is rewritten. Native apps keep every feature; the theme
    supplies the window frame, title bar buttons on the right, menus, controls, typography scale and
    spacing through the SDK's theme tokens and components (`configureWindows`, shared controls), so
    every app adapts automatically;
  - shell pieces of its own: taskbar, start menu with search and pinned apps, tray, quick settings,
    notification panel, window snapping (halves, quarters), task view;
  - switchable at runtime, in light and dark, RTL, and with the same accent colors;
  - **our own design and assets only:** inspired by the familiar layout, but with LordOS icons,
    fonts (Inter / Plus Jakarta Sans), wallpapers and names. No copied logos, icons, wallpapers,
    fonts, sounds or product names from any company, and no third-party "clone" projects' assets
    (they often bundle copyrighted material). See `docs/LORDOS-BRAND.md` and the no-brand rule;
  - before/after screenshots of every app in both themes, like the polish pass.
  - done in 0.8.3: shape tokens for the shared controls, the frame, taskbar, start menu with
    search, tray, quick settings, notification panel, snapping (halves, thirds, quarters, by drag,
    layouts and Super+arrows), task view and the live switch; screenshots of every app in both
    themes, light and dark, in `docs/classic/`. Left for later: snap groups (a pair of snapped
    windows that move together), window thumbnails over the taskbar buttons, virtual desktops in
    the task view, and a typography scale token (apps still set their own font sizes).
- [ ] **Mail: sign in with Google and Microsoft (OAuth2)**, so nobody needs an app password. The
  pieces are prepared and disabled today (`sdk/mail.js` `OAUTH_PROVIDERS`, `lord-maild` answers
  `oauth-disabled`; see `MAIL.md`, "OAuth"):
  - the authorization-code flow with PKCE in the system browser and a loopback redirect served by
    `lord-server`; the refresh token stored in the system keyring (Passwords), never in the account
    file; `AUTHENTICATE XOAUTH2` in `imap.c` and `smtp.c`; token refresh and revoke on account removal;
  - Google calendars and contacts (CalDAV/CardDAV) with the same sign-in;
  - **needs iPERFEX to register LordOS with each provider** (only the project owners can do this):
    a Microsoft Entra app registration (public client, personal and work accounts,
    `IMAP.AccessAsUser.All`, `SMTP.Send`, `offline_access`; publisher verification optional) and a
    Google Cloud OAuth client of type "Desktop app" with the `https://mail.google.com/` scope and the
    calendar/contacts scopes, pointing to the privacy policy and terms on lordos.org. Gmail's mail
    scope is restricted: until Google verifies the app, only up to 100 listed test users can sign in;
  - Microsoft first: it is retiring password sign-in for IMAP, so OAuth will become the only way in.

- [x] **Browsers with their own bar (0.8.4)** — lord-server honors the decoration mode an app asks
  for (xdg-decoration and KDE's server-decoration), also while the window is open; Chromium and
  Firefox draw their own bar by default, with LordOS's window buttons (traffic lights from LordOS's
  GTK theme in Chromium), and the window menu, resize edges and snap layouts keep working. See
  `docs/WAYLAND.md`, "Who draws the frame".
- [x] **Settings → Appearance: "Browsers: tabs in the title bar" (0.8.4)** — one switch, on by
  default, that sets the browsers' own preference in the user's profiles instead of each browser's
  settings page (`docs/WAYLAND.md`, "The switch in Settings"):
  - **Chromium:** `browser.custom_chrome_frame` (and `extensions.theme.system_theme` = 1, the GTK
    theme, unless the profile uses a theme extension) in each profile's `Preferences`, only while
    Chromium is closed (if it is open, applied as soon as it quits), through the narrow
    `lord.browserFrame` call (`hw/brframe.c`): those keys and nothing else, validated JSON before
    and after, a backup and an atomic rename;
  - **Firefox:** a marked `user.js` line `user_pref("browser.tabs.inTitlebar", 1|0)` in each
    profile under `~/.config/mozilla/firefox` (where Firefox 153 puts them) and `~/.mozilla/firefox`,
    removed again once Firefox has read it; a line the user wrote is kept;
  - existing Chromium profiles that don't follow the switch (upgrades from 0.8.3) are counted on the
    page, with a «Change them» button;
  - the page says the change applies the next time each browser opens.
- [ ] **Traffic lights in Firefox too** (optional, separate). Firefox draws its window buttons
  itself (not with the GTK theme), so it would need a per-profile `chrome/userChrome.css` plus
  `toolkit.legacyUserProfileCustomizations.stylesheets`; weigh it against touching browser profiles
  before doing it.
- [x] **LordOS Imager (0.8.4)** — our own card and stick writer with a first-run experience for the
  Raspberry Pi 5, in LordOS (`apps/imager`) and as a Tauri app for other computers
  (`desktop/imager`, one UI for both): signed releases downloaded and cached with resume, only
  removable drives, verified writes (read back and compared), the first-run file
  (`lordos-firstrun.txt`: computer name, language, user, Wi-Fi with the derived key, SSH, remote
  desktop, appearance, cooling) applied once by `lordos-firstrun` on the board. See `docs/IMAGER.md`.
  - [ ] boot a card it wrote on a real Raspberry Pi 5 (applied with `--root` to the 0.8.3 image so far);
  - [ ] real sticks and cards on each desktop platform (only virtual ones so far), the Mac's
    authorized-open prompt and udisks2/polkit on a desktop Linux;
  - [ ] sign the Mac and Windows downloads (an identity to be chosen) and publish them with the release;
  - [ ] Wi-Fi scan in the desktop app; WPA3-only networks (today the board needs the password typed there).

## After the final ISO

- [ ] **AI assistants through MCP, with permissions** (study first, then build). Let an AI
  assistant, local or remote, use LordOS through the Model Context Protocol only as far as the
  user allows, following the same rules as the rest of the distribution. Today there is no
  automation API at all (see `PAPER.md`, "Automation").
  - **Study before code:** a design document (`docs/MCP.md`) reviewed like the security review:
    threat model (prompt injection from web pages, mail and files the assistant reads; a
    compromised or over-eager assistant; data leaving the machine), transports (local socket
    only by default; remote access only through an explicit, authenticated opt-in), which MCP
    features to support (tools first; resources read-only; no sampling), and how it fits the
    privilege split (`lord-server` never runs as root and never runs arbitrary commands).
  - **Off by default.** A switch in Settings → Privacy, and every assistant appears as its own
    client with a name and an identity, like an app.
  - **Ask, then grant, like camera and microphone:** each assistant requests the capabilities it
    wants and the user approves them in a LordOS prompt, per assistant, with "once", "while
    connected" or "always", revocable at any time in Settings. Nothing is granted silently, and
    a request text written by the assistant is never shown as if LordOS wrote it.
  - **Closed, validated tools only:** a fixed catalogue of actions with typed arguments (open an
    app or a file the user picked, read the system or network state, create a calendar event,
    draft a mail, change a named setting, take a screenshot of one window), each mapped to an
    existing SDK or broker operation with the same checks as the UI. **No shell, no arbitrary
    command execution, no generic file access**, never root; anything destructive or
    outward-facing (sending mail, deleting, installing, changing security settings) needs a
    confirmation from the user every time, and some actions are never exposed (passwords and
    the keyring, the firewall's own rules, the signing keys).
  - **Visible and auditable:** a menu bar indicator while an assistant is connected or acting,
    every call logged and shown in the Monitor (who, what, when, result), a "stop all" button,
    rate limits, and the per-app firewall and sandbox still applying to whatever the assistant
    triggers.
  - **Tests:** permission prompts and revocation, injection attempts through web pages and
    mail, denied and confirmation-required paths, and an external security review of the
    design and the code before it ships.

- [ ] **Lumo, the local assistant (after 0.8.7)** — an assistant that
  runs on the machine, talks with the user and uses LordOS only through the closed tools of the
  MCP item above. Input: the maintainer's design notes and the results of a prototype on another system (Qwen3 1.7B and
  4B with llama.cpp; on the test PC the CPU beat the RX 580 offload by 2.5×, so nothing is assumed
  per vendor). Adapted to LordOS's rules:
  - **Inference in a sandboxed helper** (`lord-ai`, like `lord-media`): the only binary that links
    llama.cpp; no network, the model file as an fd, a private channel to `lord-server` (no TCP port,
    no HTTP API another app could reach). The Python prototype is reference only and doesn't ship.
  - **Models only with consent:** a signed catalogue on lordos.org (hash and licence per model),
    downloaded through the package engine's checks, loaded on demand and unloaded when idle.
  - **AutoTune** on `server/src/sysreport/` and LordBench: the CPU always measured as the baseline,
    thread counts, partial and full GPU offload where the backend allows it (Vulkan in `lord-ai`
    only), prompt and generation speed, memory; the choice and its reason stored per machine,
    re-measured after a kernel, driver or engine change; the desktop keeps its share of the CPU.
  - **Permission levels mapped to what exists:** reading (level 0) through the SDK's read-only
    calls; user actions (level 1) behind a LordOS prompt like camera and microphone; administrator
    actions (level 2) only through existing closed admin actions, with the password typed in a
    system dialog checked by the root side, never in the chat or the model's context. The level is
    LordOS's policy, never something the model declares.
  - **First demo:** "Lumo, open the calculator" opens it through `lord.apps.launch(id)` with an id
    LordOS offered, and the result shown is the launcher's real result.
  - **For the benchmark:** explain a report in plain words, compare it with published community
    reports, suggest what to try (a driver, a setting) and draft the issue text the user sends.
  - **Tests:** `lumo-agent-bench`, 50–100 scenarios (right tool, valid arguments, unknown tools,
    ambiguous requests, consent, admin without rights, real errors, prompt injection through files,
    mail and pages), scored separately from speed; a fast model that misuses tools isn't chosen.
  - **Off by default**, visible while working (menu bar indicator, every call in the Monitor), and
    reviewed like the MCP study before it ships.

- [ ] **Virtual machines**: a **free desktop virtualization app**, with the polish and
  way of working of the best commercial ones (not Proxmox's, which only contributes the pieces underneath: KVM, QEMU and drivers).
  A generic name and our own logo, icon and colors:
  - **Control Center** with the list of VMs and their live thumbnail, state, start, pause,
    suspend and shut down;
  - **Installation Assistant**: cards for downloading popular guest systems,
    Ubuntu, Debian, Fedora, Android, «Install from an ISO file», «Migrate from another PC»;
  - **VM window** with its bar (Actions, Devices: USB, CD, folders, network, sound) and
    the **Window, Full Screen, Seamless and Picture in Picture** modes;
  - **Configuration** in tabs: General, Options (startup and shutdown,
    optimization, sharing, applications, Seamless), Hardware (CPU and memory, graphics,
    mouse and keyboard, network, sound, USB, disks, printers, TPM, boot), Security, Backup;
  - **profiles** when creating the VM (Productivity, Software Development, Testing, Design, Games)
    that tune CPU, memory and options automatically;
  - the guest's apps in the app launcher and the Dock, opening LordOS files with a guest
    app, clipboard and dragging files between the two;
  - the **KVM** hypervisor (the kernel's); **QEMU** as the device engine, with **swtpm** (TPM)
    and **OVMF** (UEFI and secure boot) for guests that require them;
  - the same drivers Proxmox uses: **virtio-win** (disk, network, memory, 2D display
    `viogpudo`, virtiofs for non-Linux guests) signed by Red Hat, the **QEMU guest agent**
    (`qemu-ga`) and the **SPICE agent** (clipboard, resolution, file dragging); they are
    downloaded from Fedora's official source the first time and their signature is verified; Linux
    guests already ship them in the kernel;
  - our own assistant (automatic download and installation, mounts virtio-win during the guest
    install and installs the tools at the end), snapshots, cloning, host USB passthrough to the
    VM, shared folders (virtiofs), resolution that follows the window;
  - the VM's display inside our compositor via dmabuf (no QEMU windows);
  - **Seamless** mode: apps of a Linux guest as standalone LordOS windows (forwarding
    Wayland); other guests with our own agent, later;
  - accelerated 3D in Linux guests (virgl/venus); on other guests only 2D for now (there is no good free
    3D driver);
  - another architecture (arm64 on a PC, amd64 on the Pi) through TCG emulation: it works, but 5–20×
    slower;
  - later, a lightweight engine of our own only for Linux guests (like crosvm/Firecracker).
- [ ] **Android:** our own container with LineageOS and apps from our Store.
- [ ] **Firewall, ask mode** (an alert on the first connection attempt, like a per-app network monitor).
- [ ] **Accessibility:** screen reader, zoom, high contrast.
- [ ] Full **demo video** (installation + tour, English voice-over with Spanish
  subtitles).
