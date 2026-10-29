# System Information, LordBench and hardware reports (proposal)

**Status: design proposal. Built and released in 0.8.5-beta (2026-10-27; see "Work plan for 0.8.5" below): the collector, `lordos-sysreport report|fetch`, the System Information app (Overview, Hardware, Compatibility, Checks, Report), the Sandbox Probe and the shared measurement library `server/src/bench`. LordBench (0.8.6) is in development: the `lord-bench` helper, its CPU, memory, GPU, storage and sustained workloads, the interface (layout, paint, shaping) and video workloads, the desktop-impact mode (the compositor's frames, their phases and pointer-to-screen latency under load), the boot test, the guards, `lordos-sysreport bench` and System Information's Performance tab are built, with two of the three reference machines (VM 116 and the Raspberry Pi 5 16 GB; see "Built so far" under 0.8.6); the modest x86 PC reference is not. Community reports (0.8.7) and a `checks` subcommand of `lordos-sysreport` are not built.** It plans three releases and a later stage:

| Release | What |
|---|---|
| **0.8.5 — System Information** | Every piece of hardware detected, what LordOS uses (above all the GPU), the services, the paper's claims checked on this machine, JSON/Markdown reports. |
| **0.8.6 — LordBench** | Measurements of CPU, memory, GPU, the compositor, video and storage under recorded, reproducible conditions, with versioned results. |
| **0.8.7 — Community hardware reports** | Publishing reports as GitHub issues, validating them, and a tested-hardware catalogue on lordos.org that the app also reads. |
| **Later — Lumo and AutoTune** | `ROADMAP.md`, "Lumo, the local assistant": explains a report and tunes settings from what the machine measured. |

Each claim of the paper, how it is checked today and what is still missing: `PAPER-VALIDATION.md`.

The aim is that every claim LordOS makes (memory at idle, GPU use, isolation) can be backed by a
reproducible report, not only a screenshot. Results must be technically defensible first and
attractive second.

## Architecture: the logic is not in the interface

The collector, the checks and the benchmark are native components with no UI, used by three
clients so no logic is duplicated:

```
server/src/sysreport/        collector + checks (library, read-only, no root)
server/src/bench/            LordBench workloads (library)
        │
        ├── lord-server: lord.sysinfo (js_hw.c) → apps/sysinfo        (the app)
        ├── lordos-sysreport  report | fetch | bench [--quick|--full] [--json]  (CLI, runs as the user)
        └── tests (fake sysfs trees, fixtures) and, later, Lumo's tools
```

- **`lordos-sysreport`** is an ordinary user command in `/usr/bin` that the user or a test script
  runs; `lord-server` never runs it. It writes the same JSON as the app, so reports can be made on
  a machine without a session (a headless Pi, a VM over SSH, CI).
- **Shared with the Test Bench.** `lordos-testbench` (0.8.5, `src/testbench`, a Wayland client that checks
  the display server from the outside) and LordBench keep their own names and jobs, but the measurement
  pieces live once, in `server/src/bench/`, and both link them: `bench.c` (the clock; frame times with
  average, p50, p99 and frames that missed the refresh budget; presentation accounting with vblanks skipped
  and zero-copy frames; input-to-screen latency; the GL renderer's kind; JSON strings; the report written
  atomically to `~/.local/state/lordos/<name>.json`) and `glscene.c` (the fixed 3D scene, drawn with GLES2
  in whatever context is current: the bench's wayland-egl window, LordBench's offscreen context). Tested by
  `build/bench-test`. LordBench's workloads go next to them.
- **`lord-bench`** is the helper the app starts for workloads, launched like `lord-doc` and
  `lord-media`: fixed path (`<lord-server dir>/lord-bench`), no arguments, minimal environment,
  channel on fd 3, `sandbox_apply_helper()` before any work; it gets only the render node and a
  temporary file as fds. It links libc, Mesa's GBM/EGL/GLES (already used by the GPU path) and the
  vendored QuickJS (one of its workloads), nothing new. The CLI links the same library directly.

## 0.8.5 — System Information

One native app, **System Information** (`apps/sysinfo`), also opened from About LordOS →
**System Report…**. Tabs: **Overview**, **Hardware**, **Compatibility**, **Checks**, **Report**
(LordBench adds **Performance** in 0.8.6).

### Hardware

A sidebar tree and a detail pane, with vendor and device names from `pci.ids` and `usb.ids`:
hardware overview (model, vendor, firmware version and date, board, chassis type, Secure Boot),
CPU (cores, threads, frequencies, caches, SSE4.2/AVX/AVX2/AVX-512/FMA, NEON/SVE, governor,
microcode), memory (total, swap, zram; per module through the read-only admin action
`hw-memory-modules`), graphics, displays (monitor name and size from the EDID, mode, refresh,
scale, card, scanout), storage (interface and link speed, size, SMART health), USB and
Thunderbolt, PCI with drivers and missing firmware, audio, network, Bluetooth, power, thermal and
fans, cameras and input, Raspberry Pi (revision, bootloader, the firmware's under-voltage alarm and, where the
kernel exposes them, its throttling flags,
fan header, kernel revision), software (Debian, kernel, the kernel options LordOS depends on, the
LordOS services and their state, the code manifest).

Serials, UUIDs, MAC addresses and EDID serials are shown only on screen, on request, and never
enter a report.

### The GPU as separate layers

"Is the GPU used" is not one answer. For every card the app shows each layer on its own, with
**Available**, **Not available**, **Not checked** or **Not measurable on this driver**:

| Layer | How it is known |
|---|---|
| Device detected | PCI or platform device, vendor and model |
| Kernel driver loaded | the device's `driver` link (`amdgpu`, `i915`, `xe`, `nouveau`, `nvidia-drm`, `v3d`/`vc4`, `virtio_gpu`…) |
| DRM/KMS available | a card node with connectors, modesetting working (`kms.c`) |
| Render node | `/dev/dri/renderD*` openable by the session |
| EGL context active | `lord-server` created one; the GL renderer string; hardware or software (llvmpipe) |
| Composition | the GPU path is on, or the exact reason it's off (the same one the log gives) |
| Direct scanout | seen during a fullscreen video or app, with the count since the session started |
| Video decode | VA-API profiles per codec (`MK_HWPROBE`); on the Pi 5 the HEVC V4L2 decoder |
| Video encode | the remote desktop's encoder when it's used |
| Vulkan | ICD files present only ("Not checked"); loading Vulkan belongs to stage 2 |

Live load is shown only where the driver exposes it (`amdgpu`: busy % and VRAM); elsewhere the app
says it isn't measurable instead of showing a zero. The Nvidia proprietary driver is shown as
**untested** with an invitation to send a report.

### Checks: configured versus effective

Each check says what it verified, at one of two levels, and never claims the stronger one without
the test:

- **Configured**: the mechanism is present (a kernel option, a service running, a flag in
  `/proc`).
- **Effective**: a probe tried something that must fail, and it failed.

| Id | Paper | Configured | Effective |
|---|---|---|---|
| `sec.server-user` | §2.2 | `lord-server`'s uid ≠ 0 | — |
| `sec.app-sandbox` | §6.4 | `Seccomp: 2`, `NoNewPrivs: 1` on an isolated app | a probe app (a system package started like any isolated app) tries to read a file outside its grant, open a socket, `ptrace`, create a user namespace and reach another app's scope; every attempt must be refused |
| `sec.helper-sandbox` | §6.7 | — | a child of the probe app that takes `sandbox_apply_helper()` checks that a unix socket, `exec`, `fork` and the app's data folder fail (0.8.5); `lord-bench` repeats it after its own lock-in on every LordBench run, with an internet socket, a new file and a system file too, shown in Performance's Stability verdict (0.8.6, in development) |
| `sec.landlock` | §6.4 | Landlock ABI ≥ 6 | covered by the probe app |
| `sec.netperm` | §6.6 | `lord-netperm` running, programs attached, devices and Bluetooth programs loaded | the probe app, denied the network by a temporary rule, fails to connect to a local listener |
| `sec.code` | §6.9 | manifest present | every loaded bundle verified in this session |
| `sec.remote-closed` | §11.4 | remote desktop setting | nothing listens on 5673 when it's off |
| `sec.cgroups` | §11.4 | root-owned session tree, one scope per app | — |
| `perf.idle-memory` | §7.1 | — | memory used after 60 s idle at the desktop, with the session's resolution and number of displays recorded |
| `perf.idle-cpu` | §7.1 | — | CPU idle over 30 s with nothing open |
| `perf.boot` | §7.1 | — | systemd's firmware, loader, kernel and userspace times |
| `gpu.used` | §7.3 | hardware renderer exists | GPU path on, or a warning with the reason |
| `sys.time`, `sys.firmware`, `sys.fans`, `sys.memcg`, `sys.pi-bootloader`, `sys.updates` | various | as listed in the Hardware tree | — |

Thresholds are constants in `apps/sysinfo/checks.js`, signed with the theme bundle (the separate
`checks.json` first planned was not needed). A failed check offers **Report a problem** with the bug template.

### Compatibility

A panel per component (GPU, display, storage, network, Wi-Fi, Bluetooth, audio, power, suspend)
with its state: **Works**, **Works with limits** (and which), **Doesn't work**, **Not checked**. In
0.8.5 it comes only from this machine's detection and checks; from 0.8.7 it also shows what other
users confirmed for the same device and LordOS version, marked as community data.

### Reports

A versioned JSON (`schema: 1`) and a Markdown summary, saved to `~/Documents`:

- **Built from a fixed allow-list of fields.** Serials, UUIDs, MAC and IP addresses, host and user
  names, Wi-Fi network names and home-folder paths are never collected into it; a unit test feeds a
  fake system full of such values and checks none reaches the output.
- **Preview first**: the exact text that would be shared, field by field.
- **Explicit consent** for every share, never remembered.

## 0.8.6 — LordBench

### Two kinds of tests, kept apart

- **Peak performance**: the workload gets the machine. LordBench first waits for a quiet system (CPU
  idle above a threshold for 10 s) and refuses or marks the result if something else keeps the CPU
  busy; it records the load it saw.
- **Desktop impact**: the same workloads at low priority while the compositor animates a scripted
  scene, measuring the compositor's frame times and input latency. This answers "does the desktop
  stay smooth under load", a separate question with separate results.

### Workloads

| Category | Workloads |
|---|---|
| CPU, one core and all cores | SHA-256 (`pkg/crypto.c`), deflate (built as xz decoding, below), a matrix multiply; LordOS's own work: layout and paint of a fixed scene, shaping a fixed paragraph in several scripts, QuickJS running a fixed script |
| Memory | bandwidth (copy, triad) and latency (pointer chase) |
| GPU | offscreen GLES on the render node: fill rate, texture upload, composing N screen-size layers, timed with fences |
| Compositor | `lord-server`'s own frame counters during a scripted window drag, CPU path and GPU path |
| Video | short test clips we generate (H.264, HEVC, VP9, AV1; 1080p and 4K) decoded in `lord-media`, hardware and software (only software decoding is built, below); the remote desktop's encoders |
| Storage | sequential and random I/O on a size-limited temporary file (Full only, with a warning on SD cards) |
| Boot | the seconds from the kernel's start to the first frame on the monitor, recorded by `lord-sessiond` on every boot with the version and build (`/var/lib/lordos/boot-times.jsonl`, built in 0.8.5; [RESULTS.md](RESULTS.md), "Boot time"): LordBench reads that history, shows each version's median and spread over its boots (the first boot after an update apart: it builds caches), and with the user's consent can reboot N times in a row to measure it |
| Sustained | several minutes of all-core load (Full only) |

### Frame times against the refresh budget

The compositor results are reported as the **real time to build a frame** (paint plus present),
and separately as the share of the display's budget it uses (16.7 ms at 60 Hz, 8.3 ms at 120 Hz,
6.9 ms at 144 Hz) and the frames missed. No fixed threshold for every display: a check passes when
the 99th percentile fits the budget of the refresh rate actually in use.

