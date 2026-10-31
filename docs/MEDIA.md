# Media: Photos, Player, Screenshots, Camera and Recorder (M9)

Five preinstalled native LordOS apps, plus a sandboxed helper, `lord-media`. It is the only LordOS
program that links FFmpeg, and only to decode and encode. The player, audio/video sync, the
interface and compositing are our own.

| App | id | What it does |
|---|---|---|
| **Photos** | `photos` | Photo library of the pictures folder (XDG, `~/Pictures` in English) with thumbnails and a viewer: zoom, panning, rotation, full screen, slideshow, animated GIF and basic editing (crop, rotate, brightness and contrast) saved as a copy |
| **Player** | `player` | Video with synced audio, time bar, volume, `.srt` and in-track subtitles, full screen, picture in picture; music library by artist and album with covers, queue, shuffle and repeat; media keys and the Control Center |
| **Screenshots** | `screenshots` | Screen, window or area, timer, to the clipboard or to `~/Pictures/Screenshots`, floating thumbnail and annotation editor (arrow, box, text, blur); record the screen to MP4 with optional microphone |
| **Camera** | `camera` | V4L2 preview, JPEG photo, H.264 + AAC video, mirror, choose camera |
| **Recorder** | `recorder` | Record the microphone, waveform, list, rename, trim and export to Opus, MP3 or WAV |

The names are generic and the icons are our own, drawn with nodes like those of the other apps.
We took the way the apps of popular desktop systems work, never their logos or their design.

## Screenshots (VM 119, real lord-server, Proxmox `screendump`)

| | |
|---|---|
| ![photo library](photos-library.png) | ![avif](photos-avif.png) |
| Photo library: thumbnails made by lord-media (PNG, JPEG, WebP, BMP, HEIC, AVIF and GIF) | Viewer with an AVIF |
| ![zoom](photos-zoom.png) | ![edit](photos-edit.png) |
| Rotated 90° and at 150 % | Editing: crop, brightness and contrast live; "Save as copy" |
| ![albums](music-albums.png) | ![playing](music-now-playing.png) |
| Music library by album, with covers from the tags | Playing: queue, shuffle and repeat |
| ![control center](media-control-center.png) | ![video](video-subtitles.png) |
| The Control Center with what's playing | A 1080p video with `.srt` subtitles |
| ![mkv](video-subtitles-mkv.png) | ![full screen](video-fullscreen.png) |
| Subtitles from an MKV's track | Full screen |
| ![picture in picture](video-picture-in-picture.png) | ![open with](media-open-with.png) |
| Picture in picture over Files | Files: video thumbnails and "Open with → Player" |
| ![panel](screenshots-panel.png) | ![permission](screenshots-permission.png) |
| The Screenshots panel (❖⇧5) | The system asks for the screen permission the first time |
| ![area](screenshots-area.png) | ![annotate](screenshots-annotate.png) |
| Capturing an area over the frozen screen | Editor: arrow, box, blur and text, at full resolution |
| ![recording](screen-recording-in-progress.png) | ![frame](screen-recording-frame.png) |
| Recording the screen (orange microphone indicator) | A frame of the recorded MP4: protected Notes comes out black |
| ![camera](camera.png) | ![recorder](recorder-recording.png) |
| Camera (v4l2loopback test pattern; see "Tests"), with mirror and the green dot | Recorder: live level |
| ![waveform](recorder.png) | ![trim](recorder-trim.png) |
| The recording with its waveform | Trim and export |
| ![rtl](media-rtl.png) | |
| The Player in Arabic (right to left) | |

### Protected windows

| On screen | In the screenshot | On remote desktop |
|---|---|---|
| ![screen](protected-window-screen.png) | ![screenshot](protected-window-capture.png) | ![remote](protected-window-remote.png) |
| Protected Notes looks normal on the local screen | The saved PNG: the Notes content is black | What the browser receives from the remote desktop (0.8.3; since 0.8.4 the same, as WebRTC video) |

## Architecture

