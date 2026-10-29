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

## The dragged window on the GPU ("GPU drag", 0.8.6, in development)

While a window is dragged its pixels are a cached layer blitted over a copy of what is below it
(`docs/WINDOWS.md`, "The moving window is a layer"). On the GPU path, a frame that uses that copy no
longer paints and uploads the window's rectangle: the GPU writes it, with exactly the pixels the CPU would
have written. Approved by the maintainer on 2026-10-28 (step 10 of the drag-frame work).

**What the frame does** (`paint_root_gdrag` in `ui/paint.c`, `gpu_frame_ex` in `gpu/gpu.c`):

1. `below_ready` decides, as before, whether the frame uses the copy. GPU drag only acts on those frames.
2. W is where `layer_paint` would blit the layer, cut by the window's ancestors' clips and by the
   frame's damaged box D. The CPU paints only D minus W (up to four strips), each a walk of the tree
   clipped to the strip (the copy, then from the window on); the scene keeps its stale W.
3. The GPU gets the strips (the UI texture), the layer and the copy of what is below as two textures of
   their own, each uploaded only where it changed since the GPU last had it (the layer's repainted part,
   the copy's partial rebuilds; all of it after a full render, a new size, another display or a new
   context), and draws one quad over W, after the UI texture and before the menus and the cursor
   (layer 1), with blending off.
4. The shader (`fs_gdrag`) does `blit_row`'s arithmetic at k = 256 (`canvas.c`): where the layer's alpha
   is 0 the copy's pixel; else `s + floor(d·(255−a)/256)` per colour and `a + floor(((d_a·(255−a) +
   128)·257) / 65536)` for alpha, with integers held in `highp` floats. Every intermediate value is an
   integer below 2^24 (the largest is 16,744,321) and every scaling is by 2^-8 or 2^-16, so a 24-bit
   float holds it exactly; texels are read back to integers with `floor(c·255 + 0.5)` (exact even
   through 16-bit floats) and the output c/255 is converted to c by the framebuffer as the UI
   texture's own pixels are. Over a framebuffer that is black under W (no app window is drawn there),
   writing the result is what blending the UI texture gave. The pixel it works on comes from `v_px`, a
   `highp` varying in pixels that GPU drag's own vertex shader (`vs_gdrag`) passes from the quad's corners, not
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

**The stale rectangle** (`main.c`, `gdrag_on`/`gdragw` per display): the next frame that paints the display
joins W to its damage; a frame with no UI damage (the cursor, an app's new frame) draws the window again
from the textures while they are still that display's, else paints W on the CPU; direct scanout, a
failed frame and the GPU going away (`cpu_takeover`) paint it again; the frame after the drag ends
(`paint_layer_end` damages the window's box) paints it on the CPU. When the quad cannot be drawn (an
upload failed, or the layer or output is larger than the self-test covered: GPU drag turns off) `gpu_frame_ex`
returns -2 before presenting: no frame is shown with a stale W, the display keeps the one before, and
`main.c` paints W on the CPU in the next frame without trying GPU drag (`gdragcpu`). `LORD_PAINT_VERIFY` leaves W
out of its count on GPU drag frames.

**Uploads without a stall (0.8.6, in development).** The two textures get only what changed (paint.c's dirty
rectangles, in their own pixels). A drag's start, or the copy of what is below built again whole, changes all of the
copy: a screen of pixels (8.3 MB at 1920 × 1080), and on V3D `glTexSubImage2D` converts it to the GPU's tiled layout
on the CPU inside the call, so the first GPU drag frame after such a change missed its vblank. Now:

- `paint_root_body` refuses a frame GPU drag would compose when the textures still lack more than `GDRAG_FEED_PX` pixels
  (262,144, 1 MB; both textures counted) as `GDRAG_R_FEED` ("catching up"): the CPU paints W, the same pixels.
- After such a frame's flip is queued (`main.c`, after `gpu_frame_ex`), while the CPU waits for the vblank anyway,
  `paint_gdrag_feed` sends up to `GDRAG_FEED_PX` pixels of what is missing through `gpu_gdrag_feed`: the copy first, then the
  layer, rectangles cut by rows, each sent rectangle (or part) dropped from the dirty list. Each texture is asked
  first with an empty rectangle whether it holds that display's pixels at that size; if not, it is allocated
  (`glTexImage2D` with no data), marked as that display's, and all of it is sent in slices. Only frames refused for
  `GDRAG_R_FEED` send slices: sent while GPU drag is refused for another reason (the menu bar or the Dock in the damage),
  most of it would change again before it is used (`engine-test`: 162.8 million pixels against 2.4 million).
- Each texture knows whose pixels it holds (`gdragtout[2]`); a GPU drag frame that would find another display's or the
  wrong size still sends all of it (correct, and rare now). The dirty list keeps `GDRAG_DIRTY` (16) rectangles and,
  when full, merges the two whose box adds the fewest pixels (it used to grow the last one).
- Frames don't overlap (a frame starts after the last flip), so the job that read the textures has finished when
  they are written: no second copy of them is needed. If frames are ever built while a flip is pending, each
  texture needs a pair, each with its own dirty list. GLES3 pixel buffers (the GPU doing the tiling) are not used.

`engine-test`'s stub GPU does the same as `main.c` and `gpu.c` (a texture made again is filled with a pattern no
frame has, so a pixel the slices missed shows): every frame matches, and it checks that no GPU drag frame has more than
`GDRAG_FEED_PX` of the copy left to send.

**Off unless it proves itself:** `gpu_init` sets GPU drag up. It needs a fragment shader with at least 24-bit
`highp` floats (`glGetShaderPrecisionFormat`) and then a self-test, drawn into a framebuffer, read back
and compared with `canvas_blit` on the CPU (the NEON row on the Pi):
- arithmetic: every valid premultiplied (alpha, colour ≤ alpha) over every destination value (three of
  them a pixel, the destination alpha varying), every source alpha over every destination alpha, and
  random valid pixels, 3,145,728 pixels in three 1024 × 1024 tiles;
- placement: a framebuffer as large as the largest output and at least 4096 × 2048 (else the largest
  the GPU allows, and the log says `placement in WxH, the largest framebuffer the GPU allows`), a copy
  of what is below of that size and a layer at an odd offset (37, 21) that reaches its far edges, every
  pixel a different random value, so a texel read from the wrong place shows. An output larger than
  that is refused (`gpu_gdrag_ready(out)`), and a layer larger than it turns GPU drag off.

One differing pixel turns it off. The outcome is kept per GL renderer and version across contexts, so a
GPU restart doesn't run it again. The log says `gpu: GPU drag on: … self-test: N pixels, every valid
premultiplied combination and a WxH placement pass, the same as the CPU's, T ms` or `gpu: GPU drag off: …`
with the reason.

