# Virtualization

The design of **Machines** (working name), LordOS's desktop virtualization app: run other operating systems in windows of the
LordOS desktop, on the machine you have, at native speed where the hardware allows it, for free and with open source parts.
Nothing here is built yet. This is the design, and it follows the Virtual machines item in [ROADMAP.md](ROADMAP.md).

How to read it:

- **Measured** means checked on our own hardware or in the named package on 2026-11-01. **Reported** means a public source says so
  and we haven't reproduced it. **To test** means nobody here has run it. Any number under a "target" heading is a goal, not a result.
- "virtio-win guest" means a guest system that needs the virtio-win driver set. Public pages and the app use generic wording
  ("other systems"), as [the brand rule](../tools/brand-check.mjs) requires; a guest's real name appears only as the label of its
  download or its installer.

## 1. Goals

The bar is section 15: the most complete desktop virtualization app, and visually the best, with every claim marked for what Linux allows today.

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
| Raspberry Pi 5 | arm64 | KVM, GICv2 | near native CPU, slower interrupts | see 2.2, to test |
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
exposes nested virtualization. Without it the app offers TCG and says it will be slow.

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
  kernel is fine. We couldn't find a report that states it, so **it stays To test** and is the first thing Phase 0 does.
- **The price of 16K.** The vGIC warning comes from a kernel check of the GICV region's alignment (**Reported**, the 2016 patch series).
  When it fails, each guest access to the virtual CPU interface traps and is emulated: interrupt-heavy work costs more. Whether the
  firmware's 4K kernel (`kernel8.img`) avoids it is **To test**: if it does, the app can recommend, never force, a 4K boot for people
  who use VMs a lot. Our LordOS kernel has no 4K twin at this moment: the board only has the Debian-style `rpi-v8` kernel, and a
  4K LordOS build would be new work, and Pi 4 path differences apply (no Landlock on the archive kernel).
- **4 vCPUs, always.** The board has 4 Cortex-A76 cores; GICv2 itself stops at 8 CPUs, so it's not the limit here. The default for a
  guest is 3, leaving one for the desktop.
- **Memory.** 16 GB board: 15 GB visible to Linux on ours. Suggested guest maximum: board RAM minus 2 GB. 2 GB and 4 GB boards can run a
  small Linux, not a full desktop system. The kernel's guest address limit is far above any Pi.
- **No device passthrough.** There's no VFIO, and no IOMMU path worth using: PCIe, GPU and NVMe stay with the host.
- **No nested virtualization.** The kernel logs its nested trap tables, but Cortex-A76 is ARMv8.2 without FEAT_NV, so the kernel
  can't expose EL2 to a guest. KVM inside a Pi guest doesn't work. To verify with `KVM_CAP_ARM_EL2` in Phase 0.
- **Storage.** A VM on a microSD card is painful. The app warns and suggests NVMe or USB 3 SSD, and refuses nothing.
- **Temperature and power.** A VM with all cores busy is a constant load: the Cooling view and the board's fan curve matter, and the
  voltage view applies. A proper power supply is a requirement.

### 2.3 Raspberry Pi 4

Cortex-A72, ARMv8.0, GICv2, so no VHE: KVM runs split between a small hypervisor at EL2 and the kernel (**To test**, from the
architecture, not from a measurement). Slower exits, 8 GB at most. A public project limits a virtio-win guest on boards without A76
cores to an older release, because the newest needs CPU instructions these cores lack (**Reported**). Plan: Linux guests only, best
effort, with no support claim until tested. The Pi 4 path also uses an archive kernel without Landlock, so the sandbox of 6.2 is
weaker there; the app shows that in Security.

### 2.4 Emulation (TCG): honest numbers

The roadmap's 5–20× is a fair planning range, not a measurement of ours. QEMU's own measurements give several host instructions per
guest instruction before counting devices (one published report: about 6.5:1 on an x86-64 target, **Reported**), and memory-ordering
between architectures costs more on multi-core guests. Reported Pi 5 experience: a tiny amd64 Linux is slow to boot, then fine.

Decisions:

