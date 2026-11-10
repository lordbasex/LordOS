# LordOS themes

A theme is a React app that draws **the whole desktop**: wallpaper, bars, dock or taskbar, menus
and window frames. Apps (`apps/`) go inside those windows and adapt to the theme's colors on
their own. `lord-server` runs the theme with QuickJS and paints it with its C engine.

| Theme | Style |
|---|---|
| `default` | Lord Flow: menu bar, dock with magnification, app launcher, Control Center |
| `classic` | Lord Classic: a centered taskbar, start menu with search, tray with quick settings and notifications, window snapping and the SDK's Overview (0.8.3) |

```sh
npm install           # once (React + esbuild)
npm run build         # bundles every theme, with the SDK and every app
```

The user switches themes live from **Settings → Appearance → Desktop style**, which lists every
installed theme (since 0.8.5 there is no separate Themes page), with no restart: the LordOS app
windows that were open come back in the new theme. The choice is saved and respected on the next start.

## Creating a new theme

The full guide, for a React developer new to LordOS, is [`docs/THEMES-GUIDE.md`](../docs/THEMES-GUIDE.md)
(anatomy, the token contract, frames, menus, layers, building, the verified-code check, testing and
a checklist); a small starting point is [`examples/theme-minimal/`](../examples/theme-minimal/).
Note that a theme LordOS did not sign only loads on a development build (see the guide). In short:

1. Copy `themes/classic` to `themes/mytheme` (`index.jsx` puts the pieces together; the frame,
   taskbar and panels are in `components/`, the colors and shape tokens in `tokens.js`).
