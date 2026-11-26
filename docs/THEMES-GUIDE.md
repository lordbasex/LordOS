# How to build a LordOS appearance (theme)

This guide is for a React developer who has never seen LordOS. It explains what a theme is, what it
must provide, the SDK pieces it builds on, how it is built and loaded, and what to check before
sharing it. Everything here is checked against the current code; file references point to where
each piece lives. A small working starting point is in [`examples/theme-minimal/`](../examples/theme-minimal/).

Read first, if you can: [`themes/README.md`](../themes/README.md) (the short reference: components,
events and the supported style subset), [DESIGN.md](DESIGN.md) (tokens and the shared controls),
[LAYERS.md](LAYERS.md) (stacking order) and [WINDOWS.md](WINDOWS.md) (moving and resizing).

> **A theme you make runs on a normal LordOS as a community theme** (in development after 0.8.9.6.2): package its
> source as a `.zip` and install it from Settings → Appearance → Install from a File… LordOS checks it, builds it
> itself and runs it with a restricted API ([Community themes](#6-community-themes-installed-from-a-file)). Copying a
> built bundle next to LordOS's own themes still only works on a development build
> ([Verified code](#verified-code-what-happens-to-a-theme-that-lordos-did-not-sign)).

## 1. What a theme is

A theme is **the whole desktop, written in React**: wallpaper, menu bar or taskbar, Dock, app
launcher, panels, menus and every window frame. It is not a style sheet and it does not run in a web
engine. `lord-server` (the display server and Wayland compositor, in C) evaluates the theme's bundle
with QuickJS; a React reconciler (`runtime/index.js`) turns the tree into native nodes
(`lord.setStyle`, `appendChild`…) that `lord-server` lays out (flexbox) and paints itself, on the
CPU or the GPU. So you write ordinary function components with hooks, but with `<View>`, `<Text>`,
`<Image>` instead of DOM elements, and a CSS subset as React Native-style style objects
(`themes/README.md`, "Supported styles").

**What the SDK (`@lordos/sdk`, `sdk/`) gives a theme:**

- the native components and events (`View`, `Text`, `Image`, `Terminal`, `addListener`, gestures);
- the window manager: the window list, open/focus/minimize/maximize/snap, the work area, moving and
  resizing done in C (`sdk/windows.js`, `sdk/windowshell.jsx`);
- `AppHost`, which draws an app inside a window; the native apps themselves come from `apps/`
  (`@lordos/apps`) and Linux (Wayland and X11) windows arrive on their own (`sdk/wayland.js`);
- the token contract and the shared controls apps use (`ThemeProvider`, `useTheme`,
  `sdk/theme.js`, `sdk/components.jsx`, `sdk/controls.jsx`);
- the layers that order everything above the windows (`Layer`, `Overlay`, `sdk/layers.js`);
- the models behind the desktop pieces, without their look: the Dock and taskbar (`sdk/dock.js`),
  app menus and the Window menu (`useAppMenus`, `appMenuItems`, `windowMenuItems`), notifications,
  status items, search, sound, battery, Bluetooth, Night Light, displays, the clock, translations;
- the surfaces that look the same in every theme, placed by `startShell`: search, notification
  banners, permission and keyring prompts, the file chooser, the print dialog, the input method's
  candidate window, fullscreen and picture-in-picture windows.

**What the theme must provide:** the colors and shapes (tokens), the window frame, the desktop
around the windows (bars, Dock, launcher, panels), where menus open, and the keyboard shortcuts of
its own layout (for example what the Super key alone does).

**The built-in themes are the references:**

| Theme | Folder | Layout |
|---|---|---|
| Lord Flow | `themes/default` | menu bar on top (with the app's menus), magnifying Dock, app launcher, Control Center |
| Lord Classic | `themes/classic` | taskbar (bottom, top or a side), start menu with search, quick settings, notification panel, menus in each window, snapping and the SDK's Overview |
| Lord Workspaces (since 0.8.9) | `themes/workspaces` | activities (desktops with an icon, a colour, pinned apps and a layout per display), a Context Spine, group frames with static tiling, its own Search panel and Control Center, menus in each window, three effects styles ([WORKSPACES.md](WORKSPACES.md)) |
| Lord Celeste (community, since 0.8.9.5) | `themes/celeste` | a frame around each display with rounded inner corners, a bar on its thick side (running apps, status items, privacy indicators, clock), drawers that slide out of the frame's edges (dashboard, launcher, session, sidebar with the notifications), menus in each window, mirrored right to left; the theme options of `theme.json` and a node-tested geometry (`model.js`). Layout inspired by Caelestia (caelestia-dots/shell); no code from it |

`themes/greeter` is the login and lock screen: it is a separate program, not a desktop theme (see
[The login and lock screen](#the-login-and-lock-screen-are-not-your-theme)).

**Switching.** The user switches themes live from **Settings → Appearance → Desktop style**, which
since 0.8.5 lists every installed theme (the only place to choose one). It is a gallery
(`apps/settings/ThemeGallery.jsx`, with its pure parts in `sdk/themegallery.js`): a search field
(name, description, author and id, without accents), the filters All / Official / Community /
Installed and a grid of cards that wraps with the window; the arrow keys move over the cards and
Enter opens one. The official themes come first (Lord Flow, Lord Classic, then by name), then the
community ones by name; a theme with `"hidden": true` in its `theme.json` is not listed. A card opens
the theme's page: a larger picture, its description, author, license and version, its own options
and **Apply**. A theme that couldn't start in this session (lord-server went back to the one before,
`ui.theme_failed` and `ui.theme_failed_why`, read by `lord.themes.list` as `failed` and `failedWhy`)
says "Couldn't start — back on ‹the theme in use›" with **Report**, which opens the GitHub bug form in
the browser with the theme's id and the error filled in; nothing is sent until the person submits it
there. Switching happens live, in the same session: it never logs out. `lord.themes.use`
(`server/src/ui/js_sys.c`) first reads the new bundle through the verified-code check while the running theme still
runs (`js_theme_prepare` in `server/src/ui/js.c`); a refused or unreadable bundle is answered with `false` at once,
nothing is discarded, the choice goes back to the theme in use and a notification says the style couldn't be applied
(`sdk/themeswitch.jsx`). When it is accepted, `themes.use` (`sdk/index.js`) puts up a short overlay over the whole
desktop ("Changing to ‹name›…", with a ring), `lord-server` draws two more frames so it is on screen, and then
`switch_theme` (`server/src/main.c`) discards the JavaScript context and starts the new one from exactly the bytes
that were checked (`js_init_prepared`); the new theme's first frame replaces the overlay. The LordOS app windows that
were open (up to 8) reopen in the new theme with their frames, and Linux windows stay connected and get the new frame.
If the new theme throws while it starts, `lord-server` starts the theme that was running again from the bytes
verified when it was loaded (`js_init_previous`), not from its file, which a package update may have replaced under
the session; its `startShell()` puts the choice back and shows the same notification. The choice is stored as
`theme` in `~/.config/lordos/settings.json`. `server/tests/theme_switch_headless.sh` switches round the four desktops
in one headless session with a Wayland client connected.

How a session starts: `lord-sessiond` always starts `lord-server` with
`--theme <LordOS folder>/themes/default/theme.bundle.js` (`server/src/remote/sessiond.c`,
`sess.c`), and `lord-server` starts the theme the user chose instead (`chosen_theme` in `server/src/main.c` reads
`theme` from the settings), so no theme is loaded only to switch; if the default one did start anyway, its
`startShell()` switches to the chosen one. If the chosen theme can't be loaded, the session starts in the default
theme (then Lord Classic, then the recovery screen) and its `startShell()` puts the choice back and says why.

## 2. Anatomy of a theme

```
themes/<id>/
  theme.json       name, description, hidden, community, author, license, version, sketch, options…
  index.jsx        the entry point: puts the desktop together, ends with startShell(<App />)
  tokens.js        the colors and shapes (by convention; the bundled themes use makeTheme())
  components/      the frame, bars, Dock, menus… (by convention)
  assets/          images the theme loads (<Image src="assets/…">)
  theme.bundle.js  the build output (generated)
```

The theme's **id is its folder name**: letters, digits, `-` and `_`, at most 64 characters
(`valid_id` in `js_sys.c`). A folder is listed as a theme when it holds a readable
`theme.bundle.js`.

### `theme.json`

```json
{ "name": "Minimal", "description": "One sentence for Settings.", "hidden": false }
```

- `name` and `description` are what Settings shows, unless the `shell` catalog has
  `shell.theme.<id>.name` / `shell.theme.<id>.description` (the bundled themes translate theirs
  that way; `themes.list` in `sdk/index.js`). A missing or invalid `theme.json` shows the id.
- `hidden: true` keeps it out of Settings (the login screen uses it).
- It is read as text of at most 4 KB. It is data, not code, so it is not in the code manifest.
- (0.8.9) Three optional fields for Settings, which knows only Lord Flow and Lord Classic by id. `sdk/thememeta.js`
  checks them and drops anything else (labels must be `shell.*` locale keys, values short plain strings):
  - `"sketch": { "rail": true, "pills": true, "frame": true }` (also `menubar`, `dock`, `taskbar`): the drawn picture
    in Appearance → Desktop style.
  - `"options": [{ "key", "label", "hint", "choices": [[value, label], …] | "toggle": true, "default" }]`: the theme's
    own choices, shown under Desktop style and on Desktop & Dock while it is the current theme, stored in
    `settings.themeOptions[<theme id>][key]`; read them with `themeOptionValue` (`@lordos/sdk`).
  - `"desktopArrows": true`: the Desktops section offers Settings → Keyboard's ⌃-arrows switch, as for Lord Flow
    (the theme then uses `flowDesktopKeys` and `flowGrabs()`).

  Lord Workspaces' `theme.json` uses all three.
- Four optional fields that Settings → Appearance shows on the theme's card and page (checked by
  `cleanThemeMeta` in `sdk/thememeta.js`; an invalid value is dropped):

  | Field | Value | Shown as |
  |---|---|---|
  | `community` | `true` for a theme LordOS didn't make | the badge **"Community · unofficial"**, the Community filter, and a note that LordOS didn't make or review it |
  | `author` | plain text, one line, at most 80 characters, no control or bidi-override characters | the author; missing, invalid or the placeholder `"author pending"` reads **"Unknown author"** |
  | `license` | an SPDX expression: `"MIT"`, `"GPL-3.0-or-later"`, `"MIT OR Apache-2.0"` (up to four terms joined by `OR`, `AND` or `WITH`) | the license; missing reads "Not stated" |
  | `version` | `1`, `1.2`, `1.2.3` or `1.2.3.4`, with an optional `-beta.1` or `+build` suffix | "Version 1.2.3" |

  The official themes (no `community: true`) declare none of them: Settings shows LordOS as their author, MIT as
  their license and the release's version as theirs. A community theme should declare all four:

  ```json
  { "name": "Aurora", "description": "Soft colors and a floating dock.",
    "community": true, "author": "Ana Pérez", "license": "MIT", "version": "1.0.0",
    "sketch": { "dock": true, "menubar": true } }
  ```

  `community` is the author's own statement for now: an installed community theme still needs its bundle in the
  code manifest to run (below). From stage 2 of the themes plan, a theme installed from a file is a community
  theme whatever it says.
  Credit any work a community theme was inspired by in `THIRD_PARTY.md` (Lord Celeste is an example).

### `tokens.js`: the contract with the apps

Every app reads its colors with `useTheme()` (`sdk/theme.js`), so it looks right in any theme. The
theme passes its tokens to `<ThemeProvider tokens={…}>`; whatever it leaves out falls back to
`DEFAULT_TOKENS` (Lord Flow's dark colors), and its own extra keys pass through untouched. Isolated
apps (their own processes) receive the same tokens through the broker, and Linux apps get the mode,
the accent and the title-bar button order through the settings portal (`publishTokens` and
`syncPortal` in `sdk/theme.js`). So the tokens must be plain JSON values (no functions).

**Base tokens (required in practice: apps use all of them):**

| Token | Meaning | Used by |
|---|---|---|
| `mode` | `'dark'` or `'light'`: apps pick icons, shadows and contrast from it | 67 places in `apps/` |
| `accent` | the accent color (`#rrggbb`): selection, focus, toggles, primary buttons | 118 |
| `text` | primary text | 281 |
| `textDim` | secondary text: labels, hints, captions | 446 |
| `windowBg` | the window's content background | 28 |
| `sidebarBg` | sidebars and secondary panes | 17 |
| `separator` | hairlines and borders between areas (usually a translucent `rgba`) | 101 |
| `hover` | the fill of a hovered row or button (translucent) | 18 |
| `control` | neutral control fills (buttons, tracks, segmented controls) | 44 |
| `inputBg` | text fields | the SDK's `TextInput`, `SearchField` |

(The counts are uses of `t.<token>` in `apps/**/*.jsx` on 2026-10-26.)

**Optional tokens apps and the SDK read:**

- `accents`: a map name → color. Settings → Appearance shows these swatches and stores the chosen
  *name* (`settings.accent`). Use the same names as the bundled themes (`azul`, `violeta`, `rosa`,
  `naranja`, `verde`, `grafito`) so the user's choice survives a switch; `syncPortal` maps them to
  the portal's accent names.
- `wallpapers`: `[{ id, nameKey, name, src, thumb?, category?, variants? }]` for Settings →
  Wallpaper; `lordosWallpapers` from the SDK is the LordOS collection
  (`assets/wallpapers/README.md`).
- Shape tokens for the shared controls: `controlRadius`, `controlRadiusSmall`, `groupRadius`,
  `menuRadius`, `menuItemRadius`, `menuHighlight` (`'accent'`|`'subtle'`), `selection`
  (`'fill'`|`'indicator'`), `inputFocus` (`'ring'`|`'underline'`), `buttonBorder`, `popupBg`, `focusRing`. They
  restyle `Button`, `TextInput`, `GroupBox`, `SidebarItem`, menus… in every app without touching
  the apps ([DESIGN.md](DESIGN.md#theme-tokens-for-the-shared-controls)).
- Scroll bars: `scrollThumb`, `scrollThumbHover`, `scrollRadius`, `scrollThumbBorder`.
- `gtkButtons`: the GTK title-bar button order sent to Linux apps (`':minimize,maximize,close'` for
  buttons on the right; by default `'close,minimize,maximize:'`). It is used while Settings → Desktop &
  Dock → Title bars → Window buttons is Automatic; Left or Right override it.
- Window buttons: a theme's frame asks `useWindowButtons(themeStart)` (from `@lordos/sdk`; `themeStart` is
  where the theme puts them by itself) and draws its buttons first in the title bar's row when `atStart`,
  in the returned `order` (`'close'`, `'minimize'`, `'maximize'`, close always nearest the corner). Left
  and Right are the screen's sides in every language; Automatic follows the reading direction (#58).
- `radius`: a general corner radius (default 8).

`ThemeProvider` adds two tokens from the user's settings: `scrollBars` and `reduceMotion`. The
user's choices that a theme turns into tokens itself are in `settings` (`sdk/settings.js`): `mode`,
`accent` (a name), `effects` (transparency with blur), `wallpaper`. Both bundled themes do it with a
`makeTheme({ mode, accent, effects })` in `tokens.js`, called on every render with
`useSettings()`, so a change in Settings → Appearance applies at once.

### `index.jsx`: what it does and how the shell starts it

There is no exported component: `index.jsx` is a script that runs once when the bundle is
evaluated. The order the bundled themes follow:

```jsx
import { startShell, configureWindows, ThemeProvider, useSettings, useWindows, getApp, Layer } from '@lordos/sdk';
import { apps } from '@lordos/apps';          // every native app in apps/, added by the build
import { makeTheme } from './tokens.js';

// 1. At module level: the apps, the work area and the frame size (before startShell)
configureWindows({ apps, workArea: (d) => areaFor(d), chrome: { w: 2, h: TITLE_H + 2 } });

function App() {
  const [s] = useSettings();
  const t = makeTheme(s);
  const { windows, visible, focused } = useWindows();
  return (
    <ThemeProvider tokens={t}>
      {/* wallpaper, <DesktopWidgets layer="below" />, the windows, <DesktopWidgets layer="above" /> */}
      {/* <Layer name="dock">…</Layer>, <Layer name="menubar">…</Layer>, popovers, modals */}
    </ThemeProvider>
  );
}

// 2. Last: the SDK starts the desktop
startShell(<App />);
```

**`configureWindows({ apps, workArea, chrome, snap })`** (`sdk/windows.js`):

- `apps`: always the `apps` from `@lordos/apps`.
- `workArea`: a function `display → { x, y, w, h }`, the free area for windows on that display
  (without the bar and the Dock). Maximize, snapping, new windows and `wm.beginMove`'s top limit use
  it. An object is still accepted and means the main display's area. Call it again when the bar or
  Dock changes (both themes do it in a `useEffect` on the Dock settings and the screen size).
- `chrome: { w, h }`: how much the frame adds around the content (borders plus title bar), so a
  Linux window opens at the size it asked for.
- `snap: true`: window snapping (drag to the edges, `wm.snap`, `wm.snapKey`, the `snapPreview`
  store to draw the preview; [WINDOWS.md](WINDOWS.md#snapping-083)). Lord Classic turns it on.

**`startShell(element)`** (`sdk/index.js`): switches to the user's theme if it isn't this one, turns
the layers on, reopens the windows after a theme switch, renders your element inside a root that
remounts on a language change, and adds the SDK's own surfaces in their layers (search,
clipboard and emoji panels, print dialog, notification banners, prompts, Identify displays,
fullscreen windows, the input method). It also starts the background services every theme gets:
alarms, mail and reminder notifications, hardware (Night Light, idle, battery), disk health,
browser video, the network item, and the "modified component" notice.

The window objects (`useWindows()`: `windows` all of them bottom to top, `visible` without
minimized/fullscreen/picture-in-picture, `focused` the id in front) carry `id`, `app`, `title`,
`args`, `x`, `y`, `w`, `h`, `minimized`, `maximized`, `snapped`, `fullscreen`, `pip`, `protected`
and, for Linux windows, `windowMenu`. Use `wm.frame(w)` for the effective geometry (a maximized
window takes the work area), `windowTitle(w)` for the title, `windowIcon(w)` for its icon, and
`getApp(w.id)` for the app (`transparent`, `minSize`, `name`…). The `wm` object has `open`, `close`,
`focus`, `minimize`, `toggleMaximize`, `snap`, `center`, `moveToDisplay`, `arrange`, `move`,
`resize`, `setFrame`, `beginMove`, `beginResize` and more (`sdk/windows.js`).

A session starts on an empty desktop: a theme opens no window of its own at start.

### Window frames

Build every frame on **`<WindowShell win={w} style={…} onMouseDown={() => wm.focus(w.id)}>`**
(`sdk/windowshell.jsx`). It positions the window, marks the node for the C side (`wm.frameRef`), and
adds invisible resize edges on 4 sides and 4 corners (6 px outside, 3 px inside) with their cursors.
Inside it:

- a title bar whose `onMouseDown` calls **`wm.beginMove(w.id)`**. Moving and resizing are then done
  by `lord-server` in C (`server/src/ui/grab.c`): React gets no event per frame, and the moving
  window is painted from a cached layer ([WINDOWS.md](WINDOWS.md)). `beginMove` returns `false` when
  it can't (a maximized window, no button held); Lord Classic then drags out of the maximized place
  in JS with `wm.move`;
- **`<AppHost id={w.id} focused={…} />`** for the content, in a `flex: 1` box;
- no background under the content when `getApp(w.id).transparent` (the Terminal). Such a window has
  nothing behind its border either: draw the border with `solidOver(line, titleBg)` so it stays
  opaque (both themes do). Put **`<SeamCover color={titleBg} below={…} />`** (from `@lordos/sdk`) first in its title
  bar, and in any bar under it: at 125 %, 150 % or 175 % the edges between the border, the bars and the app's first
  row fall inside a pixel, and without it that row lets the desktop through (GitHub #162). `below` reaches past the
  bar into what follows it (the default theme: 2, its 1 px separator and 1 px of the app);
- the title with `windowTitle(w)` (apps change it, for example the Terminal);
- buttons that `stopPropagation()` on mouse down, so pressing them doesn't start a move.

**Linux windows** enter as windows of the internal app `linux` (`sdk/wayland.js`). When
**`w.args.csd`** is true the app draws its own title bar: render only `<WindowShell>` +
`<AppHost>` (with your shadow) and no frame. `args.csd` can change while the window is open (the
browsers' "tabs in the title bar" switch), so decide on every render. When such an app asks for the
window menu (a right click on its own bar), `w.windowMenu` is `{ x, y }`: draw your window menu there
and call `wm.closeWindowMenu(w.id)` when it closes ([WAYLAND.md](WAYLAND.md)). With `chrome`
declared in `configureWindows`, server-side-decorated windows open at the size they requested.

### Menus

The SDK gives menus as **item lists**; the theme draws them. An item is
`{ label, onClick, hint, disabled, checked, submenu, header }` or `'-'` for a separator.

| Source | What it is |
|---|---|
| `useAppMenus(winId)` | the app's own menus (`[{ label, items }]`, e.g. the Terminal's Shell, Edit, View); `null` if it has none. Isolated apps' menus come through the broker and arrive the same way |
| `appMenuItems(winId)` | the app menu: About *app*, Settings…, Hide, Hide Others, Show All, Quit (`sdk/appcontrol.js`) |
| `windowMenuItems(winId)` | the Window menu, the same in every theme: Minimize, Zoom, Fill, Center, Move & Resize (halves, quarters, thirds, arrangements), Move to *display*, Bring All to Front, the window list (`sdk/windowmenu.js`) |
| `dockMenu(entry)` | a Dock or taskbar item's context menu |
| `trashMenu({ onEmpty })` | the Trash's menu |
| `recentItems(n)`, `storeUpdates()`, `runningApps()`, `forceQuit(key)`, `appGroup(winId)` | for a system menu like Lord Flow's crown menu |

Where they go is the theme's choice: Lord Flow shows the app menus in its top menu bar
(`themes/default/components/MenuBar.jsx`), Lord Classic in a strip under each window's title bar
(`themes/classic/components/Window.jsx`). A theme without either must still give the app's menus a
place, or the apps lose commands. Each bundled theme has its own `components/Menu.jsx` that puts the
menu in the `popover` layer.

### Layers

`zIndex` only orders siblings. Everything above the windows goes into a named **layer**:
`<Layer name="dock|menubar|overlay|popover|modal|notification|tooltip|drag">` (the full order, with
`floating`, `system` and `input` reserved for the SDK, is in `sdk/layers.js` and
[LAYERS.md](LAYERS.md)). The layer, not the tree, decides what is drawn and clicked on top, so a menu
is never under the Dock. A popover anchored to a control is `<Overlay anchor={ref} onClose={…}>`.
While something is in `overlay`, `modal` or `system`, Linux apps don't get the keyboard; `popover`
keeps it.

### The Dock or taskbar

`sdk/dock.js` is the model; the theme draws it ([DOCK.md](DOCK.md)):

```js
const entries = useDockEntries(['files', DOCK_LAUNCHER, 'terminal', 'settings'], { launcher: { Icon, onOpen }, recents: 0 });
// entry = { id, kind, name, Icon, windows, pinned, running, focused, locked, movable, … }
dockActivate(entry);            // click: launch, or bring up (cycle) its windows
dockMenu(entry);                // context menu items
dock.pin(id, i) · dock.move(id, i) · dock.unpin(id) · dock.reset()
```

Pinned items are typed ids (`app:`, `lpk:`, `desktop:`, `shell:launcher`; `sdk/dockmodel.js`);
Files is always first and the Trash last. `dockPrefs(settings)` gives the user's position, size,
magnification and auto-hide (and `taskbarPosition` for a taskbar). Dragging uses
`beginDockDrag`, `registerDockTarget`, the `dockDrag` and `dockPoof` stores (the theme draws the
drag image and the "poof"). The Trash: `useTrash()`, `openTrash()`, `emptyTrash()` (ask first with
`ConfirmDialog`).

### Panels: launcher, Control Center, quick settings, notifications

There are no ready-made panels in the SDK: each theme draws its own from SDK stores and controls.

- **App launcher / start menu:** `useLauncherApps()` (LordOS apps and installed Linux apps, sorted by
  name, with `name`, `Icon`, `kind`), `launchApp(id)`, `appCategory(app)`, `APP_CATEGORIES`;
  system-wide search results from `searchEverything` (Lord Classic's start menu).
- **Control Center / quick settings:** `Toggle`, `Slider`, `Segmented`; `useSound`, `sound`,
  `defaultSink`; `useBluetooth`; `useBattery`, `BatteryGlyph`; `nightActive`, `setNightManual`;
  `notifyPrefs`, `dndActive` (Do Not Disturb); `NowPlayingTile`; `useCpuUsage`, `useSystemInfo`;
  `keyboard.label(keyboard.active())`, `keyboard.next()`. See
  `themes/default/components/ControlCenter.jsx` and `themes/classic/components/QuickSettings.jsx`.
- **Status area:** `<StatusItems color={…} />` (items apps register with `registerStatusItem`, such
  as the network item) and `<PrivacyIndicators />` (camera, microphone, screen in use).
- **Notifications:** banners are drawn by the SDK (`NotificationBanner`, placed by `startShell`);
  `setBannerPlacement('bottom')` puts them above a bottom taskbar. A notification centre reads
  `notifyHistory` (and `useNotifications()` for what is on screen), with `dismissNotification(id)`
  and `clearNotifyHistory()` (`themes/classic/components/NotificationCenter.jsx`).
- **Search:** the SDK's search panel opens with ❖Space or Ctrl+Space by itself; a button calls
  `spotlight.toggle()` ([SPOTLIGHT.md](SPOTLIGHT.md)).
- **Session:** `session.quit()`, `session.lock()`, `session.switchUser()` (with `session.managed()`),
  `session.restart()`, `session.shutdown()` (60 s confirmations; render `<PowerDialog />` in the
  `modal` layer, inside your `ThemeProvider`, so it takes your colors).
- **Gestures:** `registerGestureActions({ up, down, left, right })` in a `useEffect` (it returns the
  function that removes them; [TRACKPAD.md](TRACKPAD.md)).
- **Desktop widgets:** `<DesktopWidgets layer="below" />` between the wallpaper and the windows and
  `<DesktopWidgets layer="above" />` after them (the Notes app's notes, [NOTES.md](NOTES.md)).

Keyboard shortcuts: a theme passes its own table to `useShortcuts({ grabs, table, onSuperTap })`, and the SDK
adds the common ones (search, clipboard, lock, quit…) and runs them all from one dispatcher, so a key never
also reaches an app ([SHORTCUTS.md](SHORTCUTS.md)). `grabs` names a fixed list of keys kept from Wayland
apps. Mount `<AppSwitcher />` inside the `ThemeProvider` for the app switcher, and open it with
`openSwitcher(modifierBit, back)`.

### The login and lock screen are not your theme

`themes/greeter` is loaded by `lord-sessiond` with `lord-server --greeter`, as the system user
`lordos-greeter`, from a fixed path (`server/src/remote/sessiond.c`). It is also the lock screen
(mode `lock`) and the user switcher. It calls `render()` itself instead of `startShell()`, and it never
sees passwords being checked: it sends them with `lord.greeter.send()` and the root process answers
([SESSIONS.md](SESSIONS.md)). A desktop theme can't replace or restyle it; the first frame of a
boot (the hand-off from the boot splash) is the greeter's, or the default theme's with autologin.

### Right to left

With Arabic or Hebrew the engine flips rows and alignment by itself. Your part
([LANGUAGES.md](LANGUAGES.md)):

- text that follows the language uses `textAlign: 'start'`/`'end'`, never `'left'`/`'right'`;
- use the logical edges (`paddingStart`, `marginInlineEnd`, `insetInlineStart`,
  `borderTopStartRadius`…) instead of physical ones;
- anything positioned from the mouse x (a magnifying Dock) is laid out with `direction: 'ltr'` or
  takes `direction()` into account; drop-down menus open towards the inside (`direction() === 'rtl'`);
- physical things stay physical: Super+Left snaps to the left in any language, and the Dock's
  `left`/`right` positions are screen sides.

### Light, dark, accent, transparency and motion

- Build every color from `mode` (both themes switch the whole palette in `makeTheme`).
- The accent comes as a name (`settings.accent`) mapped through your `accents`.
- `settings.effects === false` means "no transparency": draw opaque bars and `blur: 0`.
- **Reduce motion** (`settings.reduceMotion`, also `useTheme().reduceMotion`): skip decorative
  animation (both themes skip the Dock's "poof" when an item is removed, Lord Flow also the Dock's
  reorder glide; `ScrollView` stops fading).

### Several displays

Use `screenDisplays()` (`[{ id, name, x, y, w, h, scale, main }]`), `mainDisplay()`,
`displayAt(x, y)`, `displayFor(rect)` and `screen.mirror` ([DISPLAYS.md](DISPLAYS.md#the-theme-and-the-sdk)).
The bundled themes draw a wallpaper per display at its pixel size
(`wallpaperSrc(w, d.w * d.scale / 100, d.h * d.scale / 100)`), and put the Dock on the main display.
Lord Flow draws a menu bar on every display (`<MenuBars>` in `themes/default/components/MenuBar.jsx`;
off with `settings.menuBarAllDisplays === false`, read it with `menuBarEveryDisplay(settings)`):
`barDisplays`, `hasBar`, `activeBarId`, `pinBar` and `panelLeft` (`sdk/menubarmodel.js`) say which
displays get a bar, which work areas reserve it, which bar has the app's menus lit and where a panel
opens; `sharedTicker` (`sdk/ticker.js`) gives every bar's clock one timer; `spotlight.toggle(display)`
opens Search on that display. Lord Classic draws a taskbar on every display under the same setting
(`themes/classic/placement.js`; each bar registers its own `registerDockTarget`, and a drop goes to the first
bar that is hit). Your `workArea` function receives each display; `workAreaOf(d)` and
`pointerWorkArea()` read it back. New windows open on the display under the pointer, or the one the
user assigned the app to from the Dock. The window list is global: windows are not per display.

### Performance rules

Everything runs on `lord-server`'s main thread, which also composites the screen and serves the
Linux apps. A slow theme makes the whole desktop slow.

- **No React render per frame.** Moving and resizing windows is already in C. To animate, change one
  style with **`lord.setStyle(ref.current.id, 'opacity' | 'rotate' | 'scaleX' | …, value)`** from a timer or
  `requestAnimationFrame` (the `ScrollView` thumb fade in `sdk/components.jsx`, the fans in
  `sdk/cooling.jsx`); stop the loop when the animation ends or the surface is hidden, and don't start
  it with reduce motion. `lord.setStyle` takes the engine's own keys (`rotate` in degrees, `scaleX`,
  `scaleY`), not a CSS `transform` string: an unknown key changes nothing and is logged once.
- **`transform` scale and rotate are cheap**: the subtree is painted once into a cached raster and
  composited (Lord Flow's minimize effect scales a whole window). The raster is freed 30 s after its last use
  (0.8.9.1), so an animation costs its memory only while it runs and shortly after.
- **Blur costs**: `backdropFilter: blur(r)` reads a margin of 2×r around the surface, and any change
  touching it repaints the whole surface ([LAYERS.md](LAYERS.md#blurred-surfaces-and-partial-repaints)).
  Keep blurred surfaces few and small, and turn blur off with `effects: false`.
- **Subscribe narrowly**: `useStore(store, selector)` re-renders only when the selected value changes;
  a clock that renders every second should only re-render its own text.
- **Don't poll** when a store exists (sound, battery, Bluetooth and displays arrive as events). Lord
  Classic's network glyph polls every 5 s; that is the upper bound to aim for.
- The style subset has no CSS animations, dashed borders, skew or 3D (`themes/README.md`). An
  unknown style key is logged as `js: unsupported style '<key>'`, once per key per process (an
  animation that sends it every frame writes one line). After 64 different keys, one more line ends
  with `(further unknown style keys are not logged)` and no others are logged.

### Images, fonts and other files

`<Image src="assets/x.png">` is resolved against the theme's folder; `asset:wallpapers/…` against
LordOS's `assets/`; an absolute path as is (`server/src/ui/js.c`). PNG and JPEG. Text uses the
bundled Inter (400/500/600/700). The `.deb` packaging copies only `theme.json`, `theme.bundle.js`
and `assets/` of each theme (`tools/iso/mkdeb.sh`), so keep your files there.

### Translations

Visible text goes through `t('namespace.key')` ([LANGUAGES.md](LANGUAGES.md)); app names through
`appName(app)`, dates and times through `formatDate`/`formatTime`. The bundled themes' strings are in
the `shell` catalog (`assets/locales/<lang>/shell.json`, in every one of the 35 languages). A theme of your own can reuse existing
`shell.*` keys (the example does) or bring its own namespace: `lord.locale.catalog` reads
`<lang>/<namespace>.json` from `~/.local/share/lordos/locales/` first and then LordOS's
`assets/locales/` (`f_locale_catalog` in `js_sys.c`). A missing key shows the key itself. Inside the
LordOS tree, `node tools/i18n-check.mjs` checks every `t()` key in `themes/` against the English
catalog and every language against English.

## 3. Building it

`npm install` once (React, the reconciler and esbuild), then:

```sh
npm run build                         # every theme, the isolated-app host and the system apps
node tools/build-theme.mjs <id>       # only themes/<id>
```

`tools/build-theme.mjs` bundles `themes/<id>/index.jsx` with esbuild into
**`themes/<id>/theme.bundle.js`** (one minified IIFE, ES2020, JSX automatic). React, the reconciler,
`@lordos/runtime` (`runtime/index.js`) and `@lordos/sdk` (`sdk/index.js`) are **bundled into it**, and
so is every native app: the virtual module `@lordos/apps` imports `apps/<id>/index.jsx` and
`manifest.json` for each app (and checks that each has a semver `version`). That is why a theme
bundle is 1.5–1.8 MB and why every theme carries the same apps. Apps with `"process": "isolated"`
(Passwords, Mail…) run as their own processes; in the theme they are only a starter
(`isolatedApp` in `sdk/packages.js`).

The tool only builds folders under `themes/` of the tree it lives in: there is no option for a theme
elsewhere, so a theme is developed inside a LordOS development tree (copy it to `themes/<id>`).

After building, it rewrites **`code-manifest.json`** at the tree's root (`tools/code-manifest.mjs`):
the size and SHA-256 of every `themes/*/theme.bundle.js`, `assets/app-host.bundle.js` and
`assets/sysapps/*/app.js`. If the manifest changed, a `code-manifest.sig` next to it is deleted (it
signed the old one).

### Verified code: what happens to a theme that LordOS did not sign

LordOS's own JavaScript is checked before it runs ([SIGNING.md](SIGNING.md#verified-system-code-082)).
Every bundle goes through `codesig_load` (`server/src/sec/codesig.c`, called from `load_code` in
`server/src/ui/js.c`): once per process `lord-server` reads `code-manifest.json` and
`code-manifest.sig` from the LordOS folder (`/usr/lib/lordos` installed, the repository root in a
development tree) and verifies the signature with the release key compiled into it. Then, per
bundle:

| `lord-server` build | Manifest in the LordOS folder | A `themes/<id>/theme.bundle.js` that is not listed |
|---|---|---|
| **Release** (`make -C server RELEASE=1`; the published images and `.deb`) | signed by LordOS | **refused**: `code: REFUSED themes/<id>/theme.bundle.js: not listed in the manifest` |
| Release | missing, unsigned, or a bad signature | **everything is refused** (state "invalid"): the recovery screen |
| Development (`make -C server`) | none | **loads** (About says "Development build") |
| Development | unsigned (what `npm run build` writes) | **refused unless listed**: the hashes are still checked, so rebuild with `npm run build` / `build-theme.mjs` to list it |
| Development | signed | refused unless listed |

So on an installed release system, copying a theme into `/usr/lib/lordos/themes/<id>/` (which needs
root anyway) makes it **appear** in Settings → Appearance → Desktop style (the list only looks for a
readable `theme.bundle.js`), but choosing it does nothing visible except a notice: `lord.themes.use` refuses it
before the running theme is touched (`js_theme_prepare`), the session goes on in the theme it was on, the choice is
put back, and the notifications say the style couldn't be applied and "This LordOS component was modified and was
not loaded" naming the file (`sdk/codesig.js`); choosing it again in that session does nothing (`ui.theme_failed`).
**Do not** try to make it load by
regenerating the installed manifest: on a release build an unsigned or changed manifest invalidates
all of LordOS's code and leaves only the recovery screen.

A bundle outside the LordOS folder (for example `lord-server --theme /home/me/x/theme.bundle.js`)
is run only when the state is "development" (a development build with no manifest or an unsigned
one), logged as `code: … loaded unchecked (outside …, development build)`; otherwise it is refused
with "not part of LordOS's system code".

**A bundle built elsewhere can't run on release builds.** That is deliberate: a bundle in LordOS's folder is not
sandboxed. It runs inside `lord-server`, in the same JavaScript context as every native app, with
the whole `lord.*` API of the session (files, the terminal, the network, administrator requests),
and it sees the desktop's keyboard and pointer input. It also carries its own copy of the SDK,
including the code that shows the permission prompts and records the answers (`lord.perms.set`,
`server/src/ui/js_sec.c`), so a theme decides what those prompts do. Supporting
them would need, at least: a theme package format and a separate, user-confirmed trust decision
(signed like `.lpk` packages, [SIGNING.md](SIGNING.md), not the LordOS release key), a place for
user-installed themes that the code check knows about, and probably a narrower API for theme code
than what built-in code gets. Since 0.8.9.6.2 (in development) that exists for themes installed from a source `.zip`:
see [Community themes](#6-community-themes-installed-from-a-file).

## 4. Installing and testing

**Where themes live.** Installed: `/usr/lib/lordos/themes/<id>/` (`theme.json`, `theme.bundle.js`,
`assets/`), next to `/usr/lib/lordos/code-manifest.json`; the development VMs also use
`/usr/local/lib/lordos` ([VM-SETUP.md](VM-SETUP.md), step 22). The list of themes is the folder that
holds LordOS's themes (`themes_root` in `js_sys.c`), plus the community themes the person installed from a file in
`~/.local/share/lordos/themes/` ([Community themes](#6-community-themes-installed-from-a-file)).

**Trying one in a development session** (a development build of `lord-server`, see the table
above):

1. Put the theme in `themes/<id>` of your LordOS tree and run `node tools/build-theme.mjs <id>` (or
   `npm run build`).
2. Copy the tree to the VM and build `lord-server` as in [VM-SETUP.md](VM-SETUP.md), step 6 (`themes/`
   and `assets/` with `server/`). Copy `code-manifest.json` too, or none at all: a stale manifest
   refuses the new bundle.
3. Start it and choose the theme in **Settings → Appearance → Desktop style**. Passing
   `--theme ../themes/<id>/theme.bundle.js` to a standalone `sudo ./build/lord-server` also works,
   but `startShell` switches to the theme saved in `~/.config/lordos/settings.json` (`"theme"`) if it
   is another one, so set that too.
4. After rebuilding, switch to another theme and back (or log out and in) to load the new bundle.
   There is no hot reload.

A theme that throws while its bundle is evaluated makes the switch fall back to the theme you came
from (`lord-server: the theme failed, going back to …`); the error is in the log. Keep Lord Flow or
Lord Classic installed: they are the fallback (`start_theme` in `main.c`).

**Debugging.**

- Sessions started by `lord-sessiond` write their log to **`/var/log/lordos/users/<user>/compositor.log`**,
  and the shell's `js:` lines (with their stacks) to `shell.log` next to it; the previous session's are
  `compositor.log.prev` and `shell.log.prev` (docs/SYSTEM.md, "Log files"). A standalone `lord-server` writes to
  the terminal it was started from; a headless one without lord-sessiond to `~/.cache/lordos/`.
- `console.log`, `.info`, `.debug` → `js: …` lines; `console.warn` → `js: [warn] …`;
  `console.error` → `js: [error] …` (the prelude in `server/src/ui/js_prelude.h`). Exceptions in timers
  and animation frames are logged as `js: [error] timer: …` / `raf: …`.
- Useful lines: `js: theme <path> loaded in N ms (K KB)`, `code: …` (the verified-code state and each
  check or refusal), `lord-server: cambiando a tema <path>` (a switch), `js: unsupported style '<key>'`.
- `sudo /usr/lib/lordos/bin/lord-server --check-code [<folder>]` checks a whole tree the way a session
  would; `lord-server --debug` and `--profile` (per-primitive timings) for drawing problems.
- `lord.getLayout(node.id)` returns `[x, y, w, h]` of a node (both themes use it to place menus).

## 5. Checklist before sharing a theme

Check each item on a real session and take a screenshot ([VM-SETUP.md](VM-SETUP.md), `screendump`):

- **Both kinds of window:** LordOS apps (the Terminal for a transparent one, Settings, Files) and
  Linux apps with and without their own frame (`args.csd`), including the window menu from a right
  click on an app's own bar, and an app opening at the size it asked for (`chrome`).
- **Menus:** every app's menus are reachable (`useAppMenus`), the Window menu works, menus never open
  under the Dock or off screen.
- **Light and dark**, every accent, transparency on and off.
- **Right to left:** with the Arabic test pack (`docs/packs/lordos-lang-ar.json`): rows mirrored,
  `textAlign: 'start'`, menus opening inwards, mouse-driven pieces in `direction: 'ltr'`.
- **Translations:** no hard-coded text; every key in all 35 languages (`node tools/i18n-check.mjs` shows each one
  complete); long translations truncate with an ellipsis instead of overflowing.
- **Accessibility:** readable contrast of `text` and `textDim` on `windowBg`, `sidebarBg` and your
  bars in both modes; reduce motion respected; everything opened from the keyboard can be closed
  with Escape; targets big enough to hit with a touchpad.
- **Several displays:** wallpaper on each, the work area right on each, maximize on a second display,
  a display unplugged and plugged back, mirror mode; and a single display at 150 % scale.
- **The work area follows the bar and the Dock** (position, size, auto-hide) live.
- **Performance:** dragging and resizing stay smooth with your blur; nothing re-renders per frame
  when idle (no `requestAnimationFrame` loop left running).
- **Theme switch both ways** with windows open: they come back in your theme, and back in Lord Flow.
- **The login and lock screen** are the system's (`themes/greeter`): nothing in your theme should
  try to imitate them.
- **Notifications, search, permission prompts and the power dialog** look right inside your
  `ThemeProvider` (they use your tokens).
- And the [DESIGN.md](DESIGN.md#robustness-checklist) robustness checklist.

## 6. Community themes installed from a file

(0.8.9.6.2, in development; approved by the maintainer on 2026-11-26. The plan: [THEMES-PLAN.md](THEMES-PLAN.md) stage
2; AGENTS.md "Community themes".) A theme someone else made runs on a normal LordOS without being "a program with full
access". Five pieces make that so.

**1. Install from a source `.zip`.** Settings → Appearance → **Install from a File…** takes the archive
lordos-theme-kit's `/lordos-theme package` makes: one folder named after the theme's id (`[A-Za-z0-9_-]`, at most 26
characters) holding `theme.json`, `index.jsx` and its other `.js`/`.jsx` files, `.json` data, `assets/` images (PNG,
JPEG), `locales/<language>.json`, `LICENSE` and text files. The sandboxed helper **lord-themec**
(`server/src/themec/`: no files, no network, no exec) reads it strictly — any `..`, absolute name, link, device, second
folder, duplicate, encryption, Zip64, oversized entry or wrong CRC refuses the whole archive — then runs LordOS's theme
compiler (`tools/themec/themec.js`) in a QuickJS of its own: the kit's static checks and the build. Settings lists what
was refused, with the file and line. LordOS never runs a bundle from the archive: it **builds `theme.js` itself**.
An id that one of LordOS's themes has is refused.

**2. The static checks** (the kit's `check.mjs`, on the files in memory): the manifest (`name`, `description`,
`"community": true`, `author`, `license`, `version`, `capabilities`); allowed file types and sizes; imports only of
`react`, `@lordos/sdk`, `@lordos/apps` and its own files; no `eval`, `Function`, dynamic `import()`, `require`,
`globalThis`/`window`, names starting with `__`, `constructor`/`__proto__` tricks, `Reflect`, `Proxy`, `Symbol.for`,
`fetch` and friends, remote URLs, obfuscation or minified lines; no `lord.*` beyond the theme's own five functions; no
host elements (`<view>`); every visible text through `t()` (keys of its own namespace `theme-<id>` must be in
`locales/en.json`). Run `/lordos-theme check` before packaging: it is the same list.

**3. A restricted `lord.*`.** The theme's code runs in a **second JavaScript realm** of the session
(`server/src/ui/js.c` `start_community`). Its global has only:

| | |
|---|---|
| `lord` | `getLayout`, `setStyle` (its own nodes only), `measureText`, `now`, `log`, `version` |
| timers | `setTimeout`, `setInterval`, `requestAnimationFrame`, `queueMicrotask`, `console`, `performance.now` |
| imports | `react` (its own `createElement`, hooks, `memo`, context…), `@lordos/sdk` (an allowlist, `sdk/themeapi.js`), `@lordos/apps` |

LordOS's code — React, the SDK, every app and the system layer, the signed **theme host**
`assets/theme-host.bundle.js` — keeps the full API in the first realm, which is frozen before the theme's first line
(no built-in can be changed and no code made from text). lord-server checks **every native call** made while the
theme's code runs or runs for it (its timers, promises, callbacks, effects, stores and bound functions:
`server/src/ui/js_theme.c`): a theme may manage windows, launch apps through the shell, read text, pictures and
translations, lock the session, and **read** the models its `theme.json` declares:

```json
{ "capabilities": ["wifi", "bluetooth", "sound", "battery", "brightness", "media", "notifications"] }
```

`notifications` (approved by the maintainer on 2026-11-26) gives a notification centre the history as read-only plain
data: titles, bodies, apps and times, never their actions, and no dismissing or clearing.
Without a capability the SDK's hook gives a neutral value (no battery, no players). Never: the terminal, files,
administrator actions, permission answers, packages, the keyring, the clipboard, network settings, switching themes or
changing Wi-Fi, Bluetooth, the volume, the brightness or what plays. Refs and events give the theme a handle
`{ id, type }`, never LordOS's node objects; LordOS's data (windows, settings, stores) arrives as frozen copies;
whatever the theme hands back (props at any depth, arguments, what its callbacks return) is copied by an allowlist,
always as the theme's own code (a getter, a Proxy or an iterator in it runs under the theme's policy), and a host
node whose element LordOS's own React didn't make is refused (a forged React element never becomes a node); its key listeners hear keys only while the keyboard is the shell's (its
launcher, its menus, no window focused) and never while a system prompt is up; its shortcuts need ❖, ⌃ or ⌥;
`settings` is read whole but written only for its own options (`themeOptions[<id>]`) and `mode`, `accent`, `effects`,
`dock…`, `menuBar…`, `taskbar…`. A refused call throws `theme.notAllowed` and is logged as
`community theme: lord.<name> refused` in the compositor log.

**4. The system layer.** Permission, keyring and administrator prompts, the file chooser and the other system surfaces
are drawn by LordOS in the `system` layer; notification banners and the privacy pill in `notification`, above every
layer a theme can draw in (`dock`, `menubar`, `overlay`, `popover`, `modal`, `tooltip`, `drag`). A community theme
can't dismiss a banner or run its actions (a notification centre reads plain data with the `notifications`
capability), and with one the privacy pill is always in each display's corner. The lock and login screens are the
greeter's, another process.

**5. Trust and recovery.** The first time, Settings asks: **"This theme isn't from LordOS"**, with the author, what it
can't do and what it may read; only the person's answer there lets it run (lord-server keeps it in the theme's install
record, which `lord.fs` can't write). A **watchdog** (`community_watch` in `server/src/main.c`) takes a community theme
off when it throws at start, never calls `startShell`, draws nothing within 6 seconds, has its React tree die, stalls
the desktop until the main-loop watchdog stops it, or was running when two sessions in a row ended abnormally (a
lord-server that died; logging out, shutting down, restarting or switching user from LordOS is never counted, however
short, while a logout the theme's own code asks for keeps the count until the theme has run a healthy minute, and a
Restart… or Shut Down… countdown the theme starts never confirms by itself: only the person's click or key does): the previous style comes back and
Settings shows "Couldn't start" with Report, **Try Again** and **Remove**. **Ctrl+Alt+Shift+Backspace** always
restores Lord Flow.

**Where it lives:** `~/.local/share/lordos/themes/<id>/` (`theme.json`, the built `theme.js`, `assets/`, `locales/`,
`install.json`), written only by lord-server (`server/src/ui/themestore.c`). Its texts are the catalog `theme-<id>`
(`lord.locale.catalog` reads `locales/<language>.json` there).

**Tests:** `build/realm-taint-test`, `build/themestore-test`, `tests/themec_test.mjs` and
`tests/community_theme_headless.sh` (`make -C server test-full`), whose probe fixture tries every way past the policy.