- Another architecture is available, marked **Emulated**, with a line under it: "Runs much slower than a native machine."
- It isn't in the assistant's cards by default: it sits behind "Other architecture…".
- A modern desktop system for amd64 on a Pi is not offered as a card at all: it would not be usable, and we'd have to claim
  otherwise. It remains possible from an ISO, with the warning.
- GPU acceleration is off in emulated guests.
- Phase 0 records real numbers per board in this document.

## 3. Architecture

```
lord-server (the person's user, never root)
  └─ Machines app (React, sdk/vm.js → lord.vm.*)
       │  closed RPC
       ▼
  lord-vm  one per running VM: supervisor, sandboxed, owns the VM's folder
    ├─ qemu-system-<arch>  device engine, in its own sandbox and cgroup scope
    ├─ swtpm               TPM state, when the VM has one
    ├─ virtiofsd           one per shared folder
    ├─ passt or tap helper network
    └─ usbredir            attached device, on demand
       ▲
       │ p2p D-Bus: ScanoutDMABUF, keyboard, pointer, clipboard, audio, chardev
       ▼
  lord-server's compositor: the dmabuf is a media node (MediaView)

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
(**Measured**, Debian's file list, amd64; arm64 to confirm).

So one socket carries picture, input, clipboard, sound and USB, and `lord-server` needs one client for all of them. It speaks that
D-Bus protocol on a private socket; no bus daemon is involved. The media node path (`MediaView`) already shows dmabufs and shared-memory
pictures from `lord-media` and `lord-rdesk`.

- With GPU: a dmabuf goes straight to the compositor with no copy. **Target:** one frame of added latency at most.
- Without GPU acceleration (2D guests, TCG, virtio-win guests): the shared-memory path, copied on update, with damage rectangles.
- Multi-head guests: one console per head, one window or desktop per head.

Multi-plane dmabufs need `ScanoutDMABUF2`, added in 2025 (**Reported**): to check in 10.0.

### 3.3 Sandbox

QEMU parses guest-controlled data all day, so **assume it can be compromised**. What a compromised QEMU may reach is the design:

- Own user namespace if available, no capabilities, `no_new_privs`, in `app-lordos-vm-<id>.scope` with memory and CPU limits.
- **Landlock:** read-only `/usr/lib`, firmware and ISOs chosen for this VM; read-write only that VM's folder. On kernels without
  Landlock (see 2.3) the app shows "Weaker isolation on this kernel".
- **Seccomp:** QEMU's own `-sandbox on,obsolete=deny,elevateprivileges=deny,spawn=deny,resourcecontrol=deny` plus ours. Our allowlist needs care: QEMU uses many
  ioctls on `/dev/kvm`, and `lord-server`'s helper filter can't be reused unchanged.
- `/dev/kvm`, `/dev/vhost-net`, render node and tap are opened by `lord-vm` and passed as descriptors, so QEMU can't open devices.
  QEMU's `-add-fd` exists for this.
- No network sockets of its own: networking is an fd.
- Files are opened by the supervisor and passed by descriptor wherever QEMU allows (`/dev/fdset`).
- No shell, no `-readconfig` from the guest's folder: arguments are built from validated settings.
- One VM can't read another's folder: one process tree and one Landlock domain per VM.
- A guest's disk file isn't encrypted by default; see 8.

### 3.4 Access to `/dev/kvm`

`/dev/kvm` is `root:kvm 0660` and our session user isn't in `kvm` (**Measured**). Options: add the user to `kvm` (a closed admin action,
`vm-enable`, asked once, new login needed) or let the seat rule grant it. Our sessions come from `lord-sessiond`, so seat rules must be
checked. Decision: `vm-enable`, asked the first time, one question with one sentence of explanation.

### 3.5 Files on disk

```
~/Machines/<name>.lvm/
  machine.json       settings, schema-versioned
  disks/system.qcow2
  disks/data-1.qcow2
  nvram.fd           UEFI variables
  tpm/               swtpm state
  thumbnails/        last picture
  snapshots.json     names and notes; the snapshots themselves are in the qcow2
  log/               last three runs