### Thermal and power safety

- The limits come from the hardware: the thermal zones' passive and critical trip points, the
  CPU's own throttle state (cpufreq dropping below the requested frequency), the Pi's throttling
  and under-voltage flags. A run stops before the passive trip point and when throttling starts in
  a peak test; in the sustained test throttling is the result, recorded with its time and
  temperature.
- No run on battery below 30 %; a laptop on battery is recorded as such.

### Recorded conditions

Each result carries what affects it: resolution, scale and refresh rate of every display, GPU path
on or off, power source and profile, governor, temperatures at start and end, throttling seen, the
system load before starting, kernel, Mesa and LordOS versions, LordBench version. Two results are
compared only when the LordBench version matches; differing conditions are shown next to the
comparison.

### Results without one magic number

Three separate verdicts, never folded into one score:

- **Performance**: per category, normalized to a set of **reference machines** (initially a
  Raspberry Pi 5, a modest x86 PC and a VM; the Pi 5 measured is the 16 GB one), shown as "×N of reference" for each, so a board
  and a workstation are each compared with something similar.
- **Compatibility**: from the Compatibility panel.
- **Stability**: errors and crashes during the run, frames missed under load, throttling.

### Built so far (in development for 0.8.6, 2026-10-27 and 2026-10-28)

Unreleased; the latest release is 0.8.5-beta. Checked on VM 116 (Proxmox VE, 8 vCPU of a Xeon Silver
4108, 4 GB, virtio disk, no GPU passthrough: Mesa's llvmpipe) from a terminal (`build/lordbench-cli`) and
in an isolated headless session through System Information → Performance, and on a Raspberry Pi 5 16 GB
(SD card, the LordOS kernel 6.18.57-lordos.3, V3D 7.1, the fan on its header) from a terminal and in the
maintainer's own session on a 1920×1080 60 Hz monitor (HDMI-A-1 and HDMI-A-2 both tried), with a 0.8.6
development package.

- **`lord-bench`** (`server/src/bench/lbmain.c`, `mk/lordbench.mk`): launched by lord-server
  (`bench/lbrun.c`) from its own folder only (owned by root or the user, not writable by others), no
  arguments, `LANG` only (and `LORDOS_SESSIOND` in a lord-sessiond session), the channel on fd 3. It
  receives one fixed-size request (`LbGo`, `bench/lbproto.h`) with at most two fds lord-server opened: the
  GPU's render node (`/dev/dri/renderD128…`) and, for Full, an unnamed scratch file (`O_TMPFILE` in
  `~/.cache`, at most 1 GB and a quarter of the free space). Then it locks itself in with
  `sandbox_apply_helper` before any work (scope `lordos-bench.scope`, 2 GB memory cap, read-only
  `/usr/lib`, `/usr/share`, `/sys/dev/char`, `/sys/devices`, `/sys/firmware/devicetree` and `/dev/dri` for
  Mesa (without the last two, Mesa's loader on the Pi 5 could not identify the V3D device and every GPU
  measurement was skipped; the GPU itself is still reached only through the render node fd), DRM and dma-buf ioctls only when
  it got the render node, no sockets, no exec, threads only). It answers only with fixed-size records
  (`LbMsg`: a measurement id, a value and its spread); lord-server checks each one (`lb_msg_valid`: the
  magic, a known kind and id, finite values in range) and writes the JSON itself. Stopping is a SIGKILL.
- **Workloads** (`bench/lbwork.c`, `bench/lbui.c`, `bench/lbgpu.c`), each repeated three times and reported as the
  median with its spread (max − min), on one core and on all of them:
  - CPU: SHA-256 (`pkg/crypto.c`, with the CPU's SHA instructions when present); a 256×256
    double-precision matrix product; decompressing a fixed 2 MB text with the package engine's own xz
    decoder (`pkg/xz.c`; the text is generated at build time by `bench/lbcorpus.c` and compressed with
    `xz -9`), instead of the deflate in the design above; QuickJS running a fixed script (strings, JSON,
    sorting, objects, a regular expression), one runtime per thread.
  - LordOS's own interface work (`bench/lbui.c`), with the engine's real code linked into `lord-bench`
    (`ui/node.c`, `style.c`, `layout.c`, `paint.c`, `text.c`, `canvas.c`, the shaper `otf.c`, `shape*.c`,
    `bidi.c`; the Wayland, terminal, media and GPU hooks of `paint.c` are stubs, as in `tests/engine_test.c`):
    - **Layout and paint** (`cpu.ui.1`, `cpu.ui.n`, frames per second): a fixed 1920×1080 scene built in C,
      529 nodes: a translucent top bar with backdrop blur, a file manager (title bar, a 14-item sidebar, a
      toolbar, a 40-row list with ellipsized names), a settings window with a wrapped paragraph and ten rows of
      fields and switches, a notification with backdrop blur, a half-transparent card, a rotated badge and the
      Dock with backdrop blur and twelve shadowed icons, over wallpaper shapes. One frame invalidates every node,
      lays the tree out, computes the damage and paints the whole screen; the glyph, text and shadow caches stay
      warm, as on a running desktop (one frame before the clock starts fills them). `cpu.ui.1` is everything on
      one core; `cpu.ui.n` the same frames with the paint's row-local loops split in bands over up to 8 threads
      (`canvas_parallel`, the same pieces as lord-server's paint threads, `par.c`, without their real-time
      priority), so it measures what the compositor's own threads gain, not N independent copies: the engine
      keeps one tree per process.
    - **Shaping** (`cpu.shape.1`, `cpu.shape.n`, runs per second, summed over the threads): one run shapes five
      fixed paragraphs (`bench/lbtext.h`: Latin with kerning, ligatures and accents; Arabic with marks, `lam-alef`
      and embedded left-to-right words and numbers; Devanagari with conjuncts, reph and reordered vowel signs; Thai
      with stacked marks and SARA AM; Chinese with kana and full-width punctuation), each as the first step of the
      engine's text layout (`textlayout.c`): bidi levels, scripts with the common characters joining their
      neighbours, each character's font (Inter if it has the character, else its script's), each run of one
      font, script and direction through `shape_run` (GSUB, the Arabic, Indic and Thai shapers, GPOS), then the
      visual order of the paragraph as one line. Each thread has its own shaper and font tables.
    - **No files.** The scene is fixed data and the fonts are bytes inside the binary (`build/gen/lb_fonts.h`,
      made at build time by `bench/lbcorpus.c`): LordOS's Inter Regular (the scene draws every weight with it and
      uses only characters it covers, so the engine never looks for a fallback font), and Debian 13's Noto Sans Arabic,
      Devanagari, Thai and CJK SC subset to exactly the characters of those paragraphs and their decompositions,
      with every layout feature and lookup they reach (`tools/gen-lbfonts.py`, run by hand when a paragraph
      changes; the results are committed in `server/src/bench/fonts` with their OFL licence, so every build shapes
      with the same bytes; Inter is pinned by its SHA-256 in `mk/lordbench.mk`, `LB_INTER_SHA256`, and a new Inter
      fails the build until the references are measured again). With each subset `build/shape-test` gives the
      same glyph positions as with the full system font, but it is not the same work: the subsets shape about
      12 % faster than the full fonts (fewer lookups, smaller coverage tables; VM 116, 587–631 against 514–560
      runs/s on one core), so the workload is comparable only with itself. Line breaking is not part of the run: the engine's Thai word breaker reads libthai's dictionary
      from a file. A trace of a run shows these workloads open nothing; the helper's sandbox, seccomp list and fds
      are unchanged. `lord-bench` grows from 1.48 to 2.42 MB of code (about 0.63 MB of fonts) and 0.46 MB of
      static data.
    - **They measure LordOS's engine together with the machine.** An optimisation of `paint.c`, `layout.c` or the
      shaper changes these four numbers on the same hardware. So a reference value of one of them carries the
      LordOS version it was measured with (`"lordos"` in `apps/sysinfo/references.js`; `ENGINE_IDS` in
      `perf.js`) and gives a "×N" only to a result of that same version; a release re-measures them on the
      reference machines.
  - Memory: copy and triad bandwidth on all cores (32 MB arrays in Quick, 128 MB in Full) and the
    latency of a dependent pointer chase over 64 MB.
  - GPU: OpenGL ES 2 offscreen on the render node (GBM + EGL, a surfaceless context, a 1920×1080
    framebuffer object; every frame ends with `glFinish`): the 3D scene of the Test Bench
    (`bench/glscene.c`), fill rate with 16 blended full-screen quads, 1920×1080 RGBA texture uploads and
    8 blended screen-size layers.
  - Storage (Full only): sequential write with `fdatasync`, sequential read after dropping the page
    cache for that file, random 4 KiB reads for 3 s; the file is emptied at the end and, being unnamed,
    disappears with the fd.
  - Sustained (Full only, 3 minutes from the app): the matrix product on every core, a sample every
    10 s with the temperature lord-server read at that moment.
