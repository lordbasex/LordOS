# Camera Studio: plan and design for approval

**Status (2026-11-22): research and design FOR REVIEW.** Nothing here is built. No package, module, admin action,
portal, helper or library is added until the maintainer approves the exceptions in section 10 (AGENTS.md: every root
action, helper and library needs the same review). Facts are labelled the way `GO-LINK.md` does it:

- **Read**: checked in this repository or in the cited source on 2026-11-22.
- **Seen**: observed read-only on the maintainer's PC (i9-10900, RX 580, Logitech C930e, Debian 13) on 2026-11-22.
- **Estimate**: a number nobody measured yet. Every estimate has a measurement planned in section 11.
- **To verify**: not checked.

The maintainer's wish (2026-11-22): from the camera status item next to the clock (built on another branch), a popover
like macOS's Video Effects and Mic Mode menu, whose effects reach the webcam of **any** app: Meet in a browser, OBS,
go-link Desk and the others. The effects are background blur, background replacement (pictures and short loops),
chroma key with a real green screen, virtual chroma without one, Center Stage, light and colour matching, filters,
retouch and mirror. The mic modes are Standard and Voice Isolation. Then an app, "Studio", in the spirit of Camo
Studio, with our own design and assets.

## 0. Summary of the recommendation

| Question | Recommendation |
|---|---|
| How apps see the camera | **Two outputs from one pipeline.** (1) **v4l2loopback** `/dev/video50` "LordOS Studio" for Linux apps: on Debian 13 every browser and OBS's virtual-camera path open V4L2 devices. (2) The **ring that LordOS already uses** (`BOP_CAMERA`, `media.camera()`) for LordOS's own apps, isolated `.lpk` apps and go-link Desk. Phase 4 adds (3) a **PipeWire camera node plus the Camera portal** for Flatpak apps and for browsers once they use PipeWire by default. |
| Where the pixels are processed | A new sandboxed helper, **`lord-studio`**: lord-media's sources built with `LM_STUDIO`, the way `lord-stream` is built with `LM_NET`. It gets fds only: the real camera, the loopback, the render node, the model file and the background file. It has no network and never gets the PipeWire socket. |
| GPU or CPU | GLES 2/3 on the render node through GBM and a surfaceless EGL context, the way `bench/lbgpu.c` already works in a sandbox. The CPU path always works too, which the project requires. Segmentation runs on the CPU in both cases. |
| Segmentation model | **MediaPipe Selfie Segmenter, landscape 144×256** (Apache-2.0, model card by Google, a 250 KB float16 `.tflite`). Converted offline to our own flat weight file. |
| Inference runtime | **Our own small inference code in C** for exactly the operators this model and BlazeFace use. There is no new library; output is checked against ONNX Runtime on the dev machine only, the way `hb_compare.py` checks shaping against HarfBuzz. The fallback is Debian's `libonnxruntime1.21` (MIT, in Debian 13 main), linked only by `lord-studio`. That fallback is a decision for the maintainer (D3). |
| Faces (Center Stage, Studio Light, Retouch) | **BlazeFace short range** (Apache-2.0, 128×128). It uses the same operators as the segmenter, so it runs on the same inference code. |
| Mic modes | PipeWire's own **`module-echo-cancel` with WebRTC Audio Processing** (BSD-3, already in Debian 13's `libspa-0.2-modules`; **Seen** installed): noise suppression, high-pass filter and AGC as a "Voice Isolation" source. There is no new library. RNNoise (BSD-3) is not packaged in Debian, so it waits for a later phase. |
| Backgrounds | Only **CC0 and public domain** first (Wikimedia Commons, NASA, Poly Haven, ambientCG), then CC BY and CC BY-SA with credit in the catalog, exactly like `motion.json`. Our own renders are welcome. **Not Unsplash, Pexels or Pixabay** (section 6). The catalog and thumbnails ship in the package; full files download on demand, like moving wallpapers. |

## 1. What exists today (Read, Seen)

