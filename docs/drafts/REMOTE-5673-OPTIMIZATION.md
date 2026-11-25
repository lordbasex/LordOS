# Remote desktop on 5673: faster and sharper (analysis draft)

Draft, 2026-11-25, branch `test/integration-08961` (0.8.9.6-beta plus integration). **Analysis only:** no code was
changed, built or run for this document. Numbers marked *measured* come from existing docs; everything marked
*estimate* is reasoning from the code and must be measured before anyone relies on it (section 7 has the plan).
Line numbers are from this branch.

Related: [REMOTE.md](../REMOTE.md), [SCREEN-SHARING.md](../SCREEN-SHARING.md), [WAYLAND.md](../WAYLAND.md),
[GO-LINK.md](../GO-LINK.md), [BENCHMARK.md](../BENCHMARK.md), [PERFORMANCE-0.8.9.md](../PERFORMANCE-0.8.9.md),
and the parallel draft [NATIVE-REMOTE-APP.md](NATIVE-REMOTE-APP.md) (its sections 2, 3.2–3.5 and 3.10 overlap
this one; section 5 below reconciles them).

## 0. Summary

The 5673 pipeline is already frugal on the **CPU path** (headless sessions, the Raspberry Pi, VPS): it compares
only the tiles the compositor damaged, encodes only when something changed, sends the cursor apart and adds
lossless PNG tiles once the screen is still. The expensive and the broken parts are elsewhere:

1. **The GPU path (the RX 580 PC, a Pi with a monitor) is full-frame at every step**: a synchronous full-frame
   `glReadPixels` inside the frame, a single-threaded swizzle over the whole frame, then `mirror_frame` with no
   damage (every 64×64 tile compared every frame). GO-LINK.md's Pi measurement (lord-server at 97 % of a core while
   sharing with a monitor) points at this read-back. The damage the GPU path needs already exists: the ScreenCast
   portal computes it (`main.c:1739`, `wl_content_track`/`wl_content_take`, `wl.c:474-481`).
2. **VA-API is mis-tuned in three ways that hurt latency and quality**: it never follows the bridge's bit rate (stays at
   the 4000 kbit/s it was opened with), FFmpeg's default `async_depth=2` holds every packet back by one frame (so the
   last frame of a change waits for the 70 ms "flush" frame), and its stream carries no colour tags.
3. **On the maintainer's PC the encoder probably isn't VA-API at all.** `media_render_fd` opens the first render node
   (`renderD128`), which on this PC is the Intel UHD 630 (`pci-0000:00:02.0-render`), and no Intel VA driver is
   installed (only `radeonsi`, `r600`, `nouveau`, `virtio_gpu`, `d3d12`). The RX 580 is `renderD129`. libx264 would be
   used, silently. *To verify* with the top bar ("H264 por software") or the journal line `remote desktop video: … with
   libx264`.
4. **The browser's jitter buffer is the largest single latency term** (*measured* 15–80 ms while dragging, of a 72–130
   ms click-to-pixel). The page already sets `jitterBufferTarget = 0` and `playoutDelayHint = 0`, which only lower
   the minimum; the RTP `playout-delay` header extension (min = max = 0) is what makes Chrome render frames as they
   decode, and the bridge negotiates no header extension at all.
5. **Anything protected on screen (since 0.8.9.6 every password field) moves the whole session, monitor included, to
   the CPU path** while a viewer is connected, and turns the remote frame into a full-frame compare.

Top five improvements (section 6 has the full list):

| # | Change | Expected gain (*estimate*) | Effort |
|---|---|---|---|
| 1 | VA-API fixes: right render node, `async_depth=1`, follow the rate (reopen on large changes), colour VUI | PC: from x264 (~10–13 ms/frame, ~1 core) to VCE (a few ms, ~0 CPU) if F3 holds; −1 frame of latency (−16 ms at 60 fps, up to −70 ms for the last frame of a burst); 4 → up to 12 Mbit/s on LAN | S |
| 2 | Playout-delay extension (min 0, max 0) in the bridge's SDP and RTP | −15 to −60 ms click-to-pixel in Chrome/Edge | S–M |
| 3 | GPU path: damage-driven, asynchronous read-back (fence + region read, no main-loop stall, BGRA read, damage to `mirror_frame`) | PC/Pi with monitor: lord-server CPU from "a core" to a fraction; removes a per-frame stall of a few ms | M |
| 4 | Damage-driven encode input: dirty rects to `lord-media`, convert only those to YUV, x264 `mb_info` (`AVVideoHint`) for unchanged macroblocks | Pi 5 (x264): typing/small changes from ~13–17 ms to a few ms per frame; less CPU everywhere | M |
| 5 | Pacing and rate control in the bridge: a packet pacer, RTT from receiver reports, REMB/loss/RTT together, keyframe size cap | fewer keyframe-burst losses and PLI loops on Wi-Fi; steadier bit rate | M |

Zero-copy dmabuf → VA-API (no read-back at all) is the end state for the PC, but it needs a maintainer decision
(section 8, D1) and builds on #1 and #3.

## 1. The frame path today

### 1.1 Paths

There are three ways a frame reaches the encoder, chosen per frame in `main.c`:

| Path | When | Where the frame comes from | Damage passed to `mirror_frame` |
|---|---|---|---|
| **A. Memory** | headless session (Pi without monitor, VPS, a second user's session) or "capture all" | `present_memory` (`main.c:2210-2241`): the scene itself, no copy, unless a fullscreen app is painted over a copy (`canvas_copy`, `main.c:2218-2221`) | the drawn box; **none** while fullscreen and the frame after (`main.c:2213-2215`, `2238`) |
| **B. CPU, monitor** | CPU composition, or anything protected on screen (`remote_needs_cpu`, `main.c:939-943`) | `present_cpu` (`main.c:2099`), after `media_composed` (`capture.c:137-158`) | the KMS buffer's `need` box; **none** while fullscreen, and **none** when the frame is the capture copy (`main.c:2168-2170`) |
| **C. GPU, monitor** | GPU composition, nothing protected | `gpu_read_next` → `read_back` inside `gpu_frame` (`main.c:2362-2366`, `gpu.c:1407-1410`, `gpu.c:927-943`) | **always none** (`main.c:2441`) |

Only one display feeds 5673: `feed_out` (`main.c:634-644`), the main display, or all displays composed again on the
CPU into `s->cap` when Settings → Displays → "capture all" is on (`main.c:2626-2639`).

### 1.2 Step by step

| Step | Code | What happens | Damage-driven? |
|---|---|---|---|
| Compose | `paint_out`, `gpu_frame_ex` | as for the monitor; with a viewer, a fullscreen app is composed instead of scanned out (`main.c:2394`) | yes (paint.c damage) |
| Read-back (C) | `gpu.c:927-943` | `glReadPixels` of the **whole** frame, RGBA, into `rb`, **before** `eglSwapBuffers`, then a single-threaded loop that swaps R/B and flips rows over the whole frame | **no** |
| Capture copy (B, protected) | `capture.c:84-96`, `137-158` | a second CPU canvas in capture mode, repainted in the changed region; with a fullscreen app also a full copy (`capture.c:143-155`) | partly |
| Compare | `mirror.c:671-739`, `646-669` | per 64×64 tile, `memcmp` against `fb` (the last frame), copy the changed tiles into `fb`, mark per-viewer dirty maps; every tile at least every 500 ms (`FULL_CHECK_MS`); rows over the thread pool | yes on A/B, **no** on C |
| Changed box | `mirror.c:724-735` | **one** bounding box of the changed tiles goes to the encoder | box only |
| Encoder slots | `mstream.c:208-243` | copy the box (union of what each of the 3 sealed-memfd slots missed) from `fb` into a free slot; `MM_INFRAME` to `lord-media` | box only |
| Pacing | `mstream.c:218-220`, `241` | at most one frame per `1000/fps − 1` ms (fps = fastest viewer's screen, 24–240); a free slot or the frame waits; one extra identical frame 70 ms after the last change | change-driven |
| Colour conversion | `lm_stream.c:237-247` | `sws_scale` BGRA → YUV420P (x264, VP8) or NV12 (VA-API), **whole frame**, `SWS_POINT`, BT.709 limited | **no** |
| Upload (VA-API) | `lm_stream.c:250-259` | `av_hwframe_transfer_data` of the whole NV12 frame into a VA surface | **no** |
| Encode | `lm_stream.c:79-162` | see 1.3 | x264 skips cheaply; VA-API fixed cost |
| Back to lord-server | `lm_stream.c:59-77`, `mstream.c:65-96` | `MM_PACKET` chunks over the socket, reassembled | — |
| To each bridge | `mirror.c:449-462` | `RP_VFRAME` over `remote.sock`; a viewer >3 MB behind skips to the next keyframe | — |
| Packetize, encrypt, send | `conn.c:686-692`, `rtc.c:667-715` | RFC 6184 FU-A/VP8 payloads of ≤1100 bytes, SRTP, sent **back to back** with no pacing; 1024 packets kept for NACK | — |
| Browser | `client.html:359-371`, `467-480` | `<video>` with `jitterBufferTarget = 0`, `playoutDelayHint = 0`; a canvas above it for the lossless tiles; cursor as a CSS cursor | — |

Copies of a 1920×1080 frame (8.3 MB BGRA), *estimate* per changed frame:

| | A (headless) | C (GPU) |
|---|---|---|
| GPU → CPU | — | 8.3 MB, synchronous |
| swizzle + flip | — | 8.3 MB read+write, one thread |
| compare | changed tiles only | 8.3 MB compared every frame |
| into `fb` | changed tiles | changed tiles |
| into the slot | bounding box | bounding box |
| BGRA → YUV | 8.3 MB → 3.1 MB | 8.3 MB → 3.1 MB |
| upload | — (x264) | 3.1 MB (VA-API) |

### 1.3 The encoders today

**VA-API H.264** (`open_h264_vaapi`, `lm_stream.c:79-110`): Constrained Baseline, VBR with `bit_rate = maxrate =
kbps`, `rc_buffer_size` = half a second, GOP 60 s (`gop_size = fps * 60`), no B-frames, keyframes only when asked.
Not set: `async_depth` (FFmpeg 7.1 default **2**, and `h264_vaapi` advertises the `delay` capability: `ffmpeg -h
encoder=h264_vaapi` on this machine), colour VUI (no `color_range`/`colorspace`/`primaries`/`trc`, unlike the x264
path), `low_power`, `max_frame_size`, `sei` (default: identifier + timing + recovery point + A/53, a few bytes per
frame for nothing). Its first keyframe is decoded and checked before anything goes out (`lm_encheck.c`).

**libx264** (`open_sw`, `lm_stream.c:131-142`): `ultrafast`, `zerolatency`, Baseline, CRF 22 capped by VBV
(`rc_max_rate = kbps`, buffer ⅓ s), `scenecut=0`, `forced-idr`, `repeat-headers`, deblocking back on, 2 threads.
**libvpx VP8**: `realtime`, `cpu-used=8`, no lag, error-resilient, `static-thresh=100`.

**Rate control.** The bridge measures (`rtc.c:752-792`): receiver-report loss > 25 % → ×0.75, > 5 % → ×0.92, none for
1 s → ×1.25 (×1.06 for 5 s after congestion) + 50; REMB is a ceiling only when it fell by >15 % or right after loss;
range 300–12000 kbit/s (`conn.c:978`, `rtc.c:743`). `RP_RATE` → `mstream` takes the slowest viewer → `MM_RATE` →
`vid_rate` (`lm_stream.c:286-294`), which **changes nothing for VA-API** ("the others keep their rate") and only the
VBV cap for x264. The encoder starts at the 4000 kbit/s every bridge asks for in `RP_VIDEO` (`conn.c:906-911`).
**So VA-API runs at 4 Mbit/s for the whole connection** while the bridge believes it raised it to 12.

**Frame rate.** A different fastest-viewer rate **stops and restarts** the encoder (`mstream.c:176`): a new process
setup, a keyframe, and for VA-API the 1–8 frame first-keyframe check again.

**Sound:** Opus 64 kbit/s mono, 20 ms, `lowdelay` (unchanged by this analysis).

### 1.4 The lossless refresh

Once the screen has been still for 250 ms (`mirror.c:43`), each viewer's dirty tiles go as **raw BGRA** rows over
`remote.sock` (`RP_RECT`, `mirror.c:233-259`), the bridge copies them, converts to RGB and makes PNG with
stb_image_write at compression level 2 (`conn.c:627-646`, `enc.c:17-43`), halving tiles above 250 KB, over the
`tiles` data channel. Any change clears that area on the page at once (`RP_DAMAGE`, `mirror.c:743-766`). This is the
"sharp text" mechanism and it works: it is exact (4:4:4, lossless) where the screen is still. Its cost is the
bridge's PNG work and a burst of bytes right when the screen settles.

### 1.5 Where the time goes (*estimates*, to be measured)

*Measured* (REMOTE.md, VM 122, no GPU): click to pixel 72–130 ms, median ~90; network RTT 1 ms; browser jitter
buffer 15–80 ms while dragging; decode 2.6 ms. x264 12–13 ms per frame at 1280–1568 px (VM 116). Pi 5 sharing with a
monitor: lord-server 97 %, lord-media 52 % of a core, x264 17.45 ms per 1080p frame, ~21 fps (GO-LINK.md §6).

| Term | Today (*estimate*) | Lever |
|---|---|---|
| Wait for the next compose | 0–16 ms at 60 Hz | none (vblank) |
| GPU read-back (C) | GPU drain + 8 MB transfer + swizzle: a few ms on the RX 580, much more on the Pi's V3D/CMA | #3, D1 |
| Compare + copies | ~1–3 ms on the i9 at 1080p (C); small on A | #3 |
| Slot wait + pacing | 0–16 ms | pacing to presentation (2.5) |
| BGRA → YUV | 2–6 ms at 1080p (single-threaded sws) | #4 |
| Encode | x264 7–17 ms; VA-API a few ms **plus one frame held** by `async_depth` | #1, #4 |
| Packetize/send | <1 ms (bursty) | #5 |
| Network | ~1 ms LAN | — |
| Jitter buffer | 15–80 ms | #2 |
| Decode + paint | 3–10 ms | — |

## 2. What the new Wayland work enables

### 2.1 Encode only damaged regions / skip unchanged frames

**Already done on A and B:** frames go to the encoder only when a tile changed, and the CPU path's damage (UI plus
Linux apps) narrows the compare. Since 0.8.9.5 a Linux app's damage is honoured (`wl.c:496` `damage_box`,
`commit_damage`), so a blinking caret in foot is now a caret-sized box instead of a window (WAYLAND.md "Frames, titles
and damage": foot 1910 → 210–360 ms of lord-server CPU over 20 s). On A and B that translates directly into fewer
tiles compared and smaller slot copies. Xwayland sends `damage_buffer` too, so X11 apps benefit the same way.

**Not done:**

- **The GPU path passes no damage** (`main.c:2441`). The accumulator exists: the ScreenCast portal collects the UI
  damage (`main.c:1739` → `cast_damage`) and, because on the GPU path an app's new frame is only a texture, the boxes
  of the Linux windows that committed (`wl_content_track`/`wl_content_take`, `wl.c:474-481`), now tight thanks to the
  honoured damage. Feeding the same union (plus the software cursor's box when there is no cursor plane, and GPU drag's
  strips) to `mirror_frame` lets the GPU path compare only those tiles, and lets the read-back itself read only that
  region (2.2). `FULL_CHECK_MS` (every tile twice a second) stays as the safety net it already is.
- **Fullscreen** (both CPU paths) passes no damage. Since 0.8.9.6 (#232) a fullscreen window's subsurface repaints
  only its own box (`wl.c:549-562` `damage_sub`, `fs_sig`); that box can be passed instead of NULL. On a headless Pi
  with a fullscreen video, `present_memory` also copies the whole scene into `o->frame` every frame
  (`main.c:2218-2221`) before painting the fullscreen app over it: a damage-limited copy follows from the same box.
- **The encoder input is one bounding box** (`mirror.c:724-735`): a change at the top-left and one at the bottom-right
  copy the whole frame into the slot. A tile bitmap (or up to 8 rects, as paint.c already keeps, `DAMAGE_MAX`) for
  the slot copies is cheap.
- **The encoder ignores damage.** H.264 can't encode part of a frame, but two things scale with damage:
  - **Colour conversion.** Keep a persistent YUV frame in `lord-media` and convert only the dirty rects (aligned to
    2) instead of the whole frame; x264 copies its input anyway, and VA-API uploads it. Needs the rects in
    `MM_INFRAME` (validated by `lord-media`: inside the frame, at most N).
  - **x264's `mb_info`.** FFmpeg 7.1's libx264 wrapper has `mb_info` (confirmed: `ffmpeg -h encoder=libx264`): with
    `AV_FRAME_DATA_VIDEO_HINT` side data (`AVVideoHint`, rects of changed or constant blocks) x264 marks unchanged
    macroblocks constant and skips their analysis. With `ultrafast` most of x264's per-frame cost is analysis of the
    whole frame, so typing or a small animation should cost a fraction of a full frame. This is the Pi's main lever.
  - VA-API has ROI side data (`AV_FRAME_DATA_REGIONS_OF_INTEREST`), driver-dependent; VCE 3.4 support is unknown. Its
    per-frame cost is low anyway; not a priority.

### 2.2 Explicit sync: reading GPU buffers at the right moment

`linux-drm-syncobj-v1` itself is about app buffers coming *into* the compositor; nothing in it touches the remote
frame. What carries over is the machinery and the rule it follows ("nothing ever blocks: waits go through eventfds",
AGENTS.md): `syncobj.c` already exports a dmabuf's fences as a sync_file (`DMA_BUF_IOCTL_EXPORT_SYNC_FILE`,
`syncobj.c:147`), and `gpu.c` already makes native fence fds (`eglDupNativeFenceFDANDROID`, `gpu.c:556`, `1482-1486`)
for LordBench's frame phases.

Today's read-back is the opposite of that rule: `glReadPixels` into client memory makes the driver finish the whole
frame and copy it before `eglSwapBuffers`, on the main loop, every frame a viewer watches. The non-blocking version:

1. Read into a pixel-pack buffer (`GL_PIXEL_PACK_BUFFER`; needs a GLES 3 context, `gpu.c:516` asks for GLES 2 today,
   both the RX 580 and the Pi's V3D offer 3.x in the same Mesa: no new library), only the damaged region (2.1), in BGRA
   where `GL_EXT_read_format_bgra` exists (`dmaread.c:307` already checks it) so the R/B swap goes away.
2. Put a native fence after it, dup it as an fd, poll it in the main loop like the syncobj eventfds.
3. When it signals, map the buffer and call `mirror_frame` with that frame's damage: one frame later, but the
   compositor never waits. The vertical flip can be folded into the draw (render the capture target upside down) or
   done per row band on the pool.

This removes the stall from every composed frame; the remote frame arrives ~one compose later than the monitor's,
which it already does in practice because the encoder works asynchronously.

### 2.3 Zero-copy dmabuf into VA-API

The end state for a machine with a hardware encoder: the compositor blits its frame (with protected areas filled, 2.8)
into one of three capture BOs (GBM, linear or a modifier the encoder imports), exports each once as a dmabuf, and
passes the fds to `lord-media` at encoder start (as `MM_INRING` does with the memfd today). Per frame, `MM_INFRAME`
names the slot and carries a sync_file for the blit; `lord-media` imports the BO as a VA surface
(`VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2`, through FFmpeg's `AV_PIX_FMT_DRM_PRIME` → `hwmap` to VAAPI), converts BGRA
→ NV12 on the GPU (VA-API video processing, `scale_vaapi`'s path), encodes, and frees the slot. No pixel crosses the
CPU: the read-back, compare, slot copy, sws and upload all disappear.

Conditions and costs:

- **Same GPU.** The encoder must run on the compositor's render node (see F3). A cross-GPU import (Intel iGPU encoder,
  AMD compositor) needs a linear BO and PCIe traffic; possible but not the first step.
- **Compare and damage.** Without a CPU copy there is no tile compare. Change detection comes from the compositor's
  damage (2.1), which is now accurate enough to trust, with a periodic full frame as the safety net. The lossless
  tiles need CPU pixels: read back only the still tiles, only when the screen settles (rare, cheap).
- **Protected windows** must be filled on the GPU before the blit (2.8), or the zero-copy path is off while anything
  protected shows.
- **Security**: a new interface (a dmabuf of the composed screen into a sandboxed helper). `lord-media` already
  receives the render node and may use DRM/dma-buf ioctls with it (Studio, hardware decode), and it already sees the
  whole screen through the memfd, so no new data is exposed; but AGENTS.md describes `lord-media`'s inputs as files,
  devices and sealed memfds, so this is the maintainer's decision (D1).

### 2.4 Frame pacing tied to presentation

Today the encoder is fed by a timer: the newest changed frame when `1000/fps − 1` ms have passed and a slot is free
(`mstream.c:218-223`), stamped with the feed time (`mstream.c:237`, `t - e->t0`). With a 144 Hz monitor and a 60 Hz
viewer, or a 24 fps video, that samples composed frames unevenly (judder), and a frame that waited for a slot carries
a later timestamp than when it was composed.

Since 0.8.9.5 the compositor knows each display's vblank grid (`wl_presented`, `wl.c:6905`; `wlt_next_vblank`) and,
headless, runs its own software vblank (`softpace.h`) at the viewer's rate. Using that:

- stamp each frame with its **compose/flip time** (the RTP timestamp then matches when it was on screen, which is what
  the browser's jitter estimator compares arrival times against);
- headless (A): the compositor's software vblank already runs at `mirror_refresh()`; feed on that tick rather than a
  separate timer;
- with a monitor at a different rate than the viewer: pick frames on a 1/viewer-Hz grid of *presentation* times;
- for a fullscreen or dominant **video surface** (commit-timing targets, fifo cadence), follow the app's own presented
  cadence (24/30/60) instead of the viewer's Hz: no duplicated or skipped video frames.

Gain: smoothness more than latency; it also keeps the browser's jitter buffer from growing (2.6's #2 removes most of it
anyway in Chrome).

### 2.5 Content-type hints

`content-type-v1` is stored per surface and "nothing uses it yet" (`timing.c:143`, WAYLAND.md "Frame timing"). For
5673, combined with the damage area:

- **video/game dominates the damage** (a surface with that type covers most of the changed tiles): don't schedule
  lossless tiles over it (they are replaced at once and only cost PNG work), favour frame rate (keep fps, let QP rise),
  follow the app's cadence (2.4);
- **none/photo or LordOS UI only** (text, terminals, documents): favour sharpness: lower CRF/QP for x264
  (`crf` can be changed between frames: the FFmpeg wrapper reconfigures x264 when it changes), allow the rate to drop
  when bandwidth is short rather than blur, and keep the lossless refresh (which is what really makes text exact);
- apps rarely set it (mpv and games do; browsers mostly don't): treat it as a hint and fall back to the measured damage
  pattern (large area changing at a steady rate = video).

### 2.6 Cursor sent separately

**Already done.** The frame is fed without the cursor on paths A and B (`mirror_cursor`, `mirror.c:823-842`), the
position goes over the WebSocket and the image as a PNG, and the page shows it as a CSS cursor (`client.html:587-616`),
so the local pointer moves with no network latency. One gap to check: on the GPU path without a cursor plane
(`!s->hw_cursor`), `gpu_layer` draws the cursor into the frame (`main.c:2009-2018`) *before* the read-back
(`gpu.c:1406-1410`), so the remote frame would contain the cursor too (a second, lagging arrow, and every pointer move
becomes video damage). With a cursor plane (the usual case on the RX 580) it isn't drawn. *To verify* with
`LORD_HW_CURSOR`-off or a driver without a cursor plane; the fix is to read back before `draw_layer(1)`'s cursor, or
draw the cursor after the read-back.

### 2.7 Per-monitor streams

5673 shows one picture: the main display, or all displays at 100 % with "capture all" (`feed_out`, `main.c:634-644`).
The screen-sharing API already lets its client pick a display (`SHARE_DISPLAY`, SCREEN-SHARING.md "Displays"; the
same `feed_out`). Two steps:

1. **A display picker for 5673** (S–M): the page asks for a display (a new browser message checked by
   `rs_browser_input`, a new `RP_` message to the session, the same code as `SHARE_DISPLAY`: repaint that display, a
   keyframe, pointer mapped through its view). Big win on the maintainer's three 1080p monitors: "capture all" today
   is a 5760×1080 CPU-composed frame.
2. **Several displays at once** (L): one encoder per display (today `mstream` has one per codec, `MS_CODECS`), several
   video tracks or simulcast-like m-lines, the page laying them out. Costs N encoders; only worth it after step 1.

### 2.8 Protected windows and password fields: the cost of masking

Since 0.8.9.6 every password field (`TextInput secure`) is a protected node. With a viewer connected, while *anything*
protected is on screen:

- `remote_needs_cpu` (`main.c:939-943`) makes `gpu_allowed` false: **the whole session, the local monitors included,
  switches to CPU composition** (`cpu_takeover`, `gpu_shutdown`), and back when it disappears. Opening the Wi-Fi
  password dialog, Settings → Sharing's TURN field, the keyring prompt or the lock screen does it.
- `media_composed` keeps the capture canvas, repainted in the changed region (`capture.c:91-96`), plus a full copy when
  a fullscreen app is shown (`capture.c:143-155`).
- `mirror_frame` gets no damage (`main.c:2170`), so every tile is compared on every frame.
- `media_protected_count` walks the whole node tree (`capture.c:31-41`), called at least twice per frame.

The privacy rule must not weaken: no protected pixel may reach any capture (`test_protected_fuzz`). Options, cheapest
first:

1. **Pass the capture canvas's repainted region as the damage** (it is `scene->clip`, `capture.c:95`) instead of
   NULL: the compare goes back to damage-driven. Small, no privacy impact.
2. **Cache the protected count** (invalidate on node changes) instead of walking the tree per call. Small.
3. **Keep the GPU composing the monitors** and produce only the remote frame on the CPU while something is protected:
   today the CPU capture canvas lacks the Linux apps' GPU-only content, which is why the whole session switches. A
   middle way is the GPU read-back with the **protected boxes overwritten by the CPU fill**, but a translucent or blurred
   surface above a protected one would carry blended protected pixels: it is only safe if the patched region is the
   protected box plus everything above it that samples it (blur radius, translucent windows), and that needs the same
   fuzz test run on the GPU path. Medium–large, needs a security review; decision D3.

Until measured, the cost is a guess: a CPU/GPU switch is a visible hitch on the monitor (GPU context torn down and
rebuilt), and three 1080p monitors on the CPU path cost far more than on the GPU. Section 7 measures it.

### 2.9 The capture indicator

The 0.8.9.5 indicator names every program and its frames wait for the indicator to be on screen (SCREEN-SHARING.md
"The indicator"). 5673 has its own notice («Remote desktop: lord · IP»). Nothing here changes either; a shared pipeline
(section 5) must keep each door's gate (indicator for the API, notice for 5673) *before* frames are handed to that
door, not at the encoder.

## 3. Encoder tuning

### 3.1 VA-API (RX 580, VCE 3.4)

- **Render node (F3).** Open the compositor's render node (the one EGL uses, `gpu.c:626` "the render node for
  linux-dmabuf feedback") or probe each node for an H.264 *encode* entrypoint (`lm_hwprobe.c` already lists decode
  entrypoints) instead of the first one that opens (`media.c:274-283`). Log which node and driver encoded.
- **`async_depth=1`.** With 2, `avcodec_receive_packet` gives frame N's packet only after frame N+1 is sent. Because
  frames are only sent on change, the last frame of every burst waits for the next change or the 70 ms flush frame
  (`mstream.c:218`, `241`). The flush frame may be partly a workaround for this ("browsers paint a lone frame late").
  Expected: one frame less latency always, up to ~70 ms less for the frame that ends a change. Also reconsider the
  `VID_CHECK_FRAMES` budget (`lm_stream.c:44`).
- **Rate.** FFmpeg's VA-API encoder reads its rate at open. Options: reopen the encoder (in `lord-media`, keeping the
  RTP timestamps continuous as `track_ts` already handles) when the target moves by more than ~25 %, at the cost of an
  IDR; or open it in CQP/ICQ and pick QP from the target (no per-frame cap, risky on a slow link); or drive libva
  directly (large). Reopen is the simple one. Start at a better first rate too (the bridge's 4000 is a guess; a LAN
  reaches 12000 in ~10 s).
- **CBR vs VBR.** For low latency the frame size must be bounded: VBR with a VBV of one or two frames
  (`rc_buffer_size ≈ 2 × bitrate / fps`) instead of half a second, plus `max_frame_size` where the driver supports it
  (radeonsi on VCE 3.4: unknown, measure). CBR pads still frames with filler on some drivers; avoid it here because
  frames are only sent on change.
- **GOP.** A 60 s GOP with keyframes on request is right for WebRTC with NACK/PLI; keep it. Intra refresh isn't in
  FFmpeg's `h264_vaapi` and browsers need an IDR to start anyway.
- **Slices.** Several slices per frame let the browser decode the rest of a frame after a loss and let the encoder
  pipeline; FFmpeg `slices`. Small gain on LAN, measure.
- **Profile.** Constrained Baseline excludes CABAC (~10–15 % fewer bits at the same quality). Chrome, Edge, Safari and
  Firefox all offer Constrained High (`640c1f`) or High; answer it when the offer has it and keep Baseline as the
  fallback (also benefits x264 on the PC if a slower preset is used). The answer's `profile-level-id` echoes the
  browser's (`rtcwire.c:564`, `42e01f` = level 3.1), while 1080p60 needs 4.2: browsers tolerate it, but answer a level
  that matches the size and rate.
- **Colour.** Set the same VUI as x264 (`lm_stream.c:126-129`) so the VA-API stream is tagged BT.709/sRGB like the
  tiles; today it is untagged (F5).
- **SEI.** `sei=0` (drop timing/recovery/A53): a few bytes per frame.

### 3.2 x264 (Raspberry Pi 5, and any machine without an encoder)

- `ultrafast` + `zerolatency` is the right family. `zerolatency` already gives sliced threads and no lookahead.
- **Threads.** 2 today (`lm_stream.c:132`). The Pi 5 has 4 cores; lord-server needs one for composition, so try 3 and
  measure the frame time and the session's own frame rate together.
- **`mb_info`** (2.1): the largest Pi gain for desktop work.
- **Dirty-rect colour conversion** (2.1): sws on the Pi at 1080p is several ms per frame of pure overhead.
- **Chroma.** `SWS_POINT` (`lm_stream.c:240`) point-samples chroma: coloured text and 1-px coloured lines fringe.
  `SWS_AREA`/`SWS_BILINEAR` (+ `SWS_ACCURATE_RND`) for the chroma filter costs a little and should read better; check
  with SSIM against the lossless tiles on a text scene.
- **CRF by content** (2.5): e.g. 20 for text-heavy, 24–26 for video-dominated; `crf` changes between frames.
- **`slice-max-size=1100`** (one slice per RTP packet, single NAL mode): better loss behaviour on Wi-Fi, a few % bits.
- `superfast` instead of `ultrafast` turns CABAC and better motion search on (~20–30 % fewer bits) for ~1.5–2× the CPU:
  a PC-without-VA-API option, not the Pi's.
- VP8: fine as the fallback; nothing to change beyond the same dirty-rect conversion.

### 3.3 4:4:4 or a lossless text mode

H.264 High 4:4:4 Predictive is not reliably decodable in browsers' WebRTC stacks (Firefox and Safari no; Chrome's
support isn't a given), so a 4:4:4 *video* path would split browsers. LordOS already has the better answer: the
lossless PNG refresh over the still areas. Improve it rather than add 4:4:4:

- send RGB not RGBA over `remote.sock` (25 % fewer bytes) or let lord-server hand only the damaged tiles' PNG once to
  all bridges (today each bridge encodes the same PNG per viewer);
- don't send tiles that sit under a video-type surface (2.5);
- prefer the tiles in reading order around the pointer first (what the person looks at settles first);
- a faster deflate (level 1) for large tiles: PNG decode in the browser is cheap, the bridge's CPU isn't.

A "quality refresh" frame (one P-frame at very low QP after 250 ms still) is an alternative that needs no second
layer, but it is never exact and costs a bit-rate spike; keep the tiles.

### 3.4 Adaptive bit rate from RTT and loss

Today: loss from receiver reports, REMB as a ceiling when it falls. Missing:

- **RTT.** Receiver reports carry LSR/DLSR; the bridge sends sender reports (`rtc.c:723-735`), so RTT is computable
  today with no new negotiation. A growing RTT on a LAN or Wi-Fi is queueing: back off before loss appears.
- **Pacing** (section 6, #5): today a keyframe of 100–300 KB leaves as 100–270 packets back to back (`rtc.c:707-715`),
  which is what makes Wi-Fi drop the tail and the browser ask for another keyframe. A token bucket at ~2.5× the
  target rate in the bridge's loop, with audio and retransmissions first.
- **Keyframe size cap** (VA-API `max_frame_size`, x264 VBV) and a lower QP floor right after a keyframe.
- **Transport-wide congestion control** (`transport-cc` feedback, a delay-gradient estimator like Chrome's GCC) is the
  complete answer and what Pion does for go-link; it is a parser (rtcwire.c, with fuzz cases) plus an estimator: L.
- **Apply the rate at all** for VA-API (3.1).

## 4. The client side

The only client of 5673 is the browser page (`server/src/remote/client.html`, embedded in the binary). LordOS's
Remote Desk app is an RDP/VNC client and doesn't speak 5673; another LordOS machine views 5673 through Chromium or
Firefox like any computer.

- **Decode path.** `RTCPeerConnection` → `<video autoplay playsinline muted>` (`client.html:49`, `359-371`); H.264
  decode is the browser's: hardware in Chrome/Edge on Windows and macOS and in Safari; software (FFmpeg/OpenH264) in
  Firefox and in Chrome on Linux for WebRTC. A 2.6 ms decode was *measured* (Chrome on a Mac).
- **Latency knobs used:** `jitterBufferTarget = 0`, `playoutDelayHint = 0` (`client.html:367-368`). These set a
  minimum; Chrome still adds what its jitter estimator wants (15–80 ms *measured* while dragging).
- **Missing:** the RTP header extension `http://www.webrtc.org/experiments/rtp-hdrext/playout-delay` with min = max = 0,
  which Chrome offers and honours (its low-latency renderer path, used by remote desktop and cloud-gaming services).
  The bridge would echo the `a=extmap` from the offer and add the 4-byte extension to each video packet. A parser
  change in `rtcwire.c` (extmap in the offer) means a new case in `remote-sec-test` and `remote-fuzz` (CLAUDE.md).
  Firefox and Safari ignore it; they fall back to what they do today.
- **Lossless layer:** tiles decoded with `createImageBitmap` and drawn into a canvas over the video, cleared by
  `RP_DAMAGE` (`client.html:467-480`). Correct; when the page scales the picture (window smaller than the remote
  screen) both layers are resampled, so text is only exact at 1:1. A "1:1 / fit" toggle would make that visible.
- **Cursor:** a CSS cursor from the session's image, no lag (`client.html:587-616`).
- **Measuring from the page:** `requestVideoFrameCallback` gives `receiveTime`, `processingDuration`, `rtpTimestamp`
  per frame; with the `abs-capture-time` extension (or the session's compose time in the RTP timestamp, 2.4) the page
  can show compose → shown per frame in its top bar, which today shows fps and encode ms only.

## 5. One capture+encode pipeline with the native go-link app

**Much of it is already shared.** The screen-sharing API (the door a native go-link app uses) and 5673 meet at
`mirror_frame` → `mstream` → `lord-media` → `video_out`, which feeds both 5673's bridges and `share_video`
(`mirror.c:449-462`, `video_update` counts the share's holder as one more viewer, `mirror.c:159-165`). So every
improvement in sections 2.1–2.5 and 3.1–3.3 that sits in `main.c`, `gpu.c`, `capture.c`, `mirror.c`, `mstream.c` or
`lm_stream.c` speeds up both doors at once. What differs per door, and should stay per door:

| Concern | 5673 | Screen-sharing API (go-link) |
|---|---|---|
| Who may watch, gates | sign-in, lockout, the monitor notice | permission, visitor's acceptance, indicator on screen first |
| Transport, packetization, SRTP | our bridge (`rtc.c`) | the app's engine (Pion) |
| Congestion control | our bridge's estimator → `RP_RATE` | Pion's GCC → `RP_RATE` |
| Keyframe policy | PLI/FIR → `RP_KEYFRAME` (300 ms floor) | token bucket in `share.c` (SCREEN-SHARING.md "Keyframes") |
| Display choice | main or capture-all today (2.7) | `SHARE_DISPLAY` |
| Lossless refresh | PNG tiles (5673 only) | none |

Proposals for the shared core, to agree with the NATIVE-REMOTE-APP draft:

1. **One frame description**: frame + damage rects + content hint (video/text) + presentation time, produced once per
   composed frame by `main.c`, consumed by `mirror_frame`. Both doors get damage-driven encode and correct timestamps.
2. **One encoder per (display, codec, profile)**, not per door: two doors on one display share it (one door at a time
   today, so this mostly matters once per-monitor streams exist).
3. **The rate is the minimum of what every consumer asked**, as now; the encoder must actually apply it (3.1), or
   go-link's GCC is as ineffective with VA-API as our bridge is today.
4. **Profile negotiation**: GO-LINK.md says "a viewer that asks for High gets Baseline". If 5673 moves to Constrained
   High (3.1), the share API should carry the profile too (`SHARE_START` has a codec byte; a profile needs a new
   field, versioned like `SHARE_DISPLAY`).
5. **Dirty-tile PNG refresh** could be offered to API clients later as an optional channel; not needed now.
6. The zero-copy path (2.3) lives entirely below `mstream`: both doors get it for free.

**Reconciling with NATIVE-REMOTE-APP.md.** That draft proposes a shared `server/src/rtc/` library (the 5673 peer grown
into a full ICE agent, TWCC, a GCC-style estimator, a 2.5× pacer, `playout-delay`), scaling in `mstream`/`lord-media`,
and DMA-BUF into VA-API as its "phase 5". This document agrees and adds the order of work: items 2, 5 and 15 here *are*
that draft's 3.3/3.5 and should be built once, in the shared library, with 5673 as the first user (it needs no
signalhub work and gives the measurements). Three corrections to carry into that draft:

- Its 3.2 says `mirror_frame` gets the damage box and the compare covers only damaged tiles: true on the CPU paths
  only; on the GPU path (the RX 580, where its 60 fps / 50 ms target lives) no damage is passed (F4) and the read-back
  is synchronous. Item 3 here is a prerequisite for its targets.
- Its VA-API numbers assume the RX 580 encodes; on the maintainer's PC the encoder likely opens the Intel iGPU's node
  first and falls back to x264 (F3). Item 1 here is a prerequisite.
- Its 3.3 point 4 (VA-API rate by reopen or libva parameter buffers) is F1 here; whichever is chosen (D5) serves both.

## 6. Prioritized list

Effort: S ≤ 2 days, M ≤ 1–2 weeks, L more. Gains are *estimates* until section 7's runs.

| # | Change | Where | Expected gain | Effort | Risk | Security impact |
|---|---|---|---|---|---|---|
| 1 | **VA-API fixes**: encoder on the compositor's render node (or probed encode entrypoint); `async_depth=1`; follow the rate (reopen on >25 % change); colour VUI; `sei=0`; first rate from the bridge's current estimate | `media.c:274`, `lm_stream.c:79-110`, `286-294`, `conn.c:906-911` | PC: x264 → VCE if F3 holds (≈1 core → near 0; 10–13 → a few ms/frame); −1 frame latency, up to −70 ms at the end of a change; up to 3× the bit rate on LAN (sharper motion) | S | low (first-keyframe check stays) | none |
| 2 | **Playout-delay extension** (min 0, max 0) | `rtcwire.c` (extmap parse + answer), `rtc.c` (header ext) | −15 to −60 ms in Chrome/Edge | S–M | low; Firefox/Safari unchanged | parser change: new remote-sec-test + fuzz cases; no exception |
| 3 | **GPU path damage + async read-back**: feed the ScreenCast damage accumulator to `mirror_frame`; read only the damaged region; BGRA read; PBO + native fence fd polled by the loop; swizzle/flip off the main thread | `main.c:2355-2443`, `gpu.c:927-943`, `wl.c:474` | PC/Pi with monitor: lord-server from ~a core while watched to a fraction; no per-frame stall (a few ms on the RX 580, more on V3D) | M | medium (damage must be complete; `FULL_CHECK_MS` stays) | none (GLES 3 from the same Mesa) |
| 4 | **Damage into the encoder**: tile bitmap instead of one box; dirty rects in `MM_INFRAME`; persistent YUV with dirty-rect conversion; x264 `mb_info` via `AVVideoHint`; better chroma filter | `mirror.c:724-735`, `mstream.c:201-243`, `lm_stream.c:237-262` | Pi 5: desktop work from ~13–17 ms to a few ms/frame, higher fps; sharper coloured text | M | low–medium (validate rects in lord-media) | none (`lord-media` validates a new field) |
| 5 | **Bridge pacing + RTT-aware rate**: token-bucket pacer; RTT from RR LSR/DLSR; keyframe size cap | `rtc.c:667-792` | fewer burst losses and PLI loops on Wi-Fi; steadier quality | M | low | none |
| 6 | **Don't restart the encoder on a frame-rate change**; change only the feed pacing | `mstream.c:172-191` | no IDR + stall when a second viewer or a re-measured Hz arrives | S | low | none |
| 7 | **Protected-on-screen costs**: capture damage instead of NULL; cache the protected count; measure the CPU/GPU switch | `main.c:2170`, `capture.c:31-41` | full compare → damage-driven while a password field shows; fewer tree walks | S | none | none |
| 8 | **Fullscreen damage** (#232's subsurface box) to `mirror_frame` and to `present_memory`'s copy | `main.c:2168-2170`, `2213-2238`, `wl.c:549` | fullscreen video headless (Pi): one full copy + full compare less per frame | S–M | medium (verify with `LORD_PAINT_VERIFY`) | none |
| 9 | **Presentation-based pacing and timestamps**; follow a video surface's cadence | `mstream.c:208-243`, `main.c` flip handler | smoother video, steadier jitter buffer | M | low | none |
| 10 | **Content-type policy**: no tiles over video, CRF/QP by content | `mirror.c`, `lm_stream.c`, `timing.c` | fewer wasted PNG bytes; sharper text at equal bits | M | low | none |
| 11 | **Display picker for 5673** | `client.html`, `rsec.c`, `mirror.c`, `main.c:634` | on multi-monitor PCs: 1920 instead of 5760 px wide; GPU path instead of CPU "capture all" | M | low | browser message parser: new tests |
| 12 | **Constrained High profile** when offered; correct level | `rtcwire.c:564`, `lm_stream.c:100` | ~10–15 % fewer bits (VA-API, x264 non-ultrafast) | S | low (keep Baseline fallback) | parser tests |
| 13 | **GPU capture mode for protected windows** (keep monitors on the GPU) | `main.c:939`, `capture.c`, `gpu.c` | no session-wide CPU switch on every password field | L | high (privacy rule) | security review (D3) |
| 14 | **Zero-copy dmabuf → VA-API** | `gpu.c`, `mstream.c`, `lm_stream.c` | PC: no read-back, copies or sws at all; lowest latency and CPU | L | medium–high | new interface into a sandboxed helper (D1) |
| 15 | Transport-wide CC (GCC) | `rtcwire.c`, `rtc.c` | best rate control on bad networks | L | medium | parser + fuzz |
| 16 | Lossless refresh: RGB over the socket, PNG once for all bridges, level 1 | `mirror.c:233-259`, `conn.c:627-646`, `enc.c` | less bridge CPU and socket traffic when the screen settles | S–M | low | none |

## 7. Measurement plan

### 7.1 Tools that exist

- **The page's top bar** (`RP_VINFO`, `mirror.c:771-796`): codec, hardware or software, path, fps shown/encoded, the
  viewer's Hz, GPU/CPU composition, encode ms per frame on hover.
- **Journal lines**: `remote: video encoder for H.264 started (WxH, fps, kbit/s)` (`mstream.c:154`); in `lord-media`
  `remote desktop video: WxH with h264_vaapi|libx264` and the end-of-stream `N frames, X ms each` (`lm_stream.c:194`,
  `411`); in the bridge `video: peer closed (…; kbps, % lost, NACK, PLI)` (`rtc.c:1669`, `conn.c`).
- **LordBench** (BENCHMARK.md): the video job `MK_VBENCH` (`lm_vbench.c`) measures the remote desktop's own software
  encoders (`lm_stream_sw_encoder`) at 1080p; *Desktop impact* records every frame built and shown
  (`bench/lbframes.c`), and needs a remote viewer to run on a headless session; frame phases with GPU fence times
  (`lbf_recording`, `gpu_fence_time`).
- **`build/lmstream-test`** (`server/tests/lmstream_test.c`, `make -C server test-full`): drives `lord-media`'s
  `MK_STREAM` through the sealed ring with a render node; extend it to time encode per frame and to check `async_depth`
  (packet for frame N returned before frame N+1 is sent).
- **`tools/vm/e2e/ui.py`**: drives a session over `remote.sock` like the bridge (frames, input) — a viewer without a
  browser, for repeatable CPU runs.
- **`tests/protected_headless.sh`**, `engine-test` `test_protected_fuzz`: privacy regressions for items 7, 13, 14.
- **The 0.8.4 method** (REMOTE.md "Measurements"): CPU per process from `/proc`, traffic on the interface, a dragged
  window for 16 s, click-to-pixel.

### 7.2 Scenarios (same for every run)

1. still desktop, 60 s; 2. typing in foot (a script writing 10 lines/s); 3. a window dragged in circles
   (`lord.wm.script`, LordBench's figure of eight); 4. scrolling a long text page in Firefox; 5. a 1080p60 video
   fullscreen in Firefox (#232's case); 6. the same with a password field open (Wi-Fi dialog) for item 7/13.

### 7.3 Metrics

- click-to-pixel latency: a page script that clicks and timestamps until `requestVideoFrameCallback` reports the
  changed frame (colour probe), median and p95 over 100 clicks; for an absolute check, a 240 fps phone camera filming
  both screens once per machine;
- per stage: compose, read-back, compare, slot copy, sws, encode, send (temporary `CLOCK_MONOTONIC` stamps in a
  development build, reported through `RP_VINFO` or the journal);
- CPU % per process (lord-server, lord-media, the bridge), GPU busy (`amdgpu` `gpu_busy_percent`), bit rate, fps
  encoded and shown, NACK/PLI counts;
- quality: SSIM/PSNR of decoded video frames against the session's own frame (`screendump`) on scenario 2 and 4; text
  legibility before the lossless tiles land.

### 7.4 Machines

- **RX 580 PC** (i9-10900, AMD RX 580 + Intel UHD 630, three 1080p displays). First establish which render node and
  encoder 5673 uses today (F3: journal + top bar). Then runs: (a) today's build; (b) item 1 alone; (c) 1+3; (d) 1+3+4;
  each with "capture all" off and on. Viewer: Chrome on another machine on the same LAN, 60 Hz and 144 Hz screens,
  then the same over Wi-Fi for item 5. Per the memory notes, this is the maintainer's daily machine: use a second user
  session or VM 127's procedure if the main session must not be disturbed.
- **Raspberry Pi 5** (x264): headless (path A) and with its HDMI monitor (path C, GPU read-back). Runs: today; item 4;
  item 3 (monitor only); `threads=3`. Watch the session's own frame rate (it competes with x264 for the 4 cores) and
  temperature as LordBench does.
- **VM 116/122** stay the CPU reference (no GPU, no VA-API) for regressions.

Record results as CSVs next to `docs/perf/0.8.9/` and only then write numbers into REMOTE.md and the paper.

## 8. Decisions for the maintainer

- **D1 — zero-copy (item 14):** may `lord-server` hand `lord-media` dmabufs of the composed screen (capture BOs it
  allocates) and sync_file fds per frame? `lord-media` already gets the render node and sees the same pixels through
  the memfd today; this is a new kind of input to a sandboxed helper and wants an AGENTS.md entry.
- **D2 — render node choice (item 1):** use the compositor's GPU for encoding, or probe every node for an H.264 encode
  entrypoint? On a hybrid laptop the answer can differ. Related: installing Intel's VA driver
  (`intel-media-va-driver` / `-non-free`) on Intel machines is a packaging decision, not done here.
- **D3 — protected windows on the GPU path (item 13):** accept a GPU-side capture mode, with the same fuzz test on
  the GPU path as the gate? Until then a password field on screen keeps moving the whole session to the CPU while a
  viewer is connected.
- **D4 — profile:** move 5673 to Constrained High when the browser offers it (Baseline as fallback), and carry the
  profile in the screen-sharing API?
- **D5 — rate on VA-API:** reopening the encoder (an IDR per large rate change) is the cheap way; accept it, or wait for
  a libva-direct encoder?
- **D6 — per-monitor:** a display picker for 5673 first (item 11), simultaneous streams later?

None of items 1–12 or 16 adds a dependency or a privilege; items 2, 11, 12 and 15 touch parsers and need their
`remote-sec-test`/`remote-fuzz` cases.

## Appendix: findings in short

| Id | Finding | Where |
|---|---|---|
| F1 | VA-API ignores rate changes; stays at the 4000 kbit/s of `RP_VIDEO` | `lm_stream.c:286-294`, `conn.c:906-911` |
| F2 | VA-API `async_depth` default 2: one frame held; the end of a change waits for the 70 ms flush | `lm_stream.c:79-110`, `mstream.c:218` |
| F3 | Encoder opens `renderD128` first; on the PC that is the Intel iGPU, which has no VA driver installed → libx264 (*verify*) | `media.c:274-283`; `/dev/dri/by-path` |
| F4 | GPU path: synchronous full read-back before swap, single-threaded swizzle, no damage to `mirror_frame` | `gpu.c:927-943`, `1407-1410`; `main.c:2441` |
| F5 | VA-API stream has no colour VUI | `lm_stream.c:79-110` vs `126-129` |
| F6 | `SWS_POINT` chroma subsampling | `lm_stream.c:240` |
| F7 | A frame-rate change restarts the encoder | `mstream.c:176` |
| F8 | Software cursor drawn before the read-back when there is no cursor plane (*verify*) | `main.c:2009-2018`, `gpu.c:1406-1410` |
| F9 | RTP sent with no pacing | `rtc.c:667-715` |
| F10 | Anything protected → whole session to the CPU while watched; capture frame compared whole | `main.c:939-943`, `2170` |
| F11 | Fullscreen → no damage on the CPU paths; full scene copy headless | `main.c:2168-2170`, `2213-2238` |
| F12 | One bounding box for the encoder's slot copies | `mirror.c:724-735` |