- **Video** (after the helper's measurements, Quick and Full): a `lord-media` job (`MK_VBENCH`,
  `media/helper/lm_vbench.c`, launched from lord-server's folder by `bench/lbvideo.c`, no fds at all). It makes its
  own test clips in memory from a synthetic picture (gradients, moving blocks and a little noise, the same every
  time) with this system's FFmpeg: H.264 (libx264 `veryfast`, 8 Mbit/s), HEVC (libx265 `ultrafast`), VP9 (libvpx
  realtime) and AV1 (SVT-AV1 preset 12, else libaom or rav1e) at 1080p, and H.264 and HEVC at 4K. Each clip is
  decoded in software the way the player does (every core, frame and slice threads) over and over for at least a
  second, three times, as frames per second. Then the remote desktop's own encoders with their settings
  (`lm_stream_sw_encoder`: libx264 `ultrafast`/`zerolatency`, libvpx VP8 realtime) encode 1920×1080 frames as fast
  as they can. Quick measures H.264 at 1080p and the H.264 encoder; an encoder or decoder this FFmpeg lacks is
  "skipped: no codec". lord-media answers only `MM_BENCH` records (a measurement, a kind, a rate in thousandths),
  checked by `lbv_record_valid` before they become results. Hardware decoding is not measured: neither test
  machine has it (no VA-API on VM 116; the Pi 5's HEVC decoder needs FFmpeg's `v4l2request`, which Debian's FFmpeg
  lacks).
- **Desktop impact** (its own button, its own result file `lordbench-desktop.json`): after the quiet wait,
  System Information opens a second window with a scene (cards, shadows and a square turned through
  `lord.setStyle` with the engine's `rotate` key; until 0.8.6's frame-cost work it sent a `transform` key the engine
  doesn't have and stood still, so desktop results from before that change measured a lighter scene and aren't
  comparable) and lord-server moves that window along a figure of eight through the drag's own path
  (`lord.wm.script`, `grab_script_begin` in `ui/grab.c`: painted once into a layer, one step per frame, no JS per
  frame). lord-server records every frame it builds and every frame the main display shows (`bench/lbframes.c`,
  from `render()` and the page flips; one atomic read per frame when off): 10 s alone, then 10 s while `lord-bench`
  loads every core at the lowest priority (`LB_MODE_LOAD`: nice 19, idle I/O class) after 2 s of warm-up. Each
  phase gives the frames shown per second, the time to build a frame (median and 99th percentile) and the share of
  intervals between shown frames longer than 1.5 refresh periods; the load phase also gives the work the load
  got done. The verdict compares the 99th percentile with the refresh period actually in use and warns above 5 %
  late frames (problems above 25 %). Without a monitor the compositor draws only while someone watches (the
  remote desktop), so a headless session gives "skipped: too few frames" unless a viewer is connected.
- **Pointer to screen** (in both phases of the desktop test): the scene's window is moved by a **synthetic pointer**
  (`bench/lbinput.c`), a thread of lord-server that plays a 125 Hz mouse: each event is stamped with the monotonic
  clock when it is made, goes into a ring and is announced on an eventfd the main loop polls like an input device;
  the main loop takes the newest position once per frame, as it does with a real mouse's motion, and moves the
  window through the drag's path. Each event's latency ends at the presentation of the first frame that shows it or
  a later position (`lbf_input`, `lbf_built`, `lbf_presented` in `bench/lbframes.c`); the results are the median and
  the 99th percentile (`comp.in50/in99`, `desk.in50/in99`), and the verdict warns when the 99th percentile passes
  three refresh periods. It measures lord-server's part (waking up, the frame, the flip), not the mouse, USB or the
  kernel's; the positions only move LordBench's own window. Without the thread the scene moves by the clock and the
  latency is "failed". On VM 116 without a monitor: 14.6 ms median and 25.2 ms p99 alone, 15.1 and 30.9 ms under
  load; on the Pi 5 (no monitor): about 15.7 and 27 ms alone, 22 and 52 ms under load ("The desktop under load").
- **Boot time test** (Performance → Boot time, administrators): N boots in a row (3, 5 or 10). The closed admin
  action `boot-test <n>` writes `/var/lib/lordos/boot-test` in a fixed format (`bench/boottest.c`) and the session
  restarts as usual (its confirmation included). At each boot's first frame lord-sessiond records the boot with its
  number in the test and, while boots are left, writes the file again and restarts 30 s later; the login screen
  shows the boot and the countdown with **Stop the test**, and a login there stops it too (`docs/SESSIONS.md`,
  "Boot time"). The test always ends: the count only goes down, a state older than 30 minutes is discarded, and
  lord-sessiond reads the file again right before restarting. The Boot time group shows the latest test's median
  and range per version.
