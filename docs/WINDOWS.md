# Windows: moving, resizing and cursors

Moving a window has to look smooth even on a small machine and on the CPU path, which is the default
in the VMs and on the live ISO. Also, every window can be resized from its 4 sides and 4 corners,
and the cursor shows the arrow for each direction.

| | |
|---|---|
| ![edges](windows-resize-edges.png) | ![resize](windows-resize.png) |
| The 8 invisible edges of a window (Chromium with the theme's frame): ↖ ↕ ↗ ↔ ↔ ↙ ↕ ↘ | Resizing from the corner: Chromium redraws at the new size while dragging |
| ![move](windows-move.png) | ![gtk](windows-gtk-move.png) |
| Moving a window: move cursor, the Dock stays on top | GTK Demo (own frame, CSD) moved from its bar: lord-server follows `xdg_toplevel.move` in C |
| ![cursors](windows-resize-cursors.png) | ![app](windows-app-cursor.png) |
| LordOS cursors (`server/src/cursor.c`) at 2x over light and dark backgrounds; the red dot is the hotspot | The I-beam in Chromium's address bar: it asks for it via `cursor-shape-v1` and LordOS draws it |

The screenshots are from VM 118 on the CPU path (on the GPU path the cursor goes through the cursor
plane and `screendump` doesn't show it).

## Why it wasn't smooth (before)

Every mouse event called into JS and JS moved the window with React:

1. `pointermove` → `wm.move` → the window store changes → **the whole shell was re-rendered**: the
   desktop, every window, the Dock and the menu bar (about 38 ms per event in QuickJS);
2. then, layout and repaint of everything below and above the window in the old and new regions:
   background, other windows, the window's content, the terminal, shadows and the Dock's blur
   (about 17 ms);
3. with a 125 Hz or faster mouse an event arrived every 8 ms: events piled up in evdev (they got
   lost, sometimes the "release" too) and the screen updated at 15–18 frames per second.

## What was done

### One motion per frame (`main.c`)

Pointer motion is no longer sent on every evdev `SYN`: it is coalesced and sent **once per frame**,
when the page flip completes (`motion_pending`). Buttons and the wheel are sent immediately (with
whatever motion has accumulated up to that moment). That way a 1000 Hz mouse doesn't call JS or the
Linux apps a thousand times per second. Raw motion for games (`relative-pointer`) and the cursor
plane are still per event.

### C does the dragging (`ui/grab.c`, `lord.wm`)

The theme starts the drag and lord-server follows it on its own:

```js
wm.beginMove(id)              // in the title bar's onMouseDown
wm.beginResize(id, edges)     // edges as in xdg_toplevel.resize: 1 top, 2 bottom, 4 left, 8 right
```

- Per frame, C shifts the window's node (`left`/`top`, and `width`/`height` when resizing) and redoes
  the layout. **JS is not called and React doesn't re-render.**
- While it lasts, Linux apps don't receive the pointer (`wl_pointer_grab`: `leave`), as in Weston.
- On release, JS receives `__lordWm('end', { node, dx, dy, dw, dh })` and the SDK stores the geometry
  in the window manager. React sets the same value the node already had, and `lord.setStyle` no
  longer invalidates anything if the style didn't change (`js.c`).
- Limits: the minimum size is the manifest's `minSize` (Linux apps, 160×100). The frame doesn't go
  above the work area (the menu bar) and 48 px always remain visible.
- `xdg_toplevel.move` and `xdg_toplevel.resize` from apps that draw their own frame (GTK, Chromium
  with CSD) use the same thing (`sdk/wayland.js`). Since 0.8.3 X11 apps under Xwayland that draw
  their own frame ask with `_NET_WM_MOVERESIZE`, which the window manager for X (`wl/xwm.c`) turns
  into the same request.
- If the theme didn't mark the window's node (`wm.frameRef`), or the window is maximized,
  `beginMove` returns `false` and the theme can fall back to its old JS dragging.

### The moving window is a layer (`ui/paint.c`)

- **The layer.** When a move starts, the window's tree (with its shadow) is painted once into a
  separate buffer. Each frame it is just copied to the new position: text, terminal, images and
  shadow are not repainted.
  - It is painted in its screen coordinates (a canvas with a shifted origin). That way the terminal
    and Linux apps keep recording where they are.
- **If something inside changes,** like a terminal line or the blinking cursor, only that part of
  the layer is repainted.
- **If a node inside changes** (a style, a text, a transform such as LordBench's turning square, or a
  child moving inside the window), only its old box (moved with the window) and its new one are
  repainted in the layer, with a 4 px margin (`layer_tree_part`; 0.8.6, in development). The window's
  own style or size, a part outside the layer or bigger than half of it still repaint the whole layer.
  That gives the same pixels as repainting it whole, and the layer is repainted whole whenever that
  can't be shown (`layer_part_exact`, `layer_note_exact`):
  - the window moved by whole physical pixels since the layer was painted whole, the layer's box and
    offsets are the ones a full render would give, the display scale is whole, and positions, sizes,
    paddings and borders are in sixteenths of a pixel within ±4096 px, so every coordinate the paint
    derives from them is exact at both places;
  - text uses whole font sizes and was drawn by `text.c`'s own path in a font whose em is a power of
    two of at most 2048 units, as Inter's is (`text_drew_inexact`): glyph advances and kerning are then
    exact too. Text through `textlayout.c` (shaping, other scripts' fonts) or in another font (Plus
    Jakarta Sans and JetBrains Mono have 1000-unit ems) repaints the layer whole;
  - a transformed node that didn't change, an app window or a video inside makes it repaint whole (a
    transform composites through its matrix, which rounds differently at another place; painting an
    app window registers it);
  - a transformed node's image steps its bilinear weights along each row from its first pixel, so a
    part whose left edge cuts that image would change them: `canvas_affine_watch` reports it and the
    layer is painted whole instead (the margin keeps that from happening in LordBench's scene).
- **What's below** (everything painted before the window: background, desktop, the other windows)
  is also saved once. Per frame:
  1. the region that changed is copied;
  2. the layer goes on top;
  3. what is above (menu bar, Dock) is painted normally, because its blur depends on what lies
     beneath it.
- **If something below changes** (a clock, a video), the below part is rebuilt. If it changes very
  often, it stops being used.
- **When resizing** the window is repainted every frame (its content reflows), but what's below is
  still saved.
- **The damage** is the window's old region plus its new one.
- **On the GPU path:**
  - the below part records its app windows again every frame (`wl_paint_keep`/`wl_paint_kept`);
  - the layer remembers where the app holes are: exact alpha 0, distinguished from an empty
    background thanks to a sentinel value. They are reopened over the blend and the app is recorded
    at its new place (`wl_paint_holes(0)`).
  - a window with no app surface skips that work (0.8.6, in development): `layer.has_holes` is 0
    only while the hole map stands for all zeros, so `layer_holes` just turns the sentinel back into
    0 and `layer_paint` doesn't walk the map. A part that brings the first hole into such a map zeroes
    the rest of it first, so what is read is always what the full walk would have written. The
    sentinel fill writes one row and copies it.
- **There's no layer** if there is a backdrop blur inside (`backdropFilter`), or a video on the GPU
  path.
- **A `canvas_blit` detail:** opaque pixels are copied directly (same result as blending, without
  the math).

### Checking it (0.8.6, in development)

- **`test_drag_exact`** (`server/tests/engine_test.c`, run by `make -C server test`) drags LordBench's
  desktop scene along its figure of eight for 480 frames, plus seven short runs (a text that resizes a
  child, a child moving inside the window, a terminal's blinking cursor, a square turning across the
  window's edge, the window crossing x = 0 and y = 0 while the square turns, a child removed after
  frames that only copy the layer and then the square turning again, and the drag on a display at
  200 %; the last three with boxes and text at half pixels), on the CPU path and on the GPU path's
  canvas. Every frame's hash must match
  `server/tests/drag_golden_cpu.txt` and `drag_golden_gpu.txt`, written from the code before any change
  to this path: a faster frame has to give exactly the same pixels. The hashes are per architecture
  (x86_64 so far); `make -C server drag-golden` writes them, only on the reference code. A run added
  later gets its hashes the same way: the new test built against 001633b's code (with the few test entry
  points it lacks added beside it, which must leave the earlier runs' hashes identical), never against
  the code being checked.
- **Against a full repaint** the test prints a second number, which may only go down: today a drag
  frame differs from a full repaint in at most 2,265 pixels on the CPU path (black pixels at the dragged
  window's rounded top corners) and 70,940 on the GPU path (one level darker in its shadow).
- **`LORD_PAINT_VERIFY=N`** in lord-server's environment repaints the whole tree every N-th frame of a
  display into a scratch canvas, without the layer, and writes to stderr how many pixels differ from
  the frame. Use N > 1 to check the partial repaints of the layer: the check repaint also notes where
  the terminals are, so on the next frame a terminal's change repaints the whole layer.

### Resize edges (`sdk/windowshell.jsx`)

`<WindowShell>` is the window's node: the frame goes inside and around it there are 8 invisible
zones. They are 6 px outside the frame and 3 px inside. The sides carry
`ns-resize`/`ew-resize` and the corners (16 px) `nwse-resize`/`nesw-resize`. Both themes use it,
also for Linux apps with their own frame (CSD): their resize edge is in the shadow, which LordOS
crops.

```jsx
<WindowShell win={w} onMouseDown={() => wm.focus(w.id)} style={{ borderRadius: 12, overflow: 'hidden', boxShadow }}>
  <View onMouseDown={() => wm.beginMove(w.id)}>…title bar…</View>
  <AppHost id={w.id} focused={focused} />
</WindowShell>
```

### Cursors (`server/src/cursor.c`)

- **Own cursor set, drawn by LordOS.** They are vector shapes (polygons, capsules, rings with signed
  distance), rasterized with 4×4 samples per pixel at 1x (32 px) and 2x (64 px, on screens from
  2560×1440). Black fill, white outline and a soft shadow.
- **No system theme is read.** Debian doesn't install Adwaita or DMZ on the ISO, so there's no new
  dependency.
- **Shapes:** `default`, `pointer` (hand), `text`, `move`, `grab`, `grabbing`, `not-allowed`, `wait`,
  `progress`, `crosshair` and the 8 resize directions, plus `ew`, `ns`, `nesw` and `nwse`.
- **`cursor` style (CSS) on any node:** the one from the first node upward that has it applies.
  Example: `style={{ cursor: 'pointer' }}`.
- **Which cursor is shown:**
  1. during a drag, the drag's cursor: `move` or the edge arrow;
  2. over a Linux app, whatever it asks for: `wl_pointer.set_cursor` (image or hidden) or
     **`wp_cursor_shape_manager_v1`** (shape by name, used by Chromium, GTK 4 and Qt 6), drawn with
     these same shapes;
  3. otherwise, the node's.
- **Where it is drawn:** on the CPU path over the frame; on the GPU path through the KMS cursor
  plane (or as a texture if the driver has no plane). Remote desktop and recordings receive the
  image (`mirror_cursor`, `media_composed`).

### Presenting on the CPU: only what changed

Each KMS buffer receives only what changed since the last time it was drawn into. That's the region
repainted in this frame and in the previous one (double buffering), plus where the cursor was;
before, it was the whole scene (4 MB at 1280×800). They copy everything:

- the two frames after a resize or after coming back from the GPU;
- a fullscreen app.

With the Terminal being dragged, "present" went down from 0.66 to 0.35 ms per frame (and dragging
reached 95–103 frames per second: virtio-gpu completes the flip before the refresh).

## Measurements (VM 118, 1280×800 at 75 Hz, 4 vCPU)

Continuous 6 s drag in circles with a virtual tablet (uinput, 125 events/s). lord-server writes one
line per drag to `~/.cache/lordos/session.log`:

```
drag: 464 frames in 6011 ms = 77.2 FPS (464 moves); per frame: work 12.80 ms (max 36.83;
JS 0.20, layout 0.56, paint 11.43, present 0.61), max gap 36.9 ms; layer: 1023 painted, 467 copied (CPU)
```

"work" is what lord-server spends per frame: the pointer and timer JS, layout, painting
("paint") and presentation ("present").

| Path | Window | Before | After |
|---|---|---|---|
| CPU | Terminal | 17.9 FPS, 55.6 ms per frame (JS 38.2) | **77.2 FPS** (the cap is the 75 Hz refresh), 12.8 ms (JS 0.2) |
| CPU | Chromium (SSD, 900×600) | 17.1 FPS, 58.2 ms (JS 35.9) | **50.7–67 FPS**, 15–20 ms (JS 0.3) |
| GPU (`LORD_GPU=1`, llvmpipe) | Terminal | 15.3 FPS, 64.5 ms (JS 38.3) | **40 FPS**, 24 ms (JS 0.4; present with llvmpipe 10 ms) |
| GPU (`LORD_GPU=1`, llvmpipe) | Chromium | 11.5 FPS, 85.9 ms (JS 47.0) | **30 FPS**, 32 ms (JS 0.4; present with llvmpipe 12.7 ms) |
| CPU | Chromium, resizing | — | 19.5 FPS: set by Chromium, which redraws at every size |

- **"Before"** is the same tree, but with uncoalesced motion and the original themes (dragging in
  JS). The "before" lost events: out of 720 motions about 110 arrived.
- **What remains in "painting"** is mostly what's above the window: the Dock's blur and icons
  (about 4–7 ms when the window passes underneath) and copying the layer.
- **With a real GPU** (AMD), "present" is almost 0. In the VM it's llvmpipe in software.
- **Spikes of about 35 ms:**
  - the first frame (it builds the below part, which means painting the whole screen once);
  - an occasional rebuild if something below changed.

**How it was measured** (scratchpad scripts, one per VM):
- `uinput-ventanas-118.py`: runs in the VM as root and creates the tablet «LordOS prueba tableta». It
  is driven through `/run/uinput-ventanas`.
- The QMP mouse (`input-send-event`) is no good for measuring: QEMU's USB tablet has a 16-event queue
  and at 120 Hz it loses motions and even the "release".

## Several displays and HiDPI (2026-10-18)

With several displays (`DISPLAYS.md`) the layer and "what is below" belong to the display with
most of the window (`paint_layer_view`) and are in that display's physical pixels; the other
displays paint the window normally while it crosses. Dragging uses each display's work-area top
(`lord.wm.grab(…, { areas })`), so a window never goes under a menu bar. Re-measured on VM 118
(About window, 6 s circles at 125 events/s, one 1280×800 display, CPU): 6.38–6.51 ms of work per
frame before, 6.73–6.94 ms after (≈ +5 %, the per-display bookkeeping); the layer still renders
once and is then only copied. Numbers with two displays and at 150 %: `DISPLAYS.md`.

## The UI engine pass (2026-10-18)

Adding per-side borders, logical edges, ellipsis and transforms (`docs/DESIGN.md`, "Engine notes")
was checked against this path:

- **Layout of what didn't change is skipped.** A node whose subtree had no change (`node_invalidate`
  clears `layout_ok` up to the root) and that gets the same box keeps its children's layout. Moving a
  window only changes its `left`/`top` (`grab.c` calls `node_moved`), so a frame of a drag lays out the
  desktop's top level and places the rest (`place()`), instead of laying out every window.
- **A transform-only setStyle keeps the node's layout** (0.8.6, in development; `node_restyled` in
  `ui/node.c`). A change of only `rotate`, `scaleX`, `scaleY` or `transformOriginX/Y` on a node that
  already has its `StyleExt`, with no text node above it and no text or image in its subtree, marks
  it for repaint and has its parent place it again (`node_moved`) instead of `node_invalidate`.
  Layout never reads the transform and `place()` recomputes every matrix, so the result is the same;
  only that node's own layout pass is saved (its ancestors are laid out as before).
  `test_transform_restyle` compares every node's layout and every pixel with the `node_invalidate`
  path. `'transform'` itself is not a key `lord.setStyle` takes and changes nothing.
- **A main-loop pass with no timer due doesn't walk the timers** (0.8.6, in development;
  `ui/js_prelude.h`). `__lordTick` keeps the earliest deadline and returns at once when it hasn't
  come; when it has, one pass fires the due timers by deadline, then by id, as before. Clearing the
  earliest timer marks that deadline stale and it is computed again before use, so the wait returned
  to the main loop is the same number as before. lord-server makes the atoms of the three globals it
  calls on every pass once per runtime. `build/timer-order-test` runs 400 random timer scripts on this
  prelude and on the one before it and requires the same trace (callbacks, order, arguments, every
  returned wait, logs). A tick with 200 timers pending and none due went from 158.75 µs to 0.30 µs
  (VM 116).
