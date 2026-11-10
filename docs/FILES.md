# Files

`apps/files`. A browser for the home folder and the user's mounted disks (and, read-only, the rest
of the computer), with the useful parts
of a classic desktop file manager: tabs, four views, Trash, Quick Preview, Get Info, copy/cut/paste, drag and
drop, color tags and search. Disk operations belong to the server (`server/src/fsops.c`,
`lord.fs` in `js_sys.c`); the app only requests them.

## Screenshots (VM 116, real lord-server)

| | |
|---|---|
| ![icons](files-icons.png) | ![list](files-list.png) |
| Icons with real image thumbnails | List: columns, alternating rows and folders that expand in place |
| ![columns](files-columns.png) | ![gallery](files-gallery.png) |
| Columns (Miller) with the text preview | Gallery: large preview and the strip below |
| ![quick look](files-quick-preview.png) | ![info](files-get-info.png) |
| Quick Preview (Space), 1 of 5; the arrows go to the next one | Get Info (❖I): size, location, dates, permissions and «Open with» |
| ![menu](files-context-menu.png) | ![open with](files-open-with.png) |
| Context menu of a file, with tags | «Open with»: the Text Editor or a Linux app that declares the type (FeatherPad) |
| ![linux app](files-open-in-linux-app.png) | ![move](files-move.png) |
| `notes.txt` opened in FeatherPad (Qt) from «Open with» | Dragging onto a folder (it gets highlighted); with tabs |
| ![trash](files-drag.png) | ![restore](files-trash.png) |
| Dragging to the Trash in the sidebar | In the Trash: Restore or Delete permanently |
| ![empty](files-empty-trash.png) | ![tags](files-tags.png) |
| Emptying the Trash asks for confirmation | Red tag: the dot on the icon and the filter in the sidebar |
| ![search](files-search.png) | ![recents](files-recents.png) |
| Search in «Documents» or in Home, with each result's folder | Recents, by date |
| ![group](files-group-by.png) | ![applications](files-applications.png) |
| Grouped by kind | Applications (LordOS, packages and Linux) |
| ![light](files-icons-light.png) | ![light list](files-list-light.png) |
| Light theme | List in the light theme |
| ![rtl](files-rtl.png) | ![background](files-background-menu.png) |
| In Arabic: the sidebar moves to the right | Right click on empty space: New Folder, Paste, View, Sort, Group, hidden files |

## What's there

- **Toolbar:** back and forward (history per tab), the name of the place, the four views, Group
  and Sort (⇅), actions (⋯) and search.
- **Tabs** inside the window and **new windows** (`wm.open('files', { new: true, args:
  { path } })`). A file can be dropped onto a folder's tab.
