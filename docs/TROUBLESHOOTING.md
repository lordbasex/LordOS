# Troubleshooting

## "This LordOS component was modified and was not loaded"

LordOS checks its own code (the themes, the login screen, the system apps) against a list signed with
the LordOS key before running it ([SIGNING.md](SIGNING.md), "Verified system code"). This message,
or the recovery screen, means a file in `/usr/lib/lordos` no longer matches: a disk error, an edit by
hand, or tampering. To see which files:

```sh
sudo /usr/lib/lordos/bin/lord-server --check-code
```

To repair it, reinstall the package (your files and settings are not touched):

```sh
sudo apt install --reinstall lordos
```

## Typing feels slow

If keys show up late in one app (for example Chromium) but not in LordOS's own apps (Notes, the
Terminal), check where the time goes before changing anything. `tools/diag/keylag.sh` does it in
about two minutes.

### The 2-minute check

1. Get the script onto the machine (it is not part of the package):

   ```sh
   curl -fsSLO https://raw.githubusercontent.com/lordbasex/LordOS/main/tools/diag/keylag.sh
   ```

2. Run it in the LordOS Terminal: `sh keylag.sh`. It prints:
   - the version;
   - GPU or CPU composition;
   - the virtual machine and its guest tools;
   - how many times Chromium logged `The server has buggy presentation feedback` in this session.

   On LordOS 0.8.0 and the 0.8.1 pre-release ISOs, that Chromium line on the GPU path is the bug
   described below. To confirm it without the trace, reboot and add `lordos.gpu=0` to the kernel
   line for one boot. That forces CPU composition. Then type in Chromium again.

3. On 0.8.1 or later, turn the trace on for one boot. In the boot menu, press `e`, add
   `lordos.inputdebug=1` at the end of the line that starts with `linux`, and press Ctrl+X. On an
   installed system with a hidden menu, hold Shift (BIOS) or press Esc (UEFI) while it starts.
   Log in and run `sh keylag.sh` again. When it asks, click the slow app and type normally for
   30 seconds.

The script then prints p50, p95 and the maximum for each stage, in ms after the kernel's evdev
timestamp:

| Stage | What happened |
|---|---|
| `rx` | `lord-server` read the key |
| `flush` | the key was written to the app's socket (`wl_keyboard.key`) |
| `commit` | the app sent its next frame (its answer: the glyph) |
| `shown` | the page flip with that frame finished |

It also reports the share of key presses that arrived less than 15 ms after the previous one.
Real typing almost never does that. A high share means the keys were held back before the
guest's kernel and then delivered in clumps.

How to read it:

- **`rx` high:** `lord-server`'s main loop is busy. Report it with the output.
- **`commit − flush` high:** the app itself is slow.
- **`shown − commit` high:** frames reach the screen late, a compositor problem. Report it, and use
  `lordos.gpu=0` until it is fixed.
