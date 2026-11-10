# Desktop parity: what KDE Plasma and GNOME have that LordOS still lacks

The plan to close the gaps found on 2026-11-10 by reading LordOS's code against KDE Plasma 6 (KWin, Qt) and GNOME 47/48
(Mutter, GTK4). It is ordered by how much a person notices the gap every day, and each phase lists what needs the
maintainer's approval first (AGENTS.md: a new root action, helper, library or permission is reviewed before it is written).

How each item is done: a branch or worktree per item, unit tests where the logic is pure, an end-to-end check on a VM on
PROXMOX2 (`tools/vm/e2e/`, the `desk-kde` VM to compare against KDE), Claude Security on the change, then the maintainer's
machine. CHANGELOG, the paper and the site follow CLAUDE.md as usual.

Status: ⬜ not started · 🟨 in progress · ✅ done · ⛔ waiting for approval.

## Phase 0 — close 0.8.8.8

| Item | Status |
|---|---|
| Terminal: a trigger fires again for a line printed again while the old one is still on screen | ⬜ |
| Terminal: a port forward whose fixed local port is taken says so (two tabs of one profile) | ⬜ |
| Terminal: stale ssh mux sockets no longer count against the 32 looked at (`server/src/term/sshmux.c`) | ⬜ |
| Paper and site for 0.8.8.8; signed amd64 and arm64 packages; apt staging, then stable; GitHub pre-release; close #64 | ⬜ |

## Phase 0b — Databases over an SSH tunnel (top priority, the maintainer's daily use)

| Item | Why | Size | Status |
|---|---|---|---|
| Databases: connect through an SSH tunnel, as `docs/DATABASES.md` phase 2 designs it: lord-server starts `/usr/bin/ssh -W <db host>:<db port>` (fixed path, every argument from the validated favorite, `--` before the destination, the key a file under the home and not readable by others, `BatchMode`, the host key confirmed by the person on first use like a certificate) and hands its stdin/stdout to lord-sql as the server's stream; the tunnel ends with the connection | most favorites imported from Sequel Ace are SSH ones and can't connect today | M | 🟨 approved 2026-11-10 and done (sshtun.c, tested end to end through a real sshd to MariaDB on a VM); the maintainer's own favorite still to check |

## Phase 1 — quick wins, nothing new to approve

| Item | Why | Size | Status |
|---|---|---|---|
| Text fields: a right-click menu (Undo, Redo, Cut, Copy, Paste, Delete, Select All) and Ctrl/⌘+Z, ⇧Z; a right click no longer moves the caret or drops the selection | the first thing tried in any field | S | 🟨 |
| Text fields: triple click selects the line (`runtime/events.js` counts clicks) | every desktop has it | S | 🟨 |
| Drop from Files onto Notes (insert), Photos, Player, PDF Viewer (open) | the Editor and Terminal already take drops | S | ⬜ |
| `xdg-activation-v1`: a link opened from the Terminal or a notification's click raises the browser already open; activation only with a valid token | the wrong window stays in front | S | 🟨 |
| `xdg-foreign-v2` and `xdg-dialog-v1`: portal dialogs attached to their window | dialogs float loose | S | ✅ |

## Phase 2 — the compositor's big three

| Item | Why | Size | Status |
|---|---|---|---|
| Wayland drag and drop (`wl_data_device.start_drag`, offers, actions, the drag icon) and the bridge from Files to Linux apps (`text/uri-list`) | drop a file on Chrome or Firefox, drag tabs out, drag attachments | L | 🟨 built, to check on the VM: between Wayland apps, Files → Linux app and Linux app → Files (`docs/WAYLAND.md`, "Drag and drop"); XDND for X11 apps still to do |
| XDND bridge for Xwayland apps (`server/src/wl/xwm.c`) | the same for X apps (Chrome under Xwayland, older apps) | M | ⬜ |
| `text-input-v3` complete: preedit and commit from LordOS's input method (`server/src/ime.c`, `wl_textinput.c`) | Japanese, Chinese and Korean inside Linux apps | M | 🟨 built, to check on the VM (`build/textinput-client type 30`, then GNOME Text Editor) |
| Screen sharing for apps: the xdg-desktop-portal ScreenCast interface in `server/src/ui/portal.c`, screen or window chosen in LordOS's own dialog with the consent and indicator of `remote/share.c`, frames to a PipeWire stream | share a screen or a window in Meet, Zoom, Teams, Discord, OBS | L | ⛔ the PipeWire stream: a new sandboxed helper linking libpipewire (recommended) or LordOS's own PipeWire protocol |
| `keyboard-shortcuts-inhibit-v1`: an app may take ❖ and Alt+Tab while focused, after the person allows it once | VMs, remote desktops, games | S | ⛔ a new permission kind in `sec/perms.c` |

## Phase 3 — desktop services Linux apps expect

