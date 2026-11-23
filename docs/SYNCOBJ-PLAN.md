# Explicit sync for Linux apps (`linux-drm-syncobj-v1`): the plan

**Status: approved by the maintainer and built on 2026-11-22 (branch `feat/wl-syncobj`), released in 0.8.9.5.** The design
below is the one that was approved; "As built" right after it says where the code differs and why. Clients that don't
bind the global keep implicit sync (dma-resv), as `docs/GPU.md` says. Code: `server/src/wl/syncobj.h` (the pure rules),
`server/src/wl/syncobj.c` (the kernel side), the protocol and the commit queue in `server/src/wl/wl.c`.

## As built (2026-11-22)

- **Where it is offered:** whenever lord-server can open a render node with `DRM_CAP_SYNCOBJ_TIMELINE` and
  `DRM_IOCTL_SYNCOBJ_EVENTFD` (the GPU context's node first, else the first one that has them; syncobjs belong to no
  device, so a timeline made on any GPU imports on any node). That is on the GPU path **and on the CPU path**: a commit
  is applied only once its acquire point is *signalled*, so the CPU copy (`map_linear`) always reads a finished frame.
  Without such a node (no GPU driver, a kernel before 6.6) the global is not created. `LORD_SYNCOBJ=0` also turns it off.
  The node is opened once, by lord-server itself (`O_RDWR`, render ioctls only, no master), and kept for the session: a
  GPU context re-created (`GPU.md`) never invalidates a client's imported timelines.
- **Waiting (differs from "Never blocking the loop" 1 and 4):** one rule for both paths. At `wl_surface.commit` the
  acquire point is checked without blocking (`SYNCOBJ_TIMELINE_WAIT` with a deadline of 0 and `WAIT_FOR_SUBMIT`); if it
  is signalled the commit is applied at once (the usual case), otherwise its whole state goes into the surface's queue
  and an eventfd (`SYNCOBJ_EVENTFD`, flags 0: *signalled*, not only available) is polled by the Wayland event loop.
  The GPU never waits in its queue (`eglWaitSyncKHR` is not used): a late client can't delay the frame for everybody,
  which is why mutter, KWin and wlroots also wait on the CPU side. A commit behind a waiting one waits too, even with no
  point (order is kept).
- **Releasing (differs from the table's last two rows):** a release point is signalled when lord-server stops using the
  buffer, at the moment it sends `wl_buffer.release` today: at once after a CPU copy (or for a buffer it refused); on
  the GPU path after the frame that replaced the buffer is done (`wl_frame_done`), or when its direct scanout ends. The
  buffer's own fences go with it: `DMA_BUF_IOCTL_EXPORT_SYNC_FILE` (`DMA_BUF_SYNC_WRITE`: every reader's fence) into a
  temporary syncobj (`FD_TO_HANDLE` with `IMPORT_SYNC_FILE`), `SYNCOBJ_TRANSFER` onto the release point, so the GPU's
  last read is waited for in the kernel; if that export fails, `SYNCOBJ_TIMELINE_SIGNAL`. No EGL native fence is
  needed. A frame that is never shown (its surface destroyed while it waits) has its release point signalled too.
- **Limits:** as in the table, except **8** commits waiting per surface instead of 4: a Vulkan swapchain in mailbox
  mode has up to 5 images and all of them can be committed before the first one's rendering ends, and a state-only
  commit between them waits in the same queue. Points set and then dropped with the object (`destroy` before a commit)
  are forgotten without being signalled, as the protocol allows.
