# Virtualization

The design of **Machines**, LordOS's desktop virtualization app: run other operating systems in windows of the
LordOS desktop, on the machine you have, at native speed where the hardware allows it, for free and with open source parts.
This is the design, and it follows the Virtual machines item in [ROADMAP.md](ROADMAP.md). Phases 0 to 2, most of Phase 3 and part
of Phase 4 are built and released (latest release 0.8.9.5-beta); see the "Status checklist" below, audited against the code.

How to read it:

- **Measured** means checked on our own hardware or in the named package on 2026-11-01, or, where it says so, in Phase 0 on
  2026-11-04 on the machine named (section 13). A number marked **nested** was taken on an amd64 host that is itself a virtual
  machine, so it is not a bare-metal figure. **Reported** means a public source says so
  and we haven't reproduced it. **To test** means nobody here has run it. Any number under a "target" heading is a goal, not a result.
- "virtio-win guest" means a guest system that needs the virtio-win driver set. Public pages and the app use generic wording
  ("other systems"), as [the brand rule](../tools/brand-check.mjs) requires; a guest's real name appears only as the label of its
  download or its installer.


Mocks of every screen: https://claude.ai/artifact/WNiELGEazSqnr68SFWuyPz (private to the maintainer).

## Status checklist (audited 2026-11-24, against commit bb5baab62)

Every feature this document describes, checked against the source code of `test/integration-1125` at `bb5baab62`. The code is
the truth: where this document said otherwise, it was fixed in the same commit. This is the third audit; the second was
against `965a10f6e` on 2026-11-23 (68 %), before the Windows Search pause and the Linux guests' performance profile, huge
pages for shared memory, the disk cache measurement, the LordOS list from lordos.org, protected windows, Sentinela's watch
of the machines, QEMU's namespaces and seccomp allowlist and the Whonix machine group (19) landed. Latest release:
**0.8.9.5-beta**; what came after it is marked *in development*. Marks: ✅ done (in code, with a test or a recorded
on-screen check) · 🧪 done, but not checked on real hardware (headless, nested or `lordvm-dev` only) · 🟡 partial (what's
missing is named) · ❌ not built. The percentage is how much of the item exists in code. The same table, machine-readable:
[machines-checklist.json](machines-checklist.json).

### How the score is made

Each area gets a weight by how much a person using Machines feels it (sum 100). Its score is the mean of its items'
percentages, rounded half up. The total is Σ weight × score / 100, rounded. The script that writes this table and the JSON
recomputes both from the rows; the arithmetic:

| Area | Weight | Items | Mean | Score | Points (weight × score) | Before |
|---|---:|---:|---:|---:|---:|---:|
| A. Engine, lifecycle, sandbox, display | 14 | 22 | 85.91 | 86 % | 14 × 86 % = 12.04 | 78 % |
| B. Getting a system in (cards, unattended, import) | 12 | 13 | 87.85 | 88 % | 12 × 88 % = 10.56 | 76 % |
| C. The app (library, assistant, window, Configure, profiles) | 11 | 12 | 84.58 | 85 % | 11 × 85 % = 9.35 | 75 % |
| D. Snapshots, clones, storage | 9 | 6 | 60.00 | 60 % | 9 × 60 % = 5.40 | 60 % |
| E. Network | 6 | 4 | 80.00 | 80 % | 6 × 80 % = 4.80 | 80 % |
| F. Devices (USB, sound, microphone, camera, printers) | 6 | 5 | 56.00 | 56 % | 6 × 56 % = 3.36 | 56 % |
| G. Host integration (folders, transfer, clipboard, drag, Open with, apps mode) | 12 | 9 | 76.67 | 77 % | 12 × 77 % = 9.24 | 68 % |
| H. Graphics (virgl, GVT-g, venus, passthrough) | 7 | 8 | 46.25 | 46 % | 7 × 46 % = 3.22 | 46 % |
| I. Guest tools and agent (state, shell, logs) | 7 | 6 | 90.00 | 90 % | 7 × 90 % = 6.30 | 75 % |
| J. Command line and automation | 3 | 2 | 92.50 | 93 % | 3 × 93 % = 2.79 | 48 % |
| K. System ties (Monitor, power, Cleaner, Backups, Sentinela) | 3 | 4 | 83.75 | 84 % | 3 × 84 % = 2.52 | 61 % |
| L. Privacy and security model | 4 | 7 | 73.57 | 74 % | 4 × 74 % = 2.96 | 74 % |
| M. Performance (targets and the six items) | 3 | 7 | 85.71 | 86 % | 3 × 86 % = 2.58 | 86 % |
| N. Tests and validation on hardware | 3 | 5 | 55.00 | 55 % | 3 × 55 % = 1.65 | 55 % |
| **Total** | **100** | **110** | | | **76.77 → 77 %** | 69 % |

Σ = 12.04 + 10.56 + 9.35 + 5.40 + 4.80 + 3.36 + 9.24 + 3.22 + 6.30 + 2.79 + 2.52 + 2.96 + 2.58 + 1.65 = 76.77, rounded **77 %** (before: 69 %, the third audit against `bb5baab62`). The rows of the app and the system ties (C, K and four of A's) were changed with the code that built them (*in development*, "Machines to 100 %"); the table and the JSON are computed again from the rows in one pass.

An item's share of the total is its area's weight divided by the area's item count, so finishing it adds weight × (100 − %)
/ items. What would raise the total most: the `lordvm` tools and MCP (J, +1.5); raw disks, encryption, export and backups
(D, +1.5); the rest of guest → host links (G, +0.3); the file copy panel with drag out, USB, then camera and printers (G and F, +1.2
each); disposable sessions for any machine (D, +1.1); the rest of the agent shell (I, +0.2); Picture in Picture with several displays
(C, +0.9); venus, whole-GPU passthrough and several damage rectangles (H, +0.9 each); Migrate from another PC (B, +0.9);
virtio-win guests on arm64 (B, +0.8).

**What this audit changed.** From the code since `965a10f6e`: the performance profile reaches Linux guests and pauses
Windows Search (M item 5, 55 → 85 %; `lordos-guest` 7, I); shared memory's huge pages (M item 2, 85 → 95 %); the disk path
kept after measuring the host cache (M item 4, 80 → 85 %); the LordOS card's list from lordos.org (B, 80 → 85 %); the
Whonix-only disposable session in memory (D, 0 → 25 %: the feature for any machine isn't built); Sentinela's "Weaker
isolation" alert (A, 0 → 20 %). Ratings the code didn't support: the Whonix group row (B, 85 → 80 %: no Delete, Rename or
Repair of a group, no Start Gateway bar, no remaking a base from the archive); `lordos-guest` 7's `perf` not checked in a
running guest (I, 100 → 95 %). Ratings the code beat: `lordos-guest-win`'s `perf` was checked in the maintainer's guest
(I, 95 → 100 %). Rows added: Sentinela's watch of the machines (K), protected windows (L). Statements fixed: section 9
item 5 said pausing Windows Search and the Linux side weren't built; 13.8 item 7 called our filter a denylist; 19.7 said
Start Fresh writes `operation.json` and makes a changed base again from the archive (it refuses it); 19.9's failure table
read as built (a line now says which rows are); 12 lacked the new tests and the built sandbox probe; the M item 4 note
named `vmsandbox.c` for refusing `io_uring` (it is the allowlist, `vmsyscalls.h`); the I row said w11's `perf` wasn't
checked in a running guest; Phase 4 had no line for machine groups.

**The audit of 2026-11-23 (against `965a10f6e`, 66 → 68 %)** added the six performance items (M, 7 → 79 %), schema 13,
`lordos-guest-win` w11, Configure's new rows and the download retry; it lowered the resource budget (A, 50 → 35), "Guest
can see" (L, 60 → 40), recording (F, ✅ → 🧪), `isoInfo` (B, 100 → 85), the library card (C, 95 → 90) and `machines set`
(J, 100 → 95), and raised the failure page (A, 60 → 65), the requirements check (B, 70 → 80), Configure (C, 50 → 60),
storage (D), Monitor (K, 60 → 65), Part 1 step 5 (H, 0 → 40) and the targets (M, 50 → 60).

### A. Engine, lifecycle, sandbox, display (86 %)

| Feature (section) | Status | % | Evidence | Note |
|---|---|---:|---|---|
| `lord-vm` supervisor over direct QMP, `machine.json` schema 16, lock, `run.json`, crash recovery (3.8, 3.9) | ✅ | 100 | `lordvm.c`, `qmp.c`, `machine.h` (`VM_SCHEMA 16`); `vm-test`, `vmgroup-test` | Schema 16 (*in development*) adds `automation` (6.15); 15 `guest_links` and `guest_notify` (6.12); 14 a machine group member's keys (19.4) |
| KVM amd64 and arm64, TCG labelled Emulated (2.1–2.4) | ✅ | 100 | `lordvm.c` (`-accel`), `assistant.jsx` ("Other architecture…") | Pi 5, nested amd64, Xeon and the i9 PC |
| Pi defaults (3 vCPUs, RAM − 2 GB) and the microSD warning (2.2) | ✅ | 100 | `machine.c`, `js_vm.c` |  |
| `/dev/kvm` through `vm-kvm` (ACL now, group next login) (3.4) | ✅ | 100 | `vm_admin.c` |  |
| Sandbox: Landlock (`disks/` only), PDEATHSIG, `dump-guest-core=off`, fds, our seccomp allowlist on top of QEMU's own filter (a stopped call named in the log and on the failure page); QEMU in its own user, mount, ipc, uts and net namespaces and non-dumpable (3.3) | ✅ | 95 | `vmsandbox.c` (`vm_ns_enter`, `vm_ns_setup`, `vm_jail_qemu`), `vmseccomp.c`, `vmsyscalls.h`, `lordvm.c` (`sc_serve`); `vmseccomp-test`, `vm-jail-probe`, `vm_isolation.sh` | Namespaces, non-dumpable QEMU and the allowlist in development (2026-11-23); no pid namespace (GLib refuses the D-Bus display's peer then); a GVT-g vGPU machine, a bridged tap and hugetlb memory not yet run under the allowlist (their calls read in QEMU's source) |
| Every helper jailed too: lord-vm's own Landlock, `qemu-img`, `swtpm`, `passt`, `virtiofsd`; lord-vmdisplay sandboxed with a self-test; `lord-vmunpack` (a group's archive) under `sandbox_apply_helper` on descriptors; render node and VFIO confined by Landlock (3.3, 19.6) | ✅ | 100 | `lordvm.c` `vm_jail_self`, `vmsandbox.c` `vm_jail_tool*`, `render_rules`, `vgpu_rules`, `vmdisplay.c` `--selftest`, `vmunpack.c`; `vm_isolation.sh`, `vmunpack-test` | Added by the audit of 2026-11-23; `lord-vmunpack` by this one |
| Sandbox and cgroup state reported (`lord-vm --status`: Landlock, our seccomp, KVM fd, scope), nested cgroup leaves per process (3.3, 3.7) | ✅ | 100 | `lordvm.c` (`setup_cgroups`, `--status`), `diag.js`, `logs.jsx` | Added by the audit of 2026-11-23 |
| "Weaker isolation on this kernel" notice when Landlock is missing (3.3) | 🧪 | 90 | `js_vm.c` (`host().landlock`), `model.js` `isolationGaps`, `library.jsx`, `configure.jsx` (Security), `vmwindow.jsx` (Info); `vm_model_test.mjs`, `machines_app_headless.sh` | *In development*: the library's notice, Configure → Security's sandbox line and the window's Info menu, from the kernel's Landlock ABI and the running machine's VMS bits (namespaces too); Sentinela's alert as before. Not seen on a kernel without Landlock (every test host has it) |
| Render node by descriptor; one slice for all machines (3.3, 3.7) | ❌ | 0 | `lordvm.c` passes a path (`rendernode=`); one scope per machine | QEMU's `rendernode=` takes a path; Landlock limits it to that node (row above) |
| Resource budget (3.7) | 🟡 | 80 | `machine.c` `vm_budget_used_mb`, `vm_budget_fits`; `js_vm.c` `budget_ok` (`start`, `startDetached`), `lord.vm.budget`; `cli/machines.c`; `model.js` `budgetPlan`, `budgetLine`; `vmwindow.jsx`, `library.jsx`; `vm-test`, `vm_model_test.mjs`, `machines_app_headless.sh` | *In development*: one check in lord-server for every start path (the window, Start Without Window, `machines start` through the session and without one, a group's members, a disposable session's share counted); the window offers Suspend <another> or Start with less memory; the status line shows memory for machines and a PSI warning. Not built: one parent slice with lower CPU and I/O weight than the session (each machine keeps its own `memory.max`) |
| Files and contracts: `operation.json`, newer schema read-only, suspended-state check (3.5, 3.6) | ✅ | 90 | `vmops.h` `vm_op_begin`, `machine.c`, `lordvm.c` | Export manifest not built |
| Firmware identity kept with the machine; the old code file kept in the folder (3.6) | 🟡 | 40 | `machine.c` (`firmware_sha256`), `lordvm.c` | Only the hash is recorded and a mismatch logged; the old code file isn't kept. Added by the audit of 2026-11-23 |
| `lord-vmdisplay`: Unix.Map, SIGBUS guard, rate, dmabuf, cursor, audio (3.2) | ✅ | 95 | `vmdisplay.c`, `js_vm.c`; `vmdisplay-test` | Zero copy when the desktop composes on the GPU (the dmabuf is a texture); read back only for a recording, the CPU path or a failed import |
| Start, pause, suspend, resume, shut down through the agent then ACPI, force off (6.1) | ✅ | 100 | `lordvm.c`; release gates |  |
| Keep Running, Start Without Window, keep awake (5.3, 6.1) | ✅ | 100 | `js_vm.c` `startDetached`, `awake` | The idle hold wasn't watched on screen |
| Host shutdown inside logind's 45 s delay (6.1) | 🧪 | 100 | `vmhoststop.h`, `vmlogind.c`; `vmlogind-test`; migration 010 | *In development*; fake logind plus the maintainer's PC |
| A 3D guest's stop named (`qemu-3d-stop`, `qemu-3d-crash`), unattended starts in 2D (3.2, 6.10) | 🧪 | 90 | `vmlog.c`, `lordos-guest --stopping`; `guest_stop_test.sh` | *In development*; no real 6.12.117 3D guest yet |
| Self-healing network (passt restarted while running) | ✅ | 100 | `lordvm.c` (keeper, `NET_MAX_RESTARTS`) |  |
| Failure page with a cause and a fix (6.1) | ✅ | 90 | `vmwindow.jsx` (`Failed`), `model.js` `failCause`, `failActions`, `machines.why.*` and `fix.*`; `vm_model_test.mjs` | *In development*: a locked disk and a missing disk named from QEMU's words, Start with less memory after running out, Go Back to a Snapshot. Checked with QEMU's messages in the model test, not with a real failure on screen |
| A state saved by another QEMU or CPU: Resume anyway or discard (6.1) | ✅ | 90 | `lordvm.c` (`--resume-anyway`), `vmwindow.jsx`, `apps/store/syspkg.js` `qemuUpdateRisk`, `debian.js` (Updates); `store_syspkg_test.mjs` | *In development*: Store → Updates warns when an update changes QEMU while machines are suspended. HDA is migrated with the state (nothing to unplug); what can't be saved (a graphics card, 3D) is never suspended |
| Direct kernel boot (13.8 item 9) | 🟡 | 30 | `lordvm.c` | Unattended installs only; none for an installed Linux guest |
| Rename a machine with its folder and transfer folder (6.26) | 🧪 | 100 | `machine.c` (`renameat2`); `vm_rename.sh` | *In development*; headless |

### B. Getting a system in (88 %)

| Feature (section) | Status | % | Evidence | Note |
|---|---|---:|---|---|
| Cards LordOS, Debian, Ubuntu, Fedora, Kali per architecture; Windows on amd64 (4.1, 5.2) | ✅ | 100 | `assets/machines/systems.json`, `catalog.js`, `vmunattend.c` (`kali`); `vmcatalog-test`, `vm_catalog_test.mjs`, `vmunattend-test` | Kali runs end to end with its own unattended install (*in development*): amd64 to the Xfce desktop in 36 min 27 s, the Pi 5 console in 11 min 5 s, 2026-11-24 (3.9, "The cards run end to end") (was 95: Kali had no unattended install) |
| Makers' OpenPGP signatures, SHA-256, resumed downloads, removal (3.9) | ✅ | 100 | `vmcatalog.c`, `vmimage_admin.c`, `pgp.h`; `http-resume-test`, `http-retry-test` | A redirect refused with 403 or 5xx starts again from the original URL (*in development*) |
| Any ISO, recognized (5.2) | ✅ | 95 | `vm/vmiso.c`, `js_vm.c` `isoInfo`, `model.js` `isoArch`, `assistant.jsx` (the ISO tile's drop); `vmiso-test` (ASan, UBSan, fuzz), `vm_model_test.mjs` | *In development*: the architecture from the UEFI boot files' PE headers (ISO 9660, UDF, the El Torito ESP), install discs by `sources/install.*`, the name only when they say nothing (`archFrom`); an `.iso` dropped on Use an ISO picks it. Checked with real ISOs (Alpine aarch64 named `mysystem.iso`, Ubuntu Server 26.04.1, LordOS 0.8.9, an install-disc evaluation ISO). The drop has no end-to-end UI test (was 85: only the 32-byte label was read) |
| Unattended Linux: Debian and Ubuntu desktops to login (3.9) | ✅ | 100 | `vmunattend.c`; `vmunattend-test`; VM 116, 2026-11-07 |  |
| Unattended Fedora to login, Ubuntu console, the arm64 cards on the Pi | ✅ | 95 | `vmunattend.c` (`VMU_KICKSTART`, `xconfig`, `custom-environment`), `systems.json` (arm64 `append`), `catalog.js` `unattendedOf`; `vmunattend-test`, `vm_catalog_test.mjs` | Fedora 45 Workstation to GDM and GNOME (21 min 47 s) and Ubuntu 26.04.1 console to login (10 min 40 s) on amd64; Debian 13.8, Fedora 45 and Kali consoles and LordOS on the Pi 5, 2026-11-24, each from its card's verified download. Fixed on the way: Fedora's desktop booted to a text login, Fedora's console install stopped (an environment renamed), arm64 installers showed only on the serial port. Not run for lack of space on the Pi: the arm64 desktops and Ubuntu arm64 (was 🟡 60) |
| LordOS card, `lordos-auto.conf`; its signed list read from lordos.org first, GitHub second (3.9, 4.1) | ✅ | 95 | `vmunattend.c` (`VMU_LORDOS`), `vmcatalog.c` (`LIST_MIRROR`, `vmcat_list_source`), `vmimage_admin.c`; `vmcatalog-test` (the real list and signature) | Run from the card end to end on 2026-11-24: on amd64 the list from lordos.org (HTTP 200), the 0.8.9 image, the install in 1 min 19 s with 3D, the greeter and the desktop; on the Pi 5 in 2 min 2 s to the greeter and the desktop. The guest agent and apps mode on first boot wait for an ISO newer than 0.8.9 (was 🧪 85) |
| Windows 11 amd64: ISO by hash, unattended, drivers, TPM, Secure Boot, KMS client key (4.3.1) | ✅ | 100 | `windows.js`, `vmunattend.c`, `lordvm.c`; `vm_windows_test.mjs` | On the maintainer's PC |
| virtio-win guests on arm64 (4.3) | 🟡 | 70 | `machine.c` (the devices on arm64, `vm_gicv3_in`), `lordvm.c` (the arm64 template: `virt,gic-version=3`, AAVMF secure boot, `tpm-tis-device`, USB CD/DVD, ramfb), `vmunattend.c` (arm64 answers, ARM64 drivers), `systems.json` (the maker's 38 Arm64 hashes), `windows.js` `windowsEmulatedWhy`; `vm-test`, `vmunattend-test`, `vm_windows_test.mjs`, `vm_catalog_test.mjs`; the Pi 5, 2026-11-24 | *In development* (4.3.2): KVM refused on a GICv2 host (the Pi 5, measured: "KVM does not support GICv3 emulation"), so there the machine is Emulated; under TCG the template boots with secure boot enforced from the USB CD/DVD (checked with Alpine). Not checked: an install of the guest (its ~6.9 GB ISO comes only from the maker's page and didn't fit on the Pi), Setup on ramfb, the emulated x86-64 guest agent, swtpm on arm64, any GICv3 host (was ❌ 10) |
| A person's own systems: `vm-image-get-user`, `lord-pgpv` (5.6.1) | ✅ | 95 | `systems.js`, `vmurl.c`, `vmsig.c`, `lordpgpv.c`; `vmurl-test`, `vmsig-test` |  |
| Import from other apps with identity kept (6.22) | ✅ | 95 | `vmimport.c`, `migrate.jsx`, `lordvm.c` `--import-machine`, `machines import`; `vmimport-test`, `vm_import_test.sh` | No libvirt XML (`vmxml.c` is a generic reader), no driver injection, no real Windows from Fusion. *In development*: `machines import` from the terminal, and the converted disk written out before it is put in place (a machine started right after a large import timed out) |
| Requirements check (5.2) | 🟡 | 80 | `library.jsx` `Requirements`, `model.js` `reqRows`, `missingFor`, `assistant.jsx`, `vm-enable` | Create, which starts the download, waits until the parts are in (one click through the package engine). No per-system page of drivers and tools; not through the Store |
| Migrate from another PC (6.11) | 🟡 | 25 | `lordvm.c` `--import-machine` (a raw whole-disk image), `machines.c` `cmd_import` | *In development*: a backup image of a Linux PC's disk imports and boots (a 20 GB Ubuntu 26.04.1 image, 2026-11-24, with the import's `fsync` fix). Not built: the old PC's agent and the LAN copy, a connected disk, driver injection, encrypted disks (was ❌ 0) |
| Whonix (Tor) card as a machine group: Gateway and Workstation, internal link, anonymity profile, Start Fresh (19) | 🟡 | 92 | `vmgroup.c`, `vmlink.c`, `vmunpack.c`, `lordvm.c` `--group-create`, `--group-delete`, `--group-rename`, `--group-repair`, `VM_LINK`; `vmgroup-test`, `vmlink-test`, `vmunpack-test`, `vmcatalog-test`, `vm_group_model_test.mjs`, `vm_group_ui_test.mjs`, `vm_group_link.sh`, `vm_group_manage.sh`, `machines_group_manage_headless.sh` | *In development*: G0–G4 plus Delete, Rename and Repair of a group, Start Gateway and Stop Gateway, and a missing or changed base made again from the archive (2026-11-24; headless, screenshots of the dialogs; the Start Gateway bar node-tested only, its window being protected). Creating a group from the app was refused before (the image store's file name); fixed. Left: the release gate on real hardware (G5) and disk encryption of the members (layer 5) (was 80) |

### C. The app (85 %)

| Feature (section) | Status | % | Evidence | Note |
|---|---|---:|---|---|
| Library: toolbar, chips, grid or list, sort, search, status line, live card (5.1) | ✅ | 95 | `library.jsx` (`Lights`), `model.js` (`skeletonPlan`, `cardLights`); `vm_model_test.mjs`, `machines_app_headless.sh` | Placeholders from the first frame; agent and disk lights with words on the card and the row (*in development*). No Export or Templates in ··· |
| Machines settings: General, Profiles, Systems, Network, Graphics cards, Keyboard and mouse (5.1) | ✅ | 100 | `settingswin.jsx` |  |
| New Machine assistant (5.2) | 🟡 | 70 | `assistant.jsx` (5 steps) | No Detect step, folder choice, **Fresh install** snapshot or file drop |
| Machine window: bar, Actions, Devices, Apps, close dialog, resolution menu (5.3) | 🟡 | 85 | `vmwindow.jsx` (View and Info menus), `library.jsx` (Start in Full Screen), `model.js` `actualFrame`, `infoLines` | *In development*: View (Window, Full Screen, Apps, Picture in Picture, Actual Size), Info (CPU, memory, disk, the sandbox, Open Configuration), Start in Full Screen. Not built: Scale, device dots, the window keeping the guest's shape; no USB, Network or Camera in Devices |
| Display modes: Window, Full Screen, Apps (5.4) | ✅ | 100 | `vmwindow.jsx`, `appsmode.jsx` |  |
| All Displays (multi-head), Picture in Picture of a machine (5.4) | 🟡 | 45 | `vmwindow.jsx` (`PipBack`, View → Picture in Picture), `model.js` `pipFrame`; `vm_model_test.mjs` | *In development*: Picture in Picture of a machine (`wm.setPip`, on the window's display, the guest's shape, no keys and no pointer for the guest, not for a protected machine). All Displays (virtio-gpu heads, one per display) not built |
| Configure, one window with summaries (5.5) | 🟡 | 65 | `configure.jsx` (11 sections), `kit.jsx` (`SideRow` dot), `model.js` `changedSections`, `sectionsMatching` | Search over the sections and a dot on each with an unsaved change (*in development*). No Backup, ballooning, CPU model, USB rules, disk add or encryption, printers, icon and tags, MAC, speed limit; huge pages are automatic, no switch |
| Profiles and `.lordprofile` (5.6) | ✅ | 95 | `profiles.js`; `vm_profiles_test.mjs` |  |
| Official logos, opt-in (5.6.2) | ✅ | 90 | `logos.js` |  |
| What this computer can run (5.7) | 🧪 | 85 | `library.jsx` (`ThisComputer`), `model.js` `canRun`, `sensorLines`, `js_vm.c` (`host().nested`, `sensors()`); `vm_model_test.mjs`, `machines_app_headless.sh` | *In development*: amd64 and arm64 from what the host reports, the sensors (temperature, fan, a Pi's power supply) while machines run, in the Machine menu and on the first-run page. Not yet seen on screen on the Pi |
| ❖ shortcuts, Keys panel, focus gate (5.8) | ✅ | 95 | `keys.jsx`; `vm_keys_scene.mjs` | The release combination is fixed |
| Capture mouse (6.25) | 🧪 | 90 | `capture.js`, `pointerlock.h`; `vm_capture_test.mjs` | Through `lordvm-dev` only |

### D. Snapshots, clones, storage (60 %)

| Feature (section) | Status | % | Evidence | Note |
|---|---|---:|---|---|
| Snapshots with memory, NVRAM and TPM, timeline, revert, sweep, at most 64 (6.2) | ✅ | 85 | `lordvm.c` (`tpm_snap`), `snapshots.jsx`, `vmops.h` | No `fsfreeze`, no copy of `machine.json`, no notes, no snapshot before a tools update |
| Disposable sessions (6.2) | 🟡 | 25 | `lordvm.c` (the memfd overlay, `session-full`), `group.js`, `configure.jsx`; `vm_group_link.sh` | Built only for a Whonix group's Workstation (19.7: an empty overlay in memory, capped, paused when full, snapshots refused; *in development*, headless). The feature of 6.2 for any machine (a session thrown away at shutdown, from the window or the library) isn't built (was ❌ 0) |
| Clone, Duplicate, linked clone, Clone from Here, new identity (6.3) | ✅ | 90 | `lordvm.c` `--clone`, `vmops.c`, `clone.jsx` | The source must be off; no templates |
| Disks: qcow2, discard, Reclaim space, grow (6.4, 6.4.1) | ✅ | 85 | `lordvm.c` (`blockdev_disk`, `--compact --estimate`, `qemu-img resize`) | An interrupted compaction is rolled back. No qcow2 compression; the disk path itself is scored in M |
| Raw disks, disk encryption, export (`.lordvm`, OVA), backups (6.4, 3.6) | ❌ | 0 |  | Raw only for imports and read-only nodes; encrypted imports refused |
| Storage window: shared extents, frees-if-deleted, downloads (6.16) | 🟡 | 75 | `storage.jsx`, `js_vm.c` (FIEMAP) | Trash, Reclaim with an estimate, the unused shared base. Never seen on btrfs or XFS; no "about" figures; no "delete old snapshots" or "remove unused media" |

### E. Network (80 %)

| Feature (section) | Status | % | Evidence | Note |
|---|---|---:|---|---|
| Shared (passt over a socketpair), Isolated (6.5) | ✅ | 100 | `lordvm.c` (`vm_passt_argv`) |  |
| Bridged (macvtap, `vm-net-bridge`), Host only (passt on 127.0.0.1), port rules (6.5) | 🧪 | 90 | `vmnet_admin.c`, `vmnet.c`, `lordvm.c` | Headless checks; `vhost=off`, no `vhost-net` |
| Each machine named in the firewall (6.5) | 🟡 | 60 | `fw/rules.h` (`lordos:machines`), `apps/monitor/Network.jsx` | One component for every machine (Monitor names them); a forwarded port isn't shown as its own rule |
| Network card model (`--nic-model`) and an import's extra cards (3.9, 6.22) | 🧪 | 70 | `machine.h` (`nic_model`, `VM_EXTRA_NICS`) | An import's extra cards are kept but never connected. Added by the audit of 2026-11-23 |

### F. Devices (56 %)

| Feature (section) | Status | % | Evidence | Note |
|---|---|---:|---|---|
| Speakers and microphone, output and input per machine (6.9) | ✅ | 90 | `lordvm.c` (`hda-micro`), `vmdisplay.c`; microphone permission checked in `js_vm.c` | Windows guest sound not checked |
| Recording with sound, screenshot (6.20) | 🧪 | 90 | `js_vm.c`, `vmwindow.jsx` | No automated test or recorded on-screen check (was ✅) |
| CD/DVD change while running | ✅ | 100 | `lordvm.c` (`cd_change`) |  |
| USB passthrough, auto-connect rules, `vm-usb-allow` (6.6) | ❌ | 0 | no usbredir anywhere |  |
| Camera (6.9), printers (5.5) | ❌ | 0 |  |  |

### G. Host integration (77 %)

| Feature (section) | Status | % | Evidence | Note |
|---|---|---:|---|---|
| Shared folders, virtiofs, read-only by default (6.7) | ✅ | 90 | `lordvm.c` (`vhost-user-fs-pci`) | Not checked on the Pi or in a Windows guest on arm64; no SMB fallback |
| Transfer folder (6.7.1) | 🧪 | 90 | `machine.h` (`VM_XFER_*`), `lordvm.c`, `configure.jsx` | Through `lordvm-dev` |
| Clipboard: text, rich text, pictures, by direction, ask mode, secrets kept out (6.8) | ✅ | 95 | `vmagent.c`, `js_vm.c`, `x11clip.c`; `guest_clip_test.sh` |  |
| Files dropped on a machine or an apps-mode window (6.21, 7.2.1) | ✅ | 100 | `lordvm.c` (`.lordos-drop-*.part`), `AGK_PLACE` |  |
| File copy panel, flow control, resume, drag out, Send to LordOS, quarantine (6.21) | ❌ | 10 | one progress line |  |
| Open with a machine (7.3) | ✅ | 100 | `openwith.js`; `vm_openwith_test.mjs` | Windows guests |
| Apps mode: Windows and LordOS guests (7.2) | ✅ | 90 | `appsmode.jsx`, `appsstatus.jsx`, `js_guestwin.c`; `guestwin_headless.sh` | Not other Linux desktops; no window shapes |
| Guest apps in the launcher before the machine runs (6.12) | 🟡 | 40 | Dock `vm:<machine>:<app>` while apps mode runs | No saved list, no launcher entries, no "launch starts the machine" |
| Guest → host links and notifications (6.12) | 🟡 | 75 | `vmagent.c` (`ag_link_ok`, `ag_note_parse`, `ag_rate_take`), `lordvm.c` (`guest_note`), `js_vm.c` (`guestLinkAnswer`), `vmwindow.jsx`, `configure.jsx`, `tools/guest/gshell.c`, `lordos-guest-win.c` w12; `vm-test`, `guest_shell_test.sh`, a KVM guest (6.18) | *In development*: off by default (machine.json schema 15), http and https only, a link opens only from the bar's Open (after 700 ms, while in use), notifications shown as from the machine, both rate-limited; checked in a real Debian 13 guest through `lordvm-dev`. Not built: the guest desktop's own notifications passed on by themselves, a Send to LordOS entry in the guest; not checked: the bar on screen, a Windows guest (was ❌ 0) |

### H. Graphics (46 %)

| Feature (section) | Status | % | Evidence | Note |
|---|---|---:|---|---|
| virgl 3D for Linux guests on amd64 (6.10) | ✅ | 85 | `lordvm.c` (`decide_graphics`) | RX 580; off on arm64 by decision |
| Intel GVT-g vGPU with our GOP ROM, Windows guest (6.10.1, MACHINES-GPU.md) | ✅ | 85 | `vmgpu*.c`, `vmgvtwin.c`; `vmgpu-test` | No Linux guest on a vGPU; tested on device 0x9BC5 only |
| Settings → Graphics cards: Machines (shared) (MACHINES-GPU.md) | 🟡 | 60 | `apps/settings/Gpus.jsx`, `vmgpu_admin.c` | Dedicated and AI answer `gpu.roleNotYet` |
| A smoother picture, Part 1 steps 1, 2, 3, 6: frames in lord-server's loop, hardware pointer, damage, Monitor (MACHINES-GPU.md) | ✅ | 100 | `js_vm.c`, `media.c`; `engine-test`, `mview-test` | Released in 0.8.9.5. Added by the audit of 2026-11-23 (was folded into the "steps 4 and 5" row) |
| A smoother picture, Part 1 step 4: several damage rectangles (MACHINES-GPU.md) | ❌ | 0 | `vmdisplay.c` `damage()` merges into one box | Pacing (M) changed when pictures are taken, not their damage |
| A smoother picture, Part 1 step 5: scaled on the GPU (MACHINES-GPU.md) | 🟡 | 40 | `js_vm.c` (`gtex`, `gout`), `gpu/dmaread.c`, `media/mview.c` | dmabuf pictures (virgl, vGPU) are GPU textures; a 2D picture is still scaled per pixel on the CPU over the whole frame (was ❌ 0) |
| venus (Vulkan) (6.10) | ❌ | 0 |  |  |
| Whole-GPU passthrough (Mode B) (6.10, MACHINES-GPU.md) | ❌ | 0 | `vmgpu_admin.c` refuses a dedicated card | "Whole" in Configure is the largest GVT-g type, not passthrough |

### I. Guest tools and agent (90 %)

| Feature (section) | Status | % | Evidence | Note |
|---|---|---:|---|---|
| `lordos-guest` 8: clipboard, apps-mode relay, `--stopping`, `perf` over D-Bus, the agent's shell, links and notifications (6.12, 6.17, 6.18, 7, 7.1.4) | ✅ | 95 | `tools/guest/lordos-guest.c` (`VERSION "8"`), `gshell.c`, `perf.c`, `dbusmin.c`; `guest-dbus-test`, `guest_perf_test.sh`, `guest_shell_test.sh` | 8 (*in development*) adds the shell (the person's own login shell in a pty, never root) and the links and notifications socket, both checked in a running Debian 13 guest on 2026-11-24; `perf` (7) still not checked in a running guest |
| `lordos-guest-win` w12: clipboard, display, apps, open, place, pointer, gvt, extra display, tablet, perf, shell, links, notify (6.12, 6.18, 7.1.2, 7.1.4) | ✅ | 95 | `tools/guest-win/lordos-guest-win.c` (`VERSION "w12"`); `vm-test` (`AGK_PERF`, the shell's and links' frames) | w12 (*in development*) adds PowerShell in a pseudo console (never elevated) and `--open-on-host`/`--notify-host` by `WM_COPYDATA`; cross-built with mingw-w64, not yet run in a Windows guest (w11 was 100, `perf` checked in the maintainer's guest on 2026-11-23) |
| Install Agent through qemu-guest-agent; tools updated by themselves (6.17, 7.1.3) | ✅ | 90 | `lordvm.c` (`guest-file-write`), `model.js` (`TOOLS_SHIPPED`, `toolsAutoStep`) | No guest with older tools tried |
| Agent state light and capability list (6.17) | ✅ | 95 | `model.js` (`agentTone`, `agentLines`), `js_vm.c` (`lost`); `vm_model_test.mjs` | Red (*in development*) when the agent stopped answering while the guest runs, or the tools' install failed; the list has the agent's shell, links and notifications and a Windows guest's event logs. Not seen on screen yet (was 🟡 85: no red state) |
| Shell over the agent as the signed-in user (6.18) | 🟡 | 80 | `tools/guest/gshell.c`, `lordos-guest-win.c` (ConPTY), `lordvm.c` (`shell_*`), `js_vm.c` (`shell`), `vmwindow.jsx`; `guest_shell_test.sh`, `vm-test`, a KVM guest | *In development*: the signed-in person's own shell (PowerShell in virtio-win guests), flow control both ways, checked in a real Debian 13 guest through `lordvm-dev` (2026-11-24). A pane of the machine's window, not a Terminal tab; no root or administrator switch, no `lordvm exec`; w12 not run in a Windows guest; the pane not seen on screen (was ❌ 15) |
| Logs: journal, serial, machine log, Overview (6.19) | 🟡 | 85 | `logs.jsx`, `diag.js` (`journalLine`, `guestZone`), `lordvm.c` (journal and `EVLOG_PS` through qemu-ga); `vm_model_test.mjs` | *In development*: a Windows guest's System and Application event logs, a unit or source filter, times in local time with the guest's zone; the event logs not yet run in a Windows guest; Clear view was built already (was 65: no event logs, unit filter or time zone) |

### J. Command line and automation (93 %)

| Feature (section) | Status | % | Evidence | Note |
|---|---|---:|---|---|
| `machines` list, status, start, open, stop…, snapshot, clone, set, rename, ids, and for a machine group `fresh` and `--member` (19.9); 35 languages, `--json`, completion (6.15) | 🧪 | 100 | `server/src/cli/machines.c`, `mcli.c`; `mcli-test`, `machinescli_headless.sh`, `vm_rename.sh`, `vm_exec_e2e.sh` | *In development* since the last audit: `set` passes `--cpu-pin`, `--perf-profile` and `--capture-fullscreen`; `start --no-window --snapshot TAG`, `import`, `console` (each run on 2026-11-24 against a real Ubuntu 26.04.1 machine on the dev PC, in a private home). A start in a window is still checked headless only; a machine from a card is made in the app (`apply` makes one from an ISO) (was 95) |
| `lordvm` exec, logs, cp; MCP server; `lordvm apply` (6.15, 6.24) | 🧪 | 85 | `vmexec.c` (`VM_EXEC`), `lordvm.c` (`ex_begin`), `machines.c` (`cmd_exec`, `cmd_cp`, `cmd_logs`, `cmd_mcp`, `cmd_apply`), `mcli.c`, `configure.jsx` (Automation), `machine.h` (schema 16); `vmexec-test`, `mcli-test`, `vm-test`, `vm_exec_e2e.sh` | *In development* (2026-11-24): exec, cp, logs, console, apply and an MCP server of eight tools, per machine with automation on; 28 of 28 checks in a real Ubuntu 26.04.1 guest on the dev PC. `machines shell` (the agent's shell, checked against a stand-in lord-vm). Not built: exec in virtio-win guests, folders in cp, MCP's create_machine, requirements and screenshot, apply's packages and rebuild. Configure's Automation row not yet seen on screen (was ❌ 0) |

### K. System ties (84 %)

| Feature (section) | Status | % | Evidence | Note |
|---|---|---:|---|---|
| Monitor: a machine as one row (6.14) | 🟡 | 80 | `apps/monitor/Processes.jsx` (Show Machine), `Network.jsx`, `Gpu.jsx`; `apps/machines/index.jsx` (`args.show`) | Network and GPU name machines too; a machine's row opens its processes and Show Machine opens its window (*in development*). No per-machine disk and network numbers in Monitor |
| Idle block while a machine runs (6.14) | ✅ | 90 | `lord.vm.awake`, `vmlogind.c` |  |
| Cleaner, Backups, Files bundle, Search, Pause All on battery, start with the session, auto-pause, balloon (6.14, 16) | 🟡 | 75 | `clean/cleanfs.c` `clean_machine_path`, `backup/engine.c` (`hold`), `backup.c`, `machine.c` `vm_backup_hold`, `vm_dir_active`, `sdk/openwith.js` `registerBundle`, `apps/files`, `apps/machines/ties.js`; `clean-test`, `backup-test`, `vm-test`, `vm_model_test.mjs`, `machines_app_headless.sh` | *In development*: Cleaner never offers a machine's files; Backups hold back a running machine's disks and saved session; Files shows a machine as one item; Search finds machines; Pause All on battery (notice and toolbar); Start with the session; Pause while I'm away. No balloon (see 6.14), no Reclaim space in Cleaner (Machines' Storage window has it) |
| Sentinela watches running machines: sandbox and scope, a Workstation's ways out, crashes, CPU, memory, a Gateway's traffic (docs/SENTINELA.md, "Machines") | 🧪 | 90 | `sdk/sentinelavm.js`, `vmwatch.h`, `js_vm.c` (`stats`: lord-vm asked over its socket, `SO_PEERCRED`), `monitor/conn.c` (`conn_machines`); `sentinela_vm_test.mjs`, `vmwatch-test` | *In development*; off by default with the rest of Sentinela; counts only, never the guest's screen, files or addresses; checked with made-up stats and headless scenes, not on a live machine on screen. Added by this audit |

### L. Privacy and security (74 %)

| Feature (section) | Status | % | Evidence | Note |
|---|---|---:|---|---|
| No telemetry; signed catalog; downloads only when asked (8) | ✅ | 100 | `vmsig.c`, `vmcatalog.c` |  |
| Guest data treated as hostile: logs cleaned, parsers fuzzed (8) | ✅ | 90 | `logclean.h`; `vm-test` fuzz passes, `vmimport-test` mutations | No fuzz of lord-vmdisplay's D-Bus side or of `lordos-guest`'s parser |
| Closed admin actions only (8) | ✅ | 100 | `vm_admin.c`, `vmnet_admin.c`, `vmgpu_admin.c`, `vmimage_admin.c` | `vm-state-size` was listed but isn't one (fixed) |
| Root password only as a hash in the unattended files (8) | ✅ | 100 | `vmunattend.c`; `vmunattend-test` | Added by the audit of 2026-11-23 |
| Protected windows: an anonymity member's windows and apps-mode windows hidden in every capture; no card picture, screenshot or recording of it (19.5, layer 8; PRIVACY.md) | 🧪 | 85 | `model.js` (`machineProtected`), `meta.js`, `js_vm.c` (`lord.vm.protect`), `ui/paint.c`; `protect_model_test.mjs`, `protected_headless.sh` | *In development*; only for a machine with the anonymity profile (a Whonix member): no switch to protect any other machine. Headless: the display, screenshot, remote desktop, recording, portal still and window share. Added by this audit |
| "Guest can see" (8, 16) | 🟡 | 40 | `configure.jsx` (Security: six notes on the sandbox) | It doesn't list the guest's channels; each one's switch is in its own section; no single panel, no live state (was 60) |
| Encrypted machines (8) | ❌ | 0 | encrypted imports are refused | Nor the line in Security that host swap can hold guest memory |

### M. Performance (86 %)

| Feature (section) | Status | % | Evidence | Note |
|---|---|---:|---|---|
| Targets measured: CPU, network, idle CPU, the Pi desktop, disk (9, 6.4.1) | 🟡 | 60 | sections 6.4.1, 3.7.1, 13 | Disk and the host frame under pinning measured on the i9 since; pause, suspend, latency to the screen, app memory not measured; amd64 Phase 0 figures nested |
| 1. Skip unchanged frames; pace or pause a hidden machine's picture | 🧪 | 90 | `vmpace.h`, `vmdisplay.c` (`VD_PACE`), `sdk/desktopmodel.js`; `vmpace-test`, `vmdisplay-test` | *In development*; measured with QEMU's vfio refresh imitated, not on the live vGPU |
| 2. Huge pages for guest RAM | 🧪 | 95 | `vmmem.h`, `lordvm.c` (`mem_backing`), `tools/iso/mkdeb.sh` (`lordos-thp.conf`), migration 011; `vmmem-test`, `migration_shmem_huge_test.sh` | *In development*; private memory gets THP, shared memory (shared folders, the transfer folder) too since `memory-backend-ram,share=on` and `shmem_enabled=advise` (approved 2026-11-23; a running guest's memory seen as `ShmemPmdMapped`, 3.7.2); hugetlb only when the administrator reserved pages. Headless and on the dev host (was 85: shared memory's THP waited for approval) |
| 3. vCPU recommendation | 🧪 | 90 | `vmcpu.c`, `model.js` `vcpuAdvice`, `configure.jsx`; `vmcpu-test`, `vm_model_test.mjs` | *In development*; Configure and the library card, not the assistant (it uses the host's default) |
| 4. Disks: virtio-blk with an iothread, O_DIRECT, native AIO | 🟡 | 85 | `lordvm.c` (`blockdev_disk`, `disk_direct`, `virtio_disks`) | *In development*; measured on the i9 NVMe, and the host's cache bypass kept after measuring real work against it (6.4.1, "Host cache or not"); `io_uring` stays refused by the seccomp allowlist (`vmsyscalls.h`; a sandbox change to approve) (was 80) |
| 5. Guest-side performance profile through the guest tools | 🧪 | 85 | `lordvm.c` (`AGK_PERF`), `vmperfwin.c`, `configure.jsx`, `lordos-guest-win.c` w11, `tools/guest/perf.c` (`lordos-guest` 7); `vm-test`, `guest-dbus-test`, `guest_perf_test.sh` | *In development*; Windows guests: power plan, effects, transparency (checked in the maintainer's guest) and Windows Search paused (lord-vm's fixed script as SYSTEM); Linux guests: power-profiles-daemon and GNOME's animations over D-Bus (KDE and others refused). Not yet checked in a running guest: the Search scripts and the Linux part (was 🟡 55) |
| 6. vCPU pinning | ✅ | 95 | `vmpin.c`, `lordvm.c`, `configure.jsx` (Pin cores); `vmpin-test` | *In development*; measured on the i9 (8 vCPUs: host frame p99 15.2 → 9.9 ms) |

### N. Tests and validation (55 %)

| Feature (section) | Status | % | Evidence | Note |
|---|---|---:|---|---|
| Unit and headless suites in `make test` (12) | ✅ | 90 | `server/mk/vm.mk`, `vmpin.mk`, `tests/run_suites.sh` | 27 C programs (new: `vmseccomp`, `vm-jail-probe`, `vmgroup`, `vmlink`, `vmimage-group`, `vmunpack`, `np-torgw`, `vmwatch`, `guest-dbus`), 10 scripts (new: `vm_whonix_xml_test.sh`, `vm_group_link.sh`, `guest_perf_test.sh`, migration 011's), 12 node tests (new: `vm_group_model`, `vm_group_ui`, `sentinela_vm`, `protect_model`). No settings → QEMU arguments table test (`build_qemu_args` is static in `lordvm.c`; only a Whonix member's arguments are checked, against Whonix's XML) |
| Release gates: isolation, integrity, recovery, leaks (12) | ✅ | 85 | `vm_*.sh`, `tools/vm/bare-metal-gates.sh` | Manual; never run on the i9 PC |
| End-to-end bench: a headless session driven over `remote.sock` (12) | 🟡 | 50 | `tools/vm/e2e/` | Manual, not in `make test`; not every flow of 12 is scripted. Added by the audit of 2026-11-23 |
| Boot matrix in CI, virtio-win install test, storage figures, agent kill tests (12) | ❌ | 0 |  | No QEMU or KVM job in `.github/workflows` |
| Hardware matrix: Pi 5 and Xeon bare metal (gates), the i9 with RX 580 and UHD 630 (3D, GVT-g) | 🟡 | 50 | CHANGELOG | No Pi 4, no 4K Pi kernel, no Intel 3D, no NVIDIA |

## Status and how to continue (read this first)

Updated 2026-11-07, ticked again by the audits of 2026-11-23 and 2026-11-24 (the checklist above has the details). This is the resume point: what is built, what was validated where, and what each phase still needs. When
work continues, start here, pick the next open item, and tick it here (with the date and where it was checked) in the same
commit that does it. The details of each item are in the section named after it.

**Where the code is.** Machines first shipped in **0.8.8.5-beta** (it was planned as 0.8.9; the maintainer moved it forward on
2026-11-07) and is in every release since, the latest **0.8.9.5-beta**; work after it (renaming with folders, the host-stop plan,
the 3D guest stop) is on `test/integration-1125`, in development. Machines was merged with Cloud on branch `rel-0885` and from there into `main`. Its history is on `machines-089` (from
`machines-beta` and `machines-import`). Parts: `server/src/vm/` (`lord-vm`, `lord-vmdisplay`, the protocol `vmproto.h`, import,
catalog, unattended answer files `vmunattend.c`, GVT-g `vmgpu*.c`), `server/src/cli/machines.c` (the `machines` command),
`tools/guest/` and `tools/guest-win/` (the guest tools), `server/src/ui/js_vm*.c` (`lord.vm`, `lord.vmcli`), `apps/machines/` (the app, from the
mocks), `assets/machines/systems.json` and `packaging/keys/machines/` (download cards and the makers' pinned keys).

**Deactivating it (0.8.8.8).** Machines is one of the apps that can be deactivated in Store → Installed
([NATIVE-APPS.md](NATIVE-APPS.md)), but not while a machine is running, paused, starting or stopping: its window would go and
the machine with it (`vmActive` in `sdk/appsoffmodel.js`).

**How to test.**
- Unit and integration: `make -C server test` (vm-test, vmimport, vm-import, vmcatalog, http-resume, vmunattend and the suites);
  on the Mac `node server/tests/vm_catalog_test.mjs` and `node server/tests/vm_model_test.mjs`, `node tools/i18n-check.mjs`,
  `node tools/brand-check.mjs`.
- Release gates (12): `server/tests/vm_isolation.sh`, `vm_integrity.sh`, `vm_recovery.sh`, `vm_leak.sh` (helpers in
  `vm_gate_lib.sh`); on a physical PC `tools/vm/bare-metal-gates.sh` runs them and writes one results file.
- On screen in a real session: the bench in `tools/vm/e2e/` (README there): a headless LordOS session, a driver bundle, clicks and
  screenshots over `remote.sock`.
- Never on the maintainer's daily desktop without asking; the build VM (VM 116) and the Pi 5 may be used.

### Phase 0, technical proof: done (2026-11-04)

- [x] KVM, QEMU, QMP and D-Bus display, Debian 13 guest with picture and input on amd64 (nested) and the Pi 5 (section 13).
- [x] 4K guest on the 16K Pi kernel, GICv2, `device=/dev/fdset`, `ui-dbus` on arm64, TCG numbers, passt chosen over user networking,
  virtiofs on both, the Pi desktop under a 3-vCPU load.
- Left open from 13.8 (carried to later phases, below): bare-metal amd64 figures, the arm64 3D abort, dmabuf import, GICV trapping,
  passt's speed spread on the Pi, boot time.

### Phase 1, alpha: done (2026-11-05)

- [x] `lord-vm` (sandbox, QMP, cgroup limits, crash recovery), `lord-vmdisplay`, `machine.json` schema, qcow2 NVRAM (3.9).
- [x] Isolation test passes on the Pi 5 and amd64.
- [x] Phase 1's open items: Esc in a menu, CD/DVD while running, install timeout, passt crash healing.

### Phase 2, beta: built, one validation left

Built and validated:
- [x] Control Center of cards, right-click menu, five-step New Machine assistant, Configure, Logs, machine window with toolbar and
  menus, full-screen bar, BootFailed page (mocks).
- [x] Suspend and resume, Keep Running, Start Without Window, keep awake (6.1); snapshots with memory and a timeline (6.2);
  Clone, Duplicate, linked clones, Clone from Here (6.3); Storage window and Reclaim Space (6.16).
- [x] Profiles, resolution menu, Record with sound, Shell (serial console), Logs with Overview (6.18, 6.19).
- [x] Guest agent with its popover, shared clipboard (text, images, rich text, sanitized), clock after resume, Clone identity
  reset.
- [x] Self-healing network (passt restarted while running), host shutdown or logout saves running machines.
- [x] Download cards Debian, Ubuntu, Fedora, Kali per architecture: makers' OpenPGP signatures checked, SHA-256, resumed downloads,
  downloaded systems in Storage with removal.
- [x] Install without questions (Debian preseed, Ubuntu autoinstall, Fedora kickstart; password only as a SHA-512 hash).
- [x] Shut Down through the guest agent first, then the ACPI button; root cause of a guest ignoring the button (no dbus, so no
  logind; unattended installs now always bring the standard task, `qemu-guest-agent`, and `acpid` on a console Debian).
- [x] Release gates pass: Pi 5 (45/41/10), nested amd64, bare-metal Xeon Silver 4108 (47/41/10, 2026-11-06).
- [x] Debian 13.7 desktop end to end with nothing typed: card, download (stopped and continued), install, reboot, login to GNOME
  (about 28 min on 2 vCPU, 3 GB; 2026-11-07, VM 116, screenshots `u-01`…`u-08`).
- [x] Kernel paths in the real ISOs match the catalog (Ubuntu `casper/`, Fedora `images/pxeboot/`).

- [x] Library toolbar as in Remote Desk (Add, pictures or list, Sort, Show, search; status line with the summary), the machine
  window's bar as grouped icons with tooltips, and a round Play over the last picture when off (2026-11-08, for 0.8.8.7;
  headless session on the maintainer's PC, screenshots).

Still open in Phase 2:
- [x] **Ubuntu 26.04.1 unattended desktop install through its reboot and login** (2026-11-07, VM 116: 23 min on 2 vCPU,
  3 GB, 24 GB disk; login with the assistant's password, `u-14`, `u-15`; Shut Down through the agent 5.5 s). Needed the fix that
  keeps Ubuntu's browser package out (it only installs the snap, which fails inside the installer).
- [x] **Fedora 45 kickstart install through its reboot and login** (2026-11-24, the dev PC: 21 min 47 s to GDM and GNOME,
  after the `xconfig --startxonboot` fix).
- [x] Ubuntu console (no desktop) install through login (2026-11-24, the dev PC: 10 min 40 s).
- [x] Kali card end to end (2026-11-24: its new unattended install, 36 min 27 s to the Xfce desktop).
- [x] arm64 cards end to end on the Pi 5 (2026-11-24: Debian, Fedora and Kali consoles and LordOS; the arm64 desktops and
  Ubuntu arm64 didn't fit on its disk).
- [ ] Seen, not fixed (still open at the audit): on a first start the window can stay on "Display output is not active" until it is given a size.
- [ ] Whether the session's idle sleep was really held off by a running machine wasn't watched on screen (6.1).
- [ ] Shared extents (reflink) in Storage weren't seen: the test machines are ext4; check on btrfs or XFS.
- [ ] Bare-metal amd64 on the maintainer's i9 PC (AMD GPU): run `tools/vm/bare-metal-gates.sh`, then the Debian card on screen,
  and record the numbers in section 13 (13.8 item 1, with Mesa: 3D and dmabuf formats).

### Phase 3, advanced: mostly built

- [x] Migrate from another app (`.vmx`/`.vmwarevm`, `.vbox`, `.pvm`, `.ova`/`.ovf`, single VMDK/VDI/VHDX/VHD), identity kept,
  hostile-input rules, sandboxed conversion (6.22).
- [x] Shared folders (virtiofs, 6.7): built for 0.8.8.7 (2026-11-08). Checked in a headless session on the maintainer's PC
  (amd64, KVM) with a Debian 13 guest: mount, read, write, read-only refused, a host symlink resolving in the guest,
  Suspend and resume with the folder mounted (a change made on the host meanwhile seen after), a snapshot with memory
  and its revert. Not yet on the Pi 5 (arm64) nor with a virtio-win guest.
- [ ] A Windows guest migrated from VMware Fusion end to end (the maintainer's Windows 11, once its encryption is removed in
  Fusion): NVMe disk, vmxnet3, the SMBIOS UUID and MAC kept, no TPM, Secure Boot off; activation checked.
- [ ] USB passthrough of a device to a machine.
- [x] Bridged and Host-only networks (0.8.9: a macvtap for Bridged, with `vhost=off`; passt on 127.0.0.1 for Host only; 6.5).
- [ ] File copy panel with flow control and resume, Send to LordOS, drag out (7, target 300 MB/s). Dropping files in is built (6.21).
- [x] UEFI secure boot and TPM on amd64 (swtpm, its state in snapshots; 4.3.1). arm64 secure boot is still To test.
- [x] virtio-win guests on amd64 with automatic driver setup (Windows 11, 0.8.9.5, 4.3.1). arm64 is not built (stage C).
- [x] 3D with virgl for Linux guests on amd64 (6.10, "3D graphics (built)" below). Still open: venus; the arm64 3D abort (13.8
  item 2, open decision 5).
- [x] dmabuf import in the compositor (13.8 item 3): read back into the picture by the GPU path (3.2). Still open: showing the
  dmabuf with no copy, and latency to the screen (13.8 item 4).
- [ ] Picture in Picture, multi-head.
- [ ] Export, backups of machines. Monitor shows each machine as one row (6.14); no drill-in.
- [x] Command line: built as `machines` (6.15, 0.8.9.5); exec, cp, logs, console, shell, import, apply and MCP in development (2026-11-24).
- [ ] Done when: a real compatibility and security matrix per architecture (16).

### Phase 4, integration: partly built

- [x] Apps mode (guest apps as windows, in the Dock) for LordOS and Windows guests (7.2). Other Linux desktops still open.
- [x] Reproducible machines (`lordvm apply dev.toml`; 2026-11-24, in development; 6.24). Left: packages and `rebuild`.
- [x] Local MCP server (`machines mcp` / `lordvm mcp`), `exec`, `cp`, `logs`, `apply`, per machine with automation on
  (2026-11-24, in development; 6.15, 6.24). Left: exec in virtio-win guests, folders in `cp`.
- [ ] Migrate from another computer (6.11): a backup image of a Linux PC's disk imports and boots (2026-11-24); the
  agent on the old PC, the network copy and driver injection are open.
- [ ] Machine groups and the Whonix (Tor) card (19): G0–G4, Delete, Rename and Repair of a group built and checked
  headless (in development, 2026-11-24); the release gate G5 on real hardware and the members' disk encryption open.

### Phase 5, later: decided then

- [x] Own agent for other guests' windows: `lordos-guest-win` (7.1.2).
- [x] A real GPU on PCs, Intel GVT-g (6.10.1). Whole-GPU passthrough still open.
- [ ] A lightweight engine for Linux guests; reaching a machine from another computer over LordOS's remote desktop.

### Measurements and checks still open (any phase)

- [ ] 13.8 item 5: repeat the Pi's 4-vCPU drag on an untouched board, a memory-heavy guest at 3 vCPUs, the 1000 ms stalls with all
  cores busy (a desktop issue, also with no VM).
- [ ] 13.8 item 6: GICV trapping cost on the 16K kernel, and a 4K kernel (open decision 4).
- [ ] 13.8 item 7: render node by descriptor, one slice for all machines under `lord-sessiond` (3.7).
- [ ] 13.8 item 8: passt's 96–379 MB/s spread on the Pi.
- [ ] 13.8 item 9: boot time (UEFI is 12.3 s of the Pi's 18.6 s); direct kernel boot for Linux guests.
- [x] Our own seccomp filter on top of QEMU's: an allowlist since 2026-11-23 (in development), per architecture and per machine,
  measured on every configuration lord-vm makes that can run here, checked by `vmseccomp-test`, `vm-jail-probe` and the
  isolation gate (3.3). Not yet run under it: an Intel vGPU (GVT-g), a bridged tap, hugetlb memory.
- [ ] Open questions in 18 (6, 10, 11) wait for the maintainer; 1, 2, 3, 5, 8 and 9 are settled by the code.

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
containers (Android apps in the Store are for the separate LordBridge project, `docs/LORDBRIDGE.md`), and guests whose license ties them to one maker's hardware,
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
  └─ Machines app (React, apps/machines/*.js(x) → lord.vm.*)
       │  closed RPC
       ▼
  lord-vmdisplay  one per running VM: receiver and validator, sandboxed, no files, no network
  lord-vm         one per running VM: supervisor, sandboxed, owns the VM's folder
    ├─ qemu-system-<arch>  device engine, in its own sandbox and cgroup scope
    ├─ swtpm               TPM state, when the VM has one
    ├─ virtiofsd           one per shared folder
    ├─ passt               Shared network (Bridged: a macvtap descriptor opened by lord-vm)
    └─ lord-pgpv, lord-media  sandboxed helpers: a person's own system's signature, clipboard pictures
       ▲
  qemu ──p2p D-Bus──▶ lord-vmdisplay ──fixed binary messages, dmabuf by descriptor──▶ lord-server's compositor (MediaView)

Admin helper (closed actions, root)
  vm-enable, vm-kvm, vm-net-bridge, vm-net-unbridge, vm-memlock, vm-gvt-create, vm-gvt-remove,
  vm-image-get, vm-image-get-user, vm-image-list, vm-image-remove, vm-image-cancel
```

(Audited 2026-11-23: there is no usbredir and no USB action yet; USB passthrough is not built, 6.6.)

### 3.1 Parts

- **KVM** is the kernel's. We add nothing in the kernel.
- **QEMU** is the device engine, used through its control socket (QMP) and never as an app. Debian 13 ships 10.0.13
  (**Measured**, `apt-cache policy`). `qemu-system-x86` is 68 MB installed by itself (**Measured**).
- **`lord-vm`**, new, C, the supervisor: starts the tree, holds the QMP connection, applies settings, takes snapshots, reports state.
  It writes no display code.
- **`lord.vmcli`** (`server/src/ui/js_vmcli.c`, `apps/machines/cli.js`): the session socket `lordos-machines.sock` that the
  `machines` command (6.15) talks to; the app answers it.
- **`vmgvtwin.c`** (Intel's Windows graphics driver for a GVT-g machine, 6.10.1) and **`lord-pgpv`** (`lordpgpv.c`, the
  sandboxed OpenPGP check of a person's own system, 5.6.1) are parts of Machines too.
- **`lord.vm`** (`server/src/ui/js_vm.c`): the app's only way in, as `lord.rdesk` is for Remote Desk. There is no SDK
  wrapper: the app's own files (`apps/machines/model.js`, `catalog.js`, `diag.js`, `library.jsx`, `configure.jsx` and the
  other sheets) call `lord.vm.*` directly.
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

**3D graphics (built, 2026-11-20).** A Linux machine can have Graphics 3D (machine.json `"graphics": "3d"`, schema 5; Configure →
Display; the Productivity, Games and Design profiles pick it where the computer can give it). `lord-vm` then starts QEMU with `virtio-vga-gl` and
`-display dbus,p2p=yes,gl=on,rendernode=/dev/dri/renderD<N>` (`decide_graphics` in `lordvm.c`), only when all of these hold: the
device set isn't the windows one (machine.json refuses `3d` with it: no stable 3D driver for that system), the display is a
window, guest and host are amd64 (the arm64 abort of this section), QEMU's GL modules are there, and the render node lord-server
names (`--render-node`, the one its own GPU path uses) can be opened. Otherwise the machine starts with 2D and its log says why;
lord-server passes `--render-node none` when it draws without a GPU, so the CPU path never gets a 3D picture it can't read.

- With `gl=on` every picture is a dmabuf: `ScanoutDMABUF` (single plane: 10.0.13 has no `ScanoutDMABUF2`) and `UpdateDMABUF`.
  `lord-vmdisplay` checks size, stride, fourcc (`XR24`, `AR24`, `XB24`, `AB24`) and the descriptor's real length (`lseek`), copies
  nothing, and passes the descriptor on (`VD_DMABUF`, `vmproto.h`). QEMU doesn't let the guest draw again until `UpdateDMABUF` is
  answered, so the helper answers once lord-server read that frame (`VD_FRAME_DONE`) or after 250 ms: no tearing, and a stuck
  compositor holds the guest 250 ms at most.
- **QEMU's "Failed to call update: Timeout was reached"** (seen 2026-11-22 on a Windows 11 machine with an Intel GVT-g vGPU,
  five times, around a capture switch and when apps mode resized the guest to 1920x1080). QEMU gives each listener call 1 s
  (`DBUS_DEFAULT_TIMEOUT`, `include/qemu/dbus.h`) and logs this when an `UpdateDMABUF` gets no answer in time. lord-vmdisplay
  answers every listener call from its main loop at once, or holds an `UpdateDMABUF` 250 ms at most (above), and never calls
  QEMU and waits; `tests/vmdisplay_test.c` (a mock QEMU and a mock lord-server, in `make test`) checks it: with lord-server
  reading every frame, never reading one, slow with the picture for a new size, and 200 updates at once with input arriving, the
  slowest answer is 251 ms and every call is answered. QEMU itself drops listener calls: when it sends a new `ScanoutDMABUF` or
  `Disable`, every `Update`/`UpdateDMABUF`/`Scanout*` still waiting in its D-Bus queue is thrown away (`ddl_discard_display_messages`
  and the connection's filter in `ui/dbus-listener.c`, 10.0). Such a call never reaches the helper, its answer never comes, and
  QEMU logs the timeout a second later. That is the likely source of these lines: they come when the guest's picture changes
  buffers or size. On a vfio display (GVT-g) they don't stop the picture (QEMU's vfio display sends an `UpdateDMABUF` every refresh
  whatever is pending); on virgl the guest's drawing waits that second. To tell the two apart in a real session, lord-vmdisplay
  now logs a turn of its loop that took 200 ms or more, and at the end how many `UpdateDMABUF` answers it held and the longest
  (`the display listener's calls: …`): QEMU timeouts with a longest well under 1000 ms are QEMU's own drops. **To check** in the
  maintainer's session: those lines beside QEMU's.
- **A closed display connection stays registered in QEMU** (seen the same day): after the machine's window closed, QEMU went on
  calling the dead listener and logged `Failed to call update: The connection is closed` twice every 30 ms (32,557 lines in
  `lord-vm.log` in six minutes); the next window's connection is added beside it. That is QEMU's: QEMU 10's
  `dbus_console_register_listener` drops a listener on its connection's `closed` signal (`listener_vanished_cb`), yet the calls
  went on, and the D-Bus display has no call to unregister a listener, so lord-vmdisplay closing its connection (it does) is all
  this side can do; not fixed in QEMU. On 2026-11-22 the same flood filled `lord-vm.log`'s 4 MB cap and lord-vm wrote **nothing**
  more for the rest of that run (no agent or display lines). Since then (`src/vm/vmlog.c`, `build/vmlog-test`): a child's stderr
  is logged in whole lines (QEMU's timestamp and message no longer come out as two cut lines); a line that comes again with the
  same text (QEMU's own timestamp aside) is logged once and then counted, with `qemu: (the line before came N more times in S s)`
  at most every 10 s and when another line comes; and a full `lord-vm.log` is rotated (`lord-vm.log` → `.1` → `.2`, the oldest
  dropped; at most 3 × 4 MB) with a first line that says so, never left silent.
- lord-server imports each dmabuf once with EGL (`gpu_tex_dmabuf`; QEMU names the same few buffers again on each flip, known by
  inode, four kept) and reads the damaged rows back into the shared picture (`gpu_tex_read_rows`), so the rest of the picture's path
  (MediaView, thumbnails, recordings) is unchanged. It never waits on the guest's GPU work: the dmabuf's fence is polled, and a frame
  that isn't done yet is read at the next poll. An import the GPU refuses leaves the picture black and is logged once.
- QEMU's `y0_top` is in GL's terms: false means the first row in memory is the top (**Measured**, virgl on amd64, RX 580).
- virgl's state can't be migrated, so a running 3D machine can't be suspended or take or revert a snapshot with its memory:
  `lord-vm` refuses those (the app dims them), and the session's end shuts such a machine down instead of saving it.
- **Measured** 2026-11-20 on an RX 580 (amdgpu, Mesa radeonsi, Debian 13's QEMU 10.0.13 and virglrenderer 1.1.0): an Ubuntu 26.04
  guest with UEFI under the full sandbox; its kernel said `[drm] features: +virgl +edid` (read through the guest agent's journal),
  QEMU traced `virtio_gpu_features virgl 1` and `virtio_gpu_cmd_ctx_create ... name gnome-shell`, its picture came as `XB24`/`AB24`
  dmabufs with modifier `DRM_FORMAT_MOD_INVALID`, 1280 × 800, and `lordvm-dev` (built with Mesa it reads them back as lord-server
  does) saved the login screen right side up; 23 frames read back of 23, no refusals. A UEFI reset (QMP `system_reset`) didn't
  abort QEMU on amd64. Mesa in QEMU warns `os_same_file_description couldn't determine...` because our seccomp refuses `kcmp`: kept
  refused.
- **Known issue (QEMU, amd64 too, found 2026-11-23): a guest that resets its virtio-gpu-gl aborts QEMU.** On the maintainer's PC
  (RX 580, renderD129, Debian 13's QEMU 1:10.0.13+ds-0+deb13u1) an unattended install of LordOS 0.8.9 with Graphics 3D ended
  ~45 s in with `qemu-system-x86_64: ui/dbus-listener.c:511: dbus_scanout_texture: Assertion 'tex_id' failed.` and signal 6,
  right after the installer's `update-initramfs`, i.e. at the installer's restart. The path, read in Debian's 10.0.13 source and
  unchanged in 11.1.3: the guest writes 0 to the virtio status register from a vCPU, so `virtio_gpu_gl_reset()`
  (`hw/display/virtio-gpu-gl.c:116`) runs in the vCPU thread and calls `virtio_gpu_virgl_reset_scanout()`
  (`hw/display/virtio-gpu-virgl.c:1158`) → `dpy_gfx_replace_surface(con, NULL)` (`ui/console.c:838`) makes the "Display output
  is not active" placeholder and its texture with `dbus_create_texture` → `surface_gl_create_texture` (`ui/console-gl.c:75`),
  whose `glGenTextures` has no GL context current in that thread and leaves the texture 0; the D-Bus listener's
  `dbus_gl_gfx_switch` (`ui/dbus-listener.c:782`) passes it to `dbus_scanout_texture`, which asserts (`:511`). The comment
  above the call says GL must run in the main thread; `virtio_gpu_reset()` itself already defers to it (QEMU commit
  a41e2d97f92b, "virtio-gpu: reset gfx resources in main thread"), the GL reset of the scanouts doesn't. A host reset (QMP
  `system_reset`) runs in the main thread and is fine (above). **Which guests do it:** Linux guests whose kernel has
  `virtio_dev_shutdown` (`drivers/virtio/virtio.c`: break the device, then `config->reset`, on every shutdown and restart),
  backported into Debian's **6.12.117** (absent from 6.12.107 and 6.12.113): every Debian 13 guest updated to it, LordOS 0.8.9's
  installer and installed system among them, and any guest that unbinds its GPU driver. The guest has finished shutting down
  by then (file systems synced and read-only), so its disk is safe, but QEMU dies instead of powering off or restarting.
  **Upstream:** QEMU issue 4918 (https://gitlab.com/qemu-project/qemu/-/work_items/4918, opened 2026-11-20 against 11.1.3,
  open, same analysis); no fix in QEMU's tree: 11.1.3 has `assert(tex_id)` at `ui/dbus-listener.c:600`, and master (checked
  2026-11-23; last `ui/dbus-listener.c` change e12c1693, last `virtio-gpu-gl.c` change 4eb0aace) still calls the scanout reset
  from `virtio_gpu_gl_reset`, in the vCPU thread; no Debian bug against `qemu` (checked 2026-11-23); no QEMU option or guest kernel parameter
  turns the reset off. **What LordOS does:** an unattended install's start is always 2D (`vm_gl_refusal` in `machine.c`: the
  installer is a text console, and its restart is exactly this reset); the next start, the installed system's, has the
  machine's 3D again. A normal 3D start still hits it at every shutdown or restart of a 6.12.117+ guest, until QEMU moves
  the scanout reset to the main loop. lord-vm recognises QEMU's line (`vmlog_qemu_3d_abort`, `vmlog.c`) and, since
  2026-11-23 (in development after 0.8.9.5), names the end by what it knew just before (`vmlog_3d_abort_end`), so a
  guest's own shutdown or restart behaves like one:
  - **Which stop it was.** *LordOS asked* (Shut Down, closing the window, the computer's own stop: `begin_shutdown`)
    → a shutdown. *The guest said* → what it said: the LordOS tools for Linux (`lordos-guest` 6) have a
    systemd-shutdown hook (`/usr/lib/systemd/system-shutdown/lordos-guest` from the `lordos` package in a LordOS guest,
    `lordos-guest-local` written by Install Agent in other guests). systemd-shutdown runs it with `poweroff`, `halt`,
    `reboot` or `kexec` once every service has stopped and the file systems are read-only, just before the
    `reboot(2)` whose device shutdown resets virtio-gpu. It runs `lordos-guest --stopping <verb>` as root, which opens
    `org.lordos.agent` (free again: the person's helper has ended), sends `AGK_STOPPING` (kind 22, one byte: 1 power
    off, 2 restart; `vmagent.h`) and waits at most 1.5 s for lord-vm's echo. lord-vm keeps it for the run (a QMP
    `RESET` clears it: a restart QEMU did in place) and drains the port once more when QEMU ends. Helpers with the
    hook installed say `stopping` in their hello. This works with nobody signed in (the hook isn't the person's
    helper) and for every systemd guest; QMP has nothing to offer here: the abort comes before the guest's ACPI
    power-off or reset reaches QEMU, so no `SHUTDOWN` or `RESET` event is seen.
  - **What is recorded.** A shutdown: `stopped` with `guest-shutdown` (`host-shutdown` when LordOS asked), as any stop,
    no error screen. A restart, when the guest had run 30 s (`VMLOG_3D_MIN_RUN_MS`): `stopped` with `reset`, and the
    machine's window starts it again with the same settings, 3D kept (`restartsAgain`, `model.js`; the rule a machine
    with a graphics card already had). A restart that aborts again within 30 s of its start (a guest that never got
    up, or a restart loop) gets the error screen instead, so a restart happens once per boot that got somewhere. Nothing
    said, no helper with the hook in this run, and 30 s of running: most likely the guest's own shutdown or restart, but
    which isn't known: `stopped` with the new reason `qemu-3d-stop`, no error screen and never a restart; the off card
    says so (`machines.win.stopped3dHint`: if it was restarting, start it; with the guest agent and the clipboard
    helper a restart starts again by itself). Anything else, an abort not tied to a stop (a helper with the hook was
    there and said nothing) or within 30 s of the start without a word, keeps `crashed` with `qemu-3d-crash`, the error
    screen and **Use 2D Graphics and Start** (`lord.vm.set` `graphics: "2d"`, then a start).
  - **The disks.** QEMU didn't close them, whatever the name: run.json keeps code 6 (SIGABRT), and the next start runs
    the same `qemu-img check -r leaks` as after a crash for `stopped` with that code and `guest-shutdown`,
    `host-shutdown`, `reset` or `qemu-3d-stop`, as it does for `qemu-3d-crash`.
  - **Not covered.** A machine started from the command line, or kept running with its window closed, isn't started
    again after a restart (the window does it, as for a graphics card). A guest without systemd, or with tools older
    than 6, gets the "nothing said" rules. A Windows guest never has 3D (`vm_gl_refusal`).
  - **Tests:** `build/vmlog-test` (the line; every combination of `vmlog_3d_abort_end`), `build/vm-test`
    (`vm_gl_refusal`, `AGK_STOPPING`, the verbs, the hello word), `tests/guest_stop_test.sh` (`lordos-guest --stopping`
    against a fake lord-vm on a socket: the frame, the echo, 1.5 s without one, no port, a bad verb),
    `tests/vm_model_test.mjs` (the reasons, the offer, the restart, the hint). A scratch run of the real lord-vm with a
    fake QEMU (QMP, the agent's port, the assertion line, `abort()`) gave all eight cases as above: restart said after
    32 s → `stopped`/`reset`; power off said after 3 s → `guest-shutdown`; restart said after 3 s → `qemu-3d-crash`;
    nothing said, no helper, 32 s → `qemu-3d-stop`; nothing said with a helper that would say → `qemu-3d-crash`; nothing
    said after 3 s → `qemu-3d-crash`; Shut Down asked → `host-shutdown`; restart said by the hook alone (no hello) →
    `reset`. The echo reached the fake each time, and each next start ran the disk check. A real 6.12.117 guest with
    3D on real hardware is still to be checked.

### 3.3 Sandbox

QEMU parses guest-controlled data all day, so **assume it can be compromised**. What a compromised QEMU may reach is the design:

- **Own namespaces** (in development, approved by the maintainer 2026-11-23; `vm_userns_open`, `vm_ns_enter`, `vm_ns_setup` in
  `vmsandbox.c`): QEMU runs in its own user, mount, ipc, uts and net namespaces, with no capabilities and `no_new_privs`, in
  `app-lordos-lordos-vm-<id>.scope` (`setup_cgroups`, `lordvm.c`) with memory and CPU limits. The user namespace is made once at
  lord-vm's start, while lord-vm is still dumpable (a non-dumpable process can't write its own `uid_map`), by a short-lived
  child, and kept as a descriptor; QEMU's child joins it before exec. It maps only the person's uid (so D-Bus peer credentials
  with lord-vmdisplay still match) and no group. QEMU loses every capability at exec (its uid isn't 0 there). Its net namespace
  has only an unconfigured loopback: its sockets are the ones lord-vm passed, and abstract unix sockets of the session are out
  of its reach even without Landlock's scope. It stays lord-vm's plain child, so `waitpid`, PDEATHSIG, the cgroup leaf, vCPU
  pinning and root's `vm-memlock` checks see it as before. Where the kernel refuses user namespaces QEMU runs as before and the
  machine's log says so (`"namespaces":0` in `lord-vm --status --json`).
  **No pid namespace**: GLib 2.84 refuses peer credentials whose pid is 0 (`linux_ucred_check_valid`), and that is what QEMU sees
  of lord-vmdisplay from inside one, so QEMU's D-Bus display (picture, input, sound) can't authenticate: measured 2026-11-23,
  "Unexpected line 'NEGOTIATE_UNIX_FD' while in WaitingForAuth state", and reproduced with a plain GDBus server under
  `unshare --user --map-user --pid`. So a compromised QEMU can still name the person's processes: signals are refused
  (Landlock's signal scope), `setpriority` and `sched_setaffinity` too (QEMU's own `resourcecontrol=deny`), `/proc` of
  others too (Landlock), and `prlimit64` on another pid, `setpriority`, `kill`, `ptrace` and `process_vm_readv` stop QEMU
  (the seccomp allowlist, below).
  No cgroup namespace: lord-vm places QEMU.
- **Non-dumpable** (in development, same approval): `execve` makes a process dumpable again unless its binary or its ELF
  interpreter is unreadable. In QEMU's mount namespace the dynamic loader is a copy with mode `0100` mounted over the loader's
  path (made in lord-vm's private runtime folder and unlinked once mounted: a detached mount of a nameless `O_TMPFILE` can't be
  moved, ENOENT; then a write lease proves nobody else holds it open, and its inode and bytes are checked), so the kernel makes
  QEMU non-dumpable at exec. No root helper and no file shipped for it. **Measured** (2026-11-23, amd64 PC, Debian 13
  nocloud guest, 2 vCPUs, 1 GB, 5 starts each after a warm-up, alternating): fork to QEMU's exec 5 ms against 1 ms
  without, QMP ready 135 ms against 115 ms (spread 43–172 ms either way), boot to the serial login 18.5 s against 18.3 s,
  QEMU's RSS at login 409 MB against 404 MB (within run-to-run spread); the loader's copy is 225 KB of tmpfs per running
  machine. Nothing runs per frame or per guest exit, so there is no runtime cost beyond that. The kernel then looks for the first user namespace where
  the copy's owner and group are both mapped: QEMU's maps no group, so that is the computer's own, where only root has
  `CAP_SYS_PTRACE` (a namespace the person owns would let their programs back in). Its `/proc/<pid>` files become root's: the
  person's other programs, lord-vm itself and the test script that started it can't read its memory (`/proc/<pid>/mem`,
  `process_vm_readv`), environment, maps or descriptors (a memfd with guest RAM could otherwise be reopened through
  `/proc/<pid>/fd`; Yama's `ptrace_scope` 1 blocks attaching, not reading those), nor ptrace it. lord-vm checks it after exec
  (`vm_nondumpable`: `/proc/<pid>/environ` refused) and reports `"nondumpable"` in `lord-vm --status --json`. `status`, `stat`,
  `comm`, `cmdline` and `cgroup` stay readable, so Monitor, the vCPU use and the vGPU checks keep working; Monitor lists QEMU
  under root, as it already did lord-vm (also non-dumpable).
- **Nothing QEMU writes can run** (in development, approved by the maintainer 2026-11-24; `noexec_bind` and `vm_ns_setup` in
  `vmsandbox.c`): in QEMU's mount namespace, after the private loader is mounted and only after the user namespace is joined,
  the machine's `disks/` folder is bound over itself and that bind remounted `noexec,nosuid,nodev`; the same is done to
  `/dev/shm`, `/tmp`, `/var/tmp` and `$XDG_RUNTIME_DIR`, which QEMU's Landlock already keeps it from writing (they count only on
  a kernel without Landlock). The kernel then refuses to run a file there (EACCES) or to map one executable (`mmap` or `mprotect`
  with `PROT_EXEC`: EPERM), whatever seccomp lets through, which closes the disk-file half of the allowlist's kill fallback
  (below). These binds and the loader's are the only mounts added there. What the outer mount had stays (read-only stays
  read-only, its atime flags are kept; the kernel refuses to clear a flag the outer mount locked), and the loader's bind, a
  mount of its own made before, keeps exec. Nothing QEMU needs to run lives there: qcow2 and raw disks, the NVRAM, snapshots
  with memory (inside the qcow2) are read and written as before; swtpm, virtiofsd and passt run outside QEMU's namespaces
  and reach it through descriptors, as do the suspend file and a disposable session's memfd; guest memory is a memfd
  (hugetlb or not), which no mount covers. If a remount fails QEMU still runs and its log says exactly which folder and why
  (`lord-vm: QEMU's disks folder couldn't be made noexec in its mount namespace (...)`); Landlock and seccomp still hold.
  `lord-vm --status --json` says `"noexec_disks"`. **Checked** 2026-11-24 (amd64 PC, Debian 13): `build/vm-jail-probe` writes a
  real program (`/usr/bin/true`'s bytes) into a file of `disks/` from inside the jail with the allowlist in its kill fallback
  (no listener) and can neither map it executable (EPERM; with the remount left out, the same probe maps it) nor run it
  (EACCES); and real machines booted with it: a Linux guest with 2D graphics, one with 3D graphics (virgl), and an OVMF one
  with a TPM, through snapshot with memory and revert, reset, and suspend to a file and resume.
- **Landlock:** read `/usr` (execute only QEMU's binary and its loader, since 2026-11-23 with the seccomp allowlist), read the ISO attached to this VM; read-write only that VM's `disks/` folder (`vmsandbox.h`). On kernels without
  Landlock (see 2.3) the app shows "Weaker isolation on this kernel" (built, *in development*: `lord.vm.host().landlock` is the kernel's
  Landlock ABI; the library shows a notice under its toolbar, Configure → Security's sandbox line says what applies instead, and a
  running machine's Info menu reads its own VMS bits, which also name an engine that couldn't get its own namespaces; `model.js`
  `isolationGaps`). The machine log's "Sandbox:" line and Sentinela's `vmSandbox` alert say it too.
- **Seccomp:** QEMU's own `-sandbox on,obsolete=deny,elevateprivileges=deny,spawn=deny,resourcecontrol=deny` plus ours, an
  **allowlist** (in development; approved by the maintainer 2026-11-23 as isolation layer 3, for every machine). `vmseccomp.c` builds
  it from the rows in `vmsyscalls.h`; `vm_jail_qemu` (`vmsandbox.c`) applies it before the exec, so it covers QEMU from its first
  instruction. It replaced a denylist.
  - **The rows**: only the calls Debian 13's QEMU (1:10.0.13) was measured to make, per architecture (amd64 96 rows, arm64 92; a row the
    other kernel doesn't have doesn't compile there). **Measured** 2026-11-23 with the test build's learn-all mode, where every call
    that isn't a hot one reaches lord-vm, which logs it once and lets it go on (`SECCOMP_USER_NOTIF_FLAG_CONTINUE`), on: amd64 KVM and
    TCG with 2D; KVM with 3D on an AMD (radeonsi) and an Intel (i915) render node; a Windows-style machine (TPM through swtpm, secure
    boot, Hyper-V enlightenments, USB tablet, SATA discs, sound over D-Bus) at its installer; SeaBIOS with the compatible devices at
    Ubuntu's installer; arm64 KVM and amd64 TCG on the Pi 5. Each went through boot, resize, keys, clipboard, sound and microphone, a
    shared folder (virtiofsd, `share=on` memory), the agent channels, a snapshot with memory and its revert, reset, suspend to a file
    and resume (migration to a descriptor), a CD changed while running, passt restarted, shutdown and poweroff, with an
    `aio=native`, `cache.direct` disk on its I/O thread and vCPU pinning. A handful of rows weren't seen and stay, with the reason next
    to each (the vDSO's fallback for the clock, the event loop's epoll past 64 descriptors, glibc's `mremap`, `restart_syscall` after
    the vCPU kick's signal, Mesa's `sched_yield`, GIO's `shutdown`). Not run here, read in QEMU's source instead: an Intel vGPU
    (VFIO), a bridged tap, hugetlb memory.
  - **Argument rules**: `ioctl` only on fixed requests (the block layer's `BLKSSZGET` and `XFS_IOC_DIOINFO`; GLib's `TCGETS`,
    `TIOCGWINSZ` and `FIONREAD`) and on the request types of the machine's own devices: KVM's (`0xAE`) on a KVM machine, DRM's (`'d'`)
    on a 3D or vGPU machine, VFIO's (`';'`) on a vGPU machine, TUN's (`'T'` 200 and up) with a bridged tap; never `TIOCSTI`. `clone`
    makes threads only (glibc's `fork` gets EPERM; `clone3` gets ENOSYS and glibc falls back to `clone`). Executable memory,
    unless the machine runs TCG (whose code buffer must be writable and executable): every `mmap` or `mprotect` with
    `PROT_EXEC` goes to lord-vm, which lets through only the loader's kind (a private, read-only mapping of a file; on an
    arm64 CPU with BTI, `mprotect` to `PROT_EXEC|PROT_BTI` without `PROT_WRITE`), and only while QEMU starts: until its QMP
    handshake, when glibc's loader, QEMU's modules and Mesa's drivers are loaded (`vm_sc_startup_exec`; measured: no
    configuration above mapped code after it). After that, executable memory of any kind stops QEMU, whatever the file: a
    memfd (`MFD_NOEXEC_SEAL` wouldn't help: measured on 6.12, it doesn't stop `mmap` with `PROT_EXEC`) or a file of its
    `disks/` folder written to be run. `memfd_create` never takes `MFD_EXEC`. **What this doesn't stop**: anything during the
    start itself (QEMU has then read only its command line and its disks' headers); and, without a listener (the kill
    fallback, never on Debian 13's kernel), the loader's kind passes at any time, so a memfd QEMU wrote could be mapped to
    run; a file of its `disks/` folder can't, since that folder is `noexec` in QEMU's mount namespace (above). `prctl`
    only the five options measured, and `PR_SET_DUMPABLE` refused, so QEMU can't make itself dumpable again. `prlimit64` only on
    itself, `tgkill` only its own threads (the vCPU kick), no `kill`. `seccomp` never with a listener. `execve` isn't a row; `execveat` always
    reaches lord-vm, which lets exactly one through: the first, from the child it forked, before any of QEMU runs
    (`vm_sc_exec_once`; answered with `SECCOMP_USER_NOTIF_FLAG_CONTINUE`, safe there because that process is single-threaded and
    runs lord-vm's own code). Any later `execveat` stops QEMU: a descriptor number proves nothing once QEMU runs, since it can
    close it and put another binary under the same number. Landlock's execute right goes only to QEMU's binary and its loader
    (`/usr` is read-only otherwise; libraries and Mesa's drivers are mapped, which needs no execute right). Without a listener
    (the kill fallback) `execveat` passes on lord-vm's descriptor number with `AT_EMPTY_PATH`, so only QEMU itself could be run
    again, and QEMU's own `spawn=deny` refuses exec once its sandbox is up.
  - **Refused, never run**: `socket` (EACCES: its sockets are the ones lord-vm passed), `io_uring_*` (EPERM; not approved: QEMU
    probes it and keeps its other event loop), `kcmp` (EPERM; Mesa's amdgpu copes, as before). `socketpair` only AF_UNIX.
  - **Anything else stops QEMU, named**: the default action is `SECCOMP_RET_USER_NOTIF`. The filter's listener goes to lord-vm over
    the start's status socket (`SCM_RIGHTS`); the child closes its copy before the exec (QEMU holding it could answer for its own
    calls; the probe checks it holds none). The call waits in the kernel; lord-vm kills QEMU, then answers, so the call never runs,
    and writes `seccomp: QEMU made a call its allowlist doesn't have (thread N): <call and arguments>` to the log and
    `blocked by the sandbox: <call>` (with an ioctl's request, a prctl option or memory rights) to the failure page. Were lord-vm
    gone, the kernel fails the call with ENOSYS: it doesn't run either way. Where the kernel refuses a listener, the same rows run
    with `SECCOMP_RET_KILL_PROCESS` as the default (unnamed). `lord-vm --status --json` says `"seccomp_allowlist"` and
    `"seccomp_named"`.
  - **No switch for people**: only the test build `lord-vm-testpaths` (never installed) reads `LORDVM_TEST_SECCOMP`: `learn` and
    `learn-all` (log and go on: the way to see what a new QEMU needs) and `drop:<name>` (one row left out, to see a stop named).
  - **Cost** (**Measured** 2026-11-23, amd64 PC, Debian 13 nocloud guest, 2 vCPUs, 1 GB, 5 starts each after a warm-up,
    alternating with 0.8.9.5's denylist): fork to exec 1–2 ms either way, QMP ready 99–128 ms against 97–133 ms, boot to the serial
    login 12.06–12.39 s against 12.09–12.59 s, QEMU's RSS at login 391–422 MB against 396–417 MB: no difference past the spread.
    Per call (6 runs of 2,000,000 calls on one CPU, medians): `read` 120.5 ns against 121.9 ns (103.9 without a filter), `getppid`
    101.9 against 100.8 (84.5), an ioctl that the rules check 169.9 against 170.0 (117.3). The program has 400–422 instructions and
    finds a call in about eight comparisons; the kernel's cache skips it altogether for calls allowed whatever their arguments.
  - `qemu-img` and the other image tools keep the denylist (`vm_jail_tool*`), under their own sandboxes.
  QEMU's own filter costs nothing measurable: boot to login 36.5 s with it and 36.0 s without, QEMU RSS 424 MB against 422.5 MB
  (**Measured**, 2026-11-04, amd64, **nested**, 6 boots each). It also ran in every Pi run, 3D included, with no trouble (**Measured**,
  2026-11-04, Pi 5).
- **3D graphics (6.10, approved 2026-11-20):** only a Linux machine with Graphics 3D chosen gets, in QEMU's Landlock rules, read,
  write and device ioctls on the GPU's render node (`/dev/dri/renderD<N>`, never the primary `card` node), a listing of `/dev/dri`
  (names only) and read access to that GPU's PCI device folder in sysfs; without the last two Mesa's EGL doesn't start (**Measured**).
  Every other machine keeps no GPU access. QEMU's `rendernode=` takes a path, so the node can't come as a descriptor (yet). Checked
  through `--dev-qmp`: QEMU of a 3D machine still can't open `/dev/dri/card0`, `/dev/mem`, `/etc/hostname` or another sysfs device.
- `/dev/kvm`, `/dev/vhost-net`, render node and tap are opened by `lord-vm` and passed as descriptors, so QEMU can't open devices. For KVM, `-add-fd` plus `-accel kvm,device=/dev/fdset/NN`, which Debian 13's QEMU manual documents, works on both architectures with the sandbox on (**Measured**, 2026-11-04, Pi 5 and **nested** amd64). Passing the render node by descriptor is **To test**: in Phase 0 QEMU opened it itself, through the user's `render` group.
- On amd64 with `-nodefaults`, the guest needs a display device, or its kernel never starts (**Measured**, 2026-11-04, **nested**); `lord-vm` always adds one, `-display none` included. On arm64 `virt` has no keyboard of its own, so a machine with a picture also gets `virtio-keyboard-pci`.
- No network sockets of its own: networking is an fd. Debian 13's QEMU has no `-netdev passt`, so `passt` gets one end of a socketpair and QEMU the other through `-netdev stream,addr.type=fd` (**Measured**, 2026-11-04, both hosts).
- Files are opened by the supervisor and passed by descriptor wherever QEMU allows (`/dev/fdset`).
- No shell, no `-readconfig` from the guest's folder: arguments are built from validated settings.
- A machine can't outlive `lord-vm`: `PR_SET_PDEATHSIG` is set before exec. QEMU's newer `-run-with exit-with-parent=on` isn't in Debian 13's version (**Measured**: the 10.0 manual lists only `async-teardown`, `chroot` and `user`). swtpm exits with QEMU.
- `-M dump-guest-core=off`, no core dumps (`RLIMIT_CORE` 0), and non-dumpable processes (lord-vm and lord-vmdisplay by
  `PR_SET_DUMPABLE` 0, QEMU by its private loader, above), so that guest memory doesn't land in a host file.
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

What the code writes (`machine.h`, audited 2026-11-23): the NVRAM is `disks/nvram.qcow2`, the TPM state is under `disks/tpm/`
(with one `snap-<tag>.permall` per snapshot), the last picture is `thumb.png`, the logs are `log/lord-vm.log` (with `.1` and `.2`),
`serial.log` and `journal.log`, and there are `lock`, `run.json`, `operation.json` (while an operation runs) and `snapshots.json`.
Everything QEMU may write is in `disks/`.
`cpu-usage.json` is what the vCPUs used over the machine's runs, written by lord-vm for the vCPU recommendation (6.27).
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

Limits per machine aren't enough: three reasonable machines can starve the desktop. The design: all machines under one parent cgroup slice, with a memory ceiling for the sum, lower CPU and I/O weight than the session, and a floor kept for the desktop: 2 GB, or 1 GB on a 4 GB board. Starting a machine that doesn't fit asks first. Memory pressure (PSI) warns before swap does, and the summary shows the budget, not only host use.

**Built (in development, "Machines to 100 %"):** the ceiling for the sum is an admission check every start goes through, in lord-server, not in the app: `lord.vm.start` (the window) and `lord.vm.startDetached` (Start Without Window, `machines start` through the session, a group's members) call `budget_ok` (`ui/js_vm.c`), and the `machines` command's own start without a session makes the same check (`cli/machines.c`). `vm_budget_used_mb` (`machine.c`) adds up the library's active machines, the top level and a group's members, each as its cgroup limit (`vm_mem_limit_mb`: guest memory plus QEMU's share) plus a disposable session's cap (`vm_group_session_mb`, `vmgroup.c`); active is what `run.json` says with its lord-vm alive (`/proc/<pid>/comm`), read with `O_NOFOLLOW`, the starting machine left out by device and inode. The sum plus the new machine must fit in RAM minus the floor (`vm_budget_fits`); one machine alone always may start (its memory is already within `vm_mem_max_mb`). A refused start says `vm.overBudget`. The window asks first (`lord.vm.budget(dir)`): **Suspend ‹another›** for up to three running machines whose suspend frees enough (pausing frees nothing; a graphics card's or a 3D machine's can't be suspended) and **Start with ‹N› MB** (the most that fits, in 256 MB steps, at least 512 MB; `model.js` `budgetPlan`), or Cancel. The library's status line shows the memory the running machines hold of what machines may use, and "Short of memory" while PSI's `some avg10` is 10 % or more (`budgetLine`). Not built: the one parent slice with lower CPU and I/O weight than the session (each machine keeps its own `memory.max` and CPU limit in its own scope). Tests: `vm-test` (the sum: running, paused, stopped, a dead supervisor, a group's member, a damaged or linked `run.json`, a linked machine folder, an out-of-range pid, the excluded folder spelled another way) and `vm_model_test.mjs` (`budgetPlan`, `budgetLine`).

### 3.7.1 vCPU pinning

Built (in development, after 0.8.9.5). Configure → CPU & memory → **Pin cores**: Off or Automatic (`machine.json`
`"cpu_pin": "off" | "auto"`, schema 13, written only when chosen; `lord-vm --set DIR --cpu-pin off|auto|default`).

**Who does it.** `lord-vm`, to its own child: QEMU runs as the same user, so `sched_setaffinity` on its threads needs no
root and no new privilege. The code is `server/src/vm/vmpin.c`; `lordvm.c` only calls it.

**When.** Right after QEMU's control socket is up (before a vCPU runs, QEMU starts with `-S`), and before lord-vm's own
Landlock: lord-vm reads the topology from `/sys/devices/system/cpu`, takes the CPUs QEMU may run on from its affinity
(that is the cgroup's cpuset, so a restricted scope is respected), notes the cores other machines' vCPUs are already
pinned to (any `qemu-system-*` of the same user with single-CPU threads), and opens `/proc/<qemu>/task`. QEMU names its
vCPU threads in `query-cpus-fast` (`thread-id`); a thread id that isn't one of QEMU's own threads is refused. The list is
asked again after every reset and every resume, and set again only if the threads changed (or their number: then the
plan is made again).

**The choice** (`vmpin_plan`, tested with invented computers in `build/vmpin-test`):

1. Physical cores are `(physical_package_id, core_id)` pairs; SMT siblings share one. Fewer than 3 allowed: nothing.
2. The desktop keeps 1 core (2 when 6 or more are allowed), with their siblings: the lowest-numbered P cores (CPU 0 takes
   most interrupts), or E cores when keeping P cores would leave the machine fewer than 2.
3. The other free cores are ranked P before E, faster first, then by number. Hybrid CPUs are found generically: a core
   under 80 % of the fastest core's capacity is an E core; capacity is `cpu_capacity` (arm64), else the `cpu_core` /
   `cpu_atom` lists (Intel hybrid), else `cpuinfo_max_freq`.
4. One vCPU per physical core while there are cores, then the siblings in the same order. More vCPUs than those CPUs:
   nothing is pinned (two vCPUs on one CPU is worse than the scheduler's own choice), and the log says why.
5. QEMU's other threads (main loop, I/O thread, workers) get every allowed CPU no vCPU uses, without the siblings of the
   vCPUs' cores and other machines' cores while that still leaves some. Threads QEMU makes later inherit that.

On the i9-10900 (10 cores, CPU n and n + 10 siblings) a 4-vCPU machine gets CPUs 2–5 and its other threads 0–1, 6–11,
16–19; a second one started meanwhile gets 6–9 (the first one's cores are left alone) and 0–1, 10–11. The log line:
`cpu pinning: 4 vCPUs on 4 physical cores, 2 kept for the desktop; vCPUs on CPUs 2,3,4,5, QEMU's other 5 threads on
0-1,6-11,16-19`.

**Default.** A machine without a choice of its own gets Automatic with 4 vCPUs or more on a computer with 8 physical
cores or more (allowed ones), else Off (`vmpin_default_on`; Configure shows what the machine gets). **Measured**
2026-11-23 on the i9-10900 with the session running: a scratch KVM guest (host kernel, a static `/init` that runs a
fixed integer loop on every vCPU, 20 rounds, first 2 dropped) while a desktop-like load ran on the host (6 threads, each
a 60 Hz frame of 6 ms CPU, measured from tick to done), 4 runs each, pinned and unpinned alternated. Other work was
running on the computer, so the spread is wide:

| vCPUs | guest round, unpinned → pinned | host frame p99 | host frames over 16.7 ms |
|---|---|---|---|
| 2 | 0.522 → 0.531 s (1.7 % slower) | 7.71 → 6.15 ms | 0.040 → 0.010 % |
| 4 | 0.533 → 0.536 s (0.6 % slower) | 7.46 → 6.12 ms | 0.019 → 0.005 % |
| 8 | 0.582 → 0.566 s (2.7 % faster) | 15.20 → 9.86 ms | 1.709 → 0.124 % |

Pinned, the guest's rounds were steadier (4 vCPUs: 0.535–0.537 s against 0.517–0.557 s). With few vCPUs the scheduler
has room enough and a pinned vCPU can't move to an idle faster core, so small machines stay unpinned by default; from 4
vCPUs the desktop's worst frames improve, and with 8 both sides gain.

**Not done.** A guest topology that tells the guest which vCPUs are siblings (`-smp threads=2` matched to the pinning);
pinning QEMU's I/O to the desktop's cores only; NUMA memory placement (one package here).

### 3.7.2 Guest memory and huge pages (2026-11-23, in development)

What backs a machine's memory (`lordvm.c` `mem_backing`, `server/src/vm/vmmem.h`; the session log says which):

| Machine | Backing | 2 MB pages |
|---|---|---|
| No shared folder and the transfer folder off | QEMU's own anonymous memory (`-m`) | Yes, transparent huge pages: QEMU asks for them on all guest RAM (`MADV_HUGEPAGE` in `ram_block_add`) and Debian's kernel gives them with THP `always` or `madvise`. Checked: a running 2 GB guest had 1.94 GB of `AnonHugePages` |
| A shared folder or the transfer folder (on by default) | `memory-backend-ram,share=on` (vhost-user: virtiofsd maps the guest's memory; QEMU 10.0 makes it a memfd and maps it 2 MB aligned) | Yes on LordOS: shared memory gets huge pages where it asks for them with `/sys/kernel/mm/transparent_hugepage/shmem_enabled` at `advise`, which the `lordos` package sets (below). Checked: a running guest's touched memory was all `ShmemPmdMapped`. With `never` (the kernel's default, or an administrator's choice), 4 KB pages; the session log says which (`memory: shared …, 2 MB transparent huge pages (shmem_enabled advise)`) |
| Either, when the administrator reserved enough 2 MB hugetlb pages (`vm.nr_hugepages`) and all of the machine's memory fits in the free, unreserved ones at start | `memory-backend-memfd,hugetlb=on,hugetlbsize=2M,prealloc=on` (`share=on` with a shared folder) | Yes, allocated whole at start, so a guest never faults on a page that isn't there; otherwise the backing above, never a failed start |

virtiofs needs shared memory with a descriptor (vhost-user maps the guest's memory into virtiofsd), so a machine with a
shared folder can't fall back to private memory. lord-vm needs no privilege for any of it; the one system setting,
`shmem_enabled`, is the `lordos` package's (below).

**Measured** (2026-11-23, i9-10900, kernel 6.12.118, QEMU 10.0.13; a throwaway guest of the host's kernel, 2 GB, 4
vCPUs, whose 9 KB init touches and shuffles 1 GB into one random cycle, then does 3 × 20 million dependent reads over
it; 5 rounds, each configuration once per round, interleaved; medians of the 15 reads and of the 5 touch times; the
host in use, with two of the maintainer's machines running):

| Guest memory | `shmem_enabled` | Host 2 MB pages (`AnonHugePages` / `ShmemPmdMapped` of QEMU) | Read, guest THP | Read, guest 4 KB pages | Touch + shuffle 1 GB |
|---|---|---|---|---|---|
| QEMU's own (no shared folder) | any | ~1.16 GB of 1.2 GB touched | **74.2 ns** (73–77) | 93.1 ns (91–94) | 0.50 s |
| `memory-backend-memfd,share=on` (before) | `never` | 10 MB | 92.9 ns (90–109) | 108.1 ns (103–126) | 1.00 s |
| `memory-backend-memfd,share=on` | `advise` | **10 MB: none** | 93.3 ns (90–97) | 112.1 ns (107–121) | 1.03 s |
| `memory-backend-ram,share=on` (now) | `never` | 12 MB | 108.4 ns (102–131) | 140.6 ns (133–277) | 1.11 s |
| `memory-backend-ram,share=on` (now) | `advise` (LordOS) | ~1.15 GB | **74.6 ns** (72–79) | 96.1 ns (95–110) | **0.52 s** |

So a machine with the transfer folder now reads memory at random as fast as one without (75 against 93 ns, 20 %
less) and touches new memory twice as fast. Two things were needed, and neither alone is enough:

- **`shmem_enabled=advise`** (approved by the maintainer on 2026-11-23). Only memory that asks for huge pages
  (`MADV_HUGEPAGE`, which QEMU does on all guest RAM) gets them, so tmpfs, `/dev/shm` and other shared memory don't
  change. The `lordos` package ships `/usr/lib/tmpfiles.d/lordos-thp.conf` (`w
  /sys/kernel/mm/transparent_hugepage/shmem_enabled - - - - advise`), which systemd-tmpfiles applies at every boot,
  and migration `011-0.8.9.6-shmem-huge-pages.sh` applies it at once on an upgrade, only when the value is `never`
  (an administrator's `always`, `within_size`, `deny` or `force` stays) and only with systemd running (not in an
  image build, whose `/sys` may be the build host's). An administrator keeps another value with a file of the same
  name in `/etc/tmpfiles.d` (an empty one keeps the kernel's). On a kernel without transparent huge pages (the Pi 5's
  LordOS kernel) the file isn't there and both do nothing. Nothing else in sysfs is touched; lord-vm only reads it to
  log it.
- **`memory-backend-ram,share=on` instead of `memory-backend-memfd,share=on`.** QEMU maps a `memory-backend-memfd`
  only 4 KB aligned (the alignment of its page size; that backend has no `align` option), so the kernel can never put
  a 2 MB page there, whatever `shmem_enabled` says: with `advise` it still had 10 MB in huge pages. QEMU 10.0 backs
  `memory-backend-ram,share=on` with a memfd of its own (vhost-user gets its descriptor as before) mapped 2 MB
  aligned. Every machine type lord-vm pins is 10.0 or later, so its QEMU has this. A state saved with the old backend
  loads with the new one and back (both are the RAM block `mem`; checked with a saved state each way). Checked with a
  scratch machine started by lord-vm with the transfer folder: the guest mounted it and wrote a file through it, and
  QEMU's guest memory was `ShmemPmdMapped`.

The hugetlb row is unchanged (`memory-backend-memfd` with `hugetlb=on` takes its alignment from the 2 MB page size).

### 3.8 Why direct QMP, and not libvirt

Decided on 2026-11-01: `lord-vm` drives QEMU directly through QMP.

libvirt was considered seriously: its session mode runs as the user, Debian 13 has 11.3.0 (**Measured**), it supports `<graphics type='dbus' p2p='yes'>` through `virDomainOpenGraphics` (**Reported**, its documentation), and it brings machine-type and firmware handling, plus snapshots with years of edge cases. Against it: a second daemon in the session, XML as the contract, and its own process setup between us and QEMU, right where our sandbox has to be.

It stays only as a fallback, if the technical proof finds a blocker in direct QMP. Its internal snapshot code is worth reading in any case.

### 3.9 Phase 1 implementation

This section is the build log of Phases 1 to 3, kept with its dates; later work changed some of what it says. Where it and the
code differ, the code wins and the sections 5 to 7 describe what is there now. Corrections made by the audit of 2026-11-23:
`machine.json` is at schema 14 (11 `gpu`, 12 `capture_fullscreen`, 13 `cpu_pin` and `perf_profile`, 14 a machine group member's keys, 19.4) and `network` also takes `bridged` and `hostonly`; with the agent on,
the credential memfds start at fd 9, not 7; QEMU also gets `virtio-mouse-pci` (slot 0x14, Capture mouse), `usb-tablet` on xHCI for
the Windows devices and a `vfio-pci` display for an Intel vGPU, and `MESA_SHADER_CACHE_DISABLE` with 3D or a vGPU; the fixed PCI
slots now go past 8 (xHCI 0x5, HDA 0x9, seed disk 0xa, shared folders 0xb to 0xe, transfer folder 0xf, data disks 0x10 and up);
`VmMsg` has grown up to `VM_RENAME` and `VdMsg` carries dmabuf, guest audio, microphone, relative motion, capture and layout
(`vmproto.h`); the end reasons add `suspended`, `resume-failed`, `installed`, `qemu-3d-crash` and `qemu-3d-stop`; `lord-vm` has
`--set`, `--clone`, `--rename`, `--snapshot`, `--compact`, `--discard-state`, `--import-machine`, `--import-plan` and `--qmp`; the
tiles marked *Coming later* below are built; the network heals itself (passt restarted, "Self-healing network"); and the test counts
are now about 840 checks in `vm_test.c`, not 167.

Built on branch `machines-alpha` (2026-11-04/05) in two parts: the internals, then the window and the app (The app, below). Sources in
`server/src/vm/`, rules in `server/mk/vm.mk`, both binaries installed in `/usr/lib/lordos/bin` by `tools/iso/mkdeb.sh`
(QEMU and passt are not package dependencies).

**Files of one machine** (any folder; the app will use `~/Machines/<name>.lordvm`, 3.5). QEMU may write only in `disks/`, so a
compromised QEMU can't change `machine.json` (and through it, what the next start lets it read), the logs or the state:

| File | What |
|---|---|
| `machine.json` | The description, schema 1 (`machine.h`; schema 2 added `base`, `agent`, `clipboard`, schema 3 an imported machine's hardware, 6.22; 4 `shares`; 5 `secure_boot`, `tpm`, `hyperv`, `drivers`, `graphics`; 6 the network's modes, `bridge`, `ports`, `resolution`, `sound`, `volume`, `microphone`, `boot_order`, `boot_menu`; 7 `transfer`; 8 `sound_out`, `sound_in`, 6.9; 9 `clipboard_ask`, 6.8; 10 `pointer`, 7.1.2): `schema`, `id` (UUID, QEMU's `-uuid`), `name`, `arch` (`arm64`, `amd64`), `accel` (`kvm`, `tcg`), `vcpus`, `memory_mb`, `disk_gb`, `firmware` (`uefi`, `bios`), `machine_type` (pinned at creation, `virt-10.0` or `pc-q35-10.0`, read from QEMU), `firmware_code` and `firmware_sha256` (the UEFI build the NVRAM was made with; a changed build is logged at start), `display` (`window`, `none`), `network` (`shared`, `isolated`), `mac`, optional `iso`, `created`, `updated`. Unknown keys, wrong types and values out of range are refused; a newer schema is refused before its other keys are read and is never rewritten. No password is ever stored |
| `disks/system.qcow2` | System disk, made by `qemu-img` at creation (empty, or converted from an image with `--import`) |
| `disks/nvram.qcow2` | UEFI variables as qcow2, converted from the firmware's template (6.2). arm64 always uses UEFI (`AAVMF_CODE.no-secboot.fd`); amd64 uses UEFI (`OVMF_CODE_4M.fd`) when the `ovmf` package is there and SeaBIOS otherwise, which is what a Pi gets for an emulated amd64 guest |
| `log/lord-vm.log` | This run: lord-vm's lines, QEMU's and passt's stderr (whole lines; a repeated line once, then a count every 10 s), the command line (credentials named, never their values); 4 MB at most. `lord-vm.1.log` and `lord-vm.2.log` are the two runs before (6.19), or, when a run filled its log, that run's earlier parts: the log is rotated when full, never left silent, 12 MB at most in all |
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
                               └─ qemu-system-<arch>   fds: 3 /dev/kvm, 4 QMP, 5 serial, 6 network, 7… credentials (9… with the agent)
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
  `bootindex=0` on the system disk, an ISO as `scsi-cd` with `bootindex=1` (machine.json's `boot_order`, Configure → Boot, numbers the
  disk, the CD/DVD drive and the network card in the order chosen instead and gives the others no index, which SeaBIOS and OVMF
  may still try last; `boot_menu` adds
  `-boot menu=on,splash-time=5000`), `virtio-net-pci` on `-netdev stream,addr.type=fd`, `virtio-rng-pci`,
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
  QEMU's memory. Plain `Scanout`/`Update` arrays are still copied (QEMU uses them for surfaces it can't share); `ScanoutDMABUF`
  is taken for 3D machines (3.2, "3D graphics (built)"). Input goes out as D-Bus calls that expect no reply: evdev key codes mapped to QEMU's numbers
  (`vmkeys.h`), absolute pointer (relative while the guest's tablet driver isn't up; `IsAbsolute` is asked again while the pointer
  moves; relative motion only while the mouse is captured, 6.25), buttons, wheel steps, release-all on focus loss, and `SetUIInfo` at most every 100 ms for the window's size. The cursor
  (`CursorDefine`) goes to lord-server in its own sealed memfd.
- **lord-server's side of the picture** (since 2026-11-21, docs/MACHINES-GPU.md Part 1): each machine's lord-vmdisplay socket is in
  lord-server's main `poll()` (`js_vm_pollfds` / `js_vm_dispatch` in `ui/js_vm.c`, beside the media helpers'), with a 3D frame's
  dmabuf while its fence is pending. A frame is taken when it arrives and damages only the part of the MediaView where the picture
  changed (`media_src_damage_rect`: where the last paint put the picture, as fractions of the node's box; the whole node when
  rotated, mirrored or of another size); `VD_FRAME_DONE` goes right after the render that painted it (`js_vm_painted`). The
  Machines app's poll (`lord.vm.poll`, every 16 ms) only reports the state. Before, the frames waited for that poll and the
  acknowledgement for the poll after the next: a guest drawing 61 frames a second was shown at 29.5, now at 60.0, with 0.1 ms
  of paint a frame instead of 1.7 (**Measured**, the table in docs/MACHINES-GPU.md).
- **Pacing the picture** (2026-11-23, in development; `vm/vmpace.h`, `VD_PACE`, docs/MACHINES-GPU.md "Pacing a machine's
  picture"): at most one picture per refresh of the display, on every path. An Intel vGPU's picture that changed nothing 30
  times in a row is taken 100 ms apart until it changes or the person types, clicks, scrolls or moves the pointer over it
  (lord-vmdisplay ends that itself, at once). When no window shows the picture (the machine's window and all its apps mode
  windows minimized, on another desktop or wholly under an opaque window: `outOfSight` in `sdk/desktopmodel.js`, sent with
  `lord.vm.shown`), lord-server reads none of its frames and a 2D or vGPU picture is taken 500 ms apart; shown again, the
  next frame is read whole. QEMU's console waits for each held `UpdateDMABUF` answer, so a vGPU's vfio display is queried
  that much less too. A Linux 3D (virgl) picture is never held longer in the helper (the guest's GPU work waits on it).

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
| `VD_FRAME` → `VD_FRAME_DONE` | both | Damage x, y, w, h and a frame number; lord-server answers right after the paint that showed it |
| `VD_CURSOR`, `VD_POINTER` | from lord-vmdisplay | Cursor image in a memfd sealed against writing (`vm_cursor_check`), hotspot; pointer position |
| `VD_KEY`, `VD_MOTION`, `VD_BUTTON`, `VD_WHEEL`, `VD_RELEASE_ALL`, `VD_RESIZE`, `VD_RATE`, `VD_CLOSE` | to lord-vmdisplay | Input and window changes |
| `VD_PACE` | to lord-vmdisplay | The least time between pictures and why (idle, hidden), `vm/vmpace.h` |

**Command line, for development and tests:**

```
lord-vm --create DIR [--name N] [--arch arm64|amd64] [--accel kvm|tcg] [--vcpus N] [--memory MB] [--disk GB]
                     [--firmware uefi|bios] [--display window|none] [--network shared|isolated] [--import IMAGE] [--iso FILE]
lord-vm --start DIR [--paused] [--detach] [--keep-running] [--credentials-file FILE] [--dev-qmp]
lord-vm --status DIR [--json] | --check DIR | --stop DIR [--timeout S] | --poweroff DIR | --pause DIR | --resume DIR | --reset DIR
lordvm-dev run|attach DIR      plays lord-server: picture memfd, frames, PNG, typing, pointer, resize (server/tests/vm_dev.c);
                               lat (pointer move → frame / position times), fps; LORDVM_DEV_NODISPLAY=1 (commands only, while
                               lord-server shows the picture), LORDVM_DEV_CURSOR_PNG=FILE (the pointer shape the guest hands over)
```

**What is enforced, and where it stops.**

- QEMU: its own user, mount, ipc, uts and net namespaces and non-dumpable (3.3); Landlock (read `/usr`, execute only its binary and loader; read
  `/etc/ld.so.cache` and its own `/proc/<pid>`; read and write `disks/`; read the ISO;
  `/dev/null`; TCP bind and connect refused; abstract unix sockets and signals scoped), `no_new_privs`, our seccomp allowlist
  (only the calls QEMU was measured to make, with argument rules; `socket` refused; no `connect`, `bind`, `listen`, `accept`,
  namespaces, mount, `ptrace`, `process_vm_*`, `bpf`, `perf_event_open`, `keyctl`, `io_uring`, `userfaultfd`, modules, kexec,
  `TIOCSTI`, a second exec; without TCG no executable memory after the start (only the loader's mappings before it); anything else stops QEMU,
  named), then QEMU's own filter (3.3).
- passt sandboxes itself (namespaces and its own seccomp); lord-vm doesn't put Landlock on it, because Landlock would forbid the
  mounts passt's own isolation makes.
- cgroups: lord-vm enters `app-lordos-lordos-vm-<id>.scope` through lord-sessiond (memory.max = the machine's limit + 128 MB, cpu.max
  = vCPUs × 100 % + 50 %). With lord-sessiond, scopes belong to root, so lord-vm can't divide its scope: QEMU and passt share it.
  When lord-vm runs in a cgroup it may divide (a delegated scope), it makes leaves `supervisor`, `qemu` (the machine's memory limit,
  swap 0, its CPU limit) and `passt` (128 MB, 200 %); the test below runs that way. One slice for all machines, the sum of 3.7,
  needs lord-sessiond to create nested scopes: not done.
- vCPU pinning (3.7.1): after QEMU starts, lord-vm may pin each vCPU thread to its own host CPU and QEMU's other threads
  to the rest.
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
(never probed) with `"backing": null`, and runs in a jail like QEMU's (`vm_jail_tool`: `no_new_privs`, the image tools' seccomp denylist, no TCP,
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

The Machines app (`apps/machines/`) follows the approved mocks (Main, Assistant, Configure, Toolbar, FullScreen, FirstRun,
Requirements, BootFailed, Logs, PiView; see the mock artifact linked in 5). Controls of later phases (snapshots, clone, export, USB,
profiles, remote, apps mode, unattended install) are hidden, or shown disabled as *Coming later* where the mock puts them in a primary
spot; the lights and bars show only real numbers.

- **Control Center** (the library window): the summary bars *Running* (machines on of all), *Memory* (what the running machines' cgroups
  use, against this computer's RAM) and *Free* (free space where machines live); a card per machine with its picture (`thumb.png`,
  written by the machine's window every 5 s while it runs, so a stopped machine shows its last picture, dimmed), the system's own mark
  (a letter tile of ours, never a project's logo), name, system and architecture (*emulated*), what it's doing (lord-vm's state: no
  agent yet), the three lights (power; agent, always off; disk activity, from the cgroup's `io.stat`), CPU % of its vCPUs and memory from
  its cgroup (`lord.vm.stats`, two samples 1.5 s apart: `usage` in `model.js`) and what its disks use on this computer. Hover shows a
  round Start/Open button, double-click opens or starts, right-click has Start or Open, Pause, Resume, Shut Down, Restart, Force Off,
  Configure…, Logs, Show in Files, Eject ISO and Remove…. The app keeps what lord-vm doesn't need (the system's id and name, notes) in
  the machine's `app.json`. **FirstRun** (an empty library) offers New Machine and Import, and beside it *One step first* (`vm-kvm`) or
  *Virtualization is off* with *Continue Emulated*. **Requirements** (*What's Missing* in the header when something is) lists KVM, the
  engine, the firmware, the display module, the disk tools and passt with Ready/Missing and installs what's missing (`vm-enable`).
  On arm64 a **This Computer** page (PiView) says what the board runs natively, emulated or not at all, warns about an emulated machine
  and about machines on a microSD card (the library's device, from `/sys/dev/block`).
- **New Machine assistant** (full window, five steps: Choose, Profile, Name, Review, Create). Choose: *Download and install* tiles
  (LordOS, Debian, Ubuntu, Fedora) are *Coming later*, like *Migrate from another computer*; *Install from an ISO file* and *Import a
  machine* open the file dialog. The system is recognized from the ISO 9660 volume label (`lord.vm.isoInfo`) and the file's name
  (`systemOf`, `versionOf`), the architecture from the name (`archOf`), with *Other architecture…* to switch; an emulated choice says
  it's much slower. Profile: Light, Standard, Development (`profileSize`, kept within this computer). Name: prefilled
  (`suggestName`), the root password for imported cloud images, and *Customize settings before starting*. Review lists everything and
  what's missing to install; Create makes the machine and opens its window (it starts), or opens Configure instead.
- **Configure** (a window per machine): a sidebar of sections with one-line summaries, General (name, system, notes, architecture,
  created, folder), Startup and shutdown, CPU and memory (cores and memory with bars and the cgroup limit), Display, Network (Shared or
  Isolated, MAC), Sound and input, Disks (grow the system disk, insert or eject an ISO), Sharing, Boot, Security (what the sandbox
  does). Changes go through **`lord-vm --set`** (new: under the machine's lock, refused with exit 3 while it runs, the same checks as
  `--create`, the disk only grows, through the jailed `qemu-img resize`), as a background job (`lord.vm.set`); while the machine runs
  they are dimmed with a note. What later phases bring (suspend on close, 3D, forwarded ports, sound, shared folders, TPM) says so.
- **Machine window**: a toolbar with the power light and name, the resolution (*Follow the window* or a fixed size sent to the guest),
  Devices (the CD/DVD; changing it, shared folders and sound later), Logs, Screenshot (the guest's picture at its size into
  `~/Pictures/Screenshots`, with a notification), Pause or Resume (QMP `stop`/`cont`, VM_PAUSE/VM_RESUME, already in lord-vm and in the
  isolation test), Power (Shut Down, Restart, Ctrl+Alt+Delete, Force Off), Configure… and Full Screen; the menu bar has **Actions** and
  **Devices** with the same items. In full screen the bar floats at the top edge (shown when the pointer reaches it) with *Leave Full
  Screen*, and a hint at the bottom for five seconds. A failure shows **BootFailed**: why, the likely fix and lord-vm's detail,
  Start Again, Configure… and the machine log with Copy. **Logs** (a window): the machine log or the guest's serial console, a filter,
  Pause/Follow, Copy, Save (to Documents), levels guessed from the words.
- `lord.vm.stop(dir, how)` (the library's menu, through the runtime socket) takes `shutdown`, `poweroff`, `pause`, `resume` and
  `reset`, and waits up to half a second for lord-vm's answer before closing the socket.

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
the host's pointer (except while the mouse is captured, 6.25: then only the image is drawn where the guest reports its pointer,
inside the picture; the host's pointer itself never moves by what the guest says). VD_POINTER's *hidden* (the guest turned its cursor plane off: a text console, or X just quit) brings the host's arrow
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

**The memory budget at start** (3.7; since replaced by lord-server's own check on every start path, 3.7 "Built"). `vm_budget_fits` had no caller. The window then checked, before it started a machine, the machines
already running plus this one, each with QEMU's overhead (384 MB with KVM, 640 MB emulated: `memLimit` and `budgetCheck` in `model.js`),
against what this computer lets machines use (RAM minus the desktop's floor); one machine alone may always start, since the sheet keeps
its memory within that. On VM 125 (2,897 MB for machines) with the emulated machine running (1,664 MB), starting the 1 GB KVM machine
(1,408 MB) was refused with *this one needs 1408 MB with QEMU's own share, and 1233 MB are left for machines*.

**Session restore** brought every open machine window back as one more library window (they're `machines#vm-…` windows whose
machine isn't part of the saved state): VM 125 had collected twelve. Machines is now a single-window app (manifest `singleWindow`):
only its library is saved, and a saved list that names it more than once opens it once.

**The Raspberry Pi 5 with the app** (2026-11-05, the user's board, 16 GB, LordOS built from `pi-machines` = `next-0884` with this
branch). *Allow* gave the account the kvm group and an access-list entry on `/dev/kvm`, but the first start failed with QEMU's *Failed to
retrieve host CPU features*: `/dev/kvm` carries udev's `uaccess` tag, and systemd-logind rewrote its access list at the next login of any
kind (an SSH login was enough), keeping only the active seat session's user, which a LordOS session isn't. `vm-kvm` now writes
`/etc/udev/rules.d/71-lordos-kvm.rules` (`TAG-="uaccess"` for kvm) so the entry stays until the next boot, and from the next login the
kvm group covers it; after a reboot the session had group 992 (kvm) and machines started with KVM. Under GPU composition the machine's
picture was see-through: QEMU's x8r8g8b8 leaves the fourth byte 0, which the CPU composition never reads but the GPU's takes as the UI's
hole for app windows; `MediaSrc.xrgb` now draws it with alpha 255 (`force_alpha` in `mview.c`, two checks in `mview_test.c`). Then: Debian
13's nocloud arm64 qcow2 imported as an arm64 KVM machine from the app (2 vCPU, 1 GB), started in its window, root login, `uname -a`
(aarch64), `nproc` 2, 963 MB, the Shared network's address, Shut Down (`host-shutdown`); the amd64 nocloud qcow2 as an emulated machine
(the sheet's Install added `ovmf`; `accel: tcg`, `pc-q35-10.0`, UEFI), its console and login in about 2½ minutes, `uname -m` x86_64,
`nproc` 2, Shut Down. With the new UI, the Control Center showed both cards, the running machine's live picture, and *This Computer*
(Raspberry Pi 5 Model B Rev 1.1, 15.8 GB) with the emulated-machine and microSD warnings.

**The new UI on screen** (VM 125, 2026-11-05; screenshots in the branch's test notes, `machines-shots3`): the Control Center with three
machines (one running X with an xterm and a clock: its live picture, CPU 1 %, memory 1 GB, power light on, the summary *1 of 3*, *1 of 3.8
GB*, *21.4 GB* free), Pause and Resume from the right-click menu (the card said *Paused*), Configure saving 1280 MB on a machine that was off
(`machine.json` changed) and dimming everything on a running one, the Logs window on both logs, the assistant from Debian 13.7's ISO
(recognized as *Debian 13.7, amd64*, Light profile, *Create and Start*: the installer's menu in the new window), the window's Power,
Devices and resolution menus, Screenshot (a PNG of 1022 × 598 in `~/Pictures/Screenshots`), the floating full-screen bar, BootFailed
with the ISO moved away (*the attached image is missing*), FirstRun with an empty library and Requirements.

**Not done or not verified:** ~~Esc pressed while one of the window's menus is open closes it and also reaches the guest~~ (fixed in the
beta, below); BIOS machines were built for but not run from the app; ~~the package engine's download of `vm-enable` waited with no time
limit while the network didn't answer~~ (beta); ~~ejecting an ISO from a running machine and attaching another one~~ (beta); one slice for all machines and nested
leaves under lord-sessiond; ~~our seccomp allowlist for QEMU~~ (in development, 3.3); a guest cursor larger than 64 × 64 after scaling is drawn smaller (the
cursor plane's size); HiDPI (the guest is asked for the window's logical size); render node and 3D; latency to the screen. Unit tests: `vm_test.c` 167 checks (with `vm_image_kind`), `vm_model_test.mjs` 66 checks (states, actions, the sheet's
checks, letterbox and pointer scaling, guest size, refresh rate, wheel steps).

**Phase 2, the beta.** Built on branch `machines-beta` (2026-11-05), from `machines-alpha`, in the order of the beta's list; each part is its own commit. The
messages of 3.9 grew (`vmproto.h` is the contract: `VM_CDROM`, `VM_SUSPEND`, `VM_KEEP`, `VM_SNAPSHOT`, `VM_CONSOLE`, `VM_AGENT`, `VM_CLIP`
to lord-vm; `VM_NOTICE`, `VM_JOB`, `VM_CONSOLE_FD`, `VM_AGENT_STATE`, `VM_CLIP_DATA` from it; reasons `suspended` and `resume-failed`); the
version number is unchanged, since both ends ship together and an older peer drops what it doesn't know. `machine.json` is schema 2 (`base`,
`agent`, `clipboard`): a schema 1 file is read as is and written as 2 with `machine.json.schema1` kept, as 3.6 says. Measured on the amd64
build VM (**nested**, Debian 13 nocloud guest, 512 MB to 1 GB) and on screen on VM 125 (LordOS amd64, nested) unless said otherwise.

*Phase 1's open items.*

- *Esc in a menu*: a key pressed while one of the window's menus (or the agent's popover, or the disc picker) is open stays with it, down
  and up. Seen on VM 125: with Devices open, `x` didn't reach the guest's login prompt and Esc closed the menu; with no menu, `x` did.
- *CD/DVD while running*: the drive is always there (`virtio-scsi-pci` at slot 8, after the slots a machine always had, so no interface
  moves). `VM_CDROM` carries a descriptor lord-server opened for the person; lord-vm hands it to QEMU with QMP `add-fd` and inserts a raw
  node on `/dev/fdset/N` (`blockdev-open-tray`, `-remove-medium`, `-del`, `-insert-medium`, `-close-tray`), so QEMU never opens a path and
  its Landlock rules don't change; `machine.json` follows. Devices → Choose Disc Image / Eject Disc; `lord-vm --cdrom DIR IMAGE|none`. Seen
  on VM 125 with Debian 13.7's netinst ISO; the guest's state carried across a suspend and resume with the disc in.
- *Install timeout*: the package engine's HTTP client tries all of a host's addresses for 20 s together, and a host that didn't connect or
  answer isn't tried again in the same batch (`pkg/http.c`), so `vm-enable` on a network that drops packets ends in about a minute; the
  Requirements page also says the network isn't answering after 90 s without a word from the engine. Reproduced on VM 125 with passt
  removed and TCP 80 and 443 dropped by nftables: *Downloading: 0 of 0.3 MB*, then *Reading the package sources (0 of 5)* after the
  download gave up, and *The package sources didn't answer: check this computer's network, then try again* 61 to 74 s after Install;
  the page stayed usable (Back, the library), and Install worked once the rule was gone. The page now says the engine's progress in
  words and keeps its message in place of the note under the rows, so its buttons stay in the window.
- *passt crashing*: the network heals itself (below, *The network heals itself*).

*Suspend and resume (6.1).* `VM_SUSPEND` stops the vCPUs and migrates to `file:/dev/fdset/90`, a descriptor lord-vm opened on
`.suspend.state.tmp` (QEMU 10 deprecates `fd:` for files; the 128 MiB/s default limit is lifted), with progress from `query-migrate` as
`VM_JOB`; `suspend.json` records QEMU's version, the machine type, this CPU (a hash of its model and features), vCPUs and memory; both are
renamed into place under `operation.json` "suspend" and QEMU quits: *stopped, suspended*. The next start passes `-incoming defer`, loads the
state through `migrate-incoming` under "resume" and removes it before the first `cont`. A state from another QEMU, CPU, machine type, vCPU
count or memory size isn't loaded (*resume-failed*, the state kept): Try Anyway (`--resume-anyway`) or Discard (`--discard-state`).
Measured: a 512 MB guest saved 288 MB in 0.9 s and resumed in 0.4 s (build VM); a 1 GB guest saved 326 MB in 2.6 s and resumed in 0.3 s
(VM 125, from the window; text typed at its login prompt was still there). **Keep Running** (`VM_KEEP`) leaves a machine on when its
window closes; **Start Without Window** runs `lord-vm --detach --keep-running`; the card says *No window*. Closing a window asks Suspend,
Shut Down or Keep Running unless Configure says which. **Keep awake**: the session's idle sleep asks `lord.vm.awake()` and skips while a
running machine keeps the computer awake (app.json `keepAwake`, on by default); the guest is kept awake through the agent (below). Whether
the idle sleep was really held off wasn't watched on screen.

*Snapshots (6.2).* qcow2 internal snapshots, the doc's decision. Running: QMP `snapshot-save`, `snapshot-load` and `snapshot-delete` jobs
over every writable node (system disk and UEFI variables), the memory into the system disk, followed through `JOB_STATUS_CHANGE` and
`query-jobs`, the memory's size from `query-block`. Off: `qemu-img snapshot -c/-a/-d` per disk, jailed like an import. A snapshot with
memory on a machine that's off is gone back to by starting from it: a `snapshot-load` before the first `cont` (`--loadvm`; QEMU's own
`-loadvm` picks the UEFI variables for the memory and refuses). `snapshots.json` (names, times, whether memory, its size) is written last;
pictures are `snapshots/<tag>.png` (the live picture from the window, else the card's). A delete removes the entry first. At every start a
tag the list doesn't name is removed. Measured (build VM): take 1.0 s, go back 0.55 s; on VM 125, Go Back to This with *a snapshot of now
first* took both and went back in 3 s, and the text typed after the snapshot was gone. At most 64; a warning from 20 (6.2).

*Clones (6.3) and storage (6.16).* `lord-vm --clone DIR NEWDIR [--duplicate] [--linked] [--from-snapshot TAG]`, with the source off: a
reflink copy first (btrfs, XFS); with `--linked` on ext4 the source's disk moves to `<library>/.shared/` as a read-only base and both machines
get an overlay (`qemu-img create -u -b`, the base never opened) under "link" in the source; otherwise a Clone is converted (flattened,
without snapshots) and a Duplicate copied with its holes, snapshots and saved session. A linked clone's base is named in `machine.json`;
QEMU gets it as a read-only node and never follows the overlay's header; it is QEMU's only extra Landlock read; qemu-img's jail takes a
second read file to flatten one. A Duplicate keeps the id and won't start while its original runs. Measured: a linked clone of a 1.2 GB disk
in 0.14 s, both then running together; a full clone in about 5 s. **Compaction** (`--compact [--estimate]`, Reclaim Space) rewrites the disk
into a new file, checks it and renames it over, under "compact"; refused with snapshots, a saved session or a base. The **Storage** window
(mock *Storage*) is per 6.16: the volume's bar (machines, disc images in use, the Trash, other files), each machine with its clones under it,
what deleting it frees (its files less what it shares by reflink, `FIEMAP_EXTENT_SHARED`, plus its base when it's the last user, the Trash
counted), the split (disks, saved session, snapshots as *about* their memory's size, other), Reclaim Space with an estimate first, and shared
bases nobody uses. Shared extents weren't seen: VM 125 and the build VM are ext4.

*Profiles, resolution, recording.* Profiles per 5.6 (Productivity, Software Development, Testing, Design, Light desktop) scaled to the
host; Testing is Isolated; Design asks for 3D graphics (a Linux machine gets them where the computer can give them, 6.10; others say why they start with 2D). The resolution menu lists the sizes that fit the display. **Record** feeds the guest's
picture (not the screen) into lord-media's screen recorder (`media_framerec_*`), into `~/Pictures/Screenshots`; no sound yet. Seen on VM
125: a 26 s recording, a 1022 × 598 H.264 MP4 of 25 KB, and the notification with Open and Show in Files.

*Logs and the Shell (6.18, 6.19).* `VM_CONSOLE` hands out one stream to the serial console (output also to `serial.log`; the last 8 KB
replayed without terminal queries), shown with `term_create_fd` in a pane under the picture, marked GUEST with the machine's name. This is the
serial console, not the agent's shell of 6.18, so it works with no agent and no network but logs in like a console. Logs opens on an
**Overview** (unified diagnostics from run.json, the machine log and the library: state, last start, QEMU's version, sandbox, limits, network
notices, storage, an operation waiting, recoveries, problems; Copy Report), then the machine log, the serial console and the guest's journal.

*The guest agent (6.17, 7).* Opt-in per machine (`agent`; off for Testing). virtio-serial at slot 7 with two ports, sockets lord-vm holds:
`org.qemu.guest_agent.0` for **qemu-guest-agent** (the guest's package) and `org.lordos.agent` for our own channel. The doc leaves the
clipboard path open (6.8: QEMU's D-Bus clipboard needs a vdagent in the guest), so the clipboard goes through our channel and **lordos-guest**
(`tools/guest`, a small static program packaged per architecture), which reads and writes the clipboard with the guest's `wl-paste`/`wl-copy`
or `xclip`, as the desktop user, and never runs anything the host names; frames per 6.17 with a 1 MiB limit, parsed in lord-vm. It looks
for the tools in PATH at the start, every 30 s and whenever one couldn't be started; on GNOME with `DISPLAY` set it uses `xclip` only
(Mutter bridges Xwayland's CLIPBOARD; it has no data-control protocol, so `wl-paste` in the background needs a focus-stealing popup there),
elsewhere `wl-paste`/`wl-copy` under Wayland, then `xclip`. With neither installed (a stock desktop ships none) its hello says `noclip`
instead of `clipboard clipx`: lord-vm sets `VAC_CLIP_NOTOOL` instead of `VAC_CLIPBOARD`, the popover's clipboard line says *Needs xclip*
with a hint (install the helper again, or `sudo apt install xclip`), and the guest's journal says which tool is used or that none is.
Install Clipboard Helper copies it in through `guest-file-write` and one fixed script (udev rule, user unit; then, when `xclip` is
missing, `xclip wl-clipboard` through the guest's own apt-get, dnf, zypper or pacman, best effort and never failing the install offline;
then the helper restarted in each signed-in desktop session, `systemctl --user -M <user>@`). lord-vm follows the script with
`guest-exec-status` and answers when it ends (10 minutes at most), so the button stays busy meanwhile; a second click while it runs is
the same request (lord-server) or `busy` (lord-vm). The agent's light is on the card and
first on the bar, with a popover of what works now. Seen on VM 125: *Debian GNU/Linux 13 (trixie), agent 10.0.13*, journal in Logs, guest
kept awake, the helper installed in 60 ms (build VM) and from the popover, and the clipboard both ways (a guest with Xvfb and xclip: text
copied in the guest was in LordOS's clipboard history; a report copied in LordOS was in the guest's clipboard after the window came to the
front). Not tried: a guest desktop session starting the helper by itself (the unit waits for `graphical-session.target`), Wayland guests;
the tool's choice and the `noclip` hello are checked with stub tools only (`tests/guest_clip_test.sh`), not yet on a real GNOME guest.

*Release gates (12).* Scripts in `server/tests/`, each `sh tests/<gate>.sh BUILD_DIR …`, run on the amd64 build VM (**nested**, ext4):

| Gate | Script | What it does | Result |
|---|---|---|---|
| Disk integrity | `vm_integrity.sh` | SIGKILL of QEMU or lord-vm while the guest writes, in a snapshot, a revert, a suspend, three full clones, four linked clones (15 to 60 ms in) and two compactions; `qemu-img check` after each and after the next open | **46 passed**, 0 failed (leaked clusters only after some kills, repaired at the next start) |
| Failure recovery | `vm_recovery.sh` | kills lord-vm in the middle of each operation with an intent file (suspend, resume, snapshot take, delete and revert, compaction, linked clone) and checks the next open finishes it or rolls it back, says so, and leaves clean disks | **22 passed**, 0 failed |
| Leaks | `vm_leak.sh` | 100 start/stop cycles, then no process, zombie, socket, cgroup, lock or operation left, logs rotated, disks clean | **10 passed**; 100 cycles in 145 s, 1456 ms each on average, the slowest 5244 ms |

The first runs failed on things the gates were written to find: `qemu-img check` right after a kill raced the dying QEMU for
the disk locks (recovery now waits for it to be gone), a killed compaction left a `qemu-img convert` running (tools are now
started with `PR_SET_PDEATHSIG`), and the build VM's 2 GB `/tmp` ran out of space under full clones (the gates now work in
`/var/tmp`). Some kills land after the operation finished; the gate then says so and skips the "interrupted" checks rather than
pass them.

*On the Pi 5 (arm64, LordOS kernel, KVM).* The arm64 package of this branch, merged with 0.8.8.4's work so Remote Desk, the
single sign-on VPN and the Sound balance stayed (checked after the install), was installed and the Pi rebooted once. With the
Debian guest made in Phase 1: suspend 6.6 s for 393 MB of state, start from it 0.43 s (state loaded in 216 ms), a snapshot with
memory 6.5 s, going back to it 1.4 s, and delete. The gates weren't run on the Pi, and its windows weren't driven (it's the
user's desktop).

*The beta's open items, closed (2026-11-05).* Each its own commit on `machines-beta`; seen on VM 125 (LordOS amd64, nested) from the
app, measured on the build VM, and on the Pi 5 below.

- *The network heals itself (6.5).* The card sits on a QEMU hub (`-netdev hubport,id=nic0`) and passt on another port of it (`pst0` on
  `hp0`). A child forked before lord-vm's own Landlock (the *keeper*) starts a new passt on request and hands back its socket, its stderr
  and a pidfd; lord-vm takes the old backend off (`netdev_del hp<n>`, `pst<n>`) and puts the new one on (`getfd`, `netdev_add` `socket`
  with the descriptor's name, `hubport` `hp<n+1>`). The card, its MAC and the guest's interface don't change. QEMU's `stream` backend
  can't take a named descriptor once QEMU runs (it connects from a thread: *Unable to parse FD number*), `socket` can, and passt speaks
  the same framing. Right away the first time, then after 1, 2, 4, 8 s; after 5 in a minute the machine keeps no network and says so.
  `VM_NOTICE network-lost` (*reconnecting*) and `network-back` (milliseconds without network) show as a short line in the window.
  Measured: lord-vm attaches the new passt 4 to 7 ms after the old one ended; the guest's next HTTP fetch 0.22 s (build VM) and 239 ms
  (recovery gate) after the kill, then 1.1 s and 2.6 s after two more kills in the same minute. On VM 125, passt killed during a 12.7 MB
  download at 300 KB/s in the guest: the transfer didn't stop while the guest drained what it had received, its TCP connection was reset
  18.8 s later (passt's sockets went with it), curl resumed in 0.1 s and the file arrived intact (`gzip -t`). The recovery gate kills
  passt three times while the guest fetches twice a second, then until lord-vm gives up, and checks the machine keeps running.
- *Clipboard beyond text (6.8).* `AGK_CLIPX` on `org.lordos.agent`, when both hellos say `clipx`: text, `text/html` and `image/png` in
  one frame (8 MB for a picture, 1 MB for text, 8192 px a side). lord-vm checks what either side sends: UTF-8 text; HTML sanitized
  (formatting tags and their text; `script`, `style`, `iframe`, `svg` and the like go with their content; no event attributes; `href`
  only `http`, `https`, `mailto` or `#`; `style` only for colors, weights, sizes and alignment, never `url`, `expression` or `import`);
  a PNG header that is believable. lord-server re-encodes a guest's picture in lord-media's sandbox (decode and write a new PNG,
  `MW_EDIT`) before it goes on the clipboard, and puts rich text on the clipboard with its text (`wl_clip_set_html`): offered as
  `text/html` to Wayland and X11 apps, and read from apps that offer it, also from one that offers only HTML. `lordos-guest` 2 reads the
  guest's `TARGETS` (`xclip`, or `wl-paste -l`) and writes one form back (the tools hold one type each). After a resume, lord-vm asks the
  helper to say hello again. Seen on VM 125 with an X guest (Xorg on its screen, `xclip`, `feh`): a picture copied in the guest was in
  LordOS's clipboard history, re-encoded (573 bytes in, 784 out); a picture copied with `wl-copy` on the host arrived in the guest with
  the same MD5 and was shown by `feh`; rich text with a `<script>`, an `onerror`, a `javascript:` link and a `url()` background went both
  ways without them. 33 unit checks (`vm_test.c`) cover the layout, the PNG header and the sanitizer. Files by copy and paste or drag
  weren't in the beta; dropping files on a machine is built since (6.7.1, 6.21).
- *Recordings with sound (6.9, 6.20).* Every machine has an HDA card (slot 9; a state saved without it still loads, checked: suspended by
  the previous build, resumed by this one). With a window, QEMU's audio goes out on the peer-to-peer D-Bus display (`-audiodev dbus`,
  48 kHz, 16-bit stereo) and lord-vmdisplay registers an `AudioOutListener` (with its `Interfaces` property, while serving the console's
  listener too: QEMU asks both for their properties before it answers). While Record runs, lord-vmdisplay writes the sound, mixed to one
  channel, into a ring (aring.h) lord-media reads as the recording's AAC track, with silence for the time the guest plays nothing and
  nothing more than 200 ms ahead of the clock; the picture is fed at 30 frames a second. Measured on VM 125, the guest playing three
  1.5 s bursts of 1 kHz (`speaker-test`) and turning its screen red during each: video 23.634 s, audio 23.616 s (18 ms apart); tone
  starts and red frames 10 to 34 ms apart, ends 38 to 70 ms apart.
- *A Clone's identity (6.3).* A Clone gets a new SMBIOS UUID (`-uuid`) and MAC as before, and `identity.json` with the name inside the
  machine (`<name>-clone` by default, editable in the sheet; `--hostname`). At its first start lord-vm passes it as the credential
  `system.hostname`, and once qemu-guest-agent answers runs one fixed script as root: `/etc/machine-id` (and the D-Bus copy) removed and
  made again by `systemd-machine-id-setup`, which takes the new UUID; the hostname in `/etc/hostname` and `/etc/hosts`; new SSH host keys
  when there were some. Then `identity.json` goes and the window says so. **Keep identity (exact copy)**, off by default, skips it. Without
  an agent the sheet says the copy keeps the machine-id, and its name changes only on systems that take it from the credential (a
  credential can't replace an existing `/etc/hostname` or machine-id). Seen on VM 125: original `testbox`, machine-id
  `6393cf9c…8da0` = product_uuid `6393cf9c-…-8da0`, MAC `…:86:37:84`; its clone `test-box-clone`, `516e4286…1df8` = `516e4286-…-1df8`, MAC
  `…:aa:58:a9`, SSH key fingerprint changed; both running side by side. The script ran 0.9 s after the agent answered.
  **A Windows clone** (the agent says `mswindows`) gets one fixed PowerShell script as SYSTEM instead: `Rename-Computer -NewName
  <name> -Force`, without restarting it; Windows takes the new name at its next restart, which the person chooses (Machines' Shut
  Down and a start is enough). The name follows NetBIOS rules (`vm_winname_from_host`, checked in lord-vm and again in the script:
  1 to 15 letters, digits and `-`, not only digits): the clone's hostname when it fits, else its first 10 characters, `-` and 4 hex
  digits of the new id (`windows-11-pro-prueba-clone` → `windows-11-71fe`). It runs once (`identity.json` goes). The machine SID is
  **not** changed: that's sysprep's job (`sysprep /generalize` before cloning), and two clones on one domain need it; the sheet's
  identity covers the name only. Seen 2026-11-21 on a clone of a Windows 11 Pro machine: the script ran 3 s after the agent
  answered ("computer name WINDOWS-11-71FE from the next restart (now WIN-PRUEBA)"), and after Shut Down and a start `hostname`
  said `windows-11-71fe`; no identity line at that second start.
- *Host shutdown, restart and logout (6.1).* lord-vm holds a logind *delay* inhibitor (`shutdown`) while it runs. PrepareForShutdown, a
  SIGTERM (the session's units stopping) or lord-server going away without Keep Running starts a suspend to disk. The package has
  `/usr/lib/systemd/logind.conf.d/lordos-machines.conf` (`InhibitDelayMaxSec=45`; 30 until 0.8.9.5, raised on 2026-11-23 so a
  guest's ACPI shutdown has time when the machine can't be saved), and the whole stop fits inside that delay, read from logind's
  `InhibitDelayMaxUSec` (45 s when it can't be read, SIGTERM included; `server/src/vm/vmhoststop.h`): the save may run
  until delay − 12 s (+33 s), else it's cancelled and the guest is shut down (ACPI, or the agent first) until delay − 4 s (+41 s),
  then QEMU is told to quit (a clean exit: disks flushed, a vGPU's VFIO device closed), and it's killed at delay − 1 s (+44 s). A
  machine that can't be saved (a vGPU, 3D) has the whole +0 to +41 s for its guest's own shutdown (+26 s with 30). The plan
  scales with whatever logind says (`vm-test` checks 0 to 120 s, and 45 and 30 exactly), so a computer that hasn't reloaded
  logind since the upgrade still gets a plan that fits its 30 s; migration `010-0.8.9.6-machines-shutdown-delay` reloads it.
  The drop-in is in `/usr/lib` (not a conffile): dpkg replaces it, and an administrator's own value in `/etc` wins. The
  delay inhibitor is let go as soon as QEMU is gone, so systemd's last kill never meets a running QEMU. A machine with a vGPU or 3D
  graphics can't be saved and goes straight to the guest's shutdown (a paused one is resumed first: a paused guest ignores the power
  button); a machine started paused that never ran is turned off at once; with less than 3 s of save time left (logind's default 5 s
  delay, our drop-in missing) no save is tried. QEMU is started with SIGTERM, SIGINT and SIGHUP blocked, so stopping the machine's
  scope doesn't kill it first. The system bus is polled after dispatching whatever sd-bus has queued already (`vmlogind.c`): before
  2026-11-23 a message read during the synchronous calls at the start (the bus's NameAcquired) kept POLLIN out of
  `sd_bus_get_events()`, and PrepareForShutdown was handled only when the bus hung up after logind's delay (then 30 s) had run out (seen on the
  maintainer's PC with a Windows 11 vGPU machine: QEMU was then killed by systemd at the very end). The vGPU's mdev is still removed
  the usual way (lord-server's `vm-gvt-remove` once the machine reads stopped, or the admin helper's sweep); lord-vm has no admin
  channel of its own. Seen on VM 125: a machine with text on its screen, the host restarted (`systemctl reboot`):
  *the computer is shutting down: saving the machine to disk*, the SIGTERM 1.1 s later absorbed, 496 MB saved in 2.3 s; after the
  restart the card said Suspended, and Start resumed it in 1.2 s with the text on its screen. The recovery gate sends SIGTERM to lord-vm
  and QEMU at once and checks the machine is saved, then resumed.
- *Idle sleep held off (6.14).* While a machine runs and *Keep this computer awake* is on, lord-vm holds a logind *idle block*
  inhibitor, let go when the machine pauses or stops. The session's idle sleep asks `lord.vm.awake()`, says so in the session log, and
  tries again every 30 s, sleeping once no machine runs; the login screen's sleep deadline (lord-sessiond) reads logind's
  `BlockInhibited`. `systemd-inhibit --list` on VM 125 with two machines: two *LordOS Machines* idle blocks and two shutdown delays;
  none once they stopped. Seen on VM 125 with *Sleep when inactive* at 10 minutes and a machine running: at the deadline the session
  log said *idle: sleep held off: 1 running machine(s) keep this computer awake* and the computer stayed up (12:53 to 13:04, uptime
  rising); 19 s after the machine was shut down: *no running machine holds sleep off any more: going to sleep*, and the journal has
  `PM: suspend entry (s2idle)` at 13:04:47.
- *Install timeout.* Above, *Phase 1's open items*: seen against a dead network on VM 125.
- *Snapshot timeline.* With 24 snapshots it showed one row (the wrap of 7a340336 inside a scroll view was measured as one row, and a
  spacer took half the height): rows are made from the grid's width now and the timeline takes the window's height; seen with 24
  snapshots in rows of five, scrolled to *Now*.
- *Found on the way:* recovery took a QEMU that was still exiting (a zombie whose threads still held the disks' locks, with an empty
  command line) for gone, and finishing an interrupted revert failed; it is recognized by its name now (the recovery gate's revert case
  failed three times in a row before, passes now).

*Release gates after these changes* (build VM, nested amd64): integrity **46 passed**; recovery **32 passed** (22 before, plus the
network and host-stop cases); leaks **10 passed**, 100 cycles in 146 s, 1464 ms each on average, the slowest 5251 ms. On the Pi 5 (arm64, LordOS kernel 6.18.57, KVM, the Debian 13 arm64 nocloud image copied from VM 125
over the LAN): integrity **45 passed** (the one fewer is a linked clone that finished before its kill on amd64, a conditional check),
recovery **32 passed** (passt killed: the guest's next fetch 110 ms after, then 1.1 s and 2.5 s; SIGTERM saved the machine and the next
start resumed it), leaks **10 passed**, 100 cycles in 147 s, 1476 ms each, the slowest 5441 ms.

*On the Pi 5, the final arm64 build.* Built from `beta-deploy` (`pi-machines` with `machines-beta` merged, `next-0884` included: Remote
Desk's `lord-rdesk`, the Fortinet single sign-on and the Sound balance strings checked in the installed package), installed with one
reboot. Driven on the owner's desktop with a virtual pointer and keyboard, its windows left as they were: the library, Start of *Pi
Debian* from its card, the Shell pane on the serial console, Logs (Overview and the machine log, with the logind lines), Suspend (367
MB in 5.9 s) and Resume (state loaded in 217 ms), the Snapshots window and Take Snapshot (with memory, 385 MB, 9 s), Shut Down, the
Clone sheet with the name inside the machine and *Keep identity*, and a Clone (`pi-debian-clone`, a new id and MAC; removed again).
`systemd-inhibit --list` showed the machine's idle block and shutdown delay while it ran.

*Not done or not verified:* snapshot sizes count only the memory, not the disk clusters a snapshot keeps; shared extents and the
reflink path weren't seen (no btrfs or XFS here); a power cut isn't simulated; the agent's own shell (6.18) is the serial console for
now; files by copy and paste or drag (6.21) weren't built then (dropping files in is built since, 6.7.1). The guest's sound on this computer's speakers and its microphone are
built since (6.9).

*Release gates on the Pi 5 with `machines-089`* (2026-11-06; 0.8.8.4 with the beta and the import merged; `lord-vm`
built for arm64 in the arm64 build root and run from a scratch folder, the installed packages untouched, Debian 13's
nocloud arm64 image, KVM, QEMU 10.0.13, LordOS kernel 6.18.57): integrity **45 passed**, 0 failed (296 s); recovery
**41 passed**, 0 failed (301 s; the 32 of the beta plus the 9 of the import case; passt killed: the guest's next fetch
542 ms after, then 1,068 ms and 2,609 ms after two more kills, lord-vm gave up after five in a minute and said so; the
host-stop case saved the machine and the next start resumed it); leaks **10 passed** (152 s), 100 cycles in 151 s,
1,510 ms each on average, the slowest 5,345 ms. Nothing was left running or in `/var/tmp`. Two earlier tries were cut
by reboots of the board for other work (lord-vm's shutdown delay held each one the 5 s logind allowed).

**Phase 3, import of other apps' machines.** Built on branch `machines-import` (2026-11-05), from `machines-beta`: *Migrate from another
app* in the assistant, `lord-vm --import-plan` and `--import-machine`, `lord.vm.importPlan`, `importMachine` and `importInfo`, Configure's
*Switch to Fast Devices*, `machine.json` schema 3, the tool jail for any number of files (`vm_jail_tool_n`), an import case in
`vm_recovery.sh`, and `vm_import_identity.sh`. What it reads, keeps, refuses and why, how the SMBIOS UUID is derived, and what was
measured: 6.22.

**The Linux download cards (0.8.9).** Built on branch `machines-089` (2026-11-06: 0.8.8.4 with `machines-beta` and
`machines-import` merged), from the Assistant mock: the step *Choose* has *Download and install* as a grid of four
cards (a mark, the name and release, the architecture and the size), then *Or bring your own*.

- *The catalog* is a data file, `assets/machines/systems.json`, bundled into the app and installed as
  `/usr/lib/lordos/assets/machines/systems.json`, where the admin helper reads it as root. An entry names a release
  (4.1: no file is pinned): for each architecture the maker's folder (`dir`, `https://` only), its SHA-256 list, fixed
  (`sums`) or found in the folder's listing when its name changes per build (`sumsPattern`, Fedora's `CHECKSUM`), the
  image's name as an anchored extended regular expression (`file`), an optional mirror folder for the image itself
  (`fileDir`), for LordOS's own card only a copy of its signed list on lordos.org read first (`list`, "The LordOS card"
  below), the card's estimate (`approxMb`) and the most a download may be (`maxMb`), plus `hashSource` (where each
  hash comes from) and `checked` (when the entry was last tried). Cards, in this order: **Debian 13** (the netinst ISO,
  `cdimage.debian.org/debian-cd/current`, `SHA256SUMS`), **Ubuntu 26.04** (the desktop ISO, `releases.ubuntu.com` for
  amd64 and `cdimage.ubuntu.com` for arm64, `SHA256SUMS`), **Fedora 45** (Workstation Live, `dl.fedoraproject.org` for
  the list, the mirror redirector for the image, the build's `CHECKSUM`), **Kali 2026** (the installer,
  `cdimage.kali.org/current`, `SHA256SUMS`). Each ships both architectures; the names are only the cards' labels, from
  the data file, never in the catalogs of text (the brand rule).
- *Choosing a card* reads the list (`vm-image-list "<id> <arch>"`, for Fedora first the folder's listing and then
  `vm-image-list "<id> <arch> <list name>"`), and the app (`catalog.js`) takes the newest name that matches, by its
  numbers (26.04.1 over 26.04, 1.10 over 1.9; an architecture's own 64 isn't counted), with its hash and, when the list
  says it, its size. The card then shows the exact version (*Debian 13.7*), and the line under the cards the file and
  where its hash comes from; a list that can't be read says why, with *Try Again*. Nothing connects before a card is
  chosen (8). *Other architecture…* shows the same cards for the other architecture, emulated, as before.
- *Create* (step 5) downloads first: `vm-image-get "<id> <arch> <file> <sha256>"`, with its progress (*412 MB of
  760 MB*), *Checking the download's SHA-256…*, and *Stop Download* (`vm-image-cancel`); then the machine is made from
  the ISO as with *Install from an ISO file*, and its window opens on the installer.
- *What root checks* (`vmimage_admin.c`, `vmcatalog.c`; JS names an entry, never a URL or a path): the id and
  architecture are words of the catalog; every URL is the entry's folder plus a plain file name (letters, digits,
  `._+~-`) that matches the entry's pattern; the name and hash must be exactly one line of the list this helper fetched
  itself (`<hash>  <name>`, `<hash> *<name>` or `SHA256 (<name>) = <hash>`); the image is kept only if the SHA-256 of
  the bytes written is that one, else deleted (`vm.hashMismatch`). Root never parses the HTML or the list beyond that
  line comparison. One download of each image at a time (a lock per image, `images/<sha>.lock`), sizes capped by `maxMb`, lists by 1 MB.
- *Where it lands:* `/var/lib/lordos/machines/lists/<id>-<arch>.list` (the signed text of the maker's list, written only
  after its signature verified) and `/var/lib/lordos/machines/images/<sha256>.iso` with `<sha256>.json` (what it is:
  catalog id, architecture, file name, size), root's and readable by all: one copy for every account, named by its
  content, so an image there is the one that was checked, and an account can't change it under another's machine.
  Removing one is `vm-image-remove`, an administrator's action, since other accounts' machines may use it.
- *Network:* the bytes come through the Store's downloader (`pkg/http.c`, a child as `nobody`, TLS by `lord-tls` with the
  system's certificate authorities), whose child enters `lordos-net-machines.scope` instead of the Store's
  (`fetch_batch_in`): a component of its own, firewall app `lordos:machines`, "Machines" in the Monitor
  (FIREWALL.md, "Machines' downloads"). Since 0.8.8.6 the running machines' own traffic (their scope
  `app-lordos-lordos-vm-<id>.scope`, where QEMU and passt run) is the same app: "Machines · <machine name>" in the
  Monitor, and a rule for Machines applies to the guests too.
- *The makers' signatures (0.8.9, closed 2026-11-06).* Before any hash in a list is trusted, the helper checks the
  list's OpenPGP signature with the maker's key pinned in the package. All four sign with RSA 4096 (v4 signatures):
  Debian with SHA-256 and Ubuntu with SHA-512 over a detached signature (`SHA256SUMS.sign`, `SHA256SUMS.gpg`), Kali
  the same (`SHA256SUMS.gpg`, SHA-512), Fedora a clear-signed `CHECKSUM` (SHA-256, text signature), so `pkg/pgp.c`
  needed no new algorithm, only detached signatures (`pgp_verify_detached`, binary 0x00 and text 0x01) and
  `pgp_keyring_pin` (only the primary keys whose fingerprints the catalog lists, and their subkeys, may sign); the
  clear-signed path now shares that verifier with InRelease (a real Debian InRelease still verifies, and one changed
  byte fails). The keys, fetched once from each project's official source and stripped to the key and its
  self-signatures (`gpg --export-options export-minimal`), are `packaging/keys/machines/*.asc`, installed in
  `/usr/lib/lordos/assets/machines/keys`; each fingerprint was compared, over https, with the one the project
  publishes, and `systems.json` says where for each (`keySource`, `fingerprintChecked`):

  | Card | Key | Fingerprint | Fetched from | Fingerprint compared with |
  |---|---|---|---|---|
  | Debian | Debian CD signing key (2011) | `DF9B 9C49 EAA9 2984 3258 9D76 DA87 E80D 6294 BE9B` | keyring.debian.org | https://www.debian.org/CD/verify |
  | Ubuntu | CD Image Automatic Signing Key (2012) | `8439 38DF 228D 22F7 B374 2BC0 D94A A3F0 EFE2 1092` | keyserver.ubuntu.com | https://ubuntu.com/tutorials/how-to-verify-ubuntu |
  | Fedora | Fedora 45 | `4F50 A611 4CD5 C697 6A7F 1179 655A 4B02 F577 861E` | https://fedoraproject.org/fedora.gpg | https://fedoraproject.org/security/ |
  | Kali | Archive Automatic Signing Key (2025), expires 2028-04-17 | `827C 8569 F251 8CC6 77FE CA1A ED65 462E C8D5 E4C5` | https://archive.kali.org/archive-key.asc | https://www.kali.org/docs/introduction/download-official-kali-linux-images/ |

  A catalog entry without `signature`, `keys` or a well-formed fingerprint is refused like any malformed entry. A list
  that isn't signed (no signature file, a 404; a Fedora list that isn't clear-signed) is refused as `vm.listUnsigned`;
  a signature that doesn't verify with the pinned keys (a changed list, another list's signature, another key, an
  expired or revoked key, a signature from the future, a key file that lacks a pinned key) as `vm.listBadSignature`
  with pgp.c's reason. Only the signed text is kept (a clear-signed list without its armour), and the old unchecked
  `<id>-<arch>.sums` of the first build is deleted. The folder listing Fedora's list is found in isn't signed and isn't
  trusted for anything but a name that must match the entry's pattern; that list is then checked like the others.
  Not covered: an older list, validly signed, served again (a mirror that rolls back would offer an older image of the
  same release, still verified against its maker's signature). When a maker rotates its key (Fedora per release,
  Kali's 2028 expiry), the catalog and its key file change with the release that adds the new card.
- *Resumed downloads.* `pkg/http.c` grew `FetchItem.resume`: a cut download keeps `dest.part` (root's, next to its
  final name, `<sha256>.iso.part`), and the next try asks for the rest with `Range: bytes=<size>-`. Only a 206 whose
  `Content-Range` starts exactly there and whose length matches is appended (the hash seeded by reading the part
  first, so the SHA-256 is over the whole file); a 200 (a server that ignores Range) starts again from the beginning,
  a wrong 206 is dropped on a new connection and asked again without a range, a 416 too. A part that isn't a plain
  file of root's, a link, or as big as the most the entry may be is never continued. A changed part is caught by the
  hash at the end and deleted with the download (`vm.hashMismatch`). The Store's own downloads don't use it.
  *Stop Download* keeps the part: the assistant says *Download stopped* with **Continue Download**, and the helper
  says first how much is continued (`vm.downloadResumed`); Storage shows the part and removes it.
- *Removing a downloaded image* (6.16): the Storage window lists them under the machines (the mock's *Downloads*,
  also a category of its bar, an image in use counted once): the system and version from the catalog and the
  image's `.json`, the file, its size, which machines of this account have it in their CD drive, and **Remove,
  frees …** with a confirmation that says which machines start with an empty drive and that other accounts' machines
  lose it too; disabled while one of those machines runs. The helper refuses too when any process holds the image
  open (`/proc/*/fd`: QEMU of any account's running machine, `vm.imageInUse`) or it is downloading, and deletes its
  part and `.json` with it; the app then ejects it from this account's machines (`lord.vm.eject`), so they still start.
- *Install without questions (0.8.9, the mock "Unattended").* Step 3 becomes *Set up* for a card whose catalog entry
  has an `unattended` block (LordOS, Debian, Ubuntu, Fedora, Kali): a switch, on by default as in the mock, *Account
  name* (the person's own login name), *Password*, and *Computer name*, *Time zone* and *Keyboard* filled from this
  computer (`lord.vm.hostDefaults()`: `/etc/localtime`, `/etc/default/keyboard`, `LANG`). The Review says *Without
  questions: account … on …*. What happens:
  - *The answer file* is the system's own installer's: a Debian **preseed** (Kali's is the same, kind `kali`, with
    Kali's archive `http.kali.org/kali`, no Debian `security, updates` services and Kali's own tasks), an Ubuntu
    **autoinstall** (cloud-init NoCloud `user-data` and `meta-data`) or a Fedora **kickstart**, made by `lord-vm --create … --unattended SPEC`
    (`vm/vmunattend.c`). lord-server writes SPEC (0600, in the runtime folder; lord-vm reads and deletes it at once)
    from the assistant's words, with the password replaced by its SHA-512 crypt hash (`crypt_r`, a random salt): the
    text the person typed never reaches a file, and the answer files carry `passwd/user-password-crypted`,
    `identity.password` or `user --iscrypted`. Every field is checked again in lord-vm (account names, no quotes or
    control characters, a crypt hash, an RFC 1123 host name, the ISO paths and a command line without shell
    characters); the root account is locked, the person's account is the administrator (sudo, wheel).
  - *What is installed:* the system's own desktop (Debian's GNOME task, Ubuntu's `ubuntu-desktop-minimal`, Fedora
    Workstation with `xconfig --startxonboot`, Kali's Xfce and default tools: tasks `desktop, desktop-xfce, meta-default,
    standard`), plus `qemu-guest-agent`, and the agent is turned on for the machine. Without the desktop (`desktop:
    false` in the spec, not offered by the assistant): the standard system (Debian, Kali, with `acpid`), Ubuntu's server
    or Fedora's `custom-environment` (Fedora 45 has no `minimal-environment` any more). Ubuntu installs from its server
    image with the desktop's packages added, and Fedora from its network installer (both hidden catalog entries,
    `ubuntu-server` and `fedora-netinst`, checked against the same signed lists as the cards): the desktop images run
    a live session that needs 4 GB and doesn't take an answer file in the same way, and Fedora's live image doesn't
    install from a kickstart.
  - *How it reaches the installer, with no network server and no boot menu to type into:* the first start boots the
    installer's own kernel and initrd, read from the ISO (`vmu_iso_find`: ISO 9660 with Rock Ridge, Joliet or plain
    names, every record bounded), with the entry's command line (`-kernel`, `-initrd`, `-append`; `{label}` becomes the
    ISO's volume label, which Fedora's `inst.stage2` needs; on arm64 each Linux card has its own line with `console=tty0
    acpi=nospcr`: there the firmware's ACPI serial port table, SPCR, makes the serial port the kernel's console even
    with `console=tty0`, and Debian's and Kali's installers ran only on the serial port, the window black). Debian's
    and Kali's preseed goes **inside the initrd**, as a second
    cpio archive after the maker's (the kernel unpacks both and the installer reads `/preseed.cfg` before its first
    question); Ubuntu's and Fedora's go on a **seed disk**, a 2 MiB FAT image made in C (`vmu_fat_image`, long names
    so `user-data` and `ks.cfg` read as such) labelled `CIDATA` (cloud-init) or `OEMDRV` (anaconda), on a read-only
    virtio disk in slot 0xa. The ISO stays in the CD drive, where each installer finds its packages. These files are
    `<machine>/disks/unattended/{vmlinuz,initrd,cmdline,seed.img}`, 0600.
  - *The end:* QEMU runs with `-action reboot=shutdown`, so the installer's last restart stops it (`SHUTDOWN`,
    `guest-reset`); lord-vm then overwrites and deletes the seed disk, deletes the folder, takes the ISO out of
    `machine.json`, and stops with the new reason `installed` (`VR_INSTALLED`, added at the end of `vmproto.h`). The
    machine's window starts it again at once: the installed system. QEMU gives a `-kernel` boot index 0, so during
    that first start the system disk is 1 and the CD 2.
- *Install without questions, tested (2026-11-06 and 07).* `tests/vmunattend_test.c` **82** (the spec and the ones it
  refuses, the three answer files with the hash and never a plain password, the keyboard's codes and description, the
  command line and `{label}`, the FAT seed disk read back by long name and passed by `fsck.fat -n`, the cpio, ISO 9660
  images made in the test with Rock Ridge, plain and Joliet names, broken records, an oversized directory, a file past
  the end, and `vmu_prepare` end to end), `vmcatalog-test` 226, `vm_catalog_test.mjs` 61, `vm_model_test.mjs` 153.
  The kernel paths were checked in the real images: Debian `install.amd/vmlinuz`, `initrd.gz`; Ubuntu 26.04.1 server
  `casper/vmlinuz`, `casper/initrd` (its GRUB entry: `linux /casper/vmlinuz ---`); Fedora 45 netinst
  `images/pxeboot/vmlinuz`, `initrd.img`, volume label `Fedora-E-dvd-x86_64-45` (its GRUB entry has the same
  `inst.stage2=hd:LABEL=…`). On VM 116 (now 12 GB and a 2 GB swap file), in a real session, screenshots
  `out/final/machines-e2e/u-*.png`, one guest at a time:
  - **Debian 13.7 desktop, nothing typed:** the card, *Set up* (`u-02`), Review (`u-03`), the download, stopped and
    continued (`08`, `09`), then the installer from language to GNOME with no question (`u-05`), the installer's
    restart ended QEMU (*the install is done*), lord-vm emptied `disks/unattended` and took the ISO out, the window
    started the machine again: GRUB from the disk and GDM with the account `tester` (`u-07`), and the login with
    the password typed in the assistant (`u-08`, GNOME's welcome). Install 28 minutes with GNOME (2 vCPU, 3 GB).
    Two fixes on the way: (1) the first run stopped once on *keyboard-configuration* (the installed system's package
    asks for the layout again when only `xkb-keymap` is preseeded): the preseed now also carries `layoutcode`,
    `variantcode`, `modelcode` and the layout's xkb description from this computer's rules
    (`/usr/share/X11/xkb/rules/base.lst`: *English (US)*), and the next run had no question; (2) lord-vm's own
    Landlock lets it remove files in the machine's folder, not folders: the empty `unattended` folder stays (a
    start only looks for `unattended/cmdline`). Also seen: the first start's picture said *Display output is not
    active* while the installer ran, until the window was given a size (1280 × 800); a start after that showed it
    at once. Not changed here (the display follows the window's size on the second start), noted below.
  - **Shut Down of the installed Debian desktop:** through qemu-guest-agent (*asking qemu-guest-agent to shut the guest
    down*), stopped in 3.0 and 3.5 s (two runs), reason `host-shutdown`.
  - **Ubuntu 26.04.1:** the card resolved its server image from the same signed list, made the CIDATA seed disk and
    `autoinstall ---`, and subiquity ran the whole autoinstall with no question (`u-13`: network, storage, curtin,
    `qemu-guest-agent`) up to *installing ubuntu-desktop-minimal*, where the build VM's disk filled (QEMU paused the
    guest on `BLOCK_IO_ERROR`, `u-14`; 43 GB of other work in `/var/tmp`, not this work's to delete). Then again
    without the desktop (`desktop: false`, 2 vCPU, 3 GB, 20 GB): the same autoinstall with no question through curtin,
    GRUB, *configuring cloud-init* and into *run_unattended_upgrades* (security updates from the archive), its disk at
    6.3 GB, when the build VM had under 500 MB left: the guest was paused and removed so the shared host wouldn't fill.
    With 22 GB free again, the desktop run went on for 22 minutes and then the guest sat with both vCPUs busy and no
    disk or network traffic, its picture blank. Its disk, read afterwards (read-only, `qemu-nbd`), told why: dpkg had
    failed at 10:49 inside `ubuntu-desktop-minimal` on the **firefox** package (`/var/crash/firefox.0.crash`: *new
    firefox package preinst maintainer script subprocess failed with exit status 1*): Ubuntu's firefox deb only
    installs the snap, which can't happen in the installer's target where snapd isn't running, and subiquity then
    waits on its error screen. The autoinstall now pins firefox out (`apt.preferences`, priority -1) when it adds the
    desktop (a browser can come from the Store afterwards); `vmunattend-test` 83. With that, the card's run (2 vCPU, 3 GB, 24 GB; the image
    already stored) installed with nothing typed in **23 minutes** (08:02:39 to 08:25:58), the installer's restart
    ended the first start, lord-vm removed the seed disk and the ISO, and the machine started again into the installed
    system: GDM with the account `tester` (`u-14`), and the password typed in the assistant logged in to the Ubuntu
    desktop (`u-15`). Shut Down through the guest agent: off in 5.5 s. Its disk took 8.9 GB.
  - **Not changed, noted:** the first start's picture can stay on *Display output is not active* while the installer
    runs at the firmware's resolution, until the window asks for a size (seen once with Debian; a start with the
    window already sized showed the installer at once).
  - **Fedora 45:** the card resolved the netinst image (`Fedora-Everything-netinst-x86_64-45-1.4.iso`, 1.0 GB,
    clear-signed list), the OEMDRV seed disk and `inst.stage2=hd:LABEL=Fedora-E-dvd-x86_64-45
    inst.ks=hd:LABEL=OEMDRV:/ks.cfg`; anaconda read the kickstart and went straight to *Installation* with every spoke
    set (language, time zone *America/Argentina/Buenos_Aires*, the metalink source, *Fedora Workstation*, automatic
    partitioning, network) and installed: disk label, ext4 `/boot`, btrfs, EFI, *Installing the software, Downloading
    packages* (`u-16`). Stopped there, as asked (the disk).
- *The cards run end to end (2026-11-24, `m100/cards-runs`).* Every card with an unattended install was run from its
  card through the installed system's login, on the maintainer's PC (amd64, 20 threads, KVM, Debian 13's QEMU
  10.0.13) and on the Raspberry Pi 5 (16 GB, LordOS 0.8.9 arm64, KVM, the same QEMU). The path: the card's image
  resolved and downloaded by `apps/machines/catalog.js` (`unattendedOf`, `listingNames`, `newest`, `pickImage`) with
  the admin actions `vm-image-list` and `vm-image-get` run as root by `tests/vm_image_cli.c` (the same
  `vmimage_admin.c`, a scratch store); on the PC the machine made by `lord.vm.create` in a headless lord-server of this
  branch (the assistant's request: `createRequest` and the unattended object of `assistant.jsx`; the password hashed
  by `js_vm.c`), on the Pi by `lord-vm --create … --unattended SPEC` with the spec `js_vm.c` writes (no session there);
  then `lord-vm --start`, both in a private user and mount namespace with a scratch home (never the person's own),
  pictures by QMP `screendump` (`--dev-qmp`), keys by QMP `send-key`. Account `tester`, keyboard and time zone this
  computer's (*latam*, *America/Argentina/Buenos_Aires*) on the PC, US and UTC on the Pi. Nothing was typed in any
  installer. Pictures and logs: `/var/tmp/lordos-cards-runs/` on the maintainer's PC (not in the repository).
  - **Downloads, verified:** PC, in parallel: Fedora 45 netinst 1,261,156,352 bytes in 142 s, Kali 2026.3 installer
    5,047,031,808 in 89 s, LordOS 0.8.9 live 1,189,533,696 in 92 s (its signed list from the copy on lordos.org, the
    first source, which answered), Ubuntu 26.04.1 server 2,927,861,760 in 148 s, Kali arm64 4,051,513,344 in 54 s. Pi
    5: Debian 13.8 arm64 netinst (737,456,128) in 14 s, Fedora 45 arm64 netinst (1,426,759,680) in 35 s, LordOS 0.8.9
    arm64 (1,113,196,544) in 14 s, Kali arm64 in 59 s; Remove (`vm-image-remove`) deleted two images there and refused
    the one a running installer held (`vm.imageInUse`).
  - **LordOS 0.8.9 (PC):** 4 vCPU, 4 GB, 32 GB, 3D (the card's size). `lordos-auto.conf` on the LORDOSAUTO seed disk,
    `lord-autoinstall` erased only `vda` and installed in **1 min 19 s**; the installer's restart ended the first start
    (`installed`), the seed disk and ISO were gone; the next start ran with 3D (virgl) and shut down cleanly; started
    in 2D for the picture: the greeter with `tester` (`lordos-02-installed`), the assistant's password logged in to the
    desktop's first-run chooser (`lordos-03-desktop`).
  - **LordOS 0.8.9 arm64 (Pi):** installed in **2 min 2 s** (`pi-lordos-01-installer`, the installer on screen), the
    greeter (`pi-lordos-02-greeter`), logged in to the desktop (`pi-lordos-03-desktop`; the installed 0.8.9 then
    offered the Store's update to 0.8.9.5).
  - **Ubuntu 26.04.1 without the desktop (PC):** 2 vCPU, 3 GB, 20 GB; autoinstall in **10 min 40 s**, then the login
    prompt, `tester` logged in, `qemu-guest-agent` active (`ubuntu-console-04-logged-in`).
  - **Fedora 45 Workstation (PC):** 4 vCPU, 4 GB, 32 GB; the first run installed in 18 min 9 s, and the session's open
    machine window started it again by itself, but it booted to a **text login** (`multi-user.target`, GDM
    inactive; `fedora-05-console-login`): anaconda in text mode leaves the console as the default unless the kickstart
    says `xconfig --startxonboot`, which it now does. The run after the fix: **21 min 47 s**, GDM with `tester`
    (`fedora-06-gdm`), logged in to GNOME's welcome (`fedora-07-desktop`); Shut Down through the agent.
  - **Fedora 45 arm64 without the desktop (Pi):** 3 vCPU, 3 GB, 14 GB; the first run stopped at anaconda's menu, *No
    match for argument: minimal-environment* (Fedora 45's comps has no such environment any more; the console system is
    now `custom-environment`); the run after the fix installed in **17 min 32 s** and logged in on the console,
    `qemu-guest-agent` active, `aarch64` (`pi-fedora-05-logged-in`).
  - **Debian 13.8 arm64 without the desktop (Pi):** 3 vCPU, 3 GB, 12 GB; installed in **7 min 53 s**, the login on
    `tty1`, `qemu-guest-agent` active (`pi-debian-03-logged-in`), Shut Down by the guest. While it installed, the
    machine's picture was black: the installer ran only on the serial port (the firmware's ACPI SPCR table makes it
    the kernel's console). Each Linux card's arm64 entry now boots its installer with `console=tty0 acpi=nospcr`.
  - **Kali 2026.3 (PC):** 4 vCPU, 4 GB, 40 GB; the new `kali` preseed with Kali's own disc profiles installed Xfce and
    the default tools in **36 min 27 s** with no question, LightDM with the keyboard *latam* (`kali-06-login`), logged
    in to the Xfce desktop (`kali-07-desktop`); Shut Down through the agent in 1.3 s; its disk took 19 GB.
  - **Kali 2026.3 arm64 without the desktop (Pi):** 3 vCPU, 3 GB, 14 GB; its first run showed the same black picture
    (the installer on the serial port only); with the arm64 line (`console=tty0 acpi=nospcr`) the installer was on
    the screen (`pi-kali-01-installer`), installed in **11 min 5 s**, the login on `tty1`, `qemu-guest-agent` active,
    `aarch64` (`pi-kali-03-logged-in`). Debian arm64 was then installed again with its new line: the installer on the
    screen too (`pi-debian-04-installer-on-screen`).
  - **Not run, for space:** the Pi 5's SD card had 5.6 GB free and its `/tmp` (tmpfs) 8 GB: the desktops on arm64
    (GNOME, Workstation, Ubuntu's, Xfce with the tools: 9 to 19 GB each) and Ubuntu 26.04.1 arm64 at all (its server
    image is 3 GB and its console install took 6.3 GB on amd64). Their path is the same code as the arm64 console runs
    above plus the amd64 desktop runs.
  - **Seen, not changed here:** in the Fedora Workstation guest the agent couldn't mount the transfer folder
    (`mkdir: cannot create directory '/mnt/LordOS': Permission denied`): Fedora confines `qemu-ga` with SELinux
    (`virt_qemu_ga_t`), most likely why its `guest-exec` can't make the folder (not looked into; 6.7.1).
- *The power button (6.1), confirmed (2026-11-07).* In a console-only Debian installed by the card's preseed
  (`desktop: false`: the *standard* task, `qemu-guest-agent`, `acpid`): the guest has the ACPI *Power Button* input
  device (`/proc/bus/input/devices`: *Power Button*, `kbd event3`), `dbus` installed, and `systemd-logind`, `acpid` and
  `qemu-guest-agent` active (`u-09`). With acpid and the agent stopped and disabled (`u-10`), Shut Down pressed only
  the button and the guest was off in **0.8 s**; again from its login prompt, through the agent, in 1.2 s. So the
  button reaches the guest through q35's ACPI, and systemd-logind turns it into a shutdown (`HandlePowerKey=poweroff`,
  its default) whenever it runs. The guest that ignored it for 60 s was installed with no task at all: `dbus` is
  *Priority: standard* in Debian 13, and nothing in such an install pulls it, so there is no system bus,
  systemd-logind doesn't run, and no acpid either; nobody hears the button. What changed: Shut Down asks
  qemu-guest-agent first when it answers and presses the button 10 s later if the guest still runs (without the
  agent, the button first); the card's installs bring the standard task or a desktop (`dbus`, logind),
  `qemu-guest-agent` with the machine's agent on, and, on a console Debian, `acpid` and `acpi-support-base`.
- *Deviation from the mock:* the mock's first card is LordOS; until 0.8.9 LordOS published no ISO as a release file,
  so its card was left out and the fourth card is Kali (in 4.1's list). The LordOS card came after (below). The mock's
  step names stay (*Install* is shown as *Create*, as in the alpha).
- *The LordOS card (0.8.9.5, 2026-11-22; maintainer's decision the same day).* The first card is **LordOS**,
  with LordOS's own icon (`LordOSLogo`: LordOS is ours, no trademark question), for amd64 and arm64: the **live ISO**
  (`lordos-live-<version>-<arch>.iso`, about 1.1 GB), not the OVA or the Pi image.
  - *Which release:* the entry's folder is a GitHub release folder with a **pinned tag**
    (`https://github.com/lordbasex/LordOS/releases/download/v0.8.9-beta/`), and `release` says its version
    (*LordOS 0.8.9*). No GitHub API call: the downloader keeps to the catalog's folder (the image's download redirects
    to GitHub's signed asset URL, as for the Imager), and the API's answers would be one more thing to parse and
    rate-limit. The release process moves the tag (`RELEASING.md`, step 1) whenever a release ships new ISOs; a release
    with a fourth number and no ISO keeps the card where it is. `vm_catalog_test.mjs` fails when the site's current ISO
    (`web/content/downloads.json`) is newer than the card's tag, so a forgotten move shows up in `make test`. Pre-releases
    (`-beta` tags) are what exists today; the card says the version only. An installed LordOS's card names the release
    its own package was built with (or a newer one after an update).
  - *The signature:* LordOS's own releases are signed with the **LordOS release key**: `SHA256SUMS.sig` is base64 of the
    Ed25519 signature over `SHA256SUMS` (`SIGNING.md`, "Release files"). The entry's `signature` is
    `{ "type": "lordos-release", "name": "SHA256SUMS.sig" }`, with no `keys` or `fingerprints` (a catalog entry of that
    kind that names them is refused). `vmcatalog.c` checks it with `img_sig_verify`, the LordOS Imager's own check of
    official images (`imager/imgverify.c`), and the key the helper passes is `codesig_release_key`, the release key
    compiled into lord-server: no new crypto, no new key, no key file on disk. A missing `SHA256SUMS.sig` is
    `vm.listUnsigned`; one that doesn't verify (a changed list, a line added, another key, an OpenPGP signature in its
    place, another list) is `vm.listBadSignature` with `imager.badSignature`. Then the image is checked against that
    signed list like any card's. `maxMb` is 4096 for both.
  - *Where the list comes from (in development after 0.8.9.5-beta; maintainer's decision 2026-11-23):* GitHub's release
    downloads sometimes answer 403 on their signed redirect, so the signed list is read from **lordos.org** first and
    from the GitHub release only if that fails; the ISO itself still comes from GitHub. The entry's `list`,
    `{ "dir": "https://lordos.org/releases/v0.8.9-beta/", "sums": "SHA256SUMS.txt", "signature": "SHA256SUMS.sig" }`,
    names the copy: the same bytes as the release's `SHA256SUMS` and `SHA256SUMS.sig`, the list with an extension because
    CloudFront serves an extensionless path as a pretty-URL page. `vmcatalog.c` takes a `list` only on an entry signed
    with `lordos-release` and a fixed `sums`, only under `https://lordos.org/releases/<something>/`, with plain names for
    both files; anything else refuses the whole entry. `vmcat_list_source` gives the order (the copy, then `dir`'s
    `SHA256SUMS` and `SHA256SUMS.sig`), and `vm-image-list` keeps the first list whose signature verifies; when both
    fail, the answer is GitHub's error. Both are checked with the same compiled-in release key, so where the list comes
    from changes nothing about what is trusted: a wrong or old file on the site fails its signature and the GitHub list
    is read instead. The release process uploads the copy (`tools/release/publish-lists.sh`, `RELEASING.md` step 9) and
    moves `list.dir` with the tag; `tools/web/deploy.sh` never deletes `releases/`.
  - *Size:* a card may carry `defaults`, the least a new machine of it gets over the chosen profile (`cardSize` in
    `catalog.js`; processors never more than this computer has): LordOS asks for **4 vCPU, 4 GB, 32 GB and 3D
    graphics** (3D where this computer can give it, 6.10).
  - *Install without questions:* `unattended` kind **`lordos`**, the live ISO's own unattended install
    (`UNATTENDED.md`, in the live ISO since 0.8.8.1): the live kernel and initrd (`live/vmlinuz`, `live/initrd.img`, the same
    paths on both architectures) with `boot=live components loglevel=3 nosplash lordos.auto=1 console=tty1`, and a FAT
    seed disk labelled **`LORDOSAUTO`** with `lordos-auto.conf` (`vmu_answer`): the account, the SHA-512 hash of the
    password, the computer's name, time zone, keyboard, locale, LordOS's interface language (this LordOS's own), `mode =
    erase`, `erase = yes`, `autologin = no`, `after = reboot`, and the disk: lord-vm fills `disk` (`vda`, or `sda`
    with the compatible devices) and `disk_bytes`, the exact size of the system disk it just made, so
    `lord-autoinstall` erases only that disk and stops, touching nothing, at anything else (the seed disk, the ISO, a
    different size). A spec of kind `lordos` without `lordos.auto=1` on its command line is refused. Its restart at the
    end ends QEMU as with the other installers, and the next start is the installed LordOS. No ISO-side change was
    needed: the live ISO already reads `lordos-auto.conf` from a `LORDOSAUTO` file system of any kind. An older live ISO
    without `lordos-autoinstall.service` (before 0.8.8.1) starts its live session instead and nothing is erased; the
    card only names 0.8.9 or newer.
  - *The guest agent from the first boot:* the `lordos` package ships `lordos-guest.service`, a user unit on by
    default (`/usr/lib/systemd/user/default.target.wants`) that runs `/usr/lib/lordos/guest/lordos-guest` only where
    `/dev/virtio-ports/org.lordos.agent` exists, which is only inside a Machines guest whose agent is on, and
    `71-lordos-guest.rules` (the same rule the Install Agent writes in other guests: the seat's person may open the
    port). On a computer, or a machine without the agent, nothing runs. The live ISO now also has `qemu-guest-agent`
    (`ISO.md`): Debian's unit starts only when the host gives the `org.qemu.guest_agent.0` port. So a LordOS machine
    made by the card has its agent connected and apps mode (7.2) offered from its first boot; both reach installed
    systems with the next ISO and the next `lordos` package.
  - *Checked:* `vmcatalog-test` with v0.8.9-beta's published `SHA256SUMS` and `SHA256SUMS.sig` (`tests/vmimage/lordos.*`)
    and the compiled-in key: the list verifies; a changed hash, a line added, another key, a 31-byte or PEM key, no
    signature, an empty, garbage or OpenPGP signature, the signature over Debian's list, and Debian's entry given the
    release key are all refused; entries of that kind with a key file, fingerprints, no name or a path for a name are
    refused. The list's copy: the real entry's `list` has the tag of its `dir`, lordos.org is tried first, GitHub second
    and nothing third; a `list` on another host, outside `releases/`, over http, with `..`, a query, a name with a path,
    a missing name, on a maker's entry or on a `sumsPattern` entry is refused; other entries have only their own folder
    (`vmcatalog-test`, `vm_catalog_test.mjs`). `vmunattend-test`: the `lordos-auto.conf` keys and values, no answers until lord-vm names the disk and its
    size, refused languages and command lines, and `vmu_prepare` end to end (the `LORDOSAUTO` FAT image read back, the
    file on it). `vm_catalog_test.mjs`: the card per architecture, the live ISO picked from the real list (never the
    OVA or a `.sources.tsv`), the tag against `downloads.json`, `unattendedOf`, `cardSize`. **Not run yet from the card:** the whole
    path on a machine (download 1.1 GB, the unattended install to the installed desktop, the agent and apps mode on
    first boot) needs the next ISO (with `qemu-guest-agent`). A LordOS 0.8.9 install in a 3D machine did run on the
    maintainer's PC (RX 580); it hit QEMU's 3D abort at the installer's restart, so an unattended start now always runs
    with 2D graphics and 3D comes back for the installed system (`vm_gl_refusal`, `lordvm.c`; 6.10).
- *Tests:* `tests/vmcatalog_test.c` (ASan, UBSan; the real catalog for both architectures, names against patterns and
  what must never pass: paths, `.torrent`, the other architecture, Debian's *edu* and *mac* images; both list forms,
  capitals, CRLF, a NUL, odd spacing; fourteen malformed catalogs refused) and `tests/vm_catalog_test.mjs` (the cards,
  the makers' lists as published on 2026-11-06, a folder listing, newest by number, the versions on the cards, progress
  lines). `build/vm-image-cli` (`tests/vm_image_cli.c`) runs the actions like the helper against the real servers.
  On the amd64 build VM the branch builds with `make` in 72 s with no warning, and `make test` passes in 635 s
  (`vmcatalog-test` 107, `vm-test` 317, `vmimport_test` 141, `vm-import` 44, `netconn-test` 36, `fw-rules-test` 16,172;
  `run_suites.sh` 29 passed, 27 skipped for want of node or devices); `vm_catalog_test.mjs` 25 and `vm_model_test.mjs`
  152 pass on node.
  *Measured* on the amd64 build VM (2026-11-06, `vm-image-cli` as root, the real servers): all eight lists read
  (Debian 302 and 98 bytes, Ubuntu 588 and 2,691, Kali 2,840 each, Fedora's two listings and then its `CHECKSUM`); the
  newest names taken by `catalog.js` from those very files were *debian-13.7.0* (both), *ubuntu-26.04.1-desktop* (both),
  *Fedora-Workstation-Live-45-1.4* (both) and *kali-linux-2026.3-installer* (both). Debian 13.7's arm64 netinst
  (735,358,976 bytes, through Debian's redirect to a mirror) downloaded in 99 s with 375 progress lines whose total came
  from the server, and was kept as `6e93fa17…024b.iso` (`sha256sum` agrees); asking again answered in 21 ms from the
  store; the same file asked for with a hash the list didn't have was refused before any byte (`vm.notListed`), and with
  a wrong hash added to the list it was downloaded and thrown away (`vm.hashMismatch`, nothing left, no `.part`); a name
  from the other architecture, a path (`../../etc/passwd`) and the image's name in place of the list's were refused
  (`admin.invalidArgument`). `lord-vm --create --iso` with that root-owned image made an arm64 machine (emulated on that
  host) whose serial console showed GRUB 12 s after the start.

  *Signatures, resume and removal, tested (2026-11-06).* `tests/vmcatalog_test.c` now **223** checks (the real catalog,
  plus every list and signature as published that day for both architectures with the pinned keys, and for each: a
  changed hash, a line added, text after a clear signature, no signature, an empty or garbage signature, another
  maker's pinned key, a key file without the pinned key, a key that isn't pinned, a signature from the future, Kali's
  key after its end, another list's signature; nine malformed signature entries). `tests/http_resume_test.c` **14**
  (a server on loopback: whole, cut and resumed, cut twice, Range ignored, a wrong range, 416, a changed part, no
  resume, an oversized part, a symbolic link in place of the part), both under ASan and UBSan in `make test`.
  `vm_catalog_test.mjs` **43**, `vm_model_test.mjs` 152. On the amd64 build VM (VM 116) `make` builds with no warning
  in 46 s (`-j6`) and `make test` passes in 325 s (`vm-test` 317, `vmimport_test` 141, `vm-import` 44, `pkg-test` and
  the suites: 29 passed, 27 skipped). Live, through `vm-image-cli` against the makers' servers: Debian's list with
  Kali's key pinned in its place was refused (`vm.listBadSignature`, `pkg.pgp.unknownKeys`), with a signature name
  that isn't published (`vm.listUnsigned`).

  *The cards driven on screen (2026-11-06).* A real headless LordOS session built from this branch on VM 116
  (`tools/vm/e2e`, the Cloud bench's approach: a test-only theme bundle with a driver, input and pictures over the
  session's `remote.sock`; lord-server started as root with `--user` so the session has its administration helper),
  screenshots in `out/final/machines-e2e/`: the Debian 13 card read Debian's list and checked its signature in 2.3 s
  (*Debian 13.7, … from its maker's signed list*); Create and Start showed the progress (*34 MB of 756 MB*); Stop
  Download at 213 MB kept the part; after the session was restarted, Create again said *Continued from the 213 MB
  downloaded before* and went on (a Range request through Debian's redirect to a mirror); stopped again (347,735,755 bytes),
  shown in Storage as *Debian 13.7, amd64 (stopped)*; Continue Download finished it, and the file's SHA-256 was the
  listed one (`a7ef94ac…e355`, 792,723,456 bytes, in three pieces). The machine (4 vCPU, 1.3 GB, 32 GB) opened on the
  installer's boot menu; with the answers of `tools/vm/e2e/preseed-debian.cfg` added to the `linux` line, Debian
  installed itself to the machine's disk (EFI, ext4 root and swap on `/dev/vda`) in about 6 minutes, rebooted into
  the installed system (GRUB from the disk), and `tester` logged in on its console: Debian 13.7, kernel
  6.12.107+deb13-amd64, booted UEFI. Storage then showed the image *In the CD drive of Debian 13.7* with Remove
  disabled while it ran (the helper also answered `vm.imageInUse`); once off, Remove asked, deleted the image, its
  `.json` and its lock, and emptied the machine's drive, which then started from its disk to the login prompt. The
  Fedora 45 card: its list found in the folder's listing and checked (clear-signed), 2,925,092,864 bytes downloaded
  in 81 s and kept, the machine created (*Create and Configure*). All eight lists (four systems, both architectures)
  were read and their signatures checked through the session's helper (1.4 to 2.5 s each); the arm64 cards show
  their versions (*Fedora 45, arm64: Fedora-Workstation-Live-45-1.4.aarch64.iso*).
  *Seen on the way, not changed here:* the installed console Debian (no desktop) didn't act on the ACPI power button
  within 60 s, so Shut Down turned it off (*It didn't shut down in time*); and inside a guest with the Shared network
  the host itself isn't reachable (the guest has the host's address and `--no-map-gw`), so the bench serves the answers
  file from another computer.

  *Was open for the beta, closed since:* the cards' unattended install (4.1: Debian preseed, Ubuntu autoinstall, Fedora
  kickstart, LordOS `lordos-auto.conf`) and the LordOS card are built. Still open: the signatures and resume on the
  Pi 5 (the code paths are the same).


## 4. Guest systems

### 4.1 Linux

Every current Linux has virtio drivers in its kernel. Linux cards: LordOS, Debian, Ubuntu, Fedora and Kali, each for the host's architecture, plus **any ISO of that architecture** from a file and a person's own systems (5.6.1). On amd64 the New Machine assistant also shows the Windows card (4.3.1): six cards, in that order (`assets/machines/systems.json`, `catalog.js`); arm64 shows five. arm64 images exist for all the Linux ones; on a Pi the UEFI arm64 ISO is the one to use, not the Pi's image.

**Always the newest image of a release.** The catalog doesn't pin a file. An entry names a release, the maker's list that names its images (a `SHA256SUMS` file, a directory listing, a releases file) and a file-name pattern, so "Ubuntu 26.04" keeps working when 26.04.2 appears. The app reads the list, takes the newest matching name (there is no separate check that it can still be downloaded: a removed file fails at `vm-image-get`), and verifies the download against that same list, whose signature it checks with the maker's key (confirmed for all four cards on 2026-11-06: every maker signs its list, with the keys pinned in 3.9, "The Linux download cards"). So the catalog stays small and rarely needs an update, and our own signature covers only the entry: which list and which key are trusted. LordOS itself is a guest too, since we build UEFI ISOs for both architectures.

Automatic install: Debian preseed (Kali's too, with its archive and tasks), Ubuntu autoinstall, Fedora kickstart, and LordOS's own `lordos-auto.conf` (the live ISO's unattended install, since 0.8.9.5: 3.9, "The LordOS card"). The person gives a name and password once, in the assistant;
it reaches the guest through a small seed disk (Debian: inside the installer's initrd) that is deleted after the first boot, and only
its hash is ever stored (built in 0.8.9: 3.9, "Install without questions").

Guest tools: `qemu-guest-agent` from the guest's own repository (the unattended installs add it), plus our `lordos-guest`, which
the `lordos` package ships in LordOS guests and **Install Agent** writes into other Linux guests through the agent
(`guest-file-write`, `lordvm.c`). No SPICE agent is used.

### 4.2 Android

The Android card was removed on 2026-11-24 by the maintainer's decision: it added nothing. Android apps are LordBridge's (docs/LORDBRIDGE.md).

### 4.3 virtio-win guests

amd64: full path, built (4.3.1). arm64: **built, not yet installed end to end** (stage C, in development after 0.8.9.5-beta;
4.3.2): the same card, template and answer file on an arm64 computer, from Microsoft's Arm64 ISO checked against the hashes
Microsoft publishes. What it uses:

| Need | amd64 | arm64 |
|---|---|---|
| Machine | q35, SMM | virt with `gic-version=3`; KVM only on a GICv3 host, else emulated (TCG) |
| Firmware | OVMF with secure boot (`OVMF_CODE_4M.secboot.fd`, `OVMF_VARS_4M.ms.fd`) | AAVMF with secure boot (`AAVMF_CODE.secboot.fd`, `AAVMF_VARS.ms.fd`, `qemu-efi-aarch64` 2025.02): no SMM on virt (below) |
| TPM 2.0 | swtpm 0.7.1, `tpm-crb` | swtpm, `tpm-tis-device` |
| CD/DVD drives | q35's SATA (`ide-cd`) | USB mass storage (`usb-bot` + `scsi-cd`), changeable like the others |
| Disk during install | viostor from `$WinPEDriver$` | `viostor/w11/ARM64` from `$WinPEDriver$` (present in virtio-win 0.1.271, **Measured**) |
| Network | NetKVM | `NetKVM/w11/ARM64` (with `netkvmp.exe`) from `$WinPEDriver$` |
| Display | virtio-vga, viogpudo, 2D | `ramfb` (the firmware's framebuffer, kept by the basic display driver), 2D at the firmware's resolution |
| Keyboard, pointer | q35's PS/2 keyboard, USB tablet | USB keyboard, USB tablet (in-box drivers) |
| 3D | none free; an Intel GVT-g vGPU with Intel's driver (6.10.1) | none |
| Agents | virtio-win's guest tools plus our `lordos-guest-win` (7.1.2) | the ARM64 drivers with `pnputil` and the x86-64 guest agent (emulated by the system); virtio-win 0.1.271 has no Arm guest tools installer; `lordos-guest-win` not built for Arm |

Facts to keep in view:

- arm64 virtio-win guests use KVM's GIC. The Raspberry Pi 5's GIC-400 is GICv2: its KVM refuses a GICv3 guest ("KVM does not
  support GICv3 emulation", **Measured** 2026-11-24 with QEMU 10.0.13; `gic-version=2`, `host` and `max` start). A project boots
  the system there with `-M virt -cpu host`, leaving the GIC choice to QEMU (**Reported**, read in its script), which we have
  not run. Machines asks for GICv3, what Microsoft's Arm requirements name: on a GICv2 host the machine is emulated (TCG) and the
  card says so; KVM with GICv2 waits for a real install on the Pi (4.3.2).
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

- Selecting an edition unattended is usually done with the maker's public generic key, which installs and doesn't activate. Built differently from what was first decided (audited 2026-11-23, `windows.js` `editionPlan`, `vmunattend.c`): the edition is always picked by its image name, with the person's own key or else the edition's public KMS client key (`systems.json` `editions[].kms`); Home is refused without a key. See 4.3.1.
- On arm64, a reported working path is to put the installer on an NVMe device with no virtio driver loaded, since Setup can fail when handed drivers it doesn't want, and to install drivers once the system is on disk, from an answer-file step. This is an alternative to mounting virtio-win during install; Phase 3 tests both per release.
- Skipping the network requirement of first-run setup uses the maker's own documented setup switch, which the maker may remove.
- Where firmware lacks secure boot, Setup is told to skip that check. Our amd64 path has real secure boot, so it doesn't need that.

Not feasible: promising this keeps working across future releases of a system we don't control. The switch is marked with the release it was last tested on.

### 4.3.1 Windows 11 on amd64, built (since 0.8.9.5)

Approved on 2026-11-20 ("stage A"): Windows 11 as a card that makes a machine Windows accepts as it is. arm64 (the
Raspberry Pi 5) is stage C.

**Where Windows comes from.** Never from LordOS: nothing of Microsoft's is hosted, mirrored or activated by us.

| Way | What | Checked with |
|---|---|---|
| Your Windows 11 | The multi-edition ISO from Microsoft's page (www.microsoft.com/software-download/windows11). Machines opens the page; the person downloads; Machines finds `Win11_*_x64*.iso` in Downloads when it's whole, or the person chooses it | The SHA-256 Microsoft publishes on that page for each of its 38 languages, pinned in `systems.json` ("isos"); `lord-vm --sha256` hashes the file |
| Try it for 90 days | Microsoft's Windows 11 Enterprise Evaluation, en-US, es-ES, pt-BR, from Microsoft's server (`software-static.download.prss.microsoft.com`) | The SHA-256 of KB5130867, pinned ("pinned"); the admin helper keeps the file only with it |

Microsoft's consumer page builds 24-hour links behind a check of its own: Machines doesn't imitate it. Microsoft signs
no list of hashes, so the catalog (part of the signed package) pins them; a new Windows release means a catalog update
(the hashes of the new build, checked against a download).

**The template** (`lord-vm --create --secure-boot on --tpm on --hyperv on --rtc local --devices windows --drivers ISO`):

- UEFI with secure boot: `OVMF_CODE_4M.secboot.fd` and `OVMF_VARS_4M.ms.fd` (Microsoft's keys enrolled), `smm=on`, the
  variables writable from SMM only.
- A TPM 2.0 (CRB): Debian's `swtpm` per machine (AGENTS.md, "Machines' TPM"): a socketpair for its control channel, its
  state in `disks/tpm`, our jail and its own seccomp, a cgroup leaf. Snapshots keep a copy of the state, clones take it,
  memory snapshots restore it through QEMU.
- Hyper-V enlightenments with KVM; the clock in local time; at least 4 GB, 2 vCPUs, 64 GB (Microsoft's VM requirements).
- Devices (`"windows"`): virtio disk and network for speed; the installer's and the drivers' CD/DVD drives on q35's
  SATA, which Setup reads with its own drivers; a USB tablet. (With virtio's SCSI CD/DVD, Setup can't read its own disc
  before it has a driver, nor the disc the driver is on.)

**Drivers.** virtio-win 0.1.271 from the project's own folder (fedorapeople.org), pinned by SHA-256 (the project
publishes no hash for the ISO). `vmu_prepare_windows` copies the disk's and network's drivers for Windows 11 (every file
their `.inf` names: `netkvm.inf` needs `netkvmp.exe` too, or Setup stops on 0x80070002) into `$WinPEDriver$` on a 2 MB
FAT seed disk, a removable USB stick, which Setup loads by itself, answer file or not: no "Load driver" step.

**Install without questions.** `Autounattend.xml` on the same seed disk, Microsoft's documented settings only: the
disk laid out for UEFI (DiskID 1: the USB seed disk enumerates first), the edition by its image name with the person's
key, or Microsoft's published KMS client key for it (Home has none and needs theirs), a local administrator account,
Microsoft's online-account screens hidden, `BypassNRO` (Microsoft's own value; it may remove it), privacy options off
(`ProtectYourPC` 3), this computer's language, keyboard (`windows.js`) and time zone (CLDR, `windows-zones.json`). At the
first sign-in the virtio-win guest tools install from their disc; the guest agent's first answer tells `lord-vm` the
install is done, and it zeroes and removes the seed disk (Setup has no password hash: the password is there in Setup's
own encoding) and takes both discs out of `machine.json`. The DVD's "Press any key to boot from CD or DVD" gets Enter
from `lord-vm` in the first seconds of the first start.

**Guided.** Windows' own installer asks everything; the seed disk holds only the drivers.

### 4.3.2 arm64 (stage C), built 2026-11-24, in development

**Where it comes from.** Microsoft's Arm64 page (www.microsoft.com/software-download/windows11arm64, "Windows 11 for
Arm-based PCs", Version 26H2): the person downloads; Machines finds `Win11_*_Arm64*.iso` in Downloads or the person
chooses it, and `lord-vm --sha256` checks it against the 38 SHA-256 hashes Microsoft publishes on that page ("Verify your
download"), pinned in `systems.json` (`windows.arch.arm64.isos`, read 2026-11-24). Microsoft offers no Arm64 evaluation,
so that tile is off. The drivers' disc is the same pinned virtio-win 0.1.271 (`virtio-win.arch.arm64`).

**The machine** (`lord-vm --create --arch arm64 --secure-boot on --tpm on --hyperv off --rtc local --devices windows`):
`virt` with `gic-version=3`, AAVMF's secure boot code and Microsoft's keys, swtpm on `tpm-tis-device`, virtio disk and
network, the installer's and the drivers' discs on two USB mass storage devices (`usb-bot` + `scsi-cd`, so the disc can be
changed like on amd64), `ramfb`, USB keyboard and tablet on xHCI, the seed disk as a USB stick. No Hyper-V enlightenments
(x86 only; `machine.c` refuses them on arm64). `vm_machine_check_host` refuses KVM for these devices on a host whose
`/proc/interrupts` names no GICv3 or GICv4 controller (`vm_gicv3_in`), and `lord.vm.create` picks TCG there itself; the
assistant says why (`windows.js` `windowsEmulatedWhy`, `machines.win.noGicThree`). `swtpm` is in `vm-enable`'s list for
both architectures.

**Secure boot on arm64.** virt has no SMM, so AAVMF keeps its variables in flash the guest's own kernel could write: secure
boot checks what boots (an unsigned loader is refused, **Measured** below) but doesn't protect the keys from a system that
is already running. amd64's OVMF keeps them SMM-only.

**Install without questions.** `vmu_prepare_windows` copies `viostor` and `NetKVM` from `<driver>/w11/ARM64` into
`$WinPEDriver$`; `Autounattend.xml` names `processorArchitecture="arm64"` in every component; at the first sign-in
`pnputil /add-driver …\w11\ARM64\*.inf /install` adds Balloon, NetKVM, viogpudo, vioinput, viorng, vioscsi, vioserial and
viostor, then `msiexec` installs the x86-64 guest agent (`guest-agent/qemu-ga-x86_64.msi`), which the system runs emulated;
its first answer ends the install as on amd64.

**Measured on the Raspberry Pi 5** (2026-11-24, a scratch `lord-vm` in private namespaces with a private home; Alpine
3.24.4 aarch64 as the disc, since it isn't signed by Microsoft): with `--accel kvm` lord-vm refuses ("this computer's
interrupt controller is GICv2…"); with TCG the machine starts as above, AAVMF finds the USB CD/DVD and refuses Alpine's
loader with secure boot on ("Access Denied"), and with secure boot off it boots it from that drive.

**Not verified:** a Windows 11 Arm64 install itself (the ISO is 6.9 GB and comes only through Microsoft's page, which
Machines doesn't imitate; the Pi has 5.6 GB free), whether Setup reads the USB CD/DVD and the ramfb picture, whether the
x86-64 guest agent runs emulated over vioserial (the install-done signal), swtpm on arm64 (not installed on our Pi), and KVM
on a GICv3 arm64 host (we have none). Until a real install passes, the card is offered on arm64 as built, not as tested.

### 4.4 Not offered

Guests whose license restricts them to their maker's hardware. Guests needing a real GPU for their desktop on hardware without passthrough.

## 5. The app

**Alpha (Phase 1):** a minimal library, the New Machine sheet and the machine window, described in 3.9 (The app). Most of what
follows is built since; the audit of 2026-11-23 marks below, per subsection, what the code does differently or doesn't have.
The status checklist at the top has the totals.

Name and icon: **Machines** (`apps/machines/manifest.json`; open decision 1 settled by the code). The icon is a screen holding a smaller rounded tile, on the LordOS shape (radius
0.225). Mocks: https://claude.ai/artifact/WNiELGEazSqnr68SFWuyPz (private until shared).

### 5.1 Control Center (main window)

**Redesigned 2026-11-22 (the approved mocks "Main" and "List"; `apps/machines/library.jsx`).** Toolbar: the gear (Machines
settings), search, grid or list, + (New machine). Chips: All, Running, Linux, Windows, each with its count (`chipCounts`,
`familyOf`; settings `machinesView.show`). Grid: the New machine card first, then a card per machine: its picture (live while
its window shows it, the last one when it's off) with the state pill, CPU and memory while running (or the graphics card's
name when off), ▶ to start or resume, Open while running; its name and one line "10 CPU · 8 GB · 64 GB · Bridged · Intel
card" (`specParts`); ··· with the machine's menu. List: picture, name and state in color, the same line, CPU and memory bars
while running, else "Last used … ago", "Resumes where you left it" or its profile (`restLine`), ▶ or Open, ···. Bottom:
running, memory, free disk (a click opens Storage), memory for machines (3.7) and "Short of memory" under PSI pressure. Arrows move the selection, Return opens or starts, Escape clears the
search; the order is in the View menu. While a machine runs its card has two lights (*in development*, `Lights`, `model.js`
`cardLights`): the guest tools' connection as lord-vm writes it in `run.json` (connected, starting, none) and the disk, lit when
QEMU's cgroup I/O counter grew between two reads (every 1.5 s); each a symbol in its color with its words beside it on a wide card
and always in its tooltip. On battery with machines running the toolbar has **Pause All** (6.14); on a kernel without Landlock a
notice under the toolbar says "Weaker isolation on this kernel" (3.3). Machines settings (gear or the Machine menu): General, Profiles (5.6), Systems
(5.6.1), Network, Graphics cards, Keyboard and mouse (`settingswin.jsx`).

The original plan below is kept for reference.


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

Steps, each one screen, with Back always available.

**Built (audited 2026-11-23, `assistant.jsx`):** five steps, Choose, Profile, Name (or Set up), Review, Create. There is no separate
Detect step, no folder choice (machines go in `~/Machines`) and no **Fresh install** snapshot.

**An ISO is recognized inline from its boot files (in development, after 0.8.9.5-beta):** `lord.vm.isoInfo` runs
`vm/vmiso.c` on the chosen file through one descriptor: the primary volume's label; the El Torito catalog's platforms (x86
BIOS 0x00, UEFI 0xEF); every `EFI/BOOT/BOOT*.EFI` (case-insensitive: X64, AA64, IA32, RISCV64) in the ISO 9660 tree (Rock
Ridge or plain names, then Joliet), in the UDF tree (an install disc keeps its files there; its ISO 9660 side has only a
README) and in El Torito's UEFI image (its FAT12/16/32 ESP, short and long names), each one's PE header machine (0x8664
amd64, 0xAA64 arm64, 0x14C i386, 0x5064 riscv64); and `sources/install.wim|esd|swm` for an install disc. The architecture
is the PE headers', else the boot files' names, else x86 when El Torito has only a BIOS entry (`archFrom: "boot"`); when
they say nothing, or name both amd64 and arm64, `model.js` `isoArch` takes it from the file's name or label (`"name"`),
and the summary card says which ("arm64, read from its boot files"), or that the boot files are for an architecture
Machines can't run (riscv64). The file is untrusted: every offset and length is checked against the file, paths are fixed
(no tree walk, so a directory that points at itself can't loop), FAT chains and UDF descriptors are capped, and a probe
stops at 4,096 reads or 24 MB (40 reads on a network or cloud mount). `tests/vmiso_test.c` builds each kind of image byte
by byte, adds hostile ones and fuzzes them under ASan and UBSan. Checked on real ISOs: Alpine 3.24.4 aarch64 virt (arm64
from its PE headers, though its file name says nothing), an amd64 install disc's evaluation (UDF, install.wim, amd64 from
the ESP), Ubuntu Server 26.04.1, LordOS 0.8.9 live, virtio-win 0.1.271 (no boot files: name).

An `.iso` dragged from Files onto the "Use an ISO" tile is picked as if chosen in its file picker (`useFilesDrop`); another
file says only an .iso goes there. Automatic setup of a Windows guest is **on** by default, and the person may type a
product key (else the edition's KMS client key, 4.3.1). The requirements check is the app's **What's Missing** page with the admin
action `vm-enable`, not the Store. The design was:

1. **Choose:** cards.
   - Download: Debian, Ubuntu, Fedora, Kali (built 2026-11-06, 3.9, "The Linux download cards"); LordOS (its live ISO, signed with the release key; since 0.8.9.5, 3.9, "The LordOS card").
   - "Install from an ISO file" (drag a file on it).
   - "Migrate from another app": a machine of another virtualization app (6.22). Built 2026-11-05; it took the tile the mock gave
     "Migrate from another PC" (6.11), which comes later.
   - "Import a machine": a qcow2 or raw disk image, see 6.22.
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

**Built (audited 2026-11-23, `vmwindow.jsx`):** the bar's groups are agent, clipboard, folders, apps | resolution, capture, Keys,
devices, sound, microphone | shell, logs, screenshot, record | play or pause, power | Configure, full screen. Power holds Shut down,
Suspend, Restart, Send Ctrl+Alt+Delete and Force off; the menus are **Actions**, **Devices** (CD/DVD, sound, microphone) and **Apps**.
The resolution menu is always shown, with **Follow the window** as one of its items. Capture is Automatic by default and released
with ❖Esc or Ctrl+Alt (fixed, 6.25). Built since (*in development*): a **View** menu (Window, Full Screen, Apps, **Picture in Picture**, **Actual Size**: the window
sized to the guest's display while the resolution is fixed, `actualFrame`), an **Info** menu (CPU, memory and disk from two
`lord.vm.stats` reads 2 s apart, the sandbox, Open Configuration…, `infoLines`) and **Start in Full Screen** in the machine's ···
menu (the window takes its display as it opens). Not built: Scale, USB, Folders, Network and Camera in Devices, per-device
status dots, the window keeping the guest's shape. The design was:

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
| **All Displays** (not built) | One head per display | virtio-gpu multi-head |
| **Apps** (seamless) | Guest apps as LordOS windows, no guest desktop (built, 7.2) | LordOS or Windows in the guest, with the LordOS tools |
| **Picture in Picture** (built, *in development*: View → Picture in Picture) | Small floating view at the bottom end of the window's display, the guest's shape kept (`pipFrame`); it takes no keys and no pointer for the guest (live control is off and not offered); its name and **Back to Window** show under the pointer, a double click goes back too; never for a protected machine (it floats on every desktop) | Our `setPip` |
| **Thumbnail** (the library's card: last picture with a round Play) | Paused at last frame, one click to resume | Suspend |

### 5.5 Configuration (one screen)

**Built (audited 2026-11-23, `configure.jsx`; the search and the dots since, *in development*):** a search over the sections'
titles and summaries and a dot on each section with a change not saved yet (`changedSections`, `sectionsMatching`); one window with eleven sections and their summaries: General, Startup and
shutdown, CPU and memory, Display, Network, Sound and input, Disks, Sharing, Boot, Security (what the engine can reach: sandbox,
files, network, limits, picture and input, passwords), plus the guest agent (tools updated by themselves), Notes, "Pointer drawn by",
compatible or fast devices and "Capture automatically in full screen". **Keep this computer awake** is on by default. Not built:
search, changed-section dots, Backup, hugepages, ballooning, CPU model, printers, USB rules, adding, resizing or encrypting disks.

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

**Built 2026-11-22 (since 0.8.9.5), from the approved mocks "Settings", "ProfileEdit" and "ProfileImport".**
Code: `apps/machines/profiles.js` (pure, `server/tests/vm_profiles_test.mjs`), `store.js`, `settingswin.jsx`, `assistant.jsx`.

A profile is the starting point of a new machine: only values, every one shown and changeable later in Configure. The New
Machine assistant starts on the default profile (Machines settings → Profiles → Make default; `settings.machinesDefaults.profile`)
or on the one an added system names, and its step 2 lists LordOS's five and the person's own.

| Profile | Processors | Memory | Disk | Graphics | Network, integration, input |
|---|---|---|---|---|---|
| Productivity (default) | 4 | 8 GB or 1/3 of RAM | 64 GB | 3D where the system can | shared; clipboard both ways; transfer folder on |
| Development | half | 16 GB or 1/2 | 80 GB | 2D | shared; clipboard both ways; transfer folder on |
| Games | half | 16 GB or 1/2 | 128 GB | 3D; Intel card (shared) when one is set up | shared; mouse captured, captured in full screen |
| 3D design | half | 32 GB or 1/2 | 80 GB | 3D | shared; clipboard both ways; transfer folder on |
| Light | 2 | 4 GB or 1/2 | 40 GB | 2D | shared; nothing shared |

**Fitted to this computer, always** (`onHost`): never more than half of its processors (`cpuCap`, also within
`vcpusRecommended`), never more memory than machines may use here (`host.memMaxMb`, `memCap`), a graphics card only when
Settings → Graphics cards set one up for machines (`gvtGpu`), 3D only where `no3dReason` allows. Machines settings shows the
result as "On this computer"; the assistant creates with it.

Note: the approved mock gives Productivity the clipboard both ways and the transfer folder; before 0.8.9 no profile shared
anything by itself. The review step still shows the clipboard switch (now on for those profiles) and the person can turn it
off; the transfer folder is the machine's own folder under ~/Machines (6.7.1), not the person's files.

**What creating applies.** `lord.vm.create` takes CPU, memory, disk, shared or isolated network, the clipboard both ways and
3D; right after, while the machine is still off, `lord.vm.set` gets the rest (`setPatch`): host-only or bridged network (the
card from Machines settings → Network, else the first wired one up), its port rules, another clipboard direction, the
transfer folder, the shared folder and the graphics card. `app.json` keeps the profile id, what closing the window does,
apps mode and the keyboard and mouse choices (`meta.input`: `mouse` auto|capture, `shortcuts` ask|never,
`captureFullscreen`). The machine window uses `meta.input` (`vmwindow.jsx`: the Keys panel and Capture mouse, 6.25).

**A profile's shared folder** is a path under the person's home, written `~/…`, no hidden folder and no `.`/`..` part
(`sharePathOk`); the review step shows it with a switch, it is shared only if it exists, and lord-vm checks it again
(`share_writable_ok`, never a folder that holds a protected file).

**The person's profiles** live in `~/.config/lordos/machines/profiles/u-<8 hex>.json`, the same JSON as a `.lordprofile`
plus their id: New, Duplicate, Edit (the six parts of the mock: identity, system, resources and graphics, network,
integration, keyboard and mouse), Delete (moved to the Trash and out of it), Make default.

#### The .lordprofile file

```json
{
  "format": "lordos-machine-profile/1",
  "name": "Linux Mint para oficina", "author": "Ana Pérez", "description": "", "color": "green",
  "system": { "kind": "linux", "suggested": ["s-1a2b3c4d", "debian"],
              "image": { "id": "s-1a2b3c4d", "name": "Linux Mint 22.1 Cinnamon", "kind": "linux", "install": "manual",
                         "url": "https://…/linuxmint-22.1-cinnamon-64bit.iso", "sha256": "<64 hex>", "size": 2900000000,
                         "signature": { "url": "https://…/sha256sum.txt.gpg", "sums": "https://…/sha256sum.txt", "fingerprint": "<40 or 64 hex>" } } },
  "resources": { "cpus": 2, "memoryMb": 4096, "diskGb": 40, "graphics": "2d", "gpu": "" },
  "network": { "mode": "hostonly", "ports": [{ "proto": "tcp", "host": 2222, "guest": 22, "open": "this-computer" }] },
  "integration": { "clipboard": "off", "share": { "path": "~/Servidor", "writable": true }, "transfer": false, "appsMode": false, "onClose": "ask" },
  "input": { "mouse": "auto", "shortcuts": "ask", "captureFullscreen": false },
  "logo": { "url": "https://…/logo.png", "sha256": "<64 hex>" }
}
```

Export writes it from the editor (a FilePicker save). Import (Machines settings → Profiles → Import…) reads it with
`readProfile`, strictly: at most 64 KB; `format` must be exactly that; a name of 1–64 characters without control or bidi
characters; known fields only (each unknown one listed: "«script»: unknown field, ignored. A profile can't run anything");
enums (`NET_MODES`, clipboard, `CLOSE_CHOICES`, `GPU_CHOICES`, mouse, shortcuts, kind, color) fall back to the default and
are listed; `cpus` 1–64 or "half", `memoryMb` 256 MB–1 TB, `diskGb` 1–16384, then fitted to this computer ("«cpus: 32»
becomes 10: never more than half of this computer"); port rules are checked as Configure checks them (`portProblems`: host
port 1024–65535, no duplicates, LAN not in host-only), at most 32, a wrong one dropped; suggested systems only those this
computer knows (the catalog, the person's own, the one the file brings). The summary sheet (the mock "ProfileImport") shows
all of it before anything is kept; Import keeps the profile and, when the file brings a system, adds it to "Added by you".
A profile never executes or downloads anything by itself.

**An import never widens anything silently** (commit security review 2026-11-22): every forwarded port of an imported
profile becomes this computer only (`readProfile` with `imported`, listed as "Port … was open to your network; now only this
computer"); the summary's ports line says each rule's reach, and "What it opens" (`exposureOf`) names a bridged network (on
the card the person set in Machines settings → Network, shown again in the review), the clipboard both ways or from the
machine, and a writable shared folder (the folder is confirmed by the person in the review). The person's own profiles keep
a port open to the network only when they chose it in the editor, whose per-rule control warns about it.

### 5.6.1 Your own systems (Machines settings → Systems)

The catalog (assets/machines/systems.json) with its state per architecture: Downloaded (an image of the card, or of the one
its unattended install boots, is in /var/lib/lordos/machines/images) or "Download · size", which downloads it ahead of time
through the same pinned and signed-list machinery as the assistant (`download.js`: vm-image-list, vm-image-get; the Windows
card its evaluation in the person's language).

**Added by you** (`systems.js`, `~/.config/lordos/machines/systems.json`, format `lordos-machine-systems/1`): name, kind
(Linux, Windows, Other: the card's mark and the library's chips), icon color, default profile, install (manual, or without
questions the way the catalog's Debian (preseed), Ubuntu (autoinstall) or Fedora (kickstart) installer does: the catalog
entry of that kind gives kernel and initrd and lord-vm checks everything again; only for Linux), and the image:

- **An ISO on this computer**, picked in the home folder: hashed at once (`lord.vm.hash`, lord-vm --sha256), its SHA-256 and
  path kept as verified.
- **An https address and its SHA-256** (required, 64 hex digits; `http://`, user names in the address, other ports than
  443, an IP address or a local name such as `localhost`, `*.local`, `*.lan` or `*.internal`, other schemes or a path that
  isn't a plain file name are refused: `httpsUrl`, `publicHost`, `fileOfUrl`). **How it downloads (approved by the
  maintainer on 2026-11-22):** "Download…" shows a sheet with the full address, the size when known, the SHA-256 and what
  the signature check will be, then runs the closed admin action `vm-image-get-user "<url> <sha256> <size|-> [<signature
  url> <checksum file url>]"` (administrators only; `server/src/vm/vmimage_admin.c`). Root checks every address again in C
  (`vmurl_check`, `pkg/pubhost.c`: https, a public DNS name with no IP literal and no local suffix, port 443 only, no user
  name or password, RFC 3986 path characters with `%XX` escapes but never `%00`, no `.`/`..` segment, no fragment, at most
  1000 bytes), the SHA-256 (64 hex) and the size (1 B to 16 GiB; unknown: up to 16 GiB). The image store must keep 2 GiB
  free after it (`vmurl_room`, `vm.noSpace`). The download is the catalog's own machinery: the Store's downloader in
  `lordos-net-machines.scope` (a child as `nobody`, TLS by `lord-tls`), with `FetchItem.public_only`: it connects only to
  public addresses the name resolves to (no private, loopback, link-local, CGNAT, documentation, multicast or reserved
  block, IPv4-mapped and 6to4 addresses judged by their IPv4 address) and follows a redirect only to an address that passes
  the same rules (`pkg.http.notPublic`). The image lands in `/var/lib/lordos/machines/images/<sha256>.iso`, resumed from
  its `.part`, with progress lines and "Stop Download" (`vm-image-cancel`), and is kept only with that SHA-256; its
  `<sha256>.json` says `"id": "user"`, the file name and the address without its query (`vmurl_redact`: never a token in
  a query, a user name or a password), which Storage shows as "file · site". The system's `verified` is then that store
  path. "Use a file I already downloaded…" stays for an ISO the person has: hashed by lord-vm and kept only if it matches.
  The browser fallback of the first build (open the address in the browser, watch Downloads) is gone.
- **Signature (optional):** a detached OpenPGP signature over a checksum file, the project's key fingerprint (40 hex for v4;
  a v6 one, 64 hex, is stored but can't be checked) and the project's **public key** (armored, pasted or chosen as a file,
  at most 32 KB; `keyOf`). With a key, `vm-image-get-user` also downloads the signature (at most 64 KB) and the checksum
  file (at most 1 MB) from their addresses, checked the same way, into `lists/u-<sha256>.sig` and `.sums` (root's; root
  never reads them). Then `lord.vm.sigCheck(sha256, key, fingerprint)` reads them (root's regular files, not writable by
  others), puts the key, the checksum file and the signature in three sealed memfds and starts **`lord-pgpv`**
  (`server/src/vm/lordpgpv.c`, approved by the maintainer on 2026-11-22): fixed path next to lord-server, no arguments,
  an empty environment, the fingerprint and the time on fd 0, the memfds on fds 3–5, its answer on fd 1. It applies
  `sandbox_apply_helper` (no network, no files, no exec, 32 MB) before reading anything, verifies with LordOS's own
  OpenPGP code (`pkg/pgp.c` through `vmsig.c`; no new library), where only the primary key with that fingerprint (or its
  subkeys) counts, and prints exactly `valid <fingerprint>` or `invalid <reason>`. The key is untrusted input, so it is
  read with `pgp_keyring_add_untrusted`, unlike root's APT keyrings: every self-signature, subkey binding (0x18) and
  revocation must verify with the primary key (SHA-256/512, RSA or Ed25519; SHA-1 ones don't count), and only verified
  ones give key flags, expiry and revocation; a primary key with no verified self-signature and a subkey with no
  verified binding are dropped, and a subkey may sign only when its newest binding gives it the sign flag and carries a
  valid back-signature (0x19) made by the subkey itself. A subkey lives no longer than its primary. So a subkey grafted
  under a genuine primary key (from a pasted block or an imported profile) can't sign as that fingerprint, and the pin
  can't be bypassed that way (Claude Security F3/F4, 2026-11-22). Debian's archive keys sign with such bound subkeys;
  Debian CD, Ubuntu's CD image, Kali and Fedora sign with the primary key. lord-server reads that line strictly
  (`vmsig_reply`: the fingerprint it asked for, a reason from a fixed list) and only then requires the image's SHA-256 to be
  a line of the very checksum file bytes it handed over (`vmsig_sums_has_hash`). Settings → Systems says "Signature:
  verified by ‹fingerprint›" or "Signature: invalid — not used" (or that the checksum file doesn't list the image), and
  the image is verified only when both the SHA-256 and the signature hold. **Pinned on first use:** the fingerprint of
  the first good check is kept in that system (`signature.pinned`, never taken from a profile); if the system's key later
  has another fingerprint (an edit, a changed definition), the download is held with "This system's key changed (it was
  …)" until the person presses "Trust the New Key". Without a key the signature is stored and shown as "Signature: not
  checked"; a profile carries the addresses, the fingerprint and the key, never the pin.

**Before a machine is made from an added system** the assistant hashes the image again and refuses it if it changed
("The SHA-256 of … doesn't match: it isn't used"). Added systems show in New Machine under "Added by you"; one not verified
yet is shown dimmed. An added Windows image is made as a plain ISO machine (no TPM template or answer file: those are for
the image Microsoft lists, 4.3.1).

**Storage and fsops_protected.** Profiles, systems.json and the logos folder are written with lord.fs as the person. None of
them launches anything or holds a permission, so they are not in `fsops_protected`; whatever they hold is validated again on
every read (`readProfile`, `readSystems`), an image is used only after its SHA-256 matched (and its signature, when the
system has a key), and lord-vm checks every value again. A program that can write the home folder could edit them, as it
could edit the ISOs themselves, including a system's key and its pinned fingerprint; it can't make Machines download or
run anything without the person pressing the button that shows the address, root checks the address again, and only an
administrator can download.

### 5.6.2 Official logos (opt-in)

Approved by the maintainer 2026-11-22. LordOS ships no third-party logo (the Debian, Fedora and Kali SVGs that were in
`assets/machines/logos` were removed); its own drawn marks are the default and the fallback. Machines settings → General →
"Official logos of the systems", off by default; turning it on asks for confirmation with "The logos are downloaded from each
project's official site and stay on this computer only. The marks belong to their owners." Off: no logo shows, at once.
"Remove downloaded logos" deletes the PNGs.

- **Which systems:** only those whose owner's policy explicitly lets a third party show the logo to identify its software
  (docs/TRADEMARKS.md, checked 2026-11-22): **Debian** (Open Use Logo). Not Ubuntu (written permission), not Fedora (limited
  to promoting the Fedora Project), not Kali (no commercial products without written permission), not Linux Mint (no
  official PNG), never Windows.
- **Format:** PNG only, at most 512 KB and 1024 × 1024 (`pngHeader`: signature, IHDR first, sane depth and color type).
- **How:** each is a hidden catalog entry of kind `logo` (`logo-debian`, `for: debian`) with its https address, SHA-256 and
  size pinned, so the admin helper's existing `vm-image-get` downloads it like any pinned file (the address from the
  signed catalog, never from JS; kept only with that hash). The app then checks the bytes, copies them to
  `~/.cache/lordos/machines/logos/.<sha>.download.png` and has lord-media re-encode them in its sandbox (`lord.media.edit`,
  MW_EDIT, fds only) into `<sha256>.png`; the copy is deleted. Only the re-encoded PNG is shown. The helper's copy stays in
  the root-owned image store (not listed in Storage); removing it needs the admin action `vm-image-remove`.
- **Profiles and added systems** may carry `logo: { url, sha256 }` (PNG, https): validated, shown in the import summary
  with "A logo in a profile is its author's responsibility: LordOS doesn't host or share it", exported as address and hash
  only, never the image. This version doesn't download them (vm-image-get-user is for a system's image only).
- About shows the trademark notice in every language.

### 5.7 What this computer can run

**Built (*in development*, `library.jsx` `ThisComputer`, `model.js` `canRun`, `sensorLines`):** one page for amd64 and arm64, in
the Machine menu (the theme's Help menu has no app items) and on the first-run page, generated from `lord.vm.host()`: native systems (KVM, or why not: off for
this user, or missing), the other architecture by emulation, the Windows 11 and Android cards (amd64 only), 3D graphics (or the
reason, the same words as Configure), a shared Intel graphics card (GVT-g), nested virtualization (KVM's `nested`). While machines
run it shows the first thermal zone's temperature, the first hwmon fan and a Raspberry Pi's power supply (the `rpi_volt` alarm),
read from world-readable sysfs (`lord.vm.sensors()`); a microSD library and an emulated machine keep their warnings. The design: a screen in the app's Help and first run lists, per host, what's native, slow, limited or impossible, in the words of section 2 and with real values for this board: cores, memory, disk type. On a Pi it also warns about microSD and shows temperature, fan and voltage while machines run. It's generated from what the host reports, so it never claims more than the hardware gives.

### 5.8 Menus, shortcuts

Follows `docs/SHORTCUTS.md`: app menus, ❖ shortcuts for the host while the app has focus outside capture, all passed to the guest while captured. The release combination (❖Esc or Ctrl+Alt) is shown when capture starts; it is fixed, not configurable (`capture.js`). In Lord Classic the same table.

Since 0.8.8.8 the VM window can also take the desktop's shortcuts (❖, ⌥Tab and the rest) while it has the keyboard, so they reach the guest (`apps/machines/vmwindow.jsx` → `sdk/kbinhibit.js`, `sdk/kbinhibitmodel.js`). It follows the same rule as a Linux app's keyboard-shortcuts inhibitor: the person allows it once (the `shortcuts` permission, Settings → Privacy → Keyboard shortcuts), ❖Esc turns it off for that window and back on, and the lock, the console switch and the media keys always stay LordOS's.

**Keys panel** (since 0.8.9.5): a keyboard button on the machine window's bar (and on the full-screen floating
bar) opens a panel, like noVNC's side keys but richer (`apps/machines/keys.jsx`; the tables and the latch logic are pure, in
`apps/machines/model.js` `KEY`, `keyCombos`, `latch*`, `comboSend`, tested in `server/tests/vm_model_test.mjs`):

- **Hold down**: Ctrl, Alt, Shift and Win (Windows guests) or Super (the others). A click presses it in the guest and keeps it
  down until the next key typed into the machine (another modifier keeps them, so held Ctrl + Shift + T works) or the next
  combination sent; then they go up in reverse. A second click lets it go. Nothing stays stuck: blur, leaving the window, the
  machine stopping and the window closing all release everything (`lord.vm.release`, the same path as a held physical key).
- **Combinations**, each one click (down in order, up in reverse, like Ctrl+Alt+Delete). Windows guests: Ctrl+Alt+Del, Win,
  Win+P, Win+D, Win+E, Win+R, Win+L, Win+Tab, Alt+Tab, Alt+F4, Ctrl+Shift+Esc, Win+Shift+←/→, Win+↑/↓, PrintScreen,
  Win+Shift+S. Others: Ctrl+Alt+Del, Super, Alt+Tab, Alt+F4, Ctrl+Alt+T, PrintScreen and the text consoles Ctrl+Alt+F1…F7.
  They go straight to the guest with `lord.vm.key`, never through LordOS's shortcuts, so ❖P reaches a Windows guest without
  the `shortcuts` permission.
- **Single keys** (Esc, Tab, Insert, Delete, Home, End, PgUp, PgDn) and **F1–F12**.
- **Show on-screen keyboard**: LordOS's own (`sdk/osk.js`). Its taps go down the same path as a physical key
  (`lord.input.tap` → `js_key`) to the window in front, so the machine window forwards them like typed keys, held keys included;
  no change to the keyboard was needed.

While the panel is open the keyboard keeps typing into the machine (it is not a menu), so a held Win and a P typed after it make
Win+P; a click outside closes it. The menu bar's **Actions → Send Keys** has the same combinations and single keys; Power and
Actions keep their own Ctrl+Alt+Delete.

**Keys reach a machine only while it has the keyboard** (security, since 0.8.9.5). A key listener
(`runtime/events.js` `addListener`) hears every key of the session, whatever window it was typed in, so a window's own
`focused` flag decides nothing on its own. Every path that sends keys to a guest (the machine window's keyboard and its
serial console pane in `vmwindow.jsx`, each apps mode window in `appsmode.jsx` `GuestWindow`, the Keys panel's held keys)
asks `wm.hasKeyboard(window)` **at each key**: true only for the front window of the showing desktop while no search,
launcher, sheet or system prompt has the keyboard (`sdk/desktopmodel.js` `keyboardWindow`, the same rule as the Wayland
keyboard's `keyboardSurface`). The first key typed while it is false lets go of everything held in the guest
(`lord.vm.release`, the held keys included), and a release is sent only for a press that went to the guest
(`apps/machines/model.js` `guestKeys`). What was wrong before: `FloatingWindows` gave every full-screen window of the
desktop the keys and the theme gave them to the top framed window too, so with a machine full screen on one display and
the Terminal on another both heard each key, and a password typed in the Terminal went into the guest; a system prompt
over the machine window sent its keys there as well. Now `AppHost` is focused only when it is also `wm.focused()`, a press
on a full-screen window brings it to the front, and going full screen brings the window forward. In apps mode a window the
guest opens or brings back takes the front only while the person is in this machine (its window or one of its guest
windows in front, `guestMayTakeFront`); otherwise it opens behind the window in front, so a guest cannot steal the focus
from a Terminal being typed in. The desktop's shortcuts held for the machine (`sdk/kbinhibit.js`) follow the same
`wm.hasKeyboard`. lord-server sends a key to a guest only from `lord.vm.key` (`ui/js_vm.c` `f_key` → `VD_KEY`); capture
mode takes the mouse only, never the keyboard, and lord-vmdisplay lets go of every held key when lord-server goes away, so a
session that re-attaches to a running machine finds nothing held. Tested by `server/tests/vm_model_test.mjs` (`guestKeys`:
focus moving to the Terminal or a search between a press and the next key), `desktop_model_test.mjs` (`keyboardWindow`)
and `vm_keys_scene.mjs` (`node tests/focus_scene_test.mjs vm_keys_scene.mjs`: a framed and a full-screen machine with the
Terminal in front, and a system prompt over the machine, through the real AppHost, FloatingWindows and `__lordKey`).

## 6. Features

### 6.1 Lifecycle

Start, pause (vCPUs stop, instant), suspend (RAM to the folder, survives a host reboot), shut down (the guest agent first when it answers, then also the ACPI power button 10 s later; without the agent, the button, then the agent if it shows up; built in 0.8.9), force off. **Host shutdown** (in development since 0.8.9.5: `vmhoststop.h`, `vmlogind.c`): lord-vm plans the stop inside logind's 45 s delay: a save until +33 s, else the guest's own shutdown, QEMU told to quit at +41 s, SIGKILL at +44 s; a machine with a graphics card or 3D goes straight to the guest's shutdown. A machine that fails or crashes shows the machine log, a likely cause when one is known (not enough memory, a missing disk, a locked file) and a fix that fits it, such as starting with less memory or going back to a snapshot. *Built since (in development):* the failure page names a locked disk (QEMU's image locking: another process holds it) and a
missing disk or disc image from QEMU's own words (`model.js` `failCause`), and offers the fix that fits (`failActions`): **Start
with ‹N› MB** after running out of memory, **Go Back to a Snapshot…** when the machine has snapshots and stopped because of the
system inside or the engine; Store → Updates warns when an update changes QEMU while machines are suspended
(`apps/store/syspkg.js` `qemuUpdateRisk`). Nothing is unplugged before a save: the HDA card is migrated with the state, and what
can't be saved (a graphics card, 3D graphics) is never suspended (`noSuspend`). *Built:* a state saved by another QEMU build is named before loading, with **Resume anyway** or **Discard saved state** (`lordvm.c`, `vmwindow.jsx`).

### 6.2 Snapshots

qcow2 internal snapshots, with RAM optional. Timeline view with names, notes, thumbnails and size. Revert asks and offers to snapshot first. Auto-snapshot before a guest's tools upgrade (not built: tools update without one, 7.1.3). Weak: internal snapshots slow down with many; a warning at 20, at most 64 (`vmops.h`). Built besides: Clone from a snapshot (`--from-snapshot`). Snapshots and backups aren't the same: Configuration says so.

**What a snapshot holds.** Without care, "snapshot" means five things: disks, memory, NVRAM, TPM and settings. The model:

- A snapshot covers **all writable disks together**, the NVRAM and the TPM state (`disks/tpm/snap-<tag>.permall`); a copy of `machine.json` is not kept (audited 2026-11-23). With memory, it resumes where it was; without, it's as after power loss.
- Taken with QMP `snapshot-save`, listing every writable node in `devices`, as QEMU advises for a consistent snapshot. It's a job: the command returns before it's done, and `lord-vm` follows `JOB_STATUS_CHANGE` and fetches errors (**Reported**, QMP reference; since 6.0). The app shows progress, and the guest may stop during the save.
- **Raw NVRAM blocks internal snapshots:** QEMU refuses with "Device 'pflash1' is writable but does not support snapshots", which is why UEFI machines have long had trouble with them (**Reported**). Our NVRAM is therefore qcow2 from the start. This can't be added later without converting.
- TPM state: swtpm's state goes with the machine's memory state in a live snapshot, as far as we can tell from how it migrates (**To test**). A disk-only snapshot copies the `tpm/` folder with the machine off.
- Without a running guest's cooperation, disks are crash-consistent. With the agent, a filesystem freeze comes first, and the timeline says which it got.
- **Failure midway:** the same `tag` may exist on some disks and not others. `snapshots.json` is written last, so an entry there is proof of a complete snapshot; a leftover tag is removed at next open, and `operation.json` (3.6) names the work.
- Revert uses `snapshot-load`, also a job, with the machine paused, after an automatic safety snapshot if there's room.

**Disposable sessions** use explicit temporary overlays on each disk, deleted at shutdown. QEMU's global `-snapshot` can't be used: its manual says it's incompatible with `-blockdev`, which we use, and it forces unsafe caching (**Reported**). NVRAM and TPM get temporary copies too, or a disposable session would change them. After a crash, the overlays are discarded at next start.

### 6.3 Clones

Two kinds, named for what they do:

- **Clone:** can run beside the original. It gets a new MAC and, through the agent, a new `machine-id` and hostname, which avoids the usual trap of two VMs receiving one DHCP address. A Windows guest gets a new computer name the same way (from its next restart); its machine SID stays (sysprep's job).
- **Duplicate:** an exact copy with the same identity, the only kind that can resume a suspended session.

A **linked clone runs alongside its original and takes only what changed**:

- On btrfs and XFS, a reflink copy: instant, shared until changed, and both are independent.
- On ext4, which has no reflink, the disk becomes a frozen read-only base, and both machines get a thin qcow2 overlay on it. A running source can't: the source must be off (`lordvm.c`); a live `blockdev-snapshot` is not built. Both then run together. The base lives in `~/Machines/.shared/` and is deleted when nothing uses it. Storage shows it.

A machine with a saved session can't be cloned until it's resumed and shut down, since a RAM image holds its identity.

The app picks by file system and says which one it used. Templates are read-only machines made to clone.

### 6.4 Storage

- **qcow2** by default: thin, snapshots, compression, encryption.
- **raw** optional: fastest, no snapshots.
- **Reflink** on btrfs and XFS: instant copies.
- Built: qcow2 through `-blockdev` with `discard=unmap`; Reclaim space compacts offline. Not built: raw disks, encryption,
  `io_uring` (our seccomp allowlist refuses it), export.
- `virtio-blk` by default (the compatible devices: SATA), served by an I/O thread of its own, the host's cache bypassed on ext4 and XFS: 6.4.1.
- TRIM from guest is passed to shrink. "Reclaim space" compacts offline.
- Export, with the machine shut down: a `.lordvm` bundle with or without snapshots, OVA, or a bare disk image, optionally encrypted. Import: 6.22.

### 6.4.1 The disk path (2026-11-23, in development)

How lord-vm gives QEMU a machine's disks (`lordvm.c` `blockdev_disk`, `disk_direct`, `virtio_disks`):

| Piece | Now | Before |
|---|---|---|
| Device | `virtio-blk-pci` for the system disk and the data disks (Linux and the virtio-win device set; Windows Setup loads `viostor` from the drivers' disc as before); SATA for the compatible devices | the same |
| I/O thread | one `iothread` (`disks`) serves every virtio-blk disk, off QEMU's main loop (which also runs the display, QMP, sound and the other devices) | the main loop |
| Host cache | bypassed (`cache.direct`, `O_DIRECT`) when the disk file is on ext4 or XFS and takes `O_DIRECT`; the guest caches for itself. Flushes still reach the disk (`no-flush` off) | the host's page cache (writeback) |
| AIO | Linux native AIO (`aio=native`) with `cache.direct`; QEMU's thread pool otherwise | the thread pool |
| Elsewhere (btrfs, tmpfs, a network file system, `O_DIRECT` refused) | the host's page cache and the thread pool, as before. Not on btrfs: `O_DIRECT` there checksums buffers a guest may still change in flight | — |
| Discard | `unmap` on the file and the qcow2 node (TRIM shrinks the file) | the same |
| `detect-zeroes` | not set (it checks every write for zeroes; not measured) | the same |
| io_uring | **not used**: our seccomp allowlist refuses `io_uring_*` (EPERM, `vmsyscalls.h`), and loosening it is a sandbox change for the maintainer to approve | the same |

The firmware, the installer's and the drivers' discs and the seed disk keep the plain file node. Snapshots with memory
(take, revert while running, delete) were checked on a running machine with the I/O thread and `cache.direct` (QEMU 10.0.13).

**Measured** (2026-11-23, Kingston NV3 NVMe, ext4, i9-10900; a throwaway guest of the host's kernel whose init writes
and reads `/dev/vda` with `O_DIRECT` (no fio in a guest without network); a fresh 8 GB qcow2 opened with the same
`-blockdev` options lord-vm gives; 4 vCPUs; two runs each, which agreed within 10 %):

| | Today (page cache, threads, main loop) | + I/O thread | + I/O thread, `cache.direct`, native AIO (new default) | + I/O thread, `cache.direct`, threads |
|---|---|---|---|---|
| Sequential write, 1 MB | 1.59 GB/s | 1.56 GB/s | **2.0–2.6 GB/s** | 2.3–2.6 GB/s |
| Sequential read, 1 MB, data not in the host's cache | 1.62 GB/s | 1.62 GB/s | **2.46–2.53 GB/s** | 2.47–2.49 GB/s |
| Random read 4 KB, 1 at a time / 4 at a time, data in the host's cache | 59.5k / 190k IOPS | **104k / 250k** | — | — |
| Random read 4 KB, 1 / 4 at a time, from the disk | — | — | 13.6–14.4k / 60k | 13.5k / 55k |
| Random write 4 KB, 1 / 4 at a time | 55k / 176k (into the host's cache) | **93k / 232k** | 35k / 103k (to the disk) | 35k / 138k |

What it means: the I/O thread is a gain everywhere (+60–75 % at one request at a time, +25–35 % at four). Bypassing
the host's cache gives 55–60 % more sequential throughput and keeps a machine's disk from filling the host's memory with
a second copy of what the guest already caches; random requests then go to the disk at its own speed (a 4 KB read in
~70 µs on this NVMe), where before a working set that still sat in the host's cache came back from memory. A guest's
own cache (its memory) serves those repeats first, so the second copy mostly helped a guest that restarts.

**Host cache or not: measured with real work, decided** (2026-11-23; the maintainer felt no difference and asked for
numbers). A Debian 13 cloud guest (4 vCPUs, 4 GB, UEFI, a 40 GB qcow2 on the same NVMe and ext4, `virtio-blk` on the
I/O thread, the `-blockdev` options lord-vm gives) ran one sequence per start, each on a fresh copy of the same disk
with the disk file dropped from the host's cache first: boot, unpack the Linux 6.12 sources (86,618 files), install
GIMP and Inkscape from the local package cache (`apt-get install --no-download`, 456 packages, dpkg's fsyncs), build
`kernel/ mm/ fs/ lib/` of that tree (`make -j4`), write a 6 GB file and read it at random (fio, `O_DIRECT` in the guest,
so the guest's own cache doesn't serve it: a working set larger than the guest's memory that fits in the host's), copy
that file with the guest's cache dropped, power off and boot again. (a) the host's cache (`cache.direct` off, QEMU's
thread pool) against (b) `cache.direct` with native AIO, 4 runs each, alternating which went first; medians (range):

| Work | (a) host cache | (b) `cache.direct` + native AIO | (b) against (a) |
|---|---|---|---|
| Boot to `systemctl is-system-running`, host cache empty | 5.06 s (5.05–5.28) | 5.25 s (5.03–5.31) | +4 % |
| Boot again right after (host cache warm in (a)) | 4.86 s (4.85–5.17) | 5.17 s (5.06–5.28) | +6 % (0.3 s) |
| Unpack 86,618 files + `sync` | 3.74 s (3.60–3.83) | **3.24 s** (3.19–3.39) | −13 % |
| Install 456 packages + `sync` | 29.2 s (28.9–30.7) | **27.4 s** (26.7–28.2) | −6 % |
| Compile (`make -j4`) | 78.4 s (78.0–80.8) | 78.2 s (75.4–80.2) | the same |
| Write 6 GB, 1 MB requests | 4.64 s (4.38–5.89) | **2.63 s** (2.62–2.64) | −43 % |
| Random 4 KB reads, 1 at a time, over those 6 GB | **74.0k IOPS** | 14.7k IOPS | ÷5 |
| Random 4 KB reads, 16 at a time | **359k IOPS** | 170k IOPS | ÷2.1 |
| Copy the 6 GB file + `sync` | 5.23 s (5.18–8.51) | 5.45 s (5.33–5.49) | +4 % |
| The disk file in the host's page cache: after boot / unpack / install / compile / fio / copy | 0.14 / 2.0 / 3.4 / 3.6 / 9.7 / **15.9 GB** | 0 | — |

**Decision: keep `cache.direct` with native AIO on ext4 and XFS for every disk, with no size or memory rule.** In the
work a person does (boot, installs, unpacking, builds, copies) the two are within ±6 %, with (b) ahead where it writes
(installs, unpacking, large writes) and (a) ahead by 0.2–0.3 s at boot and 4 % on a copy, which nobody notices,
matching what the maintainer felt. The one clear win of the host's cache is random reads of data the guest's own memory
can't hold but the host's can (5× one at a time); that same data in the guest's memory is faster still, so the fix
for such a guest is more memory, not a second copy. The price of (a) is large and grows with use: a 4 GB guest left
15.9 GB of its disk in the host's page cache after this sequence, four times its own memory, pushing out the
desktop's and the apps' cache, and on a 16 GB computer that is most of it. A rule by disk size or free memory was
considered and not built: the cache grows with what the guest touches, not with the disk's size, and a computer
with memory to spare gains only in the random-read case above. Not tried with a Windows guest (no scratch Windows
machine on this computer); Windows caches its disk in its own memory the same way. Nothing changes in lord-vm.

### 6.5 Network

| Mode | How | Reach | Notes |
|---|---|---|---|
| **Shared** (default) | `passt` user-space, no root | Guest reaches the world, world doesn't reach guest | Port forwarding by rules |
| **Bridged** | a macvtap on the wired card (`vm-net-bridge`), opened by lord-vm and passed as a descriptor | Guest on the LAN with its own address | Ethernet. Wi-Fi can't bridge: says so |
| **Host only** | `passt` with `-4 -o 127.0.0.1` (built: the host only, not other VMs) | This computer | No internet |
| **Isolated** | no NIC | Nothing | Testing |

Debian 13 has `passt` (**Measured**, 2025-05-03 snapshot). Phase 0 compared it with QEMU's `-netdev user`, with the guest
downloading 500 MB from a server on the host (**Measured**, 2026-11-04, medians of 9 runs):

| Host | `-netdev user` | `passt` | CPU, user / passt |
|---|---|---|---|
| Pi 5 | 234.5 MB/s (221–244) | 222.9 MB/s (96–379) | QEMU 249 % / QEMU 160 % + passt 80 % |
| amd64, **nested** | 252.3 MB/s (209–284) | **817.6 MB/s** (557–1005) | QEMU 266 % / QEMU 253 % + passt 61 % |

**Decision (confirmed, open question 7 closed):** `passt` stays the default for Shared. It's 3.2× faster on amd64, about equal on the
Pi, and it is the process the firewall can name as the machine. On the Pi its speed varies far more from run to run, which Phase 1
looks at. QEMU 10.0 has no `-netdev passt`, so it's connected through `-netdev stream` on a socketpair (3.3). Bridged runs with `vhost=off` (QEMU opens no device), not `vhost-net`. `passt` runs as the machine's own process, in its scope, because it's the one opening the connections: the firewall has to see **that** as the machine, as it does for `desktop:lordos-rdesk`. Centinela sees each VM as an app (`desktop:lordos-vm-<id>`, **To test**): its traffic in Shared mode appears in the firewall, the map and Monitor as "Machine: name". A rule can block it. Bridged traffic bypasses this: Security says so.

Each forwarded port has a scope: **Only this computer** by default, or **Your network**, which is a firewall rule the person sees.

**Built (0.8.9, approved by the maintainer on 2026-11-20).** machine.json schema 6 (`server/src/vm/machine.{h,c}`): `network` is
`shared`, `isolated`, `bridged` or `hostonly`; `bridge` names a wired card; `ports` is a list of at most 32 rules
`{ "proto": "tcp"|"udp", "host": 1024–65535, "guest": 1–65535, "open": "this-computer"|"lan" }`, one per protocol and host port
(`vm_ports_check`). Older files load unchanged (since then files are written at the current schema, 12). Configure → Network
shows the four modes, the card and the port list; every change needs the machine off (`lord-vm --set`, which holds its lock).

- **Forwarded ports (Shared and Host-only).** passt's own options, one per rule: `-t 127.0.0.1/H:G` (only this computer) or
  `-t 0.0.0.0/H:G` (the network, Shared only; a host-only machine never listens beyond loopback, whatever the file says), the same
  with `-u`. No rule: `-t none -u none` as before. passt binds them in the machine's scope, so `lord-netperm`'s bind programs decide
  them as incoming connections of `lordos:machines` (FIREWALL.md, "Machines' downloads"): with "no incoming connections" for
  Machines the bind is refused. Before passt starts, lord-vm binds and lets go of each port itself (`ports_free`), so a port in use
  or a refusal by the firewall fails the start with a clear line instead of passt dying and the network's healing trying again.
  passt has no Landlock of ours (3.3) and binds nothing else; QEMU still can't bind or connect (Landlock net + seccomp).
- **Host-only.** Also passt, with no root and no device: `-4 -o 127.0.0.1 -a 10.0.2.15 -n 24 -g 10.0.2.2 --no-dhcp-dns
  --no-dhcp-search`. Every socket passt opens for the guest is bound to 127.0.0.1, and the kernel refuses a loopback source towards
  any address that isn't this computer's (EINVAL, **Measured** 2026-11-20); `-4` keeps IPv6 sockets, which `-o` wouldn't bind, out.
  The gateway 10.0.2.2 is this computer's loopback (passt's default host mapping). Chosen over a root-made bridge without
  forwarding because it needs no admin action, no device and no system firewall rule, and the guest's traffic stays sockets of the
  machine's scope that the firewall sees. The cost, said in Configure: the guest also reaches what this computer serves only for
  itself on 127.0.0.1, and this computer reaches the guest only through forwarded ports.
- **Bridged.** A macvtap in bridge mode on a physical wired card, made by the closed admin action `vm-net-bridge "<id> <card>
  <mac>"` (`server/src/vm/vmnet_admin.c`, our own netlink): checked on the root side (`vmnet_card`: exists, has a device, is
  Ethernet, isn't Wi-Fi, a bridge, a bond or anything virtual, isn't a bridge or bond port), named `lvm` + 12 hex digits of the
  machine's id, with the machine's MAC (unicast, never the card's own), the alias `lordos-vm <uid> <id>` (only root can write one),
  no address of this computer's on it (IPv6 address generation off before it goes up), and `/dev/tap<index>` given to the caller,
  0600, checked by its device number and changed through an `O_PATH` descriptor. Machines runs it just before a start; lord-vm
  checks the interface (its lower card, its MAC, the device's number and owner), opens the device and QEMU gets
  `-netdev tap,fd=6,vhost=off`. `vm-net-unbridge "<id>"` removes the caller's own, never while a process holds the device; lord-server
  asks for it when the library sees the machine off (not within 30 s of a start), and the admin helper sweeps the caller's unused
  ones when it starts and ends. They never survive a restart. Over Wi-Fi a macvtap can't work (an access point takes one address per
  station), so Wi-Fi cards are listed but refused (`vm.bridgeWifi`). This computer can't reach its own guest over a macvtap, and the
  guest's frames are not sockets, so LordOS's firewall doesn't see them: Configure says both. Administrators only.

Verified on the development PC (2026-11-20, a copy of an Ubuntu 26.04 machine, headless, the probes run in the guest by a systemd
unit passed as credentials and read from its journal through the guest agent): Shared with three rules, `curl 127.0.0.1:18080`
and `172.17.0.5:18081` got the guest's page, a UDP echo through 19000 answered, a rule on loopback wasn't reachable on the LAN
address, internet reachable, host loopback not; a port in use failed the start with its line. Host-only: the guest got 10.0.2.15,
1.1.1.1:80, 9.9.9.9:443 and the LAN's gateway refused, 10.0.2.2:18999 and the host's LAN address reached, no DNS, the forwarded
port reached the guest. Bridged on `enp3s0`: the guest got 172.17.0.11/24 from the LAN's DHCP server, reached the internet, not
this computer (no route to host), as expected; `vm-net-unbridge` refused while it ran, the sweep removed the macvtap after, and a
start without it failed with its line. Not tested: a real "no incoming connections" rule for Machines, a second LAN device reaching
a "lan" port or the bridged guest, the Configure pane on screen.

### 6.6 USB

**Not built (audited 2026-11-23):** no USB passthrough, no usbredir and no `vm-usb-allow` exist; machines have `qemu-xhci` with a `usb-tablet` only for the Windows devices. The design: `usbredir` over the Chardev stream: no root, device opened by a closed admin action `vm-usb-allow`, which claims the single device for that VM and writes nothing permanent. Menu lists devices with names. Auto-connect rules per VM. USB 3 through `qemu-xhci`. Weak: isochronous devices (webcams, audio) can stutter; Camera and Microphone have their own paths. Never offered: the boot disk or the keyboard in use.

### 6.7 Shared folders

`virtiofsd` 1.13.2 (Debian, **Measured**), one per folder, in a sandbox of its own that can reach only that folder. **QEMU never gets the folder**: it holds a vhost-user socket, so a broken QEMU still can't open your files. The reverse needs saying: vhost-user requires guest memory to be shared, so a broken `virtiofsd` can read that guest's memory, and only that guest's. Machines with no shared folder don't run it, and don't pay the shared-memory backend. Linux guests mount it natively. virtio-win guests use the virtio-win virtiofs driver plus WinFsp (a separate dependency to review). Read-only by default, as in Remote Desk. Fallback: a small SMB share reachable only by that guest.

**Built (0.8.8.7).** `machine.json` schema 4 `shares`: up to 4 `{ tag, path, writable }`. `lord-vm --set DIR --share
ro|rw:TAG:FOLDER` (repeated; `--shares none` empties the list), Configure → Sharing (Add a Folder, Writable, Copy Command,
Remove) and the bar's folder button (each folder opens in Files; the mount command is copied). Which folders
(`vm_share_allowed`, checked on `--set` and again at every start): a real directory inside home but not home itself, or under
`/media/<user>` or `/mnt`; nothing hidden; never the library or what holds it. At start `lord-vm` binds a listening socket in
the machine's run folder, opens the folder once without following a link at any step (`O_PATH` per component, its path read
back from the descriptor must match), and starts `virtiofsd --fd=3 --shared-dir=/proc/self/fd/4 --sandbox=namespace
--seccomp=kill --cache=auto --migration-mode=find-paths [--readonly]` on that socket and that descriptor, so a folder of
the path swapped for a link after the check changes nothing (a cgroup leaf of 256 MB, two CPUs; its log in `log/virtiofsd-N.log`),
connects, removes the path and gives QEMU the connected socket (`-chardev socket,fd=`; QEMU can't make sockets). With a share,
the guest's memory is shared (`memory-backend-ram,share=on`, a memfd; `memory-backend-memfd` before 0.8.9.6, 3.7.2); without one nothing changes. Each folder is a `vhost-user-fs-pci` on
slots 0xb–0xe. Suspend and snapshots with memory work: virtiofsd hands its state over (`find-paths`), and the shares can't
change while a state is saved. A snapshot never rolls back the host's folder: on revert the guest sees the folder as it is
now. The package `virtiofsd` comes with "What Machines Needs" (`vm-enable`), like passt. In the guest: `mount -t virtiofs
TAG /mnt/TAG`; the transfer folder (6.7.1) is mounted by itself through the agent.

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

### 6.7.1 The transfer folder

**Built (since 0.8.9.5; maintainer's goal 2026-11-20: moving files between this computer and a machine must
be near-instant, both ways).** Every machine can have one folder of this computer that is only for moving files:
`<library>/Transfer/<machine folder's name without .lordvm>`, so `~/Machines/Transfer/Ubuntu 26.04/`, shown in Files like any
folder. It's a shared folder with the fixed tag `LordOS`, writable, served by its own virtiofsd exactly as 6.7's (same arguments,
same sandbox, same cgroup leaf, the descriptor and not the path), and its device is always on PCI slot `0xf` whatever the
person's own shares are.

- **On for new machines** (`vm_machine_defaults`: create, import), switchable in Configure → Sharing ("Transfer folder") and with
  `lord-vm --set DIR --transfer on|off`. `machine.json` schema 7 `"transfer"`; a file without it reads as off, so a machine made
  before keeps its hardware. Like the shares it is a device: it changes only with the machine off and never while a state is
  saved (`--set` refuses); `suspend.json` records whether the saved state had the device, and a resume uses that, not
  `machine.json`. Without virtiofsd installed a machine starts without it (logged) instead of failing. A person's share can't use
  the tag `LordOS` while the transfer folder is on (`vm_machine_check`; Configure picks another tag).
- **Where:** `vm_transfer_path` (machine.c). At each start `lord-vm` makes `<library>/Transfer` (0700) and the machine's folder
  in it (0777: virtiofsd can't take the guest's uids, so this lets a guest person with another uid than this account's write
  there; nothing outside the 0700 parent reaches it), then checks both are real folders of this account and opens the folder
  without following a link at any step (`open_share`).
- **Follows its machine's folder** (6.26, in development): a rename with the machine off moves `Transfer/<old>` to `Transfer/<new>` with
  its owner record. One left behind (a machine folder renamed by hand, a rename stopped between its two steps) joins its
  machine at the next claim (`vm_transfer_adopt`, run first by `vm_transfer_claim`): exactly one folder whose owner
  record holds the machine's id and whose own `<x>.lordvm` is gone, never one a Duplicate (same id) still has, never one
  put aside as `(old YYYY-MM-DD)`, never a guess between two.
- **Tied to its machine** (`vm_transfer_claim`, security review 2026-11-20): the folder's name is readable, but an owner record
  beside it, `<library>/Transfer/.<name>.machine` (outside what the guest sees), holds the machine's id. A folder of that name
  owned by another machine (one deleted, renamed or made again with the same name), or owned by none and not empty, or not a
  real folder, is moved aside to `<name> (old YYYY-MM-DD)` (then ` 2`, ` 3`…) with its record, never deleted, and the machine
  gets a new one. Removing a machine (to the Trash) moves its own folder aside the same way (`lord.vm.transferAside`), so a new
  machine of that name never starts with another's files. `lord.vm.transferFolder(dir)` makes it the same way for Open Transfer
  Folder before the first start. It is inside the library, which a person's share may never be (`vm_share_allowed`): this one
  fixed path is the exception, and it holds no machine folder (the library lists only `<name>.lordvm` directly in it), so the
  guest can't reach a `machine.json`. lord-server refuses a start when the folder is one `fsops_protected` covers, as for a
  writable share.
- **Cost:** the shared memory backend (6.7) for every machine that has it: the guest's memory counts in full once touched.
- **Linux guests:** at each connection of the agent one fixed script, as root, mounts the tag at `/mnt/LordOS` (root's own
  folder: never a path in a person's home) with `nosuid,nodev`, unless it's mounted already, then as each person with a home
  (`runuser`) adds `~/LordOS`, a link to it, when that name is free. It prints "mounted": the agent's state gets `VAC_TRANSFER`.
- **Windows guests:** the virtio-win guest tools install the VirtIO-FS service (`VirtioFsSvc`, the virtiofs driver's user-mode
  half), which needs WinFsp. WinFsp is a pinned catalog download (`assets/machines/systems.json` "winfsp": 2.1, the project's own
  GitHub release, SHA-256 and size pinned, GPLv3 with a FLOSS exception) through `vm-image-get`, kept in the image store like the
  drivers' disc. At each connection the agent says `nosvc`, `nowinfsp` (`VAC_XFER_SETUP`) or, with WinFsp there, sets the service
  to start by itself, starts it and says `mounted`. **Set Up the Transfer Folder** (the folder menu, the agent's popover, the
  Actions menu; by itself once after an unattended install, `app.json` `transferSetup`) downloads the installer if needed,
  lord-server opens only `/var/lib/lordos/machines/images/<sha256>.iso` whose root-owned record says `winfsp` and hands the
  descriptor and the hash to `lord-vm` (`VAG_TRANSFER`), which hashes it again, writes it into a SYSTEM-only private folder
  through the agent (the drops' way), and one fixed script as SYSTEM checks its SHA-256 and Authenticode signature, runs
  `msiexec /i … /qn /norestart` there, sets VirtioFsSvc to start by itself and starts it. The folder is a drive (Z: on the
  machines tested), visible in the signed-in person's session too.
- **Drops (6.21) go through it** when the guest has it mounted: `lord-vm` copies the file lord-server opened into the transfer
  folder itself, in a child process so its loop never waits (a reflink, `FICLONE`, where the file system can; else
  `copy_file_range`, which on one file system never passes through user space; else read and write), into a folder it makes
  for that drop (`.lordos-drop-<16 hex>`, 0700, holding `file`, made `O_EXCL | O_NOFOLLOW` through the folder's descriptors:
  the guest can write the transfer folder, so nothing there is followed or trusted), then hashes the person's file (SHA-256).
  Not a hard link: the guest could then write the person's original. The person's copy in the guest is written to a hidden
  file in their Downloads and gets its name only when its size and SHA-256 are the ones `lord-vm` measured; otherwise it's
  deleted and the agent's way follows. On Linux root only checks that `/mnt/LordOS` is mounted and, as the person, a fixed
  script checks the drop's folder and file aren't links, copies and hashes in one pass (`tee | sha256sum`), and names it. On
  Windows SYSTEM finds the transfer drive itself and never by a name a guest could plant: only a global drive (no person can
  make one) labelled `LordOS` whose drop folder, not a reparse point, holds `token`, a random value `lord-vm` wrote there and
  gives SYSTEM in its script; the person's own scheduled task (the drops' mechanism, 6.21) gets that exact path, checks it and
  its folder aren't reparse points, and copies and hashes it (`IncrementalHash`). Any failure tries the agent's way once more
  and looks at the guest's folder again; `lord-vm` removes the drop's folder whatever happens.
- **Who in the guest sees a drop while it passes:** on Linux the drop's folder is 0700 and the guest's kernel checks it: only
  root and the account with this computer's uid (1000 on most systems: usually the person) can enter it (checked 2026-11-20:
  another account gets "Permission denied" for the folder and the file; it can still rename the folder away, which only
  sends the drop the agent's way). On Windows the VirtIO-FS drive gives Everyone full control (its ACL is
  `O:WDG:WDD:P(A;;FA;;;WD)`, checked 2026-11-20): every account of the guest can read a dropped file during the seconds it
  passes through the transfer folder, and change it (which the hash check catches). Machines used by several people should
  drop files through the agent: turn the transfer folder off in Configure. The window's hint says
  which way the files will go, the progress line says "through the transfer folder".
- **Out of the guest:** what the guest saves in its LordOS folder or drive is in the transfer folder here at once (Open Transfer
  Folder: the folder menu, the Actions menu, the library's menu). Dragging out of the guest's window still needs a helper inside
  it: not built.
- Nothing in LordOS opens, runs or indexes the transfer folder by itself: it's a folder of the person's like Downloads.

**Measured on 2026-11-20** (amd64 host, NVMe, ext4: no reflink, so `copy_file_range`; KVM; 2 GB random file; through
`lordvm-dev`, which plays lord-server's part, not the real lord-server window; times from the request to the guest's answer):

| | Agent's way (before) | Transfer folder |
|---|---|---|
| Ubuntu 26.04 guest, drop into Downloads | about 26–30 MB/s (an 8.7 GB ISO took minutes) | 17.6 s, about 120 MB/s with the hash check (host copy and SHA-256 8.5 s, the person's copy and hash in the guest 9.2 s); 4.8 s before the check was added |
| Windows 11 guest, drop into Downloads | about 26–30 MB/s (78–80 s for this file, measured the same day) | 25.6 s, about 84 MB/s with the hash check (host 8.5 s; the person's task about 17 s, of which about 9 s is the fixed cost of the scheduled task and PowerShell, the same on the agent's way) |
| Ubuntu guest → this computer (`cp` into ~/LordOS) | not possible | 2.4 s, about 880 MB/s, the file whole on the host when the copy returns |
| Windows guest → this computer (`copy` to Z:) | not possible | 2.5 s, about 860 MB/s |
| WinFsp set up in a Windows guest | — | 8 s (installer copied, checked, installed, service started) |

Also checked: a machine suspended with the transfer folder resumes with it (suspend.json `"transfer": true`) and mounts it again;
with the guest's mount gone, a drop falls back to the agent's way and the guest's folder is mounted again; a copy whose size or
hash differs is deleted and named nothing (the Linux script run against a wrong hash and a wrong size, the Windows script as
SYSTEM); a folder of the same name left by an earlier machine moved aside at the start. The hash costs time: this computer's
SHA-256 (portable C, no SHA extensions on this CPU) runs at about 280 MB/s; running it while the guest copies (a second step
that names the guest's copy) would bring a 2 GB drop to about 10 s on Linux: not done yet.

### 6.8 Clipboard and drag

Clipboard through the agent's own channel, `org.lordos.agent` (text, rich text, pictures), direction chosen per VM, **Off for Testing**. Protected windows (`wm.setProtected`) never leak. File drag: into the guest copies through the transfer folder or the agent (6.21); out of the guest is not built. Linux: our `lordos-guest`. virtio-win guests: our `lordos-guest-win` (7.1.2).

**Only while the machine is in use (0.8.9, the guest is untrusted: clipboard stealers, address swappers).** The machine
counts as in use while its window is in front, or, in apps mode, one of its guest's windows, and for 3 seconds after it
goes to the background (a paste just after switching still works). lord-server holds that gate (`js_vm.c`,
`lord.vm.clipboardGate`, set by the app from the window's focus and from a click on the picture); lord-vm can't see the
windows, so it never decides it.

- **Host → guest, on demand.** When the person turns to the machine (its window comes to the front, a click on it) the
  guest gets an *offer*: which forms the host's clipboard holds (text, rich text, picture) and a generation number,
  no contents (`AGK_CLIPOFFER`). The guest's helper puts those forms on its own clipboard without data. Only when a
  program in the guest pastes does the helper ask for that form (`AGK_CLIPREQ`); lord-vm checks that the request is for
  the last offer and one of its forms (and at most 8 a second), and lord-server answers with the data
  (`AGK_CLIPDATA`) only if the machine is in use now and the host's clipboard is still what was offered; otherwise the
  paste gets nothing. A secret (Passwords, a password field) is never offered.
  - Windows (`lordos-guest-win` w2): delayed rendering (`SetClipboardData(format, NULL)`, `WM_RENDERFORMAT`), with the
    offer marked so Windows' clipboard history and cloud clipboard skip it. The helper waits 2 s for the answer. When it
    exits (`WM_RENDERALLFORMATS`) it empties the clipboard instead of fetching what nobody pasted.
  - Linux (`lordos-guest` 5 and later): the helper owns the X `CLIPBOARD` selection itself (`tools/guest/x11clip.c`: the X11
    protocol over the display's Unix socket with the session's MIT-MAGIC-COOKIE-1, no Xlib, no tool), so it works on X11
    and through Xwayland, which GNOME, KDE and wlroots desktops bridge to the Wayland clipboard. Large data goes as
    INCR. A guest with no X display (a Wayland session without Xwayland) and older helpers keep the push: the whole
    clipboard goes in at the same moments the offer would (window to the front, a click), never in the background.
- **Guest → host, gated.** A guest's clipboard change is put on the host's clipboard only while the machine is in use.
  A change in the background is ignored and written in the machine's log.
- A guest that asks for the clipboard, or changes it, while it isn't in use gets a notification once per burst
  ("‹machine› tried to read your clipboard in the background: blocked"; "… change … ignored").

**Ask before each paste** (Configure → Sharing → Clipboard trust; machine.json schema 9 `"clipboard_ask": true`,
`lord-vm --set DIR --clipboard-ask on|off`; "Trusted" is the default, the behaviour above). Then, on top of the gate:

- A paste in the machine (a request that passed the gate) waits for the person: a bar over the machine's picture (in apps
  mode, over the guest window in front) says "Paste into ‹machine›?" with a one-line preview (the first 80 characters of
  the text, rich text's own text, or "A picture, W×H"; a secret is never offered, so never shown) and [Don't paste]
  [Paste]. One answer covers every request of the same offer (a program that asks for several forms of one copy is
  asked once). lord-server holds the requests (`clipboardAnswer`, expiring after 30 s); the bar's question and the guest's helper wait
  25 s and then take no answer as a no (`AGO_ASK` in the offer) and then gives the program nothing. With a helper that can't ask (the push
  fallback) the bar asks before the clipboard is pushed, at the same moments.
- A copy in the machine isn't put on this computer's clipboard at once: the bar says "‹machine› copied" with its preview
  and [Ignore] [Bring to LordOS]; ignored after 60 s.
- While the bar shows, the keyboard is the bar's: Esc says no; Enter says yes only on the focused button (Tab reaches
  them). The bar's logic (slots, the dedupe per offer, the time limits) is `apps/machines/model.js` `clipAsk*`.

(Historical, done:) the clipboard went to the agent's own channel, `org.lordos.agent`: text, images and files, with the direction chosen for that machine. It's a private data channel inside the machine, so it needs no network, no port and no VNC. WebRTC's data channel solves reaching another computer, and 6.23 uses it there; inside one computer a virtio-serial port does the same job with no encryption layer to carry, since nothing leaves the host.

### 6.9 Sound, camera, microphone

Sound via `audio-dbus`, into PipeWire by `lord-server`. Microphone and camera: asked once per VM with the existing Permissions flow; camera through USB redirect or `uvc` emulation (camera not built). Off by default. Built: the speakers and the microphone through lord-vmdisplay (`hda-micro`, `sound_out`, `sound_in`).

**Built (since 0.8.9.5): speakers and microphone.**

- *The card.* Every machine has an HDA controller with one `hda-micro` codec (a speaker and a microphone), on every machine and
  whatever its settings, so the guest's hardware never changes with them and snapshots and saved states keep loading. Machines made
  before had `hda-output`: a state they saved loads into `hda-micro` (**Measured**, 2026-11-20: suspended by the previous build, resumed
  by this one, sound playing after). The other way round does not: an older build can't resume a state saved by this one (QEMU stops on
  an assertion while loading), but an older build can't open a schema 6 machine anyway.
- *Speakers.* lord-vmdisplay registers an `AudioOutListener` and an `AudioInListener`, each on a peer connection of its own, before
  its sandbox. It applies the guest's own mute and volume (QEMU leaves them to the listener, 0..255 a channel) and copies the PCM
  (48 kHz, 16-bit stereo) into a ring lord-server made (`VD_AUDIO_OUT`, `media/aring.h`), never more than 80 ms ahead of what
  lord-server read: past that a piece is dropped, never waited for. lord-server plays the ring from its own PipeWire stream
  (`media/maudio.c`, a 60 ms target buffer; PipeWire granted 50 ms), named after the machine, corked while the guest plays nothing
  (`VD_GUEST_AUDIO`). lord-vmdisplay never gets the PipeWire socket.
  When PipeWire isn't reachable as the stream opens (a machine window opened early in the session) or goes away, the stream
  connects again every 2 s while the guest plays (and the microphone's while it's open), with the same ring
  (`maudio_retry`): the helper is told nothing and what waited in the ring is dropped, not played late. Only the first failure
  is logged ("media: no PipeWire to play"), then "media: PipeWire is there now (play, after N tries)". **Measured** 2026-11-21
  with `lordvm-dev` (which does the same) and a private PipeWire with WirePlumber started 20 s after the guest's `speaker-test`
  began: back after 4 tries, then 192 000 bytes a second taken; and again after that PipeWire was stopped and started.
- *Microphone.* QEMU asks the `AudioInListener` with `Read` from its main loop and waits, so the helper answers at once: the frames the
  time owes at 48 kHz (at most 100 ms an answer; a paused machine doesn't get a burst after), from lord-server's microphone ring
  (`VD_MIC`) when there is one, else silence, with the guest's own capture mute and level applied. lord-server opens the microphone
  (its own capture stream) only while three things hold: the machine's `microphone` is on, LordOS's microphone permission allows
  Machines (`perm_get("machines", PERM_MICROPHONE)` and the restrictions, checked in C in `js_vm.c`, not only by the app), and the
  guest records now. The app asks for the permission with the system prompt the first time the guest records with the setting on; while
  the microphone is open the bar shows it and the menu bar's microphone indicator lights.
- *Settings.* machine.json schema 6: `sound` (true), `volume` (0..100, 100), `microphone` (false); an older file reads as those. They
  are host-side and change while the machine runs: through `VM_SOUND` to its lord-vm, which writes the file (it holds the lock), or
  `lord-vm --set --sound --volume --microphone` with it off. Configure → Sound and input has them; the bar's speaker mutes and unmutes.
- *Output and input (schema 8).* `sound_out` and `sound_in`: the output the guest's sound plays on and the input its microphone
  hears, each a PipeWire `node.name` (1 to 96 printable ASCII characters, no spaces: `vm_sound_dev_valid`), absent for the system's
  default, which follows Settings → Sound. Configure → Sound and input offers "System default" and every output and microphone
  Settings → Sound lists (the same `lord.sound` state, by its description); the window's speaker button is a menu with mute, the
  outputs and the inputs. Changed while running through `VM_SOUND` (`d = 1`, text `OUT\tIN`, `vm_sound_devs_parse`), or
  `lord-vm --set DIR --sound-out NODE|default --sound-in NODE|default` with it off. lord-server applies them at once: the stream
  connects again to that device with the same ring (`maudio_target`, as `maudio_retry`), created with the device's name, which
  pipewire-pulse turns into `target.object`; "default" connects without one, so the stream follows the default. At every poll
  lord-server uses the chosen device only while its sound client lists it (`snd_device_present`): unplugged, the stream goes to
  the default (the menu says "not connected"); plugged back, it moves there again. **Measured** 2026-11-21 on PipeWire 1.4.2 with
  `maudio.c` and a silent stream: moved live between two outputs (`pw-dump`: linked to each, `target.object` set), back to the
  default (no `target.object`), a name that isn't there played on the default, and a capture stream moved between two microphones.
  *Not checked:* the menus on screen and a running guest's sound while it moves.
- *Checked (**Measured**, 2026-11-20, amd64, a clone of an Ubuntu 26.04 guest, with `lordvm-dev`, which plays lord-server's part with
  lord-server's own `maudio.c` and a PipeWire stream at volume 0):* GNOME's screen reader at the login screen and `speaker-test`
  reached the stream at the real rate (192 000 bytes a second), every byte the helper wrote was taken (0 dropped), the ring stayed
  empty between PipeWire's requests; the guest's `arecord` of the microphone ring's 440 Hz tone measured 440 Hz at the level sent,
  half of it with the guest's capture at 50 %, silence with its capture off or without a ring, always the full length (2 s took 2 s of
  frames). *Not checked:* the whole path inside a running lord-server (the window, Configure, the permission prompt and the
  indicator) on screen; a Windows guest.

### 6.10 Graphics

| Guest | Host GPU | Result |
|---|---|---|
| Linux | Any Mesa GPU | virgl: OpenGL; venus: Vulkan, needs blob resources, guest kernel ≥ 5.16 (**Reported**, Mesa docs) |
| Linux | Pi V3D | virgl: QEMU 10.0.13 aborts on every UEFI boot with `gl=on` (3.2), works only with a directly booted kernel, picture not yet checked (**Measured**, 2026-11-04); venus **To test**: V3D's Vulkan features may not be enough |
| virtio-win | Any | 2D only: no good free 3D driver exists (Configure says so; machine.json refuses `3d` with the windows devices) |
| Any amd64 UEFI | Intel GVT-g iGPU not drawing the desktop | the vendor's driver on a vGPU (6.10.1) |
| Emulated | Any | 2D |

Reported notes: Intel Meteor Lake or `xe` hosts need kernel 6.16+ and QEMU 11.0+ with `honor-guest-pat=on` for venus; Debian 13 has 10.0 (**Measured**), so venus on those hosts waits for a newer QEMU. NVIDIA proprietary needs 570.86+ for venus (**Reported**). Recent security fixes around virtio-gpu blob scanouts are a reason to keep 3D opt-in. In the sandbox, 3D gives QEMU a render node: a larger attack surface, stated in Security. `vhost-user-gpu` would put rendering in its own process: to evaluate in Phase 3.

**Built (2026-11-20): virgl for Linux guests on amd64** (3.2, "3D graphics (built)"): Graphics 2D or 3D in Configure → Display, 3D
for Linux machines only, the Productivity, Games and Design profiles pick it where they can.

**Decision (changed): 3D isn't offered on arm64 hosts with Debian 13's QEMU** until the reset abort of 3.2 is fixed there, by Debian, by
a patch of ours (open decision 5) or upstream. The fault is guest-triggerable, so a direct-kernel-boot workaround isn't enough to ship it.

**Known issue (2026-11-23, its handling in development): the same abort on amd64 at a Linux guest's shutdown or restart.** Linux guests with Debian's kernel
6.12.117 or later reset virtio-gpu-gl from a vCPU on every shutdown and restart (`virtio_dev_shutdown`), which is the abort above
(QEMU issue 4918, open; 3.2, "3D graphics (built)", has the code path). An unattended install's start therefore always has 2D
graphics. A 3D machine whose guest shuts down or restarts reads as stopped, and a restart the LordOS tools announce starts
it again (3.2, `vmlog_3d_abort_end`); an abort not tied to a stop says so and offers **Use 2D Graphics and Start**.

#### 6.10.1 A real graphics card: Intel GVT-g (built 2026-11-21, since 0.8.9.5)

A machine (amd64, UEFI; Windows checked, a Linux guest not yet; its Graphics must be 2D, `machine.c`) can get a share of an Intel iGPU of the GVT-g generations (Broadwell to
Comet Lake) that doesn't draw the desktop: the guest runs the vendor's own driver (Direct3D 12, OpenGL, Vulkan) on the
real iGPU, and the picture comes back as dmabufs through the D-Bus display like a virgl machine's. Settings → Graphics
cards turns the iGPU's GVT-g on (a restart); Configure → Display → Graphics card picks None, Whole or Shared
(machine.json schema 11 `"gpu"`). Such a machine can't be suspended or keep snapshots with memory, and every reset
ends QEMU and the machine starts again. Design, admin actions, sandbox and what was checked live:
[MACHINES-GPU.md](MACHINES-GPU.md) "Mode A — built"; the rules: AGENTS.md "Machines' graphics cards".

### 6.11 Migrate from another PC

A small agent runs on the old PC, or the disk is read from a backup image, or the disk is connected. Copies the disk over LAN to a new VM and injects virtio drivers so that it boots. Weak: driver injection into a virtio-win guest is delicate; activation may need the person's attention; full-disk encryption needs the recovery key; arm64 hosts can't run an amd64 migrated system usefully, and the card says so. Phase 4.

**What works today (checked 2026-11-24, in development): a backup image of a Linux PC's disk.** A whole-disk image as
`dd` or a backup tool writes it (raw: its partition table, EFI partition and boot loader) is a disk the import of 6.22
takes (`machines import old.img "Old PC"`, or the assistant's Migrate tile): it becomes a qcow2 machine with the
compatible devices (SATA disk, e1000e), as the PC had, and boots, because Linux carries its drivers. Checked with a 20 GB
raw image of an installed Ubuntu 26.04.1 (UEFI, GPT): imported in 38 s, started at once, signed in and ran programs
through the agent (its root on `/dev/sda2`). That run found a bug: a machine started right after its import timed out,
QEMU's first open of the disk with `O_DIRECT` waiting behind gigabytes of writeback; the import now writes the disk out
(`fsync`) before it is put in place. Not built: the agent on the old PC and the copy over the network, reading a
connected disk (it needs root), driver injection for virtio-win systems, BitLocker or LUKS recovery.

### 6.12 Guest app integration

- Guest's apps in the launcher and Dock, with icons, marked with a small badge. Launching starts or resumes the VM.
- Open a LordOS file with a guest app: **built** (7.3, Windows guests): Files' Open With copies it through the transfer folder or the
  agent into the guest's Downloads and opens it there.
- Reverse: open a link or file from the guest on the host: optional. **Links built (2026-11-24, in development)**, below; files
  out of the guest are the transfer folder's (6.7.1).
- Guest notifications to the host: optional, off by default. **Built (2026-11-24, in development)**, below.

**Links and notifications from the guest (built 2026-11-24, *in development*; tools 8 and w12, machine.json schema 15).** The
guest is hostile, so neither is on by default and neither ever runs anything on this computer by itself.

- *Turning them on.* Configure → Sharing → **Links from the machine** and **Notifications from the machine** (machine.json
  `"guest_links"`, `"guest_notify"`, written only when true; `lord-vm --set DIR --guest-links on|off --guest-notify on|off`).
  Both change only with the machine off. A machine locked for anonymity (a Whonix member, 19.5) refuses both
  (`vm_machine_check`), and its agent is off anyway. lord-vm's hello to the tools says `links` and `notify` only while
  machine.json allows them, so the tools can tell a person who tries that this machine doesn't take them.
- *In the guest.* Nothing is sent by the tools on their own: a program of the signed-in person asks. Linux (`lordos-guest`
  8): `lordos-guest --open-on-host URL` and `lordos-guest --notify-host TITLE [BODY]` connect to the helper's socket,
  `$XDG_RUNTIME_DIR/lordos-guest.sock` (0600, peers of the same uid only by `SO_PEERCRED`, four at a time, 2 s and one
  frame each). Windows (`lordos-guest-win` w12): the same two options on `lordos-guest-win.exe`, which hand the running
  tools of that session one frame by `WM_COPYDATA`. Either way the tools parse the frame as lord-vm will and answer with
  an exit status: 0 sent, 1 not connected, 2 malformed, 3 this machine doesn't take them. A link is percent-encoded first
  (`ag_link_encode`: `https://x.org/a (b)` becomes `https://x.org/a%20%28b%29`). Not built: a "Send to LordOS" entry in a
  guest's browser or file manager, and passing the guest desktop's own notifications on by themselves.
- *The frames* (`vmagent.h`, strict parsers in `vmagent.c`, fuzzed in `vm-test`): `AGK_LINK` (kind 25), 1..2048 bytes,
  only `http://` or `https://`, a host of letters, digits, `.` and `-` (labels not empty, not starting or ending with `-`)
  or a bracketed IPv6 address, an optional port 1..65535, never a user name or password before the host (`@` in the
  authority: a spoofing trick), only letters, digits and `- . _ ~ : / ? # [ ] @ & = + , ; %` (each `%` with two hex
  digits): the set the launcher hands a browser as one argument (`apps_launch_uri`). `AGK_NOTIFY` (kind 26): a title of
  1..120 bytes and a body of 0..600, UTF-8 without control characters (C0, DEL, C1) or the bidi controls U+200E, U+200F,
  U+202A–U+202E, U+2066–U+2069 (which could make the words read as this computer's own).
- *lord-vm* takes them only from tools that said `links` or `notify` in their hello, only while machine.json allows that
  kind, at most 3 links at once and then one per 5 s, at most 5 notifications at once and then one per 3 s
  (`ag_rate_take`); more are dropped and counted in the machine log. It writes each again and hands lord-server a sealed
  memfd (`VM_GUEST_NOTE`); the log says that one came, never the link or the words.
- *lord-server* (`js_vm.c`) parses it again. A link is held only while the machine is in use (its window in front, as
  the clipboard's gate, 6.8): one at a time; another one meanwhile is dropped, so a link is never swapped in under the
  person's pointer. The window shows a bar: "‹machine› wants to open a link on this computer", the whole link in
  monospace, "Open only links you expect", [Ignore] [Open]. Open is enabled after 800 ms, and lord-server itself refuses
  an Open sooner than 700 ms after the app first saw the link (`guestLinkAnswer`: only the link it holds, never a URL from
  JS); it opens in the default browser through the launcher. A notification is queued (at most 8 waiting) and shown by
  the app as from the machine: "From ‹machine›" as its label, Machines' icon, no actions, no sound.
- *Checked:* `vm-test` (the parsers, the encoder, the rate limit; 80 000 random frames), `guest_shell_test.sh` (the Linux
  helper's socket, the hello, a link percent-encoded, a notification refused while the host says no and cut to its
  limits when it says yes, a `file:` link refused), and in a real KVM guest (below, 6.18).

**Status:** guest apps reach the Dock (`vm:<machine>:<app>`) only while apps mode runs (7.2); nothing is listed before
the machine runs (not built).

### 6.13 Nested virtualization

Inside a guest: amd64: the guest gets `-cpu host`, so it sees VMX or SVM wherever the host's KVM has `nested` on; there is no per-profile switch yet (the Software Development profile was meant to turn it on). arm64: kernel 6.16 enabled it for hardware that has it, off by default, through `KVM_ARM_VCPU_HAS_EL2` (**Reported**); needs FEAT_NV2, so not on Pi 5 or Pi 4 (Pi 5: `KVM_CAP_ARM_EL2` = 0, **Measured**, 2026-11-04). LordOS itself as a guest gets this for free where the host does.

### 6.14 Monitor and system ties

Monitor shows each VM as one row (built: Processes groups `desktop:lordos-vm-<id>` under the machine's name; no drill-in yet). Cleaner never touches `~/Machines` without explicit choice, and offers Reclaim space. Backups skip running VM disks unless consistent. Files shows `.lordvm` as a bundle. Search finds VMs by name. Desktops treat each VM window normally. Power: laptops on battery get a notification with Pause All; a guest's load keeps the host awake while it runs, on purpose, and the battery warning says so. Built: Monitor's row and the idle block while a machine runs (`lord.vm.awake`, lord-vm's logind inhibitor). Built since (*in
development*, "Machines to 100 %"):

- **Monitor:** a machine's row opens to its processes, and **Show Machine** on the selected row opens its window through Machines
  (`args.show`), so a protected machine's window is protected from its first frame.
- **Cleaner** never offers a machine's files: `clean_machine_path` (`clean/cleanfs.c`) refuses the Machines library and any
  `‹name›.lordvm` or `‹name›.lordgroup` folder wherever it is, during the scan and again when a plan is made (a duplicate disk can be
  a linked clone's base; a "project output" can be in a guest's shared folder). Reclaiming a machine's space is Machines' Storage
  window. Tested by `clean-test`.
- **Backups** hold back a running machine's `disks/` and `suspend.state` (the engine's `hold` callback, `backup.c`
  `hold_running_machine`, `machine.c` `vm_backup_hold` and `vm_dir_active`): QEMU writes them while it runs, so the copy wouldn't be
  consistent. A machine that is off is copied like any folder. Tested by `backup-test` and `vm-test`.
- **Files** shows `‹name›.lordvm` and `‹name›.lordgroup` as one item, a bundle (`sdk/openwith.js` `registerBundle`, `bundleFor`): its
  kind is "Machine", and opening it opens the machine's window; Open in New Tab still goes in.
- **Search** (❖Space) finds machines by name (`ties.js`, `searchMachines`).
- **Power:** when the computer leaves the charger while machines run, a notice says a guest's work drains the battery and offers
  **Pause All**; the library's toolbar has it too while on battery (`batteryNotice`).
- **Start with the session** (Configure → Startup and shutdown): the machine starts without a window once the desktop is up, through
  the budget like any start. The list is `settings.json`'s `machinesAtLogin` (machine ids, at most 8), which apps can't write
  (lord.fs's protected list), so an app can't make a machine start at login. A group's members start with their group, not here.
- **Pause while I'm away** (the same section; `app.json` `autoPause`): after 10 minutes without keyboard or mouse the machine pauses,
  and resumes on the next input; one the person paused stays paused (`awayStep`).

Not built: the balloon. A `virtio-balloon` device would let the host take memory back from an idle guest, but the guest decides how
much it gives (a hostile one gives nothing), it is one more device model parsing what the guest writes, and Whonix lists ballooning
among the features that weaken its anonymity (19.5). The memory a machine can use stays its `memory.max` (3.7), which the host
enforces whatever the guest does, and the budget (3.7) keeps the sum within what this computer can give. Decision left to the
maintainer: whether the design keeps the balloon at all.

### 6.15 Command line and coding agents

#### From the terminal: `machines` (built 2026-11-21, since 0.8.9.5; approved by the maintainer 2026-11-21)

`/usr/bin/machines` (a link to `/usr/lib/lordos/bin/machines`; `server/src/cli/machines.c`, pure parts in `mcli.c`)
works on the same library as the app, `~/Machines`, as the person, with no root and no admin action of its own:

```sh
machines list [--json]                       # name, system, state, vCPUs, memory, disk used, graphics
machines status NAME [--json]
machines start NAME [--no-window]            # as Play, or as "Start Without Window"
machines open NAME                           # its window on the desktop
machines stop|poweroff|pause|resume|suspend|reset NAME
machines snapshot NAME list [--json] | take LABEL | revert TAG | delete TAG     # TAG, or a snapshot's unique label
machines clone NAME NEW_NAME [--duplicate] [--keep-identity]
machines rename NAME NEW_NAME                # in development: the name; off, its folder and transfer folder too (6.26)
machines set NAME --memory 8G --cpus 6 --disk 64G --clipboard both --network bridged --bridge eno1 ...
machines help
```

- **NAME** is a machine's name, ASCII case ignored; its start is enough when only one machine has it (more than one:
  the command lists them and exits 4). A path to a machine folder works too, and (in development, with `rename`) so does its id (`machine.json` "id",
  the name that never changes; a Duplicate shares it, so then it's the name). Options take `--opt value` or `--opt=value`;
  `--vcpus` is the same as `--cpus`; `set` also passes `--display`, `--ports`, `--shares`, `--iso`, `--agent`,
  `--clipboard-ask`, `--graphics`, `--gpu`, `--boot-order`, `--boot-menu`, `--sound`, `--volume`, `--microphone`,
  `--sound-out`, `--sound-in`, `--pointer`, `--transfer`, `--devices`, `--nic-model`, `--resolution`, `--cpu-pin`,
  `--perf-profile`, `--capture-fullscreen` and `--name` through to lord-vm.
- **Everything but start and open runs `lord-vm`** (the one beside the command) on the machine's folder, exactly as the
  app does: `--status`, `--stop`, `--poweroff`, `--pause`, `--resume`, `--suspend`, `--reset`, `--snapshot`, `--clone`
  (the new folder named as the app names it), `--rename` (6.26: the same checks as the app, in the person's language
  before lord-vm checks them again) and `--set`. `set` takes lord-vm's `--set` options with friendlier
  values: sizes such as `8G`, `1.5G`, `512M` (a bare number is MB for memory, GB for the disk), `--cpus`, `host-only`;
  everything else is passed through and checked by lord-vm. `list` and `status` read `machine.json`, `run.json` (a
  state whose lord-vm is gone reads as off, as in the app) and `app.json` (the system's name); `status` of a running
  machine asks its lord-vm. A machine with a graphics card isn't suspended (as in the app). After `stop`, `poweroff` or
  `suspend` of a bridged or vGPU machine the command tells the session (below, `release`), so the macvtap and the vGPU
  are given back at once.
- **start and open go through the person's running LordOS session**, so they do exactly what the app does: Play in the
  machine's window (`begin()`: the bridged network's `vm-net-bridge` and the graphics card's `vm-gvt-create` first, then
  `vm-memlock` while it starts), or "Start Without Window" (`startPrep`, then `lord.vm.startDetached`, with the list
  read meanwhile so `vm-memlock` is asked). A start in a window waits up to two minutes to say "running". **Without a
  session**, `start --no-window` runs `lord-vm --start --detach --keep-running` itself only for a machine that needs no
  admin step (shared, isolated or host-only network, no graphics card) and otherwise says why (exit 5); `open` and a
  start in a window need the session (exit 5).
- **The session channel** (`server/src/vm/vmcli.h`, lord-server's side `server/src/ui/js_vmcli.c` as `lord.vmcli`, the
  app's side `apps/machines/cli.js` with `cliPlan` in `model.js`): `$XDG_RUNTIME_DIR/lordos-machines.sock`, an AF_UNIX
  stream socket made 0600 (umask 0177; never replacing anything but its own socket), only in a session's lord-server
  (never the login screen's; an `--app` process has no `lord.vmcli`). A peer is kept only when SO_PEERCRED says it runs
  as lord-server's own uid, at most 4 at a time; the command checks the server's credentials the same way. One request
  line per connection, within 5 s: `open <dir>`, `start <dir>`, `start-detached <dir>` or `release <dir>`, where `<dir>`
  is a machine folder's real path (absolute, ending `.lordvm`, no control characters, no `.`/`..` component, nothing
  after it). The app acts only on a machine its own `lord.vm.list()` has at exactly that folder (else exit 4) and answers
  one line, `<code> <text>`: the exit status and its words in the person's language. Machines deactivated in the Store
  refuses start and open. No command, option, name or other path ever crosses the channel, and it reads no file for the
  peer. `build/mcli-test` checks the parser (strict, with a fuzz pass under ASan) and `tests/machinescli_headless.sh`
  starts, opens and powers off a throwaway machine through a headless lord-server.
- **Messages** are `assets/locales/<lang>/machinescli.json` (all 35 languages), chosen by `LC_ALL`, `LC_MESSAGES` or
  `LANG` as lord-web's are (`weblangid.h`); lord-vm's own reasons stay in English. **Exit status:** 0 done, 1 failed, 2
  wrong command or value, 3 not running, 4 no such machine (or more than one), 5 needs the LordOS session. **`--json`**
  is stable: keys are only ever added (`list`: `{"library", "machines": [{name, folder, id, valid, system, state, reason,
  view, arch, vcpus, memory_mb, disk_gb, disk_used_bytes, graphics, gpu, network}]}`; `status` adds `since`, `detail`,
  `qemu_pid`; `snapshot list --json` is lord-vm's own). Bash completion: `/usr/share/bash-completion/completions/machines`
  (`machines list --names` gives the names).
- **Not yet:** creating a machine from a card (the assistant's download, checks and unattended install: `apply` makes one
  from an ISO the person already has).

#### `lordvm` and the agent: exec, cp, logs, console, MCP (built 2026-11-24, in development)

`/usr/bin/lordvm` is a second link to the same program (`lordvm exec …` and `machines exec …` are one thing). Beside the
commands above, in development:

```sh
machines start NAME --no-window --snapshot TAG     # from a snapshot with memory (lord-vm --loadvm), TAG or its unique label
machines exec "Debian Dev" [--timeout S] -- uname -m   # exits with the program's status (125: it couldn't run, 124: its time ran out)
machines cp [--force] ./notes.txt "Debian Dev":~/       # files, either way; one side is NAME:PATH (~: the person's home there)
machines logs "Debian Dev" [--serial|--machine] [--follow] [--lines N]
machines console "Debian Dev"                      # the serial console in this terminal; Ctrl+] leaves
machines shell "Debian Dev"                        # a shell as the signed-in person, through the guest's LordOS tools (6.18)
machines import SRC NAME [--arch A] [--devices compat|virtio] [--network shared|isolated] [--firmware uefi|bios]
machines apply dev.toml                            # 6.24
machines mcp                                       # a local MCP server on stdin and stdout
```

- **Automation is the person's consent, per machine.** `machine.json` schema 16 adds `"automation": true` (written only
  then; missing is off). It is turned on in Configure → Guest agent → Automation, only with the agent on and the machine
  off, after a question that names what it allows; `machines set` doesn't pass it (a terminal can't give it). The
  anonymity profile (19.5) refuses it in `vm_machine_check`, so no Whonix member can ever have it. lord-vm reads
  `machine.json` again at each request, so the machine's file decides, not what it was at the start.
- **How a program runs** (`server/src/vm/vmexec.h`, `vmexec.c`; lord-vm's `VM_EXEC`, `vmproto.h`). The command connects
  to the machine's own lord-vm socket (`$XDG_RUNTIME_DIR/lordos-vm/<id>.sock`, 0700 folder, 0600 socket, `SO_PEERCRED`
  checked both ways: the person's processes only) and sends one request as a sealed memfd (`F_SEAL_WRITE`, `SHRINK`,
  `GROW`): the program, at most 64 arguments of up to 4 KB each and 64 KB in all, at most 1 MB of standard input, a
  timeout of 1 s to 1 h. lord-vm refuses it unless automation is on, the machine runs, qemu-guest-agent answers with
  `guest-exec`, the guest is Linux (a Windows guest's program would need the scheduled-task step of 6.21: not built) and
  no other program runs. Then two fixed scripts run through `guest-exec`: as root, one only finds the signed-in person
  (the session with a seat, else any session, else the first account from 1000 up, as file drops do) and becomes them
  with `runuser`; as that person, the other runs the program in their home folder under `timeout -k 5`, keeps its
  output in a private temporary folder and prints a header and at most 128 KB of output and 32 KB of errors (qemu-guest-
  agent's answer must fit lord-vm's 256 KB line, base64 included). The program, its arguments and its input are
  `guest-exec`'s arguments and `input-data`, never script text. What comes back is parsed as hostile (`vmx_guest_parse`:
  exact sizes, at most the caps) and handed to the command in a sealed memfd (`VM_EXEC_DONE`). lord-vm's log names each
  program and how much came with it, never its arguments or input; the command also writes `log/automation.log` in the
  machine's folder. On a terminal, what the guest printed is shown with controls as `?` (a guest can't drive the
  person's terminal); to a pipe or a file it goes as it came.
- **cp** is built on it: a copy into the machine goes in 1 MB pieces into a temporary file the person's own script made
  next to the destination, then is checked (size and SHA-256) and renamed (`mv -n`, `--force` replaces); a copy out comes
  in 128 KB pieces into a temporary file on this computer, checked against the guest's SHA-256 at the end and renamed
  without replacing (`renameat2 RENAME_NOREPLACE`, or `--force`). On this computer it reads and writes only the
  person's files under lord.fs's own rules (`fsops.c`: the home folder and their mounts, never a protected path such as
  `settings.json` or the vault), checked on the opened descriptor (`/proc/self/fd`), not only on the name; a size the
  guest claims beyond the free space is refused; Ctrl+C removes the temporary file. Files only (a folder: a tar first).
  It is slow for big files (about 1–2 MB/s out of the machine): the transfer folder (6.7.1) is the way for those.
- **logs** prints the guest's journal (`log/journal.log`, read through the agent every 5 s, 6.19), its serial console or
  lord-vm's own log, made safe for a terminal; `--follow` follows it across rotations. **console** is the serial console
  (`VM_CONSOLE`) in the terminal; **shell** is the agent's shell of 6.18 (`VM_SHELL`: a shell as the signed-in person
  through the guest's LordOS tools, sized to the terminal and resized with it), as the window's Shell gives it; both keep
  printing after piped input ends and read lord-vm's other news so its sends never back up (`tests/machines_shell_test.sh`,
  against a stand-in lord-vm).
- **The MCP server** (`machines mcp`, or `lordvm mcp`): JSON-RPC 2.0, one message per line, over the standard input and
  output of a process the person started (no socket, no port). Each message is parsed strictly (`mcli_rpc_parse`: only
  `jsonrpc`, `id`, `method`, `params`; 1 MB at most); anything printed by the commands it runs is captured, never mixed
  into the protocol. Tools: `list_machines`, `machine_status`, `start_machine` (without a window), `stop_machine` (Shut
  Down), `wait_until_ready`, `run_command`, `get_logs`, `clone_machine`. It acts only on a machine of the library named by
  its name or id (never a path or a `.lordvm` name; its real folder directly in the library, no link; its `machine.json`
  read again there) whose automation is on; every call is written in the machine's `log/automation.log`. Nothing
  destructive is offered (no delete, revert or force off). Not built: `create_machine` from a card, `requirements`,
  `screenshot`.
- **Who may, in one line:** the person's own processes (the same account, as for the session channel), for a machine
  the person allowed on screen, never an anonymity machine; another process of the same account that isn't sandboxed
  can do what the person can (it could edit `machine.json` itself), which is the boundary of a Unix account; sandboxed
  apps (`--app`, Landlock) reach neither socket.
- Tests: `build/vmexec-test` (the request, answer and printout formats, every limit, the two scripts under `/bin/sh` with
  stub `runuser` and `loginctl`, a fuzz pass), `build/mcli-test` (cp's sides, JSON of any bytes, the terminal cleaning,
  apply's file, the MCP envelope, MCP names and the apply file's boundary list, fuzz passes), `build/vm-test` and
  `build/vmgroup-test` (schema 16, the anonymity lock), `tests/vm_exec_e2e.sh` (28 checks in a real guest: checked on
  2026-11-24 with the Ubuntu 26.04.1 server machine made by the Ubuntu card's unattended install, 2 vCPUs, on the dev PC,
  in a private home and namespaces: the person runs it, not root; input, exit status, timeout, the output cap; arguments
  stay arguments; cp both ways with the hash; the refusals; the logs; MCP; apply). The memory-snapshot start was checked
  on the same machine (a marker written after the snapshot was gone).
- Not built: programs in virtio-win guests through `exec` (the shell works there through the guest's tools), folders in
  `cp`.

A local **MCP server** was first planned to build machines too ("a Debian machine with 40 GB, user neil": a card, a
profile, an unattended install, a password kept in the keyring); what is built is the list above.

### 6.16 Storage window

Opened from the host meter, or Window → Storage. It answers one question: **what would really be freed?**

- One bar for the volume: machines, downloads, Trash, other files, free. Colors differ in lightness too, and each has a text label and size.
- A list with each machine under the one it came from: disk size, **what deleting it would free**, and what it shares.
- Selecting one shows what it shares and with whom, and the split by disks, suspended state, snapshots, installation media and other.
- **Downloads:** verified installers and images, removable, marked "can be downloaded again". Built in 0.8.9 for the systems the assistant downloads, with stopped downloads (3.9, "The Linux download cards").
- **Trash:** machines there still take space until emptied.
- Deleting from anywhere states the same figure before asking.
- Offers: Reclaim space (compaction, with an estimate first), delete old snapshots, remove unused media.

How it knows: backing-file chains are read from qcow2 headers; shared extents from `FIEMAP` with `FIEMAP_EXTENT_SHARED` on btrfs and XFS. A clone with shared extents frees only what changed in it; what's shared is freed once all sharers are gone. Figures are shown as "about" on btrfs with compression, where they can't be exact. Measuring runs at low priority on open and caches by file generation, so it never slows the desktop. Ext4 has no reflink, so clones there use backing files or take the full size, and the window explains why. Snapshots are counted separately, which a simple size listing hides.

### 6.17 Agent state

`lordos-guest` is one small static binary with no Python or other runtime, so it runs on a minimal guest. It's separate from `qemu-ga`, which keeps doing time, shutdown and freeze. From version 6 it also has `--stopping <verb>`, run as root by its systemd-shutdown hook at the guest's last step to say whether it powers off or restarts (`AGK_STOPPING`, 3.2 "3D graphics"); Install Agent writes that hook as `/usr/lib/systemd/system-shutdown/lordos-guest-local` (best effort: a read-only `/usr` skips it), the `lordos` package ships it as `.../lordos-guest`, and the helper's hello says `stopping` while one is there.

The first icon on the bar shows its state (`agentTone`, `model.js`): off (not connected), running (connected), paused, or red
(*in development*): the agent answered in this run of the guest and stopped answering while the guest runs (`lost`, kept by
lord-server: a guest that restarts or stops isn't lost), or installing the LordOS tools failed. Tools older than the shipped
ones show a blue dot. Clicking it opens a popover:

- **Connected:** the system and architecture as the agent reports them, agent version, connection time.
- **A list of what works now, and what doesn't and why** (`agentLines`, `model.js`): Shell (the serial console), Shell as the signed-in person (6.18, the tools 8 or w12), Logs (the journal; a Windows guest's event logs), Shut Down, Time, Keep awake, Clipboard, Resolution, Folders, Apps (works when the helper lists its windows, 7.2), Links and Notifications from the machine (6.12: Off unless Configure turned them on); Windows guests leave out Keep awake. Buttons: **Sync time**, **Set Up the Transfer Folder**, and a hint when the clipboard is off. Each line comes from the agent's reported capabilities, so it's the truth for that guest, not a table in the app.
- **Not connected:** the next step, which depends on the state. If the guest is still booting, says wait. If the agent isn't installed: an **Install Agent** button writes `lordos-guest` and its service into the guest through qemu-guest-agent (`guest-file-write`, a fixed script, `lordvm.c`); no disc and no command for the person.

**Protocol.** A virtio-serial port named `org.lordos.agent`, chosen over vsock because it works in every guest without extra drivers. Length-prefixed frames with a channel number, a `hello` carrying version and capabilities, skipped unknown kinds so that either side can grow, and a hard frame limit. `lord-vm`, not `lord-server`, parses it, inside the sandbox. A newer host updates older tools itself, on by default with a switch to turn it off, through qemu-guest-agent (7.1.3).

**Direction of trust.** The host runs things in the guest, never the reverse. A guest can't open a channel, start a host program, or read a host path. Only the person's own user can reach a VM's socket. The guest's links and notifications (6.12) are offers: a link opens only when the person clicks Open, a notification is text shown as from the machine. (Planned, not built: a profile turning the agent's shell off; the agent's shell is 6.18.)

### 6.18 Shell

**Built (audited 2026-11-23):** the Shell icon opens the guest's **serial console** in a pane under the picture (`vmwindow.jsx`,
"Serial console of NAME").

**The agent's shell (built 2026-11-24, *in development*; tools 8 and w12, "shell" in their hello).** When the guest's tools
can, the same pane shows a shell inside the guest **as the person signed in there**, over the agent (no network, no SSH: it
works with Isolated network), with a switch to the serial console and **New Shell**. It is a LordOS terminal (`term_create_fd`,
the same terminal as Terminal's tabs), titled "Shell inside ‹machine›, as the person signed in there" with the purple badge,
so it can't be taken for one of this computer's.

- *Linux guests* (`lordos-guest` 8, `tools/guest/gshell.c`): the helper runs as the signed-in person; on `AGK_SHELL_OPEN` it
  starts that person's own login shell (their `/etc/passwd` shell when it is an executable listed in `/etc/shells`, else
  `/bin/sh`, as `-name`, in their home, `TERM=xterm-256color`) in a new pty. A helper running as root opens none.
- *Windows guests* (`lordos-guest-win` w12): PowerShell (`%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe
  -NoLogo`, from the system folder) in a pseudo console (ConPTY, Windows 10 1809 and later), as the person, in their
  profile folder; tools that run elevated open none.
- *Nothing from the host names a program, an argument, a folder or a variable:* the host only types (`AGK_SHELL_DATA`),
  sizes (`AGK_SHELL_SIZE`, from the terminal's own size) and ends it (`AGK_SHELL_CLOSE`: SIGHUP to its process group, or the
  pseudo console closed). The shell's end comes back with its exit status. One shell at a time; a new one ends the old.
- *Flow control both ways* (`AGK_SHELL_ACK`): never more than 256 KiB sent and not acknowledged; lord-vm ends a shell whose
  guest sends past its window or acknowledges more than it was sent. So `cat /dev/urandom` in the guest, or a terminal that
  doesn't read, keeps memory flat.
- *What the guest writes stays text* (2026-11-24, for this shell and the serial console alike, `term_create_fd`): it can't
  set this computer's clipboard (OSC 52, iTerm's Copy: before this a guest could, past the clipboard's gate), start tmux's
  control mode (its panes and protocol), or make links (OSC 8), and of the terminal's events only its output, size, bell
  and end reach any code: no title, notification, badge, profile, command, host, secret prompt or Passwords fill or save
  (a fake password prompt can't get a saved password offered). `term-guest-test`, `vt-ring-test`.
- *Not built:* an administrator or root shell (the switch in Security), opening it as a tab of the Terminal app (it is a
  pane of the machine's window), `lordvm exec` and the MCP server's `run_command` (6.15).
- *Checked:* `guest_shell_test.sh` (the pty, its size, the exit status, 2 MB with late acknowledgements never past the
  window, the host's close ending the shell's programs), `vm-test` (the frames, fuzzed), and on 2026-11-24 in a real KVM
  guest (Debian 13, kernel 6.12.118) with the real lord-vm and `lordvm-dev` playing lord-server (below).
- *Checked in a guest* (2026-11-24, the dev PC, amd64, KVM; a scratch machine with a private HOME under `/var/tmp`, the
  Debian 13 cloud image, `lordos-guest` 8 copied in from a seed disk and run as a person `lg`, uid 1000, as its user unit
  would; Isolated network): the tools' hello gave lord-vm the shell, links and notify capabilities (machine.json with
  both switches on); `lordvm-dev`'s `shell 100 30` opened `lg`'s bash on `/dev/pts/0` at 30 × 100 rows and columns;
  `lordos-guest --open-on-host 'https://lordos.org/a b(c)'` reached lord-server's side as
  `https://lordos.org/a%20b%28c%29` and `--notify-host 'Build done' 'all 3 targets'` as that title and body (both exit
  0; the machine log says "the guest asks to open a link", "the guest sent a notification", never their contents);
  `exit 5` came back as "shell: ended (the shell exited with 5)". *Not checked:* the window's pane, bar and
  notification on screen in a real lord-server session; a Windows guest (w12).

The Shell icon opens LordOS's Terminal with a shell inside the guest: over the agent, not the network, so it works with no SSH and with Isolated network. **The signed-in user by default**, in a Linux shell or PowerShell. Root or an administrator shell only with an explicit switch, per machine, in Security. The same rule covers `lordvm exec` and the MCP server's `run_command`. The tab is titled with the machine's name and marked, so that a guest's shell can't be mistaken for a host's. Terminal size follows the tab.

### 6.19 Logs

A window following the guest's logs as they arrive: the systemd journal in Linux (read through qemu-guest-agent's `journalctl` every 5 s, `lordvm.c`). **Built (`logs.jsx`):** sources Guest, Serial console and Machine log, an **Overview** with the diagnostics of `diag.js` and **Copy report**, text filter, levels all/warnings/errors, Pause, Clear view, Copy, Save, 50 000 lines.
**Built 2026-11-24 (*in development*):** a Windows guest's **System and Application event logs** through qemu-guest-agent every
10 s (one fixed PowerShell script, `EVLOG_PS` in `lordvm.c`, whose only value is a cursor of digits that lord-vm checks,
`evlog_cursor`; written in the journal's short-iso shape, so Logs reads them like a Linux guest's; `VAC_EVENTLOG`); a **unit
or source filter** (click a unit; "Only ‹unit›" takes it off); guest times shown in **this computer's local time**, with
the guest's zone named in the footer when it differs (`diag.js` `journalLine`, `guestZone`; `vm_model_test.mjs`). *Not
checked yet:* the event logs in a running Windows guest.

- Filter by text as you type, plus level and unit or source.
- Pause (entries keep buffering, with a count of new ones), Clear view, Copy selected, Save to a file.
- Colored level plus a text label, monospaced, timestamps in local time, with the guest's time zone shown if it differs.
- A bounded ring in memory, 50 000 lines, so a noisy guest can't grow the app.
- Also **Machine log**: QEMU's and `lord-vm`'s own log, which works even if the guest never boots. A guest's log is untrusted text: escape sequences are stripped.

### 6.20 Screenshot and recording

The camera icon saves the guest's display at its real resolution, directly from the frame, with no window around it. The record icon records display and sound to a video, with hardware encoding where available, a visible red state on the bar and the Clock bar's recording indicator. Files go to the same folder as the system's Screenshots app. No screen-capture permission is needed, since it's the VM's own frame, and protected host windows are never included.

### 6.21 File copy with progress

**Status (beta, 2026-11-05, superseded):** not built then. The agent's channel carries the clipboard (text, rich text, pictures: 3.9), not files: copy and
paste of files and drag in or out need the flow control, resume and panel below, and are Phase 3 work.
**Since 0.8.9.5:** files dropped on a machine's window go into the signed-in person's Downloads
(`VM_FILE`, AGENTS.md "Machines' dropped files"), through the transfer folder at full speed when the guest has it (6.7.1), else
through qemu-guest-agent. Files arrive as hidden `.lordos-drop-*.part` files renamed when complete, and a name in use gets
" (n)". Shown is one progress line; the panel, flow control, resume, drag out, **Send to LordOS** and quarantine below are
**not built**.

Drag files and folders onto a running machine's window to copy them to the desktop of the user signed in at its display, or onto a shared folder window to choose the place. It goes through the agent, so it needs neither network nor shared folders.

A panel along the bottom of the window shows file name, size, a bar, speed, time left and Cancel. Several transfers queue in it, and it hides when done.

- Atomic: files arrive in a hidden folder beside the destination and are moved in when complete, so that nothing half-written appears.
- Name conflicts are never overwritten silently: a numbered name is used, and the panel says so.
- Flow control by acknowledgement keeps memory flat for any file size. **Target:** 300 MB/s on Linux guests with NVMe on a PC, measured in Phase 3.
- Resume after a dropped connection, by offset and hash. Optional end-to-end check, on by default for files over 1 GB.
- Names are sanitized for the guest's rules. Symbolic links aren't followed; permissions and dates carry over where the file system can.
- **Out of the guest (not built):** in Apps mode, windows are ours, so dragging out could work. From a Linux guest's own desktop, no agent can see the drag, so the agent adds **Send to LordOS** to the guest's file manager, plus `lordvm cp`. The panel says this when someone tries. Virtio-win guests allow drag-out through the agent, **To test**.
- Scanning: files coming out of a guest get the same quarantine marking as downloads.

### 6.22 Import

An importer has three steps: **detect** a file or folder, show a **plan** of what will happen, then **import** into a new `.lordvm`, leaving the original alone.

| Source | Reads | Result |
|---|---|---|
| Disk images: qcow2, raw | The image | New machine, settings from a profile (the assistant's *Import a machine*, `lord-vm --create --import`, 3.9) |
| Disk images: VMDK, VDI, VHDX, VHD | The image | New machine around it (*Migrate from another app*, below) |
| OVA, OVF | Descriptor: CPU, memory, disks, networks; a VirtualBox machine section when the package has one | Machine with those settings |
| Virtual machine bundles of other desktop tools | Their settings file and disks | Settings mapped where they mean the same; disks converted |
| libvirt domain XML | Domain | Machine with those settings (not built) |

The plan states what's lost. **Snapshots in foreign formats are flattened to the current state.** Saved sessions can't move between engines, so a machine must be shut down first. Encrypted foreign disks need decrypting first. A guest that lacks virtio drivers starts on emulated SATA and an ordinary NIC; the assistant then installs the drivers and switches. A machine of another architecture imports fine and is marked Emulated: moving an arm64 machine to a Pi works natively, moving an amd64 one doesn't. Conversions run in the sandbox, because disk images are hostile input.

**Built** (Phase 3, branch `machines-import`, 2026-11-05): *Migrate from another app* in the assistant, `lord-vm --import-plan SRC` and
`lord-vm --import-machine SRC DIR`. The goal is an exact clone, so that a licensed guest that is already activated sees as little
hardware change as possible. User-facing text says "other virtualization apps" and the file extensions; the tools are named only in
this table of formats, in code identifiers and where a name is a file-format fact.

| Format (the tool that writes it) | Settings | Disks, as read | Snapshots |
|---|---|---|---|
| `.vmx` and a `.vmwarevm` folder (VMware Workstation, Fusion) | `key = "value"` lines, `\|XX` escapes; 1 MB at most | VMDK: one sparse file (`KDMV`, its own descriptor), or a descriptor and its extents (`twoGbMaxExtentSparse`, `monolithicFlat`, split flat); stream-optimized | Delta VMDKs naming their parent (`parentFileNameHint`), followed from the current one, each parent named by us |
| `.vbox` (VirtualBox) | XML | VDI, VMDK, VHD; the media registry's tree | Differencing VDIs: **merged by lord-vm** (QEMU's `vdi` driver can't read a differencing image) |
| `.pvm` folder with `config.pvs` (Parallels Desktop) | XML | `.hdd` folders: `DiskDescriptor.xml` and `.hds` data files, read with QEMU's `parallels` driver | `.hds` layers of the snapshot tree: **merged by lord-vm** (the driver has no backing) |
| `.ova`, `.ovf` (OVF, any tool) | XML; an `.ova` is a ustar archive read in place | Stream-optimized VMDK inside the package, read through a `raw` node with `offset` and `size`: nothing is extracted | Flattened at export by the tool |
| One disk: `.vmdk`, `.vdi`, `.vhdx`, `.vhd`, `.qcow2`, `.img`/`.raw` | none: a machine with this computer's defaults; BIOS or UEFI chosen on the sheet | Format from the first bytes | A VDI layer alone is refused (import its `.vbox`) |

**What is carried over as is**, into `machine.json` schema 3 (`devices`, `nic_model`, `extra_nics`, `cpu_sockets`/`cpu_cores`/`cpu_threads`,
`smbios`, `disk_serial`, `data_disks`, `rtc`; a schema 2 file is read as is and written as 3 with `machine.json.schema2` kept, 3.6):

| What the guest saw | `.vmx` | `.vbox` | `config.pvs` | OVF | Here |
|---|---|---|---|---|---|
| SMBIOS system UUID | `uuid.bios` | `<Hardware uuid>`, else `<Machine uuid>`; `DmiSystemUuid` extra data first | `SystemUuid`, else `VmUuid` | `uuid.bios` extra config, or the VirtualBox section | `-uuid` (the machine's id) |
| Manufacturer, product, version, serial, SKU, family | `serialNumber`, `hw.model` | `DmiSystemVendor`, `…Product`, `…Version`, `…Serial`, `…SKU`, `…Family`; `AutoSerialNumGen` gives `VirtualBox-<uuid>` | — | the VirtualBox section | `-smbios type=1,…` |
| Every card's MAC and model | `ethernetN.generatedAddress` / `.address` (static), `virtualDev` | `<Adapter MACAddress type>` | `<NetworkAdapter><MAC>` | `rasd:Address`, `ResourceSubType` | first card on the network, the others present with their MAC, not connected (`extra_nics`, slots 0x18…) |
| Card model | e1000e, e1000, vmxnet3, vlance → pcnet | 82540EM → e1000, 82545EM → e1000-82545em, 82543GC → e1000-82544gc, Am79C97x → pcnet, virtio | not mapped: e1000e | E1000, VmxNet3, PCNet32 | the same QEMU device where it has one, else e1000e |
| BIOS or UEFI | `firmware = "efi"` | `<Firmware type="EFI…">` | `EfiEnabled` | `firmware` extra config, the VirtualBox section | SeaBIOS or OVMF (AAVMF on arm64) |
| vCPUs and topology | `numvcpus`, `cpuid.coresPerSocket` | `<CPU count>` as one socket of that many cores (**Reported**) | `<Cpu><Number>`, one socket | `VirtualQuantity`, `vmw:CoresPerSocket` | `-smp N,sockets=S,cores=C,threads=T` |
| Memory | `memsize` | `RAMSize` | `<RAM>` | `AllocationUnits` × `VirtualQuantity` | the same, cut to this computer's limit with a note |
| Disks, order, size | the boot disk (`bios.hddOrder`) first, then scsi, sata, nvme, ide | controllers in order, then port and device | `<Hdd>` in order | `ResourceType` 17 items in order | `system.qcow2` and `data-1…4.qcow2`, same virtual size (not rounded), boot order kept |
| Disk serial | with `disk.EnableUUID`: the descriptor's `ddb.uuid` (32 characters: too long for SATA, said on the sheet) | the port's `SerialNumber` extra data, else `VB%08x-%08x` of the base image's UUID (DevAHCI.cpp, DevATA.cpp) | `<Serial>` | — | `serial=` on the disk, 20 characters at most |
| RTC base | local for Windows guest types | `<RTC localOrUTC>`, local by default | local for the Windows type | the VirtualBox section | `-rtc base=localtime` |

**How the UUID is derived.** An app writes 16 bytes into its SMBIOS type 1 table with an SMBIOS version; the guest decodes them. Linux
(dmidecode, `/sys/class/dmi/id/product_uuid`) swaps the first three fields from SMBIOS 2.6 on and not before; Windows reads them
little-endian whatever the version. QEMU writes the UUID it gets little-endian in a newer table, so every guest reads back exactly the
`-uuid` string. So `-uuid` is the string the guest read on the source:

- `.vmx`: the bytes are `uuid.bios` as written. The table is 2.7 with `acpi.smbiosVersion2.7 = "TRUE"`, or for `virtualHW.version` 13 and
  later when the key is absent, and 2.4 before (**Reported**: the vendor's knowledge base says 13, Red Hat's article on virt-who says 12).
  Windows guests, and Linux guests on 2.7, read it swapped. **Measured** for a Linux guest on hardware version 19 (below).
- `.vbox`: the bytes are the hardware UUID in network order unless `<SmbiosUuidLittleEndian enabled="true"/>` (VirtualBox 7 writes it
  for new machines); a `DmiSystemUuid` value is always in network order; the table is 2.5 (DevPcBios.cpp, DevFwCommon.cpp, Settings.cpp).
  Linux reads the bytes as stored, Windows little-endian. **Measured** for Linux with a real `.vbox` of VirtualBox 7.2 (below).
- `config.pvs`: assumed written as the spec says, so read as written (**To test**: no such app or guest here).

A guest whose two readings differ (a Windows guest on VirtualBox's old order, a Linux guest on VMware 2.7) gets its own reading: the
guest type decides. **The license:** the identifiers are copied, the hardware is still new to the guest, so its system may ask to activate
again; the sheet says that and nothing more. There is no activation tooling of any kind.

**What the plan sheet states before importing** (`--import-plan`'s `blockers` and `notes`, the mock *Import*): a suspended source
(`checkpoint.vmState`, a `.vmss`, `stateFile`, a `.sav` or `.mem`) is refused until it's shut down there; the other app holding the
machine (a `.lck`) is refused; an encrypted machine or disk (`encryption.keySafe`, `CRYPT/` properties, `MachineEncrypted`, an `.hdd`
encryption engine, a VMDK descriptor's `encryption.`) is refused; more than five disks are refused. Notes: snapshots flattened (how many),
the virtual TPM can't be carried (with a warning to have the disk's recovery key at hand if the guest encrypts its disk with the TPM),
Secure Boot off here, UEFI boot entries made again (the guest's fallback loader starts it), attached disc images, shared folders and USB
rules left out, cards beyond the eighth, a card or a UUID the source didn't keep made new, a disk serial too long for SATA, vCPUs or memory
cut to this computer. The other architecture is marked Emulated, with *Other architecture…* to read the plan again for the other one.

**Guests without virtio drivers.** Every import starts on `devices: compat`: the system disk and data disks as `ide-hd` on SATA (q35's own
AHCI, `ide.0`…`ide.4`; on arm64 an `ich9-ahci` in the system disk's slot), the CD/DVD as `ide-cd` on port 5 (so a drivers' disc can go in
while it runs), a USB tablet (and keyboard on arm64) on `qemu-xhci`, and the card's own model (e1000e when the source's has no QEMU
twin), MAC kept. Configure → Disks → *Switch to Fast Devices* (`lord-vm --set --devices virtio --nic-model virtio`, the machine off) moves
it to virtio once the guest has the drivers, and *Switch Back* undoes it. Installing those drivers into a guest that has none (a Windows
guest: the virtio-win ISO, 4.3) is **Phase 3 work, not done**: today the person inserts the drivers' disc and installs them in the guest.

**Security: every file of the source is hostile.**

- XML (`vmxml.c`): no `<!DOCTYPE`/`<!ENTITY` (no external entities, no entity expansion), the five predefined entities and numeric
  references only (no NUL, no surrogates), 4 MB, 64 levels, 50,000 elements, 64 attributes each, no recursion, every read bounded; prefixes
  dropped, no URL resolved. `.vmx` and VMDK descriptors: 1 MB, 20,000 lines. Tar: checksums checked, plain names only, regular files only
  (a link, a device, a long-name or extended header is refused), sizes inside the file.
- Paths: a name must be plain (`vmi_rel_ok`: no `..`, `.`, empty component, `/` at the start, `\`, `:`, `~`, control characters); an
  absolute name is looked for by its last part only, in its folder; the result's real path must be inside the bundle and a regular file (a
  symlink that stays inside is fine, one that leads out is refused). VMDK extents must be in the descriptor's folder. The new machine can't
  be made inside the source's folder.
- Disks: the format comes from the first bytes (`vmi_disk_format`), checked against what the settings say; a qcow2 must have no backing
  file or data file (`vm_qcow2_plain`); `qemu-img` sees each disk only as a `json:` spec that names every driver, `"backing": null` on the
  base and each parent named by us (never the header's name), extents opened by the `file` driver, and runs in the tool jail
  (`vm_jail_tool_n`: `no_new_privs`, our seccomp filter, no TCP, Landlock with read access to exactly the files we resolved and write access to
  the new `disks/` only). VDI and `.hds` layers are merged by a jailed child of lord-vm (the same jail, no exec) that checks every block table
  entry against its file and writes one sparse raw file, then converted by the jailed `qemu-img`; the raw file is removed.
- The result: each disk passes `vm_qcow2_plain`; files are 0600; the source's files are only read (`O_NOFOLLOW`).

**Interrupted imports** (3.6): the machine is made in `<library>/.<name>.part-<pid>` with `operation.json` "import" (source and target), and
renamed into place after `machine.json`; `operation.json` goes last. A killed import leaves its hidden folder, which the next import or
clone in the library removes (the pid is gone), and says so; `qemu-img` and the merge die with lord-vm (`PR_SET_PDEATHSIG`). An intent left
after the rename is closed at the machine's next start (*the import had finished*). `vm_recovery.sh`'s import case kills it mid-conversion: no `qemu-img` left, nothing in the library, the next import removes the folder and says so, the new disks clean, the source unchanged, an intent left after the rename closed at the next start (9 checks; the gate 31 passed, 0 failed, VM116, 2026-11-05).

**Tests.** `tests/vmimport_test.c` (run by `make test` under ASan and UBSan, 141 checks): one invented fixture per format in
`tests/vmimport/` (two `.vmx`, three `.vbox`, a `config.pvs`, a `DiskDescriptor.xml`, an `.ovf`, a split VMDK descriptor) with every mapping
above, the UUID rules for each SMBIOS version and guest type, VirtualBox's default serial, tar members, XML refusals (XXE, entity
expansion, unknown entities, NUL, surrogates, mismatched tags, depth, size, element count, 200,000 comment-split text pieces in linear time), and every fixture truncated at every length and
mutated at random, 37,841 parser runs with no fault. `tests/vm_import_test.sh` (no guest boots; 44 checks, VM116, 2026-11-05): a disk named
with `..`, by an absolute path, through a symlink out; an extent with `..`, a device path, a symlink out; a snapshot whose parent is outside;
a qcow2 with a backing file under a `.vmdk` name; an encrypted, a suspended and an open machine; XXE and entity expansion in a `.vbox`; a
`.vbox` location and an `.hds` file with `..`; `.ova` members with `..` and a link; an `.ovf` href with `..`: all refused, nothing made, the
secret file in no new disk. Merges byte for byte: a split VMDK with two snapshots, two differencing VDIs (a block replaced, one zeroed), an
`.hds` snapshot layer; the sources unchanged (checksums before and after); an `.hds` block table entry past the file's end refused at merge.
`tests/vm_import_identity.sh` (boots each import with KVM; the guest reports on its serial line): below.

**Identity, measured** (`vm_import_identity.sh`, 33 passed, 0 failed; VM116, Debian 13, nested KVM, the official Debian 13 nocloud image as the guest, 2026-11-05):

| Source | Guest's `product_uuid` | Guest's MACs and drivers | Other |
|---|---|---|---|
| `.vmx`, virtualHW 19, `uuid.bios "56 4d 12 34 56 78 9a bc-de f0 11 22 33 44 55 66"` | `34124d56-7856-bc9a-def0-112233445566` (2.7: swapped), as derived | `00:0c:29:ab:cd:ef/e1000e`, `00:50:56:01:02:03/vmxnet3` | serial `EX-VMX-0001`, UEFI, 1 socket × 2 cores, the data disk's snapshot data and its exact size (64 MiB) |
| `.vbox` made by VirtualBox 7.2.22's `VBoxManage` (hardware UUID `01234567-89ab-4def-8123-456789abcdef`, stored little-endian) | `67452301-ab89-ef4d-8123-456789abcdef`, as derived | `08:00:27:1a:2b:3c/e1000`, `08:00:27:1a:2b:3d/virtio_net` | serial `EX-VBOX-0001`, vendor `Example Systems`, SATA serial `VB…` as derived from the VDI's UUID, BIOS |
| its `.ova` export (`VBoxManage export`) | the same | the same | serial and vendor from the package's machine section |
| `config.pvs` (`SystemUuid {feedface-1111-4222-8333-444455556666}`) | `feedface-1111-4222-8333-444455556666` | `00:1c:42:ab:cd:ef/e1000e` | UEFI |
| a loose VHDX | a new one | a new one | booted from SATA |

**On screen** (VM 125, LordOS 0.8.8.3 amd64 with this branch, 2026-11-05; `machines-shots5`): the assistant's *Migrate from another
app* tile, the file dialog into a `.vmwarevm` bundle (Debian's nocloud image as a split VMDK, a data disk with a snapshot, two cards,
UEFI, a vTPM, Secure Boot, a disc image, a shared folder), the plan sheet (`01-plan-sheet.png`, `02-plan-sheet-lost.png`: name, system,
CPU and memory, two disks, both MACs, firmware, the UUID kept, the devices; the TPM warning; what won't come along; the license line; space),
the progress (`03-import-progress.png`), the done sheet (`04-import-done.png`), the card in the library (`05-library-card.png`), and the machine
running in its window (`06-imported-running.png`, `07-imported-running-console.png`: the guest's kernel brings up `e1000e` as `enp0s2` and
`vmxnet3` as `enp0s24`, and its first-boot questions on the serial console). The guest's own reading of its UUID and MACs on screen wasn't
finished: VM 125 was taken back by another test session mid-way (the same readings are measured above by `vm_import_identity.sh`), and
Configure's *Switch to Fast Devices* wasn't driven on screen (`lord-vm --set --devices` is). Seen there and fixed: the sheet's name field
didn't show, "1 snapshots", the tile's hint was cut.

An independent review (path traversal, XXE, memory safety) found and the branch fixed: XML text split by many comments cost the square of
its length (a library `strstr` measures the whole rest of the input; the reader now scans only to the match and keeps each element's text
length), a VMDK descriptor's place inside an `.ova` member was checked after an unchecked multiplication, extent offsets weren't validated,
over-long descriptor lines were cut instead of refused, and a disk with more pieces than the jail lists failed obscurely. The review read the
parsers, `imp_resolve`, the specs and the jail and found the traversal and XXE guarantees held.

VirtualBox couldn't run its machine on VM116 (no `vboxdrv`), so what its own guest read wasn't seen: the derivation is from its source code,
and the measurement is that our guest reads what that derivation gives. No VMware or Parallels app was available: those fixtures are made
by `qemu-img` and written by hand from their file formats.

**Not done or not verified:** installing virtio drivers into a guest that has none (above); a Windows guest wasn't imported (no image or
license here), so the little-endian reading and the RTC are from the rules above, not seen; VMware's encrypted descriptors (the whole
descriptor encrypted) are refused only when the `.vmx` or descriptor says `encryption.`; `.vhd` fixed images are recognized by name;
an `.ova` with compressed or chunked disks is refused; `.hdd` disks split into several storages are refused; a VMDK's `ddb.uuid` serial
can't go on SATA; extra cards aren't connected; cancelling an import from the sheet; the manifest (`.mf`) of an `.ova` isn't checked; the
plan's space estimate counts the source's files, not what the guest wrote; libvirt XML.

### 6.23 Reaching a machine from another computer

On its own computer a machine's picture comes by dmabuf and its input, clipboard and sound through the D-Bus display and the agent: no network, no encoder, no VNC. Compressing it for WebRTC there would only add delay.

From another computer, a running machine is offered by LordOS's own remote desktop over WebRTC, with video and a data channel for input, clipboard and files. It uses the same pairing, stays off by default per machine, shows who is connected, grants input, clipboard, files and sound one by one, and no VNC or SPICE port is ever opened by a machine. Phase 5: **To test**.

### 6.24 Reproducible machines

`lordvm apply dev.toml` creates or updates a machine from a short text file kept beside a project. **Built 2026-11-24,
in development** (`machines apply`, `mcli_apply_parse`): a strict subset of TOML, an optional `[machine]` table, then
`name`, `arch`, `memory`, `cpus`, `disk`, `network`, `iso`, `graphics`, `clipboard`, `agent`, `ports` and `shares`
(one-line lists, as `machines set` takes them); an unknown or repeated key, anything after a value or another value form
is refused with its line. A machine of that name is changed (with it off); otherwise one is made (`lord-vm --create`, then
`--set`). The file is untrusted (it came with a repository): everything in it that reaches beyond the machine (an ISO,
forwarded ports, shared folders, a bridged or host-only network, the clipboard, the guest agent) is listed and needs the
person's yes on their own terminal (`/dev/tty`; no terminal, nothing changes); its ISO must be a `.iso` or `.img` file in
the person's Downloads folder or the Machines library; automation can't be asked for in it. Example:

```toml
[machine]
name = "Debian Dev"
memory = "8G"
cpus = 4
disk = "64G"
network = "shared"
ports = ["tcp:2222:22"]
```

Not built: packages to install through the agent, `lordvm rebuild`, a system by card name (the file names an ISO).

### 6.25 Capture mouse (built 2026-11-22, since 0.8.9.5; approved by the maintainer 2026-11-21)

A machine gets the pointer as an absolute tablet: the pointer moves freely in and out of its picture (**Automatic**, the default).
Games and programs that read relative motion (first-person views, 3D editors that turn the camera with the mouse) need it the other
way: **Captured**. The machine window's toolbar has a *Capture mouse* button (and *Actions → Capture mouse*); captured, the host's
pointer is hidden and held inside the picture, and the guest gets relative motion that never stops at a screen's edge.

- **Release**: ❖Esc or Ctrl+Alt (both, taken by lord-server before any app or the guest sees them as a release key; Ctrl+Alt still
  reaches the guest), the button, and always when the window loses the keyboard, the machine stops, its picture goes away, a dialog
  takes the window, apps mode starts or the session leaves its screen. On capturing, *Mouse captured — ❖Esc to release* shows over
  the picture for four seconds. ❖Esc is taken only while captured: the next ❖Esc gives the desktop's shortcuts back as before
  (`sdk/kbinhibit.js`), and the shell hears the lock's end (`'pointerlock'` `lockend`), so ❖'s release isn't taken for a tap
  that opens the launcher.
- **Full screen**: Configure → Display → *Capture automatically in full screen* (machine.json `capture_fullscreen`, schema 12, off
  by default; `lord-vm --set --capture-fullscreen on|off`) captures on entering full screen and releases on leaving it; after ❖Esc
  there, a click in the picture captures again. A capture asked for with the button stays across full screen (the lock moves to the
  rebuilt window's picture). The floating bar doesn't open while captured.
- **Profiles** (Settings → Profiles, the New Machine assistant): a profile's *Mouse* `capture` (app.json `input.mouse`) captures
  whenever the window comes to the front with its picture up, and a click in the picture captures again after ❖Esc; its
  *Shortcuts* `never` (`input.shortcuts`) means the window never takes LordOS's shortcuts (not even asked). A profile's *Capture in
  full screen* is written to machine.json `capture_fullscreen` with `lord.vm.set` when the machine is made (`setPatch`): that field,
  the one Configure changes, is its only home; app.json keeps only the mouse and shortcuts choices.
- **Apps mode**: not offered (the button says why): the guest's windows are LordOS windows that share this computer's pointer.
- **The pointer lock** (`server/src/pointerlock.c`, generic, not Machines'): while held the pointer stays at the centre of the
  owner's node and nothing the owner says moves it; the evdev motion (after Settings → Mouse's speed, before clamping to the screen)
  goes to the owner's sink. An owner with a button sink (Machines) gets buttons and wheel steps straight, with no hit test; others
  get them at the centre. An owner may show a pointer image somewhere in its node (`ptrlock_show`, kept inside it): only the image
  is drawn there. It ends on ❖Esc or Ctrl+Alt (`on_input`, before JavaScript), when its owner ends it, when its node has been gone for
  half a second (a rebuilt window names its new node meanwhile), or when the input goes (another console, the display detached). One
  lock at a time. Other native apps and games get it as `lord.input.lockPointer(node)` / `unlockPointer()` with the motion as
  `'pointerlock'` listener events once a frame; isolated apps don't have it (no broker call). Wayland clients keep their own
  (`zwp_pointer_constraints_v1`, `wl/ptrlock.h`). `tests/pointerlock_test.c`.
- **The machine's side**: `lord.vm.capture(sid, on, node)` (js_vm.c) takes the lock with a sink that sends each motion straight to
  lord-vmdisplay (`VD_REL`, no JavaScript in between) and tells lord-vm (`VM_CAPTURE`). lord-vm makes a relative mouse QEMU's active
  one (QMP `query-mice`, then `human-monitor-command` `mouse_set N` with an index it read and formats itself; QMP has no command for
  it) and the tablet again on release. Every machine with a window now has that relative mouse beside its tablet: a `usb-mouse`
  (compat and Windows devices) or a `virtio-mouse-pci` on slot 0x14. Not amd64's PS/2 mouse: a Linux guest drives it as VMware's
  VMMouse (QEMU's `vmport`), which drops plain relative packets (seen on the Debian 13 guest below). QEMU makes active the mouse
  whose guest driver started last, and the new mouse's starts after the tablet's at each boot, so lord-vm checks every second while
  the machine runs (`query-mice`) and makes the tablet active again when it isn't captured (seen: the virtio mouse took over 7 s into
  the boot and was set back within a second). A saved state or snapshot from before this change loads with the new mouse in its
  reset state (**to test**: reverting a running machine to such a snapshot). lord-vmdisplay sends `Mouse.RelMotion`; until QEMU says the mouse is relative it moves its own
  position by the motion and sends that, so the pointer still works without a relative mouse. With a guest that hands its pointer over
  (`MouseSet`), its image is drawn where the guest has it, kept inside the picture; otherwise none shows (games draw their own).
  **Security** (the commit review, 2026-11-22): a guest's reported position never moves the host's pointer: clicks and the wheel go
  straight to lord-vmdisplay (`VD_BUTTON`, `VD_WHEEL`) from lord-server's input path, so a guest can't steer a click onto anything
  of this computer's; at the end lord-vmdisplay lets go of everything held (`VD_RELEASE_ALL`) and the pointer is where the lock put
  it, the picture's centre. `pointerlock_test` checks that no guest position takes the pointer, or its image, out of the picture.
  A touchpad's two-finger scroll is a gesture and still reaches the window's `onWheel` while captured. `tests/vm_capture_test.mjs` (the window's decisions), `vm_test` (machine.json schema 12, the choice among
  QEMU's mice). `lordvm-dev`: `capture 0|1`, `rel DX DY`.
- **Measured** on 2026-11-22 (amd64, KVM, a Debian 13 nocloud guest under `lordvm-dev` with a unit that prints every mouse's evdev
  events to the serial line): with virtio devices, captured, `rel 10 5` moves arrived on the guest's *QEMU Virtio Mouse* as REL_X /
  REL_Y with exactly those values, `rel -3000 0` as −3000 (no edge); released, `move` arrived on *QEMU Virtio Tablet* as ABS again
  and `rel` was ignored. With compat devices the same on *QEMU USB Mouse* and *QEMU USB Tablet*. The first move after a switch goes
  the right way (lord-vmdisplay asks `IsAbsolute` again every 100 ms for 3 s). Not yet seen: the lord-server window itself (the
  pointer lock, the button, the notice, full screen), Windows guests and arm64.
- **Buttons in both modes** (fixed 2026-11-22). Free and captured, a button or wheel step goes to QEMU the same way
  (`Mouse.Press`/`Release` with QEMU's button number, 2 for the right), and QEMU hands it to whichever mouse is active: the
  tablet when free, the relative mouse when captured. Each of them reports left, right and middle. The difference was the
  order: the machine's window sends a click as the pointer's position and then the button (`VD_MOTION`, `VD_BUTTON`) in one
  burst, and lord-vmdisplay sent the button at once but the position only at the end of its turn. So the tablet pressed at the
  previous position, then moved with the button down, which is a short drag. Captured, a press only sends a button, so nothing
  moved in between. Windows' shell treats a right-button drag of more than a few pixels as a drag, and no menu opens. That is the
  likely reason right-click failed only with the mouse free, while a left click still landed. Now any motion gathered so far
  goes out before each button and wheel step, in both modes. `tests/vmdisplay_test.c` checks the order for right, left and wheel
  clicks, free and captured, and that release-all lets go of a right button. **To check** on the Windows 11 GVT-g machine: a
  right-click with the mouse free, on the desktop and in Explorer, in a window and in apps mode.
- Hotkeys are fixed for now (❖Esc and Ctrl+Alt); choosing them is for later.

### 6.26 Renaming a machine (built 2026-11-23, in development; asked by the maintainer 2026-11-23)

A machine's name is `machine.json`'s `"name"`. Its folder `~/Machines/<folder>.lordvm` and its transfer folder
`~/Machines/Transfer/<folder>` (6.7.1) were named once, at creation, so a machine renamed in Configure kept showing its
old name in Files. Now they follow the name while the machine is off.

- **The folder a name gives** is the one a new machine gets (`vm_folder_name`, machine.c, used by ui/js_vm.c and by `machines rename`;
  `machines clone` names the new folder with its own copy, `mcli_folder_name` in `mcli.c`): letters, digits, space, `-`, `_` and `.` (not first), anything else one `-`, at most 48 bytes,
  "Machine" when nothing is left. A name whose folder already reads the same once made safe keeps the folder (the
  folder `Ubuntu (dev).lordvm` stays for `Ubuntu [dev]`, which would give `Ubuntu -dev`).
- **The plan** (`vm_rename_plan`, read-only, from the library as it is): refused for a name that isn't one
  (`vm_rename_name_ok`: `vm_name_valid`, and no leading dot), for a name another machine of the library has (ASCII case
  ignored, as the command finds machines), and when the folder moves, for anything in the library called
  `<folder>.lordvm` or a folder `Transfer/<folder>` other than its own (case ignored both: a library copied to a
  case-insensitive disk must still work). `lord.vm.renamePlan(dir, name)` gives Configure the same answer as you type.
- **With the machine off** (`lord-vm --rename DIR NAME`, or `lord-vm --set DIR --name NAME`, under the machine's lock,
  after `operation.json`'s recovery): the plan again, refused with exit 5 and `{"renamed":false,"problem":...}`;
  `machine.json` with the new name first; then the folder with `renameat2(RENAME_NOREPLACE)` (never over anything) and
  an fsync of the library; then the transfer folder with its owner record (`vm_transfer_adopt`). A folder that can't
  move keeps its name, the name is still saved, and lord-vm says why (`"folder":"kept","why"`); a transfer folder that
  couldn't follow does at the next start. The answer: `{"set":true,...,"name","dir","folder":"moved"|"same"|"kept"}`.
- **What doesn't need rewriting**, checked file by file: the disks, `disks/nvram.qcow2`, `disks/tpm/`, the snapshots
  (internal to the qcow2 files; `snapshots.json` holds tags and names; their pictures are `snapshots/<tag>.png`), a
  saved session (`suspend.json`: QEMU's version, machine type, CPU, sizes; the state's stream names devices, not
  paths), `run.json`, `operation.json` (recovered first), `identity.json`, the logs and `thumb.png` are all found from
  wherever the folder is. A linked clone's frozen base is in `<library>/.shared/` (6.3), outside the folder, and its
  siblings keep naming it. The runtime socket is `$XDG_RUNTIME_DIR/lordos-vm/<id>.sock`, a bridged machine's macvtap
  `lvm<id>`: by id. The transfer folder's tag in the guest is always `LordOS`, so the guest's mount (`/mnt/LordOS`, the
  drive on the other system) doesn't change. A person's shares are outside the library (`vm_share_allowed`). Only an
  ISO or a drivers' disc kept inside the folder is a path in `machine.json` that points into it: rewritten.
- **While it runs** only the name changes: lord-vm holds the lock, so `lord-vm --rename` sends `VM_RENAME` to the
  machine's own lord-vm, which reads `machine.json`, writes it with the new name and keeps it for its later writes
  (sound). The answer says `"folder":"later"` when the folder would move. Moving the folder of a running machine (QEMU,
  virtiofsd, swtpm and the open windows all hold it) is never done, and nothing moves it by itself when the machine
  stops: once it's off, Configure → General shows **Rename Folder** under Folder whenever the name asks for another
  folder that is free, and `machines rename NAME NAME` does the same. A machine's lord-vm from before this answers "bad
  request": the command then says to shut it down first.
- **The app**: Configure → General's name can be changed while the machine runs; under it, the folder it will get, or
  why the name can't be used (Save waits). A rename's job says where the folder went (`machines.cfg.rn.*`). The
  machine's windows (its own, Configure, Snapshots, Logs) are instances named by the machine's id (`vm-<id>`,
  `cfg-<id>`…): they follow the machine to its new folder, at once after a rename here (`movedDirs`), within three
  seconds after one from the terminal (`useMachineDir`: the folder is gone, the library has one machine with that id).
  A machine window that only follows never starts the machine. Its `app.json` (system, notes, apps mode, profile) is
  inside the folder; Files' Open with and the apps mode are by id or by session; the library's order is by name, state,
  last use or size, not by folder; session restore doesn't reopen Machines windows. Nothing else stores the folder.
- **The command**: `machines rename NAME NEW_NAME` (NAME may be the id): the plan first, in the person's language, then
  `lord-vm --rename`; exit 1 when a name or folder is taken, 2 for a name that can't be one, 0 with "its folder is …",
  or, running, "once it's off, `machines rename "NEW" "NEW"` names it …".
- **Tests**: `build/vm-test` (names and folders, `vm_rename_plan` on a scratch library, `vm_transfer_adopt` with a
  Duplicate, a folder put aside and two candidates), `tests/vm_model_test.mjs` (`windowMachineId`, `followDir`,
  `renameNote`, `folderOffer`, `renameOutcome`) and `tests/vm_rename.sh` (`make test`; scratch machines only).

### 6.27 vCPU recommendation (built 2026-11-23, in development; approved by the maintainer 2026-11-23)

The maintainer's Windows 11 guest had 10 vCPUs and idled at 10 to 28 % of one core in apps mode with a text
editor open. Every vCPU is a host thread QEMU schedules, so a guest with many it doesn't use still costs the desktop. Machines
now measures what a machine's vCPUs really use and suggests a count; it never changes one by itself.

- **What lord-vm measures** (`server/src/vm/vmcpu.c`, `lordvm.c` `cpu_begin`/`cpu_tick`/`cpu_end`). QEMU runs with
  `-name ...,debug-threads=on`, so each vCPU thread is named `CPU <n>/KVM` (or `/TCG`). Right after the QMP handshake,
  before lord-vm's own Landlock (which allows only the machine's folder), lord-vm opens `/proc/<qemu>/task/<tid>/stat`
  of each of those threads and keeps the descriptors; once a minute it re-reads them with `pread` from offset 0 (nothing
  new is opened, so the Landlock rules don't change) and adds their user and system time. Only while the machine is
  running: a pause, a save to disk, a sample more than three minutes after the last (the computer slept) or one with
  fewer ticks than the last only starts the count over. The cost is one small read per vCPU a minute. QMP's
  `query-cpus-fast` gives thread ids (QEMU has no pid namespace of its own, 3.3) but no CPU time.
- **What it keeps**: `<machine>/cpu-usage.json`, `{"format":1,"windows":[[end, seconds, vcpus, avg, peak], ...]}`, one
  entry per 5 minutes of running time (the last of a run kept when it has a minute or more): its end in Unix seconds,
  its length, the vCPU count it ran with, the cores used on average and in its busiest minute, both in hundredths of a
  core. At most 288 entries (24 h of running time, about 9 KB), none older than 30 days, written as `.tmp` and renamed
  when a window closes and when QEMU ends. A clone doesn't copy it. Malformed entries are dropped on reading, in C and
  in the app.
- **The suggestion** (`apps/machines/model.js` `vcpuAdvice`, only windows at the machine's current count, of the last
  30 days, weighted by their length): nothing before 2 h of running time. **Fewer** when 95 % of the time it used under
  30 % of its vCPUs: enough for its busy minutes (95th percentile of the busiest minutes) at about 60 %, never under 2.
  **More** when half the time or more it used 85 % of them or over (a build now and then doesn't count): half again,
  at least one more, up to this computer's processors less one in eight for the desktop (at least one, never more than
  lord-vm recommends; `vcpuCeiling`). After a change the count starts over, since the old windows were at another count.
- **Where it shows.** Configure → CPU & memory: what it suggests, the reason ("over its last 6 h of running time it used
  0.2 of its 10 vCPU on average, and 1.1 in its busiest minutes") and **Use N**. With the machine off that is
  `lord-vm --set --vcpus N` at once; running or suspended it's kept in `app.json` as `nextVcpus` and written by
  `startPrep` (ui.jsx) just before the next start, whichever way it starts (window, without a window, the `machines`
  command); a suspended machine keeps it until a start after a full shutdown, since lord-vm refuses to change a saved
  machine's vCPUs. **Keep N** drops it. The library's card and list row show "Suggested: N vCPU" (or the count waiting
  for the next start), which opens that section.
- **New machines' defaults.** The profiles whose processors are "half" (Development, Games, Design) now give half of
  this computer's physical cores, at most 6 (`vcpuDefault`; `lord.vm.host().cores`, counted from
  `/sys/devices/system/cpu/cpu*/topology/thread_siblings_list` by `vm_host_cores`), instead of half its threads: 5
  on a 10-core, 20-thread PC instead of 10. Productivity stays at 4 and Light at 2; a Windows guest still gets
  at least 2, and a profile's own number is kept (up to half the processors, as before).
- Tests: `build/vmcpu-test` (the stat line, the vCPU thread names, the windows, the sleep and pause rules, the rolling
  limit, the file read back and refused; its own thread named `CPU 0/KVM` read through `vmcpu_open`/`vmcpu_ticks`),
  `tests/vm_model_test.mjs` (`cpuWindows`, `vcpuAdvice`: the maintainer's idle case, too little time, another count,
  old windows, the floor, a busy spell, saturation, the ceiling; `vcpuDefault`) and `tests/vm_profiles_test.mjs`.

### 6.28 Sentinela watches the machines (built 2026-11-23, in development; approved by the maintainer 2026-11-23)

Sentinela (docs/SENTINELA.md, "Machines"; off by default, local model only) reads what Machines already exposes every
10 s: `lord.vm.list()`, `lord.vm.stats(dir)` and per-machine socket counts from the Monitor's thread
(`lord.monitor.machinesWatched`). `stats(dir)` takes nothing of it from `run.json` (the person's file, a hint only):
it asks the machine's lord-vm over its socket as `lord-vm --status` does, and trusts the answer only when the peer
(`SO_PEERCRED`) is the person's installed `lord-vm` and the QEMU it names is its child. It then adds `qemuPid`,
`sandbox` and `cgMode` (lord-vm's answer), the kernel's `seccomp` and `noNewPrivs` for that QEMU, `scope` (`in` only
when QEMU and lord-vm are exactly in the session's `app-lordos-lordos-vm-<id>.scope` or below it) and `passt`; and
`verified: false` when the machine looks running but can't be checked, which Sentinela alerts as a sandbox problem.
All read-only; no new action on a machine, and lord-vm is unchanged (`src/vm/vmwatch.h`, `build/vmwatch-test`).

- **Always alerted, no model:** a QEMU moved out of its machine's scope; a sandbox missing Landlock, seccomp,
  no_new_privs or its cgroup; a Workstation of an anonymity group with any way out besides its link (19.3).
- **Toggles:** a machine far above its own usual CPU or memory; crash loops (3 unexpected ends in 15 minutes); for an
  anonymity group, a Gateway whose traffic or number of remote addresses jumps far above its own pattern, any
  unexpected end of a member, restarts and a pegged CPU.
- Alerts carry the machine's name and role; the model only gets counts and those words, never the guest's screen,
  files or clipboard, and can't silence an alert. Actions, each from the person's click: open the machine, show it in
  Monitor, pause or resume, shut down.

## 7. Guest tools

### 7.1 What each guest receives

| Need | Linux | virtio-win |
|---|---|---|
| Time, shutdown, IP, freeze | `qemu-guest-agent` | qemu-ga from virtio-win |
| Resolution, clipboard | QEMU's display (resolution); `lordos-guest` (clipboard) | `lordos-guest-win`, the LordOS guest tools (7.1.2) |
| Folders | Kernel | virtiofs driver + WinFsp |
| Apps mode | `lordos-guest` on a LordOS guest (7.2) | `lordos-guest-win` (7.1.2, 7.2) |
| Install | `qemu-guest-agent` from the guest's repo; `lordos-guest` written by lord-vm through it (Install Agent), or the `lordos` package in LordOS | virtio-win's tools from the attached ISO; `lordos-guest-win` written through qemu-ga, installed by itself after an unattended install |

Tool updates: the guest's own packages through its own updates; `lordos-guest` and `lordos-guest-win` updated by lord-vm itself when older than the shipped ones, on by default, with no snapshot (7.1.3); virtio-win at the pinned version.

### 7.1.1 Driver or agent, and what we would write ourselves

Copy and paste, drag, resolution and shell are **agents**: ordinary programs in the guest with no kernel signature to obtain. Graphics, disk and network are **drivers**: on virtio-win guests, kernel code that the guest refuses to load unsigned.

| Piece | First version | Our own later | Cost of our own |
|---|---|---|---|
| Linux disk, network, display, 3D | In the guest's kernel and Mesa, nothing to install | No reason to | None |
| Linux clipboard, resolution, files | `lordos-guest` (clipboard), QEMU's display (resolution), qemu-ga (files); no SPICE agent | Done: `lordos-guest` | Ours, ordinary packaging |
| virtio-win disk, network, memory, 2D display, folders | The public virtio-win set | No: they are mature and free | Would add risk and nothing else |
| virtio-win clipboard, resolution | `lordos-guest-win` (7.1.2), user mode, built | Done | A code signing certificate would only spare SmartScreen's warning, which a program qemu-guest-agent installs never meets |
| virtio-win file copy, shell, logs, Apps mode | Our agent, phase by phase | Yes | Same |
| virtio-win 3D | None | Only by following or joining the open effort below | Very high |

**3D on virtio-win guests.** No stable free driver exists. An open effort pairs a WDDM kernel driver with a Mesa user-mode driver over virgl; its Mesa and driver changes were still unmerged and described as glitchy and experimental when checked on 2026-11-01. Its driver also turns preemption off across the guest, which is a real limit. We don't ship it. We re-check it each phase, and it would come as an opt-in with a clear warning before it could be a default. Graphics passthrough of a whole GPU is a separate route, needing VFIO and a second GPU, and it isn't this.

**Signing our own kernel driver.** A driver loaded by current virtio-win guests needs the system maker's signature: an organization account in its hardware portal, an EV certificate for that account, then attestation signing or, to reach its retail update channel, certification with the hardware lab kit. That is a yearly cost and a legal entity behind it, for every architecture. So own kernel drivers are out of scope unless 3D makes them worth it. Our work goes to the agent, which needs none of this.

### 7.1.2 LordOS guest tools for Windows (`lordos-guest-win`, built 2026-11-20)

One user-mode program, `tools/guest-win/lordos-guest-win.c`: Win32 only, cross-built with mingw-w64 on the build machine
(`server/mk/vm.mk`; 184 KB, it imports only system DLLs: kernel32, user32, gdi32, shell32, ole32, oleaut32, WindowsCodecs,
dwmapi, wtsapi32, bcrypt, powrprof (w11), msvcrt). It is the Windows twin of `lordos-guest`: it holds `org.lordos.agent`
(`\\.\Global\org.lordos.agent`, vioserial) and speaks `vmagent.h`'s frames. lord-vm checks everything it sends as it
checks the Linux helper (it is the guest: untrusted); the program checks every frame the host sends with the same
parsers (`vmagent.c`) and never runs anything the host names.

- **Clipboard** (6.8): text (CF_UNICODETEXT ↔ UTF-8, CRLF ↔ LF), rich text ("HTML Format" ↔ HTML, sanitized by lord-vm
  as from Linux), pictures (the "PNG" format or CF_DIB/CF_BITMAP ↔ PNG, encoded and decoded with Windows' own WIC
  codecs). A host clipboard is offered with every form it has (text, HTML, PNG and CF_DIB) by delayed rendering: the data is fetched only when something pastes (`AGK_CLIPOFFER`, `AGK_CLIPREQ`), and the offers are kept out of clipboard history and the cloud clipboard. Changes are seen at
  once (`AddClipboardFormatListener`), not polled. The machine's direction (machine.json) is enforced in lord-vm.
- **Resolution follows the window.** viogpudo (the VirtIO GPU display-only driver) builds its mode list once at boot
  (640×480, 1024×768, the EDID's standard modes) plus one *custom* mode that follows QEMU's display size: QEMU's
  `ui_info` (lord-vmdisplay's `SetUIInfo`) raises VIRTIO_GPU_EVENT_DISPLAY, the driver rewrites that mode and tells
  Windows a monitor changed, but nothing in Windows switches to it (virtio-win's own `viogpuap` service does; it isn't
  part of 0.1.271's guest tools). lord-server now also sends the window's size to lord-vm (`VM_DISPLAY_SIZE`), which
  passes it to a helper that said "display" (`AGK_DISPLAY`; again after each hello). The helper switches the primary
  display to it: a listed mode with `ChangeDisplaySettingsExW`, else, as virtio-win's own `viogpuap` does, through the
  display configuration (`QueryDisplayConfig`, the source mode's size, `SetDisplayConfig` with SDC_ALLOW_CHANGES, saved
  to Windows' database), which makes Windows ask the driver again and take its custom mode. It tries every 250 ms for
  4 s (the driver's change may arrive after the helper's request); after 1 s it also asks the driver itself
  (`D3DKMTEscape` VIOGPU_SET_CUSTOM_RESOLUTION, drivers from 2026 on; 0.1.271 answers STATUS_INVALID_PARAMETER), and at
  the end it falls back to the largest listed mode that fits. It answers with the size it set. The framebuffer is sized at
  boot (at least 16 MB, so about 4 million pixels): a size bigger than that waits for the next start. No driver is
  patched or re-signed (Secure Boot needs Microsoft-signed drivers).
- **Apps mode, the guest side** (7.2, `LAG_*` in `vmagent.h`): the session's top-level windows (visible, not cloaked,
  not tool windows; never the taskbar or the desktop) in z-order with id, rectangle, flags, owner, pid, title and
  program name, sent on change (`SetWinEventHook`, at most 30 a second) and on request; each window's icon once (32×32
  RGBA); focus, close, minimize, restore and move of a window it listed; the Start menu's own program list
  (`shell:AppsFolder`: desktop and Store apps), and starting one only by a token the helper itself listed. While apps
  mode is on the taskbar is set to hide itself, and restored after. A Store app's window belongs to
  `ApplicationFrameHost.exe`, which draws the frame of every one of them: the helper reports it with the hosted app's
  name and icon instead, from its AppUserModelID (the frame's `PKEY_AppUserModel_ID`, else `GetApplicationUserModelId`
  of the process that owns the `CoreWindow` inside it) looked up in `shell:AppsFolder`, so Calculator and Settings are
  each their own Dock entry with their real icon. A frame whose app isn't known yet waits up to 3 s before it's listed.
- **Sessions.** One copy per session (a `Local\` mutex). The port opens once per machine: the copy in the session at
  the console holds it and lets it go when that session leaves the console (switch user, Remote Desktop) or logs off;
  the next one takes it within 2 s.
- **Installed by lord-vm** (Configure's agent popover: "Install LordOS Tools"; and by itself when an unattended
  install finishes): through qemu-guest-agent, as SYSTEM, the program is written into a private SYSTEM-only folder in
  `%SystemRoot%\Temp` (the drop's), its SHA-256 checked against what lord-vm sent, copied into `%ProgramFiles%\LordOS`
  (made or checked: not a reparse point, protected ACL: SYSTEM and Administrators full control, Users read and run),
  started at every sign-in by `HKLM\...\Run`, and started now in the signed-in person's session by a one-shot scheduled
  task (their Interactive logon; the task goes at once). vioserial lets only SYSTEM and Administrators open a port: the
  installer gives Interactive users (IU) read and write on `org.lordos.agent`'s port alone (found by what
  `\\.\Global\org.lordos.agent` points to; the device's own security property, kept across restarts) and restarts
  that port. After the install nothing runs as SYSTEM but the pointer's script below, once per run: no service.
- **The pointer, drawn by LordOS** (w5, 2026-11-21; docs/MACHINES-GPU.md Part 1 step 2). viogpudo draws the pointer into
  the picture unless its `HWCursor` value is 1 (upstream ships 0), so every pointer move was a round trip through the
  guest before it showed: 35 to 39 ms median, 45 to 47 ms p90, up to 97 ms (**Measured**, `lordvm-dev lat`, 100 moves).
  Once per run, when the tools say `pointer` in their hello, lord-vm runs one fixed script as SYSTEM through
  qemu-guest-agent (`PTR_PS` in `lordvm.c`): for each present `VioGpuDod` display adapter it sets the DWORD `HWCursor`
  on its driver key (`HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}\NNNN`, found
  through `DEVPKEY_Device_Driver` and checked by pattern) to the machine's choice and restarts only an adapter whose value
  changed (`pnputil /restart-device`: the guest's screen blinks once). The tools themselves can't write that key (they run
  as the person) and still never run elevated. With 1 the driver hands the shape over (QEMU's `CursorDefine`, `VD_CURSOR`;
  Windows' 64 × 64 arrow), lord-server draws it where the host's pointer is and stops treating the machine as one that
  draws its own (`cursor_guest_draws`); a move makes no frame (0 of 100) and its position comes back in 1 ms.
  Configure → Display → *Pointer drawn by*: **LordOS (faster)** (the default) or **The machine** (machine.json schema 10
  `"pointer": "guest"`, written only then; `lord-vm --set --pointer lordos|guest`), applied at the next start; with
  *The machine*, a shape QEMU still holds from a saved state isn't drawn over the guest's own. Machines whose tools are
  older than w5 are offered the update. **Checked** on `win-tools` (viogpudo 100.100.104.27100): w5 installed → "LordOS
  draws it (HWCursor 1, already so)"; *The machine* and a start → HWCursor 0, adapter restarted, every move a frame again;
  back → HWCursor 1, restarted, the arrow handed over. Not tested: a guest with two viogpudo adapters, an older viogpudo
  without the value.
- **Log**: `%LOCALAPPDATA%\LordOS\lordos-guest-win.log`, the person's own, started again past 256 KB.
- **Intel graphics (`gvt` in the hello):** with it, lord-vm may install Intel's GVT-g driver in the guest, hold back its
  driver updates and turn hibernation off, through qemu-ga (`vmgvtwin.c`, 6.10.1, docs/MACHINES-GPU.md). The hello's
  full list is `clipboard clipx clipreq display windows open place pointer gvt extra tablet` (`lordos-guest-win.c`).
  Options: `--port`, `--log`, `--version`.

Measured on 2026-11-20 on a Windows 11 clone of the unattended install (`win-tools.lordvm` from `win-base.lordvm`;
virtio-win 0.1.271, viogpudo 100.100.104.27100), driven by `lordvm-dev` (`agent install`, `resize`, `png`, `clip-*`,
`apps`, `winact`, `launch`), not yet inside lord-server's window:

- Install through qemu-guest-agent: 12 to 36 s; the program answers in the same session at once and after a restart
  (`Run`). Right after the install the port's device has just been restarted and vioserial refuses to keep a read
  waiting (ERROR_OPERATION_ABORTED) until the machine starts again; the helper then reads by asking every 30 ms.
- Resolution, each exact in the picture lord-vmdisplay received, 0.25 to 0.5 s after the request: 1784×830,
  1920×1080, 1366×768, 1001×777, 2000×1100, 1280×800 (before the display-configuration step, sizes Windows hadn't
  listed fell back to 1280×720 and 800×600).
- Clipboard both ways: UTF-8 text with ñ, ú, € and an emoji (code points checked in the guest), CRLF to LF; HTML (a
  `<script>` and an `onclick` taken out by lord-vm); a 97×61 PNG into the guest (CF_DIB pixel checked) and a 123×45
  picture out of it (PNG pixels checked).
- Apps mode: 74 programs listed (desktop and Store apps), Notepad and Calculator started by token, the list with
  titles, programs and focus, 32×32 icons, move to an exact rectangle, focus, minimize, restore and close; an unknown
  token and an unlisted window ignored; the taskbar hidden while on and back after.
- Not tested: switching between two people's sessions, Remote Desktop, a machine with a multi-monitor guest.

**w12 (2026-11-24, *in development*):** "shell links notify" in the hello: the agent's shell, PowerShell in a pseudo
console as the person and never elevated (6.18), and `lordos-guest-win.exe --open-on-host URL` / `--notify-host TITLE
[BODY]`, which hand this session's running tools one link or notification by `WM_COPYDATA`, passed on only while the
host's hello says it takes them (6.12). Cross-built with mingw-w64 (`build/lordos-guest-win.exe`); not yet run in a
Windows guest.

### 7.1.3 Keeping the LordOS tools up to date (since 0.8.9.5)

A guest keeps the tools it was given: a LordOS upgrade ships newer ones (`TOOLS_SHIPPED` in `apps/machines/model.js`,
`w12` and `8` today (*in development*: the agent's shell, links and notifications; before them `w11` and `7`), checked
against the sources by `vm_model_test`), but before this a guest ran its older tools, with
their known bugs, until someone found **Update LordOS Tools** in the agent's popover. Now Machines updates them by
itself (`toolsAutoStep`, run every second by the machine's window, `vmwindow.jsx`):

- **When.** The machine runs, its agent has been connected for 20 s, the tools said hello with an older version of
  the same kind (`w<n>` Windows, `<n>` Linux; a guest without the tools is never given them by itself), and nothing is
  on its way: no files copying (drops, "Open with"), no transfer-folder setup, no app of apps mode opening (its launch
  still waiting for a window), no install the person asked for. Otherwise it waits for the next second.
- **How.** The same install as the button: `lord.vm.agent(id, 'update')` is `VAG_INSTALL` with `b = 1`, so lord-vm
  writes `agent: Machines updates the LordOS tools in the guest by itself (they said w7)` in the machine's log before
  its usual install lines. No new path, privilege or program: the approved guest install (7.1.2, AGENTS.md) is all
  it uses.
- **How often.** At most three attempts per run of the machine and per shipped version, 2 and then 10 minutes apart;
  an attempt with no answer in 10 minutes failed. An install that worked but whose tools still say the older version a
  minute later is never tried again in that run. The count lives for the run, not the window: closing and reopening
  the window (or an upgrade of the host while the machine kept running) doesn't start it over unless the version to
  bring changed; a new start of the machine does.
- **Off, or given up.** One notification per run and version: "The LordOS tools in ‹machine› are out of date", with
  **Update** (the same install) and a click that brings the machine's window to the front. The agent button keeps its
  blue dot while they are older.
- **Settings.** Machines settings → General → **Update the LordOS tools in machines automatically** (on by default;
  `machinesDefaults.toolsAuto`), and per machine Configure → Guest agent → **Update the LordOS tools in this machine
  automatically** (`app.json` `toolsAuto`; while it doesn't say, the default applies). It applies at once, also while
  the machine runs.
- **The popover** shows the tools on their own line, apart from the system and qemu-guest-agent's version:
  "LordOS tools w9 (up to date)", "LordOS tools w7 → w9 available", or "… updating…" while the install runs.

Limits: only a machine whose window (or apps mode) Machines has open is watched; one kept running with its window
closed is updated the next time its window opens. Tested by `server/tests/vm_model_test.mjs` (when, once per run,
backoff, never a loop, settings, the notification); not yet on the dev VM with a real w7 guest.

### 7.1.4 The guest's performance profile (tools w11, Linux helper 7, in development)

Approved by the maintainer on 2026-11-23 (Machines performance, item 5; Windows Search and Linux guests approved the
same day, below). A per-machine switch, Configure → Guest agent → **Performance profile**, off by default and shown for
every guest (its note says what happens in a Windows guest, `machines.win.perfProfileNote`, or in a Linux one,
`machines.cfg.perfProfileNote`): machine.json schema 13
`"perf_profile": true` (written only when on), `lord-vm --set DIR --perf-profile on|off`, `lord.vm.set(dir, {
perfProfile })`, read back as `perfProfile` by `lord.vm.list`. Like the other Configure switches it changes while the
machine is off. It isn't on by default in apps mode either: it changes how the guest looks to the person (no
animations, no transparency, window outlines while dragging), which they should choose.

**Channel.** Only the existing `org.lordos.agent` port, no new program or privilege. Tools that say `perf` in their
hello get `AGK_PERF` (kind 23, exactly one byte: 1 on, 0 off; `ag_perf_parse`) after every hello, off included, so a
guest whose tools kept the profile from an earlier run gets its own settings back. They answer `AGK_PERFSTATE` (kind
24, exactly 3 bytes: on, the `AGPF_*` parts done, the parts the system refused; known bits only, never one part in
both; `ag_perfstate_parse`), which lord-vm only logs: `agent: the guest's performance profile is on (applied: power
plan visual effects transparency; refused: nothing)`. The host names no command, path, value or plan.

**What `lordos-guest-win` w11 does**, as the signed-in person (it never runs elevated) and only while the host holds the
port:

| Part (`AGPF_*`) | How | Put back |
|---|---|---|
| Power plan (`POWER`) | `PowerGetActiveScheme`, then `PowerSetActiveScheme` to High performance (`8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c`, powrprof). A standard user may switch plans unless a policy forbids it; a refusal or a missing plan is "refused" | The earlier plan, if High performance is still the active one |
| Visual effects (`EFFECTS`) | `SystemParametersInfo` without `SPIF_UPDATEINIFILE` (this session only; the person's profile keeps its own values): animations inside windows, minimize and maximize animation, menu, combo box and tooltip animation, smooth list scrolling, selection fade, pointer and window shadows, window contents while dragging | Each effect that was on and is still off |
| Transparency (`TRANSPARENCY`) | `HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize` `EnableTransparency` 0, then `WM_SETTINGCHANGE` "ImmersiveColorSet" | Its earlier value (or no value, as before) if it is still 0 |

What was there before is kept in `HKCU\Software\LordOS\GuestWin` `PerfSaved` (a fixed binary record: magic, size,
parts; read back checked, an unreadable one is dropped). It goes back when the host sends 0, when the tools let the
port go (log off, another session at the console, the host gone, exit) and when they start and find a record (the
process was killed). A part goes back only while it is still what the tools set: a change the person made in between
stays. A part already at its minimum counts as done and is left alone.

**Windows Search (built 2026-11-23, approved by the maintainer the same day).** Stopping or lowering the `WSearch`
service needs administrator rights, which the tools never have. So lord-vm runs one more fixed PowerShell as SYSTEM
through qemu-guest-agent, once per run when the tools say `perf` (like `PTR_PS`): `PERFWIN_SEARCH_ON` when machine.json
has `perf_profile`, `PERFWIN_SEARCH_OFF` when it hasn't (`server/src/vm/vmperfwin.c`). Both are compile-time constants;
the on/off bit picks one and nothing from the guest, the person, JS or machine.json goes in.

| Script | What it does |
|---|---|
| On | When `WSearch` starts automatically (`Start` 2, `DelayedAutostart` or not), that start type goes into `HKLM\SOFTWARE\LordOS\GuestWin` `WSearchStartWas` (DWORD 2 automatic, 18 automatic delayed) and the service is set to Manual (`sc.exe config WSearch start= demand`; if that fails the value goes again). Manual with a value kept: ours from an earlier run. Manual without one, or Disabled: left as it is. Then the service is stopped if it runs (`Stop-Service -Force`; not when Disabled) |
| Off | With a value kept and the service still Manual (what the script set): the start type goes back (`auto` or `delayed-auto`) and the service is started. A start type changed in between stays. The value goes either way; one that isn't 2 or 18 is dropped |

A named mutex (`Global\LordOS-wsearch`) keeps two runs apart. Each prints one line, `on|off <start type> <service>`
(`set|ours|manual|disabled|restored|changed|nothing|dropped|none|busy` and `stopped|idle|started|notstarted|-`, only the
16 combinations the scripts can print), read by `perfwin_parse` and only logged, e.g. `performance profile: Windows
Search set to start manually (its automatic start kept to put back), stopped`. So while the profile is on the indexer
stays off; it comes back at the first start with the profile off (Windows may still start a Manual service on demand,
for example when the person searches).

**Linux guests (built 2026-11-23, approved by the maintainer the same day; `lordos-guest` 7).** The helper says `perf` in
its hello and, on `AGK_PERF`, speaks D-Bus itself (`tools/guest/dbusmin.c`: a Unix socket only, never TCP or an abstract
address; EXTERNAL authentication; method calls built from its own constants; every reply checked, lengths, padding,
header fields and signatures, before a value is read; nothing else on the buses is answered). It starts no new
program (`tools/guest/perf.c`):

| Part (`AGPF_*`) | How | Put back |
|---|---|---|
| Power profile (`POWER`) | System bus only (`/run/dbus/system_bus_socket`): power-profiles-daemon's `ActiveProfile` (`org.freedesktop.UPower.PowerProfiles`, else the older `net.hadess.PowerProfiles`) read, then set to `performance` through `org.freedesktop.DBus.Properties.Set`; polkit lets the active session switch profiles. No daemon, or a refusal (a guest whose daemon has no `performance` profile, which is common in a VM): refused | The earlier profile, if `performance` is still the active one |
| Animations (`EFFECTS`) | Session bus only (`DBUS_SESSION_BUS_ADDRESS` `unix:path=`, else `$XDG_RUNTIME_DIR/bus`), only when `XDG_CURRENT_DESKTOP` names GNOME: `org.gnome.desktop.interface` `enable-animations` read through the desktop portal (`org.freedesktop.portal.Settings.ReadOne`, else `Read`), and set to false through dconf's own service (`ca.desrt.dconf.Writer.Change` on `/ca/desrt/dconf/Writer/user`, the path GSettings takes; the change set is GVariant `a{smv}` built byte for byte as GLib builds it). KDE and other desktops: refused | Its earlier value, if it is still false |

What was there before is kept in `~/.local/state/lordos-guest/perf` (0600 in a 0700 folder; a fixed three-line text
record, `power <profile or ->`, `anim <0, 1 or ->`, read back strictly, an unreadable one is dropped). It goes back on
`AGK_PERF` 0, when the port goes (the host's lord-vm gone), when the helper ends (SIGTERM at log off, SIGINT, SIGHUP;
the helper used to die at once on these) and at its next start after a crash. A part that couldn't be put back (a bus
not there any more) stays in the record and is tried again at the next start or the next 0. A part already at its
value counts as done and is left alone; lord-vm logs them as "power profile" and "animations". The calls wait 3 s at
most (5 s for the portal and dconf, which the session bus may start for the call). Built with `-DLG_NO_PERF`, the helper
has none of this and its hello doesn't say `perf`.

Tests: `build/vm-test` (schema 13 and its round trip; `AGK_PERF`; all 2^24 three-byte `AGK_PERFSTATE` payloads
against the rule; the `perf` hello word; both search scripts are constants with no format or argument; all 100
`on|off <start type> <service>` lines against the rule written out again, and malformed ones), `build/guest-dbus-test`
(the method calls, every reply prefix and single-byte change, nested variants, errors, big-endian replies, dconf's change
set against GLib's bytes, the record), `tests/guest_perf_test.sh` (two private dbus-daemons, the real dconf-service for
a temporary home, stand-ins for power-profiles-daemon and the portal: on, off, the port going, SIGTERM, a killed run put
back at the next start, a part changed in between, a refused profile, the older bus name, the portal's `Read`, KDE, no
buses), `tests/vm_model_test.mjs` (`TOOLS_SHIPPED` is the sources' w11 and 7). Built with mingw-w64 with no warning;
it imports only system DLLs (powrprof.dll added). The tools' part was checked in the maintainer's Windows 11 guest on
2026-11-23 (`performance profile is on (applied: power plan visual effects transparency; refused: nothing)`). Not yet
checked in a running guest: the Windows Search scripts and the Linux helper's part.

### 7.2 Apps mode (seamless)

**Built (2026-11-20, since 0.8.9.5).** Each window of the guest is its own LordOS window: its title (with the machine's name, so a guest's window is always told apart), its app's icon in the Dock, the taskbar and the switcher (one Dock icon per app of each machine, `vm:<machine>:<app>`), and the guest's own desktop hidden. The machine's window becomes a controller panel (about 420 × 520; since 0.8.9.5, 2026-11-22, like VMware Fusion's Unity launcher): the machine's name and **Leave Apps Mode** (Esc there leaves too, after clearing the search); **Running**, the guest's own windows (not its popups or owned windows), each with its icon (the one the Dock shows, `LAG_WINICON`) and title: a click brings its LordOS window to the front (restored if minimized; the guest follows as from the Dock), × asks the guest to close it (`LAG_WINACT` close, as the window's own close button: the guest may ask about unsaved work); and the guest's apps (`LAG_APPLIST`) with a search (each word, accents and case ignored, names that start with it first; Enter opens the first), each with a plain initial since the guest lists no app icons. The menu bar's **Applications** menu lists the same Running windows above the apps. The lists' logic is `runningWindows`, `appsMatching` and `appInitial` in `model.js`. Each machine in apps mode also has a **status item by the clock** (`apps/machines/appsstatus.jsx`, since 0.8.9.5, 2026-11-22, like Fusion's Unity menu in the menu bar): an SDK status item (`registerStatusItem`, so every theme that draws `<StatusItems />` shows it on each bar: Lord Flow's menu bar, the Classic taskbar when horizontal, Workspaces' top bar), its glyph the machine's mark (`OsMark`) and its tooltip the machine's name. Its menu works like the guest's Start menu: a search (focused; Enter opens the first match, Esc clears then closes), Running (hidden while searching), the guest's apps, then the machine's name, **Show the Machine's Window** and **Leave Apps Mode**; it uses the same list components as the controller (`appslist.jsx`), and while it is open a guest window in front doesn't take the keys. An app row takes an icon (`launcherApps`: `appIcons` by token) once the guest sends app icons; until then the initial. The item goes when apps mode ends (`appsStatusDiff` over `guests`). Both launchers give feedback: a launched app's row shows **Opening…** and ignores clicks (`launchGuest`, also for Enter and the Applications menu) until a new window of that app is listed (`windowOfApp`: its program's name holds a word of the app's name, or its title the whole name) or 5 s pass, so an app whose window opens out of sight isn't launched again and again; the status menu closes when that window appears. After ×, the row shows **Closing…**, and a note when the window is still listed 3 s later or the guest reports a refusal (`closeRefused[gid] = { reason, at }` in `guests`, reason `elevated` for a Windows window that runs as administrator: the tools' report is to come; `closeView`). The toolbar's Apps button and **Actions → Apps Mode** turn it on; the choice is kept per machine (`app.json` `appsMode`) and the window opens in it next time. It's offered only when the agent is connected and the guest's helper says `windows` in its hello (`LAG_CAP_WINDOWS`, 256, in `VM_AGENT_STATE`); otherwise the button says why: the agent is off, or the guest needs LordOS or Windows inside, with the LordOS tools.

**How.** The guest's helper lists its windows (`LAG_WINDOWS`: id, place in the guest's pixels, focused, minimized, on top, owner, pid, title, app; at most 64), their icons (`LAG_WINICON`, at most 64 × 64 RGBA) and its apps (`LAG_APPLIST`, name and an opaque token), all in `server/src/vm/vmagent.h`. lord-vm parses each with strict parsers (exact lengths, counts, sane rectangles, UTF-8 without control characters), writes it again and hands it to lord-server as a sealed memfd (`VM_WINDATA`), which parses it again. The host sends `LAG_APPS` (on/off: the guest hides its own desktop), `LAG_WINACT` (focus, close, minimize, restore, move to x, y, w, h) and `LAG_LAUNCH` with a token the guest listed (lord-server names the app by its place in the list and the list's generation; lord-vm sends the token from its own copy). The guest's screen is asked to be as large as this display (`lord.vm.resize`), so no window is cut. Each LordOS window shows its part of the machine's one picture: `<MediaView crop={[x, y, w, h]}>` (`Node.crop`, `media/mview.c`), drawn straight from the shared picture (2D) or the picture read back from the 3D dmabuf, copied as rows at 1:1; a new frame damages each window only where its part changed (`media_src_damage_rect`), and a frame that touches no shown part counts as read at once. The pointer goes to the guest at the window's place (`cropPoint`), the keyboard while the window is in front. Moving or resizing the LordOS window moves the guest's (`move`), minimizing, focusing and closing do the same in the guest; a window the guest moves or resizes itself is followed. A menu or tooltip (owned and on top) is a small frameless window over the others.

**LordOS guests.** `lordos-guest` asks the guest's own lord-server through a local endpoint for its user only (`server/src/ui/js_guestwin.c`, `$XDG_RUNTIME_DIR/lordos-guestwin.sock`, same uid, only inside a Machines guest), which lists the windows of its window manager, applies the listed actions and launches only its launcher's apps; in apps mode it hides its Dock, bar and wallpaper. **Windows guests:** `lordos-guest-win` (7.1.2): it lists the top-level windows (Win32), hides the taskbar in apps mode, and launches only a Start menu shortcut it listed. **Other Linux systems** (GNOME, KDE on Wayland) don't list their windows: apps mode isn't offered there, and the button says it needs LordOS or Windows in the machine.

**The work area.** The guest's screen is laid over the whole display, so a guest window near its top edge would put its LordOS title bar under the menu bar (a Store app opens at y = −10). Such a window opens, and follows the guest, just below the menu bar (or a bar at the start side), and the guest is asked to move its window there too (`insideWork` in `model.js`, once per place): the window keeps showing its own part of the picture 1:1, and if the guest keeps it where it was the crop is still its rectangle.

**More than one display (fixed 2026-11-21).** The guest has one screen, laid over the main display, but its LordOS windows can go anywhere: a window dragged to another monitor (x past the main display, or negative for one to its left) used to ask the guest to move its window off its own screen, where Windows hid it, its list stopped naming it and the LordOS window closed. Now the place asked of the guest is always inside its screen (`guestPlace` in `model.js`: the size kept as far as it fits, the position clamped), and the LordOS window keeps where it was put: it remembers how far it is from the guest's window (`off`) and follows the guest's moves by that much; its menus go with it. A guest window that the guest minimized right after such a move is restored, not minimized here too (`minimizedByUs`). The tools clamp a move to the monitor too (w4), so an older host can't push a window away either. Sizes go through the main display's scale, whatever the monitor's own scale.

**A guest with more than one display (fixed 2026-11-22 in 0.8.9.5, tools w8).** Windows can have more than one display on its desktop while the machine's window shows one picture: on the maintainer's GVT-g guest a 1022 × 724 primary that isn't on a PCI adapter (a basic or indirect display) sat next to the vGPU's output, the one the host shows. The tools used the primary and the virtual desktop for everything, so the size the host asked for was never set, each LordOS window showed the wrong part of the picture, and the moves the host asked for put the guest's windows on the display nobody sees. `lordos-guest-win` now picks the **shown display** (`ag_shown_pick`, `vmagent.c`, tested in `vm-test` with that guest's displays): the host's picture only ever comes from a PCI GPU (GVT-g, virtio-gpu, QXL, a passed-through card), so a display on a PCI adapter always wins over one that isn't; among those, the one chosen before by size while it stays, else the one whose mode is the size the host asked for (the primary first), else the primary, else the first; a change of which displays there are forgets the choice. (w7 chose by size alone and locked onto the basic display, which had the size asked at hello.) The shown display is made Windows' **primary** once per topology (`SetDisplayConfig`: its source at 0,0, the others in a row to its right, saved; what Settings' "Make this my main display" does), so the taskbar, the desktop's icons, new windows and a **UAC consent prompt** (Windows shows it on the secure desktop, on the primary display) are in the picture. Nothing is detached: a wrong guess costs only a primary that can be set back in Settings. The size (`AGK_DISPLAY`) is set on the shown display, and apps mode's rectangles (`LAG_WINDOWS`, `LAG_WINACT`'s move, `AGK_PLACE`'s point) are relative to its corner and kept inside it. A window on another display is moved onto it as the tools start, after the primary changes, as apps mode starts (or when the window appears) and when apps mode ends: the same place relative to the display's corner, its size kept, then clamped to the work area; a minimized one stays minimized (only where it comes back moves), a maximized one is maximized there; a window on no display at all, or one running as administrator, is left alone. The tools send the displays at hello and on every change (`AGK_DISPLAYS`, for the log only; strict parser, at most 8, printable ASCII names of at most 32 bytes): lord-vm logs `agent: guest displays: \\.\DISPLAY2 1920x1080 at 0,0 primary (shown); \\.\DISPLAY1 1022x724 at 1920,0 (not a PCI adapter)`, and the tools' own log lists every adapter and monitor, each choice and the primary made.

**Clicks on a guest with more than one display (2026-11-22, 0.8.9.5).** A guest spreads its absolute tablet over its whole desktop (Windows maps a HID tablet over every display), while QEMU's `Mouse.SetAbsPosition` scales by the picture's width: with the extra display on, a click landed scaled across both. lord-vm passes where the picture sits in the guest's desktop (`ag_displays_layout`, from `AGK_DISPLAYS`) as `VM_GUEST_LAYOUT` → lord-server → `VD_LAYOUT` → lord-vmdisplay, which gives QEMU the position of the shown display's pixel (`vd_abs_map`, within 2 pixels: QEMU takes whole picture pixels). No layout (one display): as before.

**Correction (tools w10, 2026-11-22, 0.8.9.5).** The paragraph above had it wrong: Windows maps a HID tablet (mouhid) over its *primary* display only (its input carries no `MOUSE_VIRTUAL_DESKTOP`). With the shown display primary at 0,0 beside a second one, the whole-desktop mapping put a click at about half its x. lord-vm now computes the layout with `ag_tablet_layout`: with the shown display primary there is no layout (the picture is all the tablet covers, 1:1); with another display primary the tablet can't reach the picture (logged once; the tools make the shown one primary again). The tools read the tablet's raw input in the background (`RIDEV_INPUTSINK`, mice only, nothing taken) and say how the guest maps it (`AGK_TABLET`, 21, one byte: 1 primary, 2 whole desktop); lord-vm uses the whole-desktop layout only on a 2. Their log shows the first three moves with where the pointer went against both readings. The tools also keep the shown display primary after every change of mode (at most once in 2 s and five times a minute), ask the display configuration twice at most per size (it was every 250 ms for 4 s: a mode set each time), ask again, twice at most, for a size the guest changed by itself right after (a vGPU monitor back with a new EDID), and make apps mode's list with the displays as they are at that moment, sent again after every change of the displays; Machines shows a window's picture black after a change of the picture's size until that list comes (1.5 s at most), never a wrong part.

**Frameless apps mode windows (2026-11-22, 0.8.9.5).** A guest window has no LordOS frame (`args.csd`): it is the guest's window with its own title bar and caption buttons, which minimize, maximize, close and move it in the guest, and the LordOS window follows the guest's next list. While a button is held on it, a point is mapped as the window was at the press (`anchorPoint`), so dragging the guest's title bar doesn't run away from the pointer. The machine's menus stay in the menu bar, the Dock's menu, the panel and the status item; the panel carries the machine's bar (less the resolution, Capture mouse and full screen).

**Turn Off the Extra Display (tools w9).** When the guest has a display that isn't on a PCI adapter next to the PCI one the picture shows (`VAC_EXTRA`), the agent's popover offers to turn it off: `VAG_EXTRA_OFF` → lord-vm → `AGK_EXTRA` (one byte) → the tools leave its paths out of the display configuration (never the shown one's), saved, and remember it (`HKCU\Software\LordOS\GuestWin` `ExtraDisplaysOff`, hello word `extraoff`, `VAC_EXTRA_OFF`); **Turn the Extra Display Back On** (`VAG_EXTRA_ON`) extends the desktop over every connected display again, which the tools also do by themselves when no PCI display is left. It isn't automatic because what that display is (on the maintainer's guest, a 1022 × 724 display beside the GVT-g output) isn't known for sure; the tools log its adapter's name and id. The popover shows the LordOS tools' version on its own line, apart from qemu-guest-agent's (7.1.3).

**A window that runs as administrator (2026-11-22, 0.8.9.5, tools w8).** The tools run with the person's normal rights (they are never elevated), so Windows drops their messages to a program running as administrator (Computer Management, `mmc.exe`) and refuses its moves (UIPI). Such a window is flagged in the list (`LAGW_ELEVATED`, set only when the host's hello says `elevated`); a close, move, minimize or restore that is refused is answered (`LAG_WINREFUSED`: window, action, reason), which lord-vm logs and passes on (`VM_WINREFUSED`); lord-server keeps the last refused close per window (`lord.vm.closeRefusals(session)`, `closeRefusedSeq`), and apps mode puts it in `guests[dir].closeRefused` so the window says to close it from inside the machine.

**The clipboard in apps mode.** The same gate as the machine's window (6.8): a guest window in front counts as the machine in use (`guestFocus`), the offer goes in when one comes to the front or is clicked, and a click that carried a new offer reaches the guest `CLIP_HOLD_MS` later, so a right-click's Paste finds it (2026-11-21). Checked on the card's guest with `lordvm-dev`: an offer pasted into Notepad in apps mode (one request, answered) and Notepad's copy came back.

**Not yet.** Guest apps in the launcher before the machine runs (6.12); a window's own shape (a round or see-through window shows the guest's black around it); the guest's own second screen.

### 7.2.1 Files dropped on an apps mode window

**Built (2026-11-21, since 0.8.9.5).** Files dragged from Files onto one of a guest's windows go to that window, not only to the machine's Downloads: into the folder an Explorer window shows (moved there from Downloads under their own names, " (2)" when taken), dropped on a window that takes dropped files (`WS_EX_ACCEPTFILES`: `WM_DROPFILES` at the drop point; Notepad opens the file), or, for any other window, put on the guest's clipboard as files (`CF_HDROP`, "copy") with a notice: "The files are in ‹machine›: paste them with Ctrl+V". The copy is a normal drop (the transfer folder or the agent's way, size and SHA-256 checked, the window's own progress line), at most 8 files at once (`AG_PLACE_MAX`), folders left out.

**The path.** `GuestWindow` takes the drop (Files now passes the drop point), maps it into the guest's screen (`cropPoint`) and calls `lord.vm.sendFile(session, path, { place: { win, x, y, batch, index, count } })` per file (`placeBatch`). lord-server checks the path as for Open with (home or mounts, resolved, no last link) and passes `VM_FILE` a = 2 with the window, point and batch. lord-vm refuses it unless the tools said `place` (`VAC_PLACE`, 16384) and the window is in the guest's last list; it copies the file with a token, and the person's copy step records the place and the drop's own name under it. Then `AGK_PLACE` (the token, size, SHA-256, window, point, batch, index, count: 90 bytes, `ag_place_parse`) goes to the tools, which check each file as for 7.3, wait for the whole batch (at most 10 minutes) and hand it only to that window, if it is still in their own list, as the person; each file's `AGK_PLACED` (status, how) answers its `VM_FILE`, and lord-vm logs it. Nothing names a host path or a guest path to the guest, and the tools hand on only files that arrived through that drop. **Linux guests:** not offered (see 7.3).

**Checked** on the card's guest (`lordvm-dev place`): Explorer on Documents got the file under its own name; Notepad got `WM_DROPFILES` and opened it; Explorer on Home (not a folder on disk) put it on the clipboard. A move to x = 2500 and to x = −1500 stayed on the guest's 1400 × 1050 screen.

### 7.3 Open a file in a machine ("Open with", like VMware Fusion)

**Built (2026-11-21, since 0.8.9.5).** In Files, a file's **Open with** lists, after the apps that open it, every machine
whose guest tools open files: "Windows 11 Pro pepe (Windows)". Choosing one opens the machine's window, which starts the
machine if it's off (a bridged network goes through the same `bridgeUp` first) or resumes it if it's paused or suspended,
waits for the guest tools to connect (after the guest's sign-in), copies the file in as a drop on the window does (the
transfer folder when the guest has it mounted, else the agent's way into a private folder; the same progress line), and
asks the tools to open it with the guest's default handler, as the signed-in person: a document opens in its app, and a
program (`.exe`, `.msi`, `.bat`, `.cmd`, `.lnk`…) runs, which is the point (MikroTik's `winbox.exe` straight from
Downloads). Then the machine goes into apps mode (7.2) when the guest has it, so the program comes up as its own LordOS
window in the Dock; otherwise the machine's window comes to the front. A program for another system that no app here
opens is offered to such a machine on a double click, with **Always open .exe files with ‹machine›** (kept per extension,
settings `openWithDefaults`; the Open with menu then has **Ask Each Time for .exe Files**); a double click that would
start an off machine asks first. Other files just list the machine under Open with.

**Which machines.** The ones whose tools said `open` in their hello (`AG_HELLO_OPEN`; lord-vm sets `VAC_OPEN`, 8192, in
`VM_AGENT_STATE`). The machine's window writes that into `app.json` (`opens: 'windows'`) when it sees it, so an off
machine is listed too (`apps/machines/openwith.js`, tested by `server/tests/vm_openwith_test.mjs`). Today that is
Windows guests with `lordos-guest-win` w3 or later (w4 also takes files on its windows, 7.2.1; w5 lets LordOS draw the pointer, 7.1.2; w8 follows the display the picture shows when Windows has more than one and makes it the primary, and says when a window runs as administrator; w9 turns off an extra display when asked, 7.2): install or update the tools from the agent's popover. **Linux
guests are not listed:** `lordos-guest` is approved to start only the clipboard tools (`xclip`, `wl-clipboard`) and
`systemctl --user show-environment`; opening a sent file there would mean `xdg-open` or `gio open` in the person's
session, with its own rules for executables and launchers, and needs its own review first.

**The path, and what is checked where.**
1. Files → the provider's `open(file)` → `openInMachine` (Machines): the path waits in the app (`openAsks`) until the
   window's session runs and the tools say `open`.
2. `lord.vm.sendFile(session, path, { open: true })` (`js_vm.c`): the path is resolved (`realpath`) and must be in the
   person's home folder or their mounts (`fsops_allowed`); it is opened read-only, without following a last link, and
   only a regular file goes on, as a descriptor and a name (`VM_FILE` with a = 1). `lord.vm` exists only in the
   session's JS: an `.lpk` app (`--app`, the broker has no `vm`) or a web page can't reach it.
3. lord-vm refuses it unless the guest is a Windows guest whose tools said `open`; it makes a random 128-bit token for
   the drop and copies the file exactly as any drop (6.7.1, 6.21). On the agent's way it hashes what it writes (SHA-256,
   in order); on the transfer folder's way it has the hash of its copy. The person's own copy step (the one-shot
   scheduled task that already runs **as the signed-in person**, `FX_COPY_PS`) puts the file in their Downloads under a
   free name and, only for these drops, writes where it went (base64) into `%LOCALAPPDATA%\LordOS\open\<token>`.
4. Once that step reports, lord-vm sends the tools `AGK_OPEN`: the token, the size and the SHA-256, exactly 72 bytes
   (`ag_open_parse`). No path and no command go to the guest.
5. `lordos-guest-win` (the person's own process) reads that record once and removes it: not a link or reparse point, at
   most 10 minutes old (`AG_OPEN_RECORD_MS`), a full path of text. In a thread of its own it opens the file (not a
   directory, link or reparse point), checks its size and SHA-256 against the host's, and calls
   `ShellExecuteEx("open")` on it (`"openas"`, Windows' own chooser, when nothing is associated). It answers
   `AGK_OPENED` (token, status `AGOS_*`, the system's error), and lord-vm writes it in the machine's log and answers the
   `VM_FILE` with it (`AG_OPEN_WAIT_MS`, 2 minutes, at most: a program may ask for consent first).
A program opened this way runs with the person's rights in the guest, as a double click in Explorer would; nothing runs
as SYSTEM, and nothing on this computer runs or opens the file.

**Open with, for other providers (LordBridge, Wine).** The menu is generic: `sdk/openwith.js` keeps providers
(`registerOpenProvider(p)` or `registerOpenProviders(source, () => [p])`), each `{ id, label, icon?, accepts(file),
suggests?(file), available?(), needsStart?(), open(file) }` (`sdk/openwithmodel.js`, tested by
`server/tests/openwith_model_test.mjs`). Files lists `openProvidersFor(file)` after the apps, offers
`suggestedProviderFor(file)` when no app opens a file, and keeps "Always open .ext files with …" through
`setDefaultProvider`. Machines registers one provider per eligible machine; a runner for programs of another system
will register itself the same way, with no change to Files.

## 8. Privacy and security model

- **Machines itself** makes no connection, except downloads the person starts: choosing a download card reads that one maker's checksum list, and Create downloads the image (3.9, "The Linux download cards"). The catalog is a file in the signed package, read with no connection. No telemetry.
- **A guest's connectivity is a separate thing:** the default network mode is Shared, because a machine with no network surprises people, and the assistant shows that. Testing is Isolated.
- What a guest shares is chosen by its profile and shown on the assistant's Review step before it is made (5.6): Productivity,
  Development and Design turn the clipboard (both ways) and the transfer folder on, Games the clipboard, Light neither; shared
  folders and the microphone stay off until the person turns them on. (Corrected by the audit of 2026-11-23: this line said
  "off in every profile".)
- Guest → host channels: virtio devices, the virtio-serial port of `lordos-guest`, clipboard, folders, the transfer folder. Each has an on/off in Configure. **Not built:** the **Guest can see** panel (Configure → Security holds six notes on the sandbox, not the channels); USB (no usbredir).
- `lordos-guest` traffic is treated as untrusted: strict parser, size limits, fuzz target. A guest can't start a host process, read host files beyond its folder, or inject input into other windows.
- **An encrypted machine has to be encrypted whole,** or the disk is the only safe part. **Not built (audited 2026-11-23):** no encrypted machines exist yet; an encrypted machine from another app is refused at import. The plan: disks use qcow2 LUKS, with the key in the keyring. Also covered: suspended state and snapshot memory, written inside the encrypted qcow2 and never as a separate file (**To test**); TPM state, through swtpm's own state encryption (**To test**, to confirm in Debian's build); NVRAM; recordings and screenshots stay out of the folder. Thumbnails, and persistent logs, are off, and logs stay in memory. Host swap can still hold guest memory, and only encrypted swap solves that, so Security says so. Bridged mode and encrypted disks each state what they mean in one line.
- `vm-enable`, `vm-kvm`, `vm-net-bridge`, `vm-net-unbridge`, `vm-memlock`, `vm-gvt-create`, `vm-gvt-remove` and `vm-image-get`, `-get-user`, `-list`, `-remove`, `-cancel` are closed actions with fixed arguments, on the list maintainers review (AGENTS.md). There is no `vm-usb-allow` yet.
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
| App memory, no VM running | ≤ 40 MB | No app then; not measured since | ≤ 40 MB | No app then; not measured since |
| Desktop frame time with one machine at full load | No worse than 1 frame over 60 Hz | Not measured | **No worse than 1 frame over 60 Hz at the default 3 vCPUs** (set 2026-11-04) | 59.3 FPS at 3 busy vCPUs (58.0 idle); 2.4 FPS at 4 |

The network column is a guest downloading from a server on the host, so it measures the path through the network back end, not a
real link. The Pi's boot misses its target with UEFI firmware, which takes two thirds of it; the guest's Linux alone takes 6.3 s.

**Desktop under load is the hard one on a Pi.** With all CPU cores busy, V3D went from 1.5 ms to 14–16 ms of GPU per frame with no clock change, and memory contention was measured as the cause on 2026-10-31 (`GPU.md`). Phase 0 measured window dragging with a guest busy on 3 vCPUs: 59.3 FPS, no worse than idle, so **the default of 3 cores stays** and the Pi target is now set (**Measured**, 2026-11-04, Pi 5). With 4 busy vCPUs it fell to 2.4 FPS, but so does the host with 4 busy threads and no VM (3.3 FPS), and `nice 19` on QEMU changed nothing: the stalls of about 1000 ms are a desktop scheduling problem of their own, to fix outside Machines. Two limits on this result: the guest's load was CPU-bound (SHA-256), not memory-heavy, so the memory-bus case of `GPU.md` is still **To test**; and someone used the board during the 4-vCPU runs, which are to be repeated.

**Zero copy is a goal to measure, not a promise.** 3.2 says "no copy", but it depends on GPU, format, modifier and sync. Phase 0 recorded what QEMU sends; nothing was imported yet:

- **Pi 5, V3D:** `ScanoutDMABUF` with `XB24` (XBGR8888), the V3D tiled modifier `0x0700000000000006`, one plane, a descriptor longer than stride × height (padding for the tiles), and `UpdateDMABUF` at up to 194 a second, with the helper at 2 % CPU since it copies nothing (**Measured**, 2026-11-04, direct kernel boot only, see 3.2). Import into the compositor, direct scanout, sync and correctness of the picture: **To test**.
- **amd64:** not measured: the Phase 0 host had no render node. **To test** on a bare-metal PC with Mesa, and with a vendor's proprietary driver.

The LordOS rule holds: this app must not slow the desktop. It's idle until opened and runs nothing in the background without a running VM.

**Performance work (approved 2026-11-23; all six built on `test/integration-1125`, in development; status in the checklist's area M):**
1. Skip frames that didn't change, and pace or pause the display of a machine whose window is hidden. **Built** (in
   development): 3.9 "Pacing the picture", docs/MACHINES-GPU.md "Pacing a machine's picture".
2. Huge pages for guest memory. **Built** (in development): hugetlb pages when reserved; private memory already has THP;
   shared memory (shared folders, the transfer folder) gets THP with `shmem_enabled=advise`, set by the `lordos` package,
   and `memory-backend-ram,share=on`: 3.7.2.
3. A vCPU recommendation. **Built** (in development): from what the machine's vCPUs really used (`vmcpu.c`,
   `cpu-usage.json`), in Configure → CPU & memory and on the library's card; the assistant still uses the host's default (6.27).
4. The disk path. **Built** (in development): an iothread, `cache.direct` with `aio=native` on ext4 and XFS (6.4.1);
   kept after measuring real work against the host's cache (6.4.1, "Host cache or not").
   `io_uring` stays off: our seccomp allowlist refuses `io_uring_*` (`vmsyscalls.h`) and loosening it needs the maintainer's
   review (not measured against it).
5. A guest-side performance profile applied by the guest tools. **Built** (in development): Windows guests (tools w11,
   and Windows Search paused by lord-vm's fixed script through qemu-guest-agent) and Linux guests (`lordos-guest` 7 over
   D-Bus), 7.1.4.
6. vCPU pinning to host cores. **Built** (in development): Configure → CPU & memory → Pin cores (3.7.1).

## 10. Packaging

- **Built (audited 2026-11-23):** Machines is part of the `lordos` package (app, `lord-vm`, `lord-vmdisplay`, `machines`,
  `lord-pgpv`, `lordos-guest`, `lordos-guest-win.exe`, the GVT-g GOP ROM; `tools/iso/mkdeb.sh`), and it can be deactivated in
  Store → Installed. QEMU and the rest are not package dependencies: the first time, the app's **What's Missing** runs the
  closed admin action `vm-enable`, which installs `qemu-system-<native arch>`, `qemu-system-modules-opengl`, `qemu-utils`,
  UEFI firmware (`ovmf` or `qemu-efi-aarch64`), `passt`, `virtiofsd` and `swtpm` (`vm_admin.c`); the other architecture's
  QEMU is the same action, asked for emulation. (The original plan, a separate `lordos-machines` Store package with
  `Depends:`, was not followed.)
- Sizes: `qemu-system-x86` is 68 MB (**Measured**). The whole set, with dependencies, is **To measure**: plan on 300 to 500 MB, not including any image. Each guest wants 20 to 64 GB.
- **LordOS 1.0 doesn't depend on Machines.** Its first public form is an **experimental, reduced release**, Phase 2's scope, and it ships only after passing isolation, disk integrity and failure recovery tests (section 12).
- Debian's QEMU is used, not a fork, so security updates come from Debian. If a feature needs a newer one, backport with care: open decision 5.
- Licenses: QEMU GPL-2.0, swtpm BSD-3-Clause, virtiofsd Apache-2.0/BSD-3-Clause, EDK2 BSD-2-Clause-Patent, passt BSD-3-Clause; all stated on the Store page.

## 11. i18n, accessibility, docs

All text in en, es and pt from the first commit, a `machines.json` per language. Tiles and sheets keyboard reachable. Captured VM is announced with its release keys (a visual notice only, 6.25). Guest content is opaque to screen readers; the app
doesn't say so yet. Built: `machines.json` in every UI language and `machinescli.json` in all 35. Business rule: CHANGELOG, paper, FAQ, features and site follow every shipped phase. Claims only after they were tested. RTL handled by the SDK. Help article `machines.md`: not written yet.

## 12. Tests

**The app and its system ties** (*in development*, "Machines to 100 %"): `vm-test` (the resource budget's sum, what Backups
hold back), `backup-test` (a held path), `clean-test` (a machine's files never a candidate), `store_syspkg_test.mjs` (the
QEMU update warning) and `vm_model_test.mjs` (the budget's offer, the failure page's causes and fixes, the lights, the
isolation gaps, Picture in Picture's frame, Actual Size, the Info menu, What this computer can run, Search, start with the
session, the battery notice, away, Configure's dots and search). `tests/machines_app_headless.sh OUT` (a test tool, not in
`make test`) runs the real theme in a headless lord-server on a made-up library and checks and photographs the library's lights,
the weaker isolation notice, the budget's status line, What this computer can run, Windows 11's start refused beside two
running machines with Suspend and Start with less memory, Configure's Startup and shutdown with its search and the battery
notice: every step "done" on 2026-11-24.

What runs (audited 2026-11-24, at `bb5baab62`): `make -C server test` builds and runs `vm-test` (schema and
migration, messages, fuzz passes over the guest → host parsers), `vmlog`, `vmgpu`, `vmimport`, `vmcatalog`, `http-resume`,
`http-retry`, `vmunattend`, `vmthumb`, `vmlogind`, `vmdisplay-test`, `vmpace`, `vmmem`, `vmcpu`, `vmpin`, `vmurl`, `vmsig`, `mcli-test`,
and since the last audit `vmseccomp` (the allowlist on both architectures), `vm-jail-probe` (QEMU's jail as a compromised
QEMU finds it), `vmgroup`, `vmlink`, `vmimage-group`, `vmunpack`, `np-torgw` (the Tor gateway's firewall identity),
`vmwatch` and `guest-dbus`; the scripts `vm_import_test.sh`, `vm_rename.sh`, `guest_clip_test.sh`,
`guest_stop_test.sh`, `guestwin_headless.sh`, `machinescli_headless.sh`, `vm_whonix_xml_test.sh`, `vm_group_link.sh` (with
Whonix's disks given), `guest_perf_test.sh` and `migration_shmem_huge_test.sh` (each skips without what it needs), and the
node tests `vm_model`, `vm_catalog`, `vm_windows`, `vm_capture`, `vm_openwith`, `vm_cli`, `vm_profiles`, `vm_keys_scene`,
`vm_group_model`, `vm_group_ui`, `sentinela_vm` and `protect_model`. Manual: the four
gates below, `vm_import_identity.sh`, `build/vm-image-cli`, `vm-net-cli`, `vm-gvt-cli` and `lordvm-dev` (`tests/vm_dev.c`). Not
built from the list below: the settings → QEMU arguments table test (the arguments are built in `lordvm.c`, which `vm-test`
doesn't link), a fuzz of lord-vmdisplay's D-Bus side, a boot matrix in CI, the virtio-win install test, agent kill-mid-transfer and
channel-refusal tests, the storage-figure test.

- **Unit:** settings → QEMU arguments in a table test, for each architecture; `machine.json` schema and migration.
- **Sandbox:** a test that a hostile `lord-vm` child can't open `~/.ssh`, another VM's folder, or `/dev/kvm` again. Built
  as `build/vm-jail-probe` (2026-11-23): a probe that goes through QEMU's own jail and tries, from inside, what code
  running in QEMU would.
- **Fuzz:** `lordos-guest` parser and the D-Bus listener in `lord-vmdisplay`.
- **Boot matrix** in CI where KVM exists: Debian, Ubuntu, Fedora, LordOS on amd64; same on arm64 (Pi).
- **Real hardware matrix before each claim:** Pi 5 16 KiB kernel; Pi 5 4 KiB kernel; Pi 4; Intel; AMD; a nested amd64 VM (TCG path and refused KVM).
- **Install test** of each pinned virtio-win version before the catalog moves.
- **E2E** with the bench in `tools/vm/e2e/`: the assistant to first boot, snapshot and revert, folder, USB, clipboard.
- **Agent:** fuzz the protocol from the guest side; kill mid-transfer and check that nothing partial appears; a guest asking for a channel is refused.
- **Storage figures:** clones on btrfs, XFS and ext4 compared to the real change in free space after deleting.
- **Release gate, isolation:** a QEMU made to misbehave on purpose can't reach the home folder, other machines, or the compositor; malformed scanouts sent to `lord-vmdisplay` are refused.
- **Release gate, disk integrity:** kill `lord-vm` and QEMU (a power cut isn't simulated) during write, snapshot, revert, clone and compaction, then `qemu-img check` every disk.
- **Release gate, failure recovery:** each interrupted operation of 3.6 ends finished or rolled back, with no terminal.
- **Leak test:** start and stop 100 times: no orphan processes, scopes, sockets, tap devices.

**Bare-metal gates.** The three gates (disk integrity, failure recovery, leaks) have run on the nested amd64 build VM
and on the Pi 5 (3.9); on a physical amd64 PC they run with `tools/vm/bare-metal-gates.sh`, which checks the PC first
(amd64, not a virtual machine, VT-x or AMD-V, `/dev/kvm` usable by the account, the engine, about 15 GB free on a real
disk, 2 GB of memory), downloads Debian 13's nocloud image (SHA-512 checked) unless given one, runs the gates one after
the other and writes one file with the PC's details (CPU, board, memory, disk, kernel, QEMU, passt, LordOS, `lord-vm`'s
SHA-256) and every gate's log. Exit 0 when the three pass. On the PC, from the LordOS live USB or an installed LordOS,
as the normal user (not root), with the network:

```
# 1. Machines → What's Missing → Install, then Turn On (the kvm group: log out and in once if it says so)
# 2. the source tree on the PC (a clone, or the branch copied on a USB stick)
git clone https://github.com/lordbasex/LordOS.git && cd LordOS && git checkout machines-089
# 3. on the live USB, /var/tmp is in memory: give a folder on an internal disk or a USB SSD with 15 GB free
sh tools/vm/bare-metal-gates.sh --work /media/$USER/<disk>        # installed LordOS: sh tools/vm/bare-metal-gates.sh
# 4. attach the printed results file (machines-gates-<host>-<date>.txt) to the release notes or the issue
```

`--lord-vm DIR` tests a `lord-vm` other than the installed one (`server/build` after `make`), `--image FILE` uses a
Debian 13 nocloud amd64 qcow2 already there, `--cycles N` shortens the leak gate. The run takes about 15 minutes.
`GATES_ALLOW_VM=1` lets the script run on a virtual machine, for trying the script itself; its file then says *NOT bare
metal*.

*The script tried on the amd64 build VM* (2026-11-06, `GATES_ALLOW_VM=1`, nested, Xeon Silver 4108, 8 threads, 3.8 GB,
`machines-089`'s `lord-vm`, QEMU 10.0.13): integrity **46 passed** (435 s), recovery **41 passed** (271 s; passt killed:
579 ms to the guest's next fetch, then 1,079 and 2,591 ms), leaks **10 passed** (147 s; 100 cycles in 146 s, 1,464 ms
each, the slowest 5,242 ms); exit 0, nothing left in `/var/tmp`. These are nested numbers, not bare metal.

*Bare metal (Xeon Silver 4108, Proxmox host, pve-qemu 11.1)* — **Measured** 2026-11-06, run with the maintainer's
authorization on the physical Proxmox host (Supermicro X11DDW-NT, 16 threads, 31 GB, kernel 7.0.14-23-pve), host left
unchanged: nothing installed, no user or group created, no VM touched (`qm list` identical before and after). The kit:
`lord-vm-testpaths` from `machines-089` pointed at pve-qemu 11.1.3, `/usr/share/pve-edk2-firmware/OVMF_CODE_4M.fd` and
`OVMF_VARS_4M.fd`, and passt extracted from Debian 13's package; the gates ran as `nobody` with the `kvm` group through
`setpriv` (they refuse root), `nice 19` and idle I/O, one at a time, their files in `/var/tmp/lordos-gates`, removed at
the end with the kit (0 processes left). Results: integrity **47 passed** (216 s), recovery **41 passed** (229 s; passt
killed: 390 ms to the guest's next fetch, then 1,102 and 2,583 ms), leaks **10 passed** (152 s; 100 cycles in 151 s,
1,512 ms each, the slowest 5,286 ms); exit 0. pve-qemu 11.1 has `ScanoutDMABUF2` (Debian's 10.0.13 doesn't), but these
gates run without a display, so the numbers are for the engine, the disks and the network, not for the picture. Logs
kept outside the repository (`out/final/gates-proxmox/`). A physical PC with LordOS installed on its own disk (the
maintainer's i9) is still to run the script as above, for Debian's QEMU and a real display.


## 13. Phase 0 results

Phase 0 ran on 2026-11-04, with no app: QEMU driven over QMP only, a root login on a serial console reached through a Unix socket, and
a small display prototype in C. Everything here is **Measured** on that date, on the machine named. Timings are medians of at least 3
runs unless it says otherwise. The scripts are in `tools/vm0/` (`vmdisplay-proto.c`, `vmdisplay-drive.py`).

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
3. **dmabuf import in the compositor** (done on amd64, read back by the GPU path, 3.2; zero copy still open): the tiled V3D modifier, direct scanout or composite, fences with a timeout, and checking that the
   picture is right.
4. **`lord-vmdisplay` for real:** done in 3.9 (`SIGBUS`-guarded copy, copy before forward with an acknowledged frame, a rate lord-server
   sets). Still open: latency all the way to the screen, which needs the window (Phase 1, part 2).
5. **The Pi desktop:** repeat the 4-vCPU drag on an untouched board, measure a memory-heavy guest at 3 vCPUs, and find the cause of the
   1000 ms stalls when all four cores are busy, which happens with no VM at all.
6. **GICV trapping:** the cost of interrupt-heavy work on the 16K kernel, and whether a 4K kernel removes it (open decision 4).
7. **Not touched in Phase 0:** our own seccomp filter on top of QEMU's, Landlock on QEMU, nested cgroup leaves, mouse, pause and
   resume, and UEFI vars as qcow2 are done in 3.9 (the filter was a denylist; since 2026-11-23 it is an allowlist, 3.3; nested leaves only where lord-vm's cgroup is delegated, not
   yet under lord-sessiond). Still open: the render node passed by descriptor, one slice for all machines (3.7), disk speed of qcow2.
   Done since (audited 2026-11-23): D-Bus audio (`vmdisplay.c`), the clipboard (over the agent's channel), the cursor of a
   graphical guest (`VD_CURSOR`), suspend (6.1).
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
| **2, Beta** | Control Center, assistant with Linux cards, requirements check, one-screen settings, profiles, snapshots, suspend, clones, Storage window, agent with state popover, Shell, Logs, unified diagnostics, screenshot and recording, resolution menu, Keep Running, clipboard. The experimental release | Full install and failure recovery are repeatable; release gates pass. Built on `machines-beta` (3.9, Phase 2, the beta) and merged with the import on `machines-089`: the gates pass on nested amd64 and, with `machines-089`, on the Pi 5 (45, 41 and 10 passed, 2026-11-06); the Linux download cards are built (3.9, "The Linux download cards"), with the makers' signatures checked, resumed downloads and removal in Storage, and driven on screen in a LordOS session through a whole Debian install (2026-11-06); the gates pass on bare-metal amd64 too. Unattended install built (3.9, "Install without questions") and seen into Debian's base system install on VM 116; Debian and Ubuntu to login done 2026-11-07; still open: Fedora to login |
| **3, Advanced** | UEFI secure boot, TPM, virtio-win flow on both architectures with automatic setup, virgl, venus, shared folders, USB, file copy panel, Bridged, Host only, Picture in Picture, multi-head, export and import (import of other apps' machines built, 6.22), backups, Monitor, `lordvm` with shell, exec, logs and cp | A real compatibility and security matrix per architecture. Audited 2026-11-23: built are secure boot and TPM, the virtio-win flow on amd64, virgl, shared folders, Bridged, Host only, import, `machines` (without shell, exec, logs, cp) |
| **4, Integration** | Linux Apps mode, guest apps in the Dock, reproducible machines, local MCP server, Migrate | Each on tested hardware |
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
| Host page and swap pressure | Host starves | Memory limits (cgroup, built); balloon and the warning at 80 % not built |
| Sandbox on Pi 4 kernel | Weaker isolation | Stated in Security |
| WinFsp and agents add third-party code in guests | Maintenance | Pinned, verified, opt-in |

## 16. Feature matrix

The bar: **the most complete desktop virtualization app there is, and the best looking**, on free parts. Ambition is kept honest here, one line per capability.

**Linux today** says whether the kernel and existing open source parts allow it now: **Yes**; **Work** (possible, we must build a real piece); **Partly** (some guests or hosts only); **No**. **M** is the phase of section 14 that ships it. This matrix is the plan and says what Linux allows, not what is built: "Yes" never means "done". What is built is in the status checklist at the top (audited 2026-11-23).

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
| Import qcow2, VMDK, VDI, VHDX, OVA, other tools' bundles | 3 | Yes | Built (6.22): identity kept (SMBIOS UUID, MACs, serials, firmware, topology); snapshots flattened, saved sessions refused, vTPM lost |
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
| Apps mode, virtio-win guests | 5 | Work | Built early (7.2, `lordos-guest-win`) |
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
| MCP server for coding agents | 4 | Partly | Runs programs, reads logs, starts, stops and clones machines with automation on (in development, 6.15); building a machine from a card isn't built |

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
| **Machines in the firewall** | 2 | Yes | Built partly: every machine is the one firewall component `lordos:machines`; Monitor tells machines apart |
| **No engine window, no X11** | 1 | Yes | One compositor, theme, desktops and shortcuts for all |
| **Forwarded windows are marked** | 4 | Work | A guest's window can't pass as a host's |
| **Guest logs and files are treated as hostile** | 2 | Yes | Escapes stripped, quarantine marking, fuzzed agent protocol |
| **Same features, amd64 and arm64** | 1 | Partly | The app states every difference |
| **The compositor is protected from the engine** | 1 | Yes | A separate validator; the desktop survives a broken machine |
| **Files out of the engine's reach** | 3 | Yes | Shared folders are held by their own sandbox, not by QEMU |
| **A budget across all machines** | 1 | Yes | The desktop keeps its share, whatever is running |
| **Whole-machine encryption** | 3 | Partly | Memory, TPM, NVRAM and logs too, not only the disk |
| **Unified diagnostics** | 2 | Yes | One place: machine log, guest log, agent, Monitor and firewall agree on what a machine did |
| **Reproducible machines** | 4 | Partly | `lordvm apply`: a text file beside a project (in development, 6.24); no packages or rebuild yet |
| **Desktop integration** | 4 | Work | One compositor, permissions, Dock, Files and search, not an app beside the system |
| **Claims only after testing** | always | Yes | The app and this document say what wasn't tested |

## 17. What is impossible or weak

- KVM across architectures: impossible. Emulation only.
- Full modern desktop system for amd64 on a Pi: too slow to be useful.
- 3D in virtio-win guests: no good free driver.
- 3D or GPU video decoding in emulated guests: no.
- 3D in arm64 guests with Debian 13's QEMU: QEMU aborts when the guest resets its GPU, which every UEFI boot does (6.10).
- 3D in amd64 Linux guests with kernel 6.12.117 or later: QEMU 10.0.13 aborts at each guest shutdown or restart (QEMU issue 4918); lord-vm names the end (`qemu-3d-stop`, `qemu-3d-crash`) and unattended installs start in 2D (6.10).
- GPU or PCI passthrough on a Pi: impossible on this kernel and board.
- Nested virtualization on a Pi: not with these CPUs.
- Bridging over Wi-Fi: not with ordinary drivers.
- Moving a running VM between architectures: impossible. A VM stays on its architecture; a suspended one only resumes on a compatible CPU. Moving between Pi and PC needs emulation.
- A guest locked to its maker's hardware: not offered.
- Apps mode for virtio-win guests: built with our own agent, `lordos-guest-win` (7.2).
- Secure boot on arm64 guests: possible with AAVMF only if Debian's arm64 firmware build includes it, **To test**. If it doesn't, Setup has to be told to skip that check, and the app says so.
- Dragging files out of a Linux guest's own desktop: no agent can see the drag; see 6.21.
- The host always pays: suspended guests still hold disk equal to their RAM.

## 18. Open questions

Decided on 2026-11-01: settings on one screen replaces tabs; Kali is in the catalog, which will grow later; unattended install uses index selection first and the public generic key only as its fallback (the code later chose the edition by image name with the person's key or the KMS client key, 4.3); no VNC: neither a machine's picture nor its clipboard goes through it.

1. ~~App name and icon.~~ Settled by the code: **Machines**.
2. ~~Default folder: `~/Machines`?~~ Settled by the code: `~/Machines`.
3. ~~Store item (recommended) or preinstalled?~~ Settled by the code: part of the `lordos` package, can be deactivated, QEMU installed on demand (10).
4. Do we build our own 4K kernel for the Pi if a measurement shows that it removes GICV trapping? Phase 0 didn't measure it; it moves to Phase 1 (13.8).
5. Keep Debian's QEMU (recommended, and what the code uses) or carry our own build for features like newer venus fixes? Phase 0 adds a reason: the arm64 3D abort of 3.2.
6. ~~An Android card in Machines?~~ Settled on 2026-11-24: no (4.2).
7. ~~Is `passt` or QEMU's user networking the default?~~ Decided on 2026-11-04 by measurement: `passt` (6.5).
8. ~~Does the first public release include virtio-win guests, or Linux only?~~ Settled: Windows 11 machines on amd64 since 0.8.9.5 (4.3.1).
9. ~~Does the catalog list only systems we can download freely, as recommended?~~ Settled: yes (Windows from the maker's page or its evaluation ISOs, checked against pinned hashes).
10. Is Migrate worth its cost?
11. ~~Does `lordvm mcp` ship at all?~~ Built on 2026-11-24 (in development) as the brief asked: `lordvm` is a second name of `machines`, and the MCP server acts only on a machine whose person turned automation on, never on the anonymity profile (6.15).
12. ~~Machine groups and the Whonix card: the decisions listed in 19.13.~~ Settled by the maintainer on 2026-11-23 (19.13); G0–G2 are built, *in development* (19.0).

## 19. Machine groups and Whonix (in development)

Asked by the maintainer on 2026-11-23: Whonix in the New Machine assistant, made as a **machine group** (Whonix-Gateway and
Whonix-Workstation started and stopped together, shown as one card), for anonymous browsing (onion sites included) with no
IP leak, thrown away and started again at will. The design below was checked against `test/integration-1125` at
`67eb66465` and against Whonix's download server and KVM files on 2026-11-23, and **approved by the maintainer the same
day** with the decisions of 19.13 (19.12 lists what was approved, for AGENTS.md). Phases G0–G4 of 19.11 are built on
`feat/whonix-group` and `feat/whonix-group-2` (*in development*, not in a release); 19.0 says what exists and what the checks of G0 found.

### 19.0 What is built (G0–G2, 2026-11-23)

**Built and tested** (headless, on the dev host: Debian 13, QEMU 10.0.13, kernel 6.12, KVM):

- **The catalog entry** `whonix` (`assets/machines/systems.json`; shown since G3, only with KVM on amd64) with
  Whonix's key pinned in `packaging/keys/machines/whonix-derivative.asc`. Its primary fingerprint
  `916B 8D99 C38E AF5E 8ADC 7A2A 8D66 066A 2EEA CCDA` was compared on 2026-11-23 with Whonix's signing key page
  (`https://www.whonix.org/wiki/Main/Project_Signing_Key`, the page `Whonix_Signing_Key` points to) and with
  keys.openpgp.org: the same. `vmcatalog.c` learned `"hash": "sha512"`, a detached signature named by `"suffix"`, the
  `"releases"` folder lookup with a floor, `"kind": "group"` with `"archive"` (the members' names, `maxApparentGb`,
  `maxDataGb`) and `"machine"` (each member's vCPUs and memory).
- **The admin actions** `vm-image-list` and `vm-image-get` for such an entry: `vm-image-list "whonix amd64"` fetches the
  listing of `https://download.whonix.org/libvirt/` (only names are taken from it); `"whonix amd64 <release>"` that
  release's folder; `"whonix amd64 <release> <list>"` the `.sha512sums` list and its `.asc`, verified with the pinned key
  by `pkg/pgp.c`, kept as `lists/whonix-amd64.list` with the release beside it (`.release`). A release below the floor
  (`18.2.4.3`), below the newest release ever verified (`lists/whonix-amd64.newest`, root's: the rollback floor no file of
  the person's can lower) or not a release's name is refused, and the list's own name must give that very release
  (`vmcat_release_in`: no older signed list beside a newer folder). `vm-image-get "whonix amd64 <file> <sha512>"` takes
  only an archive whose name gives the recorded release, downloads it from that release's folder under a temporary name
  (`<sha512>.tar.xz.dl`, 0600) and renames it to `images/<sha512>.tar.xz` (then 0644) only when the whole file's SHA-512
  is the list's; the image lock is taken before anything, and a file at the final name that isn't root's, single-linked
  and 0644 is thrown away, never trusted. Root hashes the bytes; it never parses the archive (`vmimage-group-test`, as
  root of a user namespace with the network replaced). `vm-image-cancel` and `vm-image-remove` take the 128-digit names too. *Not run
  as root here* (it needs root and the network: `tests/vm_image_cli.c`); the verification itself is `vmcatalog-test`,
  with the real 18.2.4.3 list and signature as fixtures.
- **`lord-vmunpack`** (`server/src/vm/vmunpack.c`, `mk/vmunpack.mk`) and a streaming decoder in `pkg/xz.c` (`xzs_*`, next to
  the in-memory `xz_decode`). The protocol: argv `lord-vmunpack` alone, an empty environment; fd 0 the archive, fds 3 and 4
  the gateway's and the workstation's new empty files (opened without `O_APPEND`), fd 5 one request line
  `<maxApparentBytes> <maxDataBytes> <gatewayRegex> <workstationRegex>`, fd 1 `progress <in> <total>` lines (at most four a
  second) and one answer, `ok <gwSize> <wsSize> <dataBytes>` or `error <word>` (`badXz truncated trailing badTar
  badChecksum badType badName duplicate missing badSparse tooLarge tooMuchData io request`). `sandbox_apply_helper` (scope
  `lordos-vmunpack`, 320 MB, no extra syscalls: `pread64`, `pwrite64`, `ftruncate` and `fstat` are in the base list) before
  the first byte. xz checks: none, CRC32 and CRC64, each verified; SHA-256 checks and dictionaries over 256 MiB are refused;
  the index must list exactly the blocks seen. The tar rules are 19.6's, plus: at most 64 members and 2²⁰ sparse
  entries, zero runs never written (holes stay holes), at most 1 MiB of zeros after the end blocks.
- **`lord-vm --group-create GROUPDIR --catalog-id whonix --archive FILE --sha512 HEX [--name N]`**: hashes the archive again
  (the file handed to the helper is the verified one), starts `lord-vmunpack` (the one next to `lord-vm`), checks each
  member with the jailed `qemu-img info -f qcow2` (no backing file, no data file, a virtual size within the cap), writes the
  bases with the jailed `qemu-img convert -f qcow2 -O qcow2` and `check`, renames them into `<library>/.shared/`
  (`whonix-<release>-<role>-<sha12>.qcow2`, 0444; a base already there is reused), makes `Gateway.lordvm` and
  `Workstation.lordvm` as linked clones with the schema 14 keys, and writes `group.json` last. Progress lines
  `{"progress":{"step":"verify|unpack|base",…}}`, then `{"created":true,…}`.
- **`group.json`** (`vmgroup.{c,h}`, 19.4) and **`machine.json` schema 14** (`group`, `role`, `"network": "link"`,
  `"uplink": "tor"`, `link_mac`, `profile_lock`, `timers`). The anonymity lock is in `vm_machine_check`, so `lord-vm --set`
  (and Configure, which goes through it) can't turn on the agent, clipboard, transfer folder, shares, ports, 3D, a vGPU,
  the microphone, SMBIOS strings, disk serials or the performance profile on a locked member.
- **The members' QEMU** (`lordvm.c`): the link card on its own hub (`lnk0`; hub 0 and slot 0x2 on the workstation, hub 1
  and slot 0x18 on the gateway), passt without DNS forwarding for `uplink: tor` (`--no-map-gw --no-dhcp-dns
  --no-dhcp-search`), and for a locked member: a private `memory-backend-memfd` (never hugetlb), no transparent huge
  pages either (`PR_SET_THP_DISABLE` on QEMU before exec: LordOS sets `shmem_enabled` to `advise` since 0.8.9.6 and QEMU
  advises them on guest memory), `mem-merge=off`,
  `vmport=off`, `hpet=off`, `vmgenid`, no sound card on the gateway and `ich9-intel-hda` + `hda-output` on the workstation,
  no first-boot credentials, no suspend (VM_SUSPEND refused; at host shutdown it is shut down, never saved). The timers are
  libvirt's (below). No `thumb.png` is written for a locked member (`js_vm.c`).
- **`VM_LINK`** (`vmproto.h`, `lordvm.c`) and lord-server's side (`vmlink.c`, `lord.vm.link(groupDir, on)`): which two
  machines are paired comes from the group's `group.json` and their `machine.json`, never from JS. Every link is put on
  with QMP (`getfd`, `netdev_add socket`, `netdev_add hubport`) and the previous one taken off (`netdev_del`): **`--link-fd`
  on the command line isn't built**, because the card is there from the start and a hot-added backend proved enough.
- **Groups in the app's model**: `lord.vm.list()` lists the members (`groupDir`, `groupId`, `role`, `primary`, `groupOk`,
  and every running machine's `vmPid`), `lord.vm.groups()` the groups; `machine_dir` accepts a member's folder;
  `model.js` has `groupMembers`, `groupState`, `groupPlan` (start and stop order), `groupLinkKey`, `groupCliPlan`; `cli.js`
  starts a group in order and links every group whose two members run under lord-vms it hasn't linked yet (every 2 s),
  so a member started anew from anywhere is linked again.
- **The `machines` command**: `list` shows a group's row (`group of 2`, its state, its members' vCPUs and memory together;
  `--json` adds `"groups"`), `status NAME [--json]`, `start NAME [--no-window]` and `open NAME` through the session (exit 5
  without one), `stop` and `poweroff` in stop order. The session channel takes a `.lordgroup` folder (`vmcli.c`).

**Built in G3–G4** (`feat/whonix-group-2`, 2026-11-24; *in development*, headless on the dev host):

- **The assistant** (`apps/machines/whonix.jsx`, `assistant.jsx`, `catalog.js`): the card "Whonix (Tor)" with its "Group · 2
  machines" badge, only with KVM on amd64 (`systemsFor(…, { kvm, hostArch })`); choosing it resolves the newest release
  folder whose signed SHA-512 list verifies, never below the catalog's floor (`releaseNames`, `newestRelease`, then the
  three `vm-image-list` calls of 19.6; `pickImage` takes only 128-digit lines for a SHA-512 entry). Then **About Whonix**
  (the diagram Internet ↔ Gateway ↔ private link ↔ Workstation, what is locked off, the random group identity, what it
  doesn't protect against, Whonix's KVM page and license, the **Disposable session** switch), the name, the review (both
  members, the memory while running with the session's cap, the place) and **Download and Create**: `vm-image-get`, then
  `lord.vm.groupCreate({ catalogId, archive, sha512, name })` (only `<image store>/<sha512>.tar.xz`, the name checked;
  `lord-vm --group-create` with its verify/unpack/base progress), the switch through `lord.vm.groupSet`, then the group
  starts (the gateway first, the workstation in its window).
- **The library** (`library.jsx`, `group.js`, `groupui.jsx`): ONE card per group (`libraryItems`: the members are no cards
  of their own), with the group's mark (no picture of a protected member exists), its state, a "Tor" chip with the
  gateway's dot and "Group of 2 machines · CPU · memory"; Play starts the group, Open shows the workstation. The chips and
  the search count it once. Its menu: Start the Group, Show Workstation, Show Gateway, Shut Down the Group (Workstation,
  then Gateway), Force Off Both, Group Snapshot… (both members' disks, the group off), Configure…, Disposable Session (a
  check item), Logs, Show in Files, **Destroy and Start Fresh…** (a confirm dialog: the Workstation only by default,
  "Also reset the Gateway" with the warning about Tor's entry guards, the number of snapshots that go; the box is
  remembered in `group.json` `fresh_gateway`). The start checks the whole group's memory budget first (`groupBudget`:
  both members with QEMU's overhead and, with a disposable session, its cap; against `memMaxMb` less what runs).
- **Configure's group view** (`configure.jsx`): a Group section first (the member switcher, the disposable session with
  its cap, the start and stop order, the release) and the anonymity rows shown "Off" with a lock and a one-line reason
  each (clipboard, transfer folder, shared folders, guest tools, 3D, microphone, ports, suspend, pictures, identity, DNS);
  Sharing, Guest agent and Network of a member show only those locked rows.
- **Protected windows**: `lord.vm.list()` gives each member its `groupKind` and `profileLock`, so `machineProtected` holds
  for both members' windows and their apps-mode windows from the first frame.
- **Start Fresh** (`lord-vm --group-fresh GROUPDIR [--gateway]`, `lord.vm.groupFresh`, `machines fresh NAME [--gateway]
  [--yes]`): every member off and locked for the whole job; each base's SHA-256 checked against `group.json` (a base that
  changed is refused); a new empty overlay over the base replaces `disks/system.qcow2` by rename (its snapshots go with
  it); `snapshots.json`, `cpu-usage.json`, the logs and any picture removed; a new random id (QEMU's `-uuid`) and new MAC
  addresses. The other member is untouched. `machines fresh` asks unless `--yes` and refuses without a terminal.
- **The disposable session in memory** (isolation layer 6, approved 2026-11-23; 19.7): with `group.json` `disposable`, the
  workstation's start makes an empty qcow2 overlay (headers only, by the jailed `qemu-img`), copies it into a memfd and
  removes the file; QEMU gets the memfd as `/dev/fdset/2` and opens `system.qcow2` read-only under it. Nothing of the
  session is written to a file, and its memory goes with QEMU. **The cap**: as much as the workstation's memory, at least
  1 GB (2 GB by default), counted in the machine's cgroup limit (QEMU writes the memfd's pages) and in the group's budget.
  **When it fills**, `lord-vm` pauses the guest (state paused, reason `session-full`; the window says "Paused: the
  disposable session's memory is full"); Shut Down or Force Off ends the session, a Resume gets 5 % more before the next
  pause. Snapshots are refused during a session. Like the guest's own memory, its pages can reach swap under memory
  pressure (19.1, "Forensics").
- **The Gateway's firewall identity** (isolation layer 4): 19.5.1.
- **Shut Down** (19.0's open finding, solved): below.
- **`machines`**: `fresh` and `--member gateway|workstation` (`start`, `stop`, `poweroff`, `status`, `open`; `--member
  workstation` starts the gateway first).
- **Storage** lists a group's verified archive (`<sha512>.tar.xz`) with the groups made from it and whether their bases
  are there (then it may go: Start Fresh needs only the bases); a group's bases count as used and are never offered for
  removal.
- **Tests**: `vm_group_ui_test.mjs` (the card, menu, Start Fresh's plan, the budget, the chips, the catalog's group card,
  release folders, SHA-512 lists, the archive in Storage); `vm_whonix_xml_test.sh` (each member's QEMU and passt
  arguments against Whonix's own XML of the pinned release, started paused from empty disks; anything new in the XML
  fails); `vm_group_link.sh` now also checks the shutdown (both members power off by themselves, Tor stops cleanly),
  the disposable session (600 MB written in the guest are in no file of this computer, `system.qcow2` byte for byte
  unchanged, the pause at the cap, the files gone at the next start) and Start Fresh (new id and MAC, the gateway kept,
  the file gone, `--gateway`); `machines_group_headless.sh` takes the screenshots.

**Not built yet**: the release gate on real hardware (19.10), disk encryption of the members' overlays (isolation layer 5,
a separate run), a "No network: the Gateway is off" bar with Start Gateway in the workstation's window, Rename and Delete
of a group from its menu, and making a missing base again from the archive.

**G0 findings (2026-11-23):**

| Check | Result |
|---|---|
| Two sandboxed QEMU 10.0.13 over one socketpair, and its swap | Works. Both members under `lord-vm` (Landlock, seccomp, KVM by descriptor), the pair handed with `VM_LINK`, frames QEMU to QEMU. QEMU's `stream` netdev can't take a descriptor once it runs (6.5), so every link is a `socket` netdev, as passt's healing does; the two framings are the same. Swapped three times in 30 ms during a 12.7 MB download through Tor in the workstation: the download finished (`code=200`); the guests never saw the cable move. On the host, `ss -xp` shows the link as one unnamed pair whose only holders are the two QEMUs; older pairs are gone |
| The card order with the planned slots | Gateway: `eth0` = 0000:00:02.0 (passt, 10.0.2.15/24, Whonix's static address; default route via 10.0.2.2), `eth1` = 0000:00:18.0 (10.152.152.10/18). Workstation: one card, `eth0` = 0000:00:02.0, 10.152.152.11/18, default route via 10.152.152.10 and nothing else, DNS 10.152.152.10. Whonix's own configuration, unchanged, matches these slots |
| What libvirt passes for Whonix's timers | Read in libvirt v11.3.0's `qemu_command.c` and `qemu_validate.c` with its QEMU 10.0 capabilities (no `virsh` on the dev host). Workstation: `-rtc base=utc` (`rtc present='no'` is ignored by libvirt: q35's RTC can't be removed), `-global kvm-pit.lost_tick_policy=delay` (`pit present='no'` is ignored too: libvirt never emits `pit=off`), `-machine …,hpet=off`, `-cpu host,kvmclock=off`. Gateway (its XML has `rtc tickpolicy='catchup' track='guest'`, `kvmclock present='yes'`, `pit` and `hpet` `present='no'`): `-rtc base=utc,clock=vm,driftfix=slew`, the same PIT global, `hpet=off`, `kvmclock=on`. Both: `pmu=off`, `kvm-pv-unhalt=on` (pvspinlock), `vmport=off`, `mem-merge=off` (nosharepages), `dump-guest-core=off`, `vmgenid`, `ICH9-LPC.disable_s3=1` and `disable_s4=1`. `lord-vm` gives exactly these (`timers`). Whonix's XML also has a SPICE channel (`com.redhat.spice.0`, vdagent) and a serial console; Machines gives neither (no virtio-serial at all) |
| `pkg/pgp.c` on Whonix's real signature | Verifies the 2026-11-04 `.sha512sums.asc` (RSA signing subkey `6E97 9B28 A6F3 7C43 BE30 AFA1 CB8D 50BB 77BB 3C48`, SHA-512, a critical creation time, Sequoia's salt notation and a `file@name` notation); a changed digit, another pinned key and a time before its creation are refused. The list and signature are `vmcatalog-test` fixtures (`tests/vmimage/whonix.*`) |
| xz speed and the unpacked sizes (LXQt 18.2.4.3) | The archive: 1,957,016,760 bytes, one xz stream of 275 blocks of 24 MiB (LZMA2, 8 MiB dictionary, CRC64) holding 6,917,150,720 bytes of tar: the license, the disclaimer, the two XMLs, then the two disks as old-GNU sparse members, each 107,384,668,160 bytes (100 GiB of qcow2 with preallocated metadata) holding 3,063,865,344 (gateway) and 3,853,221,888 (workstation) bytes of data. Our decoder: 98.7 s alone (about 70 MB/s out, 11 MB of memory); `xz -T1` 47 s; `xz -T0` 4.8 s (20 threads, 1.4 GB). `lord-vmunpack` on the whole archive: 104 s; its two files are byte for byte GNU tar's (`cmp`), slightly sparser. The whole `--group-create` from the verified archive: 236 s on the dev host. The bases: 2.85 GiB (gateway) and 3.59 GiB (workstation), 100 GiB virtual, `qemu-img check` clean. Space at the peak: the archive, the unpacked data and one base, about 11 GB |
| The internet only through the Gateway (`tests/vm_group_link.sh`) | With both members running, `curl https://check.torproject.org/api/ip` in the workstation answers `"IsTor":true`. With the gateway off, the workstation has no network at all (`curl` times out, 10.152.152.10 unreachable: the cable is pulled, nothing else is reachable). The gateway started again is linked again by the session by itself and the workstation's Tor works again |
| Shut Down on Whonix's members | **Solved 2026-11-24, without an agent and without changing the images.** The cause, read in both guests: `lxqt-powermanagement` takes logind's `handle-power-key` *block* inhibitor, and under labwc (Wayland, no `lxqt-globalkeysd`, no `XF86PowerOff` binding) nothing acts on the key, so logind (`Power key pressed short.`) does nothing; `acpid` runs `powerbtn-acpi-support.sh`, which leaves the key to logind (`HasDBusLogin1`). On the Workstation, **kloak** (Whonix's keystroke anonymizer, 0.9.1) also grabs every input device with `EVIOCGRAB`, the Power Button included, so logind never even sees it. logind ignores the inhibitors of a session that isn't active. The fix, in `lord-vm`'s Shut Down for an anonymity-profile member (`anon_power_tick`): QMP `send-key` **Ctrl+Alt+F2** (labwc switches to tty2: the desktop session is inactive), 1 s later **Right Shift+Esc** (kloak's own exit combination: it lets go of the devices; systemd restarts it 2 s later), 0.3 s later the **ACPI power button**: logind powers off, every service stopping cleanly, Tor included; the sequence again every 20 s for a guest still booting. Measured: the Gateway off in 3 s and the Workstation in 6 s with QEMU alone; `machines stop Whonix` in 8 s for the whole group, both `host-shutdown` (never forced), the Gateway's serial log showing logind's `Powering off…` and `tor@default.service: Deactivated successfully` (`vm_group_link.sh`). The keys only reach the guest's desktop and console, the same as the person typing them; no image is changed |

**How the test disks are made** (for `tests/vm_group_link.sh`, never shipped): the two members' qcow2 files from the
verified archive, converted to raw, `systemd.debug_shell=ttyS0` appended to every `linux` line of `/boot/grub/grub.cfg`
with `debugfs -w` (offset of the third partition), converted back; given as `WHONIX_GW` and `WHONIX_WS`. A root shell on
the serial console is the only change; `lord-vm`'s VM_CONSOLE reaches it.

### 19.1 Goals and threat model

Whonix is two Debian machines. The **Gateway** runs Tor and is the only one with a way out. The **Workstation** (Tor
Browser, the person's apps) has one network card, wired only to the Gateway, so everything it sends goes through Tor,
whatever an app or a malware in it tries. Whonix does that inside the guests (Tor, its firewall, stream isolation, the
fixed internal addresses); Machines has to give it exactly that wiring and nothing more.

**Goals.**

1. One card in the New Machine assistant ("Whonix (Tor)", amd64 with KVM only): the official KVM images, verified with
   Whonix's signing key, and two machines made from them with no question beyond a name.
2. A **group**: one card in the library (the Workstation's picture), started Gateway first and stopped Workstation
   first, both members reachable from the card's menu, one name for the `machines` command.
3. The Workstation has **no network but the link to the Gateway**: no passt, no host route, no DNS of this computer.
   The link reaches no other process of this computer and no other machine.
4. **Nothing of this computer goes into the guests** by default: no clipboard, transfer folder, shared folder, dropped
   files, guest agent, microphone, 3D (host GPU strings) or thumbnails on disk.
5. **Throw away and start again**: "Start Fresh" puts the Workstation (and, if asked, the Gateway) back to the verified
   image in seconds, with a new host-side identity. An optional **disposable session** forgets the Workstation's
   changes at every power-off.

**What it protects against.**

- An app or a malware **inside the Workstation**, root included, learning this computer's public IP, its LAN address or
  its DNS servers: it has no route but the Gateway, and the Gateway only forwards Tor.
- DNS leaks of the Workstation: its resolver is the Gateway's Tor DNS, and passt's DNS forwarder isn't given to either
  member.
- **Linking sessions by what the host hands the guests**: the machines' UUIDs and MAC addresses are random per group
  and new at every Start Fresh. The other SMBIOS strings stay QEMU's stock values, the same for every KVM Whonix (19.5:
  random values there would make a group *more* recognisable, not less).
- Other processes of this computer **reading or injecting** frames on the internal network: the link is an anonymous
  socketpair (19.3), not Whonix's own `udp` on 127.0.0.1:5577/6688, which every local program can reach.
- Files and clipboard of this computer reaching the Workstation, and browsing state surviving a Start Fresh.

**What it doesn't protect against** (the explanation step says the short form):

- **A compromised host.** LordOS draws the Workstation's screen and gets its keyboard, so malware in the person's
  session sees everything. A compromised LordOS is out of scope.
- **A VM escape** (QEMU or KVM) from the Workstation: our QEMU sandbox (3.3) limits it, but an escape lands in the
  person's account, where the real IP is. The same goes for a **compromised Gateway**.
- **Guest fingerprinting**: `-cpu host` shows the real CPU model to code that runs in the Workstation (Whonix's own XML
  uses host-passthrough too); the window's size becomes the guest's screen size (Tor Browser's letterboxing is the
  answer); typing rhythm and behaviour. That is Tor Browser's job, not Machines'.
- **Timing and traffic correlation** against Tor itself (a global observer, a hostile guard). **Tor use is visible** to
  the network provider and to this computer: the Gateway's connections go out through passt in its own scope, so
  LordOS's firewall, Centinela's map and Monitor show connections to Tor relays as the Tor gateway's (`lordos:tor-gateway`,
  19.5.1; bridges are set up inside the Gateway).
- **Forensics on this computer**: the disks are files in `~/Machines`; deleting an overlay on an SSD doesn't erase it,
  swap may hold guest memory, and the logs say when the group ran. This is not an amnesic system, and the explanation
  says so.
- **Side channels** between the two guests or with the host (Spectre-class, rowhammer). Pinning the members on
  different physical cores and keeping guest memory off huge pages and KSM (19.5) reduce them, nothing more.
- The person signing in to an account that names them.

**Layer 9: Sentinela** (6.28, approved 2026-11-23; off by default). With Sentinela on, a member's QEMU leaving its
scope, a member's sandbox missing a part, or the Workstation showing any network path besides its link is always
alerted; a Gateway whose traffic or remote addresses change sharply, unexpected ends, restarts and a pegged CPU are
alerted too. It reads the members' `role` and `profileLock` (19.4) from `lord.vm.list()`; it watches and tells, and
acts only on the person's click.

### 19.2 Architecture

```
~/Machines/Whonix.lordgroup/                  the group (one card); the name is the person's
  group.json                                  members, roles, order, policy (19.4)
  Gateway.lordvm/                             an ordinary machine folder (3.5), machine.json "group" set
    disks/system.qcow2                        overlay over the Gateway base
  Workstation.lordvm/                         the primary member (the card, `machines start Whonix`)
    disks/system.qcow2                        overlay over the Workstation base
    disks/session.qcow2                       only during a disposable session (19.7)
~/Machines/.shared/whonix-18.2.4.3-gateway-<sha12>.qcow2       read-only bases, made once from the verified
~/Machines/.shared/whonix-18.2.4.3-workstation-<sha12>.qcow2   archive (19.6), shared by every Whonix group
/var/lib/lordos/machines/images/<sha512>.tar.xz                 the verified download (root's, 19.6)
```

- **Members are ordinary machines.** Each member is a `.lordvm` folder with its own `machine.json`, lock, `run.json`,
  `lord-vm`, QEMU, sandbox and cgroup scope. Nothing in `lord-vm`'s life cycle changes; a member only gets a few new
  `machine.json` keys (19.4). A group adds an order and a link, not a new kind of process.
- **The group is the app's.** `lord.vm.list()` (`js_vm.c`) also reads `<library>/*.lordgroup/` (one level, at most 4
  members, each a `*.lordvm` inside it), and the app shows the group as one card. Starting and stopping in order is a
  pure plan in `model.js` (`groupPlan`, tested in node) that drives the existing `lord.vm` calls, as the window does for
  one machine now.
- **The link is lord-server's to make** (19.3): it is the one process that starts both members' `lord-vm`s
  (`--control-fd 3`), and that control channel already carries descriptors (`vmproto.h`: `VM_CDROM`, `VM_FILE`).
- A member's base is a linked-clone base (6.3): `machine.json` `base` names it (`vm_base_valid`: absolute, plain, in a
  `.shared` folder, `.qcow2`), and QEMU's Landlock already gets read access to exactly that file (`VmQemuJail.base`).

### 19.3 The internal link

**What the first proposal had, and why it changes.** The idea was QEMU's `-netdev stream` (or `dgram`) on a Unix socket
in the group's folder, the Gateway's QEMU listening and the Workstation's connecting, with Landlock letting each QEMU
reach only that socket. Our code and kernel rule that out:

- Our seccomp allowlist refuses `socket` to QEMU and has no row for `bind`, `listen`, `connect` or `accept` (`vmsyscalls.h`). QEMU only gets
  sockets that `lord-vm` made (passt's socketpair, the TPM's, virtiofsd's connected socket). A listening QEMU would be a
  sandbox exception for no gain.
- Landlock in Debian 13's kernel (6.12, ABI 6) has no rule for connecting to a path-named Unix socket; it only scopes
  abstract ones. "Only that socket" can't be expressed, and any process of the person could connect.
- Whonix's own KVM files (18.2.4.3) use `<interface type='udp'>` on 127.0.0.1:5577 ↔ 6688. QEMU would need its own UDP
  sockets (refused), and every local program could send frames into the Workstation's network or read them.

**The design.** One `socketpair(AF_UNIX, SOCK_STREAM)` per pairing, made by lord-server, one end to each member:

- Each member's internal card sits on a QEMU hub of its own (hub 1 on the Gateway, hub 0 on the Workstation), as
  Shared's card already does with passt (`lordvm.c`: `hubport` + `stream,addr.type=fd`). The card exists from the
  start; the backend comes and goes. A card without a backend is a pulled cable: the Workstation then has **no network
  at all**, which is the fail-closed state.
- When both members run, lord-server makes a pair and sends one end to each `lord-vm` with a new control message,
  **`VM_LINK`** (one descriptor). `lord-vm` checks the descriptor (`SO_DOMAIN` AF_UNIX, `SO_TYPE` SOCK_STREAM, the peer's
  `SO_PEERCRED` uid its own), passes it to QEMU with QMP `getfd`, puts it on the hub (`netdev_add` + `hubport`) and
  removes the previous one (`netdev_del`). These are the steps of passt's self-healing (`NET_MAX_RESTARTS`), with the
  descriptor coming from lord-server instead of the keeper. A member that starts while the other runs gets its end on
  its command line (`--link-fd`, as `-netdev stream,addr.type=fd`), and the running one gets its end by `VM_LINK`.
- **Restart of either member** (its QEMU ends: power off, crash, Start Fresh): the other's end reads end-of-file and its
  link is down; nothing else is reachable meanwhile. When the member is back, lord-server makes a new pair and sends
  both ends. A guest's own reboot keeps its QEMU, so the link stays.
- QEMU's `stream` and `socket` netdevs use the same framing (a 4-byte length before each frame), so the two QEMUs talk
  to each other directly. The frames never pass through lord-server or `lord-vm`, and nothing on the host parses them.
  **To check (G0):** two QEMU 10.0.13 under our sandbox exchanging frames over such a pair, one end given at start and
  one by `netdev_add`, and the swap under load.
- No address is the host's: no `tap`, bridge, route or firewall rule, and no root. Whonix's fixed internal addresses
  stay exactly as Whonix configures them (Gateway 10.152.152.10, Workstation 10.152.152.11, in 10.152.152.0/18). They
  exist only on that wire and must not change, or Whonix's firewall and Tor's listeners stop matching. What is random
  per group is what the host assigns (19.5), not these.
- Without a session (the `machines` command with no lord-server running) a group doesn't start: exit 5, "needs the
  LordOS session", as a start in a window already does.

**Card order.** Whonix names the cards by order: `eth0` is the way out and `eth1` the internal network on the Gateway;
the Workstation's only card is `eth0`. The Gateway's passt card stays at slot 0x2 and its internal card goes at 0x18
(where an imported machine's second card goes); the Workstation's internal card is at 0x2. **To check (G0):** `ip link`
in both images with these slots.

**MAC addresses** are random per group (`getrandom`, QEMU's `52:54:00` prefix like every machine) and new at every Start
Fresh. They matter little (the Gateway's outside MAC is seen only by passt, the Workstation's only by the Gateway), but
they cost nothing and keep two groups or two sessions apart.

### 19.4 Data formats

**`group.json`** (new, schema 1; a strict C parser in `server/src/vm/vmgroup.{c,h}` with its test and fuzz pass; the app
reads it only through `lord.vm.list()`):

```json
{
  "schema": 1,
  "id": "6f1c…",
  "name": "Whonix",
  "kind": "whonix",
  "members": [
    { "role": "gateway",     "dir": "Gateway.lordvm",     "start": 1, "stop": 2 },
    { "role": "workstation", "dir": "Workstation.lordvm", "start": 2, "stop": 1, "primary": true }
  ],
  "image": { "catalog": "whonix", "release": "18.2.4.3", "edition": "LXQt",
             "archive_sha512": "e57c79cb…5354",
             "bases": { "gateway": "/home/<user>/Machines/.shared/whonix-18.2.4.3-gateway-<sha12>.qcow2",
                        "workstation": "…" },
             "base_sha256": { "gateway": "…", "workstation": "…" } },
  "disposable": false,
  "fresh_gateway": false
}
```

`id` is a random UUID made once (the group's id for `machines`); `kind` is `whonix` only for now (a generic group isn't
offered, 19.13); `disposable` is the Workstation's disposable session (19.7); `fresh_gateway` remembers the Start Fresh
dialog's "Also reset the Gateway" box. Rules: `dir` a plain name ending `.lordvm` inside the group folder (no `/`, no
`..`, no link); exactly one `primary`; the start and stop orders each a permutation of 1..n; at most 4 members; unknown
keys refused, and a newer schema opened read-only, as for `machine.json` (3.6); bases must pass `vm_base_valid`.
`group.json` is written by rename, like `machine.json`.

**`machine.json` schema 14** (members only; other machines unchanged):

| Key | Gateway | Workstation | Meaning |
|---|---|---|---|
| `group` | group id | group id | member of that group: Rename, Clone, Duplicate, Delete and Move act on the group, not on one member |
| `role` | `"gateway"` | `"workstation"` | |
| `network` | `"shared"` with `"uplink": "tor"` | `"link"` (new) | `link`: one card, on the group's link only. `uplink: "tor"`: passt without DNS forwarding or ports (19.5) |
| `link_mac` | random | (none) | the Gateway's second card; the Workstation's card is `mac` |
| `profile_lock` | `"anonymity"` | `"anonymity"` | `lord-vm --set` and Configure refuse what 19.5 turns off |
| `timers` | `"whonix-gateway"` | `"whonix-workstation"` | the clock devices of 19.5 |
| `firmware` | `"bios"` | `"bios"` | Whonix's XML has no UEFI loader: SeaBIOS (`VM_FW_BIOS`, no NVRAM) |

### 19.5 The anonymity profile and each member's QEMU

Both members get the following, set at creation and locked (`profile_lock`). It follows Whonix's own advice (the
"Unsafe Features" of its KVM page: clipboard sharing, memory ballooning, KSM, huge pages, device passthrough) and closes
our own channels:

| Channel | Both members | Why |
|---|---|---|
| Clipboard, transfer folder, shared folders, dropped files, Open with, apps mode | off (`clipboard` none, `transfer` false, `shares` empty) | the host's data never reaches the guests; Whonix 18 turned its own clipboard off |
| Guest agent (qemu-guest-agent and `org.lordos.agent`) | **off**: no `virtio-serial-pci` at all (`agent` false) | Whonix advises against guest additions; with no port there is no host-to-guest command path. Cost: Shut Down is ACPI only (works, 6.1), no journal in Logs, no agent state |
| Graphics | 2D only (`virtio-vga`, the D-Bus display) | virgl would show the guest this computer's GPU and driver names (a WebGL fingerprint) |
| Sound | Gateway: no sound card. Workstation: `ich9-intel-hda` with `hda-output` (speakers only) | Whonix's XML has an output-only codec; our usual `hda-micro` gives the guest a microphone input even when muted |
| Camera, USB, printers | none (not built anyway) | passthrough is on Whonix's unsafe list |
| Suspend, memory snapshots, saving at host shutdown | refused | guest memory would land on this computer's disk; Whonix's XML turns suspend off. At host shutdown the guests are shut down (the `vmhoststop.h` plan without the save) |
| Thumbnail | `thumb.png` not written (`js_vm.c`); the live picture only while running | a picture of the browsing would stay in `~/Machines` |
| Captures of the screen (isolation layer 8; **built**, in development, approved 2026-11-23) | the members' windows and their apps-mode windows are **protected windows** (PRIVACY.md, "Protected windows"): on the display as usual, an opaque neutral fill with a lock in every screenshot, recording, remote desktop frame, screen share (whole screen or the window) and portal still. `lord.vm.protect(session)` (one way) refuses the card picture (`lord.vm.snapshot`, and the old `thumb.png` is removed), `lord.vm.screenshot`, `lord.vm.record` (one running stops) and a snapshot's picture; the library card never shows a `thumb.png` for it | the Workstation's browsing on screen would otherwise reach any capture, a remote viewer or a share |
| Guest memory | `memory-backend-memfd,share=off` with transparent huge pages off for the QEMU process (`PR_SET_THP_DISABLE`, whatever `shmem_enabled` says), `-machine mem-merge=off` (no KSM), no hugetlb | Whonix lists huge pages and KSM as unsafe (rowhammer, cross-VM deduplication). Cost: about 30 % slower random memory access (measured in 3.7.2) |
| Core dumps | `dump-guest-core=off`, none, non-dumpable, as every machine (3.3) | |
| Identity | `-uuid` random per group and per Start Fresh; no `-smbios` strings (QEMU's stock "Standard PC (Q35 + ICH9, 2009)"); no disk serial; no first-boot credentials (`type=11`) | unique strings would mark the group; stock values are what every KVM Whonix shows. Whonix's own hostname and machine id inside the guests are left as Whonix sets them |
| vCPU pinning | Automatic (3.7.1), the members on different physical cores with no shared SMT sibling | Whonix's XML pins the Gateway and the Workstation on CPUs 0 and 1 |
| Generation id | `-device vmgenid,guid=auto` | Whonix's XML has `<genid/>`: the guest's kernel reseeds its random generator when its disk or snapshot changes generation |

**Gateway** (what differs from a Shared Linux machine in `lordvm.c`):

```
qemu-system-x86_64 -nodefaults -no-user-config -name guest=Whonix-Gateway,debug-threads=on -uuid <random>
  -machine pc-q35-10.0,dump-guest-core=off,mem-merge=off,vmport=off,memory-backend=mem
  -object memory-backend-memfd,id=mem,size=2048M,share=off
  -accel kvm,device=/dev/fdset/1 -add-fd fd=3,set=1,opaque=kvm
  -cpu host,pmu=off -smp 1 -m 2048
  -rtc base=utc,driftfix=slew                  # Whonix: rtc tickpolicy=catchup, kvmclock present
  -device vmgenid,guid=auto
  -blockdev …system.qcow2 (backing: the Gateway base)… -device virtio-blk-pci,…,bootindex=1
  -netdev hubport,id=nic0,hubid=0 -netdev stream,id=pst0,addr.type=fd,addr.str=6 -netdev hubport,id=hp0,hubid=0,netdev=pst0
  -device virtio-net-pci,netdev=nic0,mac=<random>,addr=0x2                                   # eth0: passt
  -netdev hubport,id=lnk0,hubid=1 [-netdev stream,id=lk0,addr.type=fd,addr.str=<N> -netdev hubport,id=lh0,hubid=1,netdev=lk0]
  -device virtio-net-pci,netdev=lnk0,mac=<random>,addr=0x18                                  # eth1: the link
  -device virtio-rng-pci,addr=0x3
  -device virtio-vga… -display dbus,p2p=yes    # no sound, no virtio-serial, no USB tablet, no -smbios
passt -f -q -1 --fd 3 --no-map-gw --no-dhcp-dns --no-dhcp-search -t none -u none
```

The Gateway's passt is Shared's without `--dns-forward` and without forwarded ports (`uplink: "tor"`). Tor needs only
outgoing connections, and a Gateway that can't reach this computer's resolver can't leak a lookup through it even if its
own firewall failed. `--no-map-gw` already keeps this computer's loopback out of reach (6.5).

**Workstation:**

```
  -machine pc-q35-10.0,dump-guest-core=off,mem-merge=off,vmport=off,memory-backend=mem
  -object memory-backend-memfd,id=mem,size=2048M,share=off
  -cpu host,pmu=off,kvmclock=off -smp 1 -m 2048   # Whonix: kvmclock, rtc, pit, hpet "present='no'"
  -device vmgenid,guid=auto
  -netdev hubport,id=nic0,hubid=0 [-netdev stream,…,addr.type=fd + hubport]
  -device virtio-net-pci,netdev=nic0,mac=<random>,addr=0x2                                   # eth0: the link, nothing else
  -device virtio-rng-pci,addr=0x3
  -device ich9-intel-hda,… -device hda-output,…   # speakers only
  -device virtio-vga… -display dbus,p2p=yes
```

**Time.** Whonix keeps the Workstation away from the host's precise clock: its `sdwdate` sets the time from onion
sources through the Gateway, with a random offset at boot. Its XML gives the Workstation no kvmclock, RTC, PIT or HPET,
and the Gateway an RTC with catch-up and kvmclock. Which QEMU options libvirt makes of `present='no'` for `rtc`, `pit`
and `hpet` on q35 (the ICH9 RTC is part of the chipset) is **To check (G0)**: run `virsh domxml-to-native qemu-argv` on
Whonix's two XMLs on the dev VM and mirror what QEMU 10.0.13 gets, rather than guess.

Memory and vCPUs are Whonix's defaults: 2 GB and 1 vCPU each (the LXQt Gateway needs at least 768 MB, or it boots to its
console). Configure may change the Workstation's memory and vCPUs and the Gateway's memory. The group needs about 4 GB
free, checked by `groupBudget` before the start (and each member again by lord-server's budget, 3.7).

#### 19.5.1 The Gateway's firewall identity (isolation layer 4; built, in development)

Approved by the maintainer on 2026-11-23. The Gateway (the member with `"uplink": "tor"`) runs in
`app-lordos-lordos-vm-tor-<id>.scope`, the firewall app **`lordos:tor-gateway`**, with a restriction built into
lord-netperm (`FW_R_TOR_UPLINK`, never in `firewall.json`, decided before any rule): only outgoing TCP to internet
addresses; no incoming, no UDP, ICMP or raw socket (refused when the socket is made: the Gateway's passt needs none),
no LAN, private, link-local, ULA, multicast, loopback or unknown address, no special or documentation range and no
IPv6 range that carries an IPv4 inside (NAT64, 6to4, Teredo, IPv4-compatible), none of this computer's own addresses or
the networks of its links (`np_local`, read over rtnetlink; when they don't fit, every address is refused), and no
remote port 53 or 853. Monitor and Settings show it as its own row, "Tor
gateway · <name>". A Gateway guest whose own firewall failed (or root malware in it) still can't resolve a name
outside Tor, reach the LAN or this computer, or send UDP. docs/FIREWALL.md, "The Tor gateway", has the details;
`npcompile-test`, `np-torgw-test` (root, a throwaway cgroup) and `tests/vm_gateway_fw.sh` (a group under the policy:
`IsTor: true` in the Workstation, and from the Gateway with its nftables flushed, UDP DNS, TCP 53/853, LAN and host
refused while TCP 443 connects) test it.

### 19.6 Catalog entry, verification and unpacking

**What Whonix publishes** (read 2026-11-23 on `https://download.whonix.org/libvirt/18.2.4.3/`; 18.2.4.3 is the current
version on `https://www.whonix.org/wiki/KVM`). For each edition (LXQt desktop, CLI): the archive
`Whonix-LXQt-18.2.4.3.Intel_AMD64.qcow2.libvirt.xz` (1,957,016,760 bytes; the CLI one 964,237,220 bytes), its OpenPGP
signature `….libvirt.xz.asc`, a SHA-512 list `….libvirt.xz.sha512sums` (one line), that list's OpenPGP signature
`….sha512sums.asc` and signify signature `….sha512sums.sig`, and torrents. amd64 only: there is no arm64 KVM image. Each
release is a folder (`18.2.1.9/` to `18.2.4.3/` today). The key is `https://www.whonix.org/keys/derivative.asc`,
primary key **`916B 8D99 C38E AF5E 8ADC 7A2A 8D66 066A 2EEA CCDA`** (Patrick Schleizer, RSA 4096, 2014); the 2026-11-04
signatures were made by its signing subkey `6E97 9B28 A6F3 7C43 BE30 AFA1 CB8D 50BB 77BB 3C48` (RSA, SHA-512, with
Sequoia's salt notation and a critical creation time). The signify key is `https://www.whonix.org/keys/derivative.pub`
(`RWQ6KRormNEETq+M8IysxRe/HAWlqZRlO8u7ACIiv5poAW0ztsirOjCQ`). **Before the entry ships**, the fingerprint must be compared
over https with the one on Whonix's signing key page (`fingerprintChecked`), as for every card.

**The entry** (`assets/machines/systems.json`; the key file pinned as `packaging/keys/machines/whonix-derivative.asc`):

```json
{ "id": "whonix", "name": "Whonix", "release": "18", "kind": "group", "group": "whonix", "edition": "LXQt",
  "homepage": "https://www.whonix.org/wiki/KVM",
  "hashSource": "the .sha512sums list Whonix publishes next to the KVM archive, its OpenPGP signature .sha512sums.asc checked with Whonix's signing key",
  "signature": { "type": "detached", "suffix": ".asc" }, "keys": "whonix-derivative.asc",
  "fingerprints": ["916B8D99C38EAF5E8ADC7A2A8D66066A2EEACCDA"], "hash": "sha512",
  "keySource": "https://www.whonix.org/keys/derivative.asc, fetched 2026-11-23", "fingerprintChecked": "…",
  "checked": "2026-11-23",
  "archive": { "format": "tar.xz",
               "members": { "gateway":     "^Whonix-Gateway-LXQt-[0-9]+(\\.[0-9]+){3}\\.Intel_AMD64\\.qcow2$",
                            "workstation": "^Whonix-Workstation-LXQt-[0-9]+(\\.[0-9]+){3}\\.Intel_AMD64\\.qcow2$" },
               "maxApparentGb": 128, "maxDataGb": 24 },
  "machine": { "gateway": { "vcpus": 1, "memoryMb": 2048 }, "workstation": { "vcpus": 1, "memoryMb": 2048 } },
  "arch": { "amd64": { "dir": "https://download.whonix.org/libvirt/18.2.4.3/",
                       "sumsPattern": "^Whonix-LXQt-[0-9]+(\\.[0-9]+){3}\\.Intel_AMD64\\.qcow2\\.libvirt\\.xz\\.sha512sums$",
                       "file": "^Whonix-LXQt-[0-9]+(\\.[0-9]+){3}\\.Intel_AMD64\\.qcow2\\.libvirt\\.xz$",
                       "approxMb": 1867, "maxMb": 4096 } } }
```

What changes in the existing machinery (no new admin action, but changes to two; 19.12):

- `vmcat_find` learns `"hash": "sha512"`, `"signature": { "suffix" }` (the detached signature is the list's own name
  plus `.asc`, for a list found by `sumsPattern`), `kind: "group"` and `archive`. `vmcat_sums_has` accepts a 128-digit
  hash when the entry says SHA-512 (never chosen by JS).
- `vm-image-list` fetches the list and `<list>.asc` as it fetches lists now, and `vmcat_verify_list` checks it with the
  pinned key. **To check (G0):** `pkg/pgp.c` verifies the real 2026-11-04 signature (an RSA subkey with its
  back-signature, SHA-512, a critical creation-time subpacket, the notations); the three small files become a
  `vmcatalog-test` fixture.
- `vm-image-get "<id> <arch> <file> <sha512>"` hashes the download with SHA-512 (`pkg/crypto.c` has it) and keeps it as
  `images/<sha512>.tar.xz` (the extension from the entry's archive format, never from JS), resumed from `.part`, at most
  `maxMb`. Root still never parses the archive.
- The signify signature is **not** checked: it is the same person's key in a second format, and it would be a second
  code path in root. The OpenPGP list signature is what every other card uses (a decision, 19.13).

**Unpacking.** The archive is a GNU tar compressed with xz (Whonix's instructions: `tar -xSvf`). Checked 2026-11-23 on
the first 8 MB of the CLI archive: the members are `WHONIX_BINARY_LICENSE_AGREEMENT`, `WHONIX_DISCLAIMER`,
`Whonix-Gateway.xml`, `Whonix-Workstation.xml`, then `Whonix-Gateway-CLI-18.2.4.3.Intel_AMD64.qcow2` as an **old-GNU
sparse member** (type `S`, magic `ustar` followed by two spaces, an extended sparse map) whose real size is
107,384,668,160 bytes (100 GiB of qcow2 address space with preallocated metadata) holding 1.6 GB of data; the
Workstation's disk follows. After the SHA-512 check the archive is Whonix's, but a signed file is still parsed in a
sandbox, as everything from outside is:

- A new helper, **`lord-vmunpack`** (`server/src/vm/vmunpack.c`), started by `lord-vm --group-create` (a background job
  like `--create`), links only libc and LordOS's own `pkg/xz.c` (our LZMA2 decoder for Debian's indexes; no liblzma, and
  no libarchive, which only `lord-doc` may link). It gets the archive on fd 0 (opened read-only by `lord-vm` from the
  image store), two new empty files in the group's staging folder on fds 3 and 4, and fd 1 for progress and its answer.
  It calls `sandbox_apply_helper` before reading a byte (no files, no network, no exec, no extra syscalls).
- Its tar reader is strict: 512-byte headers with valid checksums. Only types `0` and `S` (old-GNU sparse with extension
  headers) are written; other regular members are read past; anything else (links, devices, directories, long names
  `L`/`K`, pax headers that rename) ends the job. A member is written only if its name matches the entry's pattern for
  a role, each role exactly once. A sparse map must be ascending, non-overlapping and inside the real size; the real
  size is at most `maxApparentGb` and the data of all members at most `maxDataGb`. Output goes by `pwrite` at the map's
  offsets, so the holes stay holes. The xz stream must end where the archive ends; truncation fails the job.
- Then the existing jailed `qemu-img` (`vm_jail_tool`, as Import uses it): `info` with `-f qcow2` must show no backing
  file, no external data file and a virtual size the entry allows; `convert -f qcow2 -O qcow2` writes the base without
  the preallocated metadata and without anything of the original header we didn't choose; `check` runs on the result.
  The temporary files are removed. The base goes to `.shared/` by rename, `chmod 0444`, and its SHA-256 into
  `group.json`.
- The XML files, license and disclaimer aren't used: the members' settings are ours (19.5), and Whonix shows its own
  first-start notices inside the guests. A test (19.10) compares our QEMU arguments with what Whonix's XML asks for, so
  that every move of the catalog to a new release is reviewed.
- Space: the archive (1.9 GB), the unpacked sparse members (to measure for LXQt; the CLI Gateway alone holds 1.6 GB) and
  the bases exist at the same time, so free space is checked before the download and before unpacking. Storage offers
  to remove the archive once both bases exist (Start Fresh needs only the bases; a missing base is made again from the
  archive, and a missing archive is downloaded again).
- The speed of our xz decoder on several GB is **To measure (G0)**.

**Which release.** The catalog pins a release folder, as the LordOS card does. A Start Fresh then always begins from
that image, with an older Tor Browser until Whonix's own updaters run inside. There are two answers for the maintainer
(19.13): move the pin with each LordOS release (simple, may lag), or let the entry name the parent folder
`https://download.whonix.org/libvirt/` and take the newest release folder whose list verifies with the pinned key,
never older than the one the catalog names (a rollback floor).

### 19.7 Start Fresh, disposable sessions, snapshots

- **Start Fresh** (with the group off; `lord-vm --group-fresh`, every member's lock held for the whole job): a new empty
  overlay `disks/.system.qcow2.fresh` over the base is made and renamed over `disks/system.qcow2`, so a cut before the
  rename leaves the old disk and one after it the new one (no `operation.json`: the rename is the only step that
  counts). The member gets a new UUID and new MACs; its snapshots go (they live in the old overlay); its
  `snapshots.json`, `cpu-usage.json`, logs and pictures are removed. It takes seconds and no download. The base's
  SHA-256 is checked against `group.json` first (a few seconds for a ~3 GB file). A base missing or changed is made
  again from the verified archive in the image store (`images/<sha512>.tar.xz`, hashed again against `group.json`'s
  SHA-512), through the same sandboxed `lord-vmunpack` and jailed `qemu-img` as creation, under `.shared/.lock`. With no
  archive, lord-vm exits 4 (`needArchive`) and Machines downloads it again for the group's own release (refused unless
  the signed list still gives the same SHA-512), then runs the job again (built 2026-11-24, in development).
- **Only the Workstation by default.** Resetting the Gateway throws away Tor's state, including its entry guards, which
  Tor keeps for months on purpose so that a hostile relay has fewer chances to become a person's guard. Whonix's own
  design keeps the Gateway long-lived (Qubes-Whonix's disposables are Workstations). The dialog offers "Also reset the
  Gateway" with that sentence; `group.json` `fresh_gateway` remembers the box.
- **Disposable session** (the group's switch, Workstation only; built in memory, approved 2026-11-23 as isolation layer
  6): at each start `lord-vm` makes an empty overlay (headers only) and copies it into a memfd, which QEMU gets as
  `/dev/fdset/2`, over `system.qcow2` opened read-only. Nothing of the session is written to a file and it goes with QEMU
  (power-off, crash, a killed `lord-vm`); no second writable path in QEMU's Landlock was needed (a descriptor, not a
  path). Its cap is the workstation's memory (at least 1 GB), counted in the cgroup limit and the group's budget; at the
  cap the guest pauses (`session-full`). Snapshots are refused during such a session (19.0, "Built in G3–G4").
- **Snapshots** stay per member and disk-only (19.5 refuses memory). The Gateway's Snapshots window warns that going
  back brings back older Tor state. A Start Fresh removes the member's snapshots, and the dialog says how many.

### 19.8 The app

**New Machine** (amd64 with KVM; the card is hidden on arm64 and with emulation):

1. **Choose:** the card "Whonix (Tor)", with its size (about 1.9 GB to download; the space on disk to measure) and "two
   machines, 4 GB of memory while running".
2. **About Whonix** (one new screen): what it is, in two sentences; what it protects and what it doesn't (19.1 in short:
   the host, VM escapes, fingerprinting, timing); "Your network provider can see that you use Tor"; links to Whonix's KVM
   page and license. A checkbox "Start each session fresh" (the disposable session), off.
3. **Name:** "Whonix" (unique in the library, as now).
4. **Review:** both members with memory and vCPUs; "Customize" opens the group's Configure after creation.
5. **Create:** list and signature, download (resumable), SHA-512, unpacking (progress from `lord-vmunpack`), the bases,
   the two members, the group. Then **Start**.

**Library:** one card with the Workstation's live picture while it runs (no `thumb.png` otherwise: a generic picture, or
the Whonix mark if official logos are on, 5.6.2), a small second status dot for the Gateway and the chip "2 machines".
Play starts the group. The card's context menu:

- Start, Shut Down (the group, in order), Force Off (both)
- **Show Workstation**, **Show Gateway** (each opens that member's own window; the Gateway's console is where bridges
  and Tor's state are)
- **Start Fresh…** (the dialog of 19.7; only with the group off), Disposable session (a check item)
- Configure…, Snapshots (per member), Logs (per member)
- Show in Files, Delete (the group and both members; a base goes when no group uses it)

Clone, Duplicate and Rename of a single member aren't offered; Rename renames the group (`renameat2` of the group
folder, as 6.26 does for a machine).

Built (2026-11-24, in development): **Rename…** (`lord-vm --group-rename`, with the group off: `group.json`'s name, the
members' names "NAME Gateway/Workstation" cut on a character boundary, and the folder by `renameat2(RENAME_NOREPLACE)`
when the name asks for another, refused for a name or folder another group or machine has, `vm_group_rename_plan`; also
`machines rename GROUP NEW`). **Delete…** (`lord-vm --group-delete`: for good, no Trash; with the group's lock and every
member's lock held, refused while one runs; the folder is renamed out of sight, checked to be the locked one, and removed
by descriptors without following links; then each base in `.shared` that nothing in the library or the Trash still names
goes, under `.shared/.lock`). **Shut Down the Gateway** while it runs without its Workstation. The Workstation window's
bar while the Gateway is down: "No network: the Gateway is off" with **Start Gateway** (node-tested only: the window is
protected, so no capture shows it).

**Machine window:** the Workstation's window is the ordinary one without the locked buttons (clipboard, folders, apps,
agent), and without Screenshot and Record. It is a protected window, opened so (built: `model.js` `machineProtected`,
`library.jsx` `openMachine`, `vmwindow.jsx`, `appsmode.jsx`), and so is the Gateway's. While the Gateway is down, its bar says "No network: the Gateway is off" with **Start Gateway**.

**Configure** for a group: a member switcher (Workstation, Gateway) over the usual one-screen Configure. The anonymity
rows (19.5) are shown off and dimmed with "Off for anonymity". A Group section has the disposable switch, the order
(read-only), the release, and "Check for a newer Whonix" (19.6).

Texts go through `t('machines.group.*')` and `t('machines.whonix.*')`, in every UI language in the same change (the
all-languages rule). The brand rule allows "Whonix" as the label of the download; the rest says "Tor gateway" and
"workstation".

### 19.9 The `machines` command and failures

```sh
machines list                      # one row: Whonix  group (Workstation, Gateway)  running | off | partial
machines start Whonix              # through the session: the Gateway, then the Workstation once the Gateway runs
machines stop Whonix               # the Workstation (ACPI, then forced after the usual wait), then the Gateway
machines start Whonix --member gateway        # one member (--member workstation starts the Gateway first)
machines status Whonix --json      # adds "group": {"id", "members": [{"role", "state", …}], "disposable"}
machines fresh Whonix [--gateway] [--yes]     # Start Fresh; needs the group off; asks unless --yes
```

The session channel's `start <dir>`, `open <dir>` and `start-detached <dir>` also take a group folder (absolute, ending
`.lordgroup`, with the same strict checks). `fresh` runs `lord-vm --group-fresh DIR` itself and needs no session. `set`
on a member refuses the locked keys (exit 2, saying which).

**When the Gateway is "up".** With no guest agent there is no Tor state to read, so the Workstation starts when the
Gateway's `lord-vm` reports `running`. That is enough: the link exists from then on, and Whonix's Workstation waits for
Tor by itself (sdwdate, Tor Browser's connection screen).

**Failures:**

| Event | What happens |
|---|---|
| Signature or SHA-512 wrong | the download is thrown away (as for every card, `vm.listBadSignature`) |
| Unpacking fails (a bad archive, a limit, no space) | the staging folder is removed, the archive stays, the step says why |
| The Gateway fails to start | the Workstation isn't started; the card shows the Gateway's failure page |
| The Workstation fails to start | the Gateway keeps running; the card offers Stop Gateway |
| The Gateway stops or crashes while the Workstation runs | the link goes down (no network: fail closed); the Workstation's bar offers Start Gateway; at its start, a new pair (19.3) |
| lord-server ends while the group runs | the QEMUs keep their link; a member restarted before the session is back starts without it |
| A member folder, a base or `group.json` missing or invalid | the card says the group is damaged, with Repair (make the member again from the base, after a confirmation) or Delete |
| Host shutdown | the Workstation's guest shutdown, then the Gateway's, inside logind's 45 s (no saves, 19.5) |
| `VM_LINK` with a bad descriptor | refused and logged; the member stays without network |

Built (2026-11-24): the start order and the Workstation not started after a Gateway failure (`groupPlan`); the link
going down with the Gateway and coming back with a new pair; the bad descriptor; Stop Gateway on the card when the
Workstation fails to start, and Start Gateway in the Workstation's bar; a damaged group (a member folder missing or not
the group's own, a base missing, or `group.json` unreadable) shown on its card with **Repair** (`lord-vm --group-repair`:
a missing or changed base made again from the verified archive, members over a changed base started fresh, a missing
member made again from its base, all after a confirmation) and **Delete**; a `group.json` that can't be read is deleted
only. At host shutdown each member is shut down by its own lord-vm (never saved), not in the group's order.

### 19.10 Tests

- `build/vmgroup-test`: the `group.json` parser (every rule of 19.4, a fuzz pass under ASan) and the schema 14 keys.
- `build/vmunpack-test`: tar and sparse fixtures made by the test (plain; old-GNU sparse with extension headers; a map
  out of order, overlapping or beyond the real size; a real size or data over the caps; long names, links, pax; a member
  twice or missing; a truncated stream, trailing data, a bad xz block) and a fuzz pass; the output's holes must stay
  holes.
- `build/vmcatalog-test`: the Whonix entry, a SHA-512 list line, the `.asc` suffix, and the real list, signature and key
  of 18.2.4.3 as fixtures (the whole check through `vmcat_verify_list`).
- `build/vm-test`: both members' QEMU arguments (no `virtio-serial`, no `hda-micro`, `mem-merge=off`, `vmgenid`, the
  timers, the slots, passt without `--dns-forward`), `VM_LINK`'s descriptor checks, the refused `--set` keys.
- `tests/vm_group_model_test.mjs`: `groupPlan` (start and stop order, each failure row of 19.9, Start Fresh's scope).
- `tests/vm_group_link.sh` (headless, two throwaway guests of the host's kernel): frames cross the link; killing one QEMU
  takes the link down; a new pair brings it back; the Workstation-role guest reaches nothing else (no DHCP, no host, no
  internet).
- `tests/vm_group_manage.sh` (lord-vm `--group-delete/rename/repair/fresh` on throwaway groups from a fixture archive)
  and `tests/machines_group_manage_headless.sh` (the same through `lord.vm` in a headless lord-server in its own
  namespaces); `vmgroup-test` also covers `vm_group_rm_at`, the group and member locks, `vm_group_rename_plan` and
  `vm_group_base_users`.
- `tests/vm_whonix_xml_test.sh`: reads the two XMLs of the pinned release (a fixture) and fails when Whonix asks for
  something 19.5 doesn't give (new devices, timers, memory), so that a catalog move is reviewed.
- **A release gate on the dev VM and the maintainer's PC**, with a screendump each: the card through creation; Tor
  Browser reaches an onion site and an IP-echo page shows a Tor exit; in the Workstation, `curl` to an IP address and a
  DNS query to 1.1.1.1 fail; a capture on the host shows only passt's connections to Tor relays and nothing from the
  Workstation; a Gateway restart with the Workstation running; Start Fresh and a disposable session; the group's stop
  order.

### 19.11 Build plan

| Phase | Work | Done when | State (2026-11-23) |
|---|---|---|---|
| G0, checks | the To check items: the link between two sandboxed QEMUs and its swap; the card order in both images; libvirt's arguments for the timers; `pgp.c` on the real signature; xz speed and the unpacked sizes of LXQt | each one answered here, with a date | done (19.0); Shut Down by ACPI still open |
| G1, download | the catalog keys (19.6), SHA-512 in `vm-image-list` and `vm-image-get`, `lord-vmunpack`, the bases through jailed `qemu-img` | bases made from a real download, tests green | done; the admin actions not yet run as root against Whonix's server |
| G2, groups | `group.json`, schema 14, groups in `lord.vm.list()`, `groupPlan`, `VM_LINK`, the library card and menu, `machines` | a headless group start and stop with the link, `vm_group_link.sh` | done (the library card and menu with G3) |
| G3, anonymity | the profile's locks and arguments (19.5) in `lord-vm`, Configure's group view | `vm-test` arguments, Configure on screen | done: `vm_whonix_xml_test.sh` checks the arguments against Whonix's XML; Configure's group view, the card, the assistant and the library menu (screenshots) |
| G4, fresh | Start Fresh, the disposable session, the snapshot rules | dev VM screendumps | done headless (`vm_group_link.sh`); also the Gateway's firewall identity and the clean Shut Down |
| G5, release | the gate of 19.10 on real hardware, CHANGELOG, paper, site, the AGENTS.md entries | the maintainer's OK | not started |

### 19.12 Approved by the maintainer on 2026-11-23 (to add to AGENTS.md)

All six were approved, with 6 (the overlay in memory) left to the agent that builds the disposable session.

1. **A new sandboxed helper, `lord-vmunpack`**: it reads the verified Whonix archive (fd 0) and writes exactly two files
   (fds 3 and 4); `sandbox_apply_helper` with no extra syscalls; it links libc and `pkg/xz.c` only; started by `lord-vm`,
   never by root. (The alternative, the same parser in a `lord-vm` child under `vm_jail_tool`, reads one path and writes
   one folder: weaker than a helper with no files at all.)
2. **Changes to the root actions `vm-image-list` and `vm-image-get`**: SHA-512 lists, a detached signature found by its
   suffix, the `.libvirt.xz` extension, all from the signed catalog and never from JS. Optionally, the "newest verified
   release folder" lookup of 19.6.
3. **A guest-to-guest channel**: lord-server makes a socketpair for each pairing and gives its ends to two machines'
   `lord-vm`s (`VM_LINK`, `--link-fd`). QEMU's sandbox doesn't change. Until now no machine could reach another except
   through a network.
4. **A change to the approved `machines` session channel**: a `.lordgroup` folder as `<dir>`.
5. **The Gateway's passt without DNS forwarding, and the members' memory without huge pages or KSM.** These take
   abilities away; they are listed so that the profile isn't loosened later without a review.
6. If chosen: the disposable overlay in memory (a second writable path in QEMU's Landlock). Chosen and built (G4) as a
   memfd handed to QEMU as a descriptor: no second writable path was needed.

### 19.13 Decisions for the maintainer

**Taken on 2026-11-23:** the newest verified release with a rollback floor (never older than a release already used);
Start Fresh resets the Workstation only by default, the Gateway optional with a warning about Tor's entry guards; the
anonymity settings locked with no override (clipboard, transfer and shared folders, the agents and virtio-serial, camera
and microphone) on both members; `lord-vm` starts `lord-vmunpack`; huge pages and KSM off for members; LXQt only;
Whonix-only groups for now (`group.json` generic enough to extend); the guest's resolution follows the window. The
firewall identity of the Gateway (TCP out only, never the LAN) is built (19.5.1).

The questions as they were asked:

- **Start Fresh's scope:** the Workstation only by default (recommended, for Tor's guards), or both members.
- **The release pin:** move it with each LordOS release, or take the newest verified folder with a rollback floor
  (recommended: a current Tor Browser after a Start Fresh).
- **The locks:** the anonymity rows locked with no override (recommended for the first release), or overridable with a
  warning, as Whonix's own documentation allows for the clipboard.
- **Who starts `lord-vmunpack`:** `lord-vm` (recommended: it does every other disk job of Machines) or lord-server (the
  usual starter of helpers).
- **Huge pages off**, at a cost of about 30 % in random memory access (recommended, Whonix's advice), or Machines'
  default.
- **The edition:** LXQt only (recommended), or a CLI choice too.
- **The firewall and logs:** whether the Gateway gets its own name in the firewall (today every machine is
  `lordos:machines`, area E), and whether `lord-vm`'s logs of group members are kept shorter.
- **A fixed guest resolution** (for example 1920 × 1080 whatever the window's size), or the window's size; Tor Browser
  letterboxes either way.
- **Generic groups:** Whonix only (recommended), or "New Group" for any machines later.