- **Guards and conditions** (`bench/lbcond.c`, read by lord-server, never by the helper): the hottest
  thermal zone (or the CPU's coretemp/k10temp/zenpower hwmon sensor) and the lowest passive and
  critical trip points; the x86 thermal throttle counters; the Raspberry Pi firmware's `get_throttled`
  flags and its `rpi_volt` under-voltage alarm; cpufreq, governor and ACPI platform profile; batteries
  (a device's own battery, such as a mouse's, is ignored) and mains. A run is refused on battery below
  30 %, waits for 10 s in a row below 15 % CPU (at most 30 s, then it runs marked "busy"), stops within
  2 °C of the passive trip point (or 10 °C of the critical one when there is no passive), and in the peak
  tests stops when the throttle counters grow or the Pi firmware slowed the processor during the run (below);
  in the sustained test throttling is recorded with its time and temperature instead. The kind of disk under
  the scratch file is recorded (NVMe, SD/eMMC, SSD, HDD, USB, virtual, or RAM in the live session). Unit tests
  on fake sysfs trees (`tests/lordbench_test.c`): an x86 laptop on battery, a Raspberry Pi 5 with the kernel
  LordOS ships, a Pi known only from its device tree, and an older Pi kernel with `get_throttled`.
- **The Raspberry Pi firmware's throttling** (0.8.6 in development, 2026-10-28; `bench/lbcond.c`, not yet run
  on a real Pi). On a Pi the firmware itself lowers the ARM clock on under-voltage, at its temperature limit
  or when the clock is capped, and cpufreq may not show it. What the kernel tells an unprivileged process,
  checked in Raspberry Pi's `rpi-6.18.y` source (the branch `tools/rpi/build-kernel.sh` builds;
  `bcm2712_defconfig` has `CONFIG_SENSORS_RASPBERRYPI_HWMON=m`):
  - `drivers/firmware/raspberrypi.c` on that branch has **no** `get_throttled` file: it only asks the firmware
    for GET_THROTTLED once to decide whether to register the `raspberrypi-hwmon` device. (Older Raspberry Pi
    kernels had a `get_throttled` attribute under the firmware node; the 2026-10-27 run on the Pi 5 found
    none, as the source says.)
  - `drivers/hwmon/raspberrypi-hwmon.c` (hwmon `rpi_volt`) asks GET_THROTTLED every 2 s **with the sticky bits
    cleared each time** (request value `0xffff`) and exposes one file, `in0_lcrit_alarm` (mode 0444): bit 16,
    "under-voltage has occurred", of the last answer. So it means "under-voltage in the last 2 s window".
  - The frequency-capped, throttled and soft-limit bits are not exposed at all on this kernel; reading them
    would need the mailbox (`/dev/vcio`, `vcgencmd`), which LordBench never opens or runs.

  So lord-server reads, world-readable and without exec or root: `rpi_volt`'s `in0_lcrit_alarm` (exactly `0`
  or `1`, anything else is unknown); `get_throttled` where a kernel has it (directly under a
  `/sys/devices/platform` node or its `<node>:firmware` child; 1-8 hex digits with an optional `0x` and a
  newline, nothing else, `rpifw_parse_throttled`; masked to the eight known bits: 0 under-voltage now, 1 ARM
  frequency capped now, 2 throttled now, 3 soft temperature limit now, 16-19 the same "has occurred"); and
  whether the machine is a Pi at all (that hwmon, that file, or a `raspberrypi,` entry in the device tree's
  root `compatible`). On a Pi, the hottest zone at or above **80 °C** counts as the firmware's limit
  (`LB_RPI_HOT`): Raspberry Pi's documentation says the Arm cores are progressively throttled between 80 and
  85 °C, and the Pi 5's kernel thermal zone has no passive trip point that would show it (only the 110 °C
  critical one, so the thermal guard alone would wait until 100 °C).

  Every guard sample (once a second, also during the video jobs and the desktop run) goes through
  `lb_rpi_watch`: the "now" bits and `LB_RPI_HOT` always count; a sticky bit or the alarm already set when
  the run starts may predate the run, so it counts only once it has been seen clear and then set again (with
  the 2 s clearing this happens within one window; on a kernel that never clears them, an event from before
  the run is never blamed on it). In a peak test, frequency capped, throttled, soft limit or the 80 °C limit
  stops the run as `throttled`, like the x86 counters; in the sustained test the first one is recorded as
  `throttleAtS`/`throttleTempC`. Under-voltage is never a stop: it is recorded on its own. The result gets
  `piFirmware` on a Pi: `{flags, voltageAlarm, underVoltage, capped, throttled, softLimit, tempLimit,
  tempLimitC}`, where `flags`/`voltageAlarm` say what was readable, `underVoltage` is present only when one of
  them was, and `capped`/`throttled`/`softLimit` only with `flags` (unknown is left out, never `false`).
  `before`/`after` keep the raw `rpiThrottled` bits and add `rpiUnderVoltageAlarm`. System Information →
  Performance: the Stability verdict warns "The board reports throttling" (capped, throttled, soft limit),
  "It reached 80 °C, where the board's firmware starts slowing the processor down", and "The board reports low
  supply voltage" from `piFirmware` (older results keep using the end's bits), and Conditions gets a "Board
  firmware" row listing what happened, or what was watched when nothing did. Machines that are not a Pi get no
  `piFirmware` and behave as before. Not verified on a real Pi yet: that the alarm reads `1` during an actual
  under-voltage, and how fast the Pi 5 reaches 80 °C without a cooler in the peak tests.

  The reader is shared (`hw/rpifw.c`, `rpifw_read`: lord-server, `lordos-sysreport` and the tests link it), and
  System Information's Raspberry Pi section uses it too (2026-10-29; before, it looked only for `get_throttled`, so on
  the Pi 5 with the LordOS kernel its Power and Throttling rows were always empty). It shows what the kernel shows
  and leaves out the rest: with only `rpi_volt` (the LordOS kernel), a Power row for the last few seconds ("No
  under-voltage in the last few seconds" or "Under-voltage in the last few seconds: use the official power supply")
  and no Throttling row; with only `get_throttled` (an older kernel), Power and Throttling "now" and "since boot";
  with both, the sticky bits cover only the last 2 s window, so the rows say so; with neither, model and revision
  only. The report's `raspberryPi` has `throttled` (the raw bits or `null`) and `underVoltageAlarm` (`true`,
  `false` or `null`). Tested on fake sysfs trees (`tests/sysreport_test.c`: the LordOS kernel with the alarm clear,
  set, malformed and missing; older Pi 4 and Pi 5 kernels with `get_throttled`; both; neither; a machine that isn't
  a Pi), not yet on a real Pi.
- **Helper-sandbox self-test** (`sec.helper-sandbox`, repeated inside `lord-bench`, `sandbox_tries` in
  `bench/lbmain.c`): right after `sandbox_apply_helper` and before any work, in every mode, `lord-bench` tries six
  things that must fail: the Sandbox Probe's helper tries (`app/probe.c`) with its own versions of the data-folder
  one, plus an internet socket and a system file. A unix socket (`helperSocket`), an internet socket
  (`helperInet`), `execve` of `/usr/bin/true` (`helperExec`), `fork` (`helperFork`), a new unnamed file in `/tmp`
  (`helperNewFile`, `O_TMPFILE`: lord-bench has no writable folder) and reading `/etc/passwd` (`helperRead`). None
  could change anything if it worked: exec is called with an invalid environment pointer, so an exec the sandbox
  let through fails with `EFAULT` before anything is replaced (and counts as not refused); a socket or the unnamed
  file is closed at once; a forked child only exits. Each try is one fixed record (`LB_MSG_SANDBOX`: the try, refused
  or not, its errno; `lb_msg_valid` checks all three), the result lists them as `sandboxTries`, the Stability
  verdict fails a run where one got through or is missing and otherwise adds "refused all 6" without changing the
  state (results from before have none); the tries are named to the user in their language ("run a program", …).
  On a kernel without Landlock (no `SB_LANDLOCK_FS` in the sandbox mask) the two file tries are expected to work:
  that is the kernel's limit, already recorded in the mask, so they make the verdict warn ("Without Landlock in
  this kernel the benchmark helper could still: …"), not fail. `lordos-sysreport bench` checks the same six by
  name: "helper sandbox: N of 6 tries refused", a `NOT REFUSED: <id>` line for each one that worked or is
  missing (and "allowed without Landlock" for the file tries on such a kernel). On VM 116 all six were refused:
  `EPERM` from seccomp for the sockets, exec and fork, `EACCES` from Landlock for the two files.
- **The result** (`~/.local/state/lordos/lordbench.json`, the previous one kept as
  `lordbench-prev.json`; a refused run keeps the last result): LordBench's version, the mode, status and
  stop reason, LordOS and kernel versions, live or installed, threads, the quiet wait, conditions before
  and after, the disk kind, the GPU kind (hardware, virtual or software), the sandbox mask and the
  helper-sandbox self-test's tries, this
  session's composition and each active display's connector, mode, refresh, scale and rotation (no
  names, vendors or identities), every measurement with its unit, what was skipped and why, and the
  sustained samples. `lord.sysinfo.bench.boots()` groups lord-sessiond's boot history by version
  (median, range, the first boot after an update apart).
- **System Information → Performance** (`apps/sysinfo/performance.jsx`, model in `perf.js`, tested in
  `server/tests/sysinfo_perf_test.mjs`): Quick and Full, progress by measurement, then the three verdicts
  (performance as "×N" per category against each reference measured with the same LordBench version;
  compatibility counted from the Compatibility tab; stability from the status, failed measurements,
  throttling, under-voltage, a sustained drop above 10 %, a busy start and the helper-sandbox self-test),
  every measurement with its
  "×N" and its change against the previous run, the conditions that differ from the previous run, the
  conditions, the boot times per version, and **Save to Documents** (the JSON). Nothing is sent.
- **Reference machines** (`apps/sysinfo/references.js`): VM 116 and the Raspberry Pi 5 16 GB, each a Full run
  copied from the `lordbench.json` it wrote (numbers below). A measurement added after a machine's run is listed in
  its `notMeasured` until it is measured again: VM 116's layout, paint and shaping numbers come from a later Full
  run; the Pi 5 has none yet. The modest x86 PC is added when measured.

