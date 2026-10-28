# GPU in LordOS: composition, dmabuf and games

lord-server has two paths for building the screen:

- **CPU** (the usual one): the UI and the app windows are copied into the canvas and a dumb
  buffer is sent with a page flip. It needs nothing besides the kernel.
- **GPU** (new, `server/src/gpu/gpu.c`): Mesa's GBM + EGL + GLES2. The user approved it on
  2026-10-16 for games and GPU apps on AMD and Nvidia. See "Dependency policy" in
  `ARCHITECTURE.md`.

Modesetting is still our own (`kms.c`, ioctls without libdrm). GBM is created on the same card fd,
and its buffers and the apps' dmabufs become framebuffers with `DRM_IOCTL_MODE_ADDFB2`.

## Screenshots (VM 116, Mesa llvmpipe forced with `LORD_GPU=1`)

| | |
|---|---|
| ![ui](gpu-ui.png) | ![gtk](gpu-gtk.png) |
| The LordOS UI composited by EGL/GLES2 | GTK Demo (shm) under the Dock: the stacking order between app windows and LordOS windows is respected |
| ![menu](gpu-menu.png) | ![dmabuf](gpu-dmabuf.png) |
| GTK menu (xdg_popup) on the top layer | `dmabuf-client`: an app's dmabuf buffers (udmabuf, linear) at 72 FPS, 400/400 presented |
| ![fs](gpu-fullscreen.png) | |
| Fullscreen: the compositor presents it on top of everything | |

## How the path is chosen

1. `LORD_GPU=0`: always CPU.
2. The kernel is asked which driver the card has (`DRM_IOCTL_VERSION`). If it's one without 3D
   (simpledrm, bochs, cirrus, qxl, ast, mgag200, vkms, udl, virtio-gpu without virgl…), the CPU
   is used **without loading Mesa**: lord-server stays at ~32 MB.
3. Otherwise, GBM + EGL are set up. If `GL_RENDERER` is a software one (llvmpipe, softpipe), it
   falls back to the CPU, because compositing with llvmpipe costs more than our own path (see
   measurements). `LORD_GPU=1` forces the GPU anyway, for testing.
4. If a frame fails (eglSwapBuffers, ADDFB2, page flip), the GPU is turned off and the CPU path
   takes over.

Diagnostics: `LORD_GPU_DEBUG=1` shows the dmabuf formats, failing imports and the frames sent by
direct scanout. The log says `gpu: GPU composition (renderer…)` or why not.

**Linux apps' frames (0.8.1 fix).** A Linux app's new frame on the GPU path is only a texture,
not UI damage. Every GPU display is therefore marked (`Out.wl_new` in `main.c`) and presents it in
the next frame. Before 0.8.1 nothing was flipped until something else changed, so typing in
Chromium showed up to a second late. Measurements and the key latency trace are in
[TROUBLESHOOTING.md](TROUBLESHOOTING.md), "Typing feels slow".

