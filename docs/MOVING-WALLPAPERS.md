# Keeping moving wallpapers cheap

A moving wallpaper is a short loop (6–10 s) played forever behind the desktop, often on several displays at once
([WALLPAPERS.md](WALLPAPERS.md) covers the catalog, the formats and the settings). Nobody looks at it most of the time, so
every frame it costs is overhead. This document is how LordOS keeps that overhead small, what was measured, the security
rules of the zero-copy input, and how other desktops handle the same problem.

**Status: in development** (after 0.8.9.6.1-beta, the latest release). Code: `server/src/media/mambient.c` (lord-server),
`server/src/media/mloop.c` (the rules, tested alone), `server/src/media/helper/lm_out.c` (lord-media's side),
`server/src/gpu/gpu.c` (`gpu_yuv_*`, `gpu_pack_*`), `sdk/motion.jsx` and `sdk/motionmodel.js`.

## The starting point

Measured by the maintainer on his PC (i9-10900, Radeon RX 580 8 GB, Mesa 25 radeonsi, VA-API working; three 1920×1080
monitors, one 1080p moving wallpaper on all three) with 0.8.9.6.2's code: **lord-server ~184 % of a core, lord-media
~58 %, the GPU's 3D engine 15 %, its video decoder ~0 %.**

0.8.9.6.2's frame path ([MEDIA.md](MEDIA.md), "Video frames: a single copy"):

```
VA-API decode → read back to RAM (NV12) → swscale to BGRA at the shown size → memfd slot (4 bytes a pixel)
  → lord-server: for each display, copy into that display's canvas, repaint what's over it (the Dock's and the menu
    bar's blur), upload the changed region (the whole display) into the UI texture → compose → flip
```

Per frame and per display: a 1080p copy (8 MB), the blurs over it, and an 8 MB upload; per frame once: a GPU→CPU
read back and a colour conversion on the CPU.

## The frame path now

```
GPU composing, a moving wallpaper:
  VA-API decode → the surface exported as two dmabuf planes (no copy) → lord-server checks and imports them
  → one shader pass into an RGBA texture → every display's view draws that texture below the UI → present
  (after one whole loop: the frames are on the GPU already, lord-media is paused, the loop plays from there)

GPU composing, a dmabuf refused or impossible (a picture over twice the box, a driver that can't export):
  VA-API decode → read back (NV12) → [scaled down if larger than the box] → memfd ring at 1.5 bytes a pixel
  → one upload of the two planes → the same shader pass → the same per-view drawing

CPU composing (GPU=0, LORD_GPU=0, no GPU): exactly as before (BGRA).
```

A new frame is never UI damage on the GPU path: the views are drawn below the UI from the texture (as a Machines 3D
picture, [MACHINES-GPU.md](MACHINES-GPU.md)), so the canvas isn't repainted and nothing is uploaded for it. Only the
strips under a blurred node (the Dock, the menu bar, an open menu) keep the CPU's pixels of the frame, so they still blur
the video: those rows alone are converted (or read back from the GPU) and those strips alone are repainted
(`nv_blur_strips` in `mview.c`, `media_src_blur_damage` in `mviews.c`).

### The frosted surfaces over the video (ws170 QA BUG-14)

Those strips were repainted with **every** frame: with a 4K wallpaper on three 4K displays, Settings, Files, Notes and
the Terminal open and translucent widgets, lord-server averaged 145 % of a core (its main thread about 80 %: clearing,
re-blurring and repainting every frosted window, the menu bar, the Dock and the widgets, and reading the video's rows
back from the GPU for them, `gpu_tex_read_rows`). Profiling one 4K display's strips showed where: of a 32 ms refresh,
about 22 ms were the blur itself and 2 ms the text; caching the windows' content apart from their backdrop would have
saved little. So the blur moved to the GPU:

- **The GPU's frosted glass** (`gpu_draw_frosted` in `gpu/gpu.c`; `wl_paint_frost` in `wl/wl.c`; `frost_source` in
  `ui/paint.c`). On a GPU display, a frosted node whose backdrop is nothing but the moving wallpaper the GPU draws below
  the UI is not blurred by the CPU: the canvas keeps the transparent hole the picture left, the node's own background
  and content are painted over it as always, and the node is noted in layer 0 right after the picture. With each
  composed frame the GPU takes the picture's part under the box (and twice the radius around it) from its texture at
  1/4 (or 1/2) of the size, blurs it across and down with one Gaussian of the variance of the CPU's three box passes,
  and draws it into the box with a rounded mask; the UI texture goes over it. A new video frame then costs the CPU
  nothing there: no strip is damaged (`paint_blurs_over` leaves these nodes out), no row is read back, nothing is
  repainted or uploaded. The decision is made in paint order and is the same for the picture's strips and the node's
  paint: the picture's view must cover the node's box, opaque and unrounded, and nothing painted after it and before
  the node may reach the box (another window, its shadow, a desktop icon, an ancestor's background, an app window, a
  transform, an ancestor's rounded clip); then the CPU blurs, as before. Never in a capture (screenshots, an agent's,
  the protected-window fill), never while a remote viewer or a recording reads the canvas (`frost_want` in `main.c`:
  turning it on or off repaints everything), never on a turned display; `LORD_GPU_FROST=0` turns it off.
