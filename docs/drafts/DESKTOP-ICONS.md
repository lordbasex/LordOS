# Icons and files on the desktop (design study)

**Status: draft, design only.** Nothing here is built. It designs the roadmap item "Icons and files on the desktop"
(`docs/ROADMAP.md`, maintainer, 2026-11-24) for the release after 0.8.9.6.1: the files and folders of the XDG Desktop
folder, plus optional places and app launchers, shown as icons on the desktop of every desktop style (Lord Flow, Lord
Classic, Lord Workspaces, Lord Celeste), and handled the way Files handles them.

How to read it:

- **Read** means checked in the code or documents of `test/integration-08961` at `0a59963ab` on 2026-11-25, with the place
  given. It says what the code says, not that anyone ran it.
- **Proposed** is this study's design. **Decision** marks a choice left to the maintainer (all of them are listed in
  section 10, each with a recommendation).
- Paths are relative to the repository root.

## 0. Summary

- **What shows.** The entries of the XDG Desktop folder (`userDirs().desktop`, so `~/Escritorio` in Spanish), and,
  each behind its own switch, Home, the Trash, the user's mounted disks, connected network places and app launchers.
  Launchers are **not files**: they are typed launcher ids (`desktop:firefox-esr`, `app:files`, `flatpak:…`, `web:…`)
  kept in `settings.json`, the same ids the Dock pins, and they open through `launchApp(id)` and therefore only through
  the approved `lord.apps.launch*` family. JS never writes a `.desktop` file and never runs one.
- **`.desktop` files other systems left in `~/Desktop`** are plain files. One whose file name is the id of an
  installed app (in `/usr/share/applications`, `~/.local/share/applications` or a Flatpak export the Store installed)
  is shown as that **installed** app (its own name and icon, never the file's) and opens it by its id. Nothing in the
  file is executed, and there is no "Allow Launching".
- **Interaction is Files'.** Click, Ctrl/⌘ and Shift selection, the rubber band (`apps/files/marquee.js`), arrows and
  type-ahead, Enter or F2 or the slow second click to rename (`sdk/slowclick.js`), double click to open
  (`sdk/sysopen.js`), Files' context menus, copy/cut/paste on Files' file clipboard, drag and drop within the desktop,
  to and from Files, to the Dock's Trash, from Linux apps (Thunderbird's `.eml`, Chromium's downloads) through the
  existing `wayland.takeDrop` gate and `fsops_resolve_copy_source`, and to Linux apps through `wayland.dragFiles`.
- **Layout.** A grid per display keyed by connector, either kept arranged (sorted) or arranged by hand on the grid;
  four icon sizes; two-line labels with an ellipsis (the whole name when selected); right to left mirrors the grid;
  logical pixels, so HiDPI and mixed DPI work as the widgets do. Widgets keep their cells and icons flow around them.
- **One SDK piece for every theme.** A pure model `sdk/deskiconsmodel.js` (node-tested) and a component
  `sdk/deskicons.jsx` (`<DesktopIcons />`), with the file logic lifted out of `apps/files/` into the SDK so the desktop
  and Files share one implementation. Each theme places one element between its wallpaper and its widgets and adds the
  SDK's menu items to its own desktop menu. Per-theme on/off; default on in Flow and Classic, off in Workspaces and
  Celeste (decision D1).
- **Performance.** Listing off the main thread (`lord.fs.listAsync`), a new read-only `lord.fs.watch` (inotify, one
  folder, coalesced) instead of polling, thumbnails from `lord-media` as Files and the file dialog already get them,
  no React render per pointer move, and a hard cap: never more icons than the grid has cells, the rest behind a
  "More in Files" tile.
- **Security.** No new exception, no new root action, no new helper, no new library, and nothing new on the
  `fsops_protected` deny list if the layout stays in `settings.json` (decision D6). The one new native API,
  `lord.fs.watch`, only observes a folder `lord.fs` may already read, is refused through the broker, and runs no code.
- **Effort.** About 25 to 34 developer days in eight phases (section 9), the first two of which (lifting Files' pieces
  into the SDK, `lord.fs.watch`) are useful to Files on their own.

## 1. What exists today

**Read.**

- **No theme shows files.** Every desktop is a wallpaper per display (`wallpaperIdFor(s, d)` by connector), the
  motion wallpaper, the widget layer below the windows, the windows, and the widget layer pinned above them:
  `themes/default/index.jsx:190-207`, `themes/classic/index.jsx:197-212`, `themes/workspaces/index.jsx:334-366`,
  `themes/celeste/index.jsx:222-240`. A right click on the wallpaper opens the theme's own menu (Flow: Change Wallpaper,
  Add Widget, App Launcher, Overview, Settings, About; Classic: Overview, Personalize, Wallpaper, Add Widget, Display
  Settings; Workspaces and Celeste similar). `docs/FILES.md` says so: "LordOS's desktops … don't show files on the
  desktop, so Files is its only user today" (of `sdk/slowclick.js`).
- **Widgets** (`sdk/widgets.js`, `sdk/deskwidgets.jsx`, `sdk/widgetgrid.js`): every theme calls `startDeskWidgets()`.
  A widget belongs to one activity (desktop) and one display (`dk`, the connector, `#<id>` when there is none), and
  holds a block of 170 px square cells (16 px gap, 24 px margin) of that display's work area, **anchored to the top
  right** in every language. Kept in `settings.deskWidgets` (at most 48). A widget whose display is gone shows on the
  main display. A display with widgets is "not empty" (`deskWidgetsShown`), which Workspaces reads to hide its
  empty-activity card (`themes/workspaces/components/GroupFrame.jsx:213`).
- **Per display rule** (maintainer, 2026-11-16): per-monitor options go in the SDK for all themes, keyed by connector
  (`displayWallpaperKey` in `sdk/wallpapermodel.js`), never by model name (the maintainer runs three identical
  monitors).