| Switch | Effect |
|---|---|
| `LORD_GPU_DRAG=0` | GPU drag off: the dragged window is painted by the CPU as before |
| `LORD_GPU_VERIFY=N` | every N-th GPU drag frame W is also painted on the CPU (a walk clipped to W on a scratch canvas) and compared with the framebuffer read back: `gpu-verify: frame N W=wxh differ=K worst=D` |

The `drag: …` and `drag (scripted): …` lines end with `GPU drag: N of M frames (Q quads), strips … Mpx,
uploaded below … Mpx, layer … Mpx (not: … layer, … clip, … ancestor, … app, … after, … post, … catching up;
layer: invalid …, holes …, …)`: frames composited, of those that used the copy; quads drawn; what the CPU painted
around W; what went up to the two textures (in GPU drag frames and in slices between frames); why the others were not;
and the `layer` refusals by cause (`GDRAG_L_*` in `ui/ui.h`: `gone`, `invalid`, `resize`, `rendering`, `capture`,
`view`, `transform`, `raw`, `holes`, `surfaces`, `premul`, `nobuf`, `tree`, `opacity`, `part`). The
`drag (scripted): …` line then ends with the frames' phases (`docs/BENCHMARK.md`, "Frame phases").

**Checked** (VM 116, x86_64):
- `canvas-test`: the shader written in C with the same float operations (`canvas_gdrag_model`) against
  `blit_row_c` and the SIMD row: all 8,421,376 valid (alpha, colour, destination) combinations, every
  alpha pair, and 200,000 random pixels, 0 differ. The NEON row was not run (no ARM machine was used).
- `engine-test`'s `gpu-drag` path: `test_drag_exact` with GPU drag and a stub GPU (the UI texture, the two textures
  with only their dirty rectangles, `main.c`'s bookkeeping, W written by `canvas_gdrag_model`) gives every
  one of the 2,417 frames of `drag_golden_gpu.txt`; 250 of 1,856 frames that use the copy are GPU drag frames
  there (241 with the uploads in slices: 9 frames catch up; no GPU drag frame sends a texture whole, and the most one
  sends fell from 934,022 to 170,330 pixels). Five more runs are compared frame by frame with the GPU path: a pause with no damage, two
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
  GPU drag off. `main.c`'s side of -2 has been built, not run.
