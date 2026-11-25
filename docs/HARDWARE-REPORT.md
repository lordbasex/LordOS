# Hardware reports

What LordOS has been run on, from the maintainer's own machine, from the hardware reports people sent on GitHub
([issues labelled `hardware-report`](https://github.com/lordbasex/LordOS/issues?q=label%3Ahardware-report)) and from
the other GitHub issues, open and closed, that name the machine they ran on. It is a knowledge base: what works, what
was found and fixed, and which combinations nobody has reported yet. Every line comes from a report, its issue thread
or this repository. Last updated 2026-11-25, after reading every issue up to #189, the hardware reports #212, #221,
#224, #230, #245 and #249, and the graphics card report #235.

**In numbers**: 27 physical machines (the maintainer's PC, 13 machines with a hardware report's data, 12 machines
named in other issues and 1 graphics card report) from the maintainer and 24 other people, plus 4 reports from virtual
machines; 28 people reported in all. GPU: 16 Intel, 5 AMD, 2 NVIDIA, 4 not
given. Not one report says LordOS failed to reach the desktop on real hardware; the problems found are listed per
machine below, with what was fixed and who confirmed it.

A report is made in LordOS itself: System Information → Report → Share on GitHub saves a `lordos-hwreport-<date>.json`
file and opens GitHub's form; the person attaches the file and submits it with their own account (`docs/BENCHMARK.md`,
the FAQ "How do I share how LordOS runs on my computer?"). The hardware-report workflow (`tools/hwreport/validate.mjs`)
checks the file and labels the issue (`report:valid` or `report:missing`, `kind:*`, `gpu:*`, `cpu:*`, `boot:*`). A
report a maintainer accepts goes to `web/content/hardware/reports/` and from there to lordos.org/hardware and the signed
snapshot System Information's Compatibility panel reads (`tools/hwreport/catalog.mjs`); so far that is every
`report:valid` report: #42, #105, #129, #135, #136, #137, #155, #156, #212, #221, #224, #230, #245 and #249 (14
reports from 13 machines: #105, #212 and #230 are the same ASUS K72F on three versions), in the `lordos-hwdata`
2026.11.25 snapshot.

The report has no serial numbers, MAC or IP addresses, host, user or network names, or paths, and this page adds none.

How to read the result column of the reports: each report lists the state of `gpu`, `display`, `storage`, `network`,
`wifi`, `bluetooth`, `audio`, `power`, `isolation` and `suspend` as *works*, *limits* (works with a limit, with a
reason), *absent* (no such hardware) or *unchecked*. "All works" means every row present is *works*. **Suspend is
*unchecked* in every report so far**: nobody has sent a report made after a suspend and resume.

## The maintainer's machine

The project's author has used LordOS every day on this PC, installed on bare metal, since 2026-11-07. It has no
GitHub report issue; what it brought is in `CHANGELOG.md`, `docs/DISPLAYS.md`, `docs/GPU.md` and `docs/SESSIONS.md`.

| | |
|---|---|
| Machine | Desktop PC |
| CPU | Intel Core i9-10900 |
| GPU | AMD Radeon RX 580 (`amdgpu`), GPU composition; Mesa 25.0.7 (`radeonsi`, Vulkan through RADV) |
| Displays | Three 1080p monitors (Samsung S27F350) on DP-2, DP-3 and HDMI-A-1 |
| Boot | Internal disk, installed |
| LordOS | Each release since 0.8.8.5 (2026-11-07) |

What it found and what was fixed on it:

- **0.8.8.6**: the hardware cursor on amdgpu is created at the driver's `DRM_CAP_CURSOR_WIDTH/HEIGHT` (128 there)
  instead of 64×64, which the driver scanned as 128×128, squashing the arrow; the GPU path's framebuffers are removed
  only after the plain buffers are on screen, which ended the kernel's "atomic remove_fb failed" warnings inside a
  session; the login screen opens on the main display the person chose (with the first DisplayPort output dark, the
  password had to be typed blind; identical monitors can report the same EDID serial, so it is kept by connector).
- **0.8.8.6**: CPU microcode on amd64 (`intel-microcode`, `amd64-microcode`): the i9-10900 on a 2022 BIOS ran revision
  `0xec`, and the kernel reported MMIO Stale Data, SRBDS and GDS as "Vulnerable: no microcode".
- **0.8.8.6**: reading aloud (`say`, lord-tts) runs on the RX 580 through Vulkan: about 10 s of speech in 1.3 s on the
  GPU and 3 s on the CPU (`docs/VOICE.md`).
- **0.8.8.7**: displays keep their signal across lock, logout and login. lord-sessiond removed the leaving session's
  framebuffers while they were on screen; amdgpu refused ("atomic remove_fb failed with -22") and the displays went
  dark, and a DisplayPort monitor put to sleep that way might not wake until unplugged. Every lit display now gets a
  black frame of its own mode first, let go with CLOSEFB.
- **0.8.8.9** (test builds, 2026-11-15): Steam installed from the Store (Commercial and Games, with the engine's i386
  multiarch), and Lost Ark, a Windows game, runs through Proton. The first try showed Steam's processes on "Vulkan
  (Lavapipe)", the software renderer: Valve's `steam-libs-i386` only recommends `mesa-vulkan-drivers:i386`, so Steam's
  Store entry now brings the 32-bit Vulkan drivers with it (`docs/PACKAGES.md`, "Multiarch"). This is the only
  machine Steam from the Store has been tried on.

## Community reports with the report file

Thirteen machines from twelve people, in sixteen report issues, 2026-11-06 to 2026-11-25: #212 and #230 are later
reports of #105's laptop, on newer versions, and #224 brought the `.json` file #171 had left out (#171 attached the page System
Information opens instead; its data was the issue's summary and that page). The kind is the one the report detected from the firmware's
chassis type; where the firmware says "desktop" for a machine the reporter describes elsewhere as a tablet, the table
says "reported as desktop".

| Issue | Machine | Kind | CPU | GPU (driver) | Boot | LordOS | Result |
|---|---|---|---|---|---|---|---|
| [#42](https://github.com/lordbasex/LordOS/issues/42) | Dell Latitude 7480 | laptop | Intel Core i5-7300U, 4 threads | Intel HD Graphics 620 (i915) | internal | 0.8.8.4 | All works; power *limits* (the battery holds 48 % of its design capacity: wear, not LordOS). The touchpad problem ([#43](https://github.com/lordbasex/LordOS/issues/43)) was fixed in 0.8.8.6. Later, after `firmware-intel-graphics` reached the initrd, the reporter needed `i915.enable_dc=0` to stop hard lockups soon after login (DMC firmware); no separate issue yet. Accepted into the tested-hardware catalogue. |
| [#105](https://github.com/lordbasex/LordOS/issues/105) | ASUS K72F | laptop | Intel Pentium P6200, 2 threads | Intel Core Processor Integrated Graphics, Ironlake (i915) | internal | 0.8.8.7; 0.8.9.2 ([#212](https://github.com/lordbasex/LordOS/issues/212)) | All works. Software problems in Files, SMB and archives were reported separately ([#101](https://github.com/lordbasex/LordOS/issues/101)–[#104](https://github.com/lordbasex/LordOS/issues/104)); #101, #103 and #104 are fixed, #102 (long file names) is open. **0.8.9.2** (#212, 2026-11-20): all works again, GPU composition on Ironlake, now with an external Dell P2418D on HDMI next to the 1600×900 panel. The Dell runs at 1920×1200 rather than its native 2560×1440: this laptop's HDMI output stops at a 165 MHz TMDS clock, a limit of the hardware, not of LordOS. The issue's note about MuseScore's menus is an app problem, followed in [#209](https://github.com/lordbasex/LordOS/issues/209). **0.8.9.5-beta** ([#230](https://github.com/lordbasex/LordOS/issues/230), 2026-11-23, a fresh install with every update applied): all works again (GPU, display, storage, network, Wi-Fi, audio, power and isolation; no Bluetooth row; suspend unchecked). Installing MuseScore and Arduino is easier now, but their menus still don't work (#209). New: the reporter's other computers show up in Files on the network, but opening them doesn't work, while a Kubuntu session on the same machine reaches them at once; no separate issue yet. |
| [#129](https://github.com/lordbasex/LordOS/issues/129) | HP EliteBook 840 G6 | laptop | Intel Core i5-8365U, 8 threads | Intel UHD Graphics 620, WhiskeyLake-U GT2 (i915) | internal | 0.8.8.7 | All works; power *limits*. Installed next to Windows 11. The same person's Bose QuietComfort 35 headset works on it ([#78](https://github.com/lordbasex/LordOS/issues/78), 0.8.8.7). |
| [#135](https://github.com/lordbasex/LordOS/issues/135) | TECLAST X98Pro | reported as desktop (the reporter calls it a tablet in [#131](https://github.com/lordbasex/LordOS/issues/131)) | Intel Atom x5-Z8500, 4 threads | Intel Atom x5-E8000/J3xxx/N3xxx integrated, Cherry Trail (i915) | eMMC | 0.8.8.7 | GPU *limits*: the 1536×2048 panel is rotated, and a rotated display is composed on the CPU. Everything else works. The tablet's power and volume buttons: [#131](https://github.com/lordbasex/LordOS/issues/131) (a fix in 0.8.8.9, not yet tried on the tablet). |
| [#136](https://github.com/lordbasex/LordOS/issues/136) | Apple MacBookPro11,4 (2015) | laptop | Intel Core i7-4770HQ, 8 threads | Intel Crystal Well integrated, Haswell (i915) | internal | 0.8.8.8 | All works. Includes a LordBench quick run. The same person's earlier issues on this MacBook: the battery percentage that stopped updating ([#25](https://github.com/lordbasex/LordOS/issues/25), fixed in 0.8.8.3), the volume, brightness and keyboard light keys (confirmed working on 0.8.8 in [#12](https://github.com/lordbasex/LordOS/issues/12)), the QuietComfort 35 headset (confirmed on 0.8.8.7 in #78) and an LG wide monitor with no screenshot offset ([#98](https://github.com/lordbasex/LordOS/issues/98)). |
| [#137](https://github.com/lordbasex/LordOS/issues/137) | Hampoo P02BD6_HI-122LP | reported as desktop (see #135) | Intel Atom x5-Z8350, 4 threads | Intel Atom x5-E8000/J3xxx/N3xxx integrated, Cherry Trail (i915) | eMMC | 0.8.8.7 | All works in the report, but sound needed the Cherry Trail audio driver installed by hand. The same reporter's Chuwi Hi12 threads (same CPU; the report does not say whether it is this machine) cover automatic rotation, fixed in 0.8.8.6 and confirmed working with the touchscreen, sound (`snd_soc_sst_bytcr_rt5651`) and charging (AXP288) in [#60](https://github.com/lordbasex/LordOS/issues/60), and the buttons in [#131](https://github.com/lordbasex/LordOS/issues/131). |
| [#149](https://github.com/lordbasex/LordOS/issues/149) | ASUS K84L | laptop | Intel Celeron B830, 2 threads | Intel 2nd Generation Core integrated, HD Graphics 2000, Sandy Bridge (i915) | internal | 0.8.8.7 | **Incomplete** (`report:missing`: the file's text was pasted instead of attached). From that text: GPU composition works; network *limits* (Ethernet not connected), no Wi-Fi adapter detected, power *limits* (worn battery). The same person's [#132](https://github.com/lordbasex/LordOS/issues/132) (open, titled "Asus X44H"; its log names the same K84L firmware model) shows no wireless card on the PCI bus and no wireless driver loaded: the card is missing or turned off below the operating system, not a missing driver. |
| [#155](https://github.com/lordbasex/LordOS/issues/155) | Apple MacBookPro12,1 (2015) | laptop | Intel Core i5-5257U, 4 threads | Intel Iris Graphics 6100, Broadwell (i915) | internal | 0.8.8.8 | All works. The FaceTime HD camera works with the out-of-tree `facetimehd` driver built by hand ([#21](https://github.com/lordbasex/LordOS/issues/21)); LordOS can't ship it, since its firmware may not be redistributed. |
| [#156](https://github.com/lordbasex/LordOS/issues/156) | Unnamed PC (no vendor in the firmware) | desktop | AMD Ryzen 7 5700X, 16 threads | AMD Radeon RX 9060 XT, Navi 44, RDNA 4 (amdgpu) | internal | 0.8.8.8 | All works; GPU composition on radeonsi (gfx1200), one HDMI monitor at 1920×1080, no battery. Includes LordBench (quick, 21 measurements). |
| [#171](https://github.com/lordbasex/LordOS/issues/171) | Gigabyte B760M C | desktop | Intel Core i9-12900F, 24 threads, Alder Lake | NVIDIA GeForce RTX 3060, GA106 (nvidia, the proprietary driver) | installed on a USB drive (480 Mbps) | 0.8.8.9 | GPU *limits* (the proprietary driver, which LordOS doesn't support for GPU composition); display, storage, network, Wi-Fi, Bluetooth, audio and isolation work; suspend unchecked. The first report with an NVIDIA card. The `.json` file itself arrived later, in [#224](https://github.com/lordbasex/LordOS/issues/224) (2026-11-22): the issue's form says 0.8.9.4, but the file is still the 0.8.8.9 capture of 2026-11-16, and it matches this row. |
| [#221](https://github.com/lordbasex/LordOS/issues/221) | ASUS F1A55-M LX (motherboard; no system vendor in the firmware) | desktop | AMD A4-3400 APU, 2 threads, K10 Llano | AMD Radeon HD 5450, Cedar, TeraScale 2 (kernel `radeon`, Mesa r600); only this card shows in the report | internal, a mechanical hard disk; legacy BIOS boot | 0.8.9.4 | All works: GPU composition on r600, one HDMI monitor at 1920×1080, Ethernet, audio and isolation; no Wi-Fi, Bluetooth or battery; suspend unchecked. The reporter finds it very fast and stable on this hardware. The first TeraScale 2 card and the first Llano APU. |
| [#245](https://github.com/lordbasex/LordOS/issues/245) | Acer Aspire R3-131T | laptop | Intel Celeron N3050, 2 threads, Braswell | Intel Atom/Celeron/Pentium x5-E8000/J3xxx/N3xxx integrated, Braswell (i915) | eMMC | 0.8.9.5-beta | All works: GPU, display, storage, network, Wi-Fi, Bluetooth, audio, power and isolation; suspend unchecked. 2 GB of memory: five pages open in Brave (YouTube and Google Maps among them) used about 1.3 GB. Includes LordBench (quick, 19 measurements). Open: the Fn key with the brightness keys does nothing; no separate issue yet. The first Braswell machine and the first Acer. |
| [#249](https://github.com/lordbasex/LordOS/issues/249) | HUAWEI CREFG-XX | laptop | Intel Core i9-13900H, 20 threads, Raptor Lake | Intel Iris Xe Graphics, Raptor Lake-P (i915) | internal, NVMe | 0.8.9.6-beta | GPU composition on Mesa's Iris driver, a 2520×1680 BOE panel (16.2"), Intel CNVi Wi-Fi, Bluetooth, power (the battery holds 85 % of its design capacity) and isolation work, and every row of the report says *works* (suspend unchecked); the reporter says everything else works perfectly. Open: **no sound** from the laptop, although the sound card is found (Intel's Sound Open Firmware driver, `sof-hda-dsp`) and the report's audio row says *works*: that row only says a card is there, not that the speakers play; no separate issue yet. The fingerprint reader doesn't work: LordOS has no fingerprint sign-in, and the reader is not among the report's USB devices. The same person's earlier [#179](https://github.com/lordbasex/LordOS/issues/179) (0.8.8.9) arrived empty. The first Raptor Lake machine, the first Intel graphics newer than Whiskey Lake and the first HUAWEI. |

## Graphics card reports

Since 0.8.9.5 every computer can send a graphics card report (Settings → Graphics cards → Send a compatibility report,
or `lordos-issue --gpu`; `docs/NVIDIA-PLAN.md`, Part 0). It names the cards and drivers but not the machine, so a
machine that only sent this report counts in the comparison for its card alone. Since 0.8.9.6.1 (in development)
`lordos-issue --gpu` (not Settings: lord-server doesn't probe cards for it) also gives each card's display engine, read
without root through the kernel's DRM interface on the nodes the user may open (`server/src/sysreport/drmprobe.c`):
explicit sync (`DRM_CAP_SYNCOBJ`, `DRM_CAP_SYNCOBJ_TIMELINE`), atomic modesetting, the planes by type, the primary
plane's scanout formats and modifiers (`IN_FORMATS`), framebuffer modifiers, PRIME, and whether a VR headset could be
leased (connectors marked `non-desktop`). `lordos-sysreport report` has the same as a "Display engine" table and a
`drm` object per GPU in its JSON.

| Issue | Card | Driver | LordOS | Result |
|---|---|---|---|---|
| [#235](https://github.com/lordbasex/LordOS/issues/235) | NVIDIA GeForce RTX 5070 Ti 16 GB, GB203, Blackwell (`10de:2c05`), the only card, the boot display | `nvidia` 620.73.04, NVIDIA's open kernel modules; `nvidia-drm` modeset and fbdev on; Mesa 25.0.7; kernel 7.3.4+deb13; UEFI, Secure Boot off | 0.8.9.5-beta | The desktop works, smooth, with no flicker, tearing or glitches. The report was made from the command line, so it doesn't say whether the desktop was composed on the GPU or the CPU. CUDA's userspace is installed (`libcuda`, `nvidia-smi`): llama.cpp loaded a 4.48 GiB model, and a 131072-token context, which needs 9.01 GiB more, didn't fit in the 5.99 GiB of video memory left, while 65536 did; that is the card's free memory, not a LordOS fault. The report also shows the `nouveau` kernel module loaded next to `nvidia`, though the card is bound to `nvidia` (see "Known problems"). The first RTX 50 card and the first report on NVIDIA's open kernel modules. |

## Machines named in other issues

Twelve more machines appear in bug reports and requests that did not use the report form. They carry less data (no
compatibility rows), so they count in the comparison only for what the issue states; "not given" means the issue does
not say. Two other issues are about machines already above: [#132](https://github.com/lordbasex/LordOS/issues/132)
(the K84L of #149) and [#60](https://github.com/lordbasex/LordOS/issues/60) (the Chuwi Hi12 of #137's reporter).

| Issue | Machine | Kind | CPU | GPU (driver) | Install | LordOS | What happened |
|---|---|---|---|---|---|---|---|
| [#2](https://github.com/lordbasex/LordOS/issues/2) | Apple MacBook Pro (2012) | laptop | Intel, Ivy Bridge (model not given) | not given | live USB | 0.8.8 | The pointer kept jumping to the middle of the screen: the `applesmc` motion sensor reports absolute X and Y and was taken for a pointer. Fixed in 0.8.8; the reporter then wrote that everything works. |
| [#3](https://github.com/lordbasex/LordOS/issues/3), [#8](https://github.com/lordbasex/LordOS/issues/8), [#12](https://github.com/lordbasex/LordOS/issues/12), [#14](https://github.com/lordbasex/LordOS/issues/14), [#21](https://github.com/lordbasex/LordOS/issues/21), [#23](https://github.com/lordbasex/LordOS/issues/23), [#78](https://github.com/lordbasex/LordOS/issues/78) | Apple MacBookPro12,1 (early 2015; a second unit, not #155's) | laptop | Intel Core i5-5257U, Broadwell | Intel Iris Graphics 6100 (i915) | installed | 0.8.8 to 0.8.8.7 | Broadcom BCM43602 Wi-Fi works with `firmware-brcm80211`, on the ISO since 0.8.8 (#3); Wi-Fi turned off after each reboot, fixed in 0.8.8 and confirmed (#8); volume, brightness and keyboard light keys with the French Mac layout, added in 0.8.8 and confirmed (#12); Wi-Fi stuck after waking from sleep, handled since 0.8.8.3 by a check and reconnect on wake (#14); the camera works with `facetimehd` built by hand (#21); the QuietComfort 35 headset paired but stayed silent until 0.8.8.7, confirmed (#78). |
| [#40](https://github.com/lordbasex/LordOS/issues/40), [#57](https://github.com/lordbasex/LordOS/issues/57), [#94](https://github.com/lordbasex/LordOS/issues/94) | HP ZBook 17 G2 | laptop | Intel Core i7-4710MQ, 8 threads, Haswell | AMD FirePro W6150M / Radeon R9 M280X, Bonaire, GCN 2 (kernel `radeon`, Mesa radeonsi) | `install.sh` over Debian 13 | 0.8.8.4 to 0.8.8.6 | GPU composition. The pointer was squashed with coloured dots under it, the same cursor-plane size bug as the RX 580's; fixed in 0.8.8.6 and confirmed. One of the two machines on the `radeon` kernel driver (with #221) and the only report of installing over an existing Debian. |
| [#67](https://github.com/lordbasex/LordOS/issues/67) | Dell Latitude 7480 (a second unit, not #42's) | laptop | Intel 7th generation Core (model not given) | Intel HD Graphics 620 (i915, as in #42) | installed | 0.8.8.5 to 0.8.8.6 | The Alps I2C touchpad ignored one-finger motion and taps (its driver reports pressures above its own range, read as a palm), and the Caps Lock light never lit. Both fixed in 0.8.8.6 and confirmed. |
| [#54](https://github.com/lordbasex/LordOS/issues/54) | Dell Latitude 5289 | 2-in-1 | Intel Core i7-7600U, Kaby Lake | Intel HD Graphics 620 (i915) | installed | 0.8.8.4 | Same Alps touchpad problem as #67, fixed in 0.8.8.6; the reporter has not confirmed. Its Wacom touchscreen and a USB keyboard and mouse were detected. |
| [#47](https://github.com/lordbasex/LordOS/issues/47) | HP laptop with a 15.3" 3840×2160 panel (model not given) | laptop | not given | not given | not given | 0.8.8 | 200 % was the largest scale. Up to 300 % since 0.8.8.6, with the pointer following the scale; the reporter had already run 300 % with a rebuilt `lord-server`. The only 4K panel reported. |
| [#97](https://github.com/lordbasex/LordOS/issues/97), [#98](https://github.com/lordbasex/LordOS/issues/98) | Lenovo ThinkPad (model not given) with an external DisplayPort monitor | laptop | not given | not given | not given | 0.8.8 to 0.8.8.8 | Closing the lid left the internal panel on; since 0.8.8.8 it turns off and its windows move to the external display, confirmed (#97). The screenshot area selection is offset across the two displays (#98, open; not seen on #136's MacBook with one external monitor). |
| [#106](https://github.com/lordbasex/LordOS/issues/106) | Lenovo ideapad 320-15IKB (80YH) with a 24" 1920×1080 monitor | laptop | Intel Core i7-7500U, 4 threads, Kaby Lake | Intel HD Graphics 620 (i915) | installed | 0.8.8 | The Intel Wireless-AC 3165 had its driver loaded but was blocked; the laptop's wireless key turned it on, and then it worked. Settings → Network's switch can't lift a hardware block. |
| [#127](https://github.com/lordbasex/LordOS/issues/127) | Tablet PC (model not given) | tablet | not given | not given | not given | 0.8.8.7 | The on-screen keyboard can't type into the administrator password prompt. That is on purpose: the prompt only takes keys from a physical keyboard, so no program in the session can type the password. |
| [#153](https://github.com/lordbasex/LordOS/issues/153) | ASUS (model not given), 2 GB of memory | not given | Intel Celeron N4000, Gemini Lake | Intel UHD Graphics 600 (i915) | installed | 0.8.8 | Smooth on a low-end machine, about 400 MB of memory in use when idle, PPSSPP without tearing. Open: the Caps Lock light stays off (LordOS writes the keyboard lights since 0.8.8.6; the exact version and a log are missing), and there is no on-screen indicator when the volume keys change the volume. |
| [#157](https://github.com/lordbasex/LordOS/issues/157) | Dell Latitude 5590 | laptop | Intel Core i7-8650U, 8 threads, Kaby Lake R | Intel UHD Graphics 620 (i915) | installed | 0.8.8.8 | Steam no longer opens after the upgrade (open; a software problem, the system report itself is clean). |
| [#159](https://github.com/lordbasex/LordOS/issues/159) | Desktop PC, two monitors (2560×1440 and 1920×1080, both 120 Hz) | desktop (the reporter's main PC) | AMD Ryzen 7 7800X3D | AMD Radeon RX 7800 XT, RDNA 3 (amdgpu) | not given | 0.8.8 | Runs as a daily test machine. Open: dragging a window is sometimes less smooth than 120 Hz and its text blurry, then recovers by itself; the Logitech MX Master 2S's back and forward buttons did nothing (lord-server passed only the left, middle and right buttons; fixed in 0.8.8.9, untested on the reporter's mouse); AppImages start from a terminal but not from Files (Files no longer opens them in TextEdit); some GTK4 dropdowns freeze on native Wayland and work under Xwayland; a polkit agent fails with "No session for pid". |

### Virtual machines

| Issue | Hypervisor | LordOS | What happened |
|---|---|---|---|
| [#9](https://github.com/lordbasex/LordOS/issues/9) | VirtualBox 7.2 | 0.8.7 to 0.8.8 | The desktop never appeared with VirtualBox's default VMSVGA adapter. Since 0.8.8, VMSVGA, VBoxSVGA and VBoxVGA reach the desktop with BIOS and EFI (tested by the maintainer), and a ready-made OVA is published. |
| [#92](https://github.com/lordbasex/LordOS/issues/92) | VirtualBox 7.2 on an AMD Ryzen 7 7840HS host | 0.8.8 | The Guest Additions CD was not shown: optical drives were left out. Since 0.8.8.7 an inserted disc is announced and opens read-only in Files. |
| [#76](https://github.com/lordbasex/LordOS/issues/76) | VMware | 0.8.8 | Copy and paste between guest and host: VMware's agent is an X11-only GTK program; 0.8.8.7 lets it start under Xwayland, which covers part of it. Open. |
| [#140](https://github.com/lordbasex/LordOS/issues/140) | QEMU/KVM (libvirt, virtio-vga) | development | `systemctl restart lordos` fails with systemd 258 or later while sessions are alive; the reporter reproduced it on bare metal too. Open. |

## Comparison

All 27 physical machines: the maintainer's PC, the thirteen with a report's data (#149 is incomplete but its data is
readable), the twelve from other issues and the graphics card report #235 (its card only). A count of "not given" means the issue does not say. Virtual machines are
counted apart.

**By GPU vendor and model family**

| Vendor | Machines | Families (count) | What is known |
|---|---|---|---|
| Intel (i915) | 16 | Ironlake (1), Sandy Bridge HD 2000 (1), Haswell Crystal Well (1), Broadwell Iris 6100 (2), Kaby Lake HD 620 (4), Kaby Lake R and Whiskey Lake UHD 620 (2), Raptor Lake-P Iris Xe (1, #249), Cherry Trail Atom (2), Braswell Celeron (1, #245), Gemini Lake UHD 600 (1) | GPU composition works on every one that says. The only GPU *limits* is a rotated tablet panel, which LordOS composes on the CPU by design (#135). One report of DMC firmware lockups on HD 620 with `firmware-intel-graphics`, worked around with `i915.enable_dc=0` (#42). |
| AMD | 5 | TeraScale 2, Cedar (1, Radeon HD 5450 on the `radeon` kernel driver and Mesa r600, #221), GCN 2, Bonaire (1, FirePro W6150M on the `radeon` kernel driver), GCN 4, Polaris (1, RX 580), RDNA 3 (1, RX 7800 XT), RDNA 4 (1, RX 9060 XT) | All compose on the GPU. The cursor-plane size fix (0.8.8.6) came from the RX 580 and also fixed the FirePro; the RX 580 brought the login-screen and display hand-off fixes. Open on the RX 7800 XT: uneven window drag at 120 Hz (#159). |
| NVIDIA | 2 | Ampere, RTX 3060 (1, #171, the proprietary driver); Blackwell, RTX 5070 Ti (1, #235, NVIDIA's open kernel modules 620.73.04) | Both reach the desktop on NVIDIA's own driver. #171's GPU is *limits* (LordOS doesn't support the proprietary driver for GPU composition); #235's desktop is smooth, and its report doesn't say how it was composed. Mesa's nouveau and NVK are included and untested. |
| Not given | 4 | MacBook Pro 2012, HP 4K laptop, ThinkPad, tablet PC | Reached the desktop; the issues are about input, scale and displays. |

**By CPU**

| Vendor | Machines | Generations (count) |
|---|---|---|
| Intel | 20 | Westmere, Pentium P6200 (1); Sandy Bridge (1); Ivy Bridge (1); Haswell (2); Broadwell (2); Kaby Lake (4); Kaby Lake R (1); Whiskey Lake (1); Comet Lake, i9-10900 (1); Alder Lake, i9-12900F (1); Raptor Lake, i9-13900H (1); Cherry Trail Atom (2); Braswell Celeron N3050 (1); Gemini Lake Celeron (1) |
| AMD | 3 | K10 Llano, A4-3400 APU (1); Zen 3, Ryzen 7 5700X (1); Zen 4, Ryzen 7 7800X3D (1) |
| Not given | 4 | HP 4K laptop, ThinkPad, tablet PC, #235's computer (a graphics card report) |

All are x86-64. No ARM machine in the issues (the Raspberry Pi 5 is the project's own reference machine,
`docs/RASPBERRY.md`).

**By kind**

| Kind | Machines |
|---|---|
| Laptop | 16 (Apple 4, Dell 3, HP 3, ASUS 2, Lenovo 2, Acer 1, HUAWEI 1) |
| Desktop | 5 (the maintainer's, #156, #159, #171, #221) |
| 2-in-1 | 1 (Dell Latitude 5289) |
| Tablet | 1 (#127, as the reporter says) |
| Reported as desktop | 2 (TECLAST X98Pro, Hampoo HI-122LP: Cherry Trail machines the reporter calls tablets in #131) |
| Not given | 2 (the ASUS with a Celeron N4000, #235's computer) |

**By install and boot medium**

| How | Machines |
|---|---|
| Installed, internal disk (from the report file) | 10 (nine reports and the maintainer's; #221 on a mechanical disk) |
| Installed, eMMC (from the report file) | 3 (#135, #137, #245) |
| Installed, USB drive (from the report data) | 1 (#171) |
| Installed, medium not in the issue | 7 (one of them, the ZBook, with `install.sh` over an existing Debian 13) |
| Live USB | 1 (MacBook Pro 2012) |
| Not given | 5 |

**By LordOS version** (the latest version each machine was reported on)

| Version | Machines |
|---|---|
| 0.8.8.4 | 2 |
| 0.8.8 (as written in the issue) | 5 |
| 0.8.8.6 | 2 |
| 0.8.8.7 | 6 |
| 0.8.8.8 | 6 (the maintainer's among them) |
| 0.8.8.9 | 1 (#171; #224's file is the same 0.8.8.9 capture) |
| 0.8.9.4 | 1 (#221) |
| 0.8.9.5-beta | 3 (#105's laptop, in #230 after #212 on 0.8.9.2; #235; #245) |
| 0.8.9.6-beta | 1 (#249) |

**What works**, each from at least one report: GPU composition on Intel graphics from 2010 (Ironlake) to 2019 (Whiskey
Lake), on 2023's Raptor Lake Iris Xe (#249) and on Cherry Trail, Braswell and Gemini Lake, and on five AMD generations from TeraScale 2 (a Radeon HD 5450 on `radeon`
and r600, #221) to RDNA 4; three monitors with DisplayPort (the RX 580), two monitors at 120 Hz (the RX 7800 XT), a
laptop with an external monitor (four reports, the latest the K72F's Dell on HDMI, #212); a 2011 Llano desktop with a
mechanical disk and legacy BIOS boot, fast and stable (#221);
a 15.3" 4K laptop panel at 300 %; Alps I2C touchpads (since 0.8.8.6) and Apple trackpads; automatic rotation and the
touchscreen on Cherry Trail tablets; Wi-Fi on Broadcom BCM43602, Intel 3165 and 8265 and Raptor Lake's CNVi (#249); a Bluetooth headset on three
laptops; the volume, brightness and keyboard light keys on two MacBook Pros; a 2 GB Celeron N4000 laptop, smooth; a
2 GB Celeron N3050 laptop booting from eMMC with every component working (#245); an RTX 5070 Ti on NVIDIA's open
kernel modules, smooth (#235).

**Fixed and confirmed by the person who reported it**: #2 (MacBook Pro 2012 pointer), #8 (Wi-Fi after reboot), #12
(media keys, by two people), #57 (squashed pointer on the FirePro), #60 (rotation on the Chuwi Hi12), #67 (Alps
touchpad and Caps Lock light), #78 (Bluetooth headset, on three machines), #97 (lid with an external monitor), #106
(Wi-Fi, after the wireless key).

**Known problems, open**: Cherry Trail tablets' audio and charging, which need a driver or a fix by hand, and their
buttons (#60, #131; the volume buttons and a power-button hold have a fix in 0.8.8.9, not yet tried on a tablet); DMC lockups on one HD 620 laptop after an Intel firmware update (#42, workaround); no wireless
card visible on the ASUS K84L (#132, below the operating system); the screenshot selection offset on a ThinkPad with
two displays (#98); the Caps Lock light and a volume indicator on an ASUS Celeron N4000 (#153); window drag
smoothness at 120 Hz, AppImages from Files, GTK4 dropdowns on native Wayland and polkit agents on a Ryzen and RX 7800 XT
desktop (#159; its mouse back and forward buttons have a fix in 0.8.8.9, not yet confirmed there); Steam on a Latitude 5590 (#157); the FaceTime HD camera, which needs a
driver LordOS can't ship (#21); the Fn brightness keys on the Acer Aspire R3-131T (#245); network computers shown in
Files but not opening on the ASUS K72F, which a Kubuntu session on the same machine reaches (#230), and menus in
MuseScore and Arduino there (#209); the `nouveau` module loaded next to NVIDIA's driver on the RTX 5070 Ti (#235: not
bound to the card, which is on `nvidia` and works, but NVIDIA's packages should keep `nouveau` from loading at all;
whether their blacklist reached that kernel's initramfs is to ask, `docs/NVIDIA-PLAN.md`, A.4); no sound from the
HUAWEI CREFG-XX, whose Sound Open Firmware card is found (#249; no separate issue yet), and its fingerprint reader,
which LordOS doesn't use for sign-in (#249).

**Untested combinations**: NVIDIA cards other than an RTX 3060 on the proprietary driver (#171) and an RTX 5070 Ti on
the open kernel modules (#235), and nouveau or NVK on any; AMD integrated graphics (APUs) on bare metal (a Radeon 780M only as a
virtual machine's host, #92; #221's A4-3400 APU draws through its HD 5450 card, and only the card shows in the report); Intel Arc, Intel graphics between Whiskey Lake and Raptor Lake (Ice Lake, Tiger Lake, Alder Lake;
#171's Alder Lake has no integrated GPU) and newer than Raptor Lake (#249); hybrid laptops with two GPUs; DisplayPort MST hubs; 4K and HiDPI external monitors; suspend and resume
(unchecked in every report file; only #14's Wi-Fi after waking is reported); ARM machines other than the reference
Raspberry Pi 5.

## Toolkits (the optional "toolkits" section, in development)

Since the toolkit audit (docs/TOOLKIT-AUDIT.md), a report can also say how GTK 3, GTK 4, Qt 5, Qt 6 and a plain X11
client did on that machine. It is optional and off by default: System Information → Report → Share on GitHub →
**Include framework compatibility** uses `lordos-testbench --toolkits`' result from the last day, or opens the audit
in a Terminal window and waits for a new one. No report so far has it.

- **What it carries** (`apps/sysinfo/hwreport.schema.json`, `$defs/toolkits`): `v` 1, the day it ran, the global
  score, how many combinations ran, and per combination (at most 9) its id, status, fatal word, toolkit version,
  the backend it saw, score, points, total, skipped count, milliseconds, the failed and skipped check ids, the
  protocol counts (globals bound and failed, binds outside the table, errors, unattributed errors) and the
  recommended variable (`GDK_BACKEND=x11` or `QT_QPA_PLATFORM=xcb`). An optional `detail` may hold at most 80
  printable ASCII characters without `/`; the audit itself never writes one.
- **How it is built** (`apps/sysinfo/toolkits.js`): from the saved file, keeping only known ids and numbers, then
  picked through the schema like the rest of the report. A hand-edited saved file can't put text into a report.
- **The report stays format 1.** Without the tick the key is absent, so such a report is exactly what older
  checkers know.
- **The workflow** (`tools/hwreport/lib.mjs`): the schema's lists (9 combinations, 6 statuses, the fatal words,
  the 14 shared check ids and `global.<interface>` for the 48 globals of the protocol table), the privacy scan over
  every string, two plausibility flags (`toolkits-date`, a day in the future; `toolkit-score`, a score that isn't
  its points), a "Toolkits" row in the comment, and the labels `toolkit:<combination>-fail` for a combination that
  crashed or scored under 80 % (docs/RELEASING.md lists all 42 labels).
- **lordos.org/hardware** shows each machine's scores under its card when its report has them.
- **The signed snapshot** keeps `lordos-hwsnapshot` version 1. `server/src/sysreport/hwsnap.c` accepts an optional
  `toolkits` field (per combination and LordOS version: reports, failures, mean score), checked as strictly as the
  devices, and `apps/sysinfo/community.js` ignores it. `tools/hwreport/catalog.mjs` writes it only with
  `--toolkits`: the readers of 0.8.9.6.1 and older accept exactly five fields and `lordos-hwdata` reaches them
  between releases, so the default snapshot stays in that shape (`build/hwsnap-test` and
  `tests/hwreport_test.mjs` check it against the old reader's rule). Turn it on once no supported release reads
  the snapshot with the old check.

## Thanks

Thank you to everyone who took the time to report how LordOS runs on their machine, and to those who came back to
confirm that a fix works:

- [ootada](https://github.com/ootada): #42, #43
- [F-iatt](https://github.com/F-iatt): #105, #212, #230
- [pct06](https://github.com/pct06): #129 and the MacBook Pro threads #3, #8, #12, #14, #21, #23, #78; confirmed #8,
  #12 and #78
- [Touchgg](https://github.com/Touchgg): #135, #137, #60, #131; confirmed #60
- [wfxdesign](https://github.com/wfxdesign): #136, #25; confirmed #12 and #78 on a second MacBook Pro, tested #97 and
  #98 on it
- [brokochan](https://github.com/brokochan): #149, #132
- [MacNB](https://github.com/MacNB): #155, #21
- [toulzemaurice-blip](https://github.com/toulzemaurice-blip): #156
- [Diegossa2](https://github.com/Diegossa2): #171, #224
- [javiernocito-tech](https://github.com/javiernocito-tech): #221
- [kessbarrasa](https://github.com/kessbarrasa): #245
- [altikardes](https://github.com/altikardes): #249, #179
- [chevro](https://github.com/chevro): #235
- [Kazukighost](https://github.com/Kazukighost): #2; confirmed
- [pccasto](https://github.com/pccasto): #40, #57, #94; confirmed #57
- [rengesh2000](https://github.com/rengesh2000): #67; confirmed
- [pierluigi65](https://github.com/pierluigi65): #54
- [bilind](https://github.com/bilind): #47
- [edwardmfho](https://github.com/edwardmfho): #97, #98; confirmed #97
- [sousair185](https://github.com/sousair185): #106; confirmed
- [ikmayonimuhammad](https://github.com/ikmayonimuhammad): #127
- [Gemasyaputra](https://github.com/Gemasyaputra): #153
- [JrocMD](https://github.com/JrocMD): #157
- [getsugah](https://github.com/getsugah): #159
- [axaratechnologies-cmd](https://github.com/axaratechnologies-cmd): #9
- [unikitty37](https://github.com/unikitty37): #92
- [ket000](https://github.com/ket000): #76
- [Kiisanz](https://github.com/Kiisanz): #140