- **The CPU's strips that remain** (a frosted window over another one, a shadow under the menu bar) are repainted at
  most every 200 ms (`MEDIA_BLUR_MS`, `media_src_blur_frame` in `mviews.c`; `LORD_MOTION_BLUR_MS`, 0 = every frame), the
  last frame always reaching them through the ambient tick (`media_src_blur_due`).
- **A blurred node with an opaque background is not a strip** (`blur_covered` in `paint.c`): its blur never shows.

Measured: `build/engine-test bench-motion` (one 3840×2160 display at scale 1, the menu bar, the Dock, a widget and two
frosted windows with text: 4.2 Mpx of strips; the read-back not counted) gives one CPU refresh of **31–32 ms, 92–97 %
of a core per display at 30 frames/s** before; with the GPU's frosted glass all 5 boxes go to the GPU and a video frame
costs the main thread **0 ms**. `build/motion-bench frost /dev/dri/renderD128` (the RX 580 of the development machine)
checks the GPU's blur against `canvas_backdrop_blur` (mean difference 1.9–2.5 levels, largest 9, five boxes from 28 px
bars to 900 × 600 windows) and times a 4K desktop's 5 boxes: **2.5 ms of GPU work a frame and 0.1 ms of CPU**. The
full desktop has not been measured again on the maintainer's three monitors yet; the line to compare is `top` on
lord-server with Settings, Files, Notes and the Terminal open.

## Each technique

### Per-display pause (lord-server's own knowledge)

Every view of a player (one per display) is looked at four times a second (`media_ambient_tick`):

- **covered**: the union of the opaque nodes painted after it (windows, a fullscreen LordOS app, the lock screen) holds
  its whole box (`paint_node_hidden` in `paint.c`, with `ui/rectsub.h`: two windows side by side, or a window under an
  opaque bar, cover it together);
- **fullscreen**: a fullscreen Linux window is on that display (`wl_fullscreen_box`);
- **dark**: the screen is off (DPMS) or nothing is drawn.

A hidden view is not presented: a new frame composes only the displays that show it. When no view of a player shows,
the player pauses (lord-media stops decoding, the cache's clock stops) and resumes on the same frame. The decision is
the compositor's, not a guess from the theme's window list: 0.8.9.6.2's JS rule paused only when one window covered a
whole display, and paused **every** display for a fullscreen window on one of them. The JS side now keeps only what it
alone knows: Reduce motion, five minutes idle, the battery, a busy CPU (`pauseReason` in `sdk/motionmodel.js`).

### Stop, not only pause

After **10 s** with no view visible (a game in full screen, the session locked under an opaque lock screen), lord-media
is ended and the loop cache freed (`MLH_STOP`, `MLOOP_HIDDEN_STOP_MS`): its RAM and the GPU memory go back to the game.
When a view shows again lord-server starts lord-media again on the same file (the path validated again as at the
start), at the same place, and the cache is built again after one pass.

### Frame-rate cap

Settings → Wallpaper → Frame rate: **Video's own** (the default) / **30 fps** / **15 fps**. lord-media skips the frames
sooner than 1/cap after the last one sent before reading them back or converting them (`MM_FPSCAP`); the decoder still
decodes every frame, since they are references. The cache shows only the frames the cap allows (`mloop_pick`). Frames
are presented only when a new one is due, at the video's own pace or lower: a 30 fps clip never makes 60 presents a
second.

### Battery mode

Settings → Wallpaper → On battery: **Pause** (the default), **Lower frame rate** (15 fps; it still pauses at 20 % or
less), **Keep playing**. Applied as soon as the power source changes (`sdk/motion.jsx` listens to the battery store).

### NV12 instead of BGRA

lord-media sends the decoder's own 4:2:0 planes (`MF_NV12`): no RGB conversion in lord-media, 1.5 bytes a pixel instead
of 4, and lord-server converts on the GPU with the video's matrix (BT.601, BT.709 or BT.2020, limited or full range:
`nv12_matrix`, shared with the CPU conversion in `nv12.c`). A picture is never sent larger than the video: a 1080p clip
on a 1440p display goes at 1080p and the GPU scales it. lord-server asks NV12 only while the GPU composes (`MM_FORMAT`)
and BGRA as soon as it doesn't.