- At runtime with llvmpipe (`LORD_GPU=1`; an isolated session lent the card by `tests/seat_cycle.c`, a
  scene moved by `lord.wm.script`; before the fixes above, with the self-test of that time: tiles only,
  run in the first drag frame): the self-test passes in 75–113 ms; with `LORD_GPU_VERIFY=1`, 1,481
  GPU drag frames of 1,483 and 0 pixels differ; across 4 context re-creations during the move, 1,328 GPU drag frames,
  0 differ; `tests/gpu_cycle.sh` (10 cycles) keeps 38 fds and the same memory. Layout and paint of a
  drag frame: 0.29–0.35 ms with GPU drag, 0.92 ms without (llvmpipe's composition is most of the frame, so
  this is no measure of the gain on a real GPU).
- With the default theme (the same kind of session, Calculator dragged over the remote desktop socket,
  real screenshots with QMP `screendump`): 152 of 153 frames GPU drag while the window stays in the band the
  menu bar and the Dock leave free, 7 of 153 along a figure of eight that reaches them; `LORD_GPU_VERIFY=1`
  over 160 GPU drag frames (W up to 462 × 592): 0 pixels differ. The nodes painted after the window are kept
  as a list that grows as needed (up to 65,536 boxes; a box inside the one noted before it is skipped, so
  the menu bar and the Dock are 2 boxes): a fixed list of 128 was full with LordOS's own shell and GPU drag
  refused every frame. LordBench's Desktop impact at 1280 × 800 gets 1 GPU drag frame of 2,024: its window,
  with its shadow, always reaches the Dock's or the menu bar's blurred backdrop, and its turning square
  makes the layer be painted whole. Its numbers with GPU drag on and with `LORD_GPU_DRAG=0` are the same within
  llvmpipe's noise.

**Not checked yet:** the Raspberry Pi 5 (V3D), the machine this is for: its self-test, `LORD_GPU_VERIFY=1`
over full LordBench desktop runs alone and under load, a perf run, a real screenshot and one run with
`LORD_SW_CURSOR=1`. Until then no gain is claimed. Near the menu bar or the Dock the frame is still
painted by the CPU.

## The frame pipeline (0.8.6, in development)

Until now a display's next frame was started only after its page flip completed: pointer input, LordBench's
scripted step, the JS frame callbacks, layout, paint, the texture upload, the GPU's draw and the flip ran one after
another, one frame in flight. When that whole chain takes more than a refresh period, every frame waits for the
second vblank and the rate locks at half the refresh. That is the Raspberry Pi 5 under an all-core load
(`docs/BENCHMARK.md`, "The desktop under load"): about 13 ms to build a frame, 3–5 ms more of GPU and submission,
98 % of the frames late, 30 fps; alone it builds in 7 ms and shows 60.

**What changed** (`main.c`; the decisions in `fpipe.c`, without I/O, tested by `build/fpipe-test`): while the flip
of frame N is pending, the CPU part of frame N+1 is done ahead (it is *prepared*): pointer input and LordBench's
scripted step are dispatched, the JS frame callbacks and the layout run (`paint_ui`) and its damage is painted into
the display's scene (`paint_out`, `out_prepare`), keeping what the submission needs on the display (`Out`): the rect
it painted, its painted rectangles for the CPU path, its GPU drag result (`gdragtaken`, `gdrag`), the cursor and app flags
(`cdirty`, `wl_new`). When N's flip event arrives, only what is left is done for N+1 (`out_submit` →
`out_present`, the same code a frame built at once uses): the texture upload, `gpu_frame_ex` (or the copy into the
dumb buffer on the CPU path) and the page flip. Then GPU drag's textures catch up and N+2 is prepared. The upload copies
the scene when it is called (`glTexSubImage2D` from client memory), so painting the next frame into the scene
right after it is safe; the dumb buffers are written only at submission, when the flip has freed one.

**When it applies** (`pipe_on`): one display with real page flips. Not without a monitor (no flip: the headless
path is unchanged), not when the driver completes flips at once and frames are paced in software (`soft`, a
virtual card: the wait there is the pacing slot, not the flip), and not with several displays (paint.c's drag
layer and GPU drag state are shared between them; each frame is built and submitted at once there, as before). Both the
GPU and the CPU composition paths use it.

**The switch** `LORD_PIPELINE`, read once at startup (the session log says which): `0` off (exactly the previous
behaviour), `2` always, anything else (and unset, the default) *while frames are heavy*: on after 2 frames in a row
whose CPU time (JS frame callbacks, layout, paint, upload, swap and flip ioctl; for a prepared frame its prepare
plus its submission) is above 60 % of the refresh period, off after 60 frames in a row below 40 % (`fpipe_note`;
`-d` logs each change). The auto mode exists because the pipeline costs a frame of latency when frames already
fit: input that comes after a frame was prepared waits for the next one, shown a vblank later. When frames don't
fit, the latency stays about the same (input waits for at most one flip and is shown two vblanks after it, where
before the flips themselves came every other vblank) and the rate can double.