- **Everything under ~40 ms but typing still lags:** the delay is before the guest's kernel, in
  the hypervisor. On VMware, the reported fixes are on the host side (VMware's forums and
  [open-vm-tools#696](https://github.com/vmware/open-vm-tools/issues/696)):
  - turn off "Accelerate 3D graphics";
  - use fewer vCPUs;
  - add `keyboard.vusb.enable = "TRUE"` to the `.vmx`.

  None of them is a guest bug.

### The trace and your privacy

The trace measures time. It is not a key log:

- **What a line holds:** an opaque sequence number, press or release, and the stage times. No key
  code, character or modifier is ever written.
- **Which keys are traced:** only keys handed to a Linux (Wayland) app. Keys for LordOS itself are
  never traced or counted: the theme, its text fields, the Terminal, the Passwords app, the
  keyring and admin prompts, the login and lock screens, isolated LordOS apps and protected
  windows.
- **Who can turn it on:** only `lord-sessiond` (root) passes `LORD_INPUT_DEBUG=1` to user
  sessions, and only when the kernel line has `lordos.inputdebug=1` or the service sets
  `LORD_INPUT_DEBUG=1`. There is no run-time switch, and the login screen never gets it.
- **Where it goes:** its own file, `$XDG_RUNTIME_DIR/lordos-keylag.log`, created with mode 0600 in
  the session's tmpfs and gone at logout. It is never written to the session log.
- **How long it runs:** it stops by itself 120 s after the first traced key, or after 4000 lines.
  Log out and back in for a new one.

The code is in `server/src/keylag.c`.

### The 0.8.1 fix: Linux apps' frames on the GPU path

The VMware report (Fusion, UEFI, 3D on): typing lagged in Chromium but not in Notes, and Chromium
logged `buggy presentation feedback` hundreds of times. Firefox looked frozen.

**Cause.** On the GPU path, a Linux app's new frame only replaces a texture. It is not UI damage,
so `render_frame` skipped every display that had no other damage, and nothing was flipped.

- **What the user saw:** the typed character appeared only when something else on the screen
  changed: the clock, the cursor, an animation.
- **What the apps got:** frame callbacks still went out, but presentation feedback piled up
  waiting for a flip.

The bug was present in 0.8.0 as well (same code). On the CPU path, the window is painted into the
UI and damages it, so that path was never affected.

**Fix** (`server/src/main.c`):

- A Linux app's new content marks every GPU display (`wl_new`), and those displays present it in
  the next frame.
- When a frame is shown by a modeset instead of a page flip, its presentation feedback is sent
  right away.
- When nothing is drawn (screen off, the session away from the monitor), apps still get their
  frame callbacks once a second and their feedback is discarded (`wl_frame_unseen`), so none of
  them stalls.

**Measured** on Proxmox (VM 123, 4 vCPUs, GPU composition with llvmpipe). The harness typed 80
keys into a Chromium text area with `tools/diag/keylag-inject.py` and timed them with the trace.
Times are in ms after the kernel event, p50 / p95:

| Build | Chromium, static page: on screen | Chromium, page animating: on screen | Chromium "buggy presentation feedback" |
|---|---|---|---|
| 0.8.0 (46c972a) | 829 / 1840 | 903 / 1876 | 113 lines |
| main before the fix | 949 / 1866 | 1000 / 1917 | 112 lines |
| main with the fix | **18 / 25** | **28 / 47** | **0** |
| main with the fix, CPU composition | 15 / 21 | 24 / 47 | 0 |

Other results:

- **Firefox with the fix** (GPU path): on screen 31 / 39 ms.
- **The test client** (`tools/diag/keylag-client.c`, a minimal Wayland window that repaints on each
  key), CPU composition: same in 0.8.0 and main. Idle: on screen 2.5 / 6.6 ms in 0.8.0 and
  2.6 / 7.5 ms in main. With Chromium animating: 32 / 47 ms in 0.8.0 and 35 / 59 ms in main.
- **The key path itself:** reading evdev and handing the key to the app (`rx`, `flush`) stayed
  under 1 ms at p50 in every build. Nothing on that path changed between 0.8.0 and 0.8.1.

### The harness (developers)

Everything is in `tools/diag/`:

- **`keylag-inject.py`** (root) creates a uinput keyboard and types at known `CLOCK_MONOTONIC`
  times. `lord-sessiond` lends the new device to the session through inotify.
- **`keylag-client.c`** prints when `wl_keyboard.key` arrived and when its frame was presented.
  Build commands are at the top of the file.
- **`LORD_INPUT_DEBUG=1`** in a drop-in for `lordos.service`
  (`[Service] Environment=LORD_INPUT_DEBUG=1`) turns the trace on in development VMs.
- **`LORD_GPU=1`** in the same drop-in forces GPU composition (llvmpipe) on a display without 3D.

### open-vm-tools

The 0.8.1 ISO keeps `open-vm-tools`, which installs only on VMware
(`ConditionVirtualization=vmware`). It runs `vmtoolsd` with these plugins:

- power operations, guest info, time sync (only if the host enables it), backup quiescing and
  `deployPkg`;
- `resolutionKMS`, which only acts when the host asks for a new display layout.

None of these is on the input path. The kernel's VMMouse driver handles the absolute pointer by
itself, and `vmtoolsd` does not touch it. No upstream report ties `open-vm-tools` to keyboard
lag: [open-vm-tools#696](https://github.com/vmware/open-vm-tools/issues/696) was closed as a
VMware Workstation bug. The VMware report above was the GPU presentation bug, and it reproduced
on Proxmox, where `open-vm-tools` is inactive.

## Known issues in 0.8.5

From the release notes (`CHANGELOG.md`, 0.8.5-beta, "Known issues and limitations"):

- **A black rectangle between the boot menu and the splash (legacy BIOS).** When the menu's 5 s run out
  (or Enter is pressed) GRUB loads the kernel and the initrd from the disc and shows its terminal area,
  painted black, until the kernel starts: a few seconds on BIOS, barely visible on UEFI. Harmless; 0.8.4's
  ISO does the same.
- **Remote desktop: black corners on a window being dragged.** Only in the frames sent to the browser;
  the monitor shows them right. Being looked at ([REMOTE.md](REMOTE.md)).
- **llama.cpp on a GPU** is not tested on real hardware yet; without a Vulkan device it runs on the CPU.
- **Searching "displays"** lists the Help article on touchpads and touch screens before Settings →
  Displays; **Settings → Displays** cuts the name of a small display in the arrangement without an
  ellipsis.
- **Steam** isn't in the Store: it needs i386 multiarch, which LordOS's package engine doesn't do yet
  ([ROADMAP.md](ROADMAP.md)).
