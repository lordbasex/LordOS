# Layers: what is drawn above what

One stacking order for the whole desktop (0.8.1). Menus, popovers, select lists, the Control
Center, notifications, tooltips and prompts are always above the Dock, the menu bar and the
windows, whatever the Dock's position (bottom, left, right), auto-hide, magnification, light or
dark mode, and in right-to-left languages. Clicks follow the same order: a click on a menu that
covers the Dock goes to the menu.

| | |
|---|---|
| ![crown menu, Dock on the left](layers-crown-dock-left-light.png) | ![dark](layers-crown-dock-left-dark.png) |
| The crown menu over the Dock on the left (it used to open underneath it) | Dark mode |
| ![Dock menu over a window](layers-dock-menu-left-dark.png) | ![Dock menu over the menu bar](layers-dock-menu-bottom-light.png) |
| A Dock menu over the windows | A tall Dock menu over the menu bar |
| ![Dock on the right](layers-dock-menu-right-dark.png) | ![Control Center](layers-control-center-dock-right.png) |
| Dock on the right: its menu over the window and the menu bar | The Control Center over the Dock on the right |
| ![select list](layers-select-passwords-light.png) | ![select list, dark](layers-select-passwords-dark.png) |
| Passwords → New item → Category: the list covers the form (the fields used to show through it) | Dark mode |
| ![Lord Classic](layers-classic-left-menu.png) | ![RTL](layers-crown-dock-right-rtl.png) |
| Lord Classic theme, taskbar on the left: its menu over the windows | Arabic, Dock on the right: the crown menu over the Dock |

## Why it broke

The engine's `zIndex` only orders **siblings** (`node_children_ordered` in `server/src/ui/node.c`),
and hit testing walks the same order (`hit` in `paint.c`). The theme drew the menu bar, then the
Dock, and each menu bar menu was a child of the menu bar: with the Dock on the left, the Dock came
later in the tree and covered the crown menu, and also took its clicks. Inside apps, a select
list drawn inside a form row sat under the rows that came after it, whatever its `zIndex`.

## The order

Each layer is a root node of the screen's size with a fixed `zIndex`, above the theme's tree;
content enters it through a React portal (`runtime/index.js`: `layerContainer`, `createPortal`).
A layer node is `pointerEvents: 'box-none'`: lord-server never hits the layer itself, only what is
in it, so an empty part of a layer lets the click through to whatever is below.

| Layer | zIndex | What goes there |
|---|---|---|
| base | 0 | the theme's own tree: wallpaper, desktop widgets, windows (in tree order) |
| `dock` | 10 | the Dock; the Lord Classic taskbar |
| `menubar` | 20 | the top menu bar |
| `floating` | 30 | fullscreen and picture-in-picture windows |
| `overlay` | 40 | the app launcher, search |
| `popover` | 50 | menus, context menus, the Dock's menus, the Control Center, select lists, popovers, the clipboard and emoji panels, the Lord Classic start menu, quick settings, notification panel and snap layouts |
| `modal` | 60 | shell sheets and confirmations (Empty Trash, Restart…, Shut Down…), the print dialog |
| `notification` | 70 | notification banners, the clipboard toast, the remote desktop badge, the displays' Identify numbers (0.8.5, they never take clicks) |
| `tooltip` | 80 | tooltips (the Dock's and the Lord Classic taskbar's names) |
| `drag` | 90 | drag images, the Dock's «poof» |
| `system` | 100 | permission prompts, the keyring password prompt, Bluetooth pairing, «Keep this display?», the file chooser |
| `input` | 110 | the input method's candidate window |

Inside one layer, what opened last is on top. The lock screen and the login screen are another
session (`lord-sessiond`, see `SESSIONS.md`), above all of this. Menus of Wayland apps
(`xdg_popup`) are painted by the compositor above everything (`wl_paint_popups`).

## Using it

```js
import { Layer, Overlay, LAYERS } from '@lordos/sdk';

// a shell surface (themes): drawn in its layer wherever it is rendered from
<Layer name="popover"><ControlCenter … /></Layer>

// a list, popover or menu opened from a control (apps and the SDK)
<Overlay anchor={buttonRef} onClose={() => setOpen(false)}>
  <View style={{ position: 'absolute', top: 32, insetInlineStart: 0, … }}>…</View>
</Overlay>
```

- **Themes:** put every surface above the windows in its layer. The Lord Flow theme's `Menu.jsx`
  does it by itself, so any menu built with it is in `popover`. `startShell` places the SDK's own
  surfaces (search, notifications, prompts, fullscreen windows) in theirs.
- **`Overlay`:** its children are positioned against the anchor's box, so `top: h + 4, left: 0`
  still means «below the button». It is not clipped by a scroll view or the rows below it.
  `onClose` is called on a click outside (the anchor included toggles itself) and on Escape. In
  the desktop's process it goes to the `popover` layer, above the Dock and the other windows; a
  protected window's list stays black in screenshots. In an isolated app's process (`.lpk`
  apps and the system apps with `"process": "isolated"`: Passwords and, since 0.8.2, Mail, Calendar,
  Contacts and Reminders) it goes to the window's own overlay host, above all
  of the window's content (the app's surface is the window, so it can't leave it).