VM 116, Full with 30 s sustained, LordBench 1 (2026-10-27, 159 s): SHA-256 166 MB/s on one core and 984 MB/s on
eight; matrix product 3.3 and 19.9 GFLOP/s; xz 60 and 363 MB/s; QuickJS 57 and 336 runs/s; memory copy
20.0 GB/s, triad 22.8 GB/s, latency 91 ns; llvmpipe: scene 802 fps, fill 407 Mpixel/s, upload 9,457 MB/s,
8 layers 22.9 fps; virtio disk: 378 MB/s write, 517 MB/s read, 6,256 random reads/s; sustained 19.6 → 19.8
GFLOP/s; software video decoding H.264 1080p 399 fps, HEVC 1080p 385, VP9 1080p 215, AV1 1080p 201, H.264 4K
76, HEVC 4K 102; encoders H.264 106 fps and VP8 54 fps at 1080p. A Quick run took 33 s that day; later Quick runs from a terminal
took 39 s, and 46 s with the interface workloads (below).

VM 116, the interface workloads, Full without sustained or storage, LordBench 1 (2026-10-28, 142 s; its other
measurements were within 4 % of the run above): layout and paint 37.0 fps on one core (spread 0.2) and 39.9 fps
with the paint threads (spread 1.3); shaping 619 runs/s on one core and 3,595 runs/s on eight (5.8×). A frame of
the scene takes about 28 ms, almost all of it paint (the layout of the 529 nodes with every node invalidated takes
0.15 ms): llvmpipe is not involved, and only the big row-local loops (fills, shadow masks, blits of 512×512 pixels
or more) are split into bands, so the paint threads gain about 8 %. Three Quick runs from a terminal gave
35.8–36.2 fps, 40.3–40.8 fps, 625–629 runs/s and 3,565–3,596 runs/s; a Quick run from a terminal now takes 46 s
(39 s before). After the review fixes (the shaping threads' shared script table set before they start, a clean
paint pool for each run; ThreadSanitizer finds nothing in `lb_ui` and `lb_shape` repeated) a fourth Quick run gave
36.4 fps, 40.6 fps, 631 runs/s and 3,607 runs/s, in 46 s, with all six sandbox tries refused. The reference values
carry the LordOS version that run reported (`0.8.5-beta`, the version in `package.json` while 0.8.6 is in
development).

Raspberry Pi 5 16 GB, Full with 180 s sustained, LordBench 1 (2026-10-27, 339 s): SHA-256 933 MB/s on one core
and 3,693 MB/s on four (the CPU's SHA instructions); matrix product 2.26 and 5.99 GFLOP/s; xz 67 and 254 MB/s;
QuickJS 54 and 181 runs/s; memory copy 7.6 GB/s, triad 7.3 GB/s, latency 115 ns; V3D (hardware): scene 795 fps,
fill 2,455 Mpixel/s, upload 3,897 MB/s, 8 layers 115 fps; SD card: 66 MB/s write, 91 MB/s read, 2,692 random
reads/s; sustained 5.86 → 5.88 GFLOP/s over 3 minutes with the fan, 55.6 °C at most, no speed lost; software
video decoding H.264 1080p 258 fps, HEVC 1080p 246, VP9 1080p 204, AV1 1080p 159, H.264 4K 55, HEVC 4K 57;
encoders H.264 75 fps and VP8 55 fps. A Quick run took 34 s. On this kernel the firmware's `get_throttled` file and
a cpufreq cooling device are not exposed, so firmware throttling can't be seen during a run (the stop on
throttling does not fire on the Pi; the `rpi_volt` under-voltage alarm is read); the sustained curve and the
temperature are the evidence that it did not throttle. (Since then the alarm is watched during the whole run and
the firmware's documented 80 °C limit counts as throttling on a Pi: "The Raspberry Pi firmware's throttling" above.)

Desktop impact:

| Machine | Phase | Frames shown | Build p50 | Build p99 | Late |
|---|---|---|---|---|---|
| VM 116, headless, one remote viewer, llvmpipe | alone | 59.6 fps | 5.4 ms | 13.3 ms | — |
| | under load | — | — | 22.9 ms | 12.5 % |
| Pi 5, 1920×1080 60 Hz monitor, GPU composition | alone | 59.4 fps | 10.9 ms | 13.0 ms | 1.0 % |
| | under load | 24.0 fps | 20.1 ms | 29.3 ms | 96.5 % |

On the Pi the desktop stays at the refresh rate alone, but under an all-core load at the lowest priority (which
still got 5.0 GFLOP/s done) it fell to 24 frames per second: a stability problem LordBench found, investigated
below ("The desktop under load"). The first scene moved its window from JS
(`wm.move` every frame) and measured React instead of the compositor (12.8 fps on VM 116); it was replaced by the
drag's own path in C before these numbers. These rows were measured with the scene's square standing still (it
sent a style key the engine lacked); with it turning, see "The drag frame's cost" below.

#### The desktop under load (2026-10-28)

**Cause.** lord-sessiond puts the compositor in the session's `server` cgroup and every app and helper in a
scope next to it (`lordos-bench.scope` for LordBench's load). A process's nice value only counts against
processes in the same cgroup; between cgroups the kernel shares the CPU by `cpu.weight`, and every one of them had
the default, 100. So the load's nice 19 did nothing against the compositor, and any app keeping every core busy
would do the same. Measured from `/proc/<pid>/task/*/schedstat` during the test (Pi 5, the session without its
monitor, CPU composition): the compositor's main thread waited for a CPU 17 ms per second alone and **299 ms per
second** under the load (1.05 ms each time it woke up), and its three paint threads waited longer than they ran.

**Change.** lord-sessiond gives the `server` leaf `cpu.weight` 1000 when it creates a session
(`CG_SERVER_CPU_WEIGHT`, `sec/cgroup.c`); app scopes and helpers keep 100. Root writes it in the root-owned subtree
it already manages; no new request or interface. Checked on VM 116: a session started by the new lord-sessiond has
`cpu.weight` 1000 on `server` and 100 on its scopes.

**Measured** with the final LordBench of this version (the scene moved by the synthetic pointer, so the pointer
latency is measured too), under load, on the Pi 5 without its monitor (CPU composition), two runs each. The
installed session can't be changed without root, so the test ran in a second session laid out the same way: the
compositor and the load in sibling cgroups (user scopes), the weight set on the compositor's.

| Compositor's weight | Frames shown | Build p50 | Build p99 | Late | Pointer to screen p50 | p99 |
|---|---|---|---|---|---|---|
| 100 (before) | 43.3–45.3 fps | 19.6–19.8 ms | 39.6–45.9 ms | 30.7–33.0 % | 31.8–33.3 ms | 66.8–75.3 ms |
| 1000 | 56.4–56.9 fps | 12.4–12.6 ms | 28.6–33.4 ms | 7.3–8.3 % | 22.1–22.2 ms | 51.5–53.4 ms |

Alone nothing changes (59.6–60 fps, about 0.2 % late, pointer to screen 15.5–15.8 ms median). The load still got
2.7–3.4 GFLOP/s done. With the earlier scene (moved by the clock instead of the synthetic pointer) the same change
took the Pi from 61 % to 35 % late frames, and VM 116 (8 vCPU) from 8.6–17.5 % to 5.6–9.0 % (p99 16.6–24.6 →
16.4–17.5 ms). The weight alone was not measured with the monitor and GPU composition on the Pi, where the 24 fps above came
from; it was measured there together with the real-time priority (below).

**Tried and dropped.** A 0.5 ms time slice for the compositor's threads (EEVDF's custom slice, `sched_setattr`):
it looked like a gain at first, but the tool that set it from outside also gave it to the load; set only on the
compositor's threads, as lord-server would, it made no difference (8–10 % late with weight 1000, 28–33 % with 100).
And turning the paint threads off when the frame waits for them under load: their own measurement showed they still
saved time, so it never acted.

**Real-time priority (approved by the maintainer on 2026-10-28).** With the weight, the main thread under load
still waited about 0.9 ms each time it woke up: a thread of the load that has a CPU keeps it until the next
scheduler tick (4 ms on the LordOS Pi kernel, `CONFIG_HZ=250`). Now the compositor's main thread and its paint
threads run at `SCHED_RR` priority 2 (the lowest real-time levels: ahead of every normal thread, behind the kernel's
own), as KWin does for its compositor and Mutter for the thread that sends frames to the screen:

- lord-sessiond starts the session's lord-server with `RLIMIT_RTPRIO` 2 and `RLIMIT_RTTIME` 5 s (soft; `remote/sess.c`).
- lord-server takes it in `par_init` (`par.c`) right after dropping root, before it starts any process or other
  thread: the main thread takes `SCHED_RR`, the paint threads inherit it, then the main thread adds
  `SCHED_RESET_ON_FORK`, so every thread and process it starts later (apps, helpers, Mesa's threads, LordBench's
  load) gets the normal policy. Then it lowers `RLIMIT_RTPRIO` to 0, soft and hard: nothing it starts can ask for
  real time, and it can't either. The root admin helper, forked before, lowers its own to 0 too (its download child
  drops to `nobody`).
- A thread that runs 5 s of CPU on real time without blocking (a bug) gets `SIGXCPU`, and every thread goes back to
  the normal policy for the rest of the session; nothing is killed. The session log says which happened
  (`compositor at real-time priority 2 (SCHED_RR, N threads)`).
- Without lord-sessiond, or on a kernel that refuses it (`CONFIG_RT_GROUP_SCHED`), nothing changes.