**VMware virtual machines** (VMware SVGA, the `vmwgfx` kernel driver). `vmwgfx` is not in the
list of plain drivers, so Mesa is loaded: with the machine's 3D acceleration on, Mesa's `svga`
driver (`vmwgfx_dri.so`, in `libgl1-mesa-dri`, which the ISO ships) is the renderer and lord-server
composites on the GPU (`GL_RENDERER` starts with `SVGA3D`); with 3D off, Mesa falls back to
llvmpipe and lord-server to the CPU path. Which one ran is in the session log: in a LordOS
Terminal, `grep gpu: ~/.cache/lordos/session.log` (for example `gpu: GPU composition (SVGA3D; …)`
or `gpu: the renderer is software (llvmpipe …): CPU composition`). Not verified: the Proxmox test
machines' "VMware compatible" display is QEMU's 2D-only `vmware-svga`, which `vmwgfx` refuses
(since 0.8.1 it isn't even loaded there; see `ISO.md`, "Firmware and displays"), so only the
presence of `vmwgfx_dri.so` in the image was checked there. On a real VMware Fusion guest (0.8.5-beta,
`results/0.8.5-beta/vmware-fusion/`) the session composes on the GPU with the SVGA3D renderer, and the
Test Bench measured 59.8 frames a second at 60 Hz (p99 17.9 ms).

**Flips that complete at once (0.8.5).** `vmwgfx` completes each page flip immediately instead of at the
next vblank, so before 0.8.5 the compositor and every app followed it at 500–580 frames a second on a
60 Hz display, burning CPU on frames nobody sees. `main.c` now counts flips that complete within an
eighth of a refresh period (`flip_instant`); from three on it paces frames itself to the mode's refresh
on a fixed grid (`pace_next`), as headless sessions already were. A flip that really waits for vblank
lowers the count again, so real graphics cards are unaffected. Measured on the VMware Fusion guest when
it was fixed: 59.8 frames a second at 60 Hz, p99 18.4 ms. The same grid paces sessions without a monitor, so waiting in whole
milliseconds no longer turns 60 frames a second into 57.

## How it composites

- **The UI** (React → CPU canvas) is a fullscreen texture. Per frame only what changed (the
  damage) is uploaded, with `GL_EXT_unpack_subimage` (`GL_UNPACK_ROW_LENGTH_EXT` = the scene's
  stride). Without that extension (Mesa lists it for every GLES2 driver; it could be missing on a
  vendor's own GLES stack) the damage goes up as whole rows, in one call while the scene's rows are
  contiguous, or one row at a time otherwise; the log says `damage is uploaded as whole rows`.
- **App windows** don't go through the canvas. Wherever a window goes, the UI leaves a
  **transparent hole** (alpha 0) and the surface is recorded. When presenting:
  1. the window textures (layer 0);
  2. on top, the UI with its premultiplied alpha;
  3. on top of everything, the apps' menus, fullscreen and the cursor (layer 1).

  If a LordOS window (or the Dock) covers an app's window, it really covers it. If one app covers
  another, too: the hole was opened later.
- **The canvas keeps alpha.** Our own blends (`canvas.c`, `term_paint.c`) now compute the output
  alpha. Nothing changes on the CPU path (KMS is XRGB).
- **A window being dragged** is painted from a cached layer. On the GPU path only the pixels of that
  layer that lie inside an app surface are taken as holes (0.8.5): before, every fully transparent
  pixel was, including those outside a window's rounded corners, which then showed black while
  dragging. (Black corners in the remote desktop's frames of a dragged window are a known issue of
  0.8.5-beta, still being looked at.)
- **Background blur** (Dock, Control Center) also carries alpha. Over an app window it tints but
  doesn't blur, because the window isn't in the canvas. Blurring app content is pending; it needs
  a GPU blur pass.
- **Content:**
  - shm: the usual copy, uploaded to the surface's texture when it changes;
  - dmabuf: `EGL_EXT_image_dma_buf_import(_modifiers)` and `GL_OES_EGL_image_external`, with no
    copies. One EGLImage per buffer, reused.
- **Repainting:** when an app only changes its content, the UI isn't repainted (no CPU per app
  frame); it's just presented again. If the size or geometry (the shape of the hole) changes, it
  is.
- **Direct scanout:** if the fullscreen window has a dmabuf the size of the output
  (XRGB/ARGB8888, one plane), there are no menus on top and the cursor goes through the cursor
  plane, its buffer goes straight to the primary plane (`PRIME_FD_TO_HANDLE` + `ADDFB2` + page
  flip): zero copies. The buffer is returned to the app when it's no longer on screen.
- **Cursor:** on the GPU path it goes through the KMS cursor plane (`DRM_IOCTL_MODE_CURSOR2`), so
  moving the mouse composites nothing. If the driver has no cursor plane, it's drawn as a quad
  (`LORD_SW_CURSOR=1` forces it).

## The dragged window on the GPU ("G0", 0.8.6, in development)

While a window is dragged its pixels are a cached layer blitted over a copy of what is below it
(`docs/WINDOWS.md`, "The moving window is a layer"). On the GPU path, a frame that uses that copy no
longer paints and uploads the window's rectangle: the GPU writes it, with exactly the pixels the CPU would
have written. Approved by the maintainer on 2026-10-28 (step 10 of the drag-frame work).

**What the frame does** (`paint_root_g0` in `ui/paint.c`, `gpu_frame_ex` in `gpu/gpu.c`):

1. `below_ready` decides, as before, whether the frame uses the copy. G0 only acts on those frames.
2. W is where `layer_paint` would blit the layer, cut by the window's ancestors' clips and by the
   frame's damaged box D. The CPU paints only D minus W (up to four strips), each a walk of the tree
   clipped to the strip (the copy, then from the window on); the scene keeps its stale W.
3. The GPU gets the strips (the UI texture), the layer and the copy of what is below as two textures of
   their own, each uploaded only where it changed since the GPU last had it (the layer's repainted part,
   the copy's partial rebuilds; all of it after a full render, a new size, another display or a new
   context), and draws one quad over W, after the UI texture and before the menus and the cursor
   (layer 1), with blending off.
4. The shader (`fs_g0`) does `blit_row`'s arithmetic at k = 256 (`canvas.c`): where the layer's alpha
   is 0 the copy's pixel; else `s + floor(d·(255−a)/256)` per colour and `a + floor(((d_a·(255−a) +
   128)·257) / 65536)` for alpha, with integers held in `highp` floats. Every intermediate value is an
   integer below 2^24 (the largest is 16,744,321) and every scaling is by 2^-8 or 2^-16, so a 24-bit
   float holds it exactly; texels are read back to integers with `floor(c·255 + 0.5)` (exact even
   through 16-bit floats) and the output c/255 is converted to c by the framebuffer as the UI
   texture's own pixels are. Over a framebuffer that is black under W (no app window is drawn there),
   writing the result is what blending the UI texture gave. The pixel it works on comes from `v_px`, a
   `highp` varying in pixels that G0's own vertex shader (`vs_g0`) passes from the quad's corners, not
   from `gl_FragCoord`: GLSL ES 1.00 makes that `mediump`, and with 16-bit floats it cannot tell x or y
   from the next pixel past 1024. Dithering is off for the quad.

**When it is not used** (the frame is painted as before; the reasons are counted in the `drag` lines):
the layer is not valid or would be painted whole this frame, has another opacity, has an app window in it,
or has a pixel with a colour above its alpha (`layer.premul_bad`: there `blit_row`'s C and NEON versions
differ from each other); W is empty; an ancestor of the window paints over W after it (a border, the
restore of rounded corners); an app window of layer 0 is drawn into W (`wl_draws_meet`); the paint order
around the window is unknown, or a node painted after the window that paints anything meets D (a view
with `overflow: hidden` and rounded corners counts even when it has no colour: the save and restore of its
corners around its children change what is below them; with a backdrop blur's reach: it would read the stale W, and a blur or a turned image paints differently when
D is cut into strips); or something changed while the strips were painted, and then W is painted by the
CPU in the same frame (post). Also not with several damaged rectangles, a rotated display, a fullscreen
app, or on another display than the one that owns the layer.

**The stale rectangle** (`main.c`, `g0on`/`g0w` per display): the next frame that paints the display
joins W to its damage; a frame with no UI damage (the cursor, an app's new frame) draws the window again
from the textures while they are still that display's, else paints W on the CPU; direct scanout, a
failed frame and the GPU going away (`cpu_takeover`) paint it again; the frame after the drag ends
(`paint_layer_end` damages the window's box) paints it on the CPU. When the quad cannot be drawn (an
upload failed, or the layer or output is larger than the self-test covered: G0 turns off) `gpu_frame_ex`
returns -2 before presenting: no frame is shown with a stale W, the display keeps the one before, and
`main.c` paints W on the CPU in the next frame without trying G0 (`g0cpu`). `LORD_PAINT_VERIFY` leaves W
out of its count on G0 frames.

**Off unless it proves itself:** `gpu_init` sets G0 up. It needs a fragment shader with at least 24-bit
`highp` floats (`glGetShaderPrecisionFormat`) and then a self-test, drawn into a framebuffer, read back
and compared with `canvas_blit` on the CPU (the NEON row on the Pi):
- arithmetic: every valid premultiplied (alpha, colour ≤ alpha) over every destination value (three of
  them a pixel, the destination alpha varying), every source alpha over every destination alpha, and
  random valid pixels, 3,145,728 pixels in three 1024 × 1024 tiles;
- placement: a framebuffer as large as the largest output and at least 4096 × 2048 (else the largest
  the GPU allows, and the log says `placement in WxH, the largest framebuffer the GPU allows`), a copy
  of what is below of that size and a layer at an odd offset (37, 21) that reaches its far edges, every
  pixel a different random value, so a texel read from the wrong place shows. An output larger than
  that is refused (`gpu_g0_ready(out)`), and a layer larger than it turns G0 off.

One differing pixel turns it off. The outcome is kept per GL renderer and version across contexts, so a
GPU restart doesn't run it again. The log says `gpu: G0 on: … self-test: N pixels, every valid
premultiplied combination and a WxH placement pass, the same as the CPU's, T ms` or `gpu: G0 off: …`
with the reason.