### The loop cache in GPU memory

After one whole pass, the loop's frames stay on the GPU and lord-server plays them at the clip's own timestamps;
lord-media is paused (no decode, no copy). The frames are packed as NV12 (RGBA8 textures holding four Y or two U-V
samples a texel, 1.5 bytes a pixel), copied on the GPU from the frame's planes, never read back (`gpu_yuv_pack`); drawing
one back is one shader pass (`gpu_pack_convert`). One cache per player, shared by every display.

- **Budget** (`mloop_budget`): Settings → Wallpaper → GPU memory for loops (Off, 256 MB, 512 MB, **1 GB**, 2 GB), never
  more than half the card's VRAM nor more than what's free past a headroom (a tenth of the VRAM, at least 256 MB), read
  from the card that renders (`gpu_cards_read`, amdgpu's `mem_info_vram_*`). Integrated GPUs (no VRAM of their own, or no
  PCIe link: APUs, Intel, the Raspberry Pi) get a sixteenth of the RAM, never more than 1 GB. A 1080p30 8 s loop is about
  705 MB; a 4K loop doesn't fit 1 GB and plays as before.
- **Recorded** only from a loop's start, at one size, without gaps (no more than three frame intervals apart) and to the
  clip's end (`mloop_rec_frame`, `mloop_rec_whole`); a pass that fails three times is not tried again until the size or
  the settings change.
- **Dropped** when the display's size or scale changes (recorded again at the new size), the frame rate is raised above
  the one it was recorded under, the setting is turned off, the GPU context is lost (a reset, a VT switch), memory
  pressure (free VRAM under half the headroom; on integrated GPUs MemAvailable under a tenth of the RAM, looked at every
  5 s), or 10 s hidden. lord-media then goes on from where the cache was.
- The loop's end stays seamless: the clips fade their last second over their first ([WALLPAPERS.md](WALLPAPERS.md),
  "Format"), and the cache keeps every frame's own timestamp.

### Zero-copy dmabuf

lord-media exports the VA-API surface (`vaExportSurfaceHandle`, `DRM_PRIME_2`, read-only, separate layers: Y as R8 and
U-V as GR88, or R16 and GR1616 for 10 bits) and sends the dmabuf fds over its existing socket with a fixed description
(`MM_DMAFRAME`, `MpDma`). lord-server checks them (below), imports each plane with `EGL_EXT_image_dma_buf_import` as an
external texture and draws the frame with the same shader. The decoder can't reuse a surface lord-server may still read:
lord-media holds each frame sent until lord-server has acknowledged one two numbers later; the GPU's implicit sync on
the buffer covers the rest (no CPU waits). Only when the picture is at most twice the box a side: the GPU then draws it down to the box in the same pass (a 4K
clip on a 1080p display is a 2 × 2 average, no shimmer); anything larger goes through the memfd ring, scaled by
lord-media. A frame drawn down isn't cached (a 4K loop wouldn't fit the budget anyway).

### A file that can't be played

A player that stops by itself is started again after a growing pause (0.5 s, 1 s, 2 s, 4 s; `sdk/motionmodel.js`
`retryDelay`), then the still poster stays. A file lord-media can't open (`media.cannotOpen`) is given up after three
failures in a row for every display (`openFailure`), its poster shows and a notification says so once; a new download
of the file clears that. Settings → Wallpaper counts a moving wallpaper as downloaded only when the variant it names is
in the store at the catalog's size (`motionHere`), and `wallpaper-get` keeps an existing file only when its size and
SHA-256 are the catalog's (otherwise it fetches it again over it). Before 0.8.9.6.2's fix (ws170 QA), choosing the same
login video twice left `.login.part` as a hard link to that video, and a still login wallpaper chosen afterwards wrote
its name into the video; `wallpaper-login` now never leaves the temporary link and writes a mark only into a new file.