- **Sidebar:**
  - Favorites: Recents, Applications and the user's folders (xdg-user-dirs);
  - Locations: Home, **This Computer** (0.8.5: `/`, read-only, named after the machine; folders
    the user can't open, and `/proc`, `/sys`, `/dev` and `/run`, show a lock; opening one says
    "Permission denied", or that Files doesn't show the system's internal folders), the disks and USB sticks mounted in `/media/$USER` or
    `/run/media/$USER` (checked every 4 s; ⏏ on hover ejects a removable disk with `disk-eject` and
    unmounts another disk's partition with `disk-unmount`), every other disk with a file system that isn't mounted, a CD or DVD
    in an optical drive among them (dimmed; a click mounts it with `disk-mount` and opens it, a disc read-only; an internal one asks for an
    administrator, as in Disks; the disk the system runs from is left out) and the Trash;
  - Color tags (they filter).

  A disc put in an optical drive during the session (a VirtualBox Guest Additions image, an install
  disc) brings a notice whose Open mounts it read-only and opens it in a new Files window
  (`sdk/diskwatch.js`).

  Folders and the Trash accept dragged files.
- **Views** (❖1 to ❖4):
  - icons (cards, with `flexWrap`; thumbnails of PNG/JPEG up to 24 MB);
  - list (Name, Modified, Size and Kind, sorted by clicking the header);
  - columns (the last three folders and the preview);
  - gallery.

  In searches, Recents and Tags, the Kind column becomes the folder.
- **Group** by kind (folders first), date (today, 7 days, 30 days, this year, older) or size, and
  **sort** by name (natural: «2» before «10»), kind, date or size. Folders go first. Recents and
  the Trash go by date.
- **Selection:** click, ❖/Ctrl+click, ⇧+click (range), ❖A, and typing the start of a name.
- **Rename in place** (Enter): the name is selected and what you type replaces it
  (`TextInput selectAll`, new in the SDK). Tags follow the file.
- **Copy, cut and paste files:** Files keeps its own list (a cut moves only between Files windows) and
  also puts the paths on the session clipboard as Linux file managers do (`text/uri-list`,
  `x-special/gnome-copied-files`, the paths as text), so they paste in a Linux file manager, a chat or an
  editor. Files copied in a Linux file manager paste in Files as a copy, only this machine's files
  (`docs/WAYLAND.md`, "Clipboard"; `apps/files/clipfiles.js`).
  Copying into the same place duplicates with a free name («notas 2.txt»).
- **Copy and move in the background** (server threads): with progress in the status bar and a
  button to cancel. If something fails, no half-made copies are left.
- **Drag and drop:** moves to a folder, to the sidebar, to a tab or to the Trash; with ⌥ it
  copies. While dragging you see the icon, the count and a «+» when copying. If a file with that
  name already exists, a free one is chosen (nothing is overwritten).
- **Trash** (freedesktop.org, the same as GNOME and KDE):
  - things from the home folder go to `~/.local/share/Trash`, and things from a mounted disk to
    `$mount/.Trash-$uid`, with their `.trashinfo` (Path, DeletionDate);
  - restoring goes to the original location; if another file is already there, it isn't
    overwritten and you are told;
  - emptying asks for confirmation;
  - permanent deletion exists only inside the Trash.
- **Quick Preview** (Space): the image, the text (the first 64 KB, in monospace), the first page of
  a PDF, a font sample, the contents of an archive, a video frame (if `media.poster` is
  available) or the file's info card. The arrows go to the next one and Space or Esc close it.
  See `DOCUMENTS.md`.
- **Archives** (M9): double-click opens them like a folder; «Extract here» and «Compress» in the
  context menu, through the sandboxed helper `lord-doc`, never leaving the destination folder
  (`DOCUMENTS.md`).
- **Get Info** (❖I): size (for folders it is computed with a 0.9 s cap), location, link,
  creation, modification and access dates, permissions and owner, tags and «Open with».
- **Open with:** the Text Editor (text and code), the Store (`.lpk`, `.deb`) and the Linux apps
  whose `.desktop` declares the type in `MimeType`. The latter are launched with
  `lord.apps.launchWith(id, path)` (see below).
- **Recents:** the files in the home folder modified in the last 30 days. It is a bounded walk: 5
  levels, 60,000 entries, no hidden files and no `node_modules`.
- **Applications:** those from `useLauncherApps()` (LordOS, packages and Linux); double-click
  opens them.
- **Tags:** red, orange, yellow, green, blue, purple and gray, stored in
  `~/.local/share/lordos/tags.json` (path → colors).
- **Path bar** at the bottom, clickable on each segment, and **status bar** («N items, X
  available», «N of M selected» or a copy's progress).
- **App menus** in the theme's bar: File, Edit, View and Go (in Lord Classic, since 0.8.5, in a strip
  under the window's title bar). In a folder the user can't write (This Computer), the menus and the
  context menu leave out or disable the actions that would change it.

## Settings (0.8.7)

**Files → Settings** (❖,), kept in `settings.json` under `files` (`apps/files/prefs.js`). General: what a new window shows, and a double click opening a folder in a new tab. Tags: a name for each color and which show in the sidebar. Sidebar: each item. Advanced: show extensions (the name shown only; renaming still works on the real one), warn before changing an extension or emptying the Trash, remove items over 30 days old from the Trash when Files opens (there is no background job), folders first, and where a search looks. What the reference has and LordOS doesn't (desktop icons, cloud sync) isn't offered.

**List columns** are data in `apps/files/columns.js`, tested without a screen. Extra columns read `lord.fs.stat`, once per file until something changes. Date Changed is the inode's. Comments live in `~/.local/share/lordos/comments.json`, like tags. There is no Version column, and no "date added", because Linux keeps neither.

## Connect to Server (0.8.7)

`apps/files/connect.js` reads an address strictly: shell metacharacters, a leading `-` and `..` are refused, and each part goes on as data. smb and nfs go to the sign-in sheet and `share-mount`, which takes private IPv4 addresses only, so a name works only if Network already saw it.

**http and https (0.8.8.6, #59)** can be a web page or a WebDAV folder. A path that names WebDAV (`/dav`, `/webdav`,
`/remote.php/…`) opens as a folder by itself; any other path makes the sheet ask, *Open as a folder* or *Open in the
browser*. As a folder the address becomes `webdav://` (http) or `davs://` (https), and that is what Recents and a kept
server remember (`looksDav`, `davOf` in `connect.js`).

**The address field** (and every one-line `TextInput`) has a caret that ←, →, Home, End and a click move; typing,
Backspace, Delete and a paste work where it is (`sdk/textfieldmodel.js`, `server/tests/textfield_model_test.mjs`).

**sftp, ssh, ftp, ftps and WebDAV are folders in Files.** `lan/rconnect.c` starts `rclone mount` as the session user, in scope `app-lordos-lordos-rclone.scope` (firewall app `desktop:lordos-rclone`), on `~/.local/share/lordos/remote/<kind> on <host>`, and the folder is a volume in the sidebar with its eject button (`fusermount3 -u`). The configuration, with the password obscured by `rclone obscure -` over a pipe, is a memfd read through `/proc/self/fd/3`: there is no file to protect, nothing is on a command line or in the environment, and it ends with the process. It stays open in rclone for as long as the folder is mounted: emptying it once mounted hung the mount on the first reconnection (tried on VM 116 against the test server, with a control that kept it), so the obscured password stays readable by the session user's own processes while mounted. Sftp's `shell_type` and hash commands are set to none: nothing runs on the server, and rclone has nothing to save to that config. `ssh-keyscan`, `openssl` and `rclone obscure` run through `rconnect_run`: in the same scope, with `sandbox_exec_child` (Landlock: the system's folders read-only, nothing written, no TCP unless they need the server), killed at 15 s, so a stalled server can't hold the job or the password. On a kernel with no Landlock only the scope and the deadline hold (Pi 4 on the archive kernel). `rconnect_config` refuses control characters in any value, so nothing can start another key (`lan_test`). rclone, fuse3, openssh-client and openssl are dependencies of `lordos`; rclone has no sandbox beyond its scope.

**Who the server is.** SFTP: `ssh-keyscan` reads the key first; a key seen for the first time shows its fingerprint and is kept in `~/.config/lordos/remote_known_hosts` (0600) only if the person accepts, a different one is refused outright, and rclone checks that file on the real connection (`known_hosts_file`, always set). ftps and https WebDAV: TLS is verified with `openssl s_client`, hostname included, and `no_check_certificate` is never set. A certificate the system doesn't trust shows its fingerprint; accepted, that certificate alone is kept per host and port under `~/.config/lordos/remote_certs` (0600) and given to rclone as `SSL_CERT_FILE`, with `SSL_CERT_DIR` pointing nowhere, so that certificate alone is trusted for that connection. An accepted certificate is checked before the system's verdict: a different one is refused even if a public authority signed it. Every SSH key that goes in the file is shown first, and an answer counts only for the fingerprint on screen. The certificate must still name the host (a subject alternative name for it): rclone's TLS has no way to accept a name mismatch except switching all checks off, which is never done, so such a server says so and doesn't connect. Plain ftp and http WebDAV say on the sheet that the password travels readable. ssh:// also offers Terminal with the line typed and **not run**: Terminal checks it against a fixed pattern, so an app that opens Terminal this way can't run anything.

A connected server can be pinned: the secondary click on its row in Locations offers Pin to Locations. It stays there, dim, while it isn't connected, and a click connects again. The address is kept in Files' settings and never the password, which comes from the keyring if it was remembered and is asked for if not.

Favorites takes folders too: drag one and an Add to Favorites row appears under the list, or use the folder's secondary click. A connected server's row in Locations can be dragged there as well; it is only a shortcut, and a volume is never moved by that drag. A favorite on a server that isn't connected shows dim and a click connects again, opening at the server's top folder. A dragged folder dropped on an existing row still moves, as before.

Tested against a real Windows 11 Pro server with a local account: the sign-in, the share list without its administrative shares, mounting and reading a file.

## Remote desktop (0.8.7)

Remote Desk (`apps/remote-desk`, `sdk/remotedesk.jsx`), one app for RDP and VNC with a switch between them, draws the remote screen with LordOS's own engine, as a media node. There is no client window, X11, Xwayland or toolkit. `lord.rdesk` (`ui/js_rdesk.c`) starts `lord-rdesk` (`rdesk/`) in `app-lordos-lordos-rdesk.scope`; it resolves and connects, then applies the helper sandbox (no new sockets, no exec, read-only `/etc/ssl` and `/usr/lib` for RDP), and paints BGRA into a memfd lord-server shares. The picture's size is sealed (`F_SEAL_SHRINK`, `F_SEAL_GROW`), lord-server takes at most 256 messages per poll and never blocks on the helper, and the scope has lord-server's memory limits (256 MB high, 512 MB max). VNC is our own RFB 3.8 client (`rdesk/rfb.c`: None and VNC auth, refusing a server that offers only None when a password was given, password wiped after the handshake, Raw, CopyRect, Hextile, resize; `tests/rdesk_test.c` with a fuzz run). VNC auth's DES is required by the protocol and is weak: the traffic is not encrypted. RDP allows only TLS and NLA (`FreeRDP_RdpSecurity` off, proved by `tests/rdesk_rdp_test.c`: no credentials go to a server that picks RDP's own security). A certificate that differs from the accepted one gets its own warning with both fingerprints (`sdk/remotecert.js`). RDP is libfreerdp's core with its software GDI, and its client library only for shared folders, handed the connected socket; `libfreerdp3-3`, `libfreerdp-client3-3` and `libwinpr3-3` are dependencies of `lordos`. An unknown or changed certificate shows its fingerprint and the person decides; a trusted one is kept in settings with the address. Passwords reach the helper in one message and go to the keyring only if asked.

### Remote Desk's window, credentials and pictures (0.8.8.4)

**The window.** A sidebar with Favorites and Devices (it hides from the toolbar or View → Show Sidebar), and a toolbar
with **+** (Add RDP Computer…, Add VNC Computer…), Pictures / List, sort (by name, by last connection, groups shown or
not), Show (all, RDP, VNC, favorites) and a search field. The computers are cards with the picture of their last
session, a pill with RDP or VNC, the name and the user name, in sections by group ("Saved PCs" is the default group,
each section folds). On hover a card gets an accent outline, a **⋯** menu (Connect, Edit…, Duplicate, Export…, add or
remove from Favorites, Remove) and a star. Double-click or Enter connects; each connection opens its own window
(`args.connect`), so the list stays. The File, View and Connections menus repeat these, plus Import… / Export… of `.rdp`
files (address, user name, screen mode, size, color depth, sound, administrative session; never a password) and, in a
session, Disconnect, Send Ctrl+Alt+Del, Full Screen and Update the Picture.

**The computer's sheet.** One **Host** field ("Name or IP address, e.g. 192.168.1.20 or pc.local", with an optional
`:port`), a **Credentials** pop-up (Ask When Required, the saved credentials, Add Credentials…) and the tabs General
(friendly name, group, reconnect if the connection is dropped, administrative session, swap mouse buttons), Display
(resolution: automatic, the presets or the custom ones; the remote desktop's scale; color quality; full screen),
Devices & Audio (play sound on this computer, on the remote computer or never; what isn't supported yet says so) and
Folders. VNC shows General and Display only. There is no Domain field: a domain belongs to a credential, typed as
`DOMAIN\user` or in the credential's optional Domain field. `user@example.com` stays whole (a full sign-in name).

**Credentials.** Saved in Remote Desk Settings → Credentials (❖,): a user name, an optional domain and display name in
settings, the password only in the keyring (`lan.remember('rdesk', <id>)`, service `rdesk://<id>/`). A password typed
at sign-in goes to the keyring only after the computer accepted it. VNC uses only the password of a credential.
Settings also has General (pictures on or off, delete them, new computers reconnect, forget trusted certificates),
Resolutions (custom sizes) and Groups.

**Migration.** `sdk/remotedeskmodel.js` (`migrate`, tested by `server/tests/remotedesk_test.mjs`) rewrites what older
versions kept once (`remoteDesks.v = 2`): one id per computer, the user and domain moved to a credential, and the
mistake the old form invited fixed: a host that isn't an address (no dots, not an IP) with a Domain that is one (an IP,
or a dotted name starting with the host's name) becomes the friendly name, and the Domain the address. A password
remembered under the old address (`legacy`) is still found and moves to the credential on first use.

**"Couldn't find it" and "doesn't answer".** `lord-rdesk` reports a name that doesn't resolve (`getaddrinfo` failed) as
`RS_NOTFOUND` ("Couldn't find “name” on the network. Check the name or use its IP address.") and an address that
refuses or times out as `RS_UNREACHABLE`. An RDP session lost without the server saying why (`freerdp_error_info` 0)
is `RS_DROPPED`: with "Reconnect if the connection is dropped" the window tries again up to three times with the
password it still holds; a sign-out or a disconnect from the remote side ends the session.

**Pictures.** 3 s after a session connects, and again at 12 s (after a sign-in), lord-server scales the shared picture
down to 640 px wide and writes `~/.local/share/lordos/remote-desk/thumbs/<id>.png` (`rdesk/thumb.c`: the folder 0700,
the file 0600, written beside it and renamed, no links followed; `tests/rdesk_thumb_test.c`). The device keeps when
(`shot`), and the card loads `<file>#<shot>`: `ui/image.c` reads a `#<digits>` suffix as a version and loads the same
cached image again when it changes. Removing a computer, changing its address or Settings → Delete All Pictures
deletes the file.

**Sound.** "On this computer" adds FreeRDP's `rdpsnd` channel with a device of our own (`rdesk/rdesksnd.c`,
`sys:lordos`): it offers 16-bit PCM at 44.1 or 48 kHz and writes 48 kHz stereo into the audio ring that lord-server
makes for the session (`media/maudio.c`, the same shared-memory ring as the media player's). The ring travels to the
helper right after the job, before its sandbox; the helper never gets the PipeWire socket. lord-server owns the
stream ("Remote Desk", with the computer's address, in Settings → Sound) and serves it from the session's media node.
"On the remote computer" asks the server to keep the sound there. The microphone, clipboard, printers, cameras, smart
cards and USB devices are not available yet. VNC has no sound.

## Views and the sidebar (0.8.7)

Each folder keeps its view and sort in `settings.files.folderViews` (400 at most, oldest out first), so they survive closing and a restart; Network, Recents and the other places keep theirs too. A folder with none takes Settings → General's default view. The sidebar's width is dragged from its edge, between 150 and 360 px, and kept (`apps/files/viewprefs.js`).

### Zoom (0.8.8.4)

The icon and list views have a **zoom** ([#33](https://github.com/lordbasex/LordOS/issues/33)): the icon (or thumbnail) and the name grow or shrink together, in seven steps from −2 to +4, with 0 the usual size. Icons go from 32 to 160 px, with cards from 76 to 196 px wide and names from 10 to 16 px; list rows go from 20 to 48 px, with icons from 12 to 40 px (thumbnails from 32 px up, as in the icon view) and names from 11 to 19 px, the other columns one size smaller. Ways to change it: the slider between a small and a large square at the end of the status bar (the squares step; a double click on the slider goes back to 0; narrow windows keep only the squares), View → Zoom In / Zoom Out / Actual Size, Ctrl and + (or =), Ctrl and −, Ctrl and 0, and Ctrl with the mouse wheel or a two-finger touchpad scroll over the files. The columns and gallery views don't zoom, and the control isn't shown there.

The step is kept **per place and per view** with the place's view and sort (`folderViews[path].zoom`, e.g. `{ "icons": 2, "list": -1 }`), so a picture folder can keep big icons and a small list while every other folder stays at the usual size; a step of 0 isn't stored. It lives in `settings.json`, like the rest of the view settings: nothing is written into the folder itself (no hidden file), so read-only folders, disks of other systems and servers keep it too and their contents don't change. The sizes and the stored value are checked by `server/tests/files_model_test.mjs` (`iconMetrics`, `listMetrics`, `zoomWith`).

## Disks' file systems (0.8.7)

NTFS, exFAT, FAT, HFS+ and APFS come as one Store item, **Disk Formats** (`lordos-disk-formats`, Commercial group, label "Commercial format"), installed and removed as one after a page lists each driver with its license and source. All of them are free software at no cost. The package adds a marker, `/usr/lib/lordos/disk-formats.enabled`, that `disk-mount` checks, and depends on ntfs-3g, exfatprogs and dosfstools, plus apfs-dkms and kernel headers on amd64 (323 MB, not on the Pi, whose kernel is LordOS's own).

APFS is a switch on that page and not a dependency: its driver is `apfs-dkms`, built for the running kernel with compilers and headers (about 500 MB, measured on VM 125 and the VMware guest), on Debian's amd64 kernel only. The item alone is about 2 MB. Without the switch an APFS volume doesn't mount; Files, Disks and the notice then lead to the same page, which offers the driver again once the item is installed. On the Pi, without the driver, the error says so and sends nowhere. `dpkg -i` on the package alone stops at missing dependencies, as with any package: the Store and `apt` resolve them.

**What gating FAT means:** only that Files and Disks won't mount a FAT volume on the desktop without the item. The kernel's `vfat` stays, so the boot partition mounts through fstab; the installer and live USB call `mount` themselves and keep `dosfstools` from their own package lists; the Imager writes images and never mounts through this action. A phone or camera card formatted FAT is the visible effect.

Tested on VM 125 from the sidebar with the drivers present: NTFS (ntfs3), exFAT and FAT32 read and write. HFS+ in a journaled volume, the usual kind, mounts read-only. APFS mounts read-only through the open driver, a volume made on another system and left empty.

## Network (0.8.7)

**Files → Network** shows the computers on the local network by itself, as a place like any folder: one entry per computer, in whichever view is chosen. Opening one connects with the best way it offers (SMB, NFS, SFTP, WebDAV, FTP, then a remote screen); with more than one, its row unfolds them, and a right click has Connect as and Copy address. RDP and VNC open their apps. It
stays off until the place is opened and leaves when it closes.

**What it lists (0.8.8.6, #59, `apps/files/netplaces.js`).** First, under *Connected*, the places this person already
uses: the shares and remote folders mounted now (open, open in a tab, keep in Locations, disconnect), the servers kept
in Locations that aren't connected (dim; a click connects, so nothing reaches the network when Files starts) and the
Cloud accounts (open, or their page in Settings → Cloud). Then, under *On this network*, the machines that offer a way
in: shared folders, exports, AFP, an SFTP, WebDAV or FTP folder, or a remote screen. A machine that answered with
nothing to open (a Wi-Fi repeater, a printer, a phone) is left out, and the status line says how many; the page's
right-click menu has *Show every device* (`settings.files.netAllDevices`). These rows aren't the folders themselves
(`place:` paths), so trash, rename, drag and copy can't reach a mount point from the Network page.

**State of the feature:** the maintainer approved the `lan` connector on 2026-10-31, so inside a session
`lan_socket` gets its sockets from it (`sec/netconn.h`, `NC_LAN`), counted and decided as the firewall app
`lordos:lan` in `lordos-net-lan.scope` (`FIREWALL.md`, "Files → Network and the connectors"). The tests below come from
before that, outside a session (see "What was tested"). **SMB with a password:** listing was run against a throwaway test server on the loopback; see "Not tested" for a real mount and desktop systems.

**How a pass works** (`server/src/lan/lan.c`, on a thread of its own):

1. It takes the interfaces with a real device (`/sys/class/net/<if>/device`: no container bridges, veths or
   tunnels), up, with a private IPv4 address (`lan_private`: 10/8, 172.16/12, 192.168/16, 169.254/16). IPv6 is not
   searched. With no such interface nothing is sent.
2. **mDNS/DNS-SD:** three legacy-unicast questions per link for the service types in `lan/lanservices.h`. Datagrams
   stay in lord-server's socket and are handed to the helper; an unconnected UDP socket in the helper could reach
   any address.
3. **TCP probe of ports 445 and 2049:** first the neighbours the kernel knows and what mDNS named, then the rest of
   the subnet, at most its /24 (`lan_sweep_range`), at most 512 addresses in a pass, at most 30 new connections a
   second (`LAN_RATE`), 24 at once, 600 ms each. A /24 takes about 20 seconds. Only addresses on a local subnet
   (`on_link`) are ever connected to, whatever an mDNS answer claims.
4. **Each server:** where 445 answered, one SMB2 exchange (below). Where 2049 answered, port 111 (portmapper) for
   mountd's port and then the export list from that same host.
5. **The ones that turned out to be there:** every neighbour that answered ARP during the sweep goes on the list,
   with port 22 tried and a reverse mDNS question for its name. This is how a machine with no shares appears.

The page opening keeps what a pass under 5 minutes old found; *Search again* needs 30 seconds between passes
(`LAN_REST_S`, `LAN_KEEP_S`). Leaving the page cancels the pass and ends the helper.

**`lord-lan`** (`lan/lanhelper.c`, `lan/lanparse.c`, protocol in `lan/lanproto.h`) reads everything that comes back.
Same rules as `lord-fetch`: fixed path next to lord-server (owned by root or the user, not writable by others), no
arguments, `LANG` and `LORDOS_SESSIOND` only, the channel on fd 3, `sandbox_apply_helper` before anything is read (no socket, connect,
bind or exec, no files, 64 MB). It links libc only. It gets datagrams and sockets lord-server already connected and
never chooses where to connect: the one port it names, mountd's, is connected by lord-server on that same host.
Its answers are parsed again with `sec/json.c` and every text goes through `lan_clean_text`. `lord.lan`
(`ui/js_lan.c`) has `start`, `stop` and `poll`: JS names no address, port or socket, and it doesn't exist for
isolated apps.

**What SMB gives without a password:** the negotiate reply, then an anonymous NTLMSSP session: the challenge carries
the computer's and domain's names, and if the server lets an anonymous session in, a `NetrShareEnum` over the
`srvsvc` pipe lists its disk shares (printers, `IPC$` and names ending in `$` are left out). SMB 2.0 to 3.0.2 are
offered; a server that only speaks SMB 1 doesn't answer and has no shares here, on purpose. A server that refuses
anonymous sessions (most desktop systems by default) shows as "lists its folders only after signing in".

**Signing in shows the folders.** *Connect* asks who connects, not a share's name: the name of a folder is something
the server knows and the person may not. `lord.lan.listShares` (`lan_list_start` in `lan/lan.c`) runs on a thread and a
`lord-lan` of its own, only to an address on a local subnet (`on_link`), through the same `lan` connector and port 445,
and hands the helper the connected socket and one request, `LQ_SMB_AUTH`, with the name and password. The helper
(`lan/smbauth.c`) answers the NTLMv2 challenge, signs what follows as SMB 2.0.2 and 2.1 do, with HMAC-SHA256, reads
`NetrShareEnum` over `srvsvc` as the anonymous path does, and wipes the password; it is in that request and nowhere
else, never on a command line or in a log. Only dialects 2.0.2 and 2.1 are offered here, because 3.x signing needs
AES-CMAC and key derivation for no gain in a list: a server that refuses both can't list, and the sheet says so and
asks for the name. A session the server marks guest or anonymous after a password counts as refused. MD4, MD5 and RC4
are in `smbauth.c` because NTLM requires them; they are used nowhere else. The sign-in carries the target name `cifs/<address>`
and a MIC over the three NTLM messages, and the server must sign its final answer with the session key: one that can't is
refused (`smb_verify`, checked against Samba on the test server). The answers after that are shown, not trusted: their signatures
aren't checked and they go through `lan_clean_text`, and the mount is the kernel's. A share chosen goes to the
same `share-mount`. *Remember the password* keeps it for the server (service `smb://<address>/`).

**Connecting** runs the closed `share-mount` action (`lan/share_admin.c`, table in `SYSTEM.md`). The kernel's own
`cifs` and `nfs` clients do the work through the new mount API, one `fsconfig()` per option: no option string, no
program, no shell. **No package is needed** beyond the kernel's modules: `cifs-utils` and `nfs-common` are not used
(Debian's and the Pi kernel both ship the modules; the client side was tested on Debian's). Addresses are IPv4 and private, never names, so nothing
resolves as root. SMB tries the password given, or the guest; NFS tries 4.2, 4.1, 4.0 and 3 (`nolock`, without
`rpc.statd` its locks stay local), `sec=sys`, and has no sign-in: the server decides by address. The folder is
`/media/<user>/<share> on <address>`, `nosuid,nodev`; on SMB the files are the session user's, 0600 and 0700.

The password goes from the sheet to the action with `{ quiet: true }` and from there to one `fsconfig()`, and is
wiped; it was not in `/proc/mounts`, the session's log, the journal or any command line. *Remember the password*
saves it in the keyring (owner `files`, service `smb://<address>/` or, for a share known beforehand, `smb://<address>/<share>`) and nowhere else; with a locked or
missing keyring the share still connects and the page says it wasn't remembered.

**A server that goes away.** `lord.fs` runs on the interface's thread, and a call on a network mount blocks in
the kernel. Measured on VM 125 with the test server's link down: browsing an SMB mount held the thread for 30
seconds, and an NFSv4.2 `soft` mount held `ls` for over 100 seconds; the Dock's look at the Trash alone was enough to hang a
session that has this mount. So `fsops_share_down` runs in `resolve_readable`, `resolve_in_home` and
`resolve_target`, before `realpath`: a thread of its own asks the server (`statvfs`), the caller waits at most 800 ms,
and while that question hangs the share counts as down at once. With the same dead link the thread was never
blocked again (24 samples, one a second, all in `poll`), and the page shows "The computer sharing this does not
answer". `fs_volumes` no longer calls `statvfs` on shares, and the Trash leaves out a share that doesn't answer.
Left open: a server that dies inside the 3 seconds an answer is trusted, and paths other than `lord.fs`'s (Quick
Preview's helpers, thumbnails) opened right after it, can still block for the kernel's time.

**A server that is only slow (0.8.8.7).** A share that answers, but in seconds, passes `fsops_share_down`, and a Cloud
account (rclone, under `~/Cloud`) isn't one of those shares at all: deleting a file in an iCloud folder froze the
desktop until rclone finished, because Files listed the folder again (and asked its `stat`, and one `stat` per row for
the list view's columns) on the interface's thread while the FUSE `unlink` still held the folder's lock in the kernel,
and going back to the parent waited for rclone's listing. Files now asks every folder for which `fs.isRemote(path)`
says so (`lord.fs.mounts()`: the mount table's `fuse.*`, `cifs`, `smb3`, `nfs*`, `9p`… mounts) through
`lord.fs.listAsync / statAsync / trashAsync / freeAsync / mkdirAsync / renameAsync`, which run on
`server/src/fsasync.c`'s workers (at most 6; 256 requests queued, `fs.busy` past that) with the same `fsops.c`
resolution and checks as the synchronous calls, and answer through `__lordFsReply`. `apps/files/remotedirs.js` keeps
each folder's last listing (shown while a new one comes), lists a folder once at a time, and hides items while a delete
or a move to the Trash runs until a new listing says whether they are gone. A move inside such a mount renames on the
copy job's thread. Still on the interface's thread there: the path checks and `lstat`s `lord.fs.copy` makes before starting a job, a
thumbnail of a file already downloaded, Quick Preview's text, and Go to Folder.

**The traffic of a mounted share isn't in the firewall.** The kernel's client opens its own sockets, outside any
app's cgroup, and reconnects from a kernel thread, so it can't be a firewall app as `ROADMAP.md` first said.

**What was tested** (2026-10-31; everything outside a LordOS session, before the connector, as said above):

| What | Where | Seen |
|---|---|---|
| A pass with the real helper in its sandbox | VM 116 and VM 125 on the real LAN | about 19 s for 506 connections on one /24; two of the maintainer's desktop computers (name, `_smb._tcp`, SSH, SFTP; SMB 3.0.2, signing required, anonymous session refused), `lordos-pi` at 172.17.0.24 (name by reverse mDNS, SSH), VM 116, VM 125, and three machines with SSH and no name |
| Share list, export list | test server on VM 125 | Samba's `Public` and `Private`, its export; reached over a veth to a network namespace, not over the LAN |
| `share-mount` / `share-unmount` | VM 125 (kernel 6.12) against that server | guest, password, wrong password (`share.denied`), NFS 4.2, writing as the user, refusals (public address, `..`, a folder that isn't a share) |
| The page and the sheet in a real `lord-server` | VM 125, a private headless session (as root with `--user`, so with the admin helper), built with `LAN_ANY_LINK` so the veth counted | the list, Connect as guest, with a typed password, NFS, ⏏, the keyring's "not remembered" note, a dead server |
| Parsers | `build/lan-test` under ASan/UBSan | every format against crafted, truncated and 40,000 damaged answers |

**Not tested:** a real SMB or NFS server on the LAN (the only ones there refuse anonymous sessions, and the
open test server was kept off the LAN), the keyring when it
is unlocked, arm64, a server that isn't Samba. No pass through the connector inside a session is recorded here yet.

**SMB with a password is not validated.** The password row above is one Samba test server over a veth, with a
development build. It has not worked against a real server on a network, and listing a server's folders with a password
isn't built, so treat signing in to a real SMB server as untested.

## Cloud (0.8.8.5)

Cloud's accounts (docs/CLOUD.md) are a section of the sidebar and their files carry a status. Built from the approved
mocks (Files, FilesMenu, ShareLink, GetInfo, States); the code is `apps/files/cloud.jsx`, the rules `sdk/cloudmodel.js`.

- **Sidebar**: a **Cloud** section between Favorites and Locations, one row per account in the order they were added,
  with the account's color and a small usage ring (used of the quota; none without a quota). An account that needs a
  new sign-in or has an error shows "!" on orange and is dimmed, as is one that is offline or whose keyring is locked.
  A click opens `~/Cloud/<name>`; a right click offers Open in New Tab and Cloud Settings…. Settings → Cloud's
  "Show in Files" switch (`settings.cloudHidden`) leaves an account out. `fuse.rclone` mounts under `~/Cloud` never
  show under Locations: the Cloud section is where they live (Connect to Server's rclone mounts are elsewhere and stay).
- **Status**: asked with `cloud.status(paths)` for the rows of the folder shown (at most 400), again on every pushed
  state (at most twice a second). The list view adds a **Status** column at the end in an account's folders; the icon
  and gallery views put a badge at the icon's corner: in the cloud (cloud outline), coming down (ring with the part
  that arrived), on this computer (check in a circle), kept (filled check), going up (ring), needs attention ("!").
  An answer with the same statuses keeps the one Files has, so the list isn't drawn again twice a second while anything
  moves.
- **Browsing** (issue #71, docs/CLOUD.md 7.3): a folder of an account is listed from rclone's listing, which the cloud
  side keeps current: Files calls `cloud.browse(path)` when it opens the folder and once a minute while it shows it, and
  lists it again when the account's `listed` goes up (the folder was listed again in the background) and on that minute.
  It is not listed on the 4-second volumes tick that local folders get (each listing is a call to rclone per file, and
  one that fell on an expired listing waited for the provider), and the status bar doesn't ask its free space (statfs
  there makes rclone ask the provider; the account's own space is shown).
- **Nothing comes down by itself**: a file only in the cloud (or whose status isn't known yet) gets its type icon, not
  a thumbnail, and the column view's and the gallery's preview don't read it. Opening it, Quick Preview and Get Info's
  folder size are explicit and bring it down. The file dialog shows no thumbnails inside an account either.
- **Context menu** (files and folders of one account): **Keep on This Computer** (with the size still to come down;
  checked when everything chosen is kept, and then a click stops keeping), **Free Up Space** (with the size it frees;
  what is open, kept or waiting to go up stays, and the status bar says how many), then for one item **Copy Link** (the
  existing link, or a new one for 7 days), **Share Link…**, **Open on Website** and, when it is shared, **Stop Sharing**.
- **Share Link…**: the link with **Copy** ("Link copied"), **Link expires** 1 day / 7 days / 30 days / Never (changing
  it makes the link again), **Stop Sharing**, **Open on Website** and the note that people, passwords and editing are
  the provider's. A storage without public links says so and offers only the website.
- **Get Info** of a cloud item: its status (with the progress, the bytes, the speed and the time left while it comes
  down), the account, where it is in it, its size in the cloud and here, when it last synced (Cloud's recent list),
  whether it is shared, a **Keep on this computer** switch and Share Link… / Open on Website. Size and Where of the
  usual card give way to these.
- **Header and status bar**: in an account's folders the toolbar has a pill "N coming down · N going up" for that
  account, and the status bar ends with "… of … used in <account> · … on this computer" instead of the disk's free space.
- **Move to Trash** on items of an account (the menu, ❖⌫, Delete, dragging onto the Trash) asks "Delete from
  <provider>?" and deletes them in the cloud (`cloud.delete`): they go to the provider's own trash where it has one,
  otherwise for good. Nothing goes to LordOS's Trash and no `.Trash-<uid>` folder is made in the cloud, so Restore isn't
  offered for them. Local items chosen with them go to the Trash as usual (docs/CLOUD.md 17.2, 18.1).
- **Offline**: opening a file only in the cloud while its account is offline shows "<name> isn't on this computer",
  with OK and Keep on This Computer.
- **Crumbs**: the path bar (and the file dialog's) starts at the account: "Work › Projects", not "Home › Cloud › Work".
- **File dialog**: a Cloud section with the accounts (not one whose keyring is locked), between Favorites and
  Locations (`cloudPlaces` in `sdk/filepickermodel.js`).

Which providers are verified on LordOS, Add Account's **Not verified yet** part and **Check This Account** (Settings →
Cloud → an account) are in docs/CLOUD.md section 19; Files shows every account the same way, verified or not.

Tests: `server/tests/cloud_model_test.mjs` (in `run_suites.sh`). Screenshots of every screen, light and dark, were taken
on VM 116 with the development stand-in for `lord.cloud` (`sdk/cloudmock.js`, `"cloudDevMock": true` in settings.json).

## Shortcuts

| Shortcut | What it does |
|---|---|
| ❖N / ❖T / ❖W | New window / new tab / close tab |
| Ctrl+Tab, ⇧Ctrl+Tab | Next / previous tab |
| ❖1 ❖2 ❖3 ❖4 | Icons / list / columns / gallery |
| Ctrl + / Ctrl − / Ctrl 0, Ctrl + wheel | Zoom in / out / actual size (icons and list) |
| ❖[ / ❖] / ❖↑ | Back / forward / enclosing folder |
| ❖↓ or double click | Open |
| Enter | Rename |
| ❖⇧N | New folder |
| ❖D | Duplicate |
| ❖C / ❖X / Ctrl+V | Copy / cut / paste files (also with Ctrl) |
| ❖⌫ or Del | Move to Trash (in the Trash: delete permanently, with confirmation) |
| ❖I | Get Info |
| Space | Quick Preview |
| ❖⇧. | Show hidden files |
| ❖A | Select all |
| Arrows, Home, End | Move the selection (in the list, → and ← expand and collapse) |

## API (`lord.fs`, `sdk/fs.js`)

| | |
|---|---|
| `copy(from, to)`, `move(from, to)` | A job on a server thread; returns its id. Recursive, keeps permissions and dates, doesn't overwrite (O_EXCL) and copies links as links. Moving on the same disk is an instant `rename`; across disks, it copies and then deletes the original. |
| `jobs()`, `cancelJob(id)`, `forgetJob(id)` | Progress: `[{ id, src, dst, move, done, failed, error, bytes, total, files }]`. The app polls it every 250 ms while there are jobs. |
| `trash(path)`, `trashList()`, `restore(id)`, `removeFromTrash(id)`, `emptyTrash()` | freedesktop Trash (`id` = path inside `…/Trash/files`). |
| `stat(path)` | `{ name, path, dir, link, target, size, mode, user, group, mtime, atime, ctime, btime, writable }` (statx, without following the link) |
| `du(path, ms)` | `{ size, files, dirs, complete }`; stops after `ms` (max. 3000) |
| `free(path)` | `{ total, free }` of the disk |
| `search(folder, text, { limit, depth })` | By name, case-insensitive and without hidden files; bounded (up to 1000 results, 16 levels and 100,000 entries) |
| `recent(limit, days)` | What was recently modified in the home folder |
| `volumes()` | `[{ path, name, fs, total, free }]` from `/proc/mounts`, only `/media/$USER/…` and `/run/media/$USER/…` |

**Security:**
- Changing things goes through `resolve_target` (`js_sys.c`) and `fsops_allowed`: only the home
  folder and the user's mounts.
- Reading (`list`, `read`, `stat`, `du`, `free`, `search`; 0.8.5, This Computer) goes through
  `resolve_readable` and `fsops_readable`: the whole tree as the session user, so the system's own
  permissions decide what can be opened, except `/proc`, `/sys`, `/dev` and `/run` (this process's
  memory, environment and open files, devices, and the session's sockets; `/run/media/$USER` stays).
  Paths are resolved with `realpath` first, so a link into those is refused too; `list` refuses
  them with `fs.systemFolder` (not `fs.outsideHome`). `stat` outside the
  writable roots answers `writable: false`, and Files then leaves out the actions that change the
  folder (new folder, paste, rename, duplicate, Trash, cut, compress, extract). There is no root
  mode. Isolated `.lpk` apps don't get this: their broker only reaches their own data folder.
  «Open with» and the helpers keep the home folder and the user's mounts.
- Writing, copying to, moving and sending to the Trash reject the protected folders
  (`fsops_protected`: `~/.local/share/applications`, `~/.config/autostart`,
  `~/.local/share/lordos/apps`, `~/.config/lordos/settings.json`, the permission and firewall
  decisions `permisos.json` and `firewall.json`, the blocklists (0.8.5), `backup.json`, the Mail
  accounts and cache, and the Passwords vaults). Copying *from* them is allowed.
- There is no general `remove`.

## «Open with» a Linux app: `lord.apps.launchWith(id, path)`

It is the same approved launcher exception (`server/src/apps.c`, see `WAYLAND.md`), with one
file:
- only apps whose `.desktop` has `MimeType` (those that declare they open files);
- the path is resolved with `realpath` and must be in the home folder or in one of the user's
  mounts;
- it goes as a single argument in the first `%f`/`%F` of its `Exec`, or in `%u`/`%U` as
  `file://…` (encoded);
- if the `Exec` has no file field code, nothing is launched;
- no shell, as always.

## The system file dialog (0.8.8)

When an app asks for a file through the system (Mail attachments, a contact's photo, importing `.ics` or `.vcf`, a
picture for Settings → Wallpaper, Notes' attachments and images, Passwords' files, and since 0.8.8.1 an isolated app's
powerbox) the dialog looks and works like Files, built from Files' own pieces
(`sdk/filepicker.jsx` on `sdk/fileparts.jsx` and `sdk/fileglyphs.jsx`; its rules are in `sdk/filepickermodel.js`):

- **Sidebar:** Home and the user's folders under Favorites, and under Locations the local disks mounted for the user
  in `/media` and `/run/media`. Network disks (`cifs`, `smb3`, `nfs`, `nfs4`, rclone and sshfs mounts, and the
  servers Files connected under `~/.local/share/lordos/remote/`) are left out: one that goes away over the network
  would hold the interface.
- **List and path:** name, date and size, sorted by a click on the column title, with the path's crumbs above. It
  never starts above the home folder or a disk.
- **Keys and mouse:** a click selects, a double click opens a folder or picks a file; Enter does the same for the row
  under the cursor (with none, it is the primary button); Esc cancels; the arrows move. Saving asks before replacing
  a file.
- **List or Thumbnails (0.8.8.3):** two buttons at the end of the toolbar switch the files between the list and
  tiles with small pictures ([#13](https://github.com/lordbasex/LordOS/issues/13),
  [#22](https://github.com/lordbasex/LordOS/issues/22)). A dialog that takes only pictures (Settings → Wallpaper,
  Notes' images, a contact's photo, a Linux app's `image/*` filter) opens in Thumbnails, any other in the list; the
  person's choice is remembered in `settings.json` (`pickerView`, `{ files, images }`, one for each kind of dialog).
  The pictures are lord-media's thumbnails, the same `~/.cache/lordos/thumbs` cache Files and Photos use
  (`useThumb`), asked for only for the tiles in sight and a row either side, so a big folder asks for a screenful and
  the dialog never waits for one: a file shows its type icon until its thumbnail arrives, and for good when it has
  none (folders, documents, SVG). Videos and music show a frame or the cover. In Thumbnails the arrows move by tile
  and by row (reversed right to left) and sorting keeps the list's last choice. In an isolated app's own process
  (Mail, Contacts, Passwords) the switch works but is not remembered: that process has no settings file.
- **Thumbnail size (0.8.8.7, [#58](https://github.com/lordbasex/LordOS/issues/58)):** in Thumbnails the toolbar has
  Files' size control (`ZoomControl` in `sdk/fileparts.jsx`, now shared: a small and a large glyph around a slider; a
  double click on the slider goes back to the usual size; a narrow dialog keeps only the glyphs) and Ctrl + / Ctrl −
  step it. It takes Files' seven steps (`ZOOM_MIN`…`ZOOM_MAX`, `sdk/filepickermodel.js`): pictures of 48 to 200 px,
  72 px being the old tile. The size is remembered with the view (`pickerView.zoom`, `{ files, images }`).

Local disks are new here: an app that asks through the dialog can now be given a file on one, when the person picks
it. The same dialog answers `org.freedesktop.portal.FileChooser` for Linux apps with no file dialog of their own,
inside the home folder (`WAYLAND.md`). Isolated `.lpk` apps keep the powerbox, drawn with this same dialog, which hands
them only the chosen file's descriptor (`PRIVACY.md`, "Powerbox").

## Tests

- `make -C server build/fsops-test && build/fsops-test` on a temporary home folder: 49/49. It covers:
  - roots, the read-only tree (`fsops_readable`: `/`, `/etc`, the user's mounts; not `/proc`,
    `/sys`, `/dev`, `/run`, another user's mounts or a relative path) and protected folders;
  - recursive copy with a link and permissions, and that it neither overwrites nor copies into itself;
  - move;
  - Trash: repeated names, restore without overwriting, delete only inside, and empty;
  - size, search and recents.
- On VM 116 (screenshots above): new folder and rename, copy and paste (with a 600 MB file in the
  background, without freezing the UI), duplicate, move by dragging onto a folder, to the Trash
  (with a valid `.trashinfo`), restore, empty with confirmation, Quick Preview with arrows, Get Info,
  the four views, tabs, search, tags, Recents, Applications, «Open with» the Editor and
  FeatherPad, light and dark theme, and Arabic.

## Pending

- Ask «Replace?» when pasting or dropping onto a name that already exists: today a free name is
  chosen.
- Selection rectangle by dragging on empty space.
- PDF, video and SVG thumbnails in the icon view (PDF already shows in Quick Preview); app icons per
  file type.
- Watching folders with inotify: today the view reloads on re-entering or after one of its own
  operations.
- The status bar and the path bar end up behind the Dock with the window maximized, as on other
  desktop systems when the Dock doesn't hide.
- On the VM, ❖C opened Settings, because some ❖ shortcut of the theme or another part of the
  system grabbed it. Ctrl+C/Ctrl+V works.