2. Edit `theme.json` (the name and description Settings shows).
3. Modify `index.jsx`. A theme must:
   - call `configureWindows({ apps, workArea })` with the free area for windows,
   - wrap everything in `<ThemeProvider tokens={...}>`,
   - draw each visible window and, inside it, `<AppHost id={w.id} focused={...} />`,
   - end with `startShell(<App />)`.

   Recommended so apps look complete:
   - title with `windowTitle(w)` (apps can change it, e.g. the Terminal),
   - no background under the content if `getApp(w.id).transparent`,
   - the app's menus with `useAppMenus(focusedWindow)` (e.g. the Terminal's Shell, Edit, View),
   - the window node with the SDK's `<WindowShell win={w} style={…frame…}>`: it adds the invisible
     borders for resizing (4 sides and 4 corners, with their cursor) and the title bar calls
     `wm.beginMove(w.id)` in `onMouseDown`. Moving and resizing are done by lord-server in C,
     without one event per frame (see `docs/WINDOWS.md`),
   - the dock's "app open" dot by comparing with `w.app` (an app can have several windows).
   - texts with `t('shell.…')` (see `docs/LANGUAGES.md`), app names with `appName(app)` and date
     and time with the SDK's `formatDate`/`formatTime`, to respect language and region,
   - the active keyboard layout with `keyboard.label(keyboard.active())` and `keyboard.next()`
     (it includes the input methods: 한, あ, 拼),
   - working right-to-left: with an RTL language the engine flips rows and alignment on its own;
     whatever is positioned with the mouse x (a Dock-style magnifier) uses `direction: 'ltr'`,
     and drop-down menus open to the left if `direction() === 'rtl'` (see the default theme and
     `docs/LANGUAGES.md`). The input method's candidate window is placed by `startShell`: the
     theme doesn't need to do anything.
   - everything above the windows in its layer (`<Layer name="dock|menubar|overlay|popover|modal|tooltip|drag">`
     from the SDK): menus, the Control Center, the app launcher, tooltips and drag images. The
     order is the layers', not the tree's, and clicks follow it, so a menu is never under the Dock
     (see `docs/LAYERS.md`). `startShell` places the SDK's own surfaces (search, notifications,
     prompts, fullscreen windows).
   - Linux app windows (Wayland) arrive as windows of the internal app `linux`: if
     `win.args.csd` is true the app draws its own frame and the theme shows only `<AppHost>`;
     and `configureWindows({ chrome: { w, h } })` says how much the theme's frame adds (borders +
     title bar) so they open at the size they asked for (see `docs/WAYLAND.md`). `args.csd` can
     change while the window is open (a browser's title bar switch): render from it every time.
     When such an app asks for the window menu (right click on its own bar), `win.windowMenu` is
     `{ x, y }`: draw the theme's window menu there and call `wm.closeWindowMenu(win.id)` when it
     closes.
4. `npm run build` and pick it in Settings → Appearance → Desktop style.

Images (`<Image src="assets/...">`) are resolved relative to the theme's folder.

## Tokens the theme must provide (contract with the apps)

`mode`, `accent`, `text`, `textDim`, `windowBg`, `sidebarBg`, `separator`, `hover`, `control`,
`inputBg`. Optional: `gtkButtons` (order of the GTK apps' buttons, e.g. `':minimize,maximize,close'`; by default the traffic lights on the left, see `docs/WAYLAND.md`), `accents` (name → color map, for Settings), `wallpapers`
(`[{ id, nameKey, name, src, thumb?, category?, variants? }]`; Settings groups them by `category` and
shows `thumb` in its grid; add `lordosWallpapers` from the SDK to offer the LordOS collection, and
draw with `wallpaperSrc(w, width, height)` so a higher-resolution variant is used where it exists,
see `assets/wallpapers/README.md`), and the overlay scroll bars' look (0.8.3, `ScrollView`):
`scrollThumb`, `scrollThumbHover` (colors), `scrollRadius` (corner radius; a pill by default) and
`scrollThumbBorder` (a hairline color). `ThemeProvider` adds `scrollBars` and `reduceMotion` from
the settings, so apps (isolated ones too) follow Settings → Appearance. The theme can add any
tokens of its own.

Optional shape tokens restyle the SDK's shared controls in every app (see `docs/DESIGN.md`, "Theme
tokens for the shared controls"): `controlRadius`, `controlRadiusSmall`, `groupRadius`, `menuRadius`,
`menuItemRadius`, `menuHighlight` (`'accent'` | `'subtle'`), `selection` (`'fill'` | `'indicator'`),
`inputFocus` (`'ring'` | `'underline'`), `buttonBorder`, `popupBg` and `focusRing` (the keyboard focus ring's color,
the accent by default). Without them a theme gets Lord
Default's shapes.

Two switches a taskbar theme usually wants: `configureWindows({ snap: true })` turns on window
snapping (drag to the edges, `wm.snap`, `wm.snapKey`, and `snapPreview` to draw the preview; see
`docs/WINDOWS.md`, "Snapping"), and `setBannerPlacement('bottom')` shows notification banners above
a bottom taskbar instead of below a menu bar.

## Native components

| Component | What it is |
|---|---|
| `<View>`  | a box with flexbox layout, background, border, shadow, blur |
| `<Text>`  | text (Inter font 400/500/600/700), with line wrapping |
| `<Terminal session={id}>` | the grid of a terminal session (drawn in C) |
| `<Image src>` | PNG or JPEG, with `objectFit` |

Events: `onMouseDown`, `onMouseUp`, `onClick`, `onDoubleClick`, `onContextMenu` (right click),
`onMouseMove`, `onMouseEnter`, `onMouseLeave`, `onWheel` (`e.deltaY`).
Touchpad and touchscreen (`docs/TRACKPAD.md`):
- `onWheel` also arrives with `e.precise: true`, `e.deltaX`/`e.deltaY` in exact pixels and
  `e.phase` (`'update'`, `'end'`, `'momentum'` while it keeps going by inertia, `'momentum-end'`);
  `e.notches` counts whole 48 px steps (for things that advance line by line).
- `onPinch({ scale, rotation, x, y, dx, dy, phase })`: `scale` relative to the start, `rotation`
  accumulated in degrees, `phase` `'begin'`/`'update'`/`'end'`.
- `onSwipe({ dx, dy, fingers, phase, system })`.

During a gesture the target stays fixed: the node that was under the pointer when it started.
Globals: `addListener('pointermove' | 'pointerup' | 'pointerdown' | 'keydown' | 'keyup' | 'swipe' | 'pinch', fn)`.

Three-finger swipes (or four, depending on Settings → Trackpad) belong to the desktop. The theme
chooses what each direction does with the SDK's `registerGestureActions({ up, down, left, right })`,
which returns the function to remove them (use it in a `useEffect`). Examples:
- `default`: up opens the app launcher and down closes it;
- `classic`: up opens the start menu and down closes it.

Without a theme action, left and right switch windows and up/down do nothing.
Keyboard: `e.key`, `e.char` (depending on the layout), `e.code` (evdev), `e.shift`, `e.ctrl`, `e.alt`.

## Desktop widgets

A theme draws `<DesktopWidgets layer="below" />` between the wallpaper and the windows and
`<DesktopWidgets layer="above" />` above them (the Notes app's notes, for example; see
`docs/NOTES.md`). Both bundled themes already do it.

## Supported styles (a CSS subset)

- **Layout:** `display` (`flex`/`none`), `position` (`relative`/`absolute`), `flexDirection`
  (`column` by default, like React Native), `flexWrap` (`nowrap`/`wrap`: children that don't fit
  move to another line; `gap` also separates the lines, which go at the start: no
  `wrap-reverse` or `alignContent`), `justifyContent`, `alignItems`, `alignSelf`, `flex`,
  `flexGrow`, `flexShrink`, `flexBasis`, `gap`, `width`/`height` (px, `%`, `auto`),
  `min*`/`max*`, `left`/`top`/`right`/`bottom`, `inset`, `margin*`, `padding*`, `overflow`, `zIndex`
  (it only orders siblings: what goes above the windows uses the layers, see below).
  `flex` follows React Native: `flex: N > 0` grows and shrinks from a zero basis, `flex: 0` keeps its
  `width`/`height` and doesn't flex, `flex: -N` only shrinks.
- **Logical edges** (they follow the node's `direction`, and win over the physical side as in React
  Native): `marginInlineStart`/`marginInlineEnd` (also `marginStart`/`marginEnd`, `marginInline`),
  `paddingInlineStart`/`paddingInlineEnd` (`paddingStart`/`paddingEnd`, `paddingInline`),
  `insetInlineStart`/`insetInlineEnd` (`start`/`end`, `insetInline`).
- **Visual:** `backgroundColor`, `background: linear-gradient(...)`, `borderRadius`, `border`,
  `borderWidth`, `borderColor`, `opacity`, `boxShadow`, `backdropFilter: blur(px)`,
  `pointerEvents` (`auto`, `none`: the node and its children are never hit, `box-none`: only its
  children are), `cursor` (`default`, `pointer`,
  `text`, `move`, `grab`, `grabbing`, `not-allowed`, `wait`, `progress`, `crosshair`, `n-resize`… `ew-resize`,
  `ns-resize`, `nesw-resize`, `nwse-resize`; the one of the first node upwards that has it applies).
- **Per-side borders:** `borderTop`/`borderRight`/`borderBottom`/`borderLeft` (`'1px solid #ccc'`), their
  `…Width` and `…Color`, and `borderStart`/`borderEnd` (also `borderInlineStart`/`borderInlineEnd`) with
  `…Width`/`…Color`, which go on the right in RTL. A side's value replaces `borderWidth`/`borderColor`
  for that side only; a side without a color uses `borderColor`.
- **Per-corner radii:** `borderTopLeftRadius`, `borderTopRightRadius`, `borderBottomRightRadius`,
  `borderBottomLeftRadius`, and the logical `borderTopStartRadius`/`borderTopEndRadius`/
  `borderBottomStartRadius`/`borderBottomEndRadius` (CSS `borderStartStartRadius`… too). With
  `overflow: 'hidden'` the children and images are clipped to that shape.
- **Transform:** `transform: 'translate(x, y) scale(s) rotate(deg)'` or the React Native array
  (`[{ translateX }, { scale }, { scaleX }, { rotate: '45deg' }]`; `rad` and `turn` work too), `translateX`,
  `translateY`, and `transformOrigin` (`'50% 50%'` by default, `'left top'`, px). As in CSS, scale and
  rotate only change how the box is painted and hit: layout doesn't move (translate does move the box).
- **Text:** `color`, `fontSize`, `fontWeight`, `lineHeight`, `textAlign`, `whiteSpace: nowrap`,
  `textOverflow: 'ellipsis'` (with `nowrap`: one line cut with "…"; without it, the lines that fit in
  the box's height, the last one ending in "…"), `numberOfLines: N` (also `lineClamp`/`WebkitLineClamp`:
  at most N lines, "…" at the end of the last one).
- **Image:** `objectFit` (`cover`, `contain`, `fill`).

Colors: `#rgb`, `#rrggbb`, `#rrggbbaa`, `rgb()`, `rgba()` and some names.
Not available yet: dashed or dotted borders, `transform: skew/matrix`, 3D transforms, CSS
animations (animate from JS with state and `requestAnimationFrame`).

The full SDK is in `apps/README.md`.

The Dock or the taskbar follow the SDK's `dockPrefs(settings)` (position, size, magnifier,
auto-hide): see `docs/DOCK.md`.