- **Files' pieces** that the desktop needs live in `apps/files/`: the file clipboard and `transfer` with its job queue
  (`ops.js`, `jobqueue.js`), free names (`names.js`), the rubber band (`marquee.js`, `marqueeview.jsx`), the drag
  stores and ghost (`dnd.jsx`), the name label and in-place rename field (`views.jsx` `Name`), opening (`ops.js`
  `openEntry`), Quick Preview (`quicklook.jsx`), and the Linux clipboard bridge (`clipfiles.js`). Already in the SDK:
  `sdk/slowclick.js`, the glyphs and `EntryIcon` with thumbnails (`sdk/fileglyphs.jsx`), `sdk/fileparts.jsx`,
  `sdk/filekinds.js`, `sdk/sysopen.js` (`openFileWithApps`, `revealPath`, `openFolder`), `sdk/filedrop.js`,
  `sdk/thumbcache.js` and `useThumb`, and `userDirs()` (`sdk/media.js:265`, parses `~/.config/user-dirs.dirs`).
- **Files has no folder watching** (`docs/FILES.md`, "Pending": "today the view reloads on re-entering or after one of
  its own operations"); there is no `lord.fs.watch` (`server/src/ui/js_sys.c:1634-1662`). inotify is used only by
  `admin.c`, `remote/monitor.c`, `ui/js_logs.c` and `netperm.c`. Files has **no undo**.
- **Drops from Linux apps** (`docs/WAYLAND.md`, "Drag and drop"): a `text/uri-list` drag released over LordOS is held
  by the compositor; only the UI's handling of that very release may claim it (`wayland.takeDrop(fn)`, `DropGate` in
  `server/src/wl/dndgate.c`), otherwise it is cancelled without its list being read. Files claims it when a folder of
  its window is under the pointer (`apps/files/index.jsx:772-779`). Nothing on the desktop claims it, so a message
  dragged from Thunderbird and released over the wallpaper "is cancelled at once" (`CHANGELOG.md`, Unreleased, #188).
  Since #188, `lord.fs.copy` takes a source anywhere `lord.fs` reads, but only a regular file, never a link or a folder
  outside the home and mounts, never for a move (`fsops_resolve_copy_source`, `server/src/fsops.c:1126`).
- **Files → other apps**: Files walks the node tree under the pointer for a `dropPath` prop (a folder that takes
  files) or an `onFilesDrop` prop (an app that takes files), `apps/files/index.jsx:736-741`. Since Files runs in the
  theme's process, a desktop node with `dropPath` already receives Files' drags with no new plumbing.
- **Focus**: there is no "nothing focused" state. `wm.focused()` is the front window of the active desktop
  (`sdk/windows.js:544`, `focusedOn` in `sdk/desktopmodel.js`); the compositor gives a Linux window the keyboard by
  the same rule. The desktop never has the keyboard today.
- **Names**: `sdk/desktopmodel.js` is taken: it is the model of Desktops (virtual desktops). The new model must not
  reuse "desktop" alone in its name.

## 2. What shows on the desktop

### 2.1 The Desktop folder

**Proposed.** The folder is `userDirs().desktop`, moved into the SDK's file layer (it is file logic, not media) and
hardened:

1. Read `XDG_DESKTOP_DIR` from `~/.config/user-dirs.dirs` (only the `"$HOME/<relative>"` form, as today; an absolute
   path outside the home is not followed).
2. `XDG_DESKTOP_DIR="$HOME"` is what `xdg-user-dirs-update` writes when the folder was missing. Showing the whole home
   folder on the desktop is never right: treat it as "no Desktop folder".
3. Resolve it (`lord.fs.stat`, which resolves the folder through `realpath`): it must be a folder inside the home or
   the user's mounts and must not be a protected folder. JS can't ask `fsops_protected` today, so `lord.fs.stat` gains
   a read-only `protected` flag (`fsops_protected_resolved` on the resolved path; a small addition, no new power).
   A `user-dirs.dirs` that points the desktop at `~/.config/autostart` or `~/.local/share/applications` shows nothing
   and logs once. Writes there would be refused in C anyway; this keeps the desktop from even listing such a folder
   as if it were the user's.
4. When the folder does not exist, the desktop shows only the places and launchers; the first drop, paste or New
   Folder creates it (`lord.fs.mkdir`, inside the home), so nobody gets an empty `~/Desktop` they never asked for.
5. Re-read when the session's language changes (Settings → Language may rename the folders on the next login) and
   when `user-dirs.dirs` changes (watched with the same `lord.fs.watch` on `~/.config`, filtered to that name).

Hidden entries (a leading dot) are not shown unless Files' "Show hidden files" is on (`settings.files`), to stay
consistent with Files. File extensions follow Files' `showExtensions` the same way.

### 2.2 Optional places

Each behind its own switch in Settings → Desktop & Dock → Desktop icons (`show.home`, `show.trash`, `show.drives`,
`show.network`). Places are not files in the folder: they are drawn by the SDK, never written anywhere, and their
names are localized labels, not file names.

| Item | Source | Opens | Its menu | Drop onto it |
|---|---|---|---|---|
| Home | `fs.home()` | `openFolder(home)` | Open, Open in New Window | move/copy into the home folder |
| Trash | `useTrash()` (the Dock's) | `openTrash()` | Open, Empty Trash (with `ConfirmDialog`) | `lord.fs.trash` |
| A mounted disk | `fs.volumes()` (`/media/$USER`, `/run/media/$USER`) | `openFolder(path)` | Open, Eject (`disk-eject` / `disk-unmount`, as Files' sidebar) | copy into it |
| A network place | Files' connected entries (`apps/files/netplaces.js connectedEntries`) | `openFolder(path)` | Open, Disconnect (as Files) | copy into it |

Defaults (decision D2): Flow like macOS (drives on, the rest off), Classic like Windows (Home and Trash on, drives off),
Workspaces and Celeste all off. A network place is listed by its mount entry, never `stat`ed on the interface's thread
(the rclone hang of #85; `fsops.c remote_children`).

### 2.3 App launchers

**Proposed.** A launcher is a typed launcher id, validated with the Dock's own rules (`dockId`/`parseDockId` in
`sdk/dockmodel.js`: `app:`, `lpk:`, `desktop:`, `web:`, `flatpak:`), and its place on the grid:

```js
settings.desktopIcons.launchers = [{ id: 'desktop:firefox-esr', dk: 'DP-2', col: 0, row: 3 }, …]   // at most 64
```

- **Adding:** "Add to Desktop" in the app launcher's and the start menu's context menu, and dragging an app from the
  app launcher or the start menu onto the desktop (the desktop registers as a `registerDockTarget` target that accepts
  only `source !== 'dock'` drags: dragging an item *out of the Dock* keeps meaning "remove it from the Dock", as today).
- **Opening:** `launchApp(id)` (`sdk/apps.js:208`), the Dock's path: `wm.open` for a LordOS app,
  `lord.apps.launchPackage` for an `.lpk`, `lord.apps.launch(desktopId)` for an installed `.desktop` (a Terminal=true
  app opens in a Terminal tab through the one-shot token), `lord.apps.launchFlatpak` for a Flatpak, `launchWebApp` for
  a web app. No new argument, path or launch path.
- **Showing:** the installed app's name and icon from `useLauncherApps()`. A launcher whose app is no longer installed
  is hidden (kept for 30 days in case the app comes back, then dropped).
- **Removing:** "Remove from Desktop" deletes the entry; nothing on disk changes. Moving a launcher to the Trash is the
  same as removing it (the Trash never receives it).
- **Not files.** A launcher can't be copied into a folder, dragged into Files or onto a Linux app (the drag offers it
  only to the desktop and the Dock). Selecting launchers together with files and choosing Copy copies only the files.

Why not a `.desktop` file in `~/Desktop` as GNOME and KDE do: JS must never write `.desktop` files (AGENTS.md), and a
file that launches something is exactly what the deny list exists to keep away from JS. A typed id in `settings.json`
can only ever name something the launcher already lists, and `settings.json` is on the deny list already.

### 2.4 `.desktop` files that other systems left there

Someone who copies a home folder from Ubuntu or Fedora, or installs a program whose installer drops a shortcut, ends
up with `foo.desktop` files in `~/Desktop`. **Proposed:**

- A `.desktop` entry that is a regular file or a link, whose **file name** (without `.desktop`) equals the id of an
  app the launcher lists from `/usr/share/applications` or `~/.local/share/applications` (`desktop:<id>`), or the id of
  an installed Flatpak export (`flatpak:<id>`), is shown as **that installed app**: the installed app's name and icon
  (`useLauncherApps()`), with a small arrow badge saying it is a shortcut. A double click is `launchApp('desktop:<id>')`.
  The file's own `Name`, `Icon` and `Exec` are **never read**: a hostile file named `firefox-esr.desktop` can at most
  open the real Firefox.
- Any other `.desktop` file is a **plain file**: a generic document icon, its file name, and a double click opens it
  in the Text Editor as text, the way Files treats it. There is no "Trust and Launch" or "Allow Launching" action, and
  the executable bit and GIO's `metadata::trusted` are ignored.
- Its context menu has Show in Files, Rename, Move to Trash and, for a matched one, "Add to Desktop as a launcher"
  (which writes a launcher entry and offers to move the file to the Trash).
- Limits, documented: custom arguments in a copied `Exec` are not honored (the installed entry runs); an app whose
  installed id differs from the copied file's name is not recognized.

The same rule covers other foreign shortcuts: `.lnk`, `.url`, `.webloc` and AppImages are plain files with Files'
behavior (an AppImage says Files doesn't run programs, `isProgram`; a Windows program goes to Open with providers).

## 3. Interaction (the same as Files)

### 3.1 Keyboard focus on the desktop

The desktop must be able to have the keyboard, which today nothing can (section 1). **Proposed:** a `desktopFocus`
state in `sdk/windows.js`:

- A click on the desktop (an icon or empty space) sets it; `focusedOn` then answers null, so no window is drawn
  focused and the compositor takes the keyboard away from Linux surfaces, as it does for the `overlay` layer. Any
  window raised, clicked or opened clears it. ❖D / Show Desktop (`toggleShowDesktop`) sets it.
- While it is set, the SDK component handles the keys below; the theme's own shortcuts (`useShortcuts`) still come
  first, as in Files.

This touches the focus model all four themes share, so it is the main technical risk of phase 3; it must be checked
with a Linux app in front (no key may leak to it) and with the lock screen (no change).

### 3.2 Selection

Files' rules, unchanged (`docs/FILES.md`, "Selection"): a click selects, ❖/Ctrl+click toggles, ⇧+click selects the
range **in the arrangement order** (column by column in the grid's flow order), ❖A selects all, Esc clears, typing the
start of a name jumps to it. A click on empty space clears the selection. The rubber band is `marquee.js` (lifted to
`sdk/marquee.js`): a drag from empty space draws the rectangle (one node moved with `lord.setStyle`), Shift adds and
Ctrl toggles; no autoscroll (the desktop does not scroll). A drag that starts on a window never starts a band; a band
never selects launchers on another display (it is clipped to the display it started on).

Arrows move the selection by cell in the grid (reversed horizontally in right to left), Home and End go to the first
and last icon; ⇧ extends.

### 3.3 Rename

Enter or F2, or the slow second click on the selected icon's **name** (`slowClick()` from `sdk/slowclick.js`, with
`focused` meaning "the desktop has the keyboard" and `sole` and `canRename` from the model). The field is Files' `Name`
field (lifted to `sdk/filename.jsx`) laid over the label, `selectAll` without the extension, Tab and Enter commit, Esc
cancels; the new name goes through `lord.fs.rename` (`RENAME_NOREPLACE`) and Files' name checks (`apps/files/names.js`:
`/`, NUL, 255 bytes). Never for places or launchers (a launcher's name is the app's). The icon keeps its grid cell
under the new name.

Classic users expect Enter to open (Windows). Files makes Enter rename in every theme. Decision D4: keep Files'
Enter = rename on every desktop (recommended, one rule), or make Enter open on the Classic desktop only.

### 3.4 Open

Double click, or ❖↓ / ❖O, open the selection: a folder in a new Files window (`openFolder`), a file with the same
rules as Files (`openFileWithApps`, so Open with providers, quarantine marks and the "Files doesn't run programs" note
are the same), a launcher with `launchApp`, a place as in 2.2. Space opens Quick Preview once `quicklook.jsx` is lifted
to the SDK (phase 7); until then Space does nothing on the desktop.

### 3.5 Context menus

The SDK returns the items; each theme merges them with its own (decision D5 on order). **Proposed** for empty space:

- New Folder (❖⇧N), Paste (when `canPaste`), Paste Copied Files from a Linux app (Files' sheet, `clipfiles.js`)
- Sort By ▸ Name / Kind / Date Modified / Size, and Keep Arranged (a check) / Clean Up (arranged once)
- Show in Files (opens the Desktop folder)
- the theme's items: Change Wallpaper…, Add Widget…, Overview, App Launcher…
- Desktop Settings… (Settings → Desktop & Dock, scrolled to Desktop icons)

For an item: Open, Open With ▸ (Files' list), Show in Files (`revealPath`), Rename, Copy, Cut, Duplicate, Compress
(lord-doc, as Files), Tags (Files' swatches and `tags.json`), Get Info (opens Files' Get Info for that path: a new
`args.info` for Files' window), Move to Trash. A read-only entry (on a read-only mount) leaves out what would change
it, as Files does. For a launcher: Open, Remove from Desktop, Show in App Launcher. For places: as in 2.2.

### 3.6 Copy, cut, paste

The desktop uses Files' file clipboard (moved to `sdk/fileclip.js`): Copy and Cut put the paths there and on the
session clipboard as `text/uri-list` and `x-special/gnome-copied-files`, so a cut on the desktop pasted in Files
moves, and the other way. Paste into the Desktop folder uses `transfer` (never overwrites, free names: «notas 2.txt»).
Pasting files copied in a Linux file manager asks first, exactly as Files does.

### 3.7 Drag and drop

All of it uses the existing protocols; nothing in C changes.

| From → to | How | Default action |
|---|---|---|
| Desktop → desktop (empty cell) | the model moves the icon to the cell (manual mode) or does nothing (kept arranged; the drag snaps back) | reposition, no file change |
| Desktop → a folder icon on the desktop | `transfer` | move (⌥ copies) |
| Desktop → Files (a folder, a tab, the sidebar) | the desktop's drag carries the same `dropPath` contract: it reads the target's `dropPath` | move (⌥ copies), same disk; copy across disks, as Files |
| Files → desktop | the desktop's empty space carries `dropPath = <Desktop folder>`, a folder icon its own path: Files' existing code moves or copies there | as Files |
| Desktop or Files → the Dock's Trash / the Trash icon | `dropTrash` (as Files) | trash |
| Desktop → a LordOS app that takes files (Terminal, Editor, Notes, Photos…) | the target's `onFilesDrop` / `onFilesOver` (`sdk/filedrop.js`) | as from Files |
| Desktop → a Linux app | `wayland.dragFiles(paths)` when the drag starts, `wayland.endDrag()` at the release | copy; Shift moves if the app takes a move |
| A Linux app (Thunderbird `.eml` in `/tmp/dnd_file/`, Chromium's download bar, a GTK file chooser, an X11 app through XDND) → desktop | at the **release** over the desktop, `wayland.takeDrop((paths) => transfer(paths, dir, false))`, `dir` being the folder icon under the pointer or the Desktop folder | copy (sources outside the home are allowed by `fsops_resolve_copy_source`: regular files only) |
| App launcher / start menu → desktop | `beginDockDrag` with `source: 'launcher'`, the desktop as a dock target | add a launcher |

The ghost is Files' `DragGhost` (lifted to `sdk/filedragview.jsx`), drawn in the `drag` layer so it follows the pointer
over windows. Only the folder or cell under the pointer redraws while dragging (Files' `dnd.target` store, #218).
Drops never overwrite; a name clash takes a free name, as Files does today.

Hover feedback during a **Linux** app's drag (a folder icon lit while Thunderbird's message is over it) needs the UI to
see pointer moves during a compositor-held drag. That is not confirmed by reading; if it doesn't, the drop still lands
in the folder under the pointer at the release, without the highlight (open question Q1).

Sandboxed sources stay refused, as today: an isolated LordOS app or a Flatpak hands LordOS no files (`dndgate`).

### 3.8 Trash and delete

❖⌫ and Del move the selection to the Trash (`lord.fs.trash`, freedesktop Trash, from the home or `$mount/.Trash-$uid`).
There is no permanent delete on the desktop (only inside the Trash, as Files). Launchers selected with files are
removed from the desktop, not trashed.

### 3.9 Undo

Files has no undo. **Proposed** (phase 7, decision D7): `sdk/fileundo.js`, a shared journal of the last 20 operations
started by the desktop **or Files**, with ❖Z on whichever has the keyboard:

| Operation | Undo | Refused when |
|---|---|---|
| Rename a→b | rename b→a (`RENAME_NOREPLACE`) | b is gone or a was taken meanwhile |
| Move to the Trash | `lord.fs.restore(id)` (never overwrites) | the trash entry is gone |
| Move (drag, cut/paste) | move back to the old folder under the old name | the old name was taken; the item changed device |
| Copy / duplicate / drop from an app | move the copies to the Trash (never a delete) | a copy was modified since (mtime) |
| New folder | move it to the Trash if it is still empty | it isn't empty |
| Reposition on the desktop | put the icons back in their cells | — |

Each step re-checks its precondition with `stat` at undo time (another process of the same user may have changed
things, section 6), and a refused undo says why. No redo in the first version.

## 4. Layout

### 4.1 The grid

**Proposed** (`sdk/deskiconsmodel.js`, pure):

- Each display's work area (`workAreaOf(d)`: menu bar, Dock, taskbar, Workspaces' bar and Celeste's frame already
  taken out) gets a grid of cells; a cell's size follows the icon size: small 32 px icons in 76×76 cells, medium 48 in
  92×96 (the default), large 64 in 108×116, extra large 96 in 140×152, margins 12 px. The numbers are tuned on the VM
  against the 8 px grid of `docs/DESIGN.md`.
- **The flow** is column-major from the **start** edge: top to bottom, then the next column towards the end edge. That
  is the convention of Windows, GNOME and macOS for desktop icons, and it leaves the top-right corner (where widgets are
  anchored) for last in left-to-right languages.
- **Obstacles.** A cell that overlaps a widget's rectangle (`widgetfeed.js setWidgetRects`, published by
  `startDeskWidgets`) or a pinned widget is taken: icons flow around widgets. When a widget is dropped over icons,
  the widget wins; the icons under it move to the nearest free cells (`nearestFree`, the same search as
  `widgetgrid.js`). Workspaces' empty-activity card and Celeste's hover edges are obstacles too (each theme passes its
  own rectangles: `obstacles(d)`).
- **Two modes** (decision D3):
  - **Keep arranged** (default): icons are sorted (folders first, then by name, kind, date or size, natural order as
    `compareNames`) and fill the flow; nothing is stored per icon. A drop of an external file just appears in its
    sorted place.
  - **Arranged by hand**: an icon keeps the cell it was dropped on, stored as `{ name → [col, row] }` per display.
    A new file takes the first free cell in the flow; "Clean Up" packs them once in the flow without sorting; "Sort By"
    sorts them once.
  - Free pixel placement (no grid) is left out of this proposal: overlap, hit testing and saving fractional positions
    cost more than they give, and every reference desktop snaps by default.
- **Where positions are kept.** In grid cells, never pixels, keyed by the display's connector (`displayWallpaperKey`),
  and stored **logically** (column counted from the start edge), so Arabic or Hebrew mirror the arrangement as they
  mirror the rest of the interface. When the grid shrinks (a smaller resolution, a larger icon size, a wider Dock) an
  icon whose cell is gone takes the nearest free one **for display only**; its saved cell stays, so it goes back when
  the room comes back (the widgets' rule in `placed()`).
- **Overflow.** Never more icons than cells. When the folder has more entries than free cells, the last cell becomes a
  "N more — Show in Files" tile (`openFolder`). This is also the hard limit on how many icons are drawn (section 5.4).

### 4.2 Icon size and labels

- Four sizes in Settings (`size: 's' | 'm' | 'l' | 'xl'`), plus Ctrl + wheel on empty desktop space steps it, as in
  Files' zoom.
- Icons are `EntryIcon` (`sdk/fileglyphs.jsx`): the same folder and document glyphs as Files, tag tint, thumbnails when
  "Show previews" is on, a lock for an unreadable folder, a small arrow for a link and for a matched `.desktop` file.
- **Labels**: centered, at most two lines (`numberOfLines: 2`, `textOverflow: 'ellipsis'`, the engine's line clamp,
  already used by Files' icon view, `apps/files/views.jsx:47-49`), white with a soft text shadow over the wallpaper
  (`shadowColor`, `shadowBlur` in `server/src/ui/style.c`), never a blurred backdrop per label (cost, section 5). The
  selected icon shows its whole name on up to five lines on a filled accent rounded rectangle, drawn above its
  neighbours, as Finder and Nautilus do. The label's text direction follows the name (the engine's bidi), and its
  alignment is `textAlign: 'center'`, so mixed Arabic and Latin names render correctly in both directions.
- Names are shown cleaned for display only (section 6.1); operations always use the real name.

### 4.3 HiDPI and mixed DPI

All geometry is in the layout's logical pixels, per display, as the widgets and the wallpaper already are
(`screenDisplays()` gives `scale`); thumbnails are asked at `iconPx * d.scale / 100` so a 200 % display gets sharp
pictures (`useThumb(path, size)`). An icon dragged from a 100 % display to a 200 % one changes cell size with it.

### 4.4 Several displays

**Proposed** (decision D8):

- **Icons on: main display** (default) or **every display**. With "every display", there is still **one set**: each
  icon lives on one display (`dk`). New files go to the display chosen as "new icons go to" (the main one by default)
  and flow onto the next display, in the order of Settings → Displays' arrangement, when it is full. Dragging an icon
  to another display moves it there. Showing the same icons on every display is not offered (selection, rename and
  drag would be ambiguous).
- A display that is unplugged: its icons are drawn on the main display (not saved there) until it comes back, as the
  widgets do. With mirrored displays, the main display only.
- Desktops (Flow, Classic, Celeste) and activities (Workspaces): the icons are the same on every desktop and activity,
  because the folder is one; positions are per display, not per desktop (decision D9).

### 4.5 Coexistence with the shell

- **Stacking**, bottom to top: wallpaper, motion wallpaper, **desktop icons**, widgets below the windows, windows,
  widgets pinned above, the shell's layers. Icons are under the widgets, but the grid keeps them out of widget cells,
  so they only touch while a widget is being dragged.
- **The Dock and the taskbar** are outside the work area, so icons never go under them; an auto-hiding Dock's revealed
  area is inside the work area, and the Dock simply covers the bottom row while shown, as on macOS.
- **The Overview**: icons are hidden while it is open (the `OverviewBackdrop` covers them), and the Overview's
  miniatures don't draw them.
- **Workspaces**: the bar (TopBand/Spine) is outside the work area; the empty-activity card is an obstacle; when the
  activity has tiled windows, the icons are behind them as with any window. A display with icons still shows its
  empty-activity card if there is room (decision D10).
- **Celeste**: the frame is outside the work area; the hover edges that open drawers are obstacles (cells within
  `hoverZone` are not used), and they are ignored while an icon is being dragged so a drag to the edge doesn't open a
  drawer.
- **Show desktop** (where the theme has it) gives the desktop the keyboard (3.1).

## 5. Performance

Everything runs on lord-server's main thread with the compositor (`docs/THEMES-GUIDE.md`, "Performance rules").

### 5.1 Listing and watching

- The folder is listed with `lord.fs.listAsync` (a worker thread; `sdk/fs.js`), never `lord.fs.list` on the UI
  thread: the Desktop folder may be on a slow disk, a Cloud mount or a share.
- **`lord.fs.watch(path, fn)` (new, read-only; phase 1)**. Proposed contract:
  - `path` is resolved like a read (`fsops_resolve_readable`: `realpath`, `fsops_readable`; never `/proc`, `/sys`,
    `/dev`, `/run`) and must be a folder; a FUSE or network mount is refused (inotify doesn't see remote changes;
    the caller falls back to listing again on focus and every 10 s while shown).
  - One inotify fd in lord-server's poll loop; `inotify_add_watch(fd, real, IN_CREATE | IN_DELETE | IN_MOVED_FROM |
    IN_MOVED_TO | IN_ATTRIB | IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR | IN_DONT_FOLLOW |
    IN_EXCL_UNLINK)`, non-recursive.
  - At most 16 watches per session (the desktop needs two: the Desktop folder and `~/.config` for `user-dirs.dirs`;
    Files gets the rest for its open folders, which closes its pending item). The 17th throws `fs.tooManyWatches`.
  - Events are **coalesced**: JS gets one call `fn({ overflow })` at most every 150 ms per watch, with no file names in
    it, and lists the folder again (asynchronously). `IN_Q_OVERFLOW`, `IN_DELETE_SELF` and `IN_MOVE_SELF` say so, so the
    desktop resolves the Desktop folder again.
  - Not available through the broker (isolated apps): refused in `brokersrv.c`, like the other calls they don't need.
  - Returns an id; `lord.fs.unwatch(id)`; every watch ends with the theme context (a theme switch).
- Between two listings the model diffs by name: kept names keep their icon (and node: React keys are the names), new
  ones are placed, gone ones leave a free cell. The desktop's own operations update the model at once, without waiting
  for the watch (as `fsVersion`/`bump` does for Files today).

### 5.2 Thumbnails

`useThumb` from `sdk/media.js` / `sdk/thumbcache.js`: `lord-media` (sandboxed) makes them off the main thread into
`~/.cache/lordos/thumbs`, shared with Files, Photos and the file dialog; a file shows its type glyph until its
thumbnail comes, and for good when it has none. The cache's rate limits (`RETRY_MS`, `KEEP_MS`) and the batched
wake-ups already keep a folder of thousands of videos from flooding lord-server. A "Show previews" switch turns them off
(on by default; decision D11 on whether PDFs and videos are included, since `lord-doc`/`lord-media` cost more for them).

### 5.3 Repaint

- The icon layer is static between changes. Selection, hover and the drop highlight redraw only the icons whose state
  changed (selectors in `useStore`, as Files' `useDropHot`), so the damage is a few icon rectangles.
- The rubber band is one node moved with `lord.setStyle` per pointer move, no React render; the ghost is the same.
- No `backdropFilter` on icons or labels: blur reads and repaints a margin around each surface (`docs/LAYERS.md`), and
  a hundred blurred labels would make every window move over the desktop expensive. The selected label's fill is opaque.
- A window moved over the desktop repaints only the uncovered region; `build/engine-test` (`test_damage_fuzz`,
  `test_damage_pipeline`) must keep passing, and a scene measures it (section 7).

### 5.4 Limits

| What | Limit | Why |
|---|---|---|
| Icons drawn per display | the grid's cells (about 200 at 1920×1080, medium) | one node tree per icon; the rest go to the "N more" tile |
| Entries listed | `lord.fs.list`'s 5000 (`LIST_CAP`) | the listing itself is bounded; past it, the tile says so |
| Watches | 16 per session | inotify's per-user limit is shared with the person's own programs |
| Launchers | 64 | `settings.json` is at most 1 MiB (`SETTINGS_MAX`) |
| Saved positions | 500 per display, the least recently moved dropped first | same |
| Undo journal | 20 operations, this session only | — |

## 6. Security

### 6.1 Threat model

The Desktop folder belongs to the user, and any program the user runs can write to it: a browser download, a mail
attachment, an archive extracted there, a Linux app, a script. Everything in it is untrusted input to the desktop.

| Threat | Answer |
|---|---|
| **Hostile file names**: control characters, a newline, U+202E (right-to-left override, `invoice‮fdp.exe` showing as `invoiceexe.pdf`), U+2066–U+2069, zero-width characters, a name of 255 bytes, a name that looks like a place ("Trash", "Home") | Names are **displayed** through a cleaner shared with the window list (`cleanWindowLabel`'s rule in `sdk/windowlistmodel.js`: control and bidi-override characters shown as `�`/dropped, not obeyed), with the ellipsis for length. Operations use the real name. Places and launchers have their own glyphs and are never files, so a folder named "Trash" looks like a folder. The type glyph comes from the real extension (`sdk/filekinds.js`), not from the shown text. |
| **A malicious `.desktop`** (an `Exec` that runs a script, an `Icon` pointing at a crafted image, a `Name` that impersonates another app) | Never parsed, never executed, never trusted for its contents (2.4). A recognized one opens the installed app of the same id through `lord.apps.launch`; its name and icon are the installed app's. |
| **Executables and scripts** (`+x` shell scripts, AppImages, ELF binaries, `.exe`) | Never run. Opening follows Files: text opens in the Text Editor, an AppImage says Files doesn't run programs, a Windows program goes to an Open with provider. The desktop has no "Run" action and never gets one (AGENTS.md: JS never runs commands). |
| **Symlinks** in the folder | Shown with a link badge; listed and `stat`ed without following (`statx` without `AT_SYMLINK_FOLLOW`, as `lord.fs.stat`). Opening a link to a folder opens Files at the **resolved** target, so `fsops_readable` applies (a link to `/proc/self` is refused). Copy and drag copy the link as a link (`fsops` copies links as links); a Linux app's drop of a link from outside the home is refused (`fsops_resolve_copy_source`). Thumbnails are made by `lord-media` from an fd lord-server opened, as today. |
| **The Desktop folder itself a symlink**, or `user-dirs.dirs` pointing somewhere odd | Resolved first; must be inside the home or the user's mounts and not protected; `$HOME` itself is refused (2.1). |
| **Huge folders** (100,000 files, a folder on a stalled share) | Listing off the thread, capped at 5000 entries; icons capped at the grid; the watch is coalesced; a share that stops answering only delays the async listing (`fsops_share_down`), never the UI. |
| **A race with another process of the same user** (a file renamed or replaced between the listing and an action; a folder swapped for a link to `~/.config/autostart` just before a drop) | The C side already decides on the resolved path at the moment of the operation: `RENAME_NOREPLACE`, `O_EXCL` copies, `fsops_protected` on the resolved destination, the Trash never overwriting on restore. The desktop never decides on what it listed: a refused operation is shown as Files shows it. Undo re-checks before acting (3.9). Such a process can already write the home folder directly; the desktop adds no power it didn't have. |
| **A drag from a sandboxed app** carrying paths it can't itself reach | Refused by `dndgate` today; nothing changes. |
| **Settings tampering** (a launcher list naming something nasty) | A launcher is only a validated typed id (`parseDockId`), and `launchApp` only launches what the launcher already lists. `settings.json` is on the deny list, so no LordOS app or theme can write it through `lord.fs`. |
| **Thumbnail parsers** on hostile images, videos and PDFs | Already inside `lord-media` / `lord-doc` sandboxes (`sandbox_apply_helper`, fds only). No new parser in lord-server. |

### 6.2 The deny list (`fsops_protected`)

- **Nothing new**, if launchers and positions live in `settings.json` (already protected). The Desktop folder is the
  user's and must stay writable, like Documents.
- If decision D6 moves the positions to their own file (to keep `settings.json` small), the file holds no security
  decision and launches nothing, so it does not need the deny list; launchers must stay in `settings.json` in every
  case. If a future version ever kept launchers elsewhere, that file would have to go on the deny list (AGENTS.md:
  "if another folder is added that the system reads to launch … something, … it goes on that list").

### 6.3 Exceptions

**None needed.** No root action, no `admin.c` change, no new helper, no new library, no new launch path, no change to
the compositor's drag-and-drop gate. The only new native API is `lord.fs.watch` / `lord.fs.unwatch`, which is a
read-only observation of a folder `lord.fs` already reads, validated in C and refused through the broker; under
AGENTS.md it is an ordinary new `lord.*` call (in `js_sys.c`, with input validation), not an exception. The same goes
for the read-only `protected` flag added to `lord.fs.stat` (2.1). Both are called out here so the maintainer sees them
before they are written.

The focus change of 3.1 (the desktop holding the keyboard) only takes the keyboard away from windows; it gives
nothing to anyone.

## 7. Tests

- **Model (node)**, `server/tests/desk_icons_model_test.mjs` for `sdk/deskiconsmodel.js`: grid per size and work area;
  the flow in LTR and RTL; obstacles (widgets, a pinned widget, Workspaces' card, Celeste's edges); keep-arranged
  sorting (natural order, folders first); manual cells kept, shrunk grid shown-but-not-saved, restored when room
  returns; overflow and the "N more" tile; several displays (main only, spread, unplugged, mirrored, three identical
  monitors told apart by connector); keyboard moves; `.desktop` matching (installed `desktop:` and `flatpak:` ids, a
  non-matching name, a link, a folder named `x.desktop`); display-name cleaning (U+202E, newline, 255 bytes);
  `XDG_DESKTOP_DIR` parsing (`$HOME`, missing, absolute, `..`); position cap and LRU.
- **Lifted Files code**: the existing `files_marquee_test.mjs`, `slowclick_test.mjs`, `files_jobqueue_test.mjs`,
  `files_clipfiles_test.mjs`, `files_names_test.mjs` and the drag and rename scenes keep passing against the SDK
  copies (phase 0 moves code; behaviour stays).
- **Undo** (`server/tests/file_undo_test.mjs`): each operation and each refusal, against a stubbed `lord.fs`.
- **C**: `build/fswatch-test` (new, in `make -C server test`): create, delete, rename within and out of the folder,
  overflow, the watch limit, refusal of `/proc`, of a file, of a FUSE mount, a watched folder that is removed or
  replaced by a link, coalescing; `build/fsops-test` unchanged. A broker case that `fs.watch` is refused.
- **Headless scenes per theme** (`tests/desk_icons_scene.mjs` through `tests/app_scene_headless.sh` with each of
  `default`, `classic`, `workspaces`, `celeste`, in light and dark, in English and Arabic): icons drawn from a
  temporary Desktop folder; select, ⇧ range, rubber band; slow-click and F2 rename; a file dragged from a Files window
  onto the desktop and back; a folder icon taking a drop; a widget added over icons; two displays; the "N more" tile;
  context menus. `tests/theme_cost_scene.mjs` measures a window dragged over 200 icons (damage only, no React render
  per frame) before and after.
- **End to end on VM 127** (`tools/vm/e2e/`), with screenshots, as #188 was checked: a Thunderbird message dropped on
  the desktop (Wayland and X11), a Chromium download dragged from the download bar, a file from the desktop into a GTK
  file chooser's drop zone and into Chromium; Ubuntu-style `.desktop` files copied into `~/Escritorio`; the maintainer's
  three monitors.
- **Claude Security** on the change (the PR rule), with the threat table of 6.1 as its brief.

### 7.1 Migration

- **Per user.** The new key `settings.desktopIcons` (`{ on: { [themeId]: bool }, size, arrange, sortBy, show: { home,
  trash, drives, network }, display, newIconsDisplay, previews, launchers, cells: { [connector]: { [name]: [c, r] } } }`)
  is new, so by the convention of `sdk/settings.js` ("new key, no migration") it needs none: a missing key reads as
  the defaults. A migration (schema 10) is needed only if decision D1 is "existing users start with icons off": it
  would set `desktopIcons.on = { default: false, classic: false }` for anyone with `schema < 10` who already used
  LordOS (`markExistingUser`'s test).
- **The per-theme default** comes from a new optional `theme.json` field, `"desktopIcons": { "default": true }`, read
  by `sdk/thememeta.js` like `displayApps` (a boolean; anything else dropped), so a community theme chooses its own.
  Flow and Classic declare `true`, Workspaces and Celeste `false` (or nothing).
- **System (root).** None: no file outside the home changes. Accounts without a Desktop folder get one lazily on the
  first drop (2.1), not from a migration.

## 8. Shared SDK parts and per-theme styling

| Piece | Where | Notes |
|---|---|---|
| Model: grid, flow, obstacles, modes, positions, displays, keys, `.desktop` matching, name cleaning | `sdk/deskiconsmodel.js` | pure, no React, no `lord.*` |
| Runtime store: the folder, the listing, the watch, the places, the launchers, saving | `sdk/deskicons.js` | `startDesktopIcons()` once per theme, like `startDeskWidgets()` |
| Component `<DesktopIcons display? obstacles? style? />` and `desktopMenuItems({ x, y, display })` | `sdk/deskicons.jsx` | one per theme, between wallpaper and widgets |
| Lifted from Files | `sdk/fileclip.js` (clipboard, Linux paste), `sdk/fileops.js` (`transfer`, job queue, free names, tags), `sdk/marquee.js` + view, `sdk/filedragview.jsx` (stores and ghost), `sdk/filename.jsx` (label and rename field), `sdk/fileundo.js`; later `sdk/quicklook.jsx` | Files imports them back; one behaviour |
| `lord.fs.watch` / `unwatch` | `server/src/ui/js_sys.c`, a small `server/src/fswatch.c` | read-only |
| Focus: `desktopFocus` | `sdk/windows.js`, `sdk/desktopmodel.js` | the four themes share it |
| Settings page | Settings → Desktop & Dock → **Desktop icons** (`apps/settings`) | the switch for the current theme, size, arrange/sort, previews, which displays, which places |

Per theme, only styling and placement, through a `look` prop (label color and shadow, selection fill, corner radius)
built from the theme's tokens:

| | Lord Flow | Lord Classic | Lord Workspaces | Lord Celeste |
|---|---|---|---|---|
| Default | on | on | off | off |
| Default places | drives | Home, Trash | none | none |
| Look | macOS-like: white labels with shadow, accent pill when selected | Windows-like: translucent square highlight over icon and label | the widgets' TangoOS tokens; labels on the Crystal surface's text color | the scheme's colours, rounded pill |
| Obstacles beyond widgets | — | — | empty-activity card | hover edges |
| Desktop menu | merged with Flow's (Change Wallpaper, Add Widget…) | merged with Classic's (Personalize, Display Settings…) | merged with its own | merged with its own |

## 9. Phases and effort

Days are one developer's working days, including tests, the VM check with screenshots and the CLAUDE.md updates
(CHANGELOG, paper, site, `docs/FILES.md`, `docs/THEMES-GUIDE.md`, a new `docs/DESKTOP-ICONS.md` that replaces this
draft) for that phase.

| Phase | What | Size | Days |
|---|---|---|---|
| 0 | Lift Files' clipboard, `transfer`/job queue/free names/tags, marquee, drag stores and ghost, name label and rename field into the SDK; Files uses them. No behaviour change. | M | 3–4 |
| 1 | `lord.fs.watch`/`unwatch` in C with `build/fswatch-test` and the broker refusal, `lord.fs.stat`'s `protected` flag; Files watches its open folders (closes `FILES.md`'s pending item) | M | 2–3 |
| 2 | `sdk/deskiconsmodel.js` and its node test (grid, flow, RTL, obstacles, modes, displays, keys, matching, names) | M | 3 |
| 3 | `sdk/deskicons.js(x)`, desktop keyboard focus, selection, rubber band, rename (slow click, F2, Enter), open, context menus, copy/paste, Trash; wired into **Flow and Classic**; Settings → Desktop icons; strings in every UI language | L | 5–7 |
| 4 | Drag and drop: within the desktop, desktop ↔ Files, to the Trash, to LordOS apps, to Linux apps, **from Linux apps** (`takeDrop`); VM 127 with Thunderbird and Chromium, Wayland and X11 | M | 3–4 |
| 5 | Places (Home, Trash, drives, network) and launchers (Add to Desktop, drag from the launcher/start menu), `.desktop` matching | M | 3 |
| 6 | **Workspaces and Celeste** (obstacles, looks, activities), several displays and mixed DPI on the maintainer's three monitors, `theme.json` `desktopIcons` | M | 2–3 |
| 7 | Undo shared with Files (`sdk/fileundo.js`); Quick Preview lifted to the SDK for Space on the desktop | M | 3 |
| 8 | Release work: paper and site (features page, FAQ), screenshots in the four themes, Claude Security, the performance scene before/after | S–M | 1–2 |
| | **Total** | | **25–34** |

Phases 0–4 and 6 make a complete feature; 5 and 7 can follow in a later release if time is short. Phases 0 and 1 are
worth shipping on their own (Files gets folder watching).

## 10. Decisions for the maintainer

| # | Decision | Recommendation |
|---|---|---|
| D1 | Default per theme, and whether existing users see icons after the update | On in Flow and Classic, off in Workspaces and Celeste; existing users get the same defaults (no migration), since nothing showed before and most Desktop folders are empty; a "What's new" line points to the switch |
| D2 | Which places show by default | Flow: drives; Classic: Home and Trash; Workspaces and Celeste: none |
| D3 | Arrangement modes | "Keep arranged" by default, plus "arranged by hand" on the grid; no free pixel placement |
| D4 | Enter on the desktop | Rename, as in Files, in every theme (one rule); F2 also renames; ❖↓ and double click open |
| D5 | Context menu order | SDK items first (New Folder, Paste, Sort By, Show in Files), then the theme's own, then Desktop Settings |
| D6 | Where positions are kept | In `settings.json` with the caps of 5.4 (nothing new on the deny list); move to a separate unprotected file only if measurements show `settings.json` writes getting slow |
| D7 | Undo | Build it shared with Files (phase 7), not desktop-only |
| D8 | Several displays | Main display only by default; "every display" spreads one set of icons, never duplicates it |
| D9 | Desktops and activities | The same icons on every desktop and activity; positions per display only |
| D10 | Workspaces' empty-activity card with icons on the display | Keep the card where it fits (it is an obstacle); hide it only when icons fill the grid |
| D11 | Thumbnails | On by default for images; videos and PDFs only with Files' own preview setting on |
| D12 | `.desktop` matching | By file name against installed ids only (never the file's contents); confirm that "Add to Desktop as a launcher" may offer to move the original file to the Trash |
| D13 | `lord.fs.watch` | Approve the read-only API as specified in 5.1 (16 watches, coalesced, no names, not through the broker, no FUSE) |

## 11. Open questions

- **Q1.** Does the UI receive pointer moves while the compositor holds a Linux app's drag (for the folder highlight
  during a Thunderbird drag)? To be checked on VM 127 in phase 4; the drop itself works either way.
- **Q2.** Taking the keyboard away from a Linux window when the desktop is clicked (3.1): confirm in phase 3 that the
  compositor's keyboard focus follows `focusedOn` returning null with no other side effect (text-input, IME, the
  activation token of the next launch).
- **Q3.** Get Info for a desktop item opens Files; Files needs a window argument to open straight on Get Info for a
  path (`args.info`). Small, but it is a change to Files' window arguments.
- **Q4.** Touch: drags are not supported by the compositor for touch (`docs/WAYLAND.md`); on a touch screen the
  desktop would offer tap to select, double tap to open and long press for the menu only. Out of scope unless asked.