| Switch | Effect |
|---|---|
| `LORD_G0=0` | G0 off: the dragged window is painted by the CPU as before |
| `LORD_GPU_VERIFY=N` | every N-th G0 frame W is also painted on the CPU (a walk clipped to W on a scratch canvas) and compared with the framebuffer read back: `gpu-verify: frame N W=wxh differ=K worst=D` |

The `drag: …` and `drag (scripted): …` lines end with `G0: N of M frames (Q quads), strips … Mpx,
uploaded below … Mpx, layer … Mpx (not: … layer, … clip, … ancestor, … app, … after, … post)`: frames
composited, of those that used the copy; quads drawn; what the CPU painted around W; what went up to the
two textures; and why the others were not.

**Checked** (VM 116, x86_64):
- `canvas-test`: the shader written in C with the same float operations (`canvas_g0_model`) against
  `blit_row_c` and the SIMD row: all 8,421,376 valid (alpha, colour, destination) combinations, every
  alpha pair, and 200,000 random pixels, 0 differ. The NEON row was not run (no ARM machine was used).
- `engine-test`'s `g0` path: `test_drag_exact` with G0 and a stub GPU (the UI texture, the two textures
  with only their dirty rectangles, `main.c`'s bookkeeping, W written by `canvas_g0_model`) gives every
  one of the 2,417 frames of `drag_golden_gpu.txt`; 250 of 1,856 frames that use the copy are G0 frames
  there. Five more runs are compared frame by frame with the GPU path: a pause with no damage, two
  displays side by side, the window over an app window below, the window against a rounded,
  bordered container's corner, and (`corner`) a rounded `overflow: hidden` box with no colour painted
  after the window. Taking out the after-node, app-window or ancestor check fails it; without counting
  the rounded clipping box as painting, 59 of `corner`'s 91 frames differ.