- **The real camera, for LordOS's apps.** lord-server validates `/dev/videoN`, checks the `camera` permission in C
  (`js_media.c` `permitted()`, `sec/perms.c`), opens the device and passes the fd only to `lord-media` (`MK_CAMERA`,
  `media/helper/lm_cam.c`: our own V4L2 code with no libv4l, and MJPEG decoded with libavcodec). Frames come back in a
  sealed memfd ring (3 BGRA slots). Isolated apps get a **read-only** reopen of that ring through `BOP_CAMERA`
  (`sec/brokercam.c`, `ui/js_brokercam.c`), and lord-server validates every ACK. Packaged apps with a companion (go-link
  Desk) use the same `media.camera()` with an encoded link (`docs/PACKAGED-APPS.md`). Sources: AGENTS.md ("The camera of
  an isolated app"), `docs/PRIVACY.md` "Camera for isolated apps", `docs/MEDIA.md`.
- **The real camera, for Linux apps.** They open `/dev/video*` themselves. The "no camera" restriction blocks V4L2
  (major 81) in `lord-netperm`'s `cgroup/dev` program. The PipeWire guard (`sec/pwguard.c`) hides every `Video/Source`
  node and destroys `Stream/Input/Video` streams for restricted apps (`docs/FIREWALL.md` lines 177–178).
- **Portals.** lord-server owns `org.freedesktop.portal.Desktop` (**Seen**: `busctl --user status` names lord-server;
  Debian's `xdg-desktop-portal.service` is inactive). It serves FileChooser, OpenURI, Inhibit, GlobalShortcuts,
  ScreenCast and Screenshot. **Camera is "never offered"** (`docs/FLATPAK.md`, the portal table).
- **The only PipeWire video producer** is `lord-screencast`, the only binary that links `libpipewire-0.3`. lord-server
  connects the socket and passes the fds. The helper lowers its own connection to its nodes and hands the app a
  restricted remote (AGENTS.md, `lord-screencast`). That is the template for a camera node in phase 4.
- **GPU inside a sandboxed helper** already works: `lord-bench` runs GLES 2 offscreen on a render node fd with GBM and
  EGL after `sandbox_apply_helper` (`server/src/bench/lbgpu.c`).
- **This PC (Seen).** It has a Logitech C930e (`uvcvideo`, `/dev/video0` and `/dev/video1`). PipeWire node 74 has
  `media.role` Camera. The raw formats are YUYV 640×480 at 30 fps, **1280×720 at only 10 fps** and 1920×1080 at 5 fps.
  720p30 and 1080p30 therefore need MJPEG, which `lm_cam.c` already decodes. The PipeWire version is 1.4.2 and
  WirePlumber is 0.5.8. `v4l2loopback` is not loaded. The machine has Chromium 156 and Google Chrome 158 (Debian
  packages) and OBS Studio 33.0.3 (Flatpak). `libonnxruntime1.21` is installed as a dependency of
  `gstreamer1.0-plugins-bad`.
- **Debian 13 candidates (Seen, `apt-cache policy`).** These are present: `v4l2loopback-dkms` 0.15.0-2,
  `libonnxruntime-dev` 1.21.0, `libopencv-dnn-dev` 4.10.0, `libwebrtc-audio-processing-1-3` 1.3,
  `libspa-0.2-modules` (with `aec/libspa-aec-webrtc.so` and `filter-graph` LADSPA and LV2 plugins). These are absent:
  RNNoise, ncnn, TensorFlow Lite or LiteRT, and libfacedetection.

## 2. Virtual camera architecture

### 2.1 What each app actually opens on Debian 13

| App | Camera path | Sees a v4l2loopback device | Sees a PipeWire camera node |
|---|---|---|---|
| Chromium 156 and Chrome 158 (deb) | V4L2 by default. PipeWire camera through the **Camera portal**, behind `chrome://flags#enable-webrtc-pipewire-camera` (feature `WebRtcPipeWireCamera`), added around M116 to M127. As far as the sources show it is **off by default** (**To verify** on 156 and 158). | **yes** (needs `exclusive_caps=1`) | only with the flag **and** a Camera portal |
| Firefox (Debian ESR or Mozilla's) | V4L2. PipeWire through the portal when `media.webrtc.camera.allow-pipewire` is true. That is the default in Fedora 41 and later; for upstream the sources show "not by default" (**To verify**). It falls back to V4L2 when the portal request fails (Mozilla bug 1849608). | **yes** | only with the pref **and** a Camera portal |
| Meet, Teams web, Zoom web | whatever the browser does | yes | as the browser |
| OBS Studio (Flatpak 33, Seen) | Input: "Video Capture Device (V4L2)", and since 30.1 "Video Capture Device (PipeWire)" through the Camera portal. Output: its virtual camera **needs v4l2loopback**. | yes (Flatpak with `--device=all`) | only through a Camera portal |
| Flatpak apps in general | V4L2 if the manifest grants devices, otherwise the Camera portal | with device access | only through a Camera portal |
| go-link Desk, the Camera app, `.lpk` apps | LordOS's `media.camera()`: lord-server opens the device | not needed | not needed: they get the **Studio ring** directly |
| GTK 4 and GStreamer apps (Snapshot…) | `pipewiresrc` through the Camera portal, or V4L2 | yes | with a portal |

**Conclusion.** On Debian 13 today, a PipeWire node alone would reach **no** mainstream app on LordOS. Browsers use
V4L2 by default, and every PipeWire camera client goes through `org.freedesktop.portal.Camera`, which LordOS doesn't
serve. Only v4l2loopback reaches Meet in Chrome and Chromium. The PipeWire node and portal are the future-proof path:
Fedora already defaults Firefox to it. They also give a cleaner permission story (section 8), so they come in phase 4.

### 2.2 v4l2loopback: cost and privileges

- **What it is.** It is an out-of-tree kernel module (GPL-2.0), packaged as `v4l2loopback-dkms` 0.15.0 in Debian 13
  main. DKMS builds it on the machine and needs `linux-headers-amd64` and a compiler. `FILES.md` measured about 500 MB
  for `apfs-dkms`, and this is in the same range (**Estimate**).
- **Secure Boot.** Debian's DKMS signs with `/var/lib/dkms/mok.pub`, which must be enrolled in MOK at the next boot.
  This is exactly the flow `NVIDIA-PLAN.md` A.5 designs with the proposed `gpu-mok-enroll` action. **Reuse that action**
  (renamed `dkms-mok-enroll`) instead of adding a second one (E-S2).
- **Raspberry Pi 5.** LordOS builds its own Pi kernel (`RASPBERRY.md`, "The LordOS kernel"). Build `v4l2loopback` as a
  module in that kernel build (`CONFIG_` is not upstream, so it builds out of tree in the same script), and ship it
  prebuilt in the image. That needs no DKMS or headers on the board.
- **Immutable edition.** DKMS is impossible on a verified `/usr` (`IMMUTABLE.md`), so the module must ship prebuilt in
  the image. Until then, Studio offers only the LordOS ring there.
- **Configuration, static and root-owned, shipped by an optional package `lordos-studio-camera`.** The module's
  options live in `/etc/modprobe.d/lordos-studio.conf`:
  `options v4l2loopback devices=1 video_nr=50 card_label="LordOS Studio" exclusive_caps=1 max_buffers=4`. The file
  `/etc/modules-load.d/lordos-studio.conf` loads it at boot. There is **no runtime root action** to create or remove
  devices: `/dev/v4l2loopback`'s control ioctls stay unused. Installing and removing the package goes through the
  Store's existing package engine.
- **Chrome lists a loopback device only while it advertises CAPTURE**, which `exclusive_caps=1` ties to a writer being
  present. `lord-studio` therefore keeps the output open and writes an **idle card** at 1 fps: a static "LordOS Studio"
  picture from memory, with the real camera **closed**. It opens the real camera only when a reader starts streaming.
  v4l2loopback signals this with its client-usage event (`V4L2_EVENT_PRI_CLIENT_USAGE`, 0.12.7 and later; **To
  verify** on 0.15). This keeps the camera light honest.
- **Formats.** It writes YUYV or NV12 at the chosen output size: 1280×720 by default, 1920×1080 on request, and
  640×360 for Pi CPU mode. WebRTC accepts all three.

### 2.3 Alternatives considered

| Option | Why not (now) |
|---|---|
| PipeWire node only | It reaches no browser by default (2.1), and it needs a new portal. It stays as phase 4. |
| `pipewire-v4l2` (`pw-v4l2` LD_PRELOAD) | It works per app launched through a wrapper, but not for sandboxed Chromium and Firefox renderers. It is fragile, and LordOS would have to rewrite launch command lines. |
| `vivid` or `v4l2-loopback`-like modules in mainline | There is none. `vivid` is a test generator that can't be fed frames. |
| Effects inside each app | Effects on the ring only reach LordOS's apps. The wish is "any app". |

### 2.4 The three outputs

```
            /dev/video0 (real camera; lord-server opens it, fd)
                         │
                         ▼
   ┌──────────────────── lord-studio (sandbox_apply_helper, no network, no PipeWire) ─────────────────────┐
   │ V4L2 capture → MJPEG/YUYV decode → segmentation (CPU) → face track → compose (GPU or CPU) → outputs │
   └───────────┬───────────────────────────────┬──────────────────────────────────┬───────────────────────┘
               │ write() YUYV/NV12             │ sealed memfd ring (BGRA)        │ (phase 4) same ring,
               ▼                               ▼                                 ▼ read-only reopen
   /dev/video50 "LordOS Studio"     lord-server: popover preview,      lord-screencast in "camera" mode:
   (v4l2loopback; lord-server       Camera app, BOP_CAMERA "studio",   Video/Source node, media.role Camera,
    opened it O_RDWR, fd)           go-link Desk (media.camera)        restricted remote via Camera portal
```

## 3. Processing pipeline

### 3.1 The helper

- **The binary.** `lord-studio` is built from lord-media's sources with `-DLM_STUDIO` plus `server/src/media/studio/*.c`,
  as `lord-stream` is built with `LM_NET` (`server/Makefile` line 945). It links the same FFmpeg and libva set and,
  when `GPU=1`, Mesa's `libgbm`, `libEGL` and `libGLESv2` (already in the policy table). It links **no new library**
  under the recommended option. lord-media itself stays unchanged and refuses `MK_STUDIO`.
- **Startup.** It starts from a fixed path (`<lord-server dir>/lord-studio`, owned by root or the user and not writable
  by others), with no arguments, `LANG` only and the channel on fd 3. `sandbox_apply_helper` runs before anything is
  read. The extra ioctls are the V4L2 class (capture and loopback output) and the DRM and dma-buf class with the render
  node. There is one process per session, and it lives only while Studio is on.
- **Fds it receives, all opened by lord-server:**
  - the real camera, after the person's choice in the popover;
  - the loopback (`O_RDWR`, checked as a V4L2 device whose `card` is "LordOS Studio" and whose driver is
    `v4l2 loopback`);
  - the render node;
  - the model files (from `/usr/share/lordos/studio/`, SHA-256 pinned in the source and checked by lord-server before
    the fd goes out);
  - the background (a catalog file in `/var/lib/lordos/studio/`, or one of the person's pictures or short videos
    through the same checks as `wallpaper-image`).
- **Things it never gets:** a path, a socket, the PipeWire socket, network, exec or a writable folder.
- **The channel.** Fixed-size records: `ST_CONFIG` (the effect parameters as numbers and enums, never text), `ST_BG`
  with an fd, `ST_CAMERA` with an fd, and `ST_STOP`. Replies are `ST_STATE` (camera open, readers, fps, frame time) and
  `ST_RING`. lord-server checks every reply (a `st_msg_valid` modelled on `lb_msg_valid`).
- **A crash** puts the idle card back from lord-server: it re-spawns the helper at most 3 times a minute, then turns
  Studio off with a notification. The compositor never waits on it (non-blocking socket, as with lord-media).

### 3.2 Per frame

1. **Capture**: MJPEG at 720p30 or 1080p30 (this C930e gives raw 720p at only 10 fps), decoded with libavcodec into
   NV12 or I420. VA-API JPEG decode is possible on the RX 580 later.
2. **Downscale** to 256×144 RGB for the segmenter, and every 6th frame to 128×128 for BlazeFace (5 face updates a second
   is enough with tracking).
3. **Segment**: the network gives a 256×144 person probability.
4. **Refine** (3.4): temporal smoothing, then a guided filter upsampled to the output size with full-resolution luma as
   the guide.
5. **Frame** (Center Stage): crop and scale the camera picture and the matte with the same smoothed rectangle.
6. **Compose**: background (blur, picture, video frame or chroma), relight, filters and LUT, retouch, mirror, overlays.
7. **Out**: write to the loopback and the ring. The ring gets only the frames a reader asked for (preview open, or
   `BOP_CAMERA` consumers).

### 3.3 GPU and CPU paths and the budget

The deadline at 30 fps is 33.3 ms. The goal is to **add at most one frame of latency** and to stay under about 10 % of
the CPU on the PC.

| Stage | PC, GPU (RX 580, radeonsi) | PC, CPU only | Pi 5, GPU (V3D 7.1, GLES 3.1) | Pi 5, CPU only |
|---|---|---|---|---|
| MJPEG 720p decode | 2–3 ms (1 core) | same | 6–9 ms | same |
| Segmenter 256×144, own C (1 thread, AVX2 / NEON) | 2–4 ms | same | 8–14 ms | same |
| BlazeFace 128×128, every 6th frame | < 1 ms on average | same | ~2 ms on average | same |
| Guided filter plus composite at 720p | 1–2 ms (shaders) | 6–10 ms | 4–7 ms | 20–30 ms (too slow) |
| Upload and readback (720p) | ~1 ms | — | ~3 ms | — |
| **Total at 720p30** | **~7–11 ms** | ~12–18 ms | **~23–33 ms** (tight) | over budget |
| 1080p30 | ~10–15 ms | ~20–28 ms | not offered | not offered |

All of these are **estimates**. They are scaled from published single-thread numbers: libfacedetection's own table on
an i7-7820X and a Pi 4, and MediaPipe's reported mobile latencies. They need measuring (section 11).

The **Pi 5 policy** follows from those numbers: 720p30 with the GPU, 640×360 at 30 fps on the CPU, and blur and
picture only (no video backgrounds) when the CPU is above 85 % (as moving wallpapers already pause, `WALLPAPERS.md`).
The segmenter runs on a second thread so that capture and composite overlap. Mask latency is then one frame, hidden by
the temporal filter.

## 4. Segmentation and matting

### 4.1 Models: licenses of code **and** weights

| Model | Input | Size | License (code / weights) | Verdict |
|---|---|---|---|---|
| **MediaPipe Selfie Segmenter, landscape** (Google, 2021) | 144×256 | 250 KB f16 | Apache-2.0 / **Apache-2.0** (Google's model card PDF; the LiteRT community card says the same) | **Chosen.** Real-time on a phone CPU. Out-of-scope uses in the card: surveillance, more than 4 m, several people at very different scales. |
| MediaPipe Selfie Segmenter, square | 256×256 | ~250 KB | Apache-2.0 / Apache-2.0 | Spare: for portrait-shaped crops |
| MediaPipe Selfie Multiclass (hair, body, face, clothes) | 256×256 | f32, larger | card on LiteRT (**To verify** the license) | Later: hair-aware edges and retouch masks |
| Google Meet segmentation (2020) | 256×144 / 160×96 | small | Google moved its license **away from Apache-2.0** in January 2021 (Jitsi's commit notes) | **Rejected** |
| PP-HumanSeg v2 Lite (PaddleSeg) | 256×144 | 5.4 MB | Apache-2.0 / Apache-2.0 (ONNX in OpenCV Zoo, Apache-2.0) | Good second choice. 20× heavier. |
| MODNet (ZHKKKe) | 512×512 | 25 MB | **Apache-2.0** in the current README. It was earlier CC BY-NC-SA 4.0, and some forks still say so. | Optional "best quality" on the PC only. Pin the commit, and keep a copy of the README stating Apache-2.0. Heavy (about 15–30 ms on the PC, **Estimate**). |
| Robust Video Matting (PeterL1n) | any | 15 MB | **GPL-3.0** code. The weights have no separate license, so treat them as GPL-3.0. | **Rejected**: GPL-3.0 weights in an MIT project's helper. Also too heavy for the Pi. |
| BodyPix (TF.js) | — | 4–90 MB | Apache-2.0 | Rejected: superseded, slow, and JS-oriented |
| U²-Net / rembg | 320×320 | 4.7–176 MB | Apache-2.0 (U²-Net) / MIT (rembg) | Rejected: photos, not real time |

Apache-2.0 obligations: the model card and license go in `THIRD_PARTY.md` and next to the files. Our converted weight
file is a derivative, so it states the conversion (Apache-2.0 §4(b)).

### 4.2 Runtime

| Runtime | License | In Debian 13 | Cost | Verdict |
|---|---|---|---|---|
| **Own C inference** (`media/studio/nn.c`) | ours (MIT) | — | About 1500 lines. Operators: conv 2D, depthwise conv, 1×1 conv, add, mul, hard-swish, ReLU, sigmoid, average pool (squeeze-excite), bilinear resize, transposed conv, concat. f16 weights are converted to f32 at load. Hand-written AVX2 and NEON kernels for 1×1 and depthwise, the rest autovectorised. | **Recommended.** No new dependency, auditable, and a fixed op set that fails closed on anything else. |
| ONNX Runtime C API 1.21 | MIT | **yes** (`libonnxruntime1.21`, 16.5 MB, pulls oneDNN, XNNPACK, protobuf, onnx, re2 and abseil) | It is an exception only for `lord-studio`. Its large C++ surface parses only our own pinned model, never untrusted data. arm64 availability is **To verify**. | Fallback (D3), and the reference oracle in tests on the dev machine. |
| TensorFlow Lite / LiteRT C API | Apache-2.0 | **no** | Vendoring means Bazel or CMake, abseil and flatbuffers | Rejected |
| ncnn | BSD-3 | no | A vendored C++ library (about 30k lines) and a model conversion | Rejected (vendoring) |
| OpenCV DNN 4.10 | Apache-2.0 | yes | A large dependency tree for one small model | Rejected |

**Feasibility of our own code.** The segmenter is a MobileNetV3-style encoder-decoder with about 100k parameters
(**Estimate** from the 250 KB f16 file), and BlazeFace has about 100k. Both use only the operators above. A converter
`tools/studio/tflite2lnn.mjs` runs once on the maintainer's machine. It parses the `.tflite` flatbuffer and writes a
flat file of tensors with a header listing each layer, and it refuses any unknown operator. The helper parses that file
with strict limits: count, shapes and total size. It is our own format, from our own package, and its hash is
checked, but the parser is still written as if the file were hostile.

**Correctness test.** `build/studio-nn-test` runs both models on 20 fixed frames shipped in `tests/data/studio/`
(CC0 or our own pictures), compares against reference outputs from ONNX Runtime stored as files, and requires a mean
absolute error below 1e-3. That mirrors `tests/hb_compare.py`.

### 4.3 Making the matte good

- **Temporal stability.** An exponential moving average per pixel on the low-resolution mask, with a weight driven by
  motion (absolute frame difference at 256×144). Still areas get α = 0.2 and moving areas α = 0.7. Hysteresis follows
  at 0.4 and 0.6 so that edges don't shimmer.
- **Edge refinement: the guided filter** (He, Sun and Tang, ECCV 2010 and TPAMI 2013; the algorithm is free to
  implement). It is O(N) with box filters, uses full-resolution luma as the guide, radius 8 at 720p and ε 1e-3. It
  keeps hair and fingers that a bilinear upsample loses. It runs as two box-filter passes on the GPU, or with a
  separable running sum on the CPU.
- **Light wrap.** Blur the background at quarter resolution and add it at the person's edge, weighted by
  `α·(1-α)`-like falloff. That ties the cut-out into the new background.

### 4.4 Chroma key with a real green screen (C or GLSL)

- **Key colour.** Picked automatically from the median of the frame's border pixels in YCbCr, or with an eyedropper on
  the preview. Green and blue presets are offered.
- **Alpha.** From the distance `d` in the CbCr plane to the key colour: `α = smoothstep(similarity, similarity +
  smoothness, d)`, with luma excluded so shadows on the screen still key. Two sliders, Similarity and Smoothness, are
  the standard model. This is our own code; OBS's filter is GPL-2.0 and none of it is copied.
- **Spill suppression.** For green, `G' = min(G, (R+B)/2)`, or the "average" variant `G' = min(G, max(R,B))`, blended
  by a Spill amount. For blue the same is done on B.
- **"Better cut-out".** The final α is the chroma α **inside a dilated person mask**. Green objects away from the person
  stay, and a wrinkled screen behind the person doesn't punch holes. Where chroma is unsure (`0.1 < α < 0.9`), the
  guided-filter matte decides.
- **Virtual chroma** (no green screen) is plain segmentation (4.3) with the same compositor. It is a mode, not a
  separate algorithm.

## 5. Faces, framing, light and colour

### 5.1 Face detection: choice

| Option | License | Notes | Verdict |
|---|---|---|---|
| **BlazeFace short range** (MediaPipe) | Apache-2.0 (Google's model card, "Short Range" PDF) | 128×128 SSD with anchors. It gives 6 keypoints (eyes, nose, mouth, ears), which Studio Light and the framing headroom need. The same operators as the segmenter. | **Chosen** |
| libfacedetection (Shiqi Yu) | BSD-3 (its LICENSE) | Pure C++ with no dependencies. 2.1 ms at 128×96 on an i7-7820X and 15 ms on a Pi 4 (its README). Its weights are compiled into C arrays. | Good fallback. It adds a C++ file set. |
| Haar cascades (OpenCV) | Apache-2.0 (4.x) / BSD-3 | Old, frontal only, many false positives, and OpenCV as a dependency | Rejected |

### 5.2 Center Stage (auto-framing)

- **Target box.** The union of the face boxes, widened by a shoulders factor of 2.2× the face width and with headroom
  that puts the eyes at 1/3 of the height. Output aspect is 16:9.
- **Smoothing.** A 1€ filter (Casiez et al., CHI 2012) on the box centre and size, with a dead zone (no move under 4 %
  of the width) and a maximum pan speed. That gives camera-operator motion, not jitter.
- **Zoom limits.** At most 1.5× from a 1080p capture to a 720p output, which is lossless. Above 1.5× it upscales, so
  there is a slider "Framing: wide ↔ tight".
- **Cameras with UVC pan, tilt and zoom** (the C930e advertises digital PTZ, **To verify** with `VIDIOC_QUERYCTRL`)
  could be driven instead of cropping. That is a later improvement; cropping works with every camera.
- **No face** for 2 s: ease back to the full frame.

### 5.3 Relighting and blending

- **Colour match** (Reinhard, Ashikhmin, Gooch and Shirley, "Color Transfer between Images", 2001). It works in a
  decorrelated space (Lab or lαβ). It moves the person's mean and standard deviation towards the new background's,
  measured on the background region near the person, with a Strength slider (default 30 %, at most 60 %). The
  statistics are smoothed over about 1 s so that the person doesn't flicker when the video background changes.
- **Exposure match.** The luma median of the person is matched towards the background's, clamped to ±1 EV.
- **Studio Light** (in the spirit of Apple's). Lift the face region (an ellipse from the BlazeFace box, feathered) by
  +0.3 to +0.6 EV and dim the background by −0.5 EV.
- **Spotlight** (Camo's name). A radial vignette centred on the face.
- **Edge light (rim).** Add the background's blurred colour along the alpha gradient (4.3's light wrap, but stronger
  and coloured).
- **Adjustments** (Camo's "Adjust"): exposure, contrast, saturation, temperature and tint, sharpness, and highlights
  and shadows. These are simple per-pixel operations or one 3D LUT built from them (the cheapest path on both GPU and
  CPU).

### 5.4 Filters, retouch, mirror, overlays

- **Filters.** 3D LUTs (33³) made by us with `tools/studio/gen-luts.mjs`: Warm, Cool, Vivid, Mono, Noir, Film, Fade and
  Sepia. They are generated, not copied, so they are MIT. On the GPU a LUT is a 3D texture (GLES 3) or a 2D strip
  (GLES 2). User `.cube` import (Camo supports Cube, 3DL and LUT) comes later, parsed in the helper with strict limits
  (size at most 65³, finite values, 1 MB).
- **Retouch.** A skin mask (a YCbCr skin range inside the face ellipse and the person matte) and an edge-preserving
  smoothing (a guided filter with self-guide, ε larger) blended by a Strength slider, plus a little under-eye lift.
  There is no reshaping of faces.
- **Mirror.** The preview is mirrored by default (people expect a mirror). The output is **not** mirrored unless the
  person chooses it (the other side should read text correctly). This follows FaceTime and Camo.
- **Overlays and scenes** (Camo calls them Scenes: Watermark, Meet & Greet, Presentation, Off-camera, BRB). They are
  phase 5, drawn by lord-server, not the helper. lord-server already composes text and pictures. It renders an overlay
  layer (name and title lower third, a logo watermark from the person's file, an "Off camera" card) into a second
  small sealed ring, and `lord-studio` composites it. That way there is no text, font or SVG parsing in the helper.

## 6. Backgrounds: sources, licenses, sizes

### 6.1 License verdict

| Source | License | Ship in LordOS? |
|---|---|---|
| **Wikimedia Commons**, files marked CC0 or public domain | CC0 / PD | **Yes, preferred.** Moving wallpapers already use it (`motion.json`: Extemporalist CC0 timelapses, NASA). |
| **NASA** (images, SVS visualisations) | US Government public domain. NASA asks that its insignia not be used to imply endorsement. | **Yes** (no logos, no astronauts' faces) |
| **Poly Haven** (HDRIs, textures) | **CC0** | **Yes.** An HDRI panorama rendered to a 16:9 view gives studios, offices, nature and interiors with no people. A good source for stills. |
| **ambientCG** (textures) | **CC0** | **Yes**, for abstract and material backgrounds |
| **Blender open movies** (Sintel, Spring, Charge, Sprite Fright…) | **CC BY 4.0** (some older ones CC BY 3.0), credit "Blender Foundation | studio.blender.org" | **Yes, with credit**, for loops and stills. The characters are not trademarks, but avoid frames whose main subject is a character, because they read as a "fan" background. |
| CC BY-SA | share-alike applies to the file only | Acceptable, as `motion.json` already does. Credit, plus a link to the license. |
| **Unsplash** (Unsplash License) | Free use, **but** "doesn't include the right to compile photos from Unsplash to replicate a similar or competing service". It is not an open license, not DFSG, and it has no model or property releases. | **No** |
| **Pexels** (Pexels License) | Free use, **but** no selling unaltered copies and no redistribution on other stock, wallpaper or "similar" platforms | **No.** A distro catalog of backgrounds is exactly that. |
| **Pixabay** (Content License) | Free use, **but** forbids "standalone" redistribution and sale | **No** |
| Our own renders and pictures (the maintainer's, LordOS brand) | MIT or our own brand assets (like `assets/wallpapers/`) | **Yes.** Includes LordOS-branded "office" and "studio" scenes. |
| AI-generated by us | as `assets/wallpapers/README.md` (the maintainer's own generator output) | Maintainer's decision (D7) |

**Recommendation.** Use CC0 and PD first (Commons, NASA, Poly Haven, ambientCG), then CC BY and CC BY-SA with credit
(Blender Studio, Commons), plus our own. **No Unsplash, Pexels or Pixabay.** Their licenses are free-to-use, not
open-source. They forbid building a collection or redistributing it, and Debian would treat them as non-free.
**Exclude identifiable people, logos, readable text and trademarks** from every background: people need model
releases, and a background with a brand reads as endorsement.

### 6.2 What to ship and how

- **Catalog.** `assets/studio/backgrounds.json` uses the same shape as `assets/wallpapers/motion.json`: id, category,
  author, license, source URL, credit line, sizes and a SHA-256 per file. Its 320×180 thumbnails ship in the package
  (about 100 × 15 KB ≈ 1.5 MB).
- **Stills.** 60–80 pictures in Office, Home, Studio, Nature, City, Abstract, Space and LordOS. 1920×1080 JPEG at
  quality 90 is about 0.4–0.8 MB each, so about 50 MB for 80. **Ship 12 in the package** (about 8 MB: the defaults of
  each category). The rest download on demand.
- **Loops.** 50–80 clips of 4–8 s at 1080p, HEVC and H.264 like moving wallpapers, about 2–5 MB each (≈ 250 MB). All of
  them download on demand. They play **without hardware decode too**: a 1080p30 software decode in `lord-studio` is
  affordable on the PC (**Estimate** 1 core at about 15 %). The Pi uses 720p HEVC with its hardware decoder, or
  stills only.
- **Downloads.** Extend the existing closed admin action family (`server/src/hw/wallpaper_admin.c`) with
  `studio-bg-get "<id> <variant>"`, which writes `/var/lib/lordos/studio/<sha256>.<ext>`. The rules are the same: JS
  names an entry, never a URL; the base is the project's GitHub owner on `raw.githubusercontent.com`; it goes through
  `fetch_batch` as `nobody` in the Store's scope; size and hash are checked; anything else is deleted (E-S4).
- **Files** live in the existing `LordOS-wallpapers` repository under `studio/` (or a sibling `LordOS-studio-assets`).
  The README credits each file, as the wallpapers' does.
- **The person's own pictures and loops (≤ 10 s)** go through the same conversion and checks as "Your pictures"
  (`wallpaper-image`) and are kept in `~/.local/share/lordos/studio/`.
- **Curation, one person, about two days.** (1) Search Commons with `incategory:` and `haslicense:cc0`/PD filters,
  Poly Haven by category and Blender Studio films. (2) Record license, author and URL from **each file's own page**.
  (3) Reject anything with people, logos or text. (4) Crop and grade to 16:9 1080p and loop with the existing fade
  method (`WALLPAPERS.md`, "Format"). (5) Get a second person to review the credits. (6) Have a `brand-check`-style
  script refuse catalog entries whose license isn't in {CC0, PD, CC BY x, CC BY-SA x, MIT-own}.

## 7. Mic modes

| Mode | What it is | How |
|---|---|---|
| **Standard** | the microphone as is | the hardware source |
| **Voice Isolation** | removes steady and impulsive noise, keeps one voice | PipeWire `libpipewire-module-echo-cancel` with `library.name = aec/libspa-aec-webrtc`. WebRTC Audio Processing 1.3 (BSD-3) is in Debian 13 and already installed (**Seen**). Arguments: `webrtc.noise_suppression = true`, `webrtc.high_pass_filter = true`, `webrtc.gain_control = true`, `webrtc.transient_suppression = true`. Echo cancellation needs playback through its sink, so it is off unless the person enables "Echo cancellation" (D8). |
| **Wide Spectrum** (Apple's third mode, optional) | no processing at all, for music | the hardware source with no filters |

- **The source.** A package drop-in, `/usr/share/pipewire/pipewire.conf.d/lordos-voice.conf` (root-owned, static, like
  `lordos-bluez-profiles.conf` in `SOUND.md`), creates the source "LordOS Voice Isolation" (`media.class` Audio/Source)
  and keeps it paused while unused.
- **Selecting a mode.** It uses lord-server's existing Pulse client (`SOUND.md`): set the default source, or move one
  app's source output, which the Control Center already does for outputs. Moving the echo-cancel capture stream to the
  chosen hardware microphone uses the same move command. There is **no new privilege**, no command run and no module
  loaded at runtime.
- **Restrictions still hold.** The PipeWire guard hides every `Audio/Source` from a no-mic app, so the virtual source is
  covered. The orange indicator names the app reading the virtual source, plus "Voice Isolation".
- **RNNoise** (Xiph, BSD-3; a 48 kHz recurrent network, very light) is **not in Debian**. The LADSPA wrapper
  `noise-suppression-for-voice` is GPL-3.0 and not packaged either. If WebRTC's suppression isn't enough in tests, a
  later phase could ship our own LADSPA build of RNNoise, but that **runs inside the user's PipeWire daemon**, outside
  every LordOS sandbox, and needs its own approval (D9).

## 8. Security and privacy

1. **Frames live only in `lord-studio`** (sandboxed, no network, no PipeWire socket, no paths) and in sealed rings.
   lord-server never decodes or processes camera frames, and the rule "decoders only in helpers" holds.
2. **The indicator stays truthful.**
   - The green dot shows the **real camera** in use and **who** uses it through Studio. An example: "Chromium · via
     LordOS Studio (Logitech C930e) · effects on".
   - Readers of the loopback are found by lord-server scanning `/proc/<pid>/fd` of the session's processes for the
     loopback's `st_rdev`. That happens only while Studio reports readers (from the client-usage event), at 1 Hz, for
     the same uid only. The process is mapped to its app with `appid_of_pid`, as notifyd does.
   - The real camera's own light turns off when no reader and no preview is active, because the helper closes the
     device and keeps only the idle card.
   - Turning Studio off with a reader attached sends a "Camera off" card, not the raw camera. The raw camera never
     reaches an app that chose "LordOS Studio" without effects being on, unless the person chose "No effects".
3. **Per-app permission and restrictions are enforced on the virtual camera too.**
   - **Linux apps:** `cgroup/dev` "no camera" blocks major 81, which covers `/dev/video50` (**To verify** in
     `tests/fwlinux/devices.sh`, which already uses v4l2loopback). The PipeWire guard covers the phase-4 node, because
     it is a `Video/Source` that is not a screen.
   - **LordOS apps and `.lpk`:** the "studio" entry in `media.cameras()` checks the `camera` permission exactly like a
     device (`permitted()`, `BOP_CAMERA`). Revoking kills the consumer's access, not Studio.
   - **Phase 4 Camera portal:** LordOS's own prompt per app, answered by the person. The restricted PipeWire remote can
     see **only** the Studio camera node, as ScreenCast's remote sees only its streams. There is never a persistent
     token without the person (D5).
4. **Known limit: same-uid spoofing.** Any unsandboxed process of the person could open `/dev/video50` as OUTPUT
   before `lord-studio` does and feed it. Mitigations:
   - lord-server opens the device at session start and keeps the writer role (v4l2loopback allows one writer once
     streaming);
   - Studio shows a warning if it can't take the output.

   An unsandboxed same-uid process could also read the real `/dev/video0` today. This is the existing Linux-app trust
   model (`FIREWALL.md`), not a new hole. It is stated honestly in `PRIVACY.md`.
5. **No network**, either for the helper or for the effects. Backgrounds come only through the closed download
   action. Models ship in the package.
6. **Protected content.** Studio never captures the screen. "Presentation" overlays of a window (phase 5) would go
   through `paint_capture` (protected windows stay black), as AGENTS.md requires for any new path that copies pixels.
7. **Settings.** `~/.config/lordos/settings.json` (already in `fsops_protected`) holds the Studio choices. Nothing in
   them is a security decision, so no new deny-list entry is needed. The camera permission stays in `permisos.json`.

## 9. User interface

### 9.1 The popover (all themes)

It opens from the camera status item next to the clock. The component is `sdk/studio.jsx`, so Lord Flow, Lord
Classic, Lord Workspaces and Celeste get it with no theme code beyond placing the item (the rule "per-display and
shared features in the SDK"). It follows `textAlign: 'start'/'end'` for right-to-left languages, and the preview has
`direction: 'ltr'` because it uses mouse x for the eyedropper.

```
┌─ Camera ─────────────────────────────────────── ⚙ ─┐
│ ┌────────────────────────────────────────────────┐ │
│ │                                                │ │
│ │            live preview (mirrored)             │ │
│ │                                                │ │
│ └────────────────────────────────────────────────┘ │
│ Camera   [ Logitech Webcam C930e            ▾ ]    │
│ In use   Chromium · effects on                ●    │
│────────────────────────────────────────────────────│
│ ◐ Portrait (blur)                      [on ]   ›   │
│ ▣ Background                           [off]   ›   │
│ ✦ Studio Light                         [off]   ›   │
│ ⌖ Center Stage                         [on ]   ›   │
│ ◑ Filters          Warm                        ›   │
│ ☺ Retouch                              [off]   ›   │
│ ⇋ Mirror my video                      [off]       │
│────────────────────────────────────────────────────│
│ Mic Mode   ( Standard | Voice Isolation | Wide )   │
│────────────────────────────────────────────────────│
│ Open Studio…                                       │
└────────────────────────────────────────────────────┘
```

"›" opens a detail page in the same popover:

- **Portrait**: blur amount.
- **Background**: None, Blur, Picture, Video, Green screen; then the thumbnails grid, "Add your own…" and Download.
- **Studio Light**: intensity.
- **Center Stage**: wide ↔ tight.
- **Filters**: the grid of LUTs with strength.
- **Retouch**: strength.

The icons are the project's own symbols, not Apple's.

### 9.2 The Studio app (`apps/studio`)

Its name is a decision: "Studio" (D6).

```
┌ Studio ──────────────────────────────────────────────────────────────────────────────┐
│ Device: [ Logitech C930e ▾ ]  Output: 1280×720 30 fps ▾   ● In use: Chromium          │
├──────────────────┬──────────────────────────────────────────────┬───────────────────┤
│ ▸ Background     │                                              │ Scenes            │
│   ○ None ● Blur  │                                              │ ┌──────┐┌──────┐ │
│   ○ Picture      │              PREVIEW (16:9)                  │ │Default││Meet &│ │
│   ○ Video        │                                              │ │      ││Greet │ │
│   ○ Green screen │       [ before | after ]  toggle             │ └──────┘└──────┘ │
│ ▸ Light          │                                              │ ┌──────┐┌──────┐ │
│   Studio Light ━━│                                              │ │Presen-││ Off  │ │
│   Match colours ━│                                              │ │tation ││camera│ │
│ ▸ Framing        │                                              │ └──────┘└──────┘ │
│   Center Stage ☑ │                                              │ + Save scene      │
│   Zoom ━━━ Pan ✥ │                                              │───────────────────│
│ ▸ Adjust         │                                              │ Overlays          │
│   Exposure ━━━━  ├──────────────────────────────────────────────┤ ☐ Name and title  │
│   Temperature ━━ │ ● Rec   ⏸   00:00   │  fps 30 · 8 ms · GPU    │ ☐ Watermark       │
│ ▸ Filters  Warm  │                                              │ ☐ "Be right back" │
│ ▸ Retouch  ━━    │                                              │                   │
│ ▸ Microphone     │                                              │                   │
│   Voice Isolation│                                              │                   │
└──────────────────┴──────────────────────────────────────────────┴───────────────────┘
```

- **Camo-inspired sections**, our own layout and names:
  - Device;
  - Background (Blur and Privacy blur, Replace, Green screen);
  - Light (Studio Light / Spotlight, colour match, rim);
  - Framing (Center Stage, manual zoom and pan, rotate);
  - Adjust (exposure, contrast, saturation, temperature, tint, sharpness);
  - Filters, Retouch and Microphone;
  - on the right, Scenes and Overlays;
  - at the bottom, Record. Recording uses the existing `cameraRecord` path in lord-media, H.264 + AAC into a file from
    the save dialog. Streaming is left to OBS.
- **Scenes** are named bundles of all settings, stored in `settings.studio.scenes`.
- **The app** runs in lord-server's context (like Camera) and uses `lord.studio.*`, registered only in the desktop's
  own context. Isolated apps never get control of Studio.

### 9.3 Settings and persistence

- `settings.studio`:
  - `on`;
  - `camera` (the device's stable id: `/dev/v4l/by-id` name, never `/dev/videoN`);
  - `output: {w, h, fps}`;
  - `effects: {portrait, background: {mode, id}, light, framing, filter, retouch, mirror}`;
  - `mic: 'standard' | 'voice' | 'wide'`;
  - `scenes: []`.
- These are new keys with defaults, so no migration is needed. If a key is renamed later, it goes in `MIGRATIONS`
  (`sdk/settings.js`).
- **Pure logic** (`sdk/studiomodel.js`) has node tests (`server/tests/studio_model_test.mjs`): effect validation,
  scene merge, the Pi policy and the indicator text.
- **i18n.** All strings use `t('studio.*')` in every language (the maintainer's rule: all languages 100 %).

## 10. Privileges and exceptions to approve (AGENTS.md proposals)

| Id | Exception | Scope | Why |
|---|---|---|---|
| **E-S1** | New sandboxed helper **`lord-studio`** (lord-media sources plus `LM_STUDIO`), linking FFmpeg and libva (as lord-media does) and Mesa GBM, EGL and GLES (as lord-bench does) | fixed path; no arguments; fds only (camera, loopback, render node, model, background); V4L2 and DRM ioctl classes; no network, no PipeWire, no exec | all camera pixels processed in one sandbox |
| **E-S2** | Optional package **`lordos-studio-camera`**: `v4l2loopback-dkms` (Debian main, GPL-2.0) plus static `/etc/modprobe.d` and `/etc/modules-load.d` files; on amd64 it pulls headers and a compiler (DKMS) | installed from the Store by the existing package engine; no runtime root action; on Secure Boot it reuses `gpu-mok-enroll` renamed **`dkms-mok-enroll`** (NVIDIA-PLAN A.5) | browsers and OBS only see V4L2 devices |
| **E-S3** | Pi image: v4l2loopback built in the LordOS Pi kernel script and shipped prebuilt | kernel build only | no DKMS on the Pi |
| **E-S4** | Admin action **`studio-bg-get "<id> <variant>"`** (and `studio-bg-remove`, admin only), the twin of `wallpaper-get` | same closed rules: catalog id, hash, size, `nobody` fetch, the Store's scope | on-demand backgrounds |
| **E-S5** | `lord.studio.*` JS API in the desktop context only, plus a "studio" source in `media.cameras()` and `BOP_CAMERA` | camera permission checked as for devices; ACK validation unchanged | LordOS apps and go-link Desk |
| **E-S6** | PipeWire drop-in `lordos-voice.conf` (module-echo-cancel with WebRTC; Debian's own module) | static file; mode selection through the existing Pulse client | Voice Isolation |
| **E-S7** (phase 4) | Serve **`org.freedesktop.portal.Camera`** (`IsCameraPresent`, `AccessCamera`, `OpenPipeWireRemote`), today "never offered", plus `lord-screencast` gaining a **camera mode** (a `Video/Source` with `media.role` Camera fed from Studio's ring; restricted remote with only that node) | prompt per app; the guard and the indicator as above; no persistent grant without the person | Flatpak apps, Firefox and Chromium on PipeWire |
| (D3) **E-S8**, only if our own inference is refused | `libonnxruntime1.21` linked **only** by `lord-studio` | data: our own pinned models | runtime |

Nothing else changes: lord-server links nothing new, gets no new root path, and runs no command.

## 11. Phases, effort, tests

The effort is for one developer with the existing patterns. These are **estimates**.

| Phase | Content | Effort | Done when (verified on the PC **and** the Pi 5, with a screenshot) |
|---|---|---|---|
| **0. Measure** | Prototype `nn.c` plus the converter. Time the segmenter and BlazeFace on the i9 and the Pi 5. Compare against ONNX Runtime on the dev machine. Load v4l2loopback on the **VM** (not this PC) and check that Chromium 156, Chrome 158, Firefox ESR and Flatpak OBS see it. | 4–5 days | Real numbers replace section 3.3's estimates, written into this file. |
| **1. Pipeline and LordOS ring** | `lord-studio`: capture, segmentation, guided filter, blur and picture backgrounds, mirror, GPU and CPU paths. The "LordOS Studio" camera in `media.cameras()` (the Camera app, go-link Desk, `.lpk`). The popover with preview, Portrait and Background. | 2–3 weeks | `build/studio-nn-test`, `build/studio-test` (pipeline on fixed frames), `studio_model_test.mjs`, engine damage tests unchanged |
| **2. Loopback and "any app"** | `lordos-studio-camera` package, the idle card, the client-usage event, reader attribution, the indicator text, the `dkms-mok-enroll` reuse, Pi kernel module | 1–2 weeks | Meet in Chromium and in Chrome shows the effects. The no-camera restriction blocks `/dev/video50` (`tests/fwlinux/devices.sh`). The camera light goes off with no reader. |
| **3. More effects and mic** | Video backgrounds, green screen, Center Stage, Studio Light, colour match, filters, retouch. Voice Isolation drop-in and Mic Mode. Background catalog plus `studio-bg-get`. Curation of about 70 stills and 50 loops. | 3 weeks plus 2 days of curation | each effect checked against reference frames; Mic Mode heard in a Meet call; credits page |
| **4. PipeWire camera** | Camera portal plus lord-screencast camera mode | 1–2 weeks | OBS Flatpak "Video Capture Device (PipeWire)" and Firefox with the pref on see "LordOS Studio"; the guard hides it from a restricted app |
| **5. Studio app** | the full app, scenes, overlays from lord-server, recording, user LUTs | 2–3 weeks | screenshots in every theme and language direction |

Every phase also updates CHANGELOG, the paper (en, es, pt), the site and `THIRD_PARTY.md` (model cards, Apache-2.0
NOTICE), as CLAUDE.md requires, and `PRIVACY.md` and `MEDIA.md` (new sections).

## 12. Risks

| Risk | Impact | Mitigation |
|---|---|---|
| DKMS on amd64: 500 MB of toolchain, breakage on kernel updates, Secure Boot enrolment | install friction | optional Store item; MOK flow shared with NVIDIA; ship prebuilt in images later |
| v4l2loopback and browser quirks (`exclusive_caps`, OBS 31 plus v4l2loopback 0.14 regression reported on Arch) | camera missing in some apps | phase 0 matrix on the VM; pin known-good versions; the idle card keeps the device listed |
| Pi 5 CPU budget | dropped frames | GPU path required on the Pi for 720p, CPU mode capped at 640×360, effects disabled by load |
| Our own inference has a bug | bad masks | a reference test against ONNX Runtime on fixed frames, as shaping does with HarfBuzz |
| Licenses of model weights drift (MODNet history) | legal | pin commits and file hashes; keep copies of model cards in `third_party/studio-models/` |
| Matting edges on hair and glasses | quality vs Camo/Apple | guided filter, light wrap, optional MODNet on the PC, green screen |
| Same-uid process feeding the loopback | spoofed video | hold the writer role, warn; documented as the Linux-app trust model |
| Double noise suppression (Meet's own plus ours) | robotic voice | Voice Isolation off by default; a tip in the popover |

## 13. Decisions for the maintainer

1. **D1.** Approve the architecture: v4l2loopback plus the LordOS ring now, and the PipeWire node plus Camera portal in
   phase 4 (E-S7)?
2. **D2.** Approve `lord-studio` (E-S1) as lord-media sources built with `LM_STUDIO` (recommended), or a job inside
   lord-media itself?
3. **D3.** Our own C inference (recommended, no new library), or Debian's ONNX Runtime linked only by `lord-studio`
   (E-S8)?
4. **D4.** `lordos-studio-camera` as an **optional** Store item (recommended), or a default Recommends of the `lordos`
   package (which brings DKMS and a toolchain to every amd64 install)?
5. **D5.** Camera portal policy (phase 4): a prompt per app and remembered per app like other permissions, with
   revocation in Settings → Privacy?
6. **D6.** Name: "Studio" or "LordOS Studio"? Status item label "Camera"?
7. **D7.** Backgrounds: CC0/PD plus CC BY and CC BY-SA with credit plus our own; no Unsplash, Pexels or Pixabay. Allow
   our own AI-generated backgrounds, as the wallpapers already do?
8. **D8.** Voice Isolation with or without echo cancellation (the latter needs playback through the echo-cancel sink)?
9. **D9.** RNNoise later as a LADSPA plugin inside the user's PipeWire daemon (outside LordOS's sandboxes), only if
   WebRTC NS isn't good enough?
10. **D10.** Default output 1280×720 at 30 fps, and 1080p offered only with the GPU path?

## 14. Sources

Repository (Read): `AGENTS.md` (lord-media, lord-stream, lord-screencast, lord-bench, broker camera, admin actions);
`docs/ARCHITECTURE.md` (dependency policy table); `docs/MEDIA.md`; `docs/PRIVACY.md` ("Camera for isolated apps",
indicators); `docs/FIREWALL.md` (no-camera: `cgroup/dev`, PipeWire guard); `docs/FLATPAK.md` (portal table: Camera
never offered); `docs/WALLPAPERS.md` and `assets/wallpapers/motion.json` (catalog, licenses, on-demand downloads);
`docs/SOUND.md` (Pulse client, PipeWire drop-ins); `docs/NVIDIA-PLAN.md` A.5 (MOK); `docs/RASPBERRY.md` (own Pi
kernel); `docs/IMMUTABLE.md` (no DKMS); `docs/GO-LINK.md` and `docs/PACKAGED-APPS.md` (`media.camera()`);
`server/src/media/helper/lm_cam.c`; `server/src/bench/lbgpu.c`; `server/src/sec/sandbox.h`; `server/src/sec/pwguard.c`.

External:

- Apple, macOS Video Effects and Mic Mode in Control Center:
  https://support.apple.com/es-us/guide/mac-help/mchlp2980/mac and
  https://support.apple.com/es-lamr/guide/facetime/fctme7c07113/mac
- Camo Studio: https://camo.com/es/support/camo/how-to-use-camo ,
  https://reincubate.com/support/camo/adjusting-video-settings , https://reincubate.com/blog/launching-camo-2 ,
  https://reincubate.com/blog/camo-2-3
- MediaPipe Image Segmenter (models and model cards):
  https://developers.google.com/edge/mediapipe/solutions/vision/image_segmenter/index ;
  https://huggingface.co/litert-community/MediaPipe-ImageSegmenter-Selfie-Landscape (Apache-2.0, 250,177 bytes);
  Google's selfie model card PDF (Apache-2.0):
  https://developers.google.cn/static/ml-kit/images/vision/selfie-segmentation/selfie-model-card.pdf
- MediaPipe Face Detector (BlazeFace short range):
  https://developers.google.com/edge/mediapipe/solutions/vision/face_detector/index ; model card "MediaPipe BlazeFace
  Model Card (Short Range)" on storage.googleapis.com/mediapipe-assets (Apache-2.0, **To verify** by reading the PDF)
- Google Meet model and the license change: https://research.google/blog/background-features-in-google-meet-powered-by-web-ml/ ;
  Jitsi commit "add segmentation model license information":
  https://forger.sitiv.fr/apitech/jitsi-meet/commit/8eed42c273511bc0019221b720458f60ee33e776
- MODNet: https://github.com/ZHKKKe/MODNet (README: Apache-2.0; earlier CC BY-NC-SA 4.0)
- Robust Video Matting: https://github.com/PeterL1n/RobustVideoMatting (GPL-3.0)
- PP-HumanSeg: https://github.com/PaddlePaddle/PaddleSeg/tree/release/2.8/contrib/PP-HumanSeg ;
  https://huggingface.co/opencv/human_segmentation_pphumanseg (Apache-2.0)
- libfacedetection: https://github.com/ShiqiYu/libfacedetection (LICENSE: 3-clause BSD; README benchmarks)
- RNNoise: https://github.com/xiph/rnnoise (COPYING: BSD-3-Clause)
- ONNX Runtime: https://github.com/microsoft/onnxruntime (MIT); Debian `libonnxruntime1.21` 1.21.0+dfsg-1
- Guided filter: K. He, J. Sun, X. Tang, "Guided Image Filtering", ECCV 2010 / TPAMI 2013
- Colour transfer: E. Reinhard et al., "Color Transfer between Images", IEEE CG&A 2001
- 1€ filter: G. Casiez, N. Roussel, D. Vogel, CHI 2012
- Firefox PipeWire camera: https://bugzilla.mozilla.org/show_bug.cgi?id=1849608 ;
  https://jgrulich.cz/2024/08/19/making-pipewire-default-option-for-firefox-camera-handling ;
  https://www.phoronix.com/news/Firefox-116-PipeWire-Camera
- Chromium PipeWire camera: https://webrtc.googlesource.com/src/+/f0be3bee1fc17d96354ce3c9bf457673b82e7d2d ;
  https://fosdem.org/2023/schedule/event/om_chromium
- OBS: 30.1 release notes ("Video Capture Device (PipeWire)"): https://obsproject.com/blog/obs-studio-30-1-release-notes ;
  v4l2loopback and OBS: https://github.com/umlaeute/v4l2loopback/wiki/OBS-Studio ; Arch regression:
  https://gitlab.archlinux.org/archlinux/packaging/packages/obs-studio/-/issues/11
- v4l2loopback: https://github.com/umlaeute/v4l2loopback (GPL-2.0); Debian `v4l2loopback-dkms` 0.15.0-2
- PipeWire echo-cancel module: https://docs.pipewire.org/page_module_echo_cancel.html ; WebRTC Audio Processing
  (BSD-3): https://gitlab.freedesktop.org/pulseaudio/webrtc-audio-processing
- Licenses of photo sites: https://unsplash.com/license , https://www.pexels.com/license/ ,
  https://pixabay.com/service/license-summary/
- Open sources: https://commons.wikimedia.org (CC0/PD filters), https://polyhaven.com/license (CC0),
  https://ambientcg.com (CC0), https://studio.blender.org/films (CC BY), https://www.nasa.gov/nasa-brand-center/images-and-media/