- **The session's waits (differs from the table; security review 2026-11-22):** the plan disconnected *the client that
  asked* once the session's eventfds were taken, so one app filling them with points it never signals would get other,
  innocent apps disconnected. Now only a client past its own share is ever disconnected: past 32 eventfds on its own
  connection, or, with the session full, when its process (every connection with the same pid) already holds 32. Any
  other client whose commit waits while the session is full is never refused: its first waiting commit is checked
  every 4 ms (`SOBJ_POLL_MS`, a non-blocking `SYNCOBJ_TIMELINE_WAIT`) instead of with an eventfd. Those polling waits
  count in the client's and the process's waits like eventfds (so the same 32 per connection and per process), and the
  session has at most 64 of them (`SOBJ_MAX_POLLS`), all checked by **one shared timer** in one bounded batch per tick
  (second security review): past 64, a commit is applied as it is (implicit sync, like an eventfd the kernel refuses),
  never a disconnection. The
  session limit is **128** eventfds (not 256), below the compositor's 256 surfaces, so other apps can still make
  windows. `syncobj.h` `sobj_wait_mode`, `tests/syncobj_client.c` `fair`.
- **Subsurfaces:** lord-server applies every subsurface's commit on its own (`set_sync` is not implemented), so each
  surface's queue is its own; nothing waits for a parent.
- **Security contexts:** offered inside them too (the plan's default; it reads nothing of other clients).

## Why

Today a Linux app's GPU buffer (a dmabuf) is shown when the app commits it, and the GPU's own implicit fences (the
dma-resv of the buffer) make the compositor's sampling wait for the app's rendering. Explicit sync replaces that with
timeline points the app and the compositor hand each other:

- **NVIDIA's own driver** has no reliable implicit sync; GNOME, KDE and wlroots added `linux-drm-syncobj-v1` so that
  Xwayland 24.1+ and Vulkan/EGL apps don't flicker or show half-drawn frames on it. (The NVIDIA driver is not supported
  by LordOS, `HARDWARE-REPORT.md`; this would be the first step if it ever is.)
- **Mesa** (Vulkan WSI from 24.1, EGL soon after) uses it when the compositor offers it: frames are no longer tied to
  the buffer's implicit fences, and a game's frame can be shown as soon as it is ready instead of whenever the kernel
  attaches the fence.
- **Xwayland 24.1+** uses it for its own GL/Vulkan clients (glamor) when offered.

## The protocol, as lord-server would speak it

- `wp_linux_drm_syncobj_manager_v1` (version 1): `import_timeline(fd)` → `wp_linux_drm_syncobj_timeline_v1`;
  `get_surface(wl_surface)` → `wp_linux_drm_syncobj_surface_v1` (one per surface: `surface_exists`).
- `wp_linux_drm_syncobj_surface_v1`: `set_acquire_point(timeline, hi, lo)` and `set_release_point(timeline, hi, lo)`,
  double-buffered with the surface's commit. A commit with a buffer must have both (`no_acquire_point`,
  `no_release_point`), a commit without one must have neither (`no_buffer`), the buffer must be a dmabuf
  (`unsupported_buffer`, shared memory keeps the copy it has today), and the acquire point must be lower than the
  release point on the same timeline (`conflicting_points`).
- Offered only when the DRM device says `DRM_CAP_SYNCOBJ_TIMELINE` (amdgpu, i915/xe, nouveau, radeon, v3d, virtio-gpu
  with recent kernels). Not offered on the CPU path without a GPU context (no device to import into), nor to a client
  in a security context unless the maintainer decides otherwise (it reads nothing of other clients, so it could be
  offered; the default here is to offer it, like linux-dmabuf).

## The DRM ioctls, all on lord-server's own DRM fd

lord-server already holds a DRM fd (the session's card, `kms.c`; the GPU context's device in `gpu.c`). Syncobjs are
per-fd handles; every one below is `DRM_RENDER_ALLOW` and needs no master:

