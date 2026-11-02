# LordOS documentation

LordOS is a free GNU/Linux distribution based on Debian 13, with its own desktop environment:
a display server and Wayland compositor written in C, its own interface engine, and native apps.
This folder documents how it works, how to build it and how to verify it. Start with the
[paper](PAPER.md) for the big picture, or [ARCHITECTURE.md](ARCHITECTURE.md) for the technical map.

Website: [lordos.org](https://lordos.org) · Brand kit: [LORDOS-BRAND.md](LORDOS-BRAND.md)

## Start here

| Document | What it covers |
|---|---|
| [SHORTCUTS.md](SHORTCUTS.md) | The shortcut dispatcher, the keys kept from apps, ❖ as Ctrl and the app switcher |
| [PAPER.md](PAPER.md) | The project paper: vision, architecture, security model, performance, roadmap |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Components, processes, dependencies and platforms |
| [ROADMAP.md](ROADMAP.md) | Milestones, what is done and what comes next |
| [DESIGN.md](DESIGN.md) | Design guide for LordOS apps |
| [LORDOS-BRAND.md](LORDOS-BRAND.md) | Logo, colours, typography and the Lumo mascot |

## Install and build

| Document | What it covers |
|---|---|
| [ISO.md](ISO.md) | The live and installable ISO for 64-bit PCs: building, booting, final test results |
| [RASPBERRY.md](RASPBERRY.md) | The Raspberry Pi 5 image, our kernel with Landlock, boot switch files |
| [IMAGER.md](IMAGER.md) | LordOS Imager: writing LordOS to cards and sticks, in LordOS and on other computers, and the first-run file |
| [INSTALL-DEBIAN.md](INSTALL-DEBIAN.md) | Installing LordOS on an existing Debian 13 with one command |
| [VM-SETUP.md](VM-SETUP.md) | The development VM, step by step |
| [UPDATES.md](UPDATES.md) | Updating an installed system in place from the signed APT repository, and migrations |
| [TROUBLESHOOTING.md](TROUBLESHOOTING.md) | Known problems and how to fix them (for example a modified system component) |
| [RELEASING.md](RELEASING.md) | How a release is made: changelog, tag, build, sign, SBOM and the GitHub release notes |
| [../CHANGELOG.md](../CHANGELOG.md) | What changed in each version (also the body of each GitHub release) |

## Desktop and display

| Document | What it covers |
|---|---|
| [WAYLAND.md](WAYLAND.md) | Linux apps (GTK, Qt, browsers) on our Wayland compositor |
| [WINDOWS.md](WINDOWS.md) | Moving and resizing windows, the Window menu, cursors |
| [DISPLAYS.md](DISPLAYS.md) | Several displays and HiDPI |
| [GPU.md](GPU.md) | GPU composition, dmabuf and the CPU path |
| [DOCK.md](DOCK.md) | Dock and taskbar |
| [DESKTOPS.md](DESKTOPS.md) | Desktops and Overview |
| [WALLPAPERS.md](WALLPAPERS.md) | Moving wallpapers on the desktop and the login screen |
| [ON-SCREEN-KEYBOARD.md](ON-SCREEN-KEYBOARD.md) | The on-screen keyboard, for a computer with no physical keyboard |
| [THEMES-GUIDE.md](THEMES-GUIDE.md) | How to build a LordOS appearance (theme): anatomy, tokens, frames, building, verified code, testing |
| [LAYERS.md](LAYERS.md) | The desktop's stacking order: what is drawn and clicked above what |
| [SPOTLIGHT.md](SPOTLIGHT.md) | Search and the advanced clipboard |
| [TRACKPAD.md](TRACKPAD.md) | Touchpads, gestures and touchscreens |
| [SESSIONS.md](SESSIONS.md) | Login screen, sessions, user switching, boot splash hand-off |
| [REMOTE.md](REMOTE.md) | Remote desktop on the LAN (off by default, port 5673), SSH and root over SSH (0.8.5, advanced) |
| [LANGUAGES.md](LANGUAGES.md) | Languages, regions, complex scripts, input methods, error messages |

## Privacy and security

| Document | What it covers |
|---|---|
| [PRIVACY.md](PRIVACY.md) | App isolation, permissions, and what is isolated today |
| [FIREWALL.md](FIREWALL.md) | Per-app firewall, restrictions, domain rules and blocklists (0.8.5) |
| [SIGNING.md](SIGNING.md) | Package signing, trust levels and release signing |
| [IMMUTABLE.md](IMMUTABLE.md) | Design study: an immutable edition with A/B system images (0.8.2, experimental prototype) |
| [SECURITY-REVIEW-0.8.0.md](SECURITY-REVIEW-0.8.0.md) | Security review before 0.8.0-beta: every finding and its status |
| [PASSWORDS.md](PASSWORDS.md) | Passwords and the system keyring |
| [COMPLIANCE.md](COMPLIANCE.md) | Licence compliance, the SBOM, and the standards our processes align with |
| [../SECURITY.md](../SECURITY.md) | How to report a vulnerability |
| [../CONTRIBUTING.md](../CONTRIBUTING.md), [../CODE_OF_CONDUCT.md](../CODE_OF_CONDUCT.md) | How to contribute, and the code of conduct |

## Apps

| Document | What it covers |
|---|---|
| [NATIVE-APPS.md](NATIVE-APPS.md) | Installable native apps (`.lpk`) |
| [PACKAGES.md](PACKAGES.md) | Debian packages and the Store: our package engine, HTTPS downloads (0.8.5), the AI and Commercial catalogs |
| [WASM.md](WASM.md) | WebAssembly apps |
| [FILES.md](FILES.md) | Files |
| [TERMINAL.md](TERMINAL.md) | Terminal |
| [EDITOR.md](EDITOR.md) | Text Editor |
| [NOTES.md](NOTES.md) | Notes |
| [DOCUMENTS.md](DOCUMENTS.md) | PDFs, archives, fonts and emoji |
| [PRINTING.md](PRINTING.md) | Printing and scanning |
| [MEDIA.md](MEDIA.md) | Photos, Player, Screenshots, Camera and Recorder |
| [VIDEO.md](VIDEO.md) | Video in the browser without hardware decoding (H.264 instead of VP9/AV1) |
| [MAIL.md](MAIL.md) | Mail, Calendar, Contacts and Reminders |
| [MONITOR.md](MONITOR.md) | Monitor: usage, processes, network map and connections, firewall, disk health, cooling |
| [SYSTEM.md](SYSTEM.md) | Disks, Backups, Logs, Clock and Help |
| [CLEANER.md](CLEANER.md) | Cleaner: storage and maintenance |
| [CLEANER-ARCHITECTURE.md](CLEANER-ARCHITECTURE.md) | Cleaner's design |
| [CLEANER-SECURITY.md](CLEANER-SECURITY.md) | Cleaner's safety model |
| [CLEANER-TESTS.md](CLEANER-TESTS.md) | Cleaner's tests and measurements |
| [CLEANER-ROADMAP.md](CLEANER-ROADMAP.md) | The state of each Cleaner feature |
| [VIRTUALIZATION.md](VIRTUALIZATION.md) | Machines, the virtualization app: plan (MVP from 0.8.8-1) |
| [GO-LINK.md](GO-LINK.md) | go-link Desk on LordOS and the public screen-sharing API: design, API from 0.8.7.1, status in the document |
| [FLATPAK.md](FLATPAK.md) | Flatpak and Flathub: a design review, not planned for any release |

## System and hardware

| Document | What it covers |
|---|---|
| [SETTINGS.md](SETTINGS.md) | Settings hardware pages: displays, Bluetooth, energy, cooling, notifications, date and time, default apps |
| [NETWORK.md](NETWORK.md) | Networking and VPNs |
| [SOUND.md](SOUND.md) | Sound (PipeWire) |
| [FANS.md](FANS.md) | Fan control on the Raspberry Pi 5 and PCs |
| [BENCHMARK.md](BENCHMARK.md) | System Information (0.8.5), LordBench and hardware reports: design and status |

## Measurements and validation

| Document | What it covers |
|---|---|
| [PAPER-VALIDATION.md](PAPER-VALIDATION.md) | Each claim of the paper, how it is checked and its state |
| [RESULTS.md](RESULTS.md) | Versioned results: measurements per version and machine, with the commands that produced them |

## Images and assets

| Folder | What it holds |
|---|---|
| [screenshots/](screenshots/) | Curated English screenshots, and `gallery.json` for the website gallery |
| [brand/](brand/) | Logo, icons, mascot and colours (see [brand/README.md](brand/README.md)) |
| [boot/](boot/) | Boot menu and splash screenshots |
| [iso-final/](iso-final/) | Screenshots of the final ISO test |
| [polish/](polish/) | Before and after screenshots of the visual polish pass |
| [packs/](packs/) | Sample language pack and script test text |
| [classic/](classic/) | Screenshots of the Lord Classic theme and the Lord Flow theme, light and dark |
| [imager/](imager/) | Screenshots of LordOS Imager (native app and desktop app) |
| [qa/](qa/) | Screenshots of the app-by-app QA pass (see [VM-SETUP.md](VM-SETUP.md)) |

## Research notes

| Document | What it covers |
|---|---|
| [HANDWIRED_NOTES.md](HANDWIRED_NOTES.md) | Research notes on a related UI project |