- **Text outside the clip is not laid out** (a list row scrolled away), and a cut "…" is remembered
  like a measurement.
- **The node stays small:** the rare styles (per-side edges, per-corner radii, scale/rotate,
  `transformOrigin`) live in a separate `StyleExt`, so every walk over the tree touches the same
  memory as before (a node 240 bytes larger made the benchmark's drag about 40 % slower).
- **Transforms:** a scaled or rotated subtree is painted into its own raster and composited; inside
  a dragged window it goes into the drag layer like anything else. A transformed window is not put in
  the drag layer.

`build/engine-test bench` (`server/tests/engine_test.c`, in the `debian:trixie` build container, i9-10900,
medians of 3 interleaved runs; "before" is the same benchmark built against the tree of checkpoint
03c2569): a 900 × 600 window with a 2000-row list (icon, name with ellipsis, size, date), a 520 × 360
dialog dragged over it for 240 frames, and a Dock with a backdrop blur.

| | Before | After |
|---|---|---|
| Layout of the list, cold / warm | 18.5 / 0.9 ms | 18.8 / 0.62 ms |
| Full-screen paint | 12.5 ms | 4.8 ms |
| Scroll frame (layout + damage + paint) | 11.9 ms | 3.8 ms |
| Drag, one display (1280×800) | 1.74 ms per frame (layout 1.0) | 1.0 ms (layout 0.17) |
| Drag, two displays (1280×800 + 1920×1200 at 150 %) | 11.3 ms | 2.5 ms |

On VM 120 (live ISO, CPU path, 1280 × 800, the Terminal dragged in circles for 6 s with a uinput
tablet, lord-server's `drag:` line), switching only `lord-server`: before 7.88–7.99 ms of work per
frame (paint 7.30–7.41, layout 0.02, 104.7–105.8 FPS), after 7.98–8.37 ms (paint 7.41–7.76, layout
0.01–0.02, 101.5–105.4 FPS). The difference is within the run-to-run spread; most of that frame is
still the Dock's blur.

## Snapping (0.8.3)

A theme turns it on with `configureWindows({ snap: true })` (Lord Classic does; Lord Flow doesn't).
The geometry is in `sdk/snap.js` (no runtime imports, tested by `server/tests/snap_test.mjs`), the
window side in `sdk/windows.js`.

- **Dropping at an edge:** within 8 px of the left or right side of a display's work area (or past
  it, over a taskbar on that side) a window takes that half; in the top or bottom fifth of the side,
  a quarter; at the top edge it is maximized. Halves, thirds and quarters round to whole pixels and
  share their middle line, so two snapped windows never overlap or leave a gap.
- **The preview** while dragging: the window still moves in C (no JS per frame). Only while a move
  is going on, the SDK reads `lord.wm.pointer()` (`ui/js_wm.c`: the pointer and the drag's edges,
  no arguments, read only) every 90 ms and sets `snapPreview` when the zone changes; the theme draws
  it under the moving window. Crossing into a zone costs one rebuild of what is below the window.
- **A scripted move (0.8.6, LordBench):** `wm.scriptMove(id, { rx, ry, periodMs })` asks lord-server to move
  the window along a figure of eight through this same path (`lord.wm.script`, `grab_script_begin` in
  `ui/grab.c`: the window painted once into a layer, one step per frame from the main loop, no button held
  and no JS per frame). The positions come from LordBench's synthetic pointer (`bench/lbinput.c`, a 125 Hz
  thread on an eventfd the main loop polls like a device; taken once per frame like a real mouse's motion), so
  the test also measures pointer-to-screen latency; without that thread the path follows the clock. Pointer
  input or `wm.stopScript()` stops it. Only System Information's desktop
  scene uses it (`apps/sysinfo/scene.jsx`), to measure the compositor ([BENCHMARK.md](BENCHMARK.md)).
- **On release** (`__lordWm('end')`) the SDK reads the pointer once more and snaps; the size from
  before the first snap is kept in `w.restore`. Dragging a snapped window away gives it that size
  back under the pointer, where it was grabbed. Resizing a snapped window keeps its new size.
- **API:** `wm.snap(id, zone)` (`left`, `right`, `top-left`, `top-right`, `bottom-left`,
  `bottom-right`, `max`, `left-third`, `center-third`, `right-third`, `left-two-thirds`,
  `right-two-thirds`, or `restore`), `wm.snapKey(id, 'left' | 'right' | 'up' | 'down')` for
  Super+arrows (`snapNext` in `snap.js`), and `w.snapped` (the zone) for a theme that squares the
  corners of a snapped window.
- **Measured** (VM 119, 1280×800, CPU path, a 960 × 600 Files window dragged in circles for 6 s
  three times through QMP, about 52 moves per second): Lord Classic 7.5–7.7 ms of work per frame (JS
  0.35, paint 6.4–6.7) and Lord Flow 13.1–13.4 ms (paint 12.1–12.3, its Dock's blur), both at
  the rate of the input. A drag that keeps crossing the top edge (the preview appearing and going)
  costs 9 ms per frame (JS 1.6). Without the preview polling, 6.3–6.6 ms.

## The Window menu, the app menu and Force Quit (0.8.5)

The same menus in both themes, built by the SDK (`sdk/windowmenu.js`, `sdk/appcontrol.js`); Lord Flow
shows them in its menu bar (Window, the app's name, the crown menu), Lord Classic in each window's own
Window and Help menus (its in-window menu strip, since it has no menu bar).

- **Window menu** (`windowMenuItems`): Minimize, Zoom, Fill, Center; **Move & Resize** with the halves
  (left, right, top, bottom), the quarters and thirds (the zones of `wm.snap`, see "Snapping"), the
  arrangements of two windows (left and right, right and left, top and bottom, bottom and top) or four
  (quarters) with the windows behind it (`wm.arrange`), and Return to Previous Size (`wm.snap(id,
  'restore')`); **Move to <display>** for every other display (`wm.moveToDisplay`: a snapped window
  keeps its zone, the rest keep their place relative to the screen); Bring All to Front; and the open
  windows, the one in front checked.
- **App menu** (`appMenuItems`, Lord Flow: a click on the app's name in the menu bar): About <app> (its
  icon, name, own version and where it comes from; a system window, `apps/appabout`), Settings… (the
  app's own, ❖,), Hide <app>, Hide Others, Show All and Quit <app> (❖Q). In Lord Classic, About <app>
  is in each window's Help menu.
- **Force Quit…** (the crown menu, a system window `apps/forcequit`) lists the open apps and ends the
  chosen one without asking it (`forceQuit`): a Linux or isolated app's own process gets SIGKILL from
  lord-server through `lord.wayland.kill(surface)` (`js_sys.c`), only that client of the same user and
  never lord-server itself; an X11 window is only asked to close (every X11 app shares Xwayland); a
  built-in app that runs in the session closes without its close guard.

## Pending

- A real GPU layer (the window as its own texture that is only shifted), so nothing is painted on the
  CPU when moving with a real graphics card.
- The Dock's blur is recomputed when a window passes underneath.
- The first frame of a drag builds the below part (about 35 ms).
- `xdg_toplevel.resize` requested by a GTK app from its edge: the edge is in the shadow that LordOS
  crops, so in practice it is resized with the theme's edges (tested with Chromium; GTK Demo was
  tested by moving it).
- System cursor themes (Xcursor) and the cursor size in Settings.