Checked on VM 116 with the new lord-sessiond: the main thread is `SCHED_RR|SCHED_RESET_ON_FORK`, the 7 paint threads
`SCHED_RR`, the other 22 threads and every child (`lord-admin`, `lord-vault`, `lord-maild`, `lord-doc`, the
terminal's shell, `lord-bench`) `SCHED_OTHER` with a real-time limit of 0. Measured on VM 116 (8 vCPU, no monitor,
CPU composition) under load, the session at weight 1000 laid out as lord-sessiond does, with the same limits; two
runs each:

| VM 116, under load | Build p99 | Late | Pointer to screen p50 | p99 | Load's CPU |
|---|---|---|---|---|---|
| Weight 1000 | 16.3–16.8 ms | 2.65–2.92 % | 14.8–15.1 ms | 31.0–32.3 ms | 91.3–91.6 s |
| + real-time priority | 11.0–12.7 ms | 0.28 % | 13.3–14.3 ms | 24.7–25.5 ms | 90.4–91.0 s |

Under load the desktop now measures as it does alone (pointer p99 about 25 ms alone), and the load got the same CPU
time.

**On the Pi 5 with its monitor (GPU composition, 1920×1080 at 60 Hz)**, the installed session with both changes
(package 0.8.6-dev3, logged in automatically), two runs: the session log says `compositor at real-time priority 2
(SCHED_RR, 4 threads)`, the `server` leaf has weight 1000, and its children run `SCHED_OTHER` with a limit of 0.

| Pi 5, monitor, under load | Frames shown | Build p50 | Build p99 | Late | Pointer to screen p50 | p99 |
|---|---|---|---|---|---|---|
| 0.8.5 (2026-10-27) | 24.0 fps | 20.1 ms | 29.3 ms | 96.5 % | — | — |
| weight + real-time priority | 28.9–30.5 fps | 15.4–16.2 ms | 18.9–19.2 ms | 94.3–98.3 % | 49.0–51.4 ms | 75.6–94.0 ms |

Alone, in the same runs: 59.0–59.2 fps, build 6.9–7.1 ms (p50), 1.4–1.7 % late, pointer to screen 25.4–25.6 ms
(p50). The scheduling part is solved: sampled during the run, the compositor's main thread waited about 0.07 ms per
second for a CPU (299 ms per second before the weight). What remains is the frame itself: under the load it takes
15–16 ms of the main thread's own CPU time to build instead of 7 ms, so it misses the 16.7 ms refresh and the
monitor shows every other one (about 30 fps). The cause is not found yet; the load keeps four cores busy with
matrix products, and the CPU and the GPU share the Pi's memory, but that has not been measured.

**What remains.** Under load a frame took about twice as long to build on the Pi (15–16 ms instead of 7 ms), which
no priority fixes. The frame-cost work below cuts what a drag frame paints (build p50 under load about 20 → 12.9 ms
with the turning square), but the build, the GPU's part and the flip still run one after another and miss the
16.7 ms refresh under the load, so the rate stays at 30 fps; the frame pipeline below is built for that and not yet
measured. The Pi kernel at `CONFIG_HZ=1000` (`tools/rpi/build-kernel.sh`, a rebuild) is not built.

#### The drag frame's cost (0.8.6, in development)

The scene's square turns since this work (the `rotate` key above), so the layer of the moved window changes every
frame and the test is heavier than the one measured before; the "before" rows below are the code before this work
with the square turning. What changed in a drag frame, all with the same pixels except an approved 1-level change
at rounded corners below the window (`docs/WINDOWS.md`, "The moving window is a layer" and "Checking it"): only the
changed part of the window's layer is painted again; what is below the window is painted again only where it
changed and is copied where the old rule painted everything; nodes the frame skips are no longer walked for their
rounded corners; the GPU path's hole walk is skipped for a window without app windows; a turn or scale no longer
lays its node out again; the JS timers no longer look at every pending timer; React sends fewer unchanged props;
`lord.stats()` reads QuickJS's memory in O(1). On the GPU path the dragged window can also be composed by the GPU
with the CPU's exact arithmetic ("GPU drag", `docs/GPU.md`, "The dragged window on the GPU").

**Measured on the Pi 5 with its monitor** (1920×1080 at 60 Hz, GPU composition, the installed session with the
weight and the real-time priority, LordBench → Desktop impact; 2026-10-28):

| Pi 5, monitor | Runs | Alone: frames shown | Build p99 | Late | Under load: frames shown | Build p50 | Late |
|---|---|---|---|---|---|---|---|
| Before (square turning) | 3 | 53.2–58.1 fps | 15.7–16.5 ms | 3.1–12.8 % | 23.6–25.1 fps | 19.8–20.3 ms | 100 % |
| Frame-cost work, `LORD_GPU_DRAG=0` | 2 | 59.8–60.0 fps | 11.3–11.6 ms | 0–0.3 % | 30.1–30.3 fps | 12.9 ms | 98.1–99.2 % |
| Frame-cost work, GPU drag on | 3 | 52.2–52.9 fps | 21.3–21.7 ms | 13.5–15.0 % | 25.8–27.5 fps | 13.4–14.2 ms | 91.5–99.4 % |

With GPU drag off the desktop is back at the refresh rate alone, and under the load the build fell from about
20 ms to 12.9 ms but every other frame is still late (30 fps): the chain of build, GPU and flip is still longer
than one period. With GPU drag on, that build stalled (52 fps alone, p99 about 21.5 ms): it ran before GPU drag's
textures caught up in slices between frames (`docs/GPU.md`, "Uploads without a stall") and before the frame phases
below existed, so the cause was not measured. <!-- PI-RESULT: GPU drag on the Pi 5 with the final build: its
self-test line from session.log (on/off, pixels, ms), LORD_GPU_VERIFY=1 differ= results, and a row "Frame-cost work,
GPU drag on (slices)" with alone fps / p99 / late and under-load fps / p50 / late; then say whether it still stalls -->
GPU drag's default is decided after that run: today it is on wherever its self-test passes, and
`LORD_GPU_DRAG=0` turns it off.

**The frame pipeline (in development, not yet measured on the Pi).** A frame that takes 13–16 ms to build cannot
fit with the GPU's part in a 16.7 ms period while the next frame is started only after the flip. Now, while a flip
is pending, the next frame's CPU part (input, JS frame callbacks, layout, paint into the scene) is done ahead, and at
the flip only its upload, the GPU's draw and its flip are left (`docs/GPU.md`, "The frame pipeline"). On by
default while frames are heavy (more than 60 % of the period, 2 frames in a row; off after 60 frames below 40 %),
`LORD_PIPELINE=0` turns it off and `LORD_PIPELINE=2` keeps it on. Only with one display that has real page flips:
without a monitor, with a driver paced in software or with several displays nothing changes. Expected on the Pi
under the load: the frames shown every vblank instead of every other one as long as the CPU part stays under one
period, with about the same pointer latency; that is a model (`build/fpipe-test` drives a display on a vblank
grid: 13 ms of CPU, 1 ms to submit and 3 ms of GPU give 299 flips in 600 periods without it and 598 with it, input
to screen at most 33.3 and 36.0 ms), not a measurement. <!-- PI-RESULT: frame pipeline on the Pi 5 with its
monitor: a table "Pi 5, monitor, pipeline" with LORD_PIPELINE unset vs LORD_PIPELINE=0, two runs each: alone and
under-load frames shown, build p50/p99, late %, pointer to screen p50/p99, and frames.phases.load prepared /
heldMs / onTime / cpuLate / gpuLate --> To measure: LordBench → Desktop impact twice with `LORD_PIPELINE` unset and
twice with `LORD_PIPELINE=0` in the session's environment, comparing `desk.fps`, `desk.missed`, `desk.in50`/`in99`
and `frames.phases.load` (`prepared`, `heldMs`, `onTime`).

#### Frame phases (0.8.6, in development)