## Security of the dmabuf input

lord-media decodes untrusted files and is sandboxed ([MEDIA.md](MEDIA.md), "The lord-media helper"); a dmabuf from it is
hostile input to lord-server, checked like any other:

- **Only dmabufs**: each fd's link must read `anon_inode:dmabuf` or `/dmabuf:`; a memfd, a file or a socket is refused.
- **Real sizes**: each object's size is `lseek(fd, 0, SEEK_END)` on lord-server's side, never the helper's word.
- **A closed description** (`mp_dma_check` in `proto.c`): exactly `sizeof(MpDma)` bytes, the magic, no padding bits;
  even sides of at least 2 and at most twice the box asked with `MM_SIZE` a side, plus 16 (never more than 8192); a known
  colour matrix and no other bit; 1–2 objects, as many as the fds that came; Y R8 with U-V GR88, or R16 with GR1616;
  every plane in an object that exists, its pitch at least a row and at most 64 KiB, `offset + pitch × (rows − 1) + row`
  inside the object (64-bit, no overflow), two planes in one object apart, every object used, at most 512 MB each.
- **The driver's own allowlist**: each plane's format and modifier must be one `eglQueryDmaBufModifiersEXT` offers.
- **Counted and closed**: every fd is closed after the import (EGL keeps its own reference); more than 240 frames a
  second is a flood and is cut off.
- **Any refusal is for good**: that player goes back to the memfd ring (`MF_NV12`), logged once
  ("zero-copy frame refused (…)"). A failed import or draw does the same.
- **The CPU path never takes a dmabuf**: without GPU composition lord-server asks BGRA, and an `MM_DMAFRAME` is
  acknowledged and its fds closed.
- **No new sandbox rule**: lord-media's player already had the DRM and dma-buf ioctl classes for VA-API; exporting and
  sending fds over its own socket adds nothing (no new syscall, no new device, no new path).

What a hostile decoder can still do: show wrong pixels in its own wallpaper (as it always could), write the surface
while lord-server draws it (tearing in its own picture), or make lord-server import buffers until refused. It can't
make lord-server read outside a buffer it checked, keep an fd open, or reach the CPU path. Tests:
`server/tests/mloop_test.c` (every rule above, malformed descriptions, a fuzz of 200 000 random ones checked against
the invariants) and `build/motion-bench run … dmabuf` on real hardware.

## Measurements