- The self-test headless (a test driver that includes `gpu.c`, a surfaceless GLES2 context on llvmpipe,
  VM 116): it passes, 11,373,321 pixels with a 4096 × 2048 placement pass for a 1920 × 1080 output in
  311–406 ms, 17,678,025 pixels (5120 × 2880) for a 5K output in 520 ms, about 1 ms when kept from an
  earlier context. With the shader changed to round `v_px` as 16-bit floats would, the tiles pass and the
  placement pass fails from x = 1024 on (6,207,545 pixels differ): the tiles alone would not catch it.
  With a layer larger than the placement covered, `gpu_frame_ex` returns -2, presents nothing and turns
  G0 off. `main.c`'s side of -2 has been built, not run.
- At runtime with llvmpipe (`LORD_GPU=1`; an isolated session lent the card by `tests/seat_cycle.c`, a
  scene moved by `lord.wm.script`; before the fixes above, with the self-test of that time: tiles only,
  run in the first drag frame): the self-test passes in 75–113 ms; with `LORD_GPU_VERIFY=1`, 1,481
  G0 frames of 1,483 and 0 pixels differ; across 4 context re-creations during the move, 1,328 G0 frames,
  0 differ; `tests/gpu_cycle.sh` (10 cycles) keeps 38 fds and the same memory. Layout and paint of a
  drag frame: 0.29–0.35 ms with G0, 0.92 ms without (llvmpipe's composition is most of the frame, so
  this is no measure of the gain on a real GPU).
