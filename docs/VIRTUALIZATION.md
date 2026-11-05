# Virtualization

The design of **Machines** (working name), LordOS's desktop virtualization app: run other operating systems in windows of the
LordOS desktop, on the machine you have, at native speed where the hardware allows it, for free and with open source parts.
This is the design, and it follows the Virtual machines item in [ROADMAP.md](ROADMAP.md). Phase 0 (section 13) and the internals
of Phase 1 (3.9) are built; the app isn't.

How to read it:

- **Measured** means checked on our own hardware or in the named package on 2026-11-01, or, where it says so, in Phase 0 on
  2026-11-04 on the machine named (section 13). A number marked **nested** was taken on an amd64 host that is itself a virtual
  machine, so it is not a bare-metal figure. **Reported** means a public source says so
  and we haven't reproduced it. **To test** means nobody here has run it. Any number under a "target" heading is a goal, not a result.
- "virtio-win guest" means a guest system that needs the virtio-win driver set. Public pages and the app use generic wording
  ("other systems"), as [the brand rule](../tools/brand-check.mjs) requires; a guest's real name appears only as the label of its
  download or its installer.


Mocks of every screen: https://claude.ai/artifact/WNiELGEazSqnr68SFWuyPz (private to the maintainer).

## 1. Goals

The bar is section 16: the most complete desktop virtualization app, and visually the best, with every claim marked for what Linux allows today.

1. **Native first.** A guest of the host's own architecture runs on the kernel's KVM. On a PC that is amd64 guests, on a Pi arm64
   guests. Nobody has to know the word "emulation" unless they ask for another architecture.
2. **One app, no assembly.** No XML, no command lines, no second window from another toolkit. Creating a VM, installing its system
   and getting its drivers is one flow.
3. **LordOS's own desktop.** The VM's picture is a node in our compositor, with no QEMU window, no X11, no toolkit. Moving, resizing,
   full screen, Picture in Picture, desktops and the theme all work as for any app.
4. **Same security model as the rest of LordOS.** `lord-server` never runs as root. Each VM is one sandboxed process tree. Privileged
   steps are closed admin actions. A guest gets nothing of the host unless the person gives it.
5. **Both architectures, same features where the hardware allows.** Where it doesn't, the app says so instead of offering a switch
   that fails.
6. **Honest about limits.** No claim on a screen that hasn't been tested on that hardware.

Non-goals for the first versions: servers and clusters, live migration between hosts, remote management of a host, GPU partitioning,
containers (the Android container in the roadmap is a separate item), and guests whose license ties them to one maker's hardware,
which are not offered.

## 2. What the hardware can and can't do

### Answer in one table

| Host | Guest | How | Speed | Status |
|---|---|---|---|---|
| amd64 | amd64 | KVM (VT-x or AMD-V) | near native | the main path |
| arm64 PC or board with a recent core | arm64 | KVM | near native | the main path |
| Raspberry Pi 5 | arm64 | KVM, GICv2 | native CPU speed (measured), interrupt cost not measured | works, see 2.2 and 13 |
| Raspberry Pi 4 | arm64 | KVM, non-VHE (to confirm) | slower, 8 GB at most | best effort |
| amd64 | arm64 | QEMU TCG | much slower, see 2.4 | works, labelled |
| arm64 | amd64 | QEMU TCG | much slower, see 2.4 | works, labelled, Pi not recommended |
| any | 32-bit x86 on amd64 | KVM | near native | works |
| any | 32-bit arm on arm64 | KVM only if the core has AArch32 at EL1 | varies | not offered |

**Can arm64 emulate amd64? Yes.** `qemu-system-x86_64` runs on an arm64 host with TCG, and Debian packages it for arm64. It can't use
KVM: hardware virtualization only runs guests of the CPU's own instruction set. There is no shortcut in the kernel for this. It is
fast enough for a text console, a server, a small Linux; it is not a usable desktop for a full modern system on a Pi.

**Can amd64 emulate arm64? Yes**, the same way, with `qemu-system-aarch64` and TCG. A strong PC gives a usable light Linux, and a slow
heavy desktop.

Inside a guest, the system's own emulator can run other-architecture programs: an arm64 guest of a modern desktop system runs
amd64 programs through the emulator it ships with (**Reported**). That is the guest's feature, and in practice the best way to run
such programs on an arm64 host, far better than emulating a whole amd64 machine.

### 2.1 amd64 hosts

KVM needs VT-x or AMD-V enabled in the firmware, plus EPT or NPT for acceptable speed. Off in firmware is the most common failure, so
the app detects it and shows the fix, not an error code. On a host that is itself a VM, KVM only exists if the outer hypervisor
exposes nested virtualization. Without it the app offers TCG and says it will be slow. Where the outer hypervisor does expose it, KVM
works: a guest inside a desktop hypervisor's VM ran at 0.98 of that VM's CPU speed (**Measured**, 2026-11-04, **nested**, section 13).

GPU passthrough (VFIO) exists on PCs with a second GPU and proper IOMMU groups. It is an advanced option for a later phase; it takes
a device away from the host and can't work with the only GPU that draws the desktop.

### 2.2 Raspberry Pi 5 (measured on our board, Model B Rev 1.1, 16 GB, LordOS kernel 6.18.57)

| Fact | Value | Source |
|---|---|---|
| `/dev/kvm` | present, `root:kvm 0660` | Measured |
| KVM | built in, `CONFIG_KVM=y` | Measured |
| Mode | `VHE mode initialized successfully`, all CPUs started at EL2 | Measured, dmesg |
| Guest physical address space | IPA limit 40 bits (1 TB) | Measured, dmesg |
| Interrupt controller | GICv2 with vGIC | Measured, dmesg |
| vGIC warning | `GICV region size/alignment is unsafe, using trapping (reduced performance)` | Measured, dmesg |
| Page size of the running kernel | 16 KiB, `CONFIG_ARM64_16K_PAGES=y`, 47-bit VA | Measured |
| Vhost, tap, bridge | `VHOST_NET`, `VHOST_VSOCK`, `TUN`, `BRIDGE`, `MACVTAP` as modules | Measured |
| VFIO | not configured | Measured, no `CONFIG_VFIO=` line |
| User access | the session user isn't in group `kvm` | Measured |

What this means:

- **KVM works on the Pi 5, in the kernel we already ship.** No new kernel is needed for a first version.
- **The 16K question.** The host's page size and the guest's are separate things: KVM's second-stage tables use the host's granule
  and the guest's own tables use the guest's, if the CPU implements it. KVM on arm64 has handled 16K hosts since 2016 (**Reported**,
  kernel patches). So a 4K Debian guest on a 16K host should run. A public project boots an arm64 virtio-win guest under KVM on Pi 5 with plain `-M virt -cpu
  host` and contains no page-size check (**Measured** by reading its script, not by running it), which suggests that the 16K default
  kernel is fine. **Measured on 2026-11-04 on our Pi 5:** a Debian 13 guest with a 4K-page kernel (6.12.115) boots and runs under KVM
  on the 16K LordOS kernel, with GICv2 and `-cpu host`, at native CPU speed (guest/host 1.00–1.01 on SHA-256, 2.4 % slower on a
  Python loop). Details in section 13.
- **The price of 16K.** The vGIC warning comes from a kernel check of the GICV region's alignment (**Reported**, the 2016 patch series).
  When it fails, each guest access to the virtual CPU interface traps and is emulated: interrupt-heavy work costs more. Whether the
  firmware's 4K kernel (`kernel8.img`) avoids it is still **To test**: Phase 0 measured no interrupt-heavy work under either kernel.
  If it does, the app can recommend, never force, a 4K boot for people
  who use VMs a lot. Our LordOS kernel has no 4K twin at this moment: the board only has the Debian-style `rpi-v8` kernel, and a
  4K LordOS build would be new work, and Pi 4 path differences apply (no Landlock on the archive kernel).
- **4 vCPUs, always.** The board has 4 Cortex-A76 cores; GICv2 itself stops at 8 CPUs, so it's not the limit here. The default for a
  guest is 3, leaving one for the desktop. **Measured on 2026-11-04:** with a guest busy on 3 vCPUs, dragging a window stayed at
  59.3 FPS (58.0 idle); with 4 busy vCPUs it fell to 2.4 FPS, with stalls of about 1000 ms. The same collapse happens with no VM when
  the host runs 4 busy threads (3.3 FPS), and `nice 19` on QEMU doesn't help, so it's the desktop's own scheduling, not a cost of
  virtualization. The 4-vCPU run had someone using the board at the same time and is to be repeated. **Decision (changed):** 3 stays the
  default, and the slider's recommended zone on a Pi ends at 3; choosing 4 is allowed, with one line saying the desktop may stall while
  the machine is busy.
- **Memory.** 16 GB board: 15 GB visible to Linux on ours. Suggested guest maximum: board RAM minus 2 GB. 2 GB and 4 GB boards can run a
  small Linux, not a full desktop system. The kernel's guest address limit is far above any Pi.
- **No device passthrough.** There's no VFIO, and no IOMMU path worth using: PCIe, GPU and NVMe stay with the host.
- **No nested virtualization.** The kernel logs its nested trap tables, but Cortex-A76 is ARMv8.2 without FEAT_NV, so the kernel
  can't expose EL2 to a guest. KVM inside a Pi guest doesn't work. **Measured on 2026-11-04:** `KVM_CAP_ARM_EL2` is 0 on `/dev/kvm`
  and on a VM descriptor, and so is `KVM_CAP_ARM_EL2_E2H0`.
- **Storage.** A VM on a microSD card is painful. The app warns and suggests NVMe or USB 3 SSD, and refuses nothing.
- **Temperature and power.** A VM with all cores busy is a constant load: the Cooling view and the board's fan curve matter, and the
  voltage view applies. A proper power supply is a requirement. With the board's fan curve, a guest busy on 3 vCPUs reached
  51–55 °C with the fan at up to about 2770 rpm and no throttling, and 4 busy vCPUs reached 55–57 °C (**Measured**, 2026-11-04, during the drag test of 13.7).

### 2.3 Raspberry Pi 4

Cortex-A72, ARMv8.0, GICv2, so no VHE: KVM runs split between a small hypervisor at EL2 and the kernel (**To test**, from the
architecture, not from a measurement). Slower exits, 8 GB at most. A public project limits a virtio-win guest on boards without A76
cores to an older release, because the newest needs CPU instructions these cores lack (**Reported**). Plan: Linux guests only, best
effort, with no support claim until tested. The Pi 4 path also uses an archive kernel without Landlock, so the sandbox of 6.2 is
weaker there; the app shows that in Security.

### 2.4 Emulation (TCG): honest numbers

The roadmap's 5–20× was a planning range. QEMU's own measurements give several host instructions per
guest instruction before counting devices (one published report: about 6.5:1 on an x86-64 target, **Reported**), and memory-ordering
between architectures costs more on multi-core guests.

**Measured on 2026-11-04** (section 13 has the full table), with the same benchmarks inside a KVM guest and an emulated one:

| Host | Emulated guest | Python loop | SHA-256, 1 thread | Boot to login, 2 / 4 vCPU |
|---|---|---|---|---|
| Pi 5 | amd64 | **25.7×** slower | 19.6× slower | 98.5 s / 82.6 s |
| amd64, **nested** | arm64 | **58.6×** slower | 6.0× slower (flattering, see 13.4) | 59.6 s / 55.1 s |

So the 5–20× range is optimistic: on the Pi the cost is 20–26×, and the Python figure is the better general one. Multi-threaded
emulation is QEMU's default (one thread per vCPU), and 4 vCPUs boot about 15 % faster than 2 on the Pi. An emulated guest also takes
more host memory than a native one: about 750–800 MB of QEMU RSS at login for a 2 GB guest, against about 420–550 MB under KVM.

Decisions:

- Another architecture is available, marked **Emulated**, with a line under it: "Runs much slower than a native machine."
- It isn't in the assistant's cards by default: it sits behind "Other architecture…".
- A modern desktop system for amd64 on a Pi is not offered as a card at all: it would not be usable, and we'd have to claim
  otherwise. It remains possible from an ISO, with the warning.
- GPU acceleration is off in emulated guests.
- Real numbers per board are recorded in section 13; a board not listed there is **To test**.

## 3. Architecture

```
lord-server (the person's user, never root)
  └─ Machines app (React, sdk/vm.js → lord.vm.*)
       │  closed RPC
       ▼
  lord-vmdisplay  one per running VM: receiver and validator, sandboxed, no files, no network
  lord-vm         one per running VM: supervisor, sandboxed, owns the VM's folder
    ├─ qemu-system-<arch>  device engine, in its own sandbox and cgroup scope
    ├─ swtpm               TPM state, when the VM has one
    ├─ virtiofsd           one per shared folder
    ├─ passt or tap helper network
    └─ usbredir            attached device, on demand
       ▲
  qemu ──p2p D-Bus──▶ lord-vmdisplay ──fixed binary messages, dmabuf by descriptor──▶ lord-server's compositor (MediaView)

Admin helper (closed actions, root)
  vm-enable, vm-bridge-on/off, vm-usb-allow
```

### 3.1 Parts

- **KVM** is the kernel's. We add nothing in the kernel.
- **QEMU** is the device engine, used through its control socket (QMP) and never as an app. Debian 13 ships 10.0.13
  (**Measured**, `apt-cache policy`). `qemu-system-x86` is 68 MB installed by itself (**Measured**).
- **`lord-vm`**, new, C, the supervisor: starts the tree, holds the QMP connection, applies settings, takes snapshots, reports state.
  It writes no display code.
- **`sdk/vm.js` and `lord.vm`**: the app's only way in, as `lord.rdesk` is for Remote Desk.
- The approach is the one `lord-rdesk` proved: a helper in its own cgroup scope with memory limits and a Landlock and seccomp sandbox, talking
  over a socketpair, drawing into something the engine shows.

### 3.2 Display: no QEMU window

