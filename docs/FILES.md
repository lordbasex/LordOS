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
    unmounts another disk's partition with `disk-unmount`), every other disk with a file system that isn't mounted
    (dimmed; a click mounts it with `disk-mount` and opens it; an internal one asks for an
    administrator, as in Disks; the disk the system runs from is left out) and the Trash;
  - Color tags (they filter).

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
- **Copy, cut and paste files:** it is a clipboard of its own, separate from the text one.
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

## Network (0.8.7, in development)

**Files → Network** shows what is on the local network by itself: computers, SMB shared folders, NFS exports,
other network storage and network services, each with its name, address, kind and what can be done with it. It
stays off until the page is opened and leaves when the page closes.

**State of the feature:** the maintainer approved the `lan` connector on 2026-10-31, so inside a session
`lan_socket` gets its sockets from it (`sec/netconn.h`, `NC_LAN`), counted and decided as the firewall app
`lordos:lan` in `lordos-net-lan.scope` (`FIREWALL.md`, "Files → Network and the connectors"). The tests below come from
before that, outside a session (see "What was tested"). **SMB with a password is not validated:** see "Not tested".

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
anonymous sessions (most desktop systems by default) shows as "lists its folders only after signing in", and the
share's name is typed in *Connect*. Listing with a password is not built: it needs NTLMv2 and signing in `lord-lan`.

**Connecting** runs the closed `share-mount` action (`lan/share_admin.c`, table in `SYSTEM.md`). The kernel's own
`cifs` and `nfs` clients do the work through the new mount API, one `fsconfig()` per option: no option string, no
program, no shell. **No package is needed** beyond the kernel's modules: `cifs-utils` and `nfs-common` are not used
(Debian's and the Pi kernel both ship the modules; the client side was tested on Debian's). Addresses are IPv4 and private, never names, so nothing
resolves as root. SMB tries the password given, or the guest; NFS tries 4.2, 4.1, 4.0 and 3 (`nolock`, without
`rpc.statd` its locks stay local), `sec=sys`, and has no sign-in: the server decides by address. The folder is
`/media/<user>/<share> on <address>`, `nosuid,nodev`; on SMB the files are the session user's, 0600 and 0700.

The password goes from the sheet to the action with `{ quiet: true }` and from there to one `fsconfig()`, and is
wiped; it was not in `/proc/mounts`, the session's log, the journal or any command line. *Remember the password*
saves it in the keyring (owner `files`, service `smb://<address>/<share>`) and nowhere else; with a locked or
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

## Shortcuts

| Shortcut | What it does |
|---|---|
| ❖N / ❖T / ❖W | New window / new tab / close tab |
| Ctrl+Tab, ⇧Ctrl+Tab | Next / previous tab |
| ❖1 ❖2 ❖3 ❖4 | Icons / list / columns / gallery |
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