- With the default theme (the same kind of session, Calculator dragged over the remote desktop socket,
  real screenshots with QMP `screendump`): 152 of 153 frames G0 while the window stays in the band the
  menu bar and the Dock leave free, 7 of 153 along a figure of eight that reaches them; `LORD_GPU_VERIFY=1`
  over 160 G0 frames (W up to 462 × 592): 0 pixels differ. The nodes painted after the window are kept
  as a list that grows as needed (up to 65,536 boxes; a box inside the one noted before it is skipped, so
  the menu bar and the Dock are 2 boxes): a fixed list of 128 was full with LordOS's own shell and G0
  refused every frame. LordBench's Desktop impact at 1280 × 800 gets 1 G0 frame of 2,024: its window,
  with its shadow, always reaches the Dock's or the menu bar's blurred backdrop, and its turning square
  makes the layer be painted whole. Its numbers with G0 on and with `LORD_G0=0` are the same within
  llvmpipe's noise.

**Not checked yet:** the Raspberry Pi 5 (V3D), the machine this is for: its self-test, `LORD_GPU_VERIFY=1`
over full LordBench desktop runs alone and under load, a perf run, a real screenshot and one run with
`LORD_SW_CURSOR=1`. Until then no gain is claimed. Near the menu bar or the Dock the frame is still
painted by the CPU.

## The CPU path on several threads (0.8.5)

Without a GPU (a virtual machine like VM 116, a board without a working driver) the CPU path paints
every frame. Its heavy pixel loops run on a small pool of threads (`server/src/par.c`, up to 8): the
backdrop blur in phases (downsample, the box passes by rows and by columns, the upsample), and the
row-local primitives (fills, strokes, gradients, blits, shadow masks) as the same call on each
thread with the clip cut to a band of rows; the remote desktop's tile comparison and the copies into
the encoder's slots too. The node tree, caches and everything else stay on the main thread, and the
pixels are the same as on one thread (`blurbench`/`rrbench` compare them byte for byte).

Waking the pool costs CPU, so it is used only while frames are heavy: it turns on when painting takes
more than 70 % of a frame at the target rate (the monitor's refresh, or the fastest remote viewer's
screen without a monitor) and off after 60 light frames in a row. On VM 116 (8 cores, Xeon 1.8 GHz), a
1920×1080 session with a 60 fps video under blurred windows: 45 frames a second on one thread, 55–58
with the pool. `canvas.c` builds at -O3 (its loops vectorize); the blur itself went from 28 ms to
2.7 ms over the day's work.

## When the GPU context is re-created

The EGL/GBM context does not live as long as the session. It is torn down and made again when the
session leaves the monitor and comes back (login screen, lock, user switch: `SESSIONS.md`), when a
remote viewer connects and leaves, when the displays change and when a frame fails. Every texture
and EGLImage dies with its context, so:

