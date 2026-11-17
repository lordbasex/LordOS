# Lord Workspaces

**In development** after 0.8.8.9 Beta (approved by the maintainer on 2026-11-15). Lord Workspaces (`themes/workspaces`)
is LordOS's third desktop theme, next to Lord Flow (`themes/default`) and Lord Classic (`themes/classic`), and it
changes neither of them. It organizes the desktop by **activities**: each activity is a desktop with a name, an icon,
a colour, the apps pinned to it and a tiling layout for every display, and its windows sit in a group frame. It came
from a proposal called "Origin Flow"; this document says what was built, what changed from the proposal, how it
performs against the other two themes and what is left for the compositor.

## What it looks like

| Piece | What it is | Where |
|---|---|---|
| **Context Spine** | The rail at the start of the main display: the LordOS crown (the system menu: About, Settings, Lock, Switch User, Log Out, Restart, Shut Down), **Activities** (the Overview), **Apps** (the search panel with every app), **Files**, and at the bottom the **Control Center** and the button that collapses it to icons. It collapses by itself below 1440 px while its setting is Automatic. It reserves its width in the work area. | `components/Spine.jsx` |
| **Activity strip** | A pill per activity (its icon in its colour; the one in view under an accent that glides to the next one on a switch), **+ New**, and "Activity 2 of 4 · up to 9". Click switches, double click renames in place, right click: Rename, Icon, colour and apps…, Delete activity… (asked first, in the page). | `components/TopBar.jsx` |
| **Status cluster** | Privacy indicators, the status items (network, Cleaner, Cloud, keyboard…), the volume, notifications with an unread dot, and the date and time (the clock wakes once a minute). | `components/TopBar.jsx` |
| **Group frame** | Per display, a frame in the accent around the work area; its header holds the activity's name (and the display's, with several), its pinned apps and the actions **Change layout**, **Add window**, **Save layout** and **Move to another activity**. With no window on a display, a short empty state offers Add window and Open pinned apps. | `components/GroupFrame.jsx` |
| **Windows** | A 36 px title bar (icon and title at the start, minimize, maximize and close as real 28 px targets with a glyph, at the end or first with Settings → Desktop & Dock → Title bars → Left). An app's own menus in a strip under it with Window and Help (there is no global menu bar; F10 or the left ⌥ alone opens the first one). Linux apps that draw their own bar (`args.csd`) get no frame and the theme's window menu where they ask for one. Right click on a title: Tile or Float this window, Move to another activity, Minimize, Maximize, Close. | `components/Window.jsx` |
| **Search** | ❖Space, Ctrl+Space (not in the Terminal) or ❖ alone, on the display under the pointer, in the overlay layer (a Linux app doesn't keep the keyboard). LordOS's own search (`searchEverything`: apps, files, Settings, notes, the calculator, Help, the Store) plus two sections this theme registers, **Activities** and **Open windows**. Typing starts at the first character; ↑ ↓ choose, Enter opens, Esc closes. With nothing typed it lists the activities and every app. | `components/Search.jsx`, `index.jsx` |
| **Control Center** | A live drawn preview of the desktop with the current choices; dark mode, Reduce motion, the **effects style**, the accent, Do Not Disturb, Night Light, Wi-Fi (its state; a click opens Settings → Network → Wi-Fi), Bluetooth, the volume and the screen's brightness, the power mode and the wallpaper. It unfolds from its button. | `components/ControlCenter.jsx` |
| **Onboarding** | On the only activity, while nothing was set up and nothing is open: 1. its name, icon and colour; 2. its apps and the layout. "Start activity" opens the apps, which the layout places. It can be skipped; the same two steps edit an activity later. | `components/ActivityEditor.jsx` |

The menus and the notification panel are Lord Classic's own components (`themes/classic/components/Menu.jsx`,
`NotificationCenter.jsx`), restyled by this theme's tokens; the window glyphs are Classic's too. The Overview, the app
switcher, the power dialog, the level HUD and the SDK's own surfaces (notifications, prompts, the file chooser…) are the
SDK's, as in every theme.

## Activities

An activity **is** one of the SDK's desktops (`sdk/desktops.js`, at most 9), so the Overview, Settings → Desktop & Dock,
session restore, a theme switch and the rule that a window on a hidden desktop never has the keyboard all work as in
the other themes. Its name is the desktop's own (`settings.desktopNames`, `renameDesktop`). What a desktop doesn't have
is kept in a new settings key, versioned, by position like the names (desktops are numbered again at every start):