QEMU's D-Bus display is exactly the fit. Started with `-display dbus,p2p=yes`, QEMU takes a Unix socket and sends display updates as
`ScanoutDMABUF` (a dmabuf fd with size, stride, fourcc and modifier) or plain `Scanout` in shared memory, plus cursor shape and pointer
state. It takes input through `Keyboard`, `Mouse` and `MultiTouch`, and has `Clipboard`, `Audio` and `Chardev` interfaces; USB
redirection travels over Chardev with the stream name `org.qemu.usbredir` (**Reported**, QEMU's documentation). Debian ships it:
`ui-dbus.so`, `audio-dbus.so`, `ui-egl-headless.so` and the virtio-gpu GL modules are in `qemu-system-modules-opengl`
(**Measured**, Debian's file list, amd64). The D-Bus display runs with Debian's 10.0.13 on both architectures (**Measured**,
2026-11-04, a Debian 13 guest on the Pi 5 and on a **nested** amd64 host, section 13.6).

So one socket carries picture, input, clipboard, sound and USB. No bus daemon is involved: QEMU accepts the peer through QMP `add_client`.

**The compositor never speaks D-Bus to QEMU.** A compromised QEMU must not get a parser inside the process that draws the whole desktop, so the top security risk gets a process of its own. `lord-vmdisplay` is small, one per VM, sandboxed with no files and no network. It speaks that D-Bus protocol to QEMU and passes `lord-server` only fixed binary messages over a socketpair, as `lord-rdesk` does.

It checks before forwarding: the size against limits, the dmabuf's real length (`lseek`) against stride and height, fourcc and modifier against what this GPU imported before, the update rate, the cursor's size, and the clipboard's size and types. Input and clipboard travel back the same way.

What this doesn't remove: importing a dmabuf still reaches the host's GPU driver in the compositor, so an import failure must show a black picture and never take the session down, and **the compositor never waits on a guest's fence without a timeout**. A guest that hangs its GPU work costs its own window, not the desktop.
 The media node path (`MediaView`) already shows dmabufs and shared-memory
pictures from `lord-media` and `lord-rdesk`.

- With GPU: a dmabuf goes straight to the compositor with no copy. **Target:** one frame of added latency at most.
- Without GPU acceleration (2D guests, TCG, virtio-win guests): the shared-memory path, copied on update, with damage rectangles.
- Multi-head guests: one console per head, one window or desktop per head.

Multi-plane dmabufs need `ScanoutDMABUF2`, added in 2025 (**Reported**). It isn't in 10.0.13 (**Measured**, 2026-11-04: not in the
introspection, the module or the 10.0.0 interface file), so Debian 13 offers single-plane dmabufs only.

**What Phase 0 changed here** (prototype on both architectures, 2026-11-04, section 13.6):

- **Decision (changed): `lord-vmdisplay` requires `Listener.Unix.Map`.** With it, the picture arrives once as a memfd (`ScanoutMap`)
  and updates as rectangles (`UpdateMap`). Without it, every full-screen update is a 4 MB `Scanout` message, QEMU uses about 70
  percentage points more CPU, and on amd64 a mode switch sends partial updates before any `Scanout`, so the picture stays stale. The
  plain array path isn't used.
- **Decision (changed): the helper caps the copy rate at the display's refresh rate.** A scrolling text console makes virtio-gpu 2D
  send about 2000 full-frame updates a second on amd64 and about 1000 on the Pi. Merging damage and copying at most 60 times a second cut
  the helper from 49 % to 8 % of a core on amd64 (**nested**) and from 86 % to 16–23 % on the Pi, with QEMU's own CPU unchanged.
- **The memfd QEMU passes is only sealed against new seals** (`F_SEAL_SEAL`), not against shrinking, so QEMU can truncate it under the
  helper's mapping. The helper's copy must survive `SIGBUS`, or it refuses a buffer without `F_SEAL_SHRINK`.
- **The shared picture tears**, since QEMU writes it while it's read. Forwarding copies the damaged area first and never hands the
  compositor QEMU's live mapping.
- **QEMU calls `ScanoutMap` synchronously from its main loop**, so the helper never makes a blocking call into QEMU: input goes out
  asynchronously. A peer-to-peer client of the system's D-Bus library works with no bus daemon, because QEMU is the authenticating
  side on both sockets.
- **arm64 3D is blocked in 10.0.13.** With `gl=on`, a reset of the virtio-gpu device from a vCPU thread aborts QEMU on an assertion
  (`dbus_scanout_texture: Assertion 'tex_id' failed`). Every UEFI boot does that reset, and a guest can do it on purpose by unbinding
  its GPU driver, so a guest can kill its own QEMU. It works only when the guest kernel boots directly. See 6.10.
- **V3D's buffers are tiled:** `XB24` (XBGR8888) with the V3D tiled modifier `0x0700000000000006`, one plane, and a descriptor longer
  than stride × height. The compositor imports with that modifier; a linear map would scramble the picture. Whether the import works and
  the picture is right is **To test**.

### 3.3 Sandbox

QEMU parses guest-controlled data all day, so **assume it can be compromised**. What a compromised QEMU may reach is the design:

- Own user namespace if available, no capabilities, `no_new_privs`, in `app-lordos-vm-<id>.scope` with memory and CPU limits.
- **Landlock:** read-only `/usr/lib`, firmware and ISOs chosen for this VM; read-write only that VM's folder. On kernels without
  Landlock (see 2.3) the app shows "Weaker isolation on this kernel".
- **Seccomp:** QEMU's own `-sandbox on,obsolete=deny,elevateprivileges=deny,spawn=deny,resourcecontrol=deny` plus ours. Our allowlist needs care: QEMU uses many
  ioctls on `/dev/kvm`, and `lord-server`'s helper filter can't be reused unchanged. QEMU's own filter costs nothing measurable: boot
  to login 36.5 s with it and 36.0 s without, QEMU RSS 424 MB against 422.5 MB (**Measured**, 2026-11-04, amd64, **nested**, 6 boots
  each). It also ran in every Pi run, 3D included, with no trouble (**Measured**, 2026-11-04, Pi 5). Our own filter on top is **To test**.
- `/dev/kvm`, `/dev/vhost-net`, render node and tap are opened by `lord-vm` and passed as descriptors, so QEMU can't open devices. For KVM, `-add-fd` plus `-accel kvm,device=/dev/fdset/NN`, which Debian 13's QEMU manual documents, works on both architectures with the sandbox on (**Measured**, 2026-11-04, Pi 5 and **nested** amd64). Passing the render node by descriptor is **To test**: in Phase 0 QEMU opened it itself, through the user's `render` group.
- On amd64 with `-nodefaults`, the guest needs a display device, or its kernel never starts (**Measured**, 2026-11-04, **nested**); `lord-vm` always adds one, `-display none` included. On arm64 `virt` has no keyboard of its own, so a machine with a picture also gets `virtio-keyboard-pci`.
- No network sockets of its own: networking is an fd. Debian 13's QEMU has no `-netdev passt`, so `passt` gets one end of a socketpair and QEMU the other through `-netdev stream,addr.type=fd` (**Measured**, 2026-11-04, both hosts).
- Files are opened by the supervisor and passed by descriptor wherever QEMU allows (`/dev/fdset`).
- No shell, no `-readconfig` from the guest's folder: arguments are built from validated settings.
- A machine can't outlive `lord-vm`: `PR_SET_PDEATHSIG` is set before exec. QEMU's newer `-run-with exit-with-parent=on` isn't in Debian 13's version (**Measured**: the 10.0 manual lists only `async-teardown`, `chroot` and `user`). swtpm exits with QEMU.
- `-M dump-guest-core=off`, no core dumps, and non-dumpable processes, so that guest memory doesn't land in a host file.
- One VM can't read another's folder: one process tree and one Landlock domain per VM.
- A guest's disk file isn't encrypted by default; see 8.

### 3.4 Access to `/dev/kvm`

`/dev/kvm` is `root:kvm 0660` and our session user isn't in `kvm` (**Measured**). Options: add the user to `kvm` (a closed admin action,
`vm-enable`, asked once, new login needed) or let the seat rule grant it. Our sessions come from `lord-sessiond`, so seat rules must be
checked. Decision: `vm-enable`, asked the first time, one question with one sentence of explanation.

**Built in the alpha** (3.9, The app): the closed admin action is `vm-kvm` (`vm-enable` installs the packages). It adds the session
user to `kvm` for the next sessions *and* gives them `rw` on `/dev/kvm` now with a POSIX access-list entry written by the helper
(`system.posix_acl_access`, no `setfacl` needed), so no new login is needed. udev drops the entry when it makes the device again; the
group membership keeps working from then on. Only an administrator can run it; the app shows the question when `/dev/kvm` exists but
can't be opened.

### 3.5 Files on disk

```
~/Machines/<name>.lordvm/
  machine.json       settings, schema-versioned
  disks/system.qcow2
  disks/data-1.qcow2
  nvram.fd           UEFI variables
  tpm/               swtpm state
  thumbnails/        last picture
  snapshots.json     names and notes; the snapshots themselves are in the qcow2
  log/               last three runs
```

Phase 1 keeps the NVRAM as `disks/nvram.qcow2` and adds `lock` and `run.json` (3.9), because QEMU may write only in `disks/`.
Files shows the folder as one item. It moves to another LordOS computer only under the conditions of 3.6. `machine.json` never holds passwords.
Keys go to the keyring.

### 3.6 Contracts and compatibility

What a machine promises, so that updates don't break it.

| Item | Rule |
|---|---|
| `machine.json` | `schema` number; migrated forward with a copy kept; a newer file is opened read-only, never rewritten |
| Identity | A UUID made once and passed with `-uuid`; a Clone gets a new one, a Duplicate keeps it |
| Machine type | Pinned at creation to a versioned type (`q35` or `virt` with its QEMU version) and kept across updates, so that the guest sees the same hardware. Moving to a newer one is an explicit action |
| Firmware | The firmware's identity is recorded with the NVRAM, which only works with a matching build. A firmware update must not orphan it: the old code file is kept in the machine's folder |
| External dependencies | Listed in `machine.json`: a shared base (6.3), an attached ISO, a shared folder path, a USB rule, a bridge. A missing one is named at start, and the machine isn't touched |
| Suspended state | Valid only with the QEMU build, machine type and CPU that wrote it. The check runs before loading; on mismatch the choice is shown: try, or discard the state and boot from disk |
| Interrupted operations | Each multi-step operation (clone, snapshot, revert, import, compaction, disk resize) writes an intent to `operation.json` first, and works on new files that replace old ones by rename. At next open, `lord-vm` finishes or rolls back and says which |

**Moving a machine.** A `.lordvm` is portable only if it's shut down, with complete disks and no external dependency; a clone over a shared base (6.3) isn't, until Export flattens it. Export checks this, and an import verifies a manifest of checksums. A suspended or running machine doesn't move, and a copy made by hand in Files is offered as a Duplicate.

### 3.7 Resource budget

Limits per machine aren't enough: three reasonable machines can starve the desktop. All machines sit under one parent cgroup slice, with a memory ceiling for the sum, lower CPU and I/O weight than the session, and a floor kept for the desktop: **target** 2 GB, or 1 GB on a 4 GB board. Starting a machine that doesn't fit asks first: pause another, start with less, or cancel. Memory pressure (PSI) warns before swap does, and the summary header shows the budget, not only host use. Whether `lord-sessiond`'s scopes can nest under one slice is **To test** in Phase 1.

### 3.8 Why direct QMP, and not libvirt

Decided on 2026-11-01: `lord-vm` drives QEMU directly through QMP.

libvirt was considered seriously: its session mode runs as the user, Debian 13 has 11.3.0 (**Measured**), it supports `<graphics type='dbus' p2p='yes'>` through `virDomainOpenGraphics` (**Reported**, its documentation), and it brings machine-type and firmware handling, plus snapshots with years of edge cases. Against it: a second daemon in the session, XML as the contract, and its own process setup between us and QEMU, right where our sandbox has to be.

It stays only as a fallback, if the technical proof finds a blocker in direct QMP. Its internal snapshot code is worth reading in any case.

### 3.9 Phase 1 implementation

Built on branch `machines-alpha` (2026-11-04/05) in two parts: the internals, then the window and the app (The app, below). Sources in
`server/src/vm/`, rules in `server/mk/vm.mk`, both binaries installed in `/usr/lib/lordos/bin` by `tools/iso/mkdeb.sh`
(QEMU and passt are not package dependencies).

**Files of one machine** (any folder; the app will use `~/Machines/<name>.lordvm`, 3.5). QEMU may write only in `disks/`, so a
compromised QEMU can't change `machine.json` (and through it, what the next start lets it read), the logs or the state:

| File | What |
|---|---|
| `machine.json` | The description, schema 1 (`machine.h`): `schema`, `id` (UUID, QEMU's `-uuid`), `name`, `arch` (`arm64`, `amd64`), `accel` (`kvm`, `tcg`), `vcpus`, `memory_mb`, `disk_gb`, `firmware` (`uefi`, `bios`), `machine_type` (pinned at creation, `virt-10.0` or `pc-q35-10.0`, read from QEMU), `firmware_code` and `firmware_sha256` (the UEFI build the NVRAM was made with; a changed build is logged at start), `display` (`window`, `none`), `network` (`shared`, `isolated`), `mac`, optional `iso`, `created`, `updated`. Unknown keys, wrong types and values out of range are refused; a newer schema is refused before its other keys are read and is never rewritten. No password is ever stored |
| `disks/system.qcow2` | System disk, made by `qemu-img` at creation (empty, or converted from an image with `--import`) |
| `disks/nvram.qcow2` | UEFI variables as qcow2, converted from the firmware's template (6.2). arm64 always uses UEFI (`AAVMF_CODE.no-secboot.fd`); amd64 uses UEFI (`OVMF_CODE_4M.fd`) when the `ovmf` package is there and SeaBIOS otherwise, which is what a Pi gets for an emulated amd64 guest |
| `log/lord-vm.log` | This run: lord-vm's lines, QEMU's and passt's stderr, the command line (credentials named, never their values); 4 MB at most. `lord-vm.1.log` and `lord-vm.2.log` are the two runs before (6.19) |
| `log/serial.log` | The guest's serial console, 1 MB at most per run (`serial.1.log`, the run before) |
| `lock` | `flock`ed by the running lord-vm: a second one on the same machine is refused |
| `run.json` | The last state lord-vm wrote: state, reason, code, pids, time, detail. Status and recovery read it |

**Limits** (3.7): vCPUs at most the host's CPUs, with a warning above all-but-one (3 on a Pi 5, 13.7); memory 256 MB up to the
host's RAM minus 2 GB (1 GB on a 4 GB board). The machine's memory limit is its memory plus QEMU's overhead (384 MB with KVM, 640
MB with TCG, whose translation cache is capped with `tb-size=256`); `vm_budget_fits` checks a new machine against the ones
running. Enforcing the sum needs one slice for all machines, which lord-sessiond can't make yet (below).

**Processes** of a running machine:

```
lord-server ──fd 3, VmMsg──▶ lord-vm (app-lordos-lordos-vm-<id>.scope)      ← lord-vm --control-fd 3 [--paused] DIR
                               ├─ passt   -f -q -1 --fd 3 --no-map-gw -t none -u none --dns-forward 169.254.0.53
                               └─ qemu-system-<arch>   fds: 3 /dev/kvm, 4 QMP, 5 serial, 6 network, 7… credentials
lord-server ──fd 3, VdMsg──▶ lord-vmdisplay <id> (lordos-vmdisplay-<id>.scope) ──fd 4, D-Bus p2p──▶ QEMU
```