- Used today by: the shell's menus, the SDK's `MenuButton`, the «fill from Passwords» button
  (`AutofillButton`), Settings' pop-up buttons and Passwords' pop-up menus. Calendar, Contacts and
  Reminders keep their own per-window popover host; the Terminal's preferences use an inline list.
- **The keyboard (0.8.2).** While anything shows in the `overlay`, `modal` or `system` layers (search,
  the launcher, sheets, system prompts), no Linux app (Wayland or X11) gets the keys: `runtime/index.js` counts each
  layer's children after every commit and `sdk/wayland.js` gives the compositor keyboard focus 0
  ([PRIVACY.md](PRIVACY.md#system-prompts-take-the-keyboard-082)). `popover` (menus, the clipboard and
  emoji panels, which paste into the app in front) keeps the app's keyboard.
- **Don't** fight the order with large `zIndex` values: they only order siblings. A new kind of
  surface gets a row in the table above (and in `sdk/layers.js`) instead.

## Blurred surfaces and partial repaints

The menu bar, the Dock, menus and the Control Center blur what is behind them
(`backdropFilter`). The blur reads the screen around the surface, a margin of twice its radius
(`canvas_backdrop_blur`). lord-server only repaints the area that changed, and outside it the
screen still holds the last frame, with the surface's own tint already on it: before 0.8.1, closing
a menu or the Control Center left a lighter band in the menu bar where the repainted area met it.
Now `paint_damage` (`paint.c`, `blur_walk`) grows the damaged area to the whole surface whenever a
change touches a blurred surface or the margin its blur reads, so the blur only sees this frame.
The GPU path composites that same canvas, so it gets the same fix (tested on VM 122 on the CPU
path).

Since 0.8.2 the damage can be several rectangles painted one by one (`paint.c`, "Several damaged
rectangles"; the Monitor's moving map dots; off by default in the 0.8.2 release, `LORD_DAMAGE=multi`
turns it on, see [MONITOR.md](MONITOR.md#082-several-damaged-rectangles-vm-119)). The rule holds for each of them: a rectangle that
touches a blurred surface or its margin grows to the whole surface, and every other rectangle
touching that margin joins it, so a blur never reads a neighbour rectangle half painted
(`test_multi_damage` in `build/engine-test`, against a full repaint).

Since 0.8.6 the margin is repainted together with the surface (`blur_near` in `paint.c`, used by
`blur_walk` and by the several-rectangles rule). Outside the repainted area the screen holds the
last frame's *final* pixels, so whatever is painted above the blurred surface and reaches into its
margin (a sibling's shadow, a menu, the window's own border over a translucent Terminal) was
blurred into the surface's edges, while a full repaint blurs only what lies below it; the
difference stayed until the next full repaint. `test_damage_fuzz` in `build/engine-test` found it
(random trees through lord-server's whole damage pipeline, every painted frame compared with a full
repaint). The cost: a repaint that touches the Dock or the menu bar also repaints the band of twice
the blur radius around it (about 41 px for a 20 px blur; the screen edges cut it). On VM 116 with
Monitor's moving map, with and without a blurred Terminal over it, layout and paint per frame stayed
the same within 0.1 ms (MONITOR.md, "The 0.8.2 integration test, repeated").
The copy of what is below a dragged window (`paint.c`, [WINDOWS.md](WINDOWS.md)) follows the same
rule: since 0.8.6 it is painted again only where it changed, and a part that touches a
blurred surface below the window grows to everything that surface's blur reads. A frame that the
old rule paints whole (what is below changed twice within 8 frames) may use the copy instead, but not
when its damaged box meets a blurred surface below the window: painted whole, that blur reads the last
frame outside the box, and the copy would not give the same pixels.

A blurred surface that an opaque node painted after it covers whole (a translucent Terminal behind
Monitor's window) shows nothing of its blur, so the rule leaves it out (`blur_hidden` in `paint.c`,
0.8.6): before, every change near it repainted the hidden surface and its margin, 45 % of the screen
per frame with Monitor's moving dots over a hidden Terminal (MONITOR.md, "A blurred window hidden
behind Monitor"). The test is the opaque-cover skip's (opaque background, opacity 1 down the tree, no
transform, away from rounded corners, clipped by its ancestors), in paint order; when the window on
top moves, turns translucent or closes, the change reaches the blurred surface and the rule applies
again (`test_hidden_blur_damage`).

| | |
|---|---|
| ![before](menubar-band-before.png) | ![after](menubar-band-after.png) |
| Before: the Control Center has just closed, a lighter band from «Help» to «US» | After: the same steps, the bar is even |

## Tests

- `build/engine-test` (`make -C server test`), `test_layers`: layers created in the «wrong» tree
  order still paint and hit in `zIndex` order; an empty layer lets the click through; a `zIndex`
  inside the base can't climb above a layer; `pointerEvents: 'none'` still hides a subtree.
  `test_backdrop_band`: after a menu closes, the blurred bar is repainted whole and matches a full
  repaint (37 levels lighter without the fix).
- On the VM: the screenshots above (VM 122), clicking the crown menu's items over the Dock (they
  open their item, not the Dock's app) and the Passwords category list (picking «Licenses» changes
  the form).