**The rules for each case:**

- **Idle desktop, input:** with nothing pending, a frame is built and flipped at once, as before. While a flip is
  pending and nothing is prepared, input is dispatched at once (before, it waited for the flip) and, if it damages
  something, painted ahead; only once a frame is prepared (or was tried) for that flip does input wait for it
  (`fpipe_blocked`). At most one frame is prepared at a time.
- **A prepare that paints nothing** (a JS frame callback with no damage) marks the display `FPIPE_EMPTY`: nothing more
  is tried until the flip, and the frame is then built as before, so there is no busy loop.
- **What changes between paint and submission** is not painted into the prepared frame: it is shown as it was
  painted, exactly as a frame built at that moment would have been, and the change goes into the next frame. The
  pointer is the exception it always was: the cursor is drawn at submission (CPU path) or moved on its plane at
  once. A Linux app's new buffer is drawn at submission (GPU path: its texture), so it is never older than before.
- **A prepared frame that can no longer be submitted as painted** is undone (`prep_undo`): what it painted, and the
  window rectangle it left to GPU drag, join the next frame's damage, and that frame is built at once. That happens when
  the scene was reallocated, the GPU context or output changed (`gpu_generation`), or, for a GPU drag frame, the drag
  layer began, ended, moved to another display or reallocated its buffers (`paint_gdrag_epoch`, `ui/paint.c`; the
  frame's `GpuDrag` points into those buffers). Every path that changes the GPU or the displays also undoes it first:
  `cpu_takeover` (screen recording, a remote viewer with protected windows, a failed frame), `gpu_start`,
  `outputs_apply` (hotplug, Settings → Displays) and `display_detach` (a session switch: the card's master is
  dropped with nothing left prepared). A frame prepared before the screen went off (idle, DPMS) is undone too, so it
  is never shown stale when the screen comes back.
- **Never while the flip is pending:** stopping the GPU for a recording or a remote viewer with protected windows
  (`cpu_switch`: `gpu_shutdown`, and `cpu_takeover` sets the dumb buffer with a modeset) waits for the flip, as it
  did before; a frame prepared for the GPU meanwhile is undone there and painted again on the CPU path.
- **GPU drag:** the prepared frame keeps its `GpuDrag` (dirty rectangles, strips, `LORD_GPU_VERIFY`'s reference) until its
  submission, and `paint_gdrag_done` runs only when its quad is drawn, as before. GPU drag's textures catch up
  (`paint_gdrag_feed`) after the submission and before the next frame is painted, so the feed never runs between a
  frame's paint and its upload (a frame built at once queues its catch-up for the end of `render`, so nothing is
  prepared in the same call after it). `engine-test` runs every drag case again with each frame presented only after the
  next frame's changes are made ("gpu-drag pipelined") and checks the same golden hashes, and that those changes never
  move the epoch.
- **-2 (unshown)**, direct scanout of a fullscreen app, the remote desktop's read-back and the protected windows'
  black: decided at submission, by the same code as before.
- **Frame callbacks and presentation:** the Linux apps' frame callbacks (`wl_frame_done`) go with a submitted frame,
  never with one only prepared; presentation feedback still follows the main display's flip events.

**LordBench's phases** (`docs/BENCHMARK.md`, "Frame phases") record a prepared frame at its submission:
`paintMs` is its prepare's, `uploadMs`, `swapMs` and `flipIoctlMs` its submission's, `renderMs` both, the new
`heldMs` the time it waited for the flip, and the synthetic pointer's latency ends at its own presentation
(`lbf_painted`, `lbf_submitted`).

**Not measured yet:** the Raspberry Pi 5 with its monitor, the machine this is for: LordBench's Desktop impact
alone and under load with `LORD_PIPELINE` unset and with `LORD_PIPELINE=0`. VM 116 has no free DRM card (its
virtio-gpu card belongs to lord-sessiond, and its flips would be paced in software anyway), so on it only the
headless path ran (unchanged: `prepared 0`), with the unit tests and `engine-test`'s pipelined drag runs. No gain
is claimed until the Pi run.

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
screen without a monitor), or (0.8.6, in development) when the whole frame (layout, paint, upload and submit)
takes more than 60 % of it, and off after 60 frames in a row with painting under 25 % and the frame under 45 %.
The second rule is for the GPU path under load: on the Pi 5 painting took about 7.5 ms there while the frame took
about 13 ms and missed the vblank, and the pool stayed off. Its gain there is not measured yet. On VM 116 (8 cores, Xeon 1.8 GHz), a
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