```

The folder is the VM: copy it to another LordOS machine and it opens. Files shows it as one item. `machine.json` never holds passwords.
Keys go to the keyring.

## 4. Guest systems

### 4.1 Linux

Every current Linux has virtio drivers in its kernel. Download cards use verified images: each has a URL, checksum source and signing
key in a signed catalog, as `apt/catalog.json` does for apps. arm64 images exist for Ubuntu, Debian and Fedora; on a Pi the UEFI
arm64 ISO is the one to use, not the Pi's image. LordOS itself is a guest too, since we build UEFI ISOs for both architectures.

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

### 4.4 Not offered

Guests whose license restricts them to their maker's hardware. Guests needing a real GPU for their desktop on hardware without passthrough.

## 5. The app

Name and icon: a generic name, ours (open decision 1). The icon is a screen holding a smaller rounded tile, on the LordOS shape (radius
0.225). Mocks: https://claude.ai/artifact/WNiELGEazSqnr68SFWuyPz (private until shared).

### 5.1 Control Center (main window)

A grid of machines, each with a **live thumbnail** while running and the last picture when off.

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
   - "Import a machine" (`.lvm`, OVA, qcow2, vmdk, vdi, vhdx).
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

### 5.5 Configuration (tabs)

Reachable from the bar and from `⋯`. Tabs with a form each; changes that need a power-off are marked and applied at the next boot.

**General:** name, icon, description, tags, system, architecture (read only), folder, created, disk used, Reclaim space.

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
| Productivity | half | 8 GB or 1/3 | 2D | Folders on, clipboard both, Apps mode, suspend on close |
| Software Development | three quarters | 16 GB or 1/2 | 2D | Port forwarding UI, serial console, nested if available, snapshots on |
| Testing | half | 4 GB | 2D | Isolated network, snapshot before each boot, nothing shared, disposable mode |
| Design | three quarters | 16 GB or 1/2 | 3D | Color-accurate full screen, tablet pointer |
| Games | all but one | 2/3 | 3D (venus) | Hugepages, Faster guest, capture on, low-latency audio |

Only what the host can do appears: a Pi has no Games profile, shows a "Light desktop" one instead, and a virtio-win guest has 2D only so Design and Games say so.

### 5.7 Menus, shortcuts

Follows `docs/SHORTCUTS.md`: app menus, ❖ shortcuts for the host while the app has focus outside capture, all passed to the guest while captured. The release combination is shown in the bar and is configurable. In Lord Classic the same table.

## 6. Features

### 6.1 Lifecycle

Start, pause (vCPUs stop, instant), suspend (RAM to the folder, survives a host reboot), shut down (ACPI, then agent), force off. Host shutdown asks each running VM to suspend. A crashed VM shows its log and a "Start again" button. Devices that can't be saved (sound, for example) are disconnected on suspend and connected again on resume. A suspended VM may not resume after QEMU is updated: the Store warns before such an update, and the VM's state says so instead of failing silently.

### 6.2 Snapshots

qcow2 internal snapshots, with RAM optional. Timeline view with names, notes, thumbnails and size. Revert asks and offers to snapshot first. "Disposable" mode: all changes are discarded at shutdown (`-snapshot`). Auto-snapshot before a guest's tools upgrade. Weak: internal snapshots slow down with many; keep under 20 with a warning. Snapshots and backups aren't the same: Configuration says so.

### 6.3 Clones

Two kinds, named for what they do:

- **Clone:** can run beside the original. It gets a new MAC and, through the agent, a new `machine-id` and hostname, which avoids the usual trap of two VMs receiving one DHCP address. Virtio-win guests keep their computer name unless changed.
- **Duplicate:** an exact copy with the same identity, the only kind that can resume a suspended session.

Storage: reflink on btrfs and XFS (instant, shared until changed), else a qcow2 backing file, where the source locks read-only, or a full copy. The app picks by file system and says which one it used. Templates are read-only machines made to clone.

### 6.4 Storage

- **qcow2** by default: thin, snapshots, compression, encryption.
- **raw** optional: fastest, no snapshots.
- **Reflink** on btrfs and XFS: instant copies.
- `io_uring` where seccomp and kernel allow; `virtio-blk` by default for speed, `virtio-scsi` for discard and many disks. `iothread` per disk.
- TRIM from guest is passed to shrink. "Reclaim space" compacts offline.
- Import: `qemu-img convert` from vmdk, vdi, vhdx, OVA unpack. Export: same, as a `.lvm` bundle or OVA.

### 6.5 Network

| Mode | How | Reach | Notes |
|---|---|---|---|
| **Shared** (default) | `passt` user-space, no root | Guest reaches the world, world doesn't reach guest | Port forwarding by rules |
| **Bridged** | tap on a bridge, closed admin action | Guest on the LAN with its own address | Ethernet. Wi-Fi can't bridge: says so |
| **Host only** | private bridge | Host and other VMs | No internet |
| **Isolated** | no NIC | Nothing | Testing |

Debian 13 has `passt` (**Measured**, 2025-05-03 snapshot); Phase 0 compares it with QEMU's `-netdev user`. `vhost-net` for Bridged. Centinela sees each VM as an app: its traffic in Shared mode appears in the firewall, the map and Monitor as "Machine: name". A rule can block it. Bridged traffic bypasses this: Security says so.

### 6.6 USB

`usbredir` over the Chardev stream: no root, device opened by a closed admin action `vm-usb-allow`, which claims the single device for that VM and writes nothing permanent. Menu lists devices with names. Auto-connect rules per VM. USB 3 through `qemu-xhci`. Weak: isochronous devices (webcams, audio) can stutter; Camera and Microphone have their own paths. Never offered: the boot disk or the keyboard in use.

### 6.7 Shared folders

`virtiofsd` 1.13.2 (Debian, **Measured**), one per folder, sandboxed to that folder. Linux guests mount it natively. virtio-win guests use the virtio-win virtiofs driver plus WinFsp (a separate dependency to review). Read-only by default, as in Remote Desk. On arm64 guests availability is **To test**. Fallback: a small SMB share reachable only by that guest. Pi kernels need `CONFIG_FUSE_FS` and shared memory backing, checked in Phase 0.

### 6.8 Clipboard and drag

Clipboard through `org.qemu.Display1.Clipboard` (text, images), direction chosen per VM, **Off for Testing**. Protected windows (`wm.setProtected`) never leak. File drag: into the guest copies through the agent; out copies from the guest. Linux: our `lordos-guest`. virtio-win guests: SPICE agent, `spice-vdagent` with the dbus Clipboard path needs confirming: QEMU's clipboard needs `qemu-vdagent` chardev (**To test**).

### 6.9 Sound, camera, microphone

Sound via `audio-dbus`, into PipeWire by `lord-server`. Microphone and camera: asked once per VM with the existing Permissions flow; camera through USB redirect or `uvc` emulation. Off by default.

### 6.10 Graphics

| Guest | Host GPU | Result |
|---|---|---|
| Linux | Any Mesa GPU | virgl: OpenGL; venus: Vulkan, needs blob resources, guest kernel ≥ 5.16 (**Reported**, Mesa docs) |
| Linux | Pi V3D | virgl **To test**; venus **To test**: V3D's Vulkan features may not be enough |
| virtio-win | Any | 2D only: no good free 3D driver exists |
| Emulated | Any | 2D |

Reported notes: Intel Meteor Lake or `xe` hosts need kernel 6.16+ and QEMU 11.0+ with `honor-guest-pat=on` for venus; Debian 13 has 10.0 (**Measured**), so venus on those hosts waits for a newer QEMU. NVIDIA proprietary needs 570.86+ for venus (**Reported**). Recent security fixes around virtio-gpu blob scanouts are a reason to keep 3D opt-in. In the sandbox, 3D gives QEMU a render node: a larger attack surface, stated in Security. `vhost-user-gpu` would put rendering in its own process: to evaluate in Phase 3.

### 6.11 Migrate from another PC

A small agent runs on the old PC, or the disk is read from a backup image, or the disk is connected. Copies the disk over LAN to a new VM and injects virtio drivers so that it boots. Weak: driver injection into a virtio-win guest is delicate; activation may need the person's attention; full-disk encryption needs the recovery key; arm64 hosts can't run an amd64 migrated system usefully, and the card says so. Phase 5.

### 6.12 Guest app integration

- Guest's apps in the launcher and Dock, with icons, marked with a small badge. Launching starts or resumes the VM.
- Open a LordOS file with a guest app: Files' Open With, through the shared folder.
- Reverse: open a link or file from the guest on the host: optional.
- Guest notifications to the host: optional, off by default.

### 6.13 Nested virtualization

Inside a guest: amd64: on by default for Software Development (`kvm_intel nested`, `kvm_amd nested`). arm64: kernel 6.16 enabled it for hardware that has it, off by default, through `KVM_ARM_VCPU_HAS_EL2` (**Reported**); needs FEAT_NV2, so not on Pi 5 or Pi 4. LordOS itself as a guest gets this for free where the host does.

### 6.14 Monitor and system ties

Monitor shows each VM as one row with a drill-in. Cleaner never touches `~/Machines` without explicit choice, and offers Reclaim space. Backups skip running VM disks unless consistent. Files shows `.lvm` as a bundle. Search finds VMs by name. Desktops treat each VM window normally. Power: laptops on battery get a notification with Pause All; a guest's load keeps the host awake while it runs, on purpose, and the battery warning says so.

### 6.15 Command line and coding agents

`lordvm list|start|stop|suspend|snapshot|clone|console <name>`, plus through the agent:

```sh
lordvm shell "Debian Dev"                 # an interactive shell
lordvm exec "Debian Dev" -- uname -m      # exits with the command's status; stdin and stdout are piped
lordvm logs "Debian Dev"                  # follows until Ctrl+C
lordvm cp ./build "Debian Dev":~/          # with progress, in either direction
```

Same rules as the app: one small tool over the same socket, no shortcut around sandbox or admin actions, reachable only by the person's own user.

A local **MCP server** (`lordvm mcp`), off until turned on in Settings, lets a coding agent list, create, start, stop, clone, screenshot and run commands in VMs. It speaks only over stdin of a process the person started. It can't touch a VM marked **No automation**, which is the default for profiles with sensitive data, and every action it takes is written to the VM's log. Destructive tools (delete, revert) ask the person on screen.

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

The Shell icon opens LordOS's Terminal with a shell inside the guest: over the agent, not the network, so it works with no SSH and with Isolated network. A root shell in Linux guests, PowerShell in virtio-win guests, or the signed-in user when chosen in Configuration. The tab is titled with the machine's name and marked, so that a guest's shell can't be mistaken for a host's. Terminal size follows the tab.

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
- Flow control by acknowledgement keeps memory flat for any file size. **Target:** 300 MB/s on Linux guests with NVMe on a PC, measured in Phase 2.
- Resume after a dropped connection, by offset and hash. Optional end-to-end check, on by default for files over 1 GB.
- Names are sanitized for the guest's rules. Symbolic links aren't followed; permissions and dates carry over where the file system can.
- **Out of the guest:** in Apps mode, windows are ours, so dragging out works. From a Linux guest's own desktop, no agent can see the drag, so the agent adds **Send to LordOS** to the guest's file manager, plus `lordvm cp`. The panel says this when someone tries. Virtio-win guests allow drag-out through the agent, **To test**.
- Scanning: files coming out of a guest get the same quarantine marking as downloads.

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

| Piece | Phase 1 | Our own later | Cost of our own |
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

**virtio-win guests:** no free way exists. Our own agent would enumerate windows and stream regions. Phase 6 at the earliest, and only if real demand exists. Until then Apps mode is off there, and the setting says so.

Each forwarded window carries a thin marker in the frame with the machine's name, so that a person can always tell a guest's window from a host's, and it can't request protected-window status or the host's secure input.

## 8. Privacy and security model

- Offline by default, except downloads the person starts. No telemetry.
- Everything shared is **off** until asked, except the Productivity profile, which states what it turns on in the Review step.
- Guest → host channels: virtio devices, vsock to `lordos-guest`, clipboard, folder, usbredir. Each has an on/off and shows in **Guest can see**.
- `lordos-guest` traffic is treated as untrusted: strict parser, size limits, fuzz target. A guest can't start a host process, read host files beyond its folder, or inject input into other windows.
- Disk encryption: qcow2 LUKS with the key in the keyring, unlocked with the person's password. Bridged mode and encrypted disks each state what they mean in one line.
- `vm-enable`, `vm-bridge-on/off`, `vm-usb-allow` are closed actions with fixed arguments, added to the list maintainers review.
- Catalog of images and tools: signed with the LordOS release key path, verified before use.
- A hostile ISO can only attack the guest, then QEMU, which is why the sandbox exists. QEMU updates arrive with Debian's.

## 9. Performance targets (none measured yet)

| Measure | amd64 host | Pi 5, NVMe |
|---|---|---|
| CPU work vs bare metal | ≥ 95 % | ≥ 90 % |
| Disk sequential, qcow2 vs host | ≥ 80 % | ≥ 70 % |
| Network, virtio Shared mode | ≥ 2 Gb/s | ≥ 500 Mb/s |
| Boot of a small Linux guest | ≤ 8 s | ≤ 15 s |
| Pause, resume | ≤ 100 ms | ≤ 100 ms |
| Suspend, resume with 8 GB | ≤ 10 s | ≤ 20 s |
| Added display latency | ≤ 1 frame with dmabuf | ≤ 2 frames |
| Idle host CPU per idle guest | ≤ 2 % of a core | ≤ 3 % of a core |
| App memory, no VM running | ≤ 40 MB | ≤ 40 MB |

The LordOS rule holds: this app must not slow the desktop. It's idle until opened and runs nothing in the background without a running VM.

## 10. Packaging

- **Machines** is a Store item, free, not preinstalled: QEMU and firmware are large and most people won't use them. A Dock entry appears once installed. Open decision 3.
- Package: `lordos-machines` (app, `lord-vm`), with `Depends: qemu-system-<native arch>`, `qemu-system-modules-opengl`, `qemu-utils`, `swtpm`, `virtiofsd`, UEFI firmware.
- Emulation: a second switch on the Store page, as Disk Formats has for APFS: `qemu-system-<other arch>`.
- Sizes: `qemu-system-x86` is 68 MB (**Measured**). The whole set, with dependencies, is **To measure**: plan on 300 to 500 MB, not including any image. Each guest wants 20 to 64 GB.
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
- **Leak test:** start and stop 100 times: no orphan processes, scopes, sockets, tap devices.

## 13. Phases

| Phase | Ships | Done when |
|---|---|---|
| **0, Proof** | No app. Answers 2.2's open items: 4K guest on 16K host, Pi GICv2, GICV trapping by kernel, TCG numbers, `ui-dbus` on arm64, nested checks | Numbers are in this document |
| **1, One machine** | `lord-vm`, sandbox, dbus display in a window, Linux from ISO, Shared network, qcow2 | Debian guest on Pi 5 and amd64 |
| **2, The app** | Control Center, assistant with Linux cards, requirements check, tabs, profiles, snapshots, suspend, clone, agent with state popover, Shell, Logs, file copy panel, Storage window, screenshot and recording, resolution menu, Keep Running, clipboard, folders, USB | A person does it all with no terminal |
| **3, Other systems and 3D** | UEFI secure boot, TPM, virtio-win flow on both architectures with automatic setup, agent on those guests, virgl, venus | Pinned drivers pass the install test |
| **4, Polish** | Picture in Picture, multi-head, Bridged, Host only, export and import, backups, Monitor, `lordvm` with shell, exec, logs and cp, local MCP server | Security review done |
| **5, Apps and migration** | Linux Apps mode, guest apps in the Dock, Migrate, drag files, Android card if the image exists | Each on tested hardware |
| **6, Later** | Our own agent for other guests' windows; a lightweight engine for Linux guests, like crosvm; GPU passthrough on PCs | Decided then |

## 14. Risks

| Risk | Effect | Plan |
|---|---|---|
| QEMU's seccomp plus ours break with updates | VMs refuse to start after a Debian update | Boot matrix on each QEMU update, staged Store update |
| A guest OS update stops booting on GICv2 or old CPUs | Pi virtio-win guests break | Test on each release, keep snapshot before updates, state clearly it's best effort |
| Upstream virtio-win regressions | Failing installs | Pinned versions |
| GPU drivers and venus: fragile | Crashes on some GPUs | 3D opt-in, per-GPU allow list, 2D fallback |
| Scope | It's the biggest app on the roadmap | Phases ship on their own |
| Dependence on 16 KiB behaviour | Pi path rests on To test items | Phase 0 first; nothing announced before |
| Host page and swap pressure | Host starves | Memory limits, balloon, warning at 80 % |
| Sandbox on Pi 4 kernel | Weaker isolation | Stated in Security |
| WinFsp and agents add third-party code in guests | Maintenance | Pinned, verified, opt-in |

## 15. Feature matrix

The bar: **the most complete desktop virtualization app there is, and the best looking**, on free parts. Ambition is kept honest here, one line per capability.

**Linux today** says whether the kernel and existing open source parts allow it now: **Yes**; **Work** (possible, we must build a real piece); **Partly** (some guests or hosts only); **No**. **M** is the phase of section 13 that ships it. Nothing here is built yet, so "Yes" never means "done".

### Running machines

| Capability | M | Linux today | Note |
|---|---|---|---|
| Native speed through KVM, amd64 and arm64 | 1 | Yes | Pi 5 details in 2.2 |
| Other architecture by emulation, labelled | 1 | Yes | 2.4 |
| Pause, suspend to disk, resume | 2 | Yes | |
| Keep running with no window, start with no window | 2 | Yes | |
| Start with the session, order and delay | 4 | Yes | |
| Auto-pause when idle | 4 | Work | Needs an idle signal from the agent |
| Memory ballooning, CPU and memory hotplug | 4 | Partly | Depends on guest |
| Keep host and guest awake while running | 2 | Yes | |
| Battery awareness, Pause All | 4 | Yes | |
| Nested virtualization | 2 | Partly | amd64 yes; arm64 only on FEAT_NV CPUs, not any Pi |

### Getting a system in

| Capability | M | Linux today | Note |
|---|---|---|---|
| Verified download cards | 2 | Yes | Signed catalog |
| Install from ISO with detection of system and architecture | 1 | Yes | |
| Requirements check with one-click install | 2 | Yes | Through the Store |
| Automatic install, Linux guests | 2 | Yes | |
| Automatic install, virtio-win guests, local account | 3 | Yes | No activation of anything |
| Drivers attached during install, tools at the end | 3 | Yes | Pinned versions |
| Migrate from a physical PC | 5 | Work | Driver injection is delicate |
| Import bundles and foreign disk formats | 4 | Yes | `qemu-img` |
| Android guest | 5 | Partly | Own image needed |
| Guests tied to one maker's hardware | never | No | Not offered |

### Display and graphics

| Capability | M | Linux today | Note |
|---|---|---|---|
| Picture in our compositor, no engine window | 1 | Yes | QEMU D-Bus display |
| Resolution follows the window, or chosen from a menu | 2 | Yes | |
| Window keeps guest's shape | 2 | Yes | |
| Full screen, one desktop per guest | 2 | Yes | |
| Several displays | 4 | Partly | virtio-gpu heads; to test on our compositor |
| Picture in Picture | 4 | Yes | Our `setPip` |
| Apps mode, Linux guests | 5 | Work | Wayland forwarding |
| Apps mode, virtio-win guests | 6 | Work | Our own agent, hard |
| OpenGL in Linux guests | 3 | Yes | virgl |
| Vulkan in Linux guests | 3 | Partly | venus, by GPU and QEMU version |
| 3D in virtio-win guests | none | No | No good free driver |
| GPU passthrough | 6 | Partly | PCs with a second GPU; never on a Pi |

### Bringing machines and host together

| Capability | M | Linux today | Note |
|---|---|---|---|
| Clipboard, by direction | 2 | Yes | |
| Shared folders, read-only by default | 2 | Yes | virtiofs; arm64 virtio-win to test |
| File copy with progress, speed and time left | 2 | Work | Our agent |
| Drag out of a Linux guest's own desktop | none | No | Replaced by Send to LordOS |
| USB attach, auto-connect rules | 2 | Yes | usbredir |
| Sound | 2 | Yes | |
| Microphone and camera on request | 3 | Yes | |
| Printers | 4 | Partly | Driverless IPP |
| Guest apps in the launcher and Dock | 5 | Work | |
| Open host files with a guest app | 5 | Work | |
| Shell in a guest with no network | 2 | Work | Agent |
| Run commands from host scripts | 4 | Work | `lordvm` |
| Local MCP server for coding agents | 4 | Work | Off by default |

### Storage, snapshots, safety

| Capability | M | Linux today | Note |
|---|---|---|---|
| Thin disks, encryption, compaction | 2 | Yes | qcow2 |
| Snapshots with timeline | 2 | Yes | |
| Disposable sessions | 2 | Yes | |
| Clones, linked or reflinked | 2 | Yes | By file system |
| Duplicates with the same identity | 2 | Yes | |
| Backup with schedule and consistency | 4 | Yes | Agent freeze |
| UEFI, secure boot, TPM 2.0 | 3 | Partly | amd64 yes; arm64 secure boot to test |
| Network: Shared, Bridged, Host only, Isolated | 2, 4 | Yes | Bridged needs Ethernet |
| Screenshot and recording, from the VM's frame | 2 | Yes | |

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
| **Machines in the firewall** | 4 | Yes | Each VM is an app in the firewall, the map and Monitor |
| **No engine window, no X11** | 1 | Yes | One compositor, theme, desktops and shortcuts for all |
| **Forwarded windows are marked** | 5 | Work | A guest's window can't pass as a host's |
| **Guest logs and files are treated as hostile** | 2 | Yes | Escapes stripped, quarantine marking, fuzzed agent protocol |
| **Same features, amd64 and arm64** | 1 | Partly | The app states every difference |
| **Claims only after testing** | always | Yes | The app and this document say what wasn't tested |

## 16. What is impossible or weak

- KVM across architectures: impossible. Emulation only.
- Full modern desktop system for amd64 on a Pi: too slow to be useful.
- 3D in virtio-win guests: no good free driver.
- 3D or GPU video decoding in emulated guests: no.
- GPU or PCI passthrough on a Pi: impossible on this kernel and board.
- Nested virtualization on a Pi: not with these CPUs.
- Bridging over Wi-Fi: not with ordinary drivers.
- Moving a running VM between architectures: impossible. A VM stays on its architecture; a suspended one only resumes on a compatible CPU. Moving between Pi and PC needs emulation.
- A guest locked to its maker's hardware: not offered.
- Apps mode for virtio-win guests: not until we write the agent.
- Secure boot on arm64 guests: possible with AAVMF only if Debian's arm64 firmware build includes it, **To test**. If it doesn't, Setup has to be told to skip that check, and the app says so.
- Dragging files out of a Linux guest's own desktop: no agent can see the drag; see 6.21.
- The host always pays: suspended guests still hold disk equal to their RAM.

## 17. Open questions

1. App name and icon.
2. Default folder: `~/Machines`?
3. Store item (recommended) or preinstalled?
4. Do we build our own 4K kernel for the Pi if Phase 0 shows that it removes GICV trapping?
5. Keep Debian's QEMU (recommended) or carry our own build for features like newer venus fixes?
6. Host our own Android image for Machines and the container, or leave Android to ISO?
7. Is `passt` or QEMU's user networking the default? Phase 0 decides by measurement.
8. Does the first public release include virtio-win guests, or Linux only?
9. Does the catalog list only systems we can download freely, as recommended?
10. Is Migrate worth its cost?
11. Does `lordvm mcp` ship at all, given that it lets a coding agent run commands in guests? Off by default is the recommendation.
12. Who may use the agent's shell: a root shell by default in Linux guests is convenient, and strong.