Without the frame pipeline a frame is started only after the last flip completed (one frame in flight), so any
frame whose whole chain (the work before `render()`, the build, the GPU's own part, the flip) takes more than one
period is shown a vblank late, and the rate locks at half the refresh. The build time alone (`desk.p50`/`p99`) can't
tell which part was too long. So lord-server also records, for every frame the main display shows, while LordBench
records or while a scripted move runs (`bench/lbframes.c`: `lbf_phase_built` when the frame is submitted, at the
end of `render()` or, for a frame the pipeline prepared, right after its submission; `lbf_phase_shown` at that
frame's flip; all of it measured by lord-server itself, never by a helper):

| Phase (JSON key, `[p50, p99]` ms) | From → to |
|---|---|
| `wakeMs` | the flip's kernel timestamp → `poll()` returned with it (frames started by a flip) |
| `beforeRenderMs` | that wake → `render()`: pointer motion, LordBench's scripted step, the JS timers |
| `renderMs` | `render()` itself (the same as the build time) |
| `paintMs` | its layout and paint |
| `uploadMs`, `gpuDragUploadMs` | the UI texture's upload; GPU drag's texture uploads in a GPU drag frame (`docs/GPU.md`) |
| `swapMs`, `flipIoctlMs` | `eglSwapBuffers`; the front buffer, its framebuffer and the page flip ioctl |
| `gpuMs` | the flip queued → the GPU finished the frame (below) |
| `vblankWaitMs` | the later of those two → the flip |
| `wakeToFlipMs` | the wake → the flip |
| `catchUpMs` | GPU drag's textures catching up after the flip was queued (`docs/GPU.md`, "Uploads without a stall") |
| `heldMs` | a prepared frame (the frame pipeline, `docs/GPU.md`): its paint done → its submission at the flip before it |

A frame the pipeline prepared (counted in `prepared`) is painted while the previous flip is pending and submitted at
that flip: its `wakeMs` runs from that flip to the wake that submitted it, `beforeRenderMs` is the input and JS
before its prepare, `paintMs` its prepare's, `uploadMs`, `swapMs` and `flipIoctlMs` its submission's, `renderMs` the
two together (its CPU time, not the time between them, which is `heldMs`), and `wakeToFlipMs` runs from its
prepare's wake, so it spans two flips. The synthetic pointer's events applied before its paint end at its own
presentation.

The GPU's finish time comes from a native fence taken right after `eglSwapBuffers`
(`EGL_ANDROID_native_fence_sync`, `gpu.c`) and read once at the frame's flip with `SYNC_IOC_FILE_INFO`: the
kernel's signal time of the sync file, one ioctl a frame, no wait. Fences are taken only while recording. Without
the extension `gpuKnown` stays 0. Each frame is also put in one class against the first vblank after it started
(the vblanks fall on the previous flip plus whole periods; for a prepared frame, after the wake that submitted it,
since its paint was done before):

- `onTime`: shown within half a period of that vblank;
- `cpuLate`: late, and its flip was queued after that vblank;
- `gpuLate`: queued before it, but the GPU finished after it;
- `otherLate`: late with both before it, or with the GPU time unknown.

And counted per frame: `paintThreads` (the pool was on), `unshown` (`gpu_frame_ex` returned -2), `uploadMpx`,
`gpuDragUploadMpx` and `catchUpMpx` (pixels per frame, average), and `gpuDrag`: `tried`, `taken`, `refused` by reason
(`layer`, `clip`, `ancestor`, `app`, `after`, `post`, `feed`), `layer` refusals by cause (`invalid`, `rendering`,
`holes`, `surfaces`, `premul`, `opacity`, `part`…), `wholeUploads` and `wholeMpx`. The result's `frames` object has
`phases.alone` and `phases.load`; the session log gets `lordbench: desktop alone: phases, N frames (ms p50/p99): …`
and `… under load: …`, and the `drag (scripted): …` line ends with the same text for the whole move. Without a
monitor a frame is "shown" when it ends (on the pacing grid), so `gpuMs` and the wait are 0. System Information does
not show these yet; results keep comparing as before (no new result ids, LordBench 1).

**Checked** on VM 116: `lordbench-test` feeds known frames (one on time, one queued after its vblank, one whose GPU
finished after it, one not shown, a prepared one) and checks each class, percentile, count and the text. A desktop
run of the branch's lord-server in an isolated headless session (a test theme moving one window with
`lord.wm.script` over 40 cards, CPU composition, one viewer on its `remote.sock`) wrote both objects: alone 601
frames, all on time, render 0.84/1.78 ms, paint 0.55/1.21 ms; under load 720–721 frames, 715–721 on time (three
runs). Those are a check that it works, not a measurement (a small scene, no GPU). The GPU fields were not
exercised: VM 116 sessions are headless, so the fence, the uploads' times and GPU drag's counts have been built,
not run. <!-- PI-RESULT: the Pi 5 run's frames.phases.load (p50/p99 of paintMs, uploadMs, gpuMs, vblankWaitMs and the
onTime / cpuLate / gpuLate counts), with and without GPU drag and the pipeline, as the first measurement of the phases
on a GPU -->

**What to run on the Pi 5 next** (the package from this work installed, the monitor on, logged in):
1. LordBench → Desktop impact, twice with the defaults (GPU drag where its self-test passes, the pipeline while frames
   are heavy), twice with `LORD_GPU_DRAG=0`, and twice with `LORD_PIPELINE=0`, in the session's environment.
2. From `~/.cache/lordos/session.log`: the `gpu: GPU drag on|off` line, the `lord-server: frame pipeline …` line, the
   `lordbench: desktop … phases` lines and the `drag (scripted):` line, and from
   `~/.local/state/lordos/lordbench-desktop.json` `frames.phases`.
3. Read under load: if most frames are `cpuLate`, the build is too long (`paintMs`, `uploadMs`, `beforeRenderMs`
   say which part, and `paintThreads` whether the pool helped); if `gpuLate`, the GPU's own work (`gpuMs`) is; if
   `onTime` is high and the rate still 30, the classification is wrong and the flip times must be looked at.
   With GPU drag, `gpuDrag.refused`/`gpuDrag.layer` say what keeps it off and `catchUpMpx` what the slices cost;
   with the pipeline, `prepared` and `heldMs` say how many frames were painted ahead and how long they waited.
4. `LORD_GPU_VERIFY=1` once with GPU drag over a Desktop impact run: `gpu-verify: … differ=0` on every line.
5. A real screenshot of a dragged window on the monitor (its rounded corners, `docs/WINDOWS.md`).

Boot: the Pi 5 reaches the login screen on its monitor 5.99 s after the kernel starts (lord-sessiond's record;
systemd-analyze gives 2.1 s kernel + 4.2 s userspace). Boots where the monitor was connected late took 61 and
103 s, which is the wait for the monitor, not the boot. The N-boot test was checked by its unit tests
(`tests/lordbench_test.c`: the state file's format, parsing, ownership, staleness and countdown) only;
the measured Pi number is a single boot.

The same Full run from System Information in VM 116's isolated headless session (3 minutes of sustained load;
its home is on tmpfs, so the storage test measured memory and says so, ×24 the reference's virtio disk; this
screenshot predates the video and desktop parts):

![System Information → Performance](sysinfo-performance.png)

**`lordos-sysreport bench [--quick | --full] [--json]`** (`sysreport/cli.c`): LordBench from a terminal, over SSH or
in CI, through lord-server's own code (`bench/lbrun.c`, the same guards, helpers and result file as the app, which
then shows it). It is installed as `/usr/lib/lordos/bin/lordos-sysreport` with `/usr/bin/lordos-sysreport` and
`lordos-fetch` as links, because `lbrun` only starts the `lord-bench` and `lord-media` next to its own binary
(`/proc/self/exe`; a group- or world-writable helper is refused, so a build tree made with umask 002 needs `chmod
g-w`). Without a session the helpers get no cgroup scope, and the desktop test needs a session. One line per
measurement (or the JSON with `--json`); exit 0 when the run finished, 1 when it didn't, and 3 when it finished
but the helper-sandbox self-test let a try through or one is missing (text output only; with `--json` the exit
code is 0 or 1 and the tries are in `sandboxTries`). On VM 116 a quick run took 46 s.

Not built yet: hardware video decoding (no test machine has VA-API, and the Pi 5's HEVC decoder needs FFmpeg's
`v4l2request`); detecting the Pi's firmware throttling during a run on kernels that don't expose it; measuring the
Pi 5 with the layout, paint and shaping workloads (its reference lists them as not measured); and the x86 PC
reference.

## 0.8.7 — Community hardware reports

- **Share on GitHub** saves the report as a small HTML page and opens it with the default browser
  (the existing `lord.apps.launchWith('', path)` rule). That page opens GitHub's new-issue form with
  the `hardware-report` template, title and summary filled in; the user attaches the JSON and
  submits it with their own account. LordOS sends nothing by itself and holds no token.
- A workflow validates each attached JSON against the schema, labels the issue by vendor and kind,
  and flags reports whose numbers are implausible for the hardware.