| Step | ioctl | What for |
|---|---|---|
| import a timeline | `DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE` (no flags) | the client's timeline fd → a handle; the fd is closed at once |
| forget a timeline | `DRM_IOCTL_SYNCOBJ_DESTROY` | when its resource is destroyed (pending waits on it are cancelled first) |
| wait for an acquire point without blocking | `DRM_IOCTL_SYNCOBJ_EVENTFD` with `DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE` (Linux 6.6+) | an eventfd that becomes readable when the point has a fence (available) |
| the acquire fence for the GPU | `DRM_IOCTL_SYNCOBJ_TRANSFER` into a temporary binary syncobj, then `DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD` with `EXPORT_SYNC_FILE` | a sync_file fd that EGL waits on in the GPU queue (`EGL_ANDROID_native_fence_sync` + `eglWaitSyncKHR`): the CPU never waits for rendering |
| signal a release point (CPU path, or nothing to sample) | `DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL` | the buffer is free now |
| signal a release point after the GPU sampled | `EGL_ANDROID_native_fence_sync` → its fd → `DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE` with `IMPORT_SYNC_FILE` into a temporary syncobj → `DRM_IOCTL_SYNCOBJ_TRANSFER` to the release point | released when the frame that read it is done on the GPU, without the CPU waiting |
| a kernel without `SYNCOBJ_EVENTFD` | — | the global is not offered (a blocking `SYNCOBJ_TIMELINE_WAIT` on the main thread is never an option) |

No new library: these are kernel uapi structs (`drm/drm.h`), as `kms.c` already uses without libdrm.

## Never blocking the loop

1. `wl_surface.commit` with an acquire point that isn't available yet: the commit's whole state (buffer, damage,
   frame callbacks, presentation feedback, viewport, scale…) is moved into a **pending commit** on that surface and
   not applied. lord-server asks for an eventfd (`SYNCOBJ_EVENTFD`, `WAIT_AVAILABLE`) and adds it to the main loop's
   `poll()` set next to the Wayland socket.