`build/motion-bench` (`server/tests/motion_bench.c`) launches `build/lord-media` exactly as lord-server does (sandbox
on, VA-API on the RX 580's render node) and does lord-server's per-frame work on a surfaceless GPU context of the same
card: no display, so the live session wasn't touched. Clip: one of the catalog's 4K HEVC loops re-encoded to 1080p30
HEVC, 8 s, a 1920×1080 box. CPU in % of one core over 23 s after a 2 s warm-up; the machine was busy with a WebKit build
(load average 16–17 on 20 threads) during every run, which costs the frame gaps some regularity, not the CPU numbers.

| Path | Views | lord-media | lord-server's frame work | Frames/s | GPU memory |
|---|---|---|---|---|---|
| BGRA (0.8.9.6.2) | 1 | 21.3 % | 9.5 % | 30.0 | — |
| BGRA (0.8.9.6.2) | 3 | 18.9 % | 19.6 % | 30.0 | — |
| NV12 ring | 1 | 6.1 % | 0.8 % | 30.0 | — |
| NV12 ring | 3 | 6.2 % | 0.8 % | 30.0 | — |
| Zero-copy dmabuf | 1 | 1.0 % | 0.4 % | 29.9 | — |
| Zero-copy dmabuf | 3 | 1.2 % | 0.5 % | 29.9 | — |
| Loop cache (after one pass) | 1 | 0.1 % (paused) | 0.2 % | 29.8 | 705 MB |
| Loop cache (after one pass) | 3 | 0.2 % (paused) | 0.2 % | 29.8 | 705 MB |

Other runs, one view:

| Clip and path | lord-media | lord-server's frame work | Frames/s |
|---|---|---|---|
| 1080p30, zero-copy, Frame rate 15 fps | 1.1 % | 0.3 % | 15.0 |
| The catalog's 4K HEVC (24 fps) on a 1080p box, BGRA (0.8.9.6.2) | 31.6 % | 5.2 % | 23.7 |
| The same, zero-copy drawn down by the GPU | 1.1 % | 0.3 % | 23.7 |
| The same on a 4K box, zero-copy | 1.1 % | 0.3 % | 23.7 |

"lord-server's frame work" is what the bench does per frame: in the BGRA rows a copy into each display's canvas and
that canvas's upload, in the others one upload (NV12) or import (dmabuf) or nothing (cache), plus the shader pass. It
leaves out what a real session adds on top: the blurred strips (since the ws170 QA blurred by the GPU where only the
video is under them, and the CPU's at most every 200 ms; per frame before; earlier still, with the
whole display's repaint around them) and composing the displays (GPU work, a few GL calls). So the session's 184 % is
not this table's 19.6 %: most of it was repainting and uploading each whole display, which the GPU path no longer does.
The full session with three monitors has **not** been measured yet: it needs the maintainer's desktop; the line to
compare is the `media: playback (wallpaper)` summary in `compositor.log` and `top` on lord-server and lord-media.

The shaders were checked on the same card (`build/motion-bench check`): the GPU's conversion and the loop cache's packed
copy match the CPU's (`nv12.c`) within one step in every matrix and range.

## How others handle it

| | What they do | Kind of source |
|---|---|---|
| KDE Plasma, [Smart Video Wallpaper Reborn](https://github.com/luisbocanegra/plasma-smart-video-wallpaper-reborn) | Pauses when a window is maximized or fullscreen or active, has a battery mode, and pauses with the screen off or locked ([announcement thread](https://discuss.kde.org/t/smart-video-wallpaper-for-plasma-6/18288)) | The project's documentation |
| KDE Plasma, [bug 460910](https://bugs.kde.org/show_bug.cgi?id=460910) | An animated wallpaper stops on the lock screen when a window is maximized | User report |
| KDE Plasma, [KWin and video](https://discuss.kde.org/t/kwin-wayland-hogs-cpu-when-playing-video/48811) | KWin's own CPU use with video playing | User report |
| GNOME, [Hanabi](https://www.github.com/jeffshee/gnome-ext-hanabi) | GStreamer with clappersink/OpenGL; pausing on fullscreen and on battery planned ([overview](https://www.linuxuprising.com/2023/04/hanabi-is-live-wallpaper-for-gnome.html)) | Project documentation and a review |
| Wallpaper Engine (Windows) | Pause or stop per rule (another app focused, maximized, fullscreen), stop frees RAM and VRAM, an FPS limit, per-app rules; the pause is not per monitor ([1](https://steamcommunity.com/app/431960/discussions/2/1752358461530583995), [2](https://steamcommunity.com/app/431960/discussions/2/3809529155526069935)) | Community discussions |
| mpv ([Ubuntu bug 1708102](https://bugs.launchpad.net/ubuntu/+source/mpv/+bug/1708102/comments/8)) | A UHD clip: `hwdec=vaapi` (zero-copy) ~5 % CPU, `vaapi-copy` ~40 %, software ~300 % | User report with measurements |
| macOS, [Aerial's notes](https://codeclimate.com/github/JohnCoates/Aerial/Documentation/HardwareDecoding.md/source) | AVFoundation decodes in hardware; HEVC Main10 is not always hardware-accelerated | A project's documentation |
| macOS Sonoma, WallpaperVideoExtension | CPU and RAM complaints ([Apple forum](https://discussions.apple.com/thread/255180158), [The Mac Observer](https://www.macobserver.com/tips/how-to/high-ram-usage-wallpapervideoextension/)) | User reports and a how-to article |

Where LordOS differs:

- **The pause is per monitor**: a covered display's copy stops presenting while the others play; the decoder pauses
  only when no copy shows. The others above pause globally.
- **One decoder for every display**: one player per file, its frame converted once and drawn by every display's view.
- **The loop cache**: after one pass a short loop plays from GPU memory with the decoder paused, within a budget read
  from the card.
- **A sandboxed decoder with a validated zero-copy**: the decoder runs in lord-media's sandbox (no network, no exec, no
  paths), and the dmabufs it hands over are checked by lord-server before the GPU sees them. mpv's numbers above are the
  same order as ours: zero-copy is what takes the copy cost away.