- **lord-vm** loads and checks `machine.json` and this computer, takes the lock, recovers a previous run that ended without a
  final state (kills what is left of it, marks it `supervisor-lost`, removes its socket), rotates the logs, enters the machine's
  scope, opens `/dev/kvm`, starts passt and QEMU paused (`-S`), completes the QMP handshake, applies its own Landlock (read and
  write in the machine's folder only) and continues QEMU unless asked to stay paused. It listens on
  `$XDG_RUNTIME_DIR/lordos-vm/<id>.sock` (folder 0700, peer uid checked) with the same messages as fd 3, so the CLI and a restarted
  lord-server can reach it. When lord-server closes fd 3, the machine is shut down (ACPI, 60 s, then off), unless lord-vm was
  started with `--keep-running`. lord-vm exits when QEMU has, after the last state.
- **QEMU** gets `-nodefaults -no-user-config`, `-uuid`, the pinned machine type with `dump-guest-core=off`, `-accel kvm,device=/dev/fdset/1`
  with `-add-fd` (or `tcg,thread=multi,tb-size=256`), `-cpu host` (`max` with TCG), every disk as `-blockdev` JSON with
  `bootindex=0` on the system disk, an ISO as `scsi-cd` with `bootindex=1`, `virtio-net-pci` on `-netdev stream,addr.type=fd`, `virtio-rng-pci`,
  `virtio-vga` on amd64 always (3.3) and `virtio-gpu-pci` + `virtio-keyboard-pci` on arm64, `virtio-tablet-pci` with a window,
  `-display dbus,p2p=yes` (or `none`), QMP and the serial port as `-chardev socket,fd=N`, and
  `-sandbox on,obsolete=deny,elevateprivileges=deny,spawn=deny,resourcecontrol=deny`. Its environment is `LC_ALL=C.UTF-8` only.
  `PR_SET_PDEATHSIG` (SIGKILL) and its own process group are set before exec, and core dumps are off.
- **Guest credentials** (systemd's first-boot settings, a root password for tests) come from `--credentials-file` (mode 0600),
  never from a command line: lord-vm puts each in a sealed memfd and QEMU reads it as `-smbios type=11,path=/proc/self/fd/N`. The
  log names them only; the isolation test checks that the password isn't on QEMU's command line or in any log.
- **lord-vmdisplay** reads the console, registers its listener (with `Listener.Unix.Map`), then applies the helper sandbox of
  `sec/sandbox.c` (no files, no new sockets, no exec, threads only, its own scope with 512 MB) before it handles anything about the
  picture. Decisions on 13.8 item 4: QEMU's memfd is sealed only against new seals (`seal_shrink: no`, again on both hosts), so
  refusing unsealed buffers would mean no picture: every read of QEMU's buffer runs under a `SIGBUS` guard instead, and a fault drops
  that buffer until the next `ScanoutMap`. The damaged area is copied into lord-server's picture (sealed by lord-server against
  shrinking and growing), at most `VD_RATE` times a second (60 by default; lord-server sends the display's real rate), and not again
  until lord-server acknowledges the frame or 250 ms pass, so lord-server never reads a picture while it's written and never sees
  QEMU's memory. Plain `Scanout`/`Update` arrays are still copied (QEMU uses them for surfaces it can't share); `ScanoutDMABUF` is
  refused in the alpha (no 3D). Input goes out as D-Bus calls that expect no reply: evdev key codes mapped to QEMU's numbers
  (`vmkeys.h`), absolute pointer (relative while the guest's tablet driver isn't up; `IsAbsolute` is asked again while the pointer
  moves), buttons, wheel steps, release-all on focus loss, and `SetUIInfo` at most every 100 ms for the window's size. The cursor
  (`CursorDefine`) goes to lord-server in its own sealed memfd.

**Messages** (`server/src/vm/vmproto.h`, the full contract is its header comment). Both channels are `SOCK_SEQPACKET`, one fixed
struct per packet, at most one descriptor per message; anything of another length or version drops the peer.

| `VmMsg` (224 bytes) | Direction | Meaning |
|---|---|---|
| `VM_STATUS`, `VM_PAUSE`, `VM_RESUME`, `VM_SHUTDOWN` (a = seconds, 0: 60), `VM_POWEROFF`, `VM_RESET` | to lord-vm | Lifecycle (6.1); each answered by `VM_REPLY` with the request's `seq` |
| `VM_DISPLAY` | to lord-vm | QMP `getfd` + `add_client @dbus-display`; answered by `VM_DISPLAY_FD` carrying the connection for lord-vmdisplay |
| `VM_QMP` | to lord-vm | One raw QMP command, only with `lord-vm --dev-qmp` (tests) |
| `VM_REPLY` | from lord-vm | a = `VME_*`, text = reason |
| `VM_STATE` | from lord-vm | a = `stopped`, `starting`, `running`, `paused`, `stopping`, `crashed`; b = why (`guest-shutdown`, `host-shutdown`, `forced`, `poweroff`, `qemu-failed`, `qemu-signal`, `out-of-memory`, `start-failed`, `guest-panic`, `supervisor-lost`, `reset`, `user`); c = exit status or signal; d = QEMU's pid; text = detail for the log and the likely-cause line of 6.1 |
| `VM_INFO` | from lord-vm | vCPUs, memory, `VMS_*` (what the sandbox applied), `VMC_*` (cgroups), the id |

| `VdMsg` (32 bytes) | Direction | Meaning |
|---|---|---|
| `VD_READY`, `VD_STATE`, `VD_STATS` | from lord-vmdisplay | Console size; connected / no picture / lost; counts every 5 s |
| `VD_SIZE` → `VD_SHM` | both | The helper asks for a picture of w × h; lord-server answers with a memfd of w × h × 4 sealed against shrinking and growing (`vm_picture_memfd`) |
| `VD_FRAME` → `VD_FRAME_DONE` | both | Damage x, y, w, h and a frame number; lord-server answers after it read the picture |
| `VD_CURSOR`, `VD_POINTER` | from lord-vmdisplay | Cursor image in a memfd sealed against writing (`vm_cursor_check`), hotspot; pointer position |
| `VD_KEY`, `VD_MOTION`, `VD_BUTTON`, `VD_WHEEL`, `VD_RELEASE_ALL`, `VD_RESIZE`, `VD_RATE`, `VD_CLOSE` | to lord-vmdisplay | Input and window changes |

**Command line, for development and tests:**

```
lord-vm --create DIR [--name N] [--arch arm64|amd64] [--accel kvm|tcg] [--vcpus N] [--memory MB] [--disk GB]
                     [--firmware uefi|bios] [--display window|none] [--network shared|isolated] [--import IMAGE] [--iso FILE]
lord-vm --start DIR [--paused] [--detach] [--keep-running] [--credentials-file FILE] [--dev-qmp]
lord-vm --status DIR [--json] | --check DIR | --stop DIR [--timeout S] | --poweroff DIR | --pause DIR | --resume DIR | --reset DIR
lordvm-dev run|attach DIR      plays lord-server: picture memfd, frames, PNG, typing, pointer, resize (server/tests/vm_dev.c)
```

**What is enforced, and where it stops.**

- QEMU: Landlock (read and execute `/usr`; read `/etc/ld.so.cache` and its own `/proc/<pid>`; read and write `disks/`; read the ISO;
  `/dev/null`; TCP bind and connect refused; abstract unix sockets and signals scoped), `no_new_privs`, our seccomp filter (no
  `socket`, `connect`, `bind`, `listen`, `accept`, namespaces, mount, `ptrace`, `process_vm_*`, `bpf`, `perf_event_open`, `keyctl`,
  `io_uring`, `userfaultfd`, modules, kexec, `TIOCSTI`), then QEMU's own filter. Ours is a denylist: an allowlist for QEMU is still
  **To test** (3.3).
- passt sandboxes itself (namespaces and its own seccomp); lord-vm doesn't put Landlock on it, because Landlock would forbid the
  mounts passt's own isolation makes.
- cgroups: lord-vm enters `app-lordos-lordos-vm-<id>.scope` through lord-sessiond (memory.max = the machine's limit + 128 MB, cpu.max
  = vCPUs × 100 % + 50 %). With lord-sessiond, scopes belong to root, so lord-vm can't divide its scope: QEMU and passt share it.
  When lord-vm runs in a cgroup it may divide (a delegated scope), it makes leaves `supervisor`, `qemu` (the machine's memory limit,
  swap 0, its CPU limit) and `passt` (128 MB, 200 %); the test below runs that way. One slice for all machines, the sum of 3.7,
  needs lord-sessiond to create nested scopes: not done.
- lord-vm's own Landlock covers files only; it keeps the right to signal QEMU and passt.

**Isolation test** (`server/tests/vm_isolation.sh BUILD_DIR IMAGE`, Phase 1's "done when"). It makes a machine from a Debian 13
image, starts it through `lordvm-dev` (lord-server's part), and has QEMU itself try what it must not do, through
`lord-vm --dev-qmp`: `blockdev-add` of files and devices, `screendump`, file, TCP, UDP and unix `chardev-add`, a user network with a
forwarded port. The guest reports its own network on the kernel log through a unit passed as a systemd credential. Then pause,
resume, a second lord-vm on the same machine, QEMU killed with `SIGSEGV`, lord-vm killed with `SIGKILL`, recovery, and an ACPI
shutdown. With `LORDVM_SCOPE=1` each lord-vm runs alone in a delegated scope (`systemd-run --user --scope -p Delegate=yes`), so the
nested leaves are tested. **Measured** on 2026-11-05, final runs:

| | Pi 5 (arm64 guest, KVM) | amd64, **nested** (amd64 guest, KVM, OVMF) |
|---|---|---|
| Result | **61 passed, 0 failed, 0 skipped** | **59 passed, 0 failed, 0 skipped** |
| QEMU's sandbox | Landlock (files, TCP, scope), our seccomp, QEMU's seccomp (`Seccomp_filters` ≥ 2), `NoNewPrivs` 1, `CapEff` 0 | same |
| QEMU's descriptors | only sockets, pipes, anonymous files, `/dev/kvm`, `/dev/null`, its disks, the firmware | same |
| Refused to QEMU | `/etc/hostname`, a file in the home folder, its own `machine.json` and logs, `/proc/<lord-vm>/environ`, a screenshot or file chardev in `/tmp`, `/dev/mem`, `/dev/mmcblk0`, `/dev/dri/renderD128`, `/dev/dri/card0`, `/dev/net/tun`, `/dev/vhost-net`, `/dev/tty`, `/dev/ptmx`; TCP connect and listen, UDP, a unix socket, the session bus, a user network with a forwarded port; nothing reached the host's listeners | same, with `/dev/nvme0n1` |
| Allowed (control) | a file in `disks/` | same |
| Guest network (passt) | address and gateway copied from the host, DNS answered, `http://1.1.1.1/` 301, `http://deb.debian.org/` 200, the host's loopback through the gateway unreachable | same |
| cgroups | QEMU in its `qemu` leaf, `memory.max` 1,476,395,008 (1024 + 384 MB); passt beside it, listening on nothing | same, in the scope lord-vm made for itself |
| Display | first picture, not black, typed login and command ran (`echo` reached the serial line), `SetUIInfo` 1024 × 768 taken by the guest, nothing refused on either side | same |
| lord-vmdisplay | seccomp, `NoNewPrivs`, no file held; its sandbox refuses `/etc`, home, `/dev/kvm`, files in `/tmp`, AF_INET, AF_UNIX, `socketpair`, `fork`; `/dev/null` and `memfd` stay | same |
| Secrets | the root password from `--credentials-file` was on no command line and in no log | same |
| Lifecycle | pause, resume, second start refused (exit 3), QEMU `SIGSEGV` → `crashed`/`qemu-signal` 11 with passt and the socket gone, lord-vm `SIGKILL` → QEMU and passt gone, status `supervisor-lost`, the next start recovers, ACPI shutdown in 4 s | same, ACPI shutdown in 4 s |

Earlier runs on the same day found, and the code now handles: on arm64, QEMU opens `/dev/kvm` again by path for `-cpu host` (a
scratch vCPU to read the CPU's features), so KVM machines have `/dev/kvm` in QEMU's Landlock rules (it holds a KVM descriptor anyway);
`--dns` together with `--dns-forward` made passt advertise an address it didn't answer on; an ACPI request sent before a nested
amd64 guest finished booting is ignored and ends as `forced` after the timeout, which is the designed fallback. Without a delegated
cgroup (a plain SSH session), the same checks pass and QEMU runs with no limits, which `VM_INFO` reports.

The `machine.json` schema, the message checks, QMP line reading, the key map and the image checks have unit tests (`server/tests/vm_test.c`, 167 checks,
run by `make test`). lord-vmdisplay and lordvm-dev build only where `libsystemd-dev` is installed (`VMDISPLAY ?=` in `mk/vm.mk`).

**Import never follows another file.** A qcow2 can name a backing file or an external data file, and an import that followed it
would copy that file (anything the person can read: keys, other people's shared files) into the new disk, where the guest reads it;
a raw file can also hold a qcow2 header. So lord-vm decides the format itself from the first bytes and the name (`vm_image_kind`,
`machine.c`): the qcow2 magic with version 2 or 3, no backing file, no external data file and not marked corrupt, or a `.img`/`.raw`
name without the magic; anything else is refused. `qemu-img` then sees the image only as a `json:` filename that names the driver
(never probed) with `"backing": null`, and runs in a jail like QEMU's (`vm_jail_tool`: `no_new_privs`, the same seccomp filter, no TCP,
Landlock with read access to that one file and write access to the machine's `disks/` only); its `info` answer is refused if it
mentions a backing file or a data file anyway. At every start the system disk's and the NVRAM's headers are checked the same way
(`vm_qcow2_plain`), and every qcow2 `-blockdev` carries `"backing": null`. `server/tests/vm_import_test.sh` (run by `make test` where
`qemu-img` is installed) makes a backing file, an external data file, a raw file named `.qcow2`, a backing file under a `.img` name, a
header patched by hand and a system disk swapped after creation: all refused, and good qcow2 and raw imports still work (10 checks,
amd64 build VM, 2026-11-05). Each layer was also checked alone: `backing: null` keeps the backing file's bytes out of a convert, and the
jail alone makes `qemu-img` fail to open the backing file.

**The app** (part 2, 2026-11-05). `lord.vm` (`server/src/ui/js_vm.c`) plays the part `vm_dev.c` plays in the tests, inside
lord-server: it lists `~/Machines/*.lordvm` (machine.json and run.json; a run.json that says active with no `lord-vm` behind its pid
shows as *Off (recovered)*, and lord-vm recovers it at the next start), runs `lord-vm --create` in the background, starts a machine
on a control socketpair (`--control-fd 3 --paused`, no cgroup step in the child), attaches to a running one through its runtime socket,
asks for the display once it has VM_INFO and a running or paused state, starts lord-vmdisplay with fd 3 and fd 4, resumes, maps each
VD_SIZE picture read-only into an image media node, acknowledges a frame only when the engine painted it (`MediaSrc.painted_seq`, set
by `media_paint`; a hidden window never paints and the helper goes on after 250 ms), gives the guest's pointer (below) to that media node, forwards evdev keys, picture-coordinate motion, buttons, wheel steps, release-all on
blur, resize after the window kept a size for 400 ms, and the display's refresh rate. At most 256 messages per channel per poll; no
call blocks. lord-vm's stderr goes to a non-blocking pipe, so a window that stops polling can't stall the supervisor; when lord-vm
ends without a last state the reason is *start-failed* (it never ran) or *supervisor-lost*.

First start of cloud images: Debian's `nocloud` image stops at systemd's first-boot questions on its serial line, where nobody sees
them. lord-server passes, at every start, this computer's language, keyboard layout and time zone as systemd credentials (5.2, "copied
from the host"; a 0600 file in the runtime folder, removed at lord-vm's first message), and, when the New sheet was given a root
password, its SHA-512 crypt hash, which lives in the machine's `firstboot.creds` (0600) until the machine's first clean shutdown. The
password itself is never written anywhere.

The Machines app (`apps/machines/`, marked **Alpha**) has a library window and one window per running machine:

- Library: name, architecture (and *Emulated*), vCPUs, memory, disk, state with a dot and words, the reason after a failure with
  *Show log*; Start or Open, Stop (Shut Down, Force Off, through the runtime socket), Delete (only when off, after a question, to the
  Trash), Eject ISO (only when off, for a machine that still has one). A banner lists what is missing and installs it with the closed admin action `vm-enable` (QEMU for this architecture, its
  UEFI firmware, `qemu-system-modules-opengl` for the D-Bus display, `qemu-utils`, `passt`, through LordOS's package engine, as
  Settings does for optional VPN programs; a stale index is read again once), and offers `vm-kvm` (3.4) when `/dev/kvm` exists but
  this account can't open it.
- New Machine sheet: name, architecture (this computer's by default; the other marked *Emulated, much slower*), vCPUs (the
  recommended number by default, a warning above it), memory (up to RAM minus the desktop's floor), disk, and the system: *Import a
  disk image (.qcow2)* or *Boot from an ISO*, chosen in the system file dialog; an optional root password for the first start.
- Machine window: the picture scaled to fit and kept in shape, input while in front, a bar with the state, Pause/Resume, Restart,
  Ctrl+Alt+Delete, Full screen and Power (Shut Down, Force Off). Closing the window shuts the machine down first (a *Shutting down*
  card with Force Off); a failure shows the reason, a likely fix and the end of the log, with Start.

**On screen** (VM 125: LordOS 0.8.8.3 amd64, nested VT-x, 4 cores, 3.8 GB, no QEMU at the start), 2026-11-05: the banner installed
the packages in about 15 s and *Allow* gave `alex` the kvm group and an access-list entry on `/dev/kvm` with no new login; Debian 13's
official nocloud qcow2 (SHA512 checked) was imported (2 vCPU, 1 GB, 20 GB) and started with KVM; firmware, kernel and login appeared in
the window, the guest followed the window's size, the root login and a shell worked from the host keyboard (`nproc` 2, 961 MB,
the Shared network's address); Pause, Resume, Ctrl+Alt+Delete (the guest rebooted), Shut Down from the library (`host-shutdown`),
Force Off, close-to-shut-down, QEMU killed with SIGSEGV (*Stopped with an error*, Start again), lord-vm killed with SIGKILL (the
window and the library say so, the next start logs the recovery) and Delete to the Trash all worked. Screenshots are kept with the
branch's test notes.

**Full screen was black** (found 2026-11-05): in a window the guest follows the window's size, so its picture is copied as is; in full
screen it's drawn scaled (a guest text console can't grow past the framebuffer it booted with: 800 × 600 on a 1280 × 800 screen), through
the media node's scaled copy (`mview.c`). Replacing the picture (`set_picture` in `js_vm.c`, the same in `js_rdesk.c`) freed that copy
but left its old size, and `media_paint` took a size that fits as proof that the copy was there: no copy was made again and the node
drew nothing, in both compositions (media nodes are painted by the CPU into the UI). Now a missing copy is always made again, and
whoever frees it clears its size (`media.c`, `js_vm.c`, `js_rdesk.c`); `tests/mview_test.c` (run by `make test`) failed 3 of 7 checks
before. Remote Desk had the same bug for a remote screen that changes size while shown scaled. Seen on VM 125 (CPU composition): the
guest's console in full screen, scaled to 1067 × 800 with black bands.

**The guest's pointer** (2026-11-05). VD_CURSOR's memfd (checked by `vm_cursor_check`) is copied and handed to the picture's media
source (`media_src_cursor`); `mcursor.c` premultiplies it, cuts it to what isn't fully transparent (keeping the hotspot inside) and scales
it to the size the picture is shown at (the factor `media_paint` used, at most 64 × 64 for the cursor plane, drawn as is within 5 %).
main.c remembers which media node the pointer is over (`over_media`, from the same hit test that picks the CSS cursor) and `cursor_now`
shows the guest's image there instead of the host's arrow, in the CPU composition, the GPU's texture and the cursor plane alike. Where
the guest puts its pointer (VD_POINTER's position) isn't used: the host's pointer drives the absolute tablet, so the image is drawn at
the host's pointer. VD_POINTER's *hidden* (the guest turned its cursor plane off: a text console, or X just quit) brings the host's arrow
back, so there's always a pointer to aim with; a fully transparent image (an app hiding it on purpose) hides it. A new display helper
starts with the host's arrow until QEMU defines the cursor again. `tests/mcursor_test.c` (21 checks). Seen on VM 125: the installed
Debian 13 guest (below) running X with an xterm showed X's I-beam over the xterm and its X-shaped root cursor elsewhere, with the hotspot
at the host's pointer, the host's arrow over the bar and outside the window, the cursor drawn larger in full screen (the picture scaled up),
and the host's arrow again on the text console after X quit.

**Installing from an ISO** (2026-11-05, VM 125). Debian 13.7.0's official amd64 netinst ISO (its SHA512 matched the official
`SHA512SUMS`) went into a new machine through *Boot from an ISO* (2 vCPU, 1 GB, an empty 20 GB disk): the firmware started the ISO's UEFI
menu, the graphical installer ran with the host's mouse and keyboard (language, location, keyboard, network by DHCP from passt, users,
guided partitioning of the whole disk: ESP, ext4, swap, base system, mirror, software selection) and finished in about 15 minutes. Its
*Continue* rebooted into the installed system with the ISO still attached: the system disk has `bootindex=0`, so the firmware started the
installed GRUB (the installer's boot entry in the NVRAM qcow2 and the disk's ESP), and the root login worked. Found and fixed:

- **The network card moved when the ISO was taken out.** QEMU numbered the PCI slots in the order of the devices, so the ISO's SCSI
  controller pushed the network card from slot 2 to slot 3: the installer configured `enp0s3`, and after the ISO was gone the card was
  `enp0s2` with nothing configured for it. Every device now has a fixed slot (`lordvm.c`): the system disk 1, the network card 2, the
  random-number device 3, the display 4, then the keyboard and tablet, and the ISO's controller 8; slots 1 to 6 are the ones a machine
  without an ISO always had, so existing machines keep theirs (an *Isolated* machine's devices after the disk move up once).
- **Taking the ISO out.** A machine that is off and has an ISO shows *ISO attached* and an **Eject ISO** button in the library;
  `lord.vm.eject(dir)` takes the machine's lock (refused while lord-vm holds it) and rewrites `machine.json` without `iso`. After it, the
  installed system started with its network up on `enp0s2` (slot 2) and `apt-get` installed packages.

Seen once, not fixed: passt (Debian 13's `0.0~git20250503.587980c-2+deb13u1`) crashed with SIGSEGV during a large `apt-get` download in
the installed guest, after logging `Flow 26 (FREE): Failed to set SO_PEEK_OFF … Socket operation on non-socket`; lord-vm logged *the
machine's network stopped* and the guest had no network until its next start. QEMU's `-netdev stream` on a descriptor can't reconnect,
so a restarted passt would need the network card to be detached and attached again (to do: a newer passt, or that reattach).

**An emulated machine** (2026-11-05, VM 125, amd64 host). The New sheet's *arm64 · Emulated, much slower* lists what this computer
lacks for it (`qemu-system-arm`, `qemu-efi-aarch64`) and now has its own **Install** button (`vm-enable arm64`; the library's banner only
covers this computer's architecture). That download stalled on VM 125, whose own internet stopped answering at the time (the gateway
answered, the build VM on the same network downloaded the same two packages at once), so they were put in place with `dpkg -i` and the
sheet's Install then said *Installed* and enabled Create. Debian 13's official nocloud arm64 qcow2 (SHA512 checked) was imported as an
arm64 machine (2 vCPU, 1 GB): `machine.json` says `accel: tcg`, `virt-10.0`, AAVMF; the library shows *arm64 · Emulated*; the window
showed the guest's console and login in about 2 minutes (QEMU at about one CPU), `uname -m` said `aarch64`, 2 CPUs, 961 MB, the Shared
network's address; Shut Down from the library stopped it (`host-shutdown`). Its scope's `memory.max` was 1,879,048,192 (1024 + 640 + 128
MB) and `cpu.max` 250000/100000.

**The memory budget at start** (3.7). `vm_budget_fits` had no caller. The window now checks, before it starts a machine, the machines
already running plus this one, each with QEMU's overhead (384 MB with KVM, 640 MB emulated: `memLimit` and `budgetCheck` in `model.js`),
against what this computer lets machines use (RAM minus the desktop's floor); one machine alone may always start, since the sheet keeps
its memory within that. On VM 125 (2,897 MB for machines) with the emulated machine running (1,664 MB), starting the 1 GB KVM machine
(1,408 MB) was refused with *this one needs 1408 MB with QEMU's own share, and 1233 MB are left for machines*.

**Session restore** brought every open machine window back as one more library window (they're `machines#vm-…` windows whose
machine isn't part of the saved state): VM 125 had collected twelve. Machines is now a single-window app (manifest `singleWindow`):
only its library is saved, and a saved list that names it more than once opens it once.

**Not done or not verified:** BIOS machines were built for but not run from the app; the package engine's download of
`vm-enable` waited with no time limit while the network didn't answer (the sheet said *Installing…* until it was closed); ejecting an ISO from a running machine
(QMP `blockdev-open-tray`/`blockdev-remove-medium`) and attaching another one; one slice for all machines and nested
leaves under lord-sessiond; our seccomp allowlist for QEMU; a guest cursor larger than 64 × 64 after scaling is drawn smaller (the
cursor plane's size); HiDPI (the guest is asked for the window's logical size); render node and 3D; latency to the screen; the Pi 5 (arm64) with
the app. Unit tests: `vm_test.c` 167 checks (with `vm_image_kind`), `vm_model_test.mjs` 66 checks (states, actions, the sheet's
checks, letterbox and pointer scaling, guest size, refresh rate, wheel steps).

## 4. Guest systems

### 4.1 Linux

Every current Linux has virtio drivers in its kernel. Cards: Ubuntu, Fedora, Debian, Kali and LordOS, each for the host's architecture, plus **any ISO of that architecture** from a file. arm64 images exist for all of them; on a Pi the UEFI arm64 ISO is the one to use, not the Pi's image.

**Always the newest image of a release.** The catalog doesn't pin a file. An entry names a release, the maker's list that names its images (a `SHA256SUMS` file, a directory listing, a releases file) and a file-name pattern, so "Ubuntu 26.04" keeps working when 26.04.2 appears. The app reads the list, takes the newest matching name, checks that it can really be downloaded (lists sometimes name removed files), and verifies the download against that same list, whose signature it checks with the maker's key where one is published (**To confirm per system**). So the catalog stays small and rarely needs an update, and our own signature covers only the entry: which list and which key are trusted. LordOS itself is a guest too, since we build UEFI ISOs for both architectures.

Automatic install: Debian preseed, Ubuntu autoinstall, Fedora kickstart. The person gives a name and password once, in the assistant;
it reaches the guest through a small seed disk that is deleted after the first boot, and is never stored.

Guest tools come from the guest's own repository: `qemu-guest-agent`, `spice-vdagent`, plus our `lordos-guest` package for what they lack.

### 4.2 Android

The roadmap's Android item is a container. As a VM, an Android guest is possible but rougher:

- LineageOS has `virtio_arm64`, `virtio_arm64only` and `virtio_x86_64` targets for QEMU, not officially supported, maintained by individuals, with no OTA
  (**Reported**, its wiki). A third-party project publishes builds.
- On CPUs without 32-bit at EL0, `virtio_arm64only` is needed.
- We would have to build, sign and host our own image to offer a card, and keep it patched.
- Guest GPU acceleration needs virgl or venus, which the targets use through Mesa. Reported color problems exist with software graphics.
- Google's services can't be included.

**Decision:** Android stays a card with "Coming later" until the container item decides the image; building one image for both is the point. Until then,
"Install from an ISO file" accepts a compatible image.

### 4.3 virtio-win guests

amd64: full path. arm64: **Reported** to work under KVM on Pi 5 with no 3D at all; lighter hardware needs an older release. What's needed:

| Need | amd64 | arm64 |
|---|---|---|
| Firmware | OVMF with secure boot | AAVMF (`qemu-efi-aarch64`, Debian 2025.02) |
| TPM 2.0 | swtpm 0.7.1 | swtpm |
| Disk during install | virtio-win viostor or vioscsi | `viostor/w11/ARM64` on the ISO (**Reported**) |
| Network | NetKVM | NetKVM ARM64 (**Reported**) |
| Display | viogpudo, 2D | viogpudo ARM64, 2D; `ramfb` early in boot (**Reported**) |
| 3D | none usable | none |
| Agents | qemu-ga, SPICE agent | qemu-ga; SPICE agent availability **To test** |

Facts to keep in view:

- arm64 virtio-win guests use KVM's GIC. On the Pi the guest sees GICv2. A project boots it with `-M virt -cpu host`, leaving the GIC
  choice to QEMU (**Measured**, read in its script). Whether current releases keep working on GICv2 is **To test** on every Phase 2
  rebuild, since a system update could end it.
- **virtio-win signing.** Public stable drivers are signed with a vendor certificate plus attestation for current systems, and don't carry the system maker's own certification
  (**Reported**, Fedora and the virtio-win project). Certified builds come only with a paid subscription. The installer may ask to trust the
  publisher: the assistant handles it, and the app says what it did.
- Reported working arm64 ISO: virtio-win-0.1.271. A later prerelease failed to find storage (RHEL-114121, **Reported**). So we **pin
  a tested version per architecture** in the signed catalog, and update it only after the install test passes.
- **Download.** From Fedora's official location, the first time. Checksum pinned in our signed catalog, since no signed checksum from
  upstream was found. Our verification is: catalog signature → pinned SHA-256 → ISO mounted read-only → Authenticode check by the
  guest itself. We don't mirror it unless its license lets us.
- **Licenses.** A guest system's license is the person's matter. The app doesn't ship, download or activate one; the card asks for an ISO
  the person got, and links to the maker's own download. No activation tools, ever. Running a retail license in a VM may be restricted, and
  the app doesn't advise on it.

**Unattended install**, a switch in the assistant, skips Setup and first-run questions: an answer file on a generated disk picks the edition, partitions, creates a local account with the given name and optional password, signs in once and installs drivers and agent. Read from working code in the wild and worth keeping in mind:

- Selecting an edition unattended is usually done with the maker's public generic key, which installs and doesn't activate. Decided: index selection in the answer file is tried first, with no key; only if Setup stops for lack of one does the generated answer file use the maker's public generic key. Both paths are tested per release: **To test**.
- On arm64, a reported working path is to put the installer on an NVMe device with no virtio driver loaded, since Setup can fail when handed drivers it doesn't want, and to install drivers once the system is on disk, from an answer-file step. This is an alternative to mounting virtio-win during install; Phase 3 tests both per release.
- Skipping the network requirement of first-run setup uses the maker's own documented setup switch, which the maker may remove.
- Where firmware lacks secure boot, Setup is told to skip that check. Our amd64 path has real secure boot, so it doesn't need that.

Not feasible: promising this keeps working across future releases of a system we don't control. The switch is marked with the release it was last tested on.

### 4.4 Not offered

Guests whose license restricts them to their maker's hardware. Guests needing a real GPU for their desktop on hardware without passthrough.

## 5. The app

**Alpha (Phase 1):** a minimal library, the New Machine sheet and the machine window, described in 3.9 (The app); everything below
is Phase 2 and later.

Name and icon: a generic name, ours (open decision 1). The icon is a screen holding a smaller rounded tile, on the LordOS shape (radius
0.225). Mocks: https://claude.ai/artifact/WNiELGEazSqnr68SFWuyPz (private until shared).

### 5.1 Control Center (main window)

The library. A **summary header**, then a grid of machines, each with a **live picture** while running and the last picture when off. All in LordOS's own flat, modern style: real data in calm type, no imitation of physical equipment.

**Header:** machines running of total, memory used of host total, free space on the library's volume, and two actions: **Disk** (the Storage window) and **New**.

**Each card shows:**

| Item | Source | Target refresh |
|---|---|---|
| Live picture | Same dmabuf, downscaled by the compositor | 1 fps visible, none hidden |
| Power light | `lord-vm` state | On change |
| Agent light | Agent connection (6.17) | On change |
| Disk light, blinks with I/O | QMP `query-blockstats` | 2 Hz, visible only |
| Name, system | `machine.json`; agent's report when connected | On change |
| What it's doing | State: Installing, Starting, At desktop, Suspended, Shut down, No window | On change |
| CPU and memory meters | The VM's cgroup scope | 1 Hz, visible only |
| Disk space | Storage analyzer (6.16) | On open, then on change |

A light never carries meaning by color alone: each has a tooltip and the state in text.

- Sidebar: All, Running, Linux, Other, Snapshots, Templates. Tags as in Files.
- Each tile: thumbnail, name, system icon, state (Running, Paused, Suspended, Off, Emulated badge), resource line (CPU, RAM) when running. Hover: Start or
  Open, Pause, Suspend, Shut down, ⋯.
- `⋯`: Open, Configure, Clone, Export, Show in Files, Delete (Trash first).
- List view with columns: name, system, architecture, state, CPU, memory, disk, last used.
- Toolbar: New, search, view, global Pause All on low battery.
- Status bar: running machines, memory in use out of host total, free disk.
- Thumbnails come from the same dmabuf, downscaled by the compositor, 1 fps, and stop when the app is hidden.
- Each tile has three small lights, each with a tooltip and a text state for those who can't tell colors apart: power, agent, disk activity. Meters show CPU and memory, plus the disk size.
- The head of the window has host meters for memory and storage; clicking storage opens the Storage window (6.16).
- Empty state with one primary action, "New Machine".

### 5.2 New Machine assistant

Steps, each one screen, with Back always available:

1. **Choose:** cards.
   - Download: Ubuntu, Debian, Fedora, LordOS; Android (later).
   - "Install from an ISO file" (drag a file on it).
   - "Migrate from another PC" (6.11).
   - "Import a machine": see 6.22.
   - "Other architecture…" (behind it, Emulated).
2. **Detect:** reads the ISO's volume label and boot files to name the system and architecture. Wrong architecture: says so, before anything starts.
3. **Profile:** see 5.6.
4. **Name, place, account:** VM name, folder, username, password, time zone and keyboard copied from the host, "Sign in automatically".
5. **Review:** CPU, memory, disk, network, firmware, plus "Customize".
6. **Progress:** download with verified signature, install with live picture and a "Open console" option, drivers, tools, snapshot **Fresh install**.

**Requirements check.** Before anything downloads, one screen lists each part the chosen system needs (engine, TPM, firmware, drivers, install tools), which are present and which aren't, and installs what's missing through the Store in one click. Nothing comes from outside the signed repositories, except the pinned driver ISO.

**Automatic setup for virtio-win guests.** A switch, off unless chosen: an answer file on a small generated disk installs the system with no questions and creates a local account, with the name and optional password given here. Debian has `wimtools` (**Measured**) for building a boot disk that starts faster than the original ISO. No product key is added and nothing gets activated: the system stays as its own rules leave it, and the screen says so. Install time is shown as an estimate only after we've measured it.

Unattended where the guest supports it, walked through otherwise: the assistant mounts virtio-win during install and installs tools at the end.

### 5.3 The VM window

A normal LordOS window, with a bar:

| Group | Items |
|---|---|
| Machine | Pause, Suspend, Restart, Shut down, Force off, Take Snapshot, Send keys (Ctrl+Alt+Del, Print, F-keys) |
| Devices | USB (list, attach), CD/DVD (image, eject), Folders, Network (on/off, mode), Sound (mute, output), Camera, Microphone |
| View | Window, Full Screen, Apps, Picture in Picture, Scale, Resolution, Actual Size |
| Info | Live CPU, memory, disk and network per guest; Open Configuration |

Quick actions stay on the bar itself, icons with tooltips, in this order: **agent state** (6.17), **Shell** (6.18), **Logs** (6.19), **Screenshot**, **Record** (6.20), **Capture keyboard and mouse**, **Pause**, **Power**. Power opens a small menu: Shut down, Restart, Suspend, Force off. Anything that needs the agent is dimmed while it's away, and its tooltip says why.

- **Keyboard capture.** Clicking in captures; a chosen combination releases it, shown once on first use. With guest tools the pointer moves freely and no capture is needed.
- Bar auto-hides in full screen and returns at the top edge, as in Remote Desk, with the same behaviour.
- Status dot for each attached device, so that what the guest can use is always visible.
- Window closing asks what to do: Suspend (default), Shut down, Keep Running. **Keep Running** leaves the VM without a window; its tile says "No window" and a click opens it. Each VM's menu also has **Start Without Window** and **Start in Full Screen**.
- With **Keep this computer awake**, the host stays awake while the VM runs in the background, and the agent keeps the guest awake too (`systemd-inhibit` in Linux). The display may still turn off.
- The window opens at the size of the guest's display and keeps its shape when resized, unless the resolution follows the window.
- A **resolution menu** on the bar lists what the guest's display offers. It's hidden while the resolution follows the window, since resizing already changes it.

### 5.4 Display modes

| Mode | What it does | Needs |
|---|---|---|
| **Window** | The guest's desktop in a window; the guest's resolution follows the window | Guest agent or virtio-gpu resize |
| **Full Screen** | Takes one display, its own desktop | Nothing |
| **All Displays** | One head per display | virtio-gpu multi-head |
| **Apps** (seamless) | Guest apps as LordOS windows, no guest desktop | See 7.2 |
| **Picture in Picture** | Small floating view, input optional; live control off by default | Our `setPip` |
| **Thumbnail** | Paused at last frame, one click to resume | Suspend |

### 5.5 Configuration (one screen)

**Decision: one window with every section down the side, each with a one-line summary of its current values, and no tabs.** The roadmap says tabs. Its five groups stay, as headings in that side list, because they're a good order, but tabs lose:

- With tabs you have to open each to see what a machine has. A summary list answers "what is this machine?" at a glance: "3 cores, 8 GB", "Shared, 1 forwarded port", "1 folder, read-only".
- Tabs plus sub-sections are two levels of navigation; this is one.
- Search filters the same list, and the changed sections show a dot.
- It works in a narrow window, where the list becomes the first page. A tab row doesn't fit.
- It reads like Settings, so nothing new to learn.

Summaries look like: General "Debian 13, arm64"; CPU and memory "3 cores, 8 GB"; Display "Follows window"; Network "Shared, 1 forwarded port"; Sharing "1 folder, clipboard both ways"; Disks "1 disk, 64 GB"; Security "Sandbox on"; Backup "Weekly, last Sunday".

Reachable from the bar and from `⋯`. Changes that need a power-off are marked and applied at the next boot.

**General** (summary: system, architecture). Name, icon, description, tags, system, architecture (read only), folder, created, disk used, Reclaim space.

**Options:**
- Startup and shutdown: start with session, delay, on window close, on host shutdown (Suspend, Shut down).
- Optimization: Faster guest / Balanced / Faster host; battery; background priority; hugepages; memory ballooning; auto-pause when idle.
- Sharing: folders (per folder read-only by default), Downloads, Desktop; clipboard (Off, To guest, From guest, Both); drag and drop; printers.
- Applications: guest apps in the launcher and Dock; file associations.
- Apps mode: on or off.

**Hardware:**
- CPU and memory: vCPUs (slider with a marked recommended zone), memory, hotplug where the guest supports it; CPU model (host, compatible).
- Graphics: 2D, 3D (virgl), 3D (Vulkan, venus), video memory, heads, scale.
- Mouse and keyboard: pointer sync, keyboard layout, key mapping, shortcut passing.
- Network: adapters, mode, MAC, port forwarding, speed limit.
- Sound: output, input, device model.
- USB: controller version, auto-connect rules.
- Disks: add, resize, bus, cache, discard, read-only, encrypted; CD/DVD.
- Printers: shared host printers, driver-free IPP.
- TPM: on or off, reset state.
- Boot: firmware (UEFI, BIOS on amd64), secure boot, boot order, boot menu.
- Serial and console: text console (useful on servers).

**Security:** isolation level shown (Landlock, seccomp, namespaces); clipboard and folders summary; encrypted disks; password to open; network isolation ("Only this computer", "Nothing"); snapshot lock; **Guest can see**: a plain list of what it has access to.

**Backup:** schedule, destination, keep N, consistent via agent, restore, exclude temp disks, ties in Backups app.

### 5.6 Profiles

Chosen when creating, changeable later. A profile only sets values shown in Configuration: nothing hidden. Numbers scale with host.

| Profile | vCPU (of host) | Memory | Graphics | Other |
|---|---|---|---|---|
| Productivity | half | 8 GB or 1/3 | 2D | Asks about folders and clipboard, Apps mode, suspend on close |
| Software Development | three quarters | 16 GB or 1/2 | 2D | Port forwarding UI, serial console, nested if available, snapshots on |
| Testing | half | 4 GB | 2D | Isolated network, snapshot before each boot, nothing shared, disposable mode |
| Design | three quarters | 16 GB or 1/2 | 3D | Color-accurate full screen, tablet pointer |
| Games | all but one | 2/3 | 3D (venus) | Hugepages, Faster guest, capture on, low-latency audio |

Only what the host can do appears: a Pi has no Games profile, shows a "Light desktop" one instead, and a virtio-win guest has 2D only so Design and Games say so.

**No profile shares anything by itself.** Productivity adds a question to the Review step, with its switches off: share Downloads read-only, share the clipboard, and in which direction. Testing starts on the Isolated network with the agent's shell off.

### 5.7 What this computer can run

A screen in the app's Help and first run lists, per host, what's native, slow, limited or impossible, in the words of section 2 and with real values for this board: cores, memory, disk type. On a Pi it also warns about microSD and shows temperature, fan and voltage while machines run. It's generated from what the host reports, so it never claims more than the hardware gives.

### 5.8 Menus, shortcuts

Follows `docs/SHORTCUTS.md`: app menus, ❖ shortcuts for the host while the app has focus outside capture, all passed to the guest while captured. The release combination is shown in the bar and is configurable. In Lord Classic the same table.

## 6. Features

### 6.1 Lifecycle

Start, pause (vCPUs stop, instant), suspend (RAM to the folder, survives a host reboot), shut down (ACPI, then agent), force off. Host shutdown asks each running VM to suspend. A machine that fails or crashes shows the machine log, a likely cause when one is known (not enough memory, a missing disk, a locked file) and a fix that fits it, such as starting with less memory or going back to a snapshot. Devices that can't be saved (sound, for example) are disconnected on suspend and connected again on resume. A suspended VM may not resume after QEMU is updated: the Store warns before such an update, and the VM's state says so instead of failing silently.

### 6.2 Snapshots

qcow2 internal snapshots, with RAM optional. Timeline view with names, notes, thumbnails and size. Revert asks and offers to snapshot first. Auto-snapshot before a guest's tools upgrade. Weak: internal snapshots slow down with many; keep under 20 with a warning. Snapshots and backups aren't the same: Configuration says so.

**What a snapshot holds.** Without care, "snapshot" means five things: disks, memory, NVRAM, TPM and settings. The model:

- A snapshot covers **all writable disks together**, the NVRAM, the TPM state and a copy of `machine.json`. With memory, it resumes where it was; without, it's as after power loss.
- Taken with QMP `snapshot-save`, listing every writable node in `devices`, as QEMU advises for a consistent snapshot. It's a job: the command returns before it's done, and `lord-vm` follows `JOB_STATUS_CHANGE` and fetches errors (**Reported**, QMP reference; since 6.0). The app shows progress, and the guest may stop during the save.
- **Raw NVRAM blocks internal snapshots:** QEMU refuses with "Device 'pflash1' is writable but does not support snapshots", which is why UEFI machines have long had trouble with them (**Reported**). Our NVRAM is therefore qcow2 from the start. This can't be added later without converting.
- TPM state: swtpm's state goes with the machine's memory state in a live snapshot, as far as we can tell from how it migrates (**To test**). A disk-only snapshot copies the `tpm/` folder with the machine off.
- Without a running guest's cooperation, disks are crash-consistent. With the agent, a filesystem freeze comes first, and the timeline says which it got.
- **Failure midway:** the same `tag` may exist on some disks and not others. `snapshots.json` is written last, so an entry there is proof of a complete snapshot; a leftover tag is removed at next open, and `operation.json` (3.6) names the work.
- Revert uses `snapshot-load`, also a job, with the machine paused, after an automatic safety snapshot if there's room.

**Disposable sessions** use explicit temporary overlays on each disk, deleted at shutdown. QEMU's global `-snapshot` can't be used: its manual says it's incompatible with `-blockdev`, which we use, and it forces unsafe caching (**Reported**). NVRAM and TPM get temporary copies too, or a disposable session would change them. After a crash, the overlays are discarded at next start.

### 6.3 Clones

Two kinds, named for what they do:

- **Clone:** can run beside the original. It gets a new MAC and, through the agent, a new `machine-id` and hostname, which avoids the usual trap of two VMs receiving one DHCP address. Virtio-win guests keep their computer name unless changed.
- **Duplicate:** an exact copy with the same identity, the only kind that can resume a suspended session.

A **linked clone runs alongside its original and takes only what changed**:

- On btrfs and XFS, a reflink copy: instant, shared until changed, and both are independent.
- On ext4, which has no reflink, the disk becomes a frozen read-only base, and both machines get a thin qcow2 overlay on it. A running source can do this live with QEMU's `blockdev-snapshot`, **To test**. Both then run together. The base lives in `~/Machines/.shared/` and is deleted when nothing uses it. Storage shows it.

A machine with a saved session can't be cloned until it's resumed and shut down, since a RAM image holds its identity.

The app picks by file system and says which one it used. Templates are read-only machines made to clone.

### 6.4 Storage

- **qcow2** by default: thin, snapshots, compression, encryption.
- **raw** optional: fastest, no snapshots.
- **Reflink** on btrfs and XFS: instant copies.
- `io_uring` where seccomp and kernel allow; `virtio-blk` by default for speed, `virtio-scsi` for discard and many disks. `iothread` per disk.
- TRIM from guest is passed to shrink. "Reclaim space" compacts offline.
- Export, with the machine shut down: a `.lordvm` bundle with or without snapshots, OVA, or a bare disk image, optionally encrypted. Import: 6.22.

### 6.5 Network

| Mode | How | Reach | Notes |
|---|---|---|---|
| **Shared** (default) | `passt` user-space, no root | Guest reaches the world, world doesn't reach guest | Port forwarding by rules |
| **Bridged** | tap on a bridge, closed admin action | Guest on the LAN with its own address | Ethernet. Wi-Fi can't bridge: says so |
| **Host only** | private bridge | Host and other VMs | No internet |
| **Isolated** | no NIC | Nothing | Testing |

Debian 13 has `passt` (**Measured**, 2025-05-03 snapshot). Phase 0 compared it with QEMU's `-netdev user`, with the guest
downloading 500 MB from a server on the host (**Measured**, 2026-11-04, medians of 9 runs):

| Host | `-netdev user` | `passt` | CPU, user / passt |
|---|---|---|---|
| Pi 5 | 234.5 MB/s (221–244) | 222.9 MB/s (96–379) | QEMU 249 % / QEMU 160 % + passt 80 % |
| amd64, **nested** | 252.3 MB/s (209–284) | **817.6 MB/s** (557–1005) | QEMU 266 % / QEMU 253 % + passt 61 % |

**Decision (confirmed, open question 7 closed):** `passt` stays the default for Shared. It's 3.2× faster on amd64, about equal on the
Pi, and it is the process the firewall can name as the machine. On the Pi its speed varies far more from run to run, which Phase 1
looks at. QEMU 10.0 has no `-netdev passt`, so it's connected through `-netdev stream` on a socketpair (3.3). `vhost-net` for Bridged. `passt` runs as the machine's own process, in its scope, because it's the one opening the connections: the firewall has to see **that** as the machine, as it does for `desktop:lordos-rdesk`. Centinela sees each VM as an app (`desktop:lordos-vm-<id>`, **To test**): its traffic in Shared mode appears in the firewall, the map and Monitor as "Machine: name". A rule can block it. Bridged traffic bypasses this: Security says so.

Each forwarded port has a scope: **Only this computer** by default, or **Your network**, which is a firewall rule the person sees.

### 6.6 USB

`usbredir` over the Chardev stream: no root, device opened by a closed admin action `vm-usb-allow`, which claims the single device for that VM and writes nothing permanent. Menu lists devices with names. Auto-connect rules per VM. USB 3 through `qemu-xhci`. Weak: isochronous devices (webcams, audio) can stutter; Camera and Microphone have their own paths. Never offered: the boot disk or the keyboard in use.

### 6.7 Shared folders

`virtiofsd` 1.13.2 (Debian, **Measured**), one per folder, in a sandbox of its own that can reach only that folder. **QEMU never gets the folder**: it holds a vhost-user socket, so a broken QEMU still can't open your files. The reverse needs saying: vhost-user requires guest memory to be shared, so a broken `virtiofsd` can read that guest's memory, and only that guest's. Machines with no shared folder don't run it, and don't pay the shared-memory backend. Linux guests mount it natively. virtio-win guests use the virtio-win virtiofs driver plus WinFsp (a separate dependency to review). Read-only by default, as in Remote Desk. Fallback: a small SMB share reachable only by that guest.

**Measured on 2026-11-04** (virtiofsd 1.13.2, `--readonly --sandbox namespace`, section 13.5):

- **Linux guests on both architectures** mount it with `mount -t virtiofs`; a 200 MB file reads back with the host's SHA-256, and writing
  fails with "Read-only file system". On arm64 virtio-win guests it's still **To test**.
- **The host kernel needs no FUSE or virtiofs option:** virtiofsd works entirely in user space, and the guest's kernel has the driver
  built in. What the host needs is `memfd_create`, which LordOS's Pi kernel has. The earlier "Pi kernels need `CONFIG_FUSE_FS`" was wrong.
- **Speed:** copying the 200 MB file inside the guest takes 0.23 s (about 870–900 MB/s) on the Pi and on amd64. On amd64 (**nested**) the
  first read of each boot was about 25× slower (5.5 s), cause not found; to check on bare metal.
- **Cost:** the shared memory backend counts in full once the guest touches it: peak QEMU RSS 803 MB against 444 MB without a share, and
  boot 42.4 s against 36.5 s (amd64, **nested**). This is why machines without a share don't get the backend.
- **Boot order:** with the virtiofs device attached, the arm64 firmware went to its built-in shell instead of booting. **Decision:**
  `lord-vm` always gives the system disk an explicit `bootindex`.

### 6.8 Clipboard and drag

Clipboard through `org.qemu.Display1.Clipboard` (text, images), direction chosen per VM, **Off for Testing**. Protected windows (`wm.setProtected`) never leak. File drag: into the guest copies through the agent; out copies from the guest. Linux: our `lordos-guest`. virtio-win guests: SPICE agent, `spice-vdagent` with the dbus Clipboard path needs confirming: QEMU's clipboard needs `qemu-vdagent` chardev (**To test**).

If the D-Bus clipboard turns out unworkable on a guest, the clipboard moves to the agent's own channel, `org.lordos.agent`: text, images and files, with the direction chosen for that machine. It's a private data channel inside the machine, so it needs no network, no port and no VNC. WebRTC's data channel solves reaching another computer, and 6.23 uses it there; inside one computer a virtio-serial port does the same job with no encryption layer to carry, since nothing leaves the host.

### 6.9 Sound, camera, microphone

Sound via `audio-dbus`, into PipeWire by `lord-server`. Microphone and camera: asked once per VM with the existing Permissions flow; camera through USB redirect or `uvc` emulation. Off by default.

### 6.10 Graphics

| Guest | Host GPU | Result |
|---|---|---|
| Linux | Any Mesa GPU | virgl: OpenGL; venus: Vulkan, needs blob resources, guest kernel ≥ 5.16 (**Reported**, Mesa docs) |
| Linux | Pi V3D | virgl: QEMU 10.0.13 aborts on every UEFI boot with `gl=on` (3.2), works only with a directly booted kernel, picture not yet checked (**Measured**, 2026-11-04); venus **To test**: V3D's Vulkan features may not be enough |
| virtio-win | Any | 2D only: no good free 3D driver exists |
| Emulated | Any | 2D |

Reported notes: Intel Meteor Lake or `xe` hosts need kernel 6.16+ and QEMU 11.0+ with `honor-guest-pat=on` for venus; Debian 13 has 10.0 (**Measured**), so venus on those hosts waits for a newer QEMU. NVIDIA proprietary needs 570.86+ for venus (**Reported**). Recent security fixes around virtio-gpu blob scanouts are a reason to keep 3D opt-in. In the sandbox, 3D gives QEMU a render node: a larger attack surface, stated in Security. `vhost-user-gpu` would put rendering in its own process: to evaluate in Phase 3.

**Decision (changed): 3D isn't offered on arm64 hosts with Debian 13's QEMU** until the reset abort of 3.2 is fixed there, by Debian, by
a patch of ours (open decision 5) or upstream. The fault is guest-triggerable, so a direct-kernel-boot workaround isn't enough to ship it.

### 6.11 Migrate from another PC

A small agent runs on the old PC, or the disk is read from a backup image, or the disk is connected. Copies the disk over LAN to a new VM and injects virtio drivers so that it boots. Weak: driver injection into a virtio-win guest is delicate; activation may need the person's attention; full-disk encryption needs the recovery key; arm64 hosts can't run an amd64 migrated system usefully, and the card says so. Phase 4.

### 6.12 Guest app integration

- Guest's apps in the launcher and Dock, with icons, marked with a small badge. Launching starts or resumes the VM.
- Open a LordOS file with a guest app: Files' Open With, through the shared folder.
- Reverse: open a link or file from the guest on the host: optional.
- Guest notifications to the host: optional, off by default.

### 6.13 Nested virtualization

Inside a guest: amd64: on by default for Software Development (`kvm_intel nested`, `kvm_amd nested`). arm64: kernel 6.16 enabled it for hardware that has it, off by default, through `KVM_ARM_VCPU_HAS_EL2` (**Reported**); needs FEAT_NV2, so not on Pi 5 or Pi 4 (Pi 5: `KVM_CAP_ARM_EL2` = 0, **Measured**, 2026-11-04). LordOS itself as a guest gets this for free where the host does.

### 6.14 Monitor and system ties

Monitor shows each VM as one row with a drill-in. Cleaner never touches `~/Machines` without explicit choice, and offers Reclaim space. Backups skip running VM disks unless consistent. Files shows `.lordvm` as a bundle. Search finds VMs by name. Desktops treat each VM window normally. Power: laptops on battery get a notification with Pause All; a guest's load keeps the host awake while it runs, on purpose, and the battery warning says so.

### 6.15 Command line and coding agents

`lordvm list|start|stop|suspend|snapshot|clone|console <name>`, plus through the agent:

```sh
lordvm shell "Debian Dev"                 # an interactive shell
lordvm exec "Debian Dev" -- uname -m      # exits with the command's status; stdin and stdout are piped
lordvm logs "Debian Dev"                  # follows until Ctrl+C
lordvm cp ./build "Debian Dev":~/          # with progress, in either direction
```

Same rules as the app: one small tool over the same socket, no shortcut around sandbox or admin actions, reachable only by the person's own user.

A local **MCP server** (`lordvm mcp`), off until turned on in Settings, lets a coding agent **build a machine**: "a Debian machine with 40 GB, user neil". Tools: `list_machines`, `requirements`, `create_machine` (card or ISO, profile, unattended), `wait_until_ready` (with progress), `machine_status`, `start`, `stop`, `clone`, `screenshot`, `run_command`, `get_logs`. A password it creates goes to the keyring and never into its replies. It speaks only over stdin of a process the person started, and runs commands as the guest's normal user (6.18). It can't touch a VM marked **No automation**, which is the default for profiles with sensitive data, and every action it takes is written to the VM's log. Destructive tools (delete, revert) ask the person on screen.

### 6.16 Storage window

Opened from the host meter, or Window → Storage. It answers one question: **what would really be freed?**

- One bar for the volume: machines, downloads, Trash, other files, free. Colors differ in lightness too, and each has a text label and size.
- A list with each machine under the one it came from: disk size, **what deleting it would free**, and what it shares.
- Selecting one shows what it shares and with whom, and the split by disks, suspended state, snapshots, installation media and other.
- **Downloads:** verified installers and images, removable, marked "can be downloaded again".
- **Trash:** machines there still take space until emptied.
- Deleting from anywhere states the same figure before asking.
- Offers: Reclaim space (compaction, with an estimate first), delete old snapshots, remove unused media.

How it knows: backing-file chains are read from qcow2 headers; shared extents from `FIEMAP` with `FIEMAP_EXTENT_SHARED` on btrfs and XFS. A clone with shared extents frees only what changed in it; what's shared is freed once all sharers are gone. Figures are shown as "about" on btrfs with compression, where they can't be exact. Measuring runs at low priority on open and caches by file generation, so it never slows the desktop. Ext4 has no reflink, so clones there use backing files or take the full size, and the window explains why. Snapshots are counted separately, which a simple size listing hides.

### 6.17 Agent state

`lordos-guest` is one small static binary with no Python or other runtime, so it runs on a minimal guest. It's separate from `qemu-ga`, which keeps doing time, shutdown and freeze.

The first icon on the bar shows its state: gray (not installed), amber (machine starting or agent reconnecting), green (connected), red (version too old or failing). Clicking it opens a popover:

- **Connected:** the system and architecture as the agent reports them, agent version, connection time.
- **A list of what works now, and what doesn't and why:** Shell, Logs, File copy, Clipboard, Resolution, Folders, Apps. Each line comes from the agent's reported capabilities, so it's the truth for that guest, not a table in the app.
- **Not connected:** the next step, which depends on the state. If the guest is still booting, says wait. If the agent isn't installed: an **Install Agent** button attaches a small verified disc, shows the one command to run, and removes the disc once connected.

**Protocol.** A virtio-serial port named `org.lordos.agent`, chosen over vsock because it works in every guest without extra drivers. Length-prefixed frames with a channel number, a `hello` carrying version and capabilities, skipped unknown kinds so that either side can grow, and a hard frame limit. `lord-vm`, not `lord-server`, parses it, inside the sandbox. A newer host updates an older agent itself only with the person's consent, from the signed disc.

**Direction of trust.** The host runs things in the guest, never the reverse. A guest can't open a channel, start a host program, or read a host path. Only the person's own user can reach a VM's socket. A profile can turn the agent's shell off while leaving file copy on, and **Testing** starts with everything off.

### 6.18 Shell

The Shell icon opens LordOS's Terminal with a shell inside the guest: over the agent, not the network, so it works with no SSH and with Isolated network. **The signed-in user by default**, in a Linux shell or PowerShell. Root or an administrator shell only with an explicit switch, per machine, in Security. The same rule covers `lordvm exec` and the MCP server's `run_command`. The tab is titled with the machine's name and marked, so that a guest's shell can't be mistaken for a host's. Terminal size follows the tab.

### 6.19 Logs

A window following the guest's logs as they arrive: the systemd journal in Linux, the System and Application event logs in virtio-win guests.

- Filter by text as you type, plus level and unit or source.
- Pause (entries keep buffering, with a count of new ones), Clear view, Copy selected, Save to a file.
- Colored level plus a text label, monospaced, timestamps in local time, with the guest's time zone shown if it differs.
- A bounded ring in memory, 50 000 lines, so a noisy guest can't grow the app.
- Also **Machine log**: QEMU's and `lord-vm`'s own log, which works even if the guest never boots. A guest's log is untrusted text: escape sequences are stripped.

### 6.20 Screenshot and recording

The camera icon saves the guest's display at its real resolution, directly from the frame, with no window around it. The record icon records display and sound to a video, with hardware encoding where available, a visible red state on the bar and the Clock bar's recording indicator. Files go to the same folder as the system's Screenshots app. No screen-capture permission is needed, since it's the VM's own frame, and protected host windows are never included.

### 6.21 File copy with progress

Drag files and folders onto a running machine's window to copy them to the desktop of the user signed in at its display, or onto a shared folder window to choose the place. It goes through the agent, so it needs neither network nor shared folders.

A panel along the bottom of the window shows file name, size, a bar, speed, time left and Cancel. Several transfers queue in it, and it hides when done.

- Atomic: files arrive in a hidden folder beside the destination and are moved in when complete, so that nothing half-written appears.
- Name conflicts are never overwritten silently: a numbered name is used, and the panel says so.
- Flow control by acknowledgement keeps memory flat for any file size. **Target:** 300 MB/s on Linux guests with NVMe on a PC, measured in Phase 3.
- Resume after a dropped connection, by offset and hash. Optional end-to-end check, on by default for files over 1 GB.
- Names are sanitized for the guest's rules. Symbolic links aren't followed; permissions and dates carry over where the file system can.
- **Out of the guest:** in Apps mode, windows are ours, so dragging out works. From a Linux guest's own desktop, no agent can see the drag, so the agent adds **Send to LordOS** to the guest's file manager, plus `lordvm cp`. The panel says this when someone tries. Virtio-win guests allow drag-out through the agent, **To test**.
- Scanning: files coming out of a guest get the same quarantine marking as downloads.

### 6.22 Import

An importer has three steps: **detect** a file or folder, show a **plan** of what will happen, then **import** into a new `.lordvm`, leaving the original alone.

| Source | Reads | Result |
|---|---|---|
| Disk images: qcow2, VMDK, VDI, VHDX, raw | The image | New machine, settings from a profile |
| OVA, OVF | Descriptor: CPU, memory, disks, networks | Machine with those settings |
| Virtual machine bundles of other desktop tools | Their settings file and disks | Settings mapped where they mean the same; disks converted |
| libvirt domain XML | Domain | Machine with those settings |

The plan states what's lost. **Snapshots in foreign formats are flattened to the current state.** Saved sessions can't move between engines, so a machine must be shut down first. Encrypted foreign disks need decrypting first. A guest that lacks virtio drivers starts on emulated SATA and an ordinary NIC; the assistant then installs the drivers and switches. A machine of another architecture imports fine and is marked Emulated: moving an arm64 machine to a Pi works natively, moving an amd64 one doesn't. Conversions run in the sandbox, because disk images are hostile input.

### 6.23 Reaching a machine from another computer

On its own computer a machine's picture comes by dmabuf and its input, clipboard and sound through the D-Bus display and the agent: no network, no encoder, no VNC. Compressing it for WebRTC there would only add delay.

From another computer, a running machine is offered by LordOS's own remote desktop over WebRTC, with video and a data channel for input, clipboard and files. It uses the same pairing, stays off by default per machine, shows who is connected, grants input, clipboard, files and sound one by one, and no VNC or SPICE port is ever opened by a machine. Phase 5: **To test**.

### 6.24 Reproducible machines

`lordvm apply dev.toml` creates or updates a machine from a short text file kept beside a project: system, resources, forwarded ports, folders, packages to install through the agent. A team gets the same environment by running one command, `lordvm rebuild` starts over from it, and the file is reviewed like code. Shared folders and root access in such a file still ask the person on screen: a file from a repository is untrusted. Phase 4.

## 7. Guest tools

### 7.1 What each guest receives

| Need | Linux | virtio-win |
|---|---|---|
| Time, shutdown, IP, freeze | `qemu-guest-agent` | qemu-ga from virtio-win |
| Resolution, clipboard | `spice-vdagent` or `lordos-guest` | SPICE agent |
| Folders | Kernel | virtiofs driver + WinFsp |
| Apps mode | `lordos-guest` | our agent, later |
| Install | From guest's repo | Mounted ISO, one run, silent |

Tool updates: Linux through guest's own updates, plus `lordos-guest` in our repository; virtio-win at the pinned version, offered by the app, with a snapshot first.

### 7.1.1 Driver or agent, and what we would write ourselves

Copy and paste, drag, resolution and shell are **agents**: ordinary programs in the guest with no kernel signature to obtain. Graphics, disk and network are **drivers**: on virtio-win guests, kernel code that the guest refuses to load unsigned.

| Piece | First version | Our own later | Cost of our own |
|---|---|---|---|
| Linux disk, network, display, 3D | In the guest's kernel and Mesa, nothing to install | No reason to | None |
| Linux clipboard, resolution, files | `spice-vdagent`, then `lordos-guest` | Yes: `lordos-guest` | Ours, ordinary packaging |
| virtio-win disk, network, memory, 2D display, folders | The public virtio-win set | No: they are mature and free | Would add risk and nothing else |
| virtio-win clipboard, resolution | SPICE agent | Yes: `lordos-guest` for virtio-win, user mode | Code signing certificate, to avoid warnings |
| virtio-win file copy, shell, logs, Apps mode | Our agent, phase by phase | Yes | Same |
| virtio-win 3D | None | Only by following or joining the open effort below | Very high |

**3D on virtio-win guests.** No stable free driver exists. An open effort pairs a WDDM kernel driver with a Mesa user-mode driver over virgl; its Mesa and driver changes were still unmerged and described as glitchy and experimental when checked on 2026-11-01. Its driver also turns preemption off across the guest, which is a real limit. We don't ship it. We re-check it each phase, and it would come as an opt-in with a clear warning before it could be a default. Graphics passthrough of a whole GPU is a separate route, needing VFIO and a second GPU, and it isn't this.

**Signing our own kernel driver.** A driver loaded by current virtio-win guests needs the system maker's signature: an organization account in its hardware portal, an EV certificate for that account, then attestation signing or, to reach its retail update channel, certification with the hardware lab kit. That is a yearly cost and a legal entity behind it, for every architecture. So own kernel drivers are out of scope unless 3D makes them worth it. Our work goes to the agent, which needs none of this.

### 7.2 Apps mode (seamless)

**Linux guests:** apps speak Wayland to a small compositor proxy in the guest (`lordos-guest`), which forwards surfaces over vsock to the host, as `waypipe` does. On the host each surface is a native LordOS Wayland client, so it's a real window with its own Dock entry. Zero-copy with virtio-gpu cross-domain is the target; a first version copies. Guest X11 apps need the guest's Xwayland. Host keeps no X11.

**virtio-win guests:** no free way exists. Our own agent would enumerate windows and stream regions. Phase 5 at the earliest, and only if real demand exists. Until then Apps mode is off there, and the setting says so.

Each forwarded window carries a thin marker in the frame with the machine's name, so that a person can always tell a guest's window from a host's, and it can't request protected-window status or the host's secure input.

## 8. Privacy and security model

- **Machines itself** makes no connection, except downloads the person starts and the signed catalog when the assistant opens. No telemetry.
- **A guest's connectivity is a separate thing:** the default network mode is Shared, because a machine with no network surprises people, and the assistant shows that. Testing is Isolated.
- Everything shared with a guest is **off** until the person turns it on, in every profile (5.6).
- Guest → host channels: virtio devices, vsock to `lordos-guest`, clipboard, folder, usbredir. Each has an on/off and shows in **Guest can see**.
- `lordos-guest` traffic is treated as untrusted: strict parser, size limits, fuzz target. A guest can't start a host process, read host files beyond its folder, or inject input into other windows.
- **An encrypted machine has to be encrypted whole,** or the disk is the only safe part. Disks use qcow2 LUKS, with the key in the keyring. Also covered: suspended state and snapshot memory, written inside the encrypted qcow2 and never as a separate file (**To test**); TPM state, through swtpm's own state encryption (**To test**, to confirm in Debian's build); NVRAM; recordings and screenshots stay out of the folder. Thumbnails, and persistent logs, are off, and logs stay in memory. Host swap can still hold guest memory, and only encrypted swap solves that, so Security says so. Bridged mode and encrypted disks each state what they mean in one line.
- `vm-enable`, `vm-bridge-on/off`, `vm-usb-allow` are closed actions with fixed arguments, added to the list maintainers review.
- Catalog of images and tools: signed with the LordOS release key path, verified before use.
- A hostile ISO can only attack the guest, then QEMU, which is why the sandbox exists. QEMU updates arrive with Debian's.

## 9. Performance targets, and what Phase 0 measured

Targets are goals. The Phase 0 columns are **Measured** on 2026-11-04 (section 13); the amd64 ones are **nested** and are not bare-metal
figures. Phase 0 had no app and no `lord-vmdisplay` yet, so app memory and latency to the screen are still open.

| Measure | amd64 target | amd64, Phase 0 (**nested**) | Pi 5 target | Pi 5, Phase 0 |
|---|---|---|---|---|
| CPU work vs bare metal | ≥ 95 % | 98 % (SHA-256), 96 % (Python), vs the VM host | ≥ 90 % | 100–101 % (SHA-256), 97.6 % (Python) |
| Disk sequential, qcow2 vs host | ≥ 80 % | Not measured | ≥ 70 % | Not measured |
| Network, virtio Shared mode | ≥ 2 Gb/s | 6.5 Gb/s with `passt`, 2.0 Gb/s with `user` (guest from host) | ≥ 500 Mb/s | 1.8 Gb/s with `passt`, 1.9 Gb/s with `user` (guest from host) |
| Boot of a small Linux guest | ≤ 8 s | 36.5 s, of which 5 s is the image's boot menu countdown; not a real-hardware figure | ≤ 15 s | 18.2–18.6 s with UEFI (12.3 s firmware + 6.3 s Linux); 5.8 s with a directly booted kernel |
| Pause, resume | ≤ 100 ms | Not measured | ≤ 100 ms | Not measured |
| Suspend, resume with 8 GB | ≤ 10 s | Not measured | ≤ 20 s | Not measured |
| Added display latency | ≤ 1 frame with dmabuf | QEMU → helper 101–107 µs median, p95 ≤ 178 µs (shared memory); to the screen not measured | ≤ 2 frames | QEMU → helper 122–360 µs median, p95 up to 6.4 ms when copying every update (shared memory); dmabuf not measured |
| Idle host CPU per idle guest | ≤ 2 % of a core | 2.8–2.9 % QEMU + 0.25 % helper, text console | ≤ 3 % of a core | 1.2 % QEMU + 0.4 % helper (2D); 3.3 % QEMU with 3D |
| App memory, no VM running | ≤ 40 MB | No app yet | ≤ 40 MB | No app yet |
| Desktop frame time with one machine at full load | No worse than 1 frame over 60 Hz | Not measured | **No worse than 1 frame over 60 Hz at the default 3 vCPUs** (set 2026-11-04) | 59.3 FPS at 3 busy vCPUs (58.0 idle); 2.4 FPS at 4 |

The network column is a guest downloading from a server on the host, so it measures the path through the network back end, not a
real link. The Pi's boot misses its target with UEFI firmware, which takes two thirds of it; the guest's Linux alone takes 6.3 s.

**Desktop under load is the hard one on a Pi.** With all CPU cores busy, V3D went from 1.5 ms to 14–16 ms of GPU per frame with no clock change, and memory contention was measured as the cause on 2026-10-31 (`GPU.md`). Phase 0 measured window dragging with a guest busy on 3 vCPUs: 59.3 FPS, no worse than idle, so **the default of 3 cores stays** and the Pi target is now set (**Measured**, 2026-11-04, Pi 5). With 4 busy vCPUs it fell to 2.4 FPS, but so does the host with 4 busy threads and no VM (3.3 FPS), and `nice 19` on QEMU changed nothing: the stalls of about 1000 ms are a desktop scheduling problem of their own, to fix outside Machines. Two limits on this result: the guest's load was CPU-bound (SHA-256), not memory-heavy, so the memory-bus case of `GPU.md` is still **To test**; and someone used the board during the 4-vCPU runs, which are to be repeated.

**Zero copy is a goal to measure, not a promise.** 3.2 says "no copy", but it depends on GPU, format, modifier and sync. Phase 0 recorded what QEMU sends; nothing was imported yet:

- **Pi 5, V3D:** `ScanoutDMABUF` with `XB24` (XBGR8888), the V3D tiled modifier `0x0700000000000006`, one plane, a descriptor longer than stride × height (padding for the tiles), and `UpdateDMABUF` at up to 194 a second, with the helper at 2 % CPU since it copies nothing (**Measured**, 2026-11-04, direct kernel boot only, see 3.2). Import into the compositor, direct scanout, sync and correctness of the picture: **To test**.
- **amd64:** not measured: the Phase 0 host had no render node. **To test** on a bare-metal PC with Mesa, and with a vendor's proprietary driver.

The LordOS rule holds: this app must not slow the desktop. It's idle until opened and runs nothing in the background without a running VM.

## 10. Packaging

- **Machines** is a Store item, free, not preinstalled: QEMU and firmware are large and most people won't use them. A Dock entry appears once installed. Open decision 3.
- Package: `lordos-machines` (app, `lord-vm`), with `Depends: qemu-system-<native arch>`, `qemu-system-modules-opengl`, `qemu-utils`, `swtpm`, `virtiofsd`, UEFI firmware.
- Emulation: a second switch on the Store page, as Disk Formats has for APFS: `qemu-system-<other arch>`.
- Sizes: `qemu-system-x86` is 68 MB (**Measured**). The whole set, with dependencies, is **To measure**: plan on 300 to 500 MB, not including any image. Each guest wants 20 to 64 GB.
- **LordOS 1.0 doesn't depend on Machines.** It's an optional Store package that follows the roadmap's place, after the final ISO. Its first public form is an **experimental, reduced release**, Phase 2's scope, and it ships only after passing isolation, disk integrity and failure recovery tests (section 12).
- Debian's QEMU is used, not a fork, so security updates come from Debian. If a feature needs a newer one, backport with care: open decision 5.
- Licenses: QEMU GPL-2.0, swtpm BSD-3-Clause, virtiofsd Apache-2.0/BSD-3-Clause, EDK2 BSD-2-Clause-Patent, passt BSD-3-Clause; all stated on the Store page.

## 11. i18n, accessibility, docs

All text in en, es and pt from the first commit, a `machines.json` per language. Tiles and sheets keyboard reachable. Captured VM is announced with its release keys. Guest content is opaque to screen readers, and the app says so. Business rule: CHANGELOG, paper, FAQ, features and site follow every shipped phase. Claims only after they were tested. RTL handled by the SDK. Help article `machines.md`.

## 12. Tests

- **Unit:** settings → QEMU arguments in a table test, for each architecture; `machine.json` schema and migration.
- **Sandbox:** a test that a hostile `lord-vm` child can't open `~/.ssh`, another VM's folder, or `/dev/kvm` again.
- **Fuzz:** `lordos-guest` parser and D-Bus listener in `lord-server`.
- **Boot matrix** in CI where KVM exists: Debian, Ubuntu, Fedora, LordOS on amd64; same on arm64 (Pi).
- **Real hardware matrix before each claim:** Pi 5 16 KiB kernel; Pi 5 4 KiB kernel; Pi 4; Intel; AMD; a nested amd64 VM (TCG path and refused KVM).
- **Install test** of each pinned virtio-win version before the catalog moves.
- **E2E** with `vm.sh`: the assistant to first boot, snapshot and revert, folder, USB, clipboard.
- **Agent:** fuzz the protocol from the guest side; kill mid-transfer and check that nothing partial appears; a guest asking for a channel is refused.
- **Storage figures:** clones on btrfs, XFS and ext4 compared to the real change in free space after deleting.
- **Release gate, isolation:** a QEMU made to misbehave on purpose can't reach the home folder, other machines, or the compositor; malformed scanouts sent to `lord-vmdisplay` are refused.
- **Release gate, disk integrity:** kill `lord-vm`, QEMU and the power during write, snapshot, revert, clone and compaction, then `qemu-img check` every disk.
- **Release gate, failure recovery:** each interrupted operation of 3.6 ends finished or rolled back, with no terminal.
- **Leak test:** start and stop 100 times: no orphan processes, scopes, sockets, tap devices.

## 13. Phase 0 results

Phase 0 ran on 2026-11-04, with no app: QEMU driven over QMP only, a root login on a serial console reached through a Unix socket, and
a small display prototype in C. Everything here is **Measured** on that date, on the machine named. Timings are medians of at least 3
runs unless it says otherwise. The scripts are in `tools/vm0/` (not committed yet).

### 13.1 Machines and versions

| | Pi 5 | amd64 (**nested**) |
|---|---|---|
| Hardware | Raspberry Pi 5, 16 GB, 4 × Cortex-A76 | A VM with 8 vCPUs and 16 GB, on a desktop hypervisor running on a 10-core amd64 desktop CPU at 2.8 GHz base, AVX2, no SHA instructions |
| Host kernel | LordOS `6.18.57-lordos.3-rpi-2712`, 16K pages | Debian `6.12.115+deb13.1-cloud-amd64`, nested KVM on in the kernel module, no DRM drivers |
| QEMU | Debian `1:10.0.13+ds-0+deb13u1` | same |
| Other parts | virtiofsd 1.13.2, passt (Debian 13), AAVMF from `qemu-efi-aarch64` 2025.02, virglrenderer 1.1.0 | virtiofsd `1.13.2-1+deb13u1`, passt `0.0~git20250503.587980c-2+deb13u1`, same AAVMF, SeaBIOS |
| Guests | Debian 13.7 nocloud images, arm64 and amd64, kernel `6.12.115+deb13.1` with 4K pages, checked against Debian's `SHA512SUMS` | same |
| Benchmarks | OpenSSL 3.5.7, Python 3.13.5, on host and guests | same |

Every amd64 number below is a guest inside a guest. Bare-metal amd64 is still **To test**, and should be better, above all for boot
time and for I/O that causes many exits.

### 13.2 KVM guests

| Measure | Pi 5, arm64 guest | amd64 guest (**nested**) |
|---|---|---|
| First probe: boot to login, shutdown, QEMU RSS | 18.6 s (12.3 s firmware + 6.3 s Linux), 2.2 s, 530 MB, at 2 vCPU and 1 GB | 36.6 s, 3.7 s, 409 MB |
| Boot to login, 3 vCPU, 2 GB | 18.2 s (cold first boot 19.1 s) | 36.5 s (6 boots, 34.4–36.6) |
| Boot to login, kernel booted directly, no firmware, 2 vCPU | 5.8 s | Not measured |
| Shutdown, 3 vCPU | 3.9 s | 5.4 s; 3.4 s right after the benchmarks: it depends on the guest's uptime |
| QEMU RSS at login, 3 vCPU, 2 GB | 553 MB, peak 571 MB | 424 MB, peak 444 MB |
| `/dev/kvm` through `-add-fd` and `-accel kvm,device=/dev/fdset/1` | works | works, with `-sandbox on`; threads are `CPU n/KVM` |
| QEMU's `-sandbox on,…` | used in every run | no measurable cost: 36.5 s against 36.0 s, RSS 424 against 422.5 MB |

- **4K guest on the 16K Pi kernel: works.** GICv2 with vGIC, `-M virt -cpu host`, nothing special.
- **Where the amd64 boot goes** (1 boot, `systemd-analyze`): kernel 7.0 s + user space 4.8 s = 11.8 s. The rest is SeaBIOS and the image's
  5 s boot menu countdown. The guest kernel alone takes about 2 s to reach ACPI setup, which looks like a cost of nesting (not measured
  on bare metal). This figure shouldn't be quoted for real amd64 hardware.
- **On the Pi, firmware is two thirds of the boot.** Without it, the guest is at its login in 5.8 s instead of 18.5 s.
- **With `-nodefaults`, an amd64 guest needs a display device**, or its kernel never starts (3.3).
- **EL2 on the Pi:** `KVM_CAP_ARM_EL2` = 0 and `KVM_CAP_ARM_EL2_E2H0` = 0, on `/dev/kvm` and on a VM descriptor;
  `KVM_CAP_ARM_VM_IPA_SIZE` = 40 as a sanity check. No nested virtualization on a Pi 5, as 2.2 expected.

### 13.3 CPU benchmarks, KVM guest against its host

Same commands on host and guest: `openssl speed -seconds 5 -bytes 16384 sha256` with 1 thread and with `-multi 3`, and a Python loop
(`sum(i*i for i in range(10**7))`). Guests have 3 vCPUs and 2 GB.

| Measure | Pi 5 host | Pi 5 guest | Ratio | amd64 host (**nested**) | amd64 guest | Ratio |
|---|---|---|---|---|---|---|
| SHA-256, 1 thread | 1,471,362 kB/s | 1,489,224 kB/s | 1.01 | 576,346 kB/s | 565,264 kB/s | 0.98 |
| SHA-256, `-multi 3` | 4,448,351 kB/s | 4,431,472 kB/s | 1.00 | 1,703,897 kB/s | 1,663,707 kB/s | 0.98 |
| Python loop | 1.148 s | 1.176 s | 2.4 % slower | 0.471 s | 0.489 s | 3.8 % slower |

KVM on the Pi 5 runs at native CPU speed on the 16K kernel we already ship. On amd64 the ratio is against the VM that hosts the guest,
not against bare metal.

### 13.4 Emulation (TCG)

| Measure | Pi 5 host, amd64 guest (`-cpu max`, SeaBIOS) | amd64 host (**nested**), arm64 guest (`-cpu max`, AAVMF) |
|---|---|---|
| Boot to login, 2 vCPU | 98.5 s (98.3–100.2) | 59.6 s (59.1–60.0) |
| Boot to login, 4 vCPU | 82.6 s (82.2–82.8) | 55.1 s (54.8–55.3) |
| Boot with `thread=single`, 2 vCPU (1 run) | 152.9 s | Not measured |
| Shutdown, 2 / 4 vCPU | 7.3 s / 7.0 s | 4.6 s / 4.9 s |
| QEMU RSS at login (peak), 2 / 4 vCPU | 753 (818) / 768 (827) MB | 785 (825) / 804 (838) MB |
| SHA-256, 1 thread, against the KVM guest | 19.6× / 19.3× slower | 6.0× / 5.9× slower |
| SHA-256, all vCPUs, against the KVM guest's `-multi 3` | 23× slower with `-multi 4` on 4 vCPU | 4.75× slower with `-multi 4`; QEMU at 374 % CPU |
| Python loop, against the KVM guest | **25.7× / 26.4× slower** | **58.6× / 57.4× slower** |

- **Multi-threaded TCG is QEMU's default:** plain `-accel tcg` gives one `CPU n/TCG` thread per vCPU, and an explicit `thread=multi`
  booted in the same 98.1 s.
- **The SHA-256 ratio on amd64 flatters emulation.** The emulated arm64 CPU reports `sha2 sha512 sve2`, while the host has no SHA
  instructions and OpenSSL uses its AVX2 code there. The Python ratio, about 58×, is the better general figure. On the Pi the emulated
  amd64 CPU reports `sha_ni` and `avx2`.
- The 5–20× planning range of 2.4 is optimistic for the Pi.

### 13.5 Networking and shared folders

**Networking** (guest downloads 500 MB of zeros from Python's HTTP server on the host's loopback, `curl`; 9 runs, 3 boots per mode,
alternated). The same server on host loopback gives 2,252–2,443 MB/s, so it isn't the bottleneck.

| | Pi 5 | amd64 (**nested**) |
|---|---|---|
| `-netdev user` | 234.5 MB/s (221–244), QEMU 249 % CPU | 252.3 MB/s (209–284), QEMU 266 % CPU |
| `passt` through `-netdev stream` on a socketpair | 222.9 MB/s (96–379), QEMU 160 % + passt 80 % | 817.6 MB/s (557–1005), QEMU 253 % + passt 61 % |
| `-netdev passt` | Not in QEMU 10.0 | same |

Why `passt` scales on the faster CPU and not on the Pi isn't measured; one guess is that QEMU's user network is bound by one place in
its code. The decision is in 6.5.

**Shared folders** (virtiofsd 1.13.2, `--readonly --sandbox namespace --cache auto`, `memory-backend-memfd` with `share=on`,
`vhost-user-fs-pci`, a 200 MB random file; guest cache dropped before each copy, host cache warm):

| | Pi 5, arm64 guest | amd64 guest (**nested**) |
|---|---|---|
| Mount, read, SHA-256 matches, writing refused | yes | yes, 3 of 3 boots |
| Copy to the guest's `/dev/shm` | 0.230 s, about 870 MB/s | 0.230 s (0.164–0.430) after the first; the first read of each boot 5.5 s, cause not found |
| Copy to the guest disk with `sync` | 0.334 s | 0.297 s |
| `dd` to `/dev/null` | 1.4 GB/s | 1.4 GB/s |
| Cost at boot | Not measured | 42.4 s against 36.5 s; peak QEMU RSS 803 against 444 MB |

- The Pi kernel has `CONFIG_FUSE_FS=m`, no `CONFIG_VIRTIO_FS` and `CONFIG_MEMFD_CREATE=y`, and nothing more is needed on the host.
- The arm64 firmware dropped into its shell when the virtiofs device was attached, until the disk got `bootindex=0` (6.7).
- virtiofsd's own CPU wasn't measured: its sandbox does the work in a child process, and only the parent was sampled.

### 13.6 Display prototype

A helper of about 640 lines of C (`tools/vm0/vmdisplay-proto.c`) attached to a running, paused QEMU through QMP `getfd` and
`add_client protocol=@dbus-display`, registered a listener on `Console_0` over a second socketpair, received the picture, and logged in
on the guest's console through `org.qemu.Display1.Keyboard` alone. It uses the system's own D-Bus library peer to peer, with no bus and
no `Hello`, and checks every message before using it: size up to 8192 × 8192 and 256 MiB, `x/a8r8g8b8` only, stride at least width × 4,
data length equal to stride × height, the descriptor's real length (`lseek`) at least offset + stride × height, damage inside the picture,
cursor at most 256 × 256 with its hotspot inside. Guests: Debian 13, 2 vCPU, 2 GB, `virtio-vga` on amd64, `virtio-gpu-pci` plus
`virtio-keyboard-pci` on arm64. 3 boots per mode.

**What QEMU 10.0.13 exposes** (introspected over the peer connection):

- `/org/qemu/Display1`: object manager. `/org/qemu/Display1/VM`: Name, UUID, ConsoleIDs, Interfaces.
- `Console_0`: `Console` (RegisterListener, SetUIInfo; Label, Head, Type, Width, Height, DeviceAddress, Interfaces), `Keyboard` (Press,
  Release; Modifiers), `Mouse` (Press, Release, SetAbsPosition, RelMotion; IsAbsolute), `MultiTouch` (SendEvent; MaxSlots).
- `/org/qemu/Display1/Clipboard`: Register, Unregister, Grab, Release, Request.
- Listener interfaces built in: `Listener`, `Listener.Unix.Map` and two for another host system. QEMU picks the optional ones from the
  listener's `Interfaces` property.
- No `ScanoutDMABUF2`, so no multi-plane dmabufs on Debian 13.
- Audio and Chardev objects weren't there, because no D-Bus audio or chardev back end was configured: **To test**.

**Results** (medians of 3 boots; CPU from `/proc/<pid>/stat` over 10 s windows; `yes` and a `dmesg` loop on the console as load):

| Measure | amd64 shared memory, every update (**nested**) | amd64, 60 Hz cap | Pi shared memory, every update | Pi, 60 Hz cap | Pi 3D, direct kernel boot |
|---|---|---|---|---|---|
| Attach, QMP to listener | 5.0 ms | 4.9 ms | 4.7 ms | 4.7 ms | 4.4 ms |
| First picture after QEMU starts | 47 ms | 68 ms | 92 ms | 92 ms | 201 ms |
| Idle: updates/s, helper CPU, QEMU CPU | 4.9, 0.25 %, 2.9 % | 4.9, 0.23 %, 2.8 % | 4.9, 0.4 %, 1.2 % | 4.9, 0.4 %, 1.2 % | 4.9, 0.0 %, 3.3 % |
| `yes`: updates/s received | 1938 | 2320 | 465 | 1065 | 194 (`UpdateDMABUF`) |
| `yes`: copied | 7.9 GB/s | 241 MB/s (58.9 copies/s) | 1.90 GB/s | 239 MB/s (58.3 copies/s) | nothing (not imported) |
| `yes`: helper CPU / QEMU CPU | 48.7 % / 177 % | 8.4 % / 178 % | 86.3 % / 208 % | 16.1 % / 202 % | 2.1 % / 197 % |
| `dmesg` loop: helper CPU / QEMU CPU | 56.3 % / 192 % | 8.8 % / 181 % | 82.5 % / 190 % | 22.8 % / 203 % | 2.0 % / 197 % |
| QEMU update to helper, median / p95, `yes` | 107 / 178 µs | 362 / 617 µs | 161 / 514 µs | 122 / 248 µs | Not measured |
| Same, `dmesg` loop | 101 / 157 µs | 97 / 166 µs | 360 / 6439 µs | 262 / 811 µs | Not measured |
| Helper RSS / QEMU RSS | 13.5 / 433 MB | 13.4 / 446 MB | 18.0 / 582 MB | 18.0 / 581 MB | 2.8 / 563 MB |
| Messages refused | 0 | 0 | 0 | 0 | 0 |

- **The array path** (`--no-map`, every update a `Scanout` message): QEMU at 249 % CPU on amd64 under `yes` against 177 %, the helper at
  61 %, about 600 full pictures a second through the socket; on the Pi 131 a second at 224 % QEMU CPU. On amd64 a mode switch sent
  partial updates for the new size before any `Scanout`; the helper refused them correctly, about 3350 per boot, and the picture stayed
  stale. On the Pi the firmware's mode switches arrived as full pictures and nothing was refused.
- **Latency** was measured from QEMU's `dbus_update` trace event (timestamped by QEMU) to the helper's receipt on the same clock. That
  trace doesn't cover `Scanout` or `UpdateDMABUF`, so those paths have no latency figure. Time to the screen isn't measured: there was no
  compositor in the loop.
- **Damage:** while the console scrolled, every update was the full 1280 × 800 picture on both architectures.
- **The shared memory** is a memfd of 4,096,000 bytes with seals `F_SEAL_SEAL` only, the same on both. Pictures read from QEMU's live
  mapping mid-scroll tear.
- **Cursor:** no `CursorDefine` arrived: a text console has no cursor plane. **To test** with a graphical guest.
- **3D, amd64:** not measured. The host had no render node, and QEMU with `virtio-vga-gl -display dbus,p2p=yes,gl=on` stopped with
  `egl: no drm render node available`.
- **3D, Pi:** with AAVMF, QEMU aborted right after the guest kernel's EFI stub exited boot services, on
  `ui/dbus-listener.c:511: dbus_scanout_texture: Assertion 'tex_id' failed` (exit 134). Unbinding the guest's `virtio_gpu` driver in a
  directly booted guest gives the same abort; a QMP `system_reset` doesn't. Likely cause, read in the 10.0 source and not debugged:
  the device reset runs on the vCPU thread, with no current EGL context, so the replacement surface gets no texture, and the same order
  is still on upstream master. With a directly booted kernel it works: `ScanoutDMABUF` with `XB24`, modifier `0x0700000000000006`,
  `y0_top=0`, stride 2560 at 640 × 480 and 5120 at 1280 × 800, descriptor 4,177,920 bytes against stride × height 4,096,000. During the
  aborted UEFI boot QEMU also sent `AB24` with `y0_top=1`, its own 2D surfaces as textures. Whether the 3D picture is right isn't
  known: the prototype doesn't import, and QMP `screendump` returns "no surface".
- **Identity:** on the Pi, QEMU and the helper ran as the normal user with the `kvm` group added, and the render node was opened through
  the user's `render` group, not as root.

### 13.7 The Pi desktop under VM load

A virtual tablet dragged the Remote Desk window by its title bar, 6 drags of 3 s, while the board's temperature, fan and clocks were
read once a second.

| Load | FPS while dragging | Longest gap | Board |
|---|---|---|---|
| None | 58.0 (work 7.15 ms per frame) | 84 ms | 44–46 °C, fan off, 1800–2300 MHz, not throttled |
| KVM guest busy on 3 vCPUs | **59.3** | 59 ms | 51–55 °C, fan up to about 2770 rpm, 2400 MHz, not throttled |
| KVM guest busy on 4 vCPUs | **2.4** (2.0–3.0) | 1000 ms | 55–57 °C, about 2870 rpm, 2400 MHz, not throttled |
| Same, QEMU at `nice 19` | 2.3 | about 1000 ms | |
| No VM, host runs `openssl speed -multi 4` | 3.3 | 965 ms | |

- Three busy vCPUs leave dragging as smooth as idle, so the default of 3 holds (2.2, 9).
- Four busy threads collapse the desktop with or without a VM, and a lower priority doesn't help. Frames stall for about 1000 ms, which
  looks like a timer: a desktop scheduling issue, not a cost of virtualization.
- **Caveat:** from 18:51 on, someone used the board in person (local keyboard and mouse, a local root shell, the Settings app and the
  interface language changed) during the 4-vCPU run and the two extra runs. The three agree, but the 4-vCPU result is to be repeated on
  an untouched desktop.
- The load was CPU-bound (SHA-256). A memory-heavy guest, the contention case of `GPU.md`, wasn't measured.

### 13.8 What Phase 1 starts from

Answered: 4K guest on a 16K host, GICv2, `device=/dev/fdset`, `ui-dbus` on arm64, TCG numbers, EL2 on the Pi, `passt` against user
networking, virtiofs on both architectures, the desktop under a 3-vCPU load. Still open:

1. **amd64 on bare metal:** boot, CPU, network, virtiofs (including the slow first read), 3D and dmabuf formats with Mesa, and with a
   vendor's proprietary driver. Every amd64 figure here is nested.
2. **The arm64 3D abort:** a fix in Debian's QEMU, a patch of ours (open decision 5), or 3D stays off on arm64 (6.10).
3. **dmabuf import in the compositor:** the tiled V3D modifier, direct scanout or composite, fences with a timeout, and checking that the
   picture is right.
4. **`lord-vmdisplay` for real:** done in 3.9 (`SIGBUS`-guarded copy, copy before forward with an acknowledged frame, a rate lord-server
   sets). Still open: latency all the way to the screen, which needs the window (Phase 1, part 2).
5. **The Pi desktop:** repeat the 4-vCPU drag on an untouched board, measure a memory-heavy guest at 3 vCPUs, and find the cause of the
   1000 ms stalls when all four cores are busy, which happens with no VM at all.
6. **GICV trapping:** the cost of interrupt-heavy work on the 16K kernel, and whether a 4K kernel removes it (open decision 4).
7. **Not touched in Phase 0:** our own seccomp filter on top of QEMU's, Landlock on QEMU, nested cgroup leaves, mouse, pause and
   resume, and UEFI vars as qcow2 are done in 3.9 (the filter is a denylist; nested leaves only where lord-vm's cgroup is delegated, not
   yet under lord-sessiond). Still open: the render node passed by descriptor, one slice for all machines (3.7), D-Bus audio, clipboard
   and Chardev, the cursor of a graphical guest, disk speed of qcow2, suspend.
8. **`passt` on the Pi:** why its speed ranges from 96 to 379 MB/s from run to run, and whether that matters for real traffic.
9. **Boot time:** UEFI firmware is 12.3 s of the Pi's 18.6 s. Whether a lighter firmware setup or direct kernel boot for Linux guests is
   worth offering.

## 14. Phases

**Decided on 2026-11-01, moved on 2026-11-02 and 2026-11-03:** the MVP starts in **0.8.9**, with `lord-vm` and direct QMP, on this stack:

```
Machines (React + LordOS SDK)
  → lord-vm in C (supervisor, QMP, limits, sandbox)
    → QEMU + KVM
      → Debian 13 guest, amd64 and arm64
        → D-Bus display with validated buffers
          → lord-server MediaView, a window in the compositor
```

| Phase | Ships | Done when |
|---|---|---|
| **0, Technical proof** | No app. KVM, QEMU, QMP and D-Bus display: a Debian 13 guest with picture and input on amd64 and Pi 5, over direct QMP. Answers 2.2's open items: 4K guest on 16K host, Pi GICv2, GICV trapping by kernel, TCG numbers, `ui-dbus` on arm64, nested checks, `device=/dev/fdset`, formats and modifiers per GPU, desktop under load. No agent, no 3D, no USB, no snapshots | Done 2026-11-04: numbers in section 13, open items in 13.8 |
| **1, Alpha** | `lord-vm`, `lord-vmdisplay`, sandbox, native window, qcow2 with qcow2 NVRAM, Shared network, schema and contracts (3.6), resource budget. Create, start, stop and recover without a terminal | Isolation test passes on Pi 5 and amd64 |
| **2, Beta** | Control Center, assistant with Linux cards, requirements check, one-screen settings, profiles, snapshots, suspend, clones, Storage window, agent with state popover, Shell, Logs, unified diagnostics, screenshot and recording, resolution menu, Keep Running, clipboard. The experimental release | Full install and failure recovery are repeatable; release gates pass |
| **3, Advanced** | UEFI secure boot, TPM, virtio-win flow on both architectures with automatic setup, virgl, venus, shared folders, USB, file copy panel, Bridged, Host only, Picture in Picture, multi-head, export and import, backups, Monitor, `lordvm` with shell, exec, logs and cp | A real compatibility and security matrix per architecture |
| **4, Integration** | Linux Apps mode, guest apps in the Dock, reproducible machines, local MCP server, Migrate, Android card if the image exists | Each on tested hardware |
| **5, Later** | Our own agent for other guests' windows; a lightweight engine for Linux guests, like crosvm; GPU passthrough on PCs; reaching a machine from another computer | Decided then |

## 15. Risks

| Risk | Effect | Plan |
|---|---|---|
| QEMU's seccomp plus ours break with updates | VMs refuse to start after a Debian update | Boot matrix on each QEMU update, staged Store update |
| A guest OS update stops booting on GICv2 or old CPUs | Pi virtio-win guests break | Test on each release, keep snapshot before updates, state clearly it's best effort |
| Upstream virtio-win regressions | Failing installs | Pinned versions |
| GPU drivers and venus: fragile | Crashes on some GPUs | 3D opt-in, per-GPU allow list, 2D fallback |
| Scope | It's the biggest app on the roadmap | Phases ship on their own; 1.0 doesn't wait for it |
| A compromised QEMU attacks the compositor | Whole desktop falls | `lord-vmdisplay`, fence timeouts, tolerant import |
| A failure midway through a snapshot or clone | Machine damaged | Intent file, rename, release gates |
| Desktop stutters while a machine works | Fails the LordOS rule | Budget, measured target, fewer default cores on a Pi |
| Dependence on 16 KiB behaviour | Pi path rests on To test items | Phase 0 measured a 4K guest working on the 16K kernel at native CPU speed; the GICV trapping cost is still open (13.8) |
| A guest aborts its own QEMU (3D on arm64, QEMU 10.0.13) | The machine dies on every UEFI boot or by the guest's choice | 3D off on arm64 until fixed (6.10) |
| Host page and swap pressure | Host starves | Memory limits, balloon, warning at 80 % |
| Sandbox on Pi 4 kernel | Weaker isolation | Stated in Security |
| WinFsp and agents add third-party code in guests | Maintenance | Pinned, verified, opt-in |

## 16. Feature matrix

The bar: **the most complete desktop virtualization app there is, and the best looking**, on free parts. Ambition is kept honest here, one line per capability.

**Linux today** says whether the kernel and existing open source parts allow it now: **Yes**; **Work** (possible, we must build a real piece); **Partly** (some guests or hosts only); **No**. **M** is the phase of section 14 that ships it. Nothing here is built yet, so "Yes" never means "done".

### Running machines

| Capability | M | Linux today | Note |
|---|---|---|---|
| Native speed through KVM, amd64 and arm64 | 1 | Yes | Pi 5 details in 2.2 |
| Reach a machine from another computer, over LordOS's remote | 5 | Work | WebRTC, paired, off by default; see 6.23 |
| Other architecture by emulation, labelled | 1 | Yes | 2.4 |
| Pause, saved sessions (suspend to disk), resume | 2 | Yes | May not resume across QEMU updates |
| Keep running after closing the window, start with no window | 2 | Yes | |
| Start with the session, order and delay | 3 | Yes | |
| Auto-pause when idle | 3 | Work | Needs an idle signal from the agent |
| Memory ballooning, CPU and memory hotplug | 3 | Partly | Depends on guest |
| Keep host and guest awake while running | 2 | Yes | |
| Battery awareness, Pause All | 3 | Yes | |
| Nested virtualization | 2 | Partly | amd64 yes; arm64 only on FEAT_NV CPUs, not any Pi |

### Getting a system in

| Capability | M | Linux today | Note |
|---|---|---|---|
| Cards for Ubuntu, Fedora, Debian, Kali, always the newest image of each release | 2 | Yes | 4.1 |
| Any ISO of the host's architecture | 1 | Yes | |
| Install from ISO with detection of system and architecture | 1 | Yes | |
| Requirements check with one-click install | 2 | Yes | Through the Store |
| Automatic install, Linux guests | 2 | Yes | |
| virtio-win guest with optional unattended install, skipping Setup | 3 | Partly | amd64 and arm64; can't be promised across releases; no activation (4.3) |
| Drivers attached during install, tools at the end | 3 | Yes | Pinned versions |
| Migrate from a physical PC | 4 | Work | Driver injection is delicate |
| Import qcow2, VMDK, VDI, VHDX, OVA, other tools' bundles | 3 | Yes | Snapshots flattened, saved sessions lost (6.22) |
| Android guest | 4 | Partly | Own image needed |
| Guests tied to one maker's hardware | never | No | Not offered: neither licensed nor possible on these hosts |

### Display and graphics

| Capability | M | Linux today | Note |
|---|---|---|---|
| Picture in our compositor, no engine window | 1 | Yes | QEMU D-Bus display |
| Library card: lights, live picture, what it's doing, CPU and memory meters, disk space | 2 | Yes | 5.1 |
| Summary header: running, memory, free space, Disk, New | 2 | Yes | 5.1 |
| Settings on one screen with a summary per section | 2 | Yes | 5.5, replaces the roadmap's tabs |
| Window opens at the guest's size and keeps its shape | 2 | Work | Our window manager needs an aspect constraint |
| Resolution menu in the toolbar | 2 | Yes | Hidden while it follows the window |
| Start in full screen, in its own desktop | 2 | Work | Full screen exists; the start path is new |
| Resolution follows the window, or chosen from a menu | 2 | Yes | |
| Several displays | 3 | Partly | virtio-gpu heads; to test on our compositor |
| Picture in Picture | 3 | Yes | Our `setPip` |
| Apps mode, Linux guests | 4 | Work | Wayland forwarding |
| Apps mode, virtio-win guests | 5 | Work | Our own agent, hard |
| OpenGL in Linux guests | 3 | Partly | virgl; on arm64 hosts QEMU 10.0.13 aborts on UEFI boot, so off there for now (6.10) |
| Vulkan in Linux guests | 3 | Partly | venus, by GPU and QEMU version |
| 3D in virtio-win guests | none | No | No good free driver |
| GPU passthrough | 5 | Partly | PCs with a second GPU; never on a Pi |

### Bringing machines and host together

| Capability | M | Linux today | Note |
|---|---|---|---|
| Clipboard for all guests, by direction | 2 | Yes | Text first; images to test |
| Shared folders for all guests, read-only by default | 3 | Yes | virtiofs; on arm64 virtio-win guests, a host-only SMB share if that fails |
| Drag and drop into any guest, with progress, speed, time left | 3 | Work | Our agent |
| Drag out of virtio-win guests | 3 | Work | To test |
| Drag out of a Linux guest's own desktop | none | No | No agent can see the drag. Send to LordOS instead; Apps mode can |
| USB attach, auto-connect rules | 3 | Yes | usbredir |
| Sound | 2 | Yes | |
| Microphone and camera on request | 3 | Yes | |
| Printers | 3 | Partly | Driverless IPP |
| Guest apps in the launcher and Dock | 4 | Work | |
| Open host files with a guest app | 4 | Work | |
| Shell in a guest with no network | 2 | Work | Agent |
| Run commands from the host | 3 | Work | `lordvm` |
| Live logs | 2 | Work | Agent |
| MCP server, a coding agent builds a machine | 4 | Work | Off by default |

### Storage, snapshots, safety

| Capability | M | Linux today | Note |
|---|---|---|---|
| Thin disks, encryption, compaction | 2 | Yes | qcow2 |
| Snapshots with timeline, all disks, NVRAM and TPM | 2 | Partly | TPM in snapshots to test |
| Disposable sessions | 2 | Yes | Explicit overlays, not `-snapshot` |
| Linked clones running beside the original, only changed space | 2 | Yes | Reflink, or a frozen base with overlays on ext4; live freeze to test |
| Duplicates with the same identity | 2 | Yes | |
| Backup with schedule and consistency | 3 | Yes | Agent freeze |
| UEFI, secure boot, TPM 2.0 | 3 | Partly | amd64 yes; arm64 secure boot to test |
| Network: Shared, Bridged, Host only, Isolated | 1, 3 | Yes | Bridged needs Ethernet |
| Screenshot and recording with sound, from the VM's frame | 2 | Yes | |
| Storage window | 2 | Yes | 6.16 |

### Beyond the usual

What people don't normally get from a desktop virtualization app, and the reason for LordOS's to exist:

| Capability | M | Linux today | Why it's better |
|---|---|---|---|
| **Real storage accounting per machine** | 2 | Yes | Says what deleting would really free, what's shared, before you do it |
| **Live guest logs**, filter, pause, copy | 2 | Work | Debug from the host, plus the engine's log when the guest doesn't boot |
| **Agent state with a capability list** | 2 | Work | What works in this guest now and why not, from the guest itself |
| **A sandbox per machine** | 1 | Yes | Landlock, seccomp, cgroup scope, descriptors only: a broken engine is still shut in |
| **Permission-gated devices** | 2 | Yes | Camera, mic, USB, folders pass the system's permission flow, one screen shows all |
| **"Guest can see"** | 2 | Yes | One plain list of everything a guest can reach |
| **Machines in the firewall** | 2 | Yes | Each VM is an app in the firewall, the map and Monitor |
| **No engine window, no X11** | 1 | Yes | One compositor, theme, desktops and shortcuts for all |
| **Forwarded windows are marked** | 4 | Work | A guest's window can't pass as a host's |
| **Guest logs and files are treated as hostile** | 2 | Yes | Escapes stripped, quarantine marking, fuzzed agent protocol |
| **Same features, amd64 and arm64** | 1 | Partly | The app states every difference |
| **The compositor is protected from the engine** | 1 | Yes | A separate validator; the desktop survives a broken machine |
| **Files out of the engine's reach** | 3 | Yes | Shared folders are held by their own sandbox, not by QEMU |
| **A budget across all machines** | 1 | Yes | The desktop keeps its share, whatever is running |
| **Whole-machine encryption** | 3 | Partly | Memory, TPM, NVRAM and logs too, not only the disk |
| **Unified diagnostics** | 2 | Yes | One place: machine log, guest log, agent, Monitor and firewall agree on what a machine did |
| **Reproducible machines** | 4 | Work | A text file beside a project, `lordvm apply` |
| **Desktop integration** | 4 | Work | One compositor, permissions, Dock, Files and search, not an app beside the system |
| **Claims only after testing** | always | Yes | The app and this document say what wasn't tested |

## 17. What is impossible or weak

- KVM across architectures: impossible. Emulation only.
- Full modern desktop system for amd64 on a Pi: too slow to be useful.
- 3D in virtio-win guests: no good free driver.
- 3D or GPU video decoding in emulated guests: no.
- 3D in arm64 guests with Debian 13's QEMU: QEMU aborts when the guest resets its GPU, which every UEFI boot does (6.10).
- GPU or PCI passthrough on a Pi: impossible on this kernel and board.
- Nested virtualization on a Pi: not with these CPUs.
- Bridging over Wi-Fi: not with ordinary drivers.
- Moving a running VM between architectures: impossible. A VM stays on its architecture; a suspended one only resumes on a compatible CPU. Moving between Pi and PC needs emulation.
- A guest locked to its maker's hardware: not offered.
- Apps mode for virtio-win guests: not until we write the agent.
- Secure boot on arm64 guests: possible with AAVMF only if Debian's arm64 firmware build includes it, **To test**. If it doesn't, Setup has to be told to skip that check, and the app says so.
- Dragging files out of a Linux guest's own desktop: no agent can see the drag; see 6.21.
- The host always pays: suspended guests still hold disk equal to their RAM.

## 18. Open questions

Decided on 2026-11-01: settings on one screen replaces tabs; Kali is in the catalog, which will grow later; unattended install uses index selection first and the public generic key only as its fallback; no VNC: neither a machine's picture nor its clipboard goes through it.

1. App name and icon.
2. Default folder: `~/Machines`?
3. Store item (recommended) or preinstalled?
4. Do we build our own 4K kernel for the Pi if a measurement shows that it removes GICV trapping? Phase 0 didn't measure it; it moves to Phase 1 (13.8).
5. Keep Debian's QEMU (recommended) or carry our own build for features like newer venus fixes? Phase 0 adds a reason: the arm64 3D abort of 3.2.
6. Host our own Android image for Machines and the container, or leave Android to ISO?
7. ~~Is `passt` or QEMU's user networking the default?~~ Decided on 2026-11-04 by measurement: `passt` (6.5).
8. Does the first public release include virtio-win guests, or Linux only?
9. Does the catalog list only systems we can download freely, as recommended?
10. Is Migrate worth its cost?
11. Does `lordvm mcp` ship at all, given that it lets a coding agent run commands in guests? Off by default is the recommendation.