- **lordos.org/hardware**: the catalogue built from accepted reports: GPUs and drivers tested,
  boards, laptops with suspend, battery and Wi-Fi, known problems and the version that fixed them,
  real LordOS performance per device. The app's Compatibility panel reads a signed snapshot of it
  (downloaded with the package engine's checks), so it works offline.

## From the live USB, without installing

The app and LordBench are in the ISO, so anyone can start a PC from the LordOS USB stick, check
the machine and send a report without installing anything or touching its disks. This is likely
the main source of PC reports: it costs the tester nothing and answers "will LordOS work here"
before installing.

- **A boot menu entry**, *Check this computer*, starts the live session straight into System
  Information (0.8.5) and its Performance tab (0.8.6). The normal live session has it in the Dock.
- **The report says it was live**: boot medium (USB, its link speed), the overlay in RAM, no
  persistence. Live and installed results are kept apart in comparisons and in the catalogue.
- **Different references where live changes the numbers**: idle memory is compared with the
  paper's live figure (465 MB) and the overlay's size is subtracted and shown; boot time includes
  reading from the stick and is reported, not judged.
- **Nothing written to the computer's disks.** Internal disks are detected and their SMART health
  read; the storage test only runs on the live overlay in RAM and, read-only, on the USB stick, and
  says so. Measuring the internal disks waits for an installed system.
- **Where the report goes:** with no persistent home, the app saves it to a removable drive the user
  picks (a second stick, or a writable partition on the same one if present) or shares it directly
  through the browser when there is a network connection.
- **Missing firmware and drivers** are especially valuable here: the report lists what the ISO
  lacked for this machine (Wi-Fi, GPU firmware), which tells us what the ISO should carry.
- **The installer reuses the same checks** (`server/src/sysreport/`): before installing it shows
  the Compatibility summary (for example "GPU: not used, software renderer" or "Wi-Fi: firmware
  missing") so the user knows before replacing their system.
- **Limit:** machines with Secure Boot on can't start the stick until LordOS is signed for it
  (paper §10.2); the website's download page says so next to the *Check this computer* guide.

## New interfaces, all under the rules of `AGENTS.md`

- `lord.sysinfo` in `server/src/ui/js_hw.c`: `report()`, `markdown()`, `facts()`, `probe()` (built in
  0.8.5; the checks are evaluated in `apps/sysinfo/checks.js`); in 0.8.6
  `bench.start(kind)`, `bench.poll()`, `bench.cancel()`.
- `lordos-sysreport`, a user command (not started by `lord-server`).
- One read-only admin action, `hw-memory-modules` (anyone, no arguments): SMBIOS memory device
  entries parsed strictly in C, returning slot, size, type, speed and manufacturer only.
- The probe app for the effective checks: a signed system package started through the existing
  `lord.apps.launchPackage(id)`; no new launcher path.
- `lord-bench` (0.8.6), as above.
- Data packages `pci.ids` and `usb.ids` (Debian, architecture `all`), documented in `VM-SETUP.md`.

## Work plan for 0.8.5

**Built (2026-10-26):** `server/src/sysreport/` (the collector, `ids.c` for `pci.ids`/`usb.ids`, the JSON
and Markdown output), `lordos-sysreport report [--json | --markdown]` in `/usr/bin` (package `lordos`,
which recommends `pci.ids` and `usb.ids`), and `tests/sysreport_test.c`: five fake machines (AMD desktop,
Intel laptop, Nvidia desktop, Raspberry Pi 5, VM) full of serials, UUIDs, MAC addresses, a host name, a
user name, monitor serials and a MAC inside an interface name, none of which may reach a report
(`build/sysreport-test fixture <dir> <kind>` writes one for `--root`). Checked on VM 116 against the real
machine. `lordos-fetch` (the same binary under that name; `lordos-sysreport fetch`) shows the crown in
the brand colors beside a short summary (system, computer, kernel, uptime, packages, shell, display, CPU,
GPU, memory, disk, isolation); the user and host names appear only on screen and `--share` leaves them out
for a screenshot to post; `--color=auto|always|never`, `NO_COLOR` respected, true color when `COLORTERM`
says so, plain ASCII without color. **The app (2026-10-26):** `apps/sysinfo` (also About LordOS → System Report…) with Overview, Hardware and
Compatibility, from `lord.sysinfo.report()` (`ui/js_hw.c`): the same report plus `session`, how this
session composes the screen now (`lord_composition()` in `main.c`: `gpu`, `headless`, `no-display`,
`rotated`, `capture` or `gpu-off`) and the GPU context (`gpu_info()`: the last `gpu_init` outcome as a
fixed code, the renderer, render node, dmabuf import and direct scanouts since start). The layers and
the Compatibility panel are worked out in `apps/sysinfo/model.js` (`server/tests/sysinfo_model_test.mjs`).
Checked on VM 116 in an isolated headless session (own home, runtime folder and bus):

![Overview](sysinfo-overview.png)

![Graphics as separate layers](sysinfo-graphics.png)

![Compatibility](sysinfo-compatibility.png)

**Report tab (2026-10-26):** the exact text that would be saved, as Markdown (`lord.sysinfo.markdown()`:
`lordos-sysreport`'s text plus a "This session" section) or JSON (the report as `lord.sysinfo.report()`
gives it); **Copy** and **Save to Documents** (`lordos-report-<date>.md|json`, `-2`, `-3`… never over an
existing file), with **Show in Files**. Nothing is sent anywhere: publishing comes with 0.8.7.

![The Report tab](sysinfo-report.png)

**Memory modules (2026-10-26):** Hardware → Memory → **Read modules** runs the read-only admin action
`hw-memory-modules` (the SMBIOS type 17 entries only root can read, parsed in `sysreport/smbios.c`:
slot, bank, size, type, speeds, manufacturer; never serial, asset tag or part number). On VM 116 the
parser read `mem 4096|RAM|0|0|DIMM 0||QEMU` from QEMU's real tables (`build/sysreport-test smbios` as
root); the button itself was not exercised on screen (the isolated test session has no admin helper,
so it shows disabled):

![Hardware → Memory](sysinfo-memory.png)

**Checks, part 1 (2026-10-26):** what `lord-server` can see itself, evaluated in `apps/sysinfo/checks.js`
(the thresholds are constants there, so they are signed with the theme bundle instead of a separate
`checks.json`), from `lord.sysinfo.facts()` (`sysreport/facts.c`: lord-server's uid/euid from
`/proc/self/status`, its cgroup and who owns it, the app scopes beside it, and whether anything
listens on port 5673 in `/proc/net/tcp{,6}`): display server without root (§2.2), Landlock ABI ≥ 6
(§6.4), `lord-netperm` running with cgroup BPF and the BPF LSM (§6.6), verified system code
(effective: every bundle loaded this session matched; §6.9), remote desktop closed when off
(effective: nothing listens on its port; §11.4), the session's cgroup tree owned by root (only inside
`…/lordos-session-<uid>/server`; §11.4), composition on the GPU (§7.3) and lord-server's memory now
(measured, no pass or fail; §7.1). On VM 116 it found two real things: `lord-netperm` isn't installed
there (the unit doesn't exist), and the first run refused a stale `app-host.bundle.js` in the test
tree. Part 2 is below.

![Checks, part 1](sysinfo-checks.png)

**Checks, part 2: the Sandbox Probe (2026-10-26, approved by the maintainer).** A system package,
`sysprobe` (`apps/sysprobe`, built into `assets/sysapps/sysprobe` like Passwords, hidden from the launcher
with the new manifest field `"hidden": true`), started with the existing `lord.apps.launchPackage(id)`
like any isolated app: no new argument, path or exec. In app mode (`app/appclient.c`), for that system
package only, `app/probe.c` runs instead of the app's code:

1. **Before the app's sandbox**, while the process is single-threaded and has loaded nothing, it forks a
   child that takes the decoding helpers' profile (`sandbox_apply_helper`, as `lord-doc` and `lord-media`)
   and tries a unix socket, exec, fork and the app's data folder.
2. It applies **the same sandbox as any isolated app** (same read-only folders, no network) and tries
   what must fail: the home folder, `/etc/passwd`, the root cgroup's `cgroup.procs`, an internet and a
   unix socket, `ptrace`, a user namespace, `mount`, `exec`. Its own data folder must work (the control:
   a probe that can't write there reports nothing it can be trusted for).
3. It asks its broker for what must be refused: a path outside its folder, the clipboard without
   permission, the FIDO2 device, `lord-maild`.
4. It sends the result as `BOP_PROBE` and exits, without a window. `brokersrv.c` takes `BOP_PROBE` only
   from that system package, validates the JSON (≤ 8 KB) and adds what only it can read: the process's
   `NoNewPrivs`, `Seccomp` and `CapEff` and its cgroup with `memory.max`. `lord.sysinfo.probe()` returns
   the last result; `apps/sysinfo/checks.js` (`evaluateProbe`) turns it into four rows: the app sandbox
   (§6.4), the helper sandbox (§6.7), the broker (§6.5) and the app's own capped scope (§6.4).

On VM 116, in the isolated headless session, every try was refused (9 + 4 + 4) and the process reported
no_new_privs, seccomp mode 2 and no capabilities; the scope row is a warning there because that session
isn't in a LordOS session tree (lord-sessiond refused the scope, as it must). Its first run also caught a
mistake in the probe itself: stacking the helpers' profile inside the app's sandbox is refused by the
app's seccomp (rightly), so the helper child now starts before it.

![The Sandbox Probe](sysinfo-probe.png)

Not yet: a connection refused by the per-app firewall (it needs an app with network access and a
temporary rule), video decode and encode and Vulkan (shown as *Not checked*).

1. `server/src/sysreport/`: collector, GPU layers, `pci.ids`/EDID readers, fake-sysfs fixtures
   (AMD desktop, Intel laptop, Nvidia desktop, Raspberry Pi 5, VM), the anonymization test,
   `hw-memory-modules`, `lordos-sysreport report`.
2. The app: Overview, Hardware, Compatibility; About's button; en, es, pt; right to left.
3. Checks, configured and effective, the probe app, thresholds signed in `apps/sysinfo/checks.js` (built); `lordos-sysreport checks` (not built yet).
4. Reports with preview; docs; screenshots on VMs; the first real reports: the Raspberry Pi 5 with a
   monitor and the maintainer's PC (Core i9-10900, Radeon RX 580), the first real AMD data in the
   project.