```
 apps (React)  Photos · Player · Screenshots · Camera · Recorder
      │         <MediaView media={id}/> · sdk/media.js (media, usePlayer, nowPlaying)
      ▼
 lord-server   ui/js_media.c (lord.media, validation and permissions)
      │        media/media.c   helpers, jobs, thumbnails, paths
      │        media/mview.c   draws the media node (direct copy, zoom, rotation, mipmaps)
      │        media/capture.c screenshots, recording, remote desktop, protected windows
      │        media/maudio.c  PipeWire streams for media (playback and recording)
      │        media/annot.c   annotations
      │   SEQPACKET socket + fds (media/proto.h)   shared memory (frames, audio)
      ▼
 lord-media    media/helper/*  (FFmpeg + libva; sandbox; no network or files)
```

### The `lord-media` helper

- **How it's launched:** lord-server launches it on demand (`media_spawn`), with a fixed path:
  `<lord-server folder>/lord-media`, which must be owned by root or the user and not writable by
  others. It takes no arguments, the environment is minimal and the socket goes on fd 3. There is
  one per player, camera or recording, and two *workers* for short jobs (thumbnails and probes in
  the background; images, edits and exports in the foreground). Workers exit on their own after
  20 s without work. A one-shot `MK_HWPROBE` helper lists, once per boot, which codecs VA-API
  decodes on the render nodes it gets (`lm_hwprobe.c`, for "Video in the browser": see
  [VIDEO.md](VIDEO.md)). A one-shot `MK_VBENCH` helper (0.8.6, LordBench) gets no fds at all: it makes
  its test clips in memory and answers only fixed `MM_BENCH` records (`lm_vbench.c`, lord-server's side in
  `bench/lbvideo.c`; see [BENCHMARK.md](BENCHMARK.md)).