| Item | Why | Size | Status |
|---|---|---|---|
| MPRIS client: Linux players (Spotify, browsers, mpv) in Now Playing, and the media keys reach them | music and video in the browser without global controls | S/M | 🟨 built, to check on the VM (`server/src/hw/mpris.c`, `sdk/mprismodel.js`) |
| StatusNotifierItem host (`org.kde.StatusNotifierWatcher`, dbusmenu) shown with LordOS's status items | Nextcloud, Telegram, Discord, Steam, VPNs lose their icon | M | 🟨 built, to check on the VM (`server/src/ui/sni.c`, `sdk/trayitems.jsx`, docs/WAYLAND.md "Tray icons") |
| XDG autostart (`/etc/xdg/autostart`, `~/.config/autostart`) through a closed launcher, listed in Settings | sync clients, VPNs and messengers start with the session | M | ⛔ a new launch path |
| Portals OpenURI and Inhibit; `org.freedesktop.ScreenSaver` Inhibit tied to `wl_idle_inhibited()` | links from Flatpaks; video players that keep the screen on | S | 🟨 built, to check on the VM |
| Global shortcuts portal | push-to-talk, OBS | M | ⬜ |
| Drop onto isolated apps (Mail attaches, Calendar and Contacts import): the dropped files granted to that app as the file chooser grants them | Mail can't take a dragged file | M | ⛔ a new broker message and grant |
| Files puts `text/uri-list` and `x-special/gnome-copied-files` on the clipboard when copying files | copy in Files, paste in a GTK or Qt app | S | 🟨 built, to check on the VM (both ways for Wayland apps; files an X11 app copies arrive as text) |

## Phase 4 — the SDK's keyboard and widgets

| Item | Why | Size | Status |
|---|---|---|---|
| A keyboard focus model in the runtime: `focusable`, Tab and ⇧Tab between controls, a focus ring shown only when the keyboard is used, Space and Enter activate | everything is mouse-only today except text fields | L | ⬜ |
| Menus with arrows, Enter, type-ahead, delayed submenus, opening left in RTL | keyboard use | M | ⬜ |
| SDK components with keyboard support built in: List/Table with Shift/Ctrl range selection and type-to-find, Dropdown, Checkbox, Radio, Tabs, Popover, Tooltip, ProgressBar | each app has its own today (three `Popover`s, two tables…) | M | ⬜ |
| Sliders and steppers with arrows and the wheel; ScrollView with Page Up/Down, Home, End and horizontal scrolling | keyboard use | S | ⬜ |
| Destructive dialogs start on Cancel; a dialog takes keys only when in front | Enter emptied the Trash | S | ⬜ |

## Phase 5 — text, language and accessibility

| Item | Why | Size | Status |
|---|---|---|---|
| Caret and selection drawn from the shaped text (bidi rectangles) and movement by grapheme clusters (`server/src/ui/bidi.c`, `textlayout.c`) | Arabic or Hebrew mixed with numbers; emoji split by the arrows | M | ⬜ |
| Native primary selection (select copies, middle click pastes) over the compositor's | long-time Linux users | S | 🟨 built, to check on the VM |
| Spell checking: hunspell in a sandboxed helper fed words over an fd; red underline and suggestions in the field's menu | Mail and Notes | M | ⛔ a new helper |
| Screen reader: an accessibility tree from `server/src/ui` nodes exported over AT-SPI2; high contrast tokens; a font scale | blind and low-vision people can't use LordOS | L | ⛔ the D-Bus export |
| Show password (eye) and a Caps Lock warning in password fields | login, Wi-Fi, the vault | S | ⬜ |

## Phase 6 — hardware that needs its own test bench

| Item | Why | Size | Status |
|---|---|---|---|
| `linux-drm-syncobj-v1` (explicit sync) | Nvidia's proprietary driver flickers | M | ⬜ needs an Nvidia card |
| `tablet-v2` from evdev (pressure, tilt, pen buttons) without libinput | Krita, GIMP, Xournal++ | M | ⬜ needs a tablet |
| `tearing-control-v1` and VRR (`VRR_ENABLED`) | games with FreeSync/G-Sync | M | ⬜ needs a VRR monitor |
| Xwayland apps sharp on HiDPI (the X apps scale themselves, as in KWin) | old X apps look blurry | M | ⬜ |
| `security-context-v1` | Flatpak sandboxes | S | ⬜ |
| Suspend tested on real hardware and offered in the session menu | laptops | M | ⬜ needs a laptop |
| Color management and HDR (ICC per output first) | HDR monitors | L | ⬜ needs HDR hardware |

## Left out on purpose

`wlr-layer-shell` (third-party panels), `data-control` (outside clipboard managers; GNOME doesn't offer it either),
`foreign-toplevel` (third-party docks) and `session-lock` (LordOS locks the screen itself): each would let an outside
program draw over or read the session, against AGENTS.md's model.