- **Each context has a generation** (`gpu_generation()`), and each `GpuTex` remembers its own.
  A texture from an earlier context is *stale*: `gpu_draw` skips it, `gpu_tex_pixels` makes a new
  one and `gpu_tex_free` only frees its memory (its GL name may already belong to a texture of the
  new context, such as the UI's).
- **dmabuf imports are cached per `wl_buffer` and per context** (`dm_tex` in `wl.c`): a buffer is
  imported again the first time it is drawn in a new context. A failed import is remembered only
  for that context.
- **Going away** (`gpu_shutdown`): while the old context still works, the compositor reads back the
  last frame of every window showing a dmabuf (`gpu_tex_read`) so the CPU path can keep drawing it,
  and frees its textures. Off the GPU, linear dmabufs are mapped and copied as always; one the CPU
  can't read (tiled) is held and the last good frame stays on screen. Buffers in a format the last
  GPU context imported are still accepted while off the GPU.
- **Coming back** (`gpu_init`): held buffers are imported again (no need to wait for the app's next
  frame), shm textures are uploaded again and everything is repainted.
- **Feedback:** if the format table or the device changed (GPU ↔ CPU), the `zwp_linux_dmabuf_feedback_v1`
  objects are sent the new one, so apps reallocate buffers the new path can use.
- **Direct scanout:** the buffers on the primary plane are released with the old context.

Before 0.8.1 the cached imports and window textures were reused in the new context: after the
login screen or a lock, Chromium's window (and other app windows, shm ones too) came back black.
Test with `tests/gpu_cycle.sh` (needs a free card: stop `lordos.service` first; `build/seat-cycle`
plays `lord-sessiond`'s part, lending the card and taking it back N times and saving what the CRTC
shows). VM 119, virtio-gpu with llvmpipe (`LORD_GPU=1`), Chromium (`--ozone-platform=wayland`,
which draws with shm there) and `dmabuf-client`, 10 and 20 cycles: Chromium keeps drawing every
time it's shown again (before the fix it was black from the second time on), the session's fds
stay flat (29) and RSS levels off at ~196 MB, as without the fix (llvmpipe's own memory);
`LORD_GPU=0`, 5 cycles: 69 MB flat. The VM can't import a real GPU dmabuf (see "Untested"), so the
re-import of an EGL-imported buffer was checked by reading the code path, not on screen;
`gpu_tex_read` was checked against freshly uploaded textures (0 differing pixels).

| | |
|---|---|
| ![before](gpu-relogin-before.png) | ![after](gpu-relogin-after.png) |
| Before: Chromium black after the session came back to the monitor | After: still drawing after 10 off/on-monitor cycles |

## New protocols

| Protocol | Version | What it does |
|---|---|---|
| `zwp_linux_dmabuf_v1` | 4 | The apps' GPU buffers, with feedback: format and modifier table, main render node. With GPU, whatever EGL imports; on the CPU path, only linear ARGB/XRGB8888, which is mapped (`DMA_BUF_IOCTL_SYNC`) and copied. If EGL can't import a linear dmabuf, it's mapped anyway |
| `wp_presentation` | 1 | Time and vblank number of the flip that showed each frame (KMS's `flip_ns`/`flip_seq`) |
| `wp_viewporter` | 1 | Cropping and scaling of a surface (GPU path; the CPU path scales with a bilinear blit since `DISPLAYS.md`) |
| `zwp_relative_pointer_manager_v1` | 1 | Raw mouse motion, without edges (first-person games) |
| `zwp_pointer_constraints_v1` | 1 | Lock or confine the pointer to the window; locked: no `motion`, fixed cursor |
| `xdg_toplevel.set_fullscreen` | — | Now real fullscreen: it takes the output, above everything, with the keyboard; **❖Esc** exits |
| `wl_pointer.set_cursor` | — | The app's cursor (image or **hidden**, as games do) while the pointer is over it |

Several displays and HiDPI (`DISPLAYS.md`): one GBM/EGL surface and UI texture per display on a
single context; app textures are shared and drawn at each display's scale; `wp_fractional_scale_v1`
and `zxdg_output_manager_v1` are there. Direct scanout only with one display.

Not there: `xdg_activation` and explicit sync (`linux-drm-syncobj`). dma-resv
implicit sync is used.

## Tested (VM 116 `lordos-gpu`)

There's no real GPU in the VM: the Proxmox host only has the ASPEED card, with no render node, so
virgl doesn't start. virtio-gpu 2D was used with software Mesa inside (llvmpipe on kms_swrast).
That really exercises the GBM/EGL/GLES/KMS code, but **not** the AMD or Nvidia drivers.

| Test | Result |
|---|---|
| UI via EGL/GLES2 (llvmpipe, `LORD_GPU=1`) | looks the same as via CPU (screenshot) |
| GTK Demo (shm), menus, Dock on top | correct layer order; clicks and menus work |
| `glmark2-es2-wayland` 640×400 | 75 FPS on GPU (synced to the 75 Hz vblank); uses shm, because llvmpipe in the app doesn't use dmabuf |
| `vkcube` (lavapipe) | works (300 frames), via shm |
| `dmabuf-client` (linear udmabuf) in a window | GPU: 72 FPS, 400/400 presented (wp_presentation); CPU: 131 FPS |
| `dmabuf-client --fullscreen` 1280×800 | GPU: 42.9 FPS; CPU: 42.5 FPS; on top of everything and with keyboard |
| `dmabuf-client --lock` (lock + relative + hidden cursor) | locked; relative events arrive; 0 absolute while locked; hidden cursor; on GPU and on CPU |
| neverball (SDL2 + OpenGL over Wayland) | forced GPU: ~20 frames/s; CPU: ~24 frames/s (llvmpipe: the game and the compositor compete for the same CPU) |
| Automatic selection in the VM | "virtio_gpu without 3D" → CPU without loading Mesa: 32 MB of RSS |
| `LORD_GPU=0` | everything as before |

lord-server CPU with `glmark2-es2-wayland` in a window for 10 s:

| Path | CPU | RSS |
|---|---|---|
| GPU with llvmpipe (`LORD_GPU=1`) | ~300 % (llvmpipe composites 1280×800 per frame with several threads) | 158 MB |
| CPU | ~100 % | 36 MB |

With llvmpipe the GPU isn't worth it; that's why CPU is the default. With a real card,
compositing is the GPU's work, and the CPU only uploads what changed in the UI.

**Untested:**
- **Real AMD and Nvidia:** pending the live ISO on the user's PC (AMD).
- **Importing a GPU dmabuf via EGL:** virtio-gpu doesn't accept dmabufs from another source, so in
  the VM that import fails (`EGL_BAD_ALLOC`) and udmabufs are mapped.
- **Direct scanout:** in the VM it fails for the same reason. The code is there and falls back to
  compositing if it fails.

To test it on the AMD PC (live ISO):
1. Boot.
2. In the LordOS Terminal, `grep gpu: ~/.cache/lordos/session.log` (or System Information →
   Hardware → Graphics, 0.8.5): it must say
   `GPU composition (AMD Radeon …)` and `cursor by hardware (cursor plane)`.
3. `vkcube`, `glmark2-es2-wayland`, a game (SuperTuxKart, 0 A.D.).
4. With `LORD_GPU_DEBUG=1`, in fullscreen `direct scanout: N frames` must appear.

## Test client

`server/tests/dmabuf_client.c` (`make -C server build/dmabuf-client`, needs
`libwayland-client` and `/dev/udmabuf`, which in Debian belongs to the `kvm` group):

```sh
build/dmabuf-client [--fullscreen] [--lock] [--frames N] [--size 800x600]
```

It prints frames, FPS, presented/discarded and, with `--lock`, the pointer events.

## Packages

To build with GPU (if they're missing, `make` builds without GPU, `GPU=0`):

```sh
sudo apt-get install -y --no-install-recommends libgbm-dev libegl-dev libgles-dev
```

At runtime: `libgbm1`, `libegl1`, `libgles2`, `libegl-mesa0` and the driver
(`libgl1-mesa-dri`: radeonsi/AMD, iris/Intel, nouveau; Nvidia: its driver with GBM, or NVK).

Only for testing in VM 116: `mesa-utils`, `mesa-utils-bin`, `mesa-vulkan-drivers`,
`vulkan-tools`, `glmark2-es2-wayland` and `neverball`. The user `lord` was added to the `kvm`
group for `/dev/udmabuf`.

On the Proxmox host `libgl1` and `libegl1` were installed (`vga: virtio-gl` requires them). Still,
virgl doesn't work without a render node on the host, and VM 116 was left with `vga: virtio`.

## VM 116 (`lordos-gpu`)

- Full clone of 115 (`qm clone 115 116 --name lordos-gpu --full`), with `vga: virtio`
  (2D).
- IP 172.17.0.52 via cloud-init (`ipconfig0`), snippet `local:snippets/lordos-gpu.yaml` (same as
  115's with another hostname), new MAC.
- It's for the GPU path. 115 is still the development VM and wasn't touched.

## Pending

- Test on real AMD (live ISO) and on Nvidia.
- UI blur that includes app content (GPU pass).
- Direct scanout of non-fullscreen windows (overlay planes).
- `linux-drm-syncobj` (explicit sync, the one Nvidia prefers), `xdg_activation`, tearing control (`wp_tearing_control`) and VRR.
- Letting JS know a window is fullscreen (today it receives the `fullscreen` event, but the themes
  ignore it).