- **It never receives a path.** lord-server validates each path with the same rules as `lord.fs`
  (home folder and the user's mounts), opens the file and passes it the fd.
  - Outputs are created as temporary files next to the destination. When they finish well they're
    renamed with `RENAME_NOREPLACE`, so a file is never overwritten.
  - The camera (`/dev/videoN`) and the render node (`/dev/dri/renderD*`, for VA-API) are opened by
    lord-server too.
  - Audio doesn't arrive as a socket (see "Audio").
- **Sandbox:** the one the helpers share, `sandbox_apply_helper` from `sec/sandbox.c` (the same as
  lord-doc). It's applied before reading a byte of the file and before initializing VA-API:
  - Landlock: read-only `/usr/lib`, `/usr/share`, `/etc/ld.so.cache` and `/sys/devices`; nothing
    from the home folder, `/tmp`, `/dev` or `/run`;
  - seccomp with TSYNC: no `socket`/`connect`/`bind`, no `exec`, `clone` only for threads, and
    `ioctl` only from the DRM/dma-buf (VA-API) or V4L2 (camera) classes when needed;
  - `no_new_privs`, no capabilities, `PR_SET_DUMPABLE=0`, no core dumps and the
    `app-lordos-media.scope` cgroup with a memory cap.

  `build/media-test sandbox` tests it from outside: opening `/etc/passwd` or `/home`, creating in
  `/tmp`, an AF_INET or AF_UNIX `socket`, `fork` and `exec`: all fail.
- **If it crashes, the desktop carries on.**
  - lord-server never waits for the helper: non-blocking socket, small messages.
  - The shared memory the helper sends must come sealed against shrinking (`F_SEAL_SHRINK`, it is
    checked): a compromised helper can't bring lord-server down with `SIGBUS`.
  - When the socket closes, the window gets `closed` (`crashed: true`) and shows a notice. The
    process is waited for or killed.
  - Tested with a real bug (a queue overflow at the end of the file, now fixed): the segfault ended
    up in the kernel log, the Player showed the notice and the rest of the desktop carried on.
- **Hardware acceleration:** `hwaccel=vaapi|v4l2request|none`, selectable with
  `LORD_MEDIA_HWACCEL`; `LORD_MEDIA_HW=0` turns it off.
  - With VA-API, the driver (radeonsi or iHD) decodes and the NV12 frame is downloaded to scale it.
  - `v4l2request` (Raspberry Pi 5 HEVC) is prepared: it needs the Raspberry Pi FFmpeg and creating
    the device over fds (TODO in `lm_io.c`).
  - If there is no driver, the decoder carries on in software without interruption.
  - (0.8.7, in development) If the hardware decoder fails during a file, 8 failures in a row (`lm_hwfall.h`; a frame
    clears them, so one damaged packet changes nothing) or frames that can't be read back from the hardware make the
    player open a software decoder and seek to where it was. Tested with a decoder made to fail after 30 frames
    (`build/lord-media-hwfall`, `build/media-test play`, 640 × 360 H.264 on VM 116): 119 frames received in 4 s, the largest gap
    41 ms, against 38 ms without the failure. That build fakes the failure on a software decoder: **it has not run on real VA-API hardware.**

### Video frames: a single copy

1. lord-media decodes and scales each frame (swscale, with the video's BT.709/601 color matrix)
   **to the exact size it's shown at**. lord-server tells it the node's box with `MM_SIZE`.
2. It writes it into one of three slots of a sealed memfd and announces `MM_FRAME`. lord-server
   replies `MM_ACK` when it starts showing it, and from then on the previous ones are free. If no
   slot is free, the frame is dropped instead of waiting.
3. The `media` node copies the slot straight onto the canvas (`canvas_blit`): that's the
   compositing copy, there is no other. With zoom, rotation or mirror (Photos, Camera) an
   intermediate image of the visible size is built, bilinear and with mipmaps for large photos,
   which is reused as long as nothing changes.
4. **Painting from the video:** if everything that changed in the frame is inside an opaque video
   (`media_cover_node`), `paint.c` doesn't paint what lies below it in paint order (background,
   windows behind, the Dock's blur); it walks those nodes only for their clips. The layers on top
   (controls, subtitles) are painted as usual. It brought lord-server's cost with a 1080p video
   down from 42 % to 11–13 % of a core in a window, and from 92 % to 14 % in full screen (see
   "Measurements"). It isn't used on the GPU path, which builds its window list there.

On the GPU path (`GPU.md`) the node is painted into the UI texture, and only the region that
changed is uploaded. **Pending:** true zero-copy, exporting the VA-API surface as a dmabuf (NV12)
and importing it through EGL as an external texture. It wasn't written because there's nowhere to
test it: neither the VM nor the host has a GPU with VA-API.

### Audio: PipeWire belongs to lord-server, not the helper

The PipeWire socket (PulseAudio protocol) allows loading modules, recording other streams and
changing the output, so a compromised decoder must not have it. Audio goes through shared memory
(`media/aring.h`):

- **Playback:**
  - lord-media writes PCM (s16, 48 kHz, stereo) into a 1.4 s ring that lord-server creates (sealed
    memfd).
  - lord-server owns the PipeWire stream (`maudio.c`, with the asynchronous client `mpulse.c`) and
    copies from the ring when PipeWire asks (`REQUEST`).
  - The stream shows up as "Player" in Settings → Sound, and the volume is set there.
- **The clock:**
  - After each `GET_PLAYBACK_LATENCY`, every 40 ms while playing, lord-server writes into the
    ring's page which byte is being heard (`read_index − output latency`) and at which
    `CLOCK_MONOTONIC` instant, with a seqlock.
  - lord-media reads it without locks or round trips and presents each frame when
    `pts ≤ clock + 4 ms`; if it's late, it drops it.
  - Without audio, the clock is the system clock. If lord-server stops reading the ring for more
    than 2 s (PipeWire went down), the video carries on with the system clock.
- **Recording** (microphone, after the permission): the other way around. lord-server owns the
  recording stream and writes into the ring; lord-media reads it for the Recorder's WAV or the AAC
  of screen and camera recordings.
- **Seek:** lord-media bumps `serial` and `discard_to`; lord-server drops what came before and
  does a `FLUSH` of the stream.
- **Distrust:** lord-server never trusts the shared header (the other side can write it). It uses
  its own size and its own cursor and bounds everything (`ar_write_t`, `ar_read_t`,
  `ar_avail_t`): the worst that can happen is lost or repeated audio.

### Protected windows

`wm.setProtected(id, true)` (SDK) marks a window as protected, like the capture protection of other desktop systems
(KeePassXC uses it). The Passwords app uses it (`"protected": true` in its manifest). It looks normal on screen and comes out
**black** in:

- **Screenshots** (`lord.media.screenshot`): the tree is painted again onto a separate canvas in
  "capture mode" (`paint_capture`). A node with `protected` is a black rectangle, and whatever is
  on top of the protected window shows as usual.
- **Screen recording and remote desktop** (`server/src/remote`): if there is any protected window,
  a second canvas is kept in capture mode, repainted only in the region that changed. If there is
  none, the frame is used as is, at no cost. `media_composed` in `main.c` gives that canvas to the
  remote (`mirror_frame`) and to the recording.
- **Thumbnails:** the floating thumbnail and the Screenshots editor come from the same capture.
- **Linux apps (Wayland):** the content, its popup menus and the full screen of a protected window
  also come out black (`wl.c`, `node_protected`).

How it works:

- The SDK sets the `protected` prop on the node of the window's content (`AppHost`).
- The engine stores it in `Node.protect` (`lord.setProp(id, 'protected', bool)`).
- The theme's frame (title included) stays visible, so an app that needs it sets a generic title.
- Screen recording composites on the CPU. Since 0.8.4 a remote desktop viewer no longer does: the
  GPU path reads the composed frame back (`gpu_read_next`), except while a protected window is on
  screen, when the session composites on the CPU so the window can be painted black (`REMOTE.md`).
- Screenshots has a menu item "Privacy → Protect the front window from screenshots" to use it with
  any window.

### Permissions

Screen, microphone and camera go through the system permissions (`sec/perms.c`, `PRIVACY.md`):

- the SDK asks for the permission with `requestPermission` (the system draws the prompt);
- `js_media.c` checks again in C that the saved decision is "allow": `permitted()`; isolated `.lpk` apps go through the broker,
  and their camera through `BOP_CAMERA` (`sec/brokercam.c`, `PRIVACY.md`);
- while in use, the green and orange dots in the menu bar light up (`markDeviceUse`).

### Shortcuts (global, also with a Linux app in front)

| Shortcut | What it does |
|---|---|
| ❖⇧3 or Print Screen | Capture the whole screen |
| ❖⇧4 | Capture an area (Space: a window; Esc cancels) |
| ❖⇧5 | Screenshots panel; if recording, stops the recording |
| Media keys (play/pause, next, previous, stop) | Whatever is playing now (`nowPlaying`) |
| Player: Space, ← →, ⇧← ⇧→, ↑ ↓, F | Pause, ±5 s, ±30 s, volume, full screen |
| Photos: ← →, + −, 0, 1, R, F, Space, ❖P | Previous/next, zoom, fit, 100 %, rotate, full screen, slideshow, print |
| Recorder: R, Space | Record or stop, play |

❖⇧3/4/5 weren't used by anyone (see `FILES.md`, `NOTES.md`, `TERMINAL.md`). The compositor
passes the media keys and Print Screen to LordOS even when a Wayland app has focus (`wl_key`).

## API

### `lord.media` (`server/src/ui/js_media.c`)

| Function | What it does |
|---|---|
| `play(app, path, { loop, native, autoplay, audio, hw, volume, music, appName })` | Player → id. `native`: original size (GIF in Photos) |
| `control(id, 'play'\|'pause'\|'seek'\|'volume'\|'audioTrack'\|'subTrack'\|'loop', value)` | |
| `view(id, { fit, zoom, cx, cy, rotate, mirror, background, brightness, contrast })` | How it's drawn in `<MediaView>` |
| `image(app, path)` → id | Decoded photo (`ready` event) |
| `thumb(path, side)` | Path in `~/.cache/lordos/thumbs` or `null` while it's being made (`thumb` event) |
| `probe(path)`, `waveform(path, bars)`, `poster(path, side)` + `take(id)` | Jobs (`job` event); `poster` leaves RGBA pixels |
| `edit(app, path, output, { rotate, brightness, contrast, cx, cy, cw, ch, refw, format, quality })` | Edited copy |
| `transcode(app, path, output, { format: 'opus'\|'mp3'\|'wav', from, to })` | Export audio with trimming |
| `screenshot(app, { x, y, width, height })` → id, `crop`, `annotate(base, ops, dest)` | Screenshots (`screen` permission) |
| `saveImage(id, output, { format })`, `copyImage(id)` | PNG/JPEG to a file or to the clipboard |
| `screenRecord(app, output.mp4, { mic, fps })`, `audioRecord(app, output.wav)`, `stop(id)` | Recordings (`screen`, `microphone` permissions) |
| `cameras()`, `camera(app, device, { width, height })`, `photo(id, output, mirror)`, `cameraRecord(id, output \| null, { mic, mirror })` | Camera (`camera` permission) |
| `info(id)`, `close(id)`, `protectedCount()`, `hwaccel()` | |

Events: `__lordMedia(id, type, data)`. Types: `info`, `pos`, `state`, `sub`, `stats`, `level`,
`ready`, `error`, `closed`, `done`, `photo`, `video`, `job` and `thumb`.

### `@lordos/sdk`

- `media` (the above with promises and permissions), `usePlayer(id)` (the state survives the app
  being remounted), `nowPlaying`/`useNowPlaying`, `NowPlayingTile` (for a theme's Control Center),
  `useThumb(path)`, `mediaFormatTime` and `userDirs()`.
- `<MediaView media={id} />` (runtime) and the `protected` prop.
- Windows:
  - `wm.setProtected(id, on)`;
  - `wm.setFullscreen(id, on)`: the SDK draws it on top of everything, and Esc exits;
  - `wm.setPip(id, frame | null)`: a small frameless window, always on top and draggable.

  They're painted by `FloatingWindows`, which `startShell` puts in place, so themes don't have to
  do anything.
- `useWindowState(key, initial)`: a window's state that survives it changing place (full screen or
  picture in picture).

## Files and Files (the app)

- **Thumbnails:**
  - they go in `~/.cache/lordos/thumbs/<sha256 of path, date, size and side>.png`;
  - the background worker makes them;
  - the ones that fail aren't requested again during the session;
  - Files uses them for videos, audio covers and the images stb doesn't read (WebP, HEIC, AVIF).
- **"Open with":** Photos for images and Player for audio and video, both as the default option.
- **Quick Preview:** the one in Files (from the documents agent) uses `media.poster`.
- **Where things are saved:**
  - screenshots in `~/Pictures/Screenshots`;
  - camera in `~/Pictures/Camera`;
  - recordings in `~/Music/Recordings`;
  - with xdg-user-dirs the user's folders are respected, and the subfolder's name follows the
    language (`Capturas`, `Cámara`, `Grabaciones` in Spanish).
- **Music library:** saved in `~/.cache/lordos/music.json`, by path, size and date. Only the tags
  of what changed are read again.

## Measurements (VM 119)

Big Buck Bunny 1080p H.264 at 30 frames/s with MP3 audio. VM with 4 vCPU (Xeon Silver 4108 at
1.8 GHz) and virtio-gpu without 3D, so it composites on the CPU and **there is no VA-API**.

| Test | lord-server | lord-media | Frames |
|---|---|---|---|
| `media-test` (no compositor, 960×540) | — | 37–43 % | 30.0/s, 0 dropped in 60 s |
| In the session, windowed (~958×539), before painting from the video | 41.5 % | 38–42 % | |
| In the session, windowed, after | 10.9–13.0 % | 38–49 % | 1962 of 1968 shown, **0 dropped** (60 s) |
| Full screen (1280×720), before | 92.1 % | 50 % | |
| Full screen, after | 14.1–14.7 % | 52–54 % | 14 dropped out of 1971 (0.7 %), when switching to full screen |
| With VA-API | **not measured** | **not measured** | The VM has no driver (`vainfo` fails); the code falls back to software. It needs measuring on the AMD PC (see below) |

CPU as % of one core, from `/proc/<pid>/stat` over 20–60 s; with the video, the VM's total CPU
stays at ~20 %. A/V sync measured inside the player (`|frame pts − audio clock|` when presenting
it, not counting vblank):

| | Mean | Maximum |
|---|---|---|
| Before (lord-media with the PipeWire socket, latency on the same stream) | 0.97–1.01 ms | 25–28 ms |
| Now (lord-server ring + seqlock clock), `media-test` | 1.7–1.9 ms | 4–10 ms |
| Now, in the session (60 s) | 0.55 ms | 31 ms (at startup) |

The test clip is a `testsrc2` with a 100 ms beep every second (`sync.mp4`). In both versions
playback starts with a bit of silence while PipeWire fills the buffer (~0.3 s).

Others: 256 px thumbnail of a 1080p video, 150 ms; decoding 0.5 to 1.4 MP photos, 50–120 ms
(HEIC 118 ms); screen recording with libx264 `veryfast`, variable rate (it only encodes when
something changes), 0 dropped frames.

**To measure VA-API on the AMD PC** (live ISO):
1. `vainfo`: it must say radeonsi and list `H264`.
2. Play a 1080p video and look in `~/.cache/lordos/session.log` for the playback summary line
   (`media: playback (…): …, VA-API, …`).
3. With `python3 server/tests/media_cpu.py 30` (or `top`), compare against
   `LORD_MEDIA_HWACCEL=none`.

## Tests

- `make -C server build/lord-media build/media-test` (needs FFmpeg's `-dev` packages; if they're
  missing, `make` builds everything else without `lord-media`).
- `build/media-test probe|thumb|image|wave|export|edit|play|sandbox` launches `lord-media` the same
  way lord-server does, with no arguments and with fds:
  - `play` measures frames, drops, A/V offset and CPU, with audio through `maudio.c` as in the
    session;
  - `sandbox` checks the sandbox.
- **On VM 119**, with lord-server installed as in the `.deb` (`/usr/lib/lordos`,
  `lordos.service`), the screenshots above were taken:
  - all the apps;
  - permissions;
  - media keys (`sendkey audioplay`);
  - Control Center;
  - full screen and picture in picture;
  - screenshots with a protected window in the screenshot, the recording and remote desktop;
  - clipboard (`wl-paste -t image/png` receives the PNG);
  - camera and recorder;
  - RTL (Arabic).
- **Samples** (free licenses):
  - Big Buck Bunny (Blender Foundation, CC BY 3.0);
  - the EPs "Erase un EP" by Soroa and "Pilorama" by Bulbajar (archive.org, CC BY 3.0), with the
    covers embedded for the test;
  - images from Google's WebP gallery, Wikimedia Commons (rotating earth, public domain), Nokia
    HEIF and link-u (AVIF).
- **Camera:** the VM has no webcam. It was emulated with `v4l2loopback` (`/dev/video10`, "Camara
  virtual (v4l2loopback)") fed by `ffmpeg` (`testsrc2` 1280×720 YUYV), as a
  `lordos-test-camfeed` service.
- **Microphone:** a `module-null-sink` + `module-remap-source` ("Micrófono virtual") with a clip in
  a loop (`lordos-test-micfeed`).

## Pending

- **VA-API measured on real hardware**, and dmabuf → EGL **zero-copy** on the GPU path. Test them
  on the AMD PC.
- **LordBench's video workloads with hardware decoding (maintainer's decision):** `MK_VBENCH` gets no fds and no ioctl
  classes; a hardware workload needs the render node and `SANDBOX_IOCTL_DRM` for that job, which widens its sandbox.
- **Raspberry Pi 5:** HEVC through `v4l2request` over fds (Debian's FFmpeg doesn't have it; the maintainer's decision:
  it needs another FFmpeg, and the device opens `/dev/media*` by path, which the sandbox doesn't allow).
- **Image subtitles** (PGS, DVD): only text ones today.
- **Playback speed** and saved **playlists**.
- **Screenshots:** record only an area (today, the whole screen) and record system audio (today,
  the microphone).
- **Protected windows:** the theme's frame and the title are still visible in screenshots (only
  the content is black).
- **Photos:** save as WebP (there's no encoder without libwebp), metadata (EXIF) in the copy and
  custom albums.
- **Full screen:** once (it didn't happen in two more tries) playback stopped when entering it; the
  cause still needs finding.