```
settings.workspaces = { v: 1, activities: [ { icon, color, pins, layouts, setup } | null, … ] }
settings.themeOptions.workspaces = { surface, spine, spineEvery }
```

`layouts` is per display (by connector name): `{ layout, ratio, apps }`. A new key, so no migration
(`sdk/settings.js` lists both with "new key, no migration"). The model is `model/activities.js`.

## Tiling

Layouts (`model/layouts.js`), chosen per display: **Free** (windows stay where they are; the default), **One at a
time**, **Two halves** (two columns; more windows stack in rows, alternating sides), **Main and side** (a main tile and
a column of up to three; it keeps three slots, the side split in two, free ones shown), **Three columns** and
**Grid** (four slots at least, two by two). The tiler (`tiler.js`) writes the SDK's
window store once for every window that moves (the same fields `wm.setFrame` writes) when something changes: a window
opens, closes, is minimized or comes back, the layout or its divider changes, the activity in view changes, the
displays do. Nothing runs per frame, nothing polls.

- **The divider** of Two halves, Main and side and Three columns sits in the gap between the tiles. Dragging it shows a line in the
  drag layer, moved with `lord.setStyle` (no React render per pointer move); the tiles change once, on release.
- **The magnet (Free layout, `magnet.js`, `model/magnet.js`):** a window dragged to the left or right edge of a display
  takes that half (the SDK's snapping, `configureWindows({ snap: true })`). If one other window of that activity is on
  the display, it takes the other half by itself; with several, a chooser over the empty half lists them (this
  display's first; a click snaps it there; Escape or a click outside closes it). The two windows are then a pair:
  resizing one by the shared edge moves the other's edge to it on release, never below its minimum width. Moving
  either one, minimizing, maximizing or closing it ends the pair. The magnet writes the window store only for the window
  that follows, and a pair formed in one change is judged from the next.
- **The padlock** at the end of each frame header locks that display of the activity (`layouts[connector].locked`,
  kept by Save layout). Locked, the header shows only the padlock, the divider can't be dragged, and the display's
  windows lose their maximize and close buttons, their window menu, title-bar drag and resize edges (a small padlock
  takes those buttons' place). Minimize stays: a window minimized on a locked display leaves its slot, and the app
  brought back or opened there takes it, so the layout holds while the apps in it change. The SDK refuses the person's
  other ways too (`configureWindows({ locked })`, `isWindowLocked`): `wm.beginMove`/`beginResize`, `move`, `resize`,
  `toggleMaximize`, `snap`, `snapKey`,
  `center`, `arrange`, a Linux app's move or resize request, and `close` unless forced. What a program does still
  applies: the tiler, a forced close, an app closing its own window (a Linux app's own close button included). New
  windows may open on a locked display and take their tile.
- **The frame header** names the display by its model, plus its connector when two displays share the model
  ("S27F350 · DP-2").
- **Tiles are slots** (`model/layouts.js` `slotStacks`). A window minimized while in a tile keeps its slot: the other
  windows stay where they are and the slot shows as a dashed free space with Add window. A window that opens takes
  the first free slot of its display; a window brought back from minimized takes its slot again, and the window shown
  there meanwhile is minimized (they swap). Closing a window gives its slot up. A window minimized before the tiler
  placed it gets no slot.
- **Drag to rearrange** (`dropInto`, `slotAt`). A tiled window dragged with its size kept (or snapped to an edge) onto
  a display with tiles takes the slot under the drop: on its own display it swaps with the window there; on another
  display it gets a slot of its own there and leaves its old one. Dropped on a free slot (a minimized window's place)
  it takes that slot and its own goes, so a half stays one half (`dropSlot`); the minimized window waits behind it and
  comes back by swapping. Dropped on a display in Free, it floats there.
- **Where a window will land** (`components/SnapTargets.jsx`, Lord Workspaces only). While a window is dragged over a
  tiled display, a glass rectangle shows the slot it will take (`slotPreview`, read from the SDK's `moveGrab` a few
  times a second, one render per change of slot); the drop takes that slot whatever the window's centre (`dropHint`).
  Over a free display the SDK's snap preview shows in the same glass. The glass is the Crystal material's blur over
  LordOS's own surfaces, a tint over Linux apps (the renderer doesn't blur their pixels on the GPU yet), a 1 px inner
  light and the accent edge; it grows from the pointer in 150 ms, still under Reduce motion.
- **Snap thirds**: resting on the left or right edge for 0.5 s turns the half into a third (`configureWindows({
  snapThirds: true })`, `sdk/snap.js` `dwellZone`; off in Lord Flow and Lord Classic).
- **The layout flyout**: resting 0.45 s on a window's maximize button opens halves, thirds, main and side, and quarters
  under it. A click puts the window in that zone: on a free display the SDK's snap zone (a half also wakes the magnet),
  on a tiled display that layout is set there and the window takes that slot (`placeInSlot`). Not on locked windows,
  and not on the buttons Linux apps draw themselves (Chrome): for those, **Snap window…** in the window menu (a right
  click on Chrome's tab strip, or on any title bar) opens the same flyout at the pointer.
- **Widgets** (`sdk/deskwidgets.jsx`, `sdk/widgetgrid.js`, `components/WidgetPicker.jsx`; Lord Workspaces only). Each
  display's work area holds a grid of 170 px cells with 16 px gaps, anchored to its top right (TangoOS's grid). A widget
  takes a block of cells by its size: small 1×1, medium 2×1, large 2×2, wide 4×1, extra large 4×2; no other widget can be placed
  or dropped over it. Widgets belong to an activity and a display (its connector): another activity shows its own, or
  none. A display with widgets but no window isn't empty: the empty state's card is left out there. The "+" in the
  frame header is a menu (Add window, Add widget, Add web app, the last opening the Store's Web apps page); the
  wallpaper's right-click menu starts with the same three. **Add widget** opens the catalog over that display, with a
  button per size; the widget goes on the first free block from the top right ("no room" says so). Dragged anywhere, a
  dashed block shows where it will land, the nearest free one; its right-click menu changes its size (where it fits),
  its options (the clock: analog or digital, cities from the Clock app) and removes it; it opens inside the
  widget's display. Kinds and sizes: Clock (s
  analog face or digital, m with world cities, l, w four cities in a row), Calendar (s today and the next event, m the next
  three, l the month), System (CPU and memory as tick rings), Network (rates; m with the last minute's columns),
  Disks, Temperatures and fans, Battery (only with one), Reminders, Clipboard, Claude Code, Now playing and Read aloud;
  the Notes app's sticky note stays a free note of its own. The look is TangoOS's: 22 px corners, the Crystal blur
  (none in Opaque), 12 px headings, large figures. Clocks move once a minute (no seconds hand: no timer every
  second), and every reading runs only while its widget is shown. Kept in `settings.deskWidgets`
  (`[{ id, type, size, desk, dk, col, row, opts }]`).
- **Weather** (approved by the maintainer on 2026-11-16; s, m, l). The only widget that uses the internet, and only once
  the person presses **Turn on** on it (`settings.weather.on`, off by default; its right-click menu turns it off and
  picks °C or °F, by default the region's). The desktop names a time zone (its own and, at l, three of the Clock app's
  cities); lord-server takes that zone's coordinates from tzdata's `zone1970.tab` (no GPS, no address, nothing typed)
  and, through its own connector (`lordos:weather`: its own scope, firewall row and rules), opens a socket to
  `api.open-meteo.com:443` only. The sandboxed `lord-fetch` speaks TLS on it, sends one fixed GET built from the two
  numbers, and parses the answer strictly into numbers (`server/src/fetch/weather.c`: temperature, feeling, WMO code,
  day or night, six days of code, high and low; anything missing or out of range refuses the whole answer). lord-server
  checks those numbers again and keeps each zone's forecast 30 minutes (a failure waits 10; at most 8 zones). JS sees
  only `lord.weather.request(zone)` and `lord.weather.get(zone)`, in the desktop's own context (never an isolated app).
  The card shows a sky gradient by condition and time of day, the city, the temperature, a drawn icon, high
  and low; m adds the next five days, l the week with range bars and three cities. Tests: `tests/weather_test.c`
  (path and parsing), `tests/weather_model_test.mjs`, and the scene checks that nothing is asked while it's off.
  **Location** (0.8.8.10): the widget's menu has "Location…", a search over an offline list of cities
  (`assets/geo/cities.tsv`, GeoNames' cities15000, CC BY 4.0, credited in About; refreshed before each release by
  `tools/geo/fetch-cities.sh`). Searching asks nothing of the network (`lord.weather.cities(query)`, lord-server reads
  the file: `src/fetch/geocities.c`, `tests/geocities_test.c`). The pick is kept as `settings.weather.city` {id,
  name, cc}; the widget then asks for `geonames/<id>`, and lord-server takes that city's coordinates from its own
  list, never from JS. "Automatic" goes back to the time zone's.
- **VPN** (s, m, l). The VPNs set up in Settings → Network → VPN (`lord.net.vpnList()`), turned on and off with the
  same closed actions Settings uses (`net-vpn-up` / `net-vpn-down`, open to the session's user): no new privilege. A
  widget drives one VPN (`opts.vpn`, chosen from its right-click menu; with a single VPN it is that one), so several
  VPN widgets can sit side by side, one per VPN. Small: the name, a switch with relief (a sunken track, a raised knob,
  ON/OFF) and the state or the address; medium adds the address and the traffic of that VPN's own interface; large
  lists every VPN with a switch each. State, address and traffic come from the interface in `lord.net.interfaces()`
  every 3 s while shown (`sdk/vpnmodel.js`, `tests/vpn_model_test.mjs`). When the VPN asks for a password, a single
  sign-on or a certificate, or fails, the widget offers **Open in Settings**, where those questions are answered.
- **Timer** (s, m), **Recent files** (m, l), **Updates** (s, m) and **Apps** (s, m, l), the rest of the widget plan:
  the timer and stopwatch are the Clock app's own (`sdk/alarms.js`: they keep running and ring with the widget gone;
  a tick each second only while one runs); recent files are `lord.fs.recent` (Files' Recents), opened with their
  app; Updates reads the signed lists' last check (`lordosUpdate`, `storeUpdates`, `restartNeeded`: nothing is
  fetched) every 10 minutes and opens the Store's Updates page; Apps launches 4 or 8 apps, the Dock's pins until
  chosen from its menu (`opts.apps`).
- **Video** (m, s, l, xl): a camera, a stream or a TV channel played by `lord-stream` (docs/MEDIA.md, "Network streams"):
  an address (RTSP, RTMP, SRT, RTP, UDP, HLS, DASH, MJPEG, HTTP) or a channel of an M3U list, set from its menu or
  its card. Muted by default; pause closes a live stream (no bandwidth, nothing decoding); Reconnecting while a lost
  stream comes back. Small and medium ask a camera for its sub stream where the address says how. The password of an
  address goes to the keyring (`opts` keeps `{ url, user, name }`).
- **Claude Code** (s, m, l; `sdk/claudewidgetmodel.js`): every session of every account — `~/.claude` and any
  `~/.claude-*` folder (another account through `CLAUDE_CONFIG_DIR`), the account named from that folder's
  `.claude.json` (name and plan only). Rings for the context, the plan's 5 hours and the week, with when each renews
  (small: the context ring and two bars). At 70 % of any of them the card glows amber, at 85 % red, pulsing slowly
  (a timer only while it glows, no sound). Claude Code reports no daily figure: the 5-hour window is the closest.
  An account whose status line isn't connected to the Terminal shows its sessions' state only.
- **Sound wave** (w, m, l, xl; `sdk/soundwavemodel.js`, drawn in C by `<SoundWave>`, docs/SOUND.md): glowing lines that
  move with the sound of the desktop, one app (by name) or the microphone, one source at a time. Transparent by default
  (`opts.frame` `'none'`, no card: part of the wallpaper; only while dragged a dashed outline shows) or on a card that
  names the source. Colours, lines and sensitivity from its right-click menu. Repaints only its box, only while there
  is sound.
- **Pinned widgets** (`opts.pin`, `widgetgrid.js` `PINS` and `pinnedRect`): any widget's right-click menu has **Center
  at the bottom**, which keeps it centred along the bottom of its display's work area, off the grid (it holds no cells
  and may sit over another widget). Dragging it puts it back on the grid. A new sound wave starts pinned there.
- **Apps with their own frame** (Cleaner) draw no lights in Lord Workspaces: the theme's buttons at the end are the
  only ones, as in Lord Classic.
- **A window resized by hand** (or maximized) leaves its tile and floats until **Tile this window** from its title
  bar's menu, or until a layout is chosen again for the display it is on now (a floating window belongs to the display
  it is on, wherever it was tiled before). Dialogs, notices and the theme's own panels never tile.
- **A window whose minimum size is bigger than its tile** keeps its minimum at the tile's corner, inside the work area
  (it overlaps its neighbour instead of being cut), and its title bar shows an amber dot ("needs more room than its
  tile").
- **An app that answers with another size** (Google Chrome's minimum size, a terminal's character grid) at the tile's
  corner within 1.5 s keeps that size and stays in its tile; it is asked again only when its tile changes
  (`model/layouts.js` `judgeFrame`). A resize by hand later than that floats the window as above.
- **Bounded:** a pass with nothing new moves nothing (a second pass is a no-op); at most 10 moves of one window and 60
  passes a second, past which the pass waits a second (`underBudget`, `MAX_PASSES` in `tiler.js`). A display is keyed
  by its connector, never its model's name (three monitors of one model share it), and two keys are never alike
  (`displayKeys`). Tests: `workspaces_model_test.mjs` and `workspaces_scene.mjs` (idempotent passes, an app that
  answers every size with another one).

## Several displays

The maintainer's main case is one activity spanning all his monitors (three 1080p displays daily), and that is the
default:

- **One activity spans every display.** The SDK's desktops are global: switching activity switches all the displays
  together. Each display has its own group frame, header, layout and divider, computed in its own work area; a window
  belongs to one display and never straddles two in a tiled layout.
- **Save layout remembers each monitor.** For every display, by its connector name (`DP-1`, `HDMI-A-1`…), the layout,
  the divider and the app of each tile in order. When those apps open again (a new session, session restore, the
  pinned apps) their windows go back to that display and that tile.
- **Fewer monitors.** A saved display that isn't connected has its apps joined to the main display's tiles
  (`foldMissing`). **A monitor unplugged:** the SDK moves its windows to the main display (`keepWindowsInWorkAreas`),
  where they join the tiles; **plugged back:** windows with a saved place on it go back there. Nothing is closed or
  lost. `server/tests/workspaces_scene.mjs` checks this with three displays.
- **The spine** is on the main display, or on every display (Settings → Desktop & Dock, or Appearance). The strip and
  the status cluster are on the main display. **Search, the Control Center and Add window open on the display under
  the pointer** (or the one whose button was used).
- **Mirrored displays** show one picture, laid out for the main display.

What needs SDK or compositor work, not the theme:

- **A different activity per monitor** (secondary in the request): the SDK has one active desktop for every display
  (`activeDesktop`, `visibleOn`, `keyboardSurface`, the compositor's `parked` state), so per-display desktops need a new
  desktop model in the SDK and in `wl.c`'s parking, with the keyboard rule kept. Not built.
- **The Overview per display:** the SDK's Overview lays out the windows in the main display's work area; per-display
  grids are an SDK change (`sdk/overview.jsx`). Today the Overview is the SDK's, on the main display.

## Effects styles

A new theme setting, **Effects style** (`themeOptions.workspaces.surface`), in the Control Center and in Settings:

| Style | What it is |
|---|---|
| **Opaque** | Solid panels, no blur. The cheapest to paint. |
| **Translucent** | The classic look: a tinted panel that blurs what is behind it. |
| **Crystal** | The maintainer's own name for this look: the translucent panel plus light — a soft two-stop sheen from the top, a darker inner lower rim (the light bending at the glass's lower edge) and a depth shadow, tuned per light and dark. The default. |

Settings → Appearance → Transparency off (`settings.effects === false`) is always Opaque. Rules that keep it cheap:

- **Live blur at rest: the spine, the activity strip and the status cluster** (the last two since 0.8.8.10, when the
  engine began keeping a blurred backdrop while nothing behind it changes and repainting each damaged rectangle on its
  own). The header chips and the empty state use the same glass without a live blur (a thicker tint: `bgStill`);
  Search and the Control Center blur while they are open.
- **The bars' controls** (`components/BarButton.jsx`): like a video player's, each control of the spine, the strip and
  the cluster sits on its own pill that shows under the pointer (`itemHover`), glyphs in the spine's colour (`iconFg`,
  `iconFgHover`), thin lines between groups (`barSeparator`). An activity's icon keeps the colour chosen in Edit
  activity; status items in colour (an app's own icon) keep theirs.
- **No full-screen blur, no full-screen scrim.** The group frame is only a border: a fill over the whole work area was
  the costly part of an activity switch on the CPU path.
- **Clearances.** The engine repaints a blurred node and twice its blur radius around it whenever a change touches that
  margin, and with one damage box per frame (the default) everything else that changed joins it. The geometry
  (`model/geometry.js`) keeps every window, its resize grips, its shadow and the top band out of the spine's margin
  (checked in `workspaces_model_test.mjs`): measured before the fix, each frame of the strip's glide repainted the
  spine's whole height.

Gradients have two stops (what the renderer draws). The colours are the brand's violets (`#7C5CFF`, `#5B3FE0`; the
gradient `#B46CFF → #4F46E5` on the onboarding's primary button) with violet-biased neutrals, and the shared accent
names (azul, violeta, rosa, naranja, verde, grafito) so the person's accent survives a theme switch; in light mode each
accent is one step deeper. Contrast is checked in the model test: text on windows, panels and title bars ≥ 7:1, dim
text ≥ 4.5:1, white on every accent ≥ 3:1 (large text and controls) in both modes. The crown is the brand's
full-colour render, never recoloured, with no glow.

## Motion

Every transition changes a node's native transform or opacity with `lord.setStyle` from one shared frame loop
(`motion.js`): lord-server paints the subtree once into a cached raster and composites it, and React doesn't render per
frame. Durations are 120–260 ms; with **Reduce motion** (Settings → Appearance, and the Control Center) every one
jumps to its end, and the SDK's own desktop slide is off too.

- **Activity switch:** the SDK slides and fades the arriving windows (as in the other themes), then the accent under
  the strip's pills glides to the new activity with a small overshoot (a translate and a horizontal scale from its left
  edge). It starts after the windows' slide: in the same frames the two would be repainted as one box spanning both.
- **Spine:** each item answers the pointer with a spring (scale on its icon: up on hover, down on press).
- **Search** drops in from a little above and settles; **the Control Center** unfolds from its button.
- **Windows** open with a zoom from where they were launched (the spine, the search panel), and minimize into their
  display's tray of minimized windows (the frame header shows one icon per minimized window of the activity on that
  display; a click brings the window back where it was, its frame unchanged). A maximized window fills its display's
  work area inside the group frame, never the whole canvas of several monitors.

## Keys

The theme's table (`index.jsx`) on top of the SDK's base: ❖Space and Ctrl+Space open Search (the theme's entries win
over the base's), ❖ alone opens it with every app; ❖⌥D collapses or expands the spine; the app switcher (❖Tab by
default), ❖\` the app's next window, ❖M minimize, ❖H hide, ❖⌥H hide others, ❖, the app's settings, and Lord Flow's
Desktops keys (⌃↑ the Overview, ⌃← ⌃→ and ❖⌃← ❖⌃→ switch activities, ⌃↓ the app's windows). Over a Linux app the
compositor keeps Lord Flow's fixed lists (`flow-desktops`, or `flow` with Settings → Keyboard's ⌃ arrows off,
`server/src/wl/keygrab.c`): no new C table, so the keys it keeps are exactly those.

## Settings

Settings knows the two bundled themes by id; for any other theme it now reads three optional fields of its
`theme.json`, checked by `sdk/thememeta.js` (labels must be `shell.*` locale keys, values short plain strings,
anything else dropped):

- `sketch` (`rail`, `pills`, `frame`, `menubar`, `dock`, `taskbar`): the drawn picture in Appearance → Desktop style.
- `options`: the theme's own choices, under Desktop style and on Desktop & Dock while it is the current theme, stored in
  `settings.themeOptions[<theme id>]` (one key for every theme, so a theme's id can't reach another setting). Lord
  Workspaces declares Effects style, Context Spine (Automatic, With labels, Icons only) and Show the spine on every
  display.
- `desktopArrows`: the Desktops section shows Settings → Keyboard's ⌃-arrows switch, as for Lord Flow.

On Desktop & Dock, such a theme gets the desktops (here: activities), its options and the title bars' buttons, with no
Dock or taskbar section. Lord Flow and Lord Classic declare none of the three and see exactly what they did.

## Widget data

The desk widgets get their data from one feed (`sdk/widgetfeed.js`, pure logic in `sdk/widgetfeedmodel.js`, tested
by `server/tests/widget_feed_test.mjs`) instead of a timer each:

- **One read per source.** Each piece of data is a source with a key (`minute`, `cpu`, `sysinfo`, `net`, `net.ifs`,
  `claude`, `vpn.list`), read once however many widgets show it; a widget's own data (its disks, its VPN's rates, its
  recent files, the update status) is a source of its own.
- **The interval of the widgets that can be seen.** A widget subscribes with how often it wants the data
  (`useFeed(key, read, ms)`). The source runs at the shortest interval among its visible subscribers, and not at all
  when none is visible: no timer, no read, no render. Defaults: the clock and the calendar once a minute, CPU and the
  network rates every 2 s, memory every 4 s, the network interfaces every 8 s, the VPN list every 10 s, disks every
  30 s, Claude Code's sessions every minute, recent files every minute, updates every 10 minutes.
- **Aligned ticks.** A source's reads fall on multiples of its interval since the epoch, so the ones due in the same
  second are read together and their widgets re-render in one frame: the 2 s and 4 s sources tick with each other,
  and everything ticks on the minute with the clock.
- **Seen or not.** A widget wholly inside a window frame of the activity in view counts as hidden (a partly covered
  one is seen; the overview shows every widget). Widgets of other activities aren't mounted at all. A widget is
  shown again at once and hidden only after a second, so a window dragged across the desk doesn't stop and restart
  the sources under it. While nobody sees the screen (the displays off after idle, the session locked or switched
  away with no remote viewer: `lord.system.seen()`, the same rule the compositor uses to stop drawing) the ticks
  come but read nothing.
- **Last value, then fresh.** A widget that shows again keeps its last value and gets a fresh read at once if it is
  older than its interval. A rate (CPU, network) read after a time unseen is the average over that time; the network
  history carries on with a gap, nothing made up.
- **Same data, no render.** A read equal to the last (`sameValue`) notifies nobody.

## Performance

Measured on the development machine (Intel Core i9-10900, AMD Radeon RX 580; CPU path, `LORD_GPU=0`), a headless
1920×1080 `lord-server --profile` with a private home and runtime folder, the same scenario in each theme: three
windows (Text Editor, Files, Terminal), then 20 focus changes, 20 title changes and 10 activity (desktop) switches
between one with the windows and an empty one, then 10 s idle. A headless session paints only while someone watches,
so a screen recording (10 fps) was kept running during the scenario; paint times are `--profile`'s own. Lord
Workspaces in its default layout (Free), dark mode.

Medians of three runs each (2026-11-15); "paint" is the sum of `--profile`'s paint times over the phase, "screen" the
average share of the screen repainted per frame:

| Phase | Lord Flow | Lord Classic | Workspaces, Crystal | Workspaces, Opaque |
|---|---|---|---|---|
| First full frame after the recording starts | 63 ms | 61 ms | 63 ms | 59 ms |
| 20 focus changes: paint (frames), screen | 84.6 ms (23), 71 % | 94.5 ms (23), 77 % | **57.3 ms** (23), **28 %** | **56.7 ms** (21), **31 %** |
| 20 title changes: paint, screen | 39.5 ms, 24 % | 34.5 ms, 23 % | 37.7 ms, 22 % | 37.5 ms, 23 % |
| 10 desktop / activity switches: paint (frames), screen | 330 ms (47), 46 % | 344 ms (49), 44 % | 349 ms (104), 24 % | 364 ms (109), 25 % |
| 3 s idle with the recording on: frames | 4 | 4 | 4 | 4 |
| 10 s idle without a viewer: CPU ticks of `lord-server` | 16 | 16 | 16 | 14 |

The switches spread widely from run to run (Lord Flow 262–352 ms, Workspaces Crystal 217–397 ms): within that spread
the three themes cost the same. Workspaces draws more frames there (the windows' slide, then the pills' glide), each of
them smaller.

What a change costs in native calls (`server/tests/theme_cost_scene.mjs`, the stub `lord`, one display, three
windows; numbers, not checks):

| Change | Lord Flow | Lord Classic | Lord Workspaces |
|---|---|---|---|
| A window's title, ×10 | 10 | 10 | 10 |
| The window in front, ×10 | 160 | 290 | 210 |
| A window opened and closed | 206 | 247 | 287 |
| Desktop / activity switches, ×4 | 44 | 68 | 212 (352 in 0.8.8.10's first drafts) |
| Intervals alive at rest | 250 ms, 1 s (the clock), 2 s | 250 ms, 1 s (the clock), 2 s, 5 s (the network glyph) | 250 ms (the SDK's) |
| Timer wake-ups per minute at rest | 330 | 342 | 240 (+1 for the clock) |

(First render: 494, 385 and 1,736 calls; Workspaces' includes the first-run onboarding.)

What these say:

- **Focus changes** are the cheapest of the three: Workspaces repaints about a third of what Flow and Classic do,
  because nothing else on screen depends on the window in front (Flow's menu bar and Classic's taskbar change with it).
- **Idle:** no timer every second (the clock wakes at each minute's turn), and the only interval left is the SDK's own.
  At rest nothing is painted.
- **Activity switches** cost about the same as a desktop switch in Lord Flow and Lord Classic (within their run-to-run
  spread), with the glide added after the windows' slide; they don't beat them yet. Earlier drafts were several times as costly (scaling the display-sized group frame, a scrim
  over the work area, changes inside the spine's blur margin); each of those was measured and removed.
- **In native calls** a switch costs more than in Flow and Classic: the spine, the strip and the frame header show the
  activity in view (its icon, name, pins and layout), so they change with it. What changes is kept to styles: every
  activity's icon stays drawn in the header and only the one in view shows, the spine's activities are one keyed list
  (a switch moves two tiles and recolours their outline), a pill's window count is hidden rather than removed. That
  took a switch from 352 calls ×4 to 212, though the spine now also shows the recent activities.

## Changed from the proposal

- **Weather widget:** not yet; the shell asks nothing from the internet, and a reviewed path for Open-Meteo waits for
  the maintainer's OK (docs/ROADMAP.md).
- **"Seek" is Search and "Control Atlas" is the Control Center**, LordOS's own names.
- **The group doesn't move as a whole and the divider applies on release:** no `setFrame` loop per pointer move.
- **The group frame is a border, not a frosted panel,** and Crystal's live blur stays on the spine and on the open
  panels: a full-screen blur on the CPU path would repaint the whole screen with every change.
- **Live miniatures of hidden activities** are not possible yet (below); the Overview shows them as frames with the
  app's icon.

## Tests

- `server/tests/workspaces_model_test.mjs` (in `make test`): the geometry on one display and several, right to left,
  mirrored displays and the blur clearances; the five layouts, the divider, the minimum-size fit, the saved order and
  homes, a missing monitor folded into the main one; the settings key, pins, onboarding; theme.json's fields and
  `sdk/thememeta.js`'s checks; the three styles and the palette's contrast.
- `server/tests/workspaces_test.mjs` with `workspaces_scene.mjs` (in `make test`): the whole theme over a stub `lord`
  with three monitors — a frame per display, the spine on the main one, the work areas, no timer every second, split
  on one display and single on another, a window moved by hand floating and tiling again, Save layout, a monitor
  unplugged and plugged back, a new activity, Search on the display it opens for, Crystal and Opaque.
- `theme_cost_scene.mjs` (a measurement, not in `make test`): `node tests/workspaces_test.mjs theme_cost_scene.mjs
  <default|classic|workspaces>`.
- On screen: headless sessions on the development machine, screenshots in light and dark × Opaque, Translucent and
  Crystal, Search, the Control Center, the empty state, the onboarding and the layouts. **Not yet checked** on a real
  session with real monitors, with a Linux app's own bar, in a right-to-left language, or on the GPU path.

## Deferred: compositor and SDK work

- **Live miniatures of hidden activities** in the Overview and the strip: hidden desktops' windows aren't painted (a
  Linux window there is parked), so the compositor would need to keep and scale their last buffers.
- **Real frosted glass over Linux apps on the GPU path:** the theme's UI is painted on the CPU and the GPU only
  composites; over a Linux app's window a blur only tints. A GPU blur pass is compositor work.
- **Alt+Space** (and other keys over Linux apps): the compositor's key lists are fixed C tables
  (`server/src/wl/keygrab.c`); Workspaces reuses Lord Flow's. A table of its own is a reviewed change.
- **Weather**: a widget waiting for a reviewed network path (docs/ROADMAP.md).
- **A different activity per monitor** and **the Overview per display** (above): SDK and compositor changes.
- **Damage in several rectangles** (`LORD_DAMAGE=multi`, off by default): with it, a small change far from the spine
  would never join a large one; the theme already keeps its moving parts out of the spine's margin.