2. When the eventfd is readable, the pending commit is applied exactly as a commit is today (`srf_commit`'s body),
   then the next pending one, in order. Subsurfaces in synchronized mode wait with their parent's commit.
3. An acquire point already available when committed (the usual case: the app waited itself) is applied at once,
   with no eventfd.
4. The GPU waits for the actual rendering through the exported sync_file (`eglWaitSyncKHR`); the CPU never calls a
   blocking wait. On the CPU path (no GPU context), a dmabuf is mapped and copied only after its acquire point is
   *signalled*, not only available: `SYNCOBJ_EVENTFD` without `WAIT_AVAILABLE`.

## Limits per client (all fail closed)

| What | Limit | Past it |
|---|---|---|
| imported timelines | 64 per client | `wl_client_post_no_memory` (the client is disconnected) |
| syncobj surfaces | one per surface (the protocol's rule), at most 256 per client | as above |
| pending commits per surface | 4 | the oldest waiting ones stay; the fifth commit disconnects the client (`no_memory`): a client that keeps committing unsignalled frames is broken |
| eventfds alive | 32 per client, 256 in the session | as above for the client; the session limit refuses new waits by disconnecting the client that asked (as built: 128 in the session, and only a process past its share is disconnected; see "As built") |
| point values | any u64; the release point must be above the acquire point on the same timeline | `conflicting_points` |

Every handle, eventfd and pending commit is freed when its resource, surface or client goes (the destroy listeners
`wl.c` already has), so a client can't leak them past its own life.

## A point that never signals

Only that surface freezes: its pending commits stay pending, its frame callbacks (which belong to them) are not sent,
and the window keeps showing its last applied frame. Nothing else waits on it: other surfaces of the same client,
other clients, the UI and the cursor go on. After 5 s lord-server logs it once per surface (`wayland: <app>: a frame
waits for its acquire point for 5 s`); it never times out by itself (the protocol has no timeout, and showing an
unfinished frame would be what explicit sync exists to prevent). Closing the window, or the client going away, frees
everything. A release point is always signalled, even for a frame never shown (the buffer was replaced, or the
surface destroyed), so the app never waits on lord-server forever.

## What Xwayland, NVIDIA and Mesa expect

- **Xwayland 24.1+** binds the global when present and uses it for its GL/Vulkan clients; with it, X11 games on a
  dmabuf path present through syncobj too. Our Xwayland launcher (`xwayland.c`) passes nothing new.
- **NVIDIA's driver** (555+) needs it to avoid flicker under Xwayland and in native Wayland Vulkan; the rest of what it
  needs (GBM, `linux-dmabuf` with its modifiers) LordOS already has, but the driver stays unsupported.
- **Mesa's Vulkan WSI** (24.1+) uses it when offered and falls back to implicit sync when not, so offering it changes
  no behaviour for an app that doesn't bind it. Mesa's EGL Wayland platform does the same.
- Chromium and Firefox don't need it today (implicit sync on Mesa).

## Tests (to write with the code)

- **Unit (pure header, `src/wl/syncobj.h`):** the commit queue state machine (available at once, waits, two pending in
  order, the fifth disconnects, a surface destroyed with commits pending frees them and signals their release points),
  the protocol's error rules (both points, neither, shared memory, conflicting points), the limits.
- **With the kernel, no GPU needed:** a test client (`tests/syncobj_client.c`) that makes a timeline on a render node
  (`DRM_IOCTL_SYNCOBJ_CREATE` + `HANDLE_TO_FD`), commits a udmabuf frame with an unsignalled acquire point, checks that
  no frame callback comes, signals the point from the CPU (`TIMELINE_SIGNAL`), checks that the frame callback and the
  presentation feedback come and that the release point is signalled; then a point never signalled: another window of
  the same client keeps getting frames. Run against a headless lord-server (skipped with "needs a render node with
  syncobj timelines" when there is none).
- **GPU path:** the same client with `LORD_GPU=1` on VM 116 or the Pi 5, and `vkcube`/`glmark2-es2-wayland` with Mesa
  24.1+ (explicit sync in use: `WAYLAND_DEBUG` shows `set_acquire_point`), no tearing or half-drawn frames, and
  LordBench's frame phases unchanged.
- **ASan build** of lord-server for the whole syncobj suite (handles and eventfds freed with their resources).

### Results (2026-11-22)

- **Unit:** `build/syncobj-test` (error rules, commit action, queue, limits; in `make test`).
- **Kernel:** `build/syncobj-kernel-test` (import, a non-blocking check, the eventfd waking on the app's signal, release
  at once and through a udmabuf's exported fences, a non-dmabuf fd falling back, references) and
  `tests/syncobj_headless.sh` (`tests/syncobj_client.c`: wait, order, freeze, limit, every error, implicit again), both
  in `tests/run_suites.sh`. They pass on the development PC (Linux 6.12, i915 + amdgpu render nodes) and on VM 116
  (virtio-gpu render node), headless on the CPU path.
- **GPU path, VM 116:** lord-server on the monitor with `gpu: GPU composition (llvmpipe …)` (`LORD_GPU=1`, the card
  lent by `build/seat-cycle` with `lordos.service` stopped for the run and started again): the same client passes every
  check (the presentation check aside: in that setup no frame is presented at all, the same with `origin/main` and a
  plain `dmabuf-client`, so it is not this work). virtio-gpu refuses to import udmabufs in EGL (`GPU.md`), so there too
  the buffers are copied and released at once: **a buffer sampled in place on a real GPU and released after the next
  frame with its fences is not checked on hardware yet** (the kernel plumbing of that release is, above).
- **Real client:** `vkcube --wsi wayland` (Mesa 25.0.7, RADV on an AMD RX 580) against a headless lord-server binds the
  global and sends `set_acquire_point`/`set_release_point` with every commit: 600 frames at 60.1 fps in FIFO, 1500 at
  330 fps in mailbox, no protocol error, no stall. `eglgears_wayland` (Mesa's EGL doesn't use it in 25.0) unchanged.
- **ASan:** lord-server built with `-fsanitize=address` ran the whole client suite with no error.
- **Not checked:** NVIDIA's driver (unsupported, no card), Xwayland 24.1's use of it, GPU sampling in place on a real
  card (above), LordBench's frame phases with an explicit-sync client.

## What it costs

About 600 lines in `wl.c`/`syncobj.c` plus the header and tests; one more poll entry per waiting surface. No new
dependency, no new privilege, no admin action. The global would be listed in `docs/WAYLAND.md`'s table and in
`AGENTS.md` once approved.
