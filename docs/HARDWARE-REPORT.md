# Hardware reports

What LordOS has been run on, from the maintainer's own machine, from the hardware reports people sent on GitHub
([issues labelled `hardware-report`](https://github.com/lordbasex/LordOS/issues?q=label%3Ahardware-report)) and from
the other GitHub issues, open and closed, that name the machine they ran on. It is a knowledge base: what works, what
was found and fixed, and which combinations nobody has reported yet. Every line comes from a report, its issue thread
or this repository. Last updated 2026-11-15, after reading every issue up to #159.

**In numbers**: 22 physical machines (the maintainer's PC, 9 hardware reports with the report file and 12 machines
named in other issues) from the maintainer and 19 other people, plus 4 reports from virtual machines; 23 people
reported in all. GPU: 14 Intel, 4 AMD, 0 NVIDIA, 4 not
given. Not one report says LordOS failed to reach the desktop on real hardware; the problems found are listed per
machine below, with what was fixed and who confirmed it.

A report is made in LordOS itself: System Information → Report → Share on GitHub saves a `lordos-hwreport-<date>.json`
file and opens GitHub's form; the person attaches the file and submits it with their own account (`docs/BENCHMARK.md`,
the FAQ "How do I share how LordOS runs on my computer?"). The hardware-report workflow (`tools/hwreport/validate.mjs`)
checks the file and labels the issue (`report:valid` or `report:missing`, `kind:*`, `gpu:*`, `cpu:*`, `boot:*`). A
report a maintainer accepts goes to `web/content/hardware/reports/` and from there to lordos.org/hardware and the signed
snapshot System Information's Compatibility panel reads (`tools/hwreport/catalog.mjs`); so far that is #42.

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

Nine reports from eight people, 2026-11-06 to 2026-11-15. The kind is the one the report detected from the firmware's
chassis type; where the firmware says "desktop" for a machine the reporter describes elsewhere as a tablet, the table
says "reported as desktop".

| Issue | Machine | Kind | CPU | GPU (driver) | Boot | LordOS | Result |
|---|---|---|---|---|---|---|---|
| [#42](https://github.com/lordbasex/LordOS/issues/42) | Dell Latitude 7480 | laptop | Intel Core i5-7300U, 4 threads | Intel HD Graphics 620 (i915) | internal | 0.8.8.4 | All works; power *limits* (the battery holds 48 % of its design capacity: wear, not LordOS). The touchpad problem ([#43](https://github.com/lordbasex/LordOS/issues/43)) was fixed in 0.8.8.6. Later, after `firmware-intel-graphics` reached the initrd, the reporter needed `i915.enable_dc=0` to stop hard lockups soon after login (DMC firmware); no separate issue yet. Accepted into the tested-hardware catalogue. |
| [#105](https://github.com/lordbasex/LordOS/issues/105) | ASUS K72F | laptop | Intel Pentium P6200, 2 threads | Intel Core Processor Integrated Graphics, Ironlake (i915) | internal | 0.8.8.7 | All works. Software problems in Files, SMB and archives were reported separately ([#101](https://github.com/lordbasex/LordOS/issues/101)–[#104](https://github.com/lordbasex/LordOS/issues/104)); #101, #103 and #104 are fixed, #102 (long file names) is open. |
| [#129](https://github.com/lordbasex/LordOS/issues/129) | HP EliteBook 840 G6 | laptop | Intel Core i5-8365U, 8 threads | Intel UHD Graphics 620, WhiskeyLake-U GT2 (i915) | internal | 0.8.8.7 | All works; power *limits*. Installed next to Windows 11. The same person's Bose QuietComfort 35 headset works on it ([#78](https://github.com/lordbasex/LordOS/issues/78), 0.8.8.7). |
| [#135](https://github.com/lordbasex/LordOS/issues/135) | TECLAST X98Pro | reported as desktop (the reporter calls it a tablet in [#131](https://github.com/lordbasex/LordOS/issues/131)) | Intel Atom x5-Z8500, 4 threads | Intel Atom x5-E8000/J3xxx/N3xxx integrated, Cherry Trail (i915) | eMMC | 0.8.8.7 | GPU *limits*: the 1536×2048 panel is rotated, and a rotated display is composed on the CPU. Everything else works. The tablet's power and volume buttons: [#131](https://github.com/lordbasex/LordOS/issues/131) (a fix in 0.8.8.9, not yet tried on the tablet). |
| [#136](https://github.com/lordbasex/LordOS/issues/136) | Apple MacBookPro11,4 (2015) | laptop | Intel Core i7-4770HQ, 8 threads | Intel Crystal Well integrated, Haswell (i915) | internal | 0.8.8.8 | All works. Includes a LordBench quick run. The same person's earlier issues on this MacBook: the battery percentage that stopped updating ([#25](https://github.com/lordbasex/LordOS/issues/25), fixed in 0.8.8.3), the volume, brightness and keyboard light keys (confirmed working on 0.8.8 in [#12](https://github.com/lordbasex/LordOS/issues/12)), the QuietComfort 35 headset (confirmed on 0.8.8.7 in #78) and an LG wide monitor with no screenshot offset ([#98](https://github.com/lordbasex/LordOS/issues/98)). |
| [#137](https://github.com/lordbasex/LordOS/issues/137) | Hampoo P02BD6_HI-122LP | reported as desktop (see #135) | Intel Atom x5-Z8350, 4 threads | Intel Atom x5-E8000/J3xxx/N3xxx integrated, Cherry Trail (i915) | eMMC | 0.8.8.7 | All works in the report, but sound needed the Cherry Trail audio driver installed by hand. The same reporter's Chuwi Hi12 threads (same CPU; the report does not say whether it is this machine) cover automatic rotation, fixed in 0.8.8.6 and confirmed working with the touchscreen, sound (`snd_soc_sst_bytcr_rt5651`) and charging (AXP288) in [#60](https://github.com/lordbasex/LordOS/issues/60), and the buttons in [#131](https://github.com/lordbasex/LordOS/issues/131). |
| [#149](https://github.com/lordbasex/LordOS/issues/149) | ASUS K84L | laptop | Intel Celeron B830, 2 threads | Intel 2nd Generation Core integrated, HD Graphics 2000, Sandy Bridge (i915) | internal | 0.8.8.7 | **Incomplete** (`report:missing`: the file's text was pasted instead of attached). From that text: GPU composition works; network *limits* (Ethernet not connected), no Wi-Fi adapter detected, power *limits* (worn battery). The same person's [#132](https://github.com/lordbasex/LordOS/issues/132) (open, titled "Asus X44H"; its log names the same K84L firmware model) shows no wireless card on the PCI bus and no wireless driver loaded: the card is missing or turned off below the operating system, not a missing driver. |
| [#155](https://github.com/lordbasex/LordOS/issues/155) | Apple MacBookPro12,1 (2015) | laptop | Intel Core i5-5257U, 4 threads | Intel Iris Graphics 6100, Broadwell (i915) | internal | 0.8.8.8 | All works. The FaceTime HD camera works with the out-of-tree `facetimehd` driver built by hand ([#21](https://github.com/lordbasex/LordOS/issues/21)); LordOS can't ship it, since its firmware may not be redistributed. |
| [#156](https://github.com/lordbasex/LordOS/issues/156) | Unnamed PC (no vendor in the firmware) | desktop | AMD Ryzen 7 5700X, 16 threads | AMD Radeon RX 9060 XT, Navi 44, RDNA 4 (amdgpu) | internal | 0.8.8.8 | All works; GPU composition on radeonsi (gfx1200), one HDMI monitor at 1920×1080, no battery. Includes LordBench (quick, 21 measurements). |

## Machines named in other issues

Twelve more machines appear in bug reports and requests that did not use the report form. They carry less data (no
compatibility rows), so they count in the comparison only for what the issue states; "not given" means the issue does
not say. Two other issues are about machines already above: [#132](https://github.com/lordbasex/LordOS/issues/132)
(the K84L of #149) and [#60](https://github.com/lordbasex/LordOS/issues/60) (the Chuwi Hi12 of #137's reporter).

| Issue | Machine | Kind | CPU | GPU (driver) | Install | LordOS | What happened |
|---|---|---|---|---|---|---|---|
| [#2](https://github.com/lordbasex/LordOS/issues/2) | Apple MacBook Pro (2012) | laptop | Intel, Ivy Bridge (model not given) | not given | live USB | 0.8.8 | The pointer kept jumping to the middle of the screen: the `applesmc` motion sensor reports absolute X and Y and was taken for a pointer. Fixed in 0.8.8; the reporter then wrote that everything works. |
| [#3](https://github.com/lordbasex/LordOS/issues/3), [#8](https://github.com/lordbasex/LordOS/issues/8), [#12](https://github.com/lordbasex/LordOS/issues/12), [#14](https://github.com/lordbasex/LordOS/issues/14), [#21](https://github.com/lordbasex/LordOS/issues/21), [#23](https://github.com/lordbasex/LordOS/issues/23), [#78](https://github.com/lordbasex/LordOS/issues/78) | Apple MacBookPro12,1 (early 2015; a second unit, not #155's) | laptop | Intel Core i5-5257U, Broadwell | Intel Iris Graphics 6100 (i915) | installed | 0.8.8 to 0.8.8.7 | Broadcom BCM43602 Wi-Fi works with `firmware-brcm80211`, on the ISO since 0.8.8 (#3); Wi-Fi turned off after each reboot, fixed in 0.8.8 and confirmed (#8); volume, brightness and keyboard light keys with the French Mac layout, added in 0.8.8 and confirmed (#12); Wi-Fi stuck after waking from sleep, handled since 0.8.8.3 by a check and reconnect on wake (#14); the camera works with `facetimehd` built by hand (#21); the QuietComfort 35 headset paired but stayed silent until 0.8.8.7, confirmed (#78). |
| [#40](https://github.com/lordbasex/LordOS/issues/40), [#57](https://github.com/lordbasex/LordOS/issues/57), [#94](https://github.com/lordbasex/LordOS/issues/94) | HP ZBook 17 G2 | laptop | Intel Core i7-4710MQ, 8 threads, Haswell | AMD FirePro W6150M / Radeon R9 M280X, Bonaire, GCN 2 (kernel `radeon`, Mesa radeonsi) | `install.sh` over Debian 13 | 0.8.8.4 to 0.8.8.6 | GPU composition. The pointer was squashed with coloured dots under it, the same cursor-plane size bug as the RX 580's; fixed in 0.8.8.6 and confirmed. The only report of the `radeon` kernel driver and of installing over an existing Debian. |
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

All 22 physical machines: the maintainer's PC, the nine reports with a file (#149 is incomplete but its data is
readable) and the twelve from other issues. A count of "not given" means the issue does not say. Virtual machines are
counted apart.

**By GPU vendor and model family**

| Vendor | Machines | Families (count) | What is known |
|---|---|---|---|
| Intel (i915) | 14 | Ironlake (1), Sandy Bridge HD 2000 (1), Haswell Crystal Well (1), Broadwell Iris 6100 (2), Kaby Lake HD 620 (4), Kaby Lake R and Whiskey Lake UHD 620 (2), Cherry Trail Atom (2), Gemini Lake UHD 600 (1) | GPU composition works on every one that says. The only GPU *limits* is a rotated tablet panel, which LordOS composes on the CPU by design (#135). One report of DMC firmware lockups on HD 620 with `firmware-intel-graphics`, worked around with `i915.enable_dc=0` (#42). |
| AMD | 4 | GCN 2, Bonaire (1, FirePro W6150M on the `radeon` kernel driver), GCN 4, Polaris (1, RX 580), RDNA 3 (1, RX 7800 XT), RDNA 4 (1, RX 9060 XT) | All compose on the GPU. The cursor-plane size fix (0.8.8.6) came from the RX 580 and also fixed the FirePro; the RX 580 brought the login-screen and display hand-off fixes. Open on the RX 7800 XT: uneven window drag at 120 Hz (#159). |
| NVIDIA | 0 | none | **Untested**: no report. Mesa's nouveau and NVK are included; the proprietary driver is not supported. |
| Not given | 4 | MacBook Pro 2012, HP 4K laptop, ThinkPad, tablet PC | Reached the desktop; the issues are about input, scale and displays. |

**By CPU**

| Vendor | Machines | Generations (count) |
|---|---|---|
| Intel | 17 | Westmere, Pentium P6200 (1); Sandy Bridge (1); Ivy Bridge (1); Haswell (2); Broadwell (2); Kaby Lake (4); Kaby Lake R (1); Whiskey Lake (1); Comet Lake, i9-10900 (1); Cherry Trail Atom (2); Gemini Lake Celeron (1) |
| AMD | 2 | Zen 3, Ryzen 7 5700X (1); Zen 4, Ryzen 7 7800X3D (1) |
| Not given | 3 | HP 4K laptop, ThinkPad, tablet PC |

All are x86-64. No ARM machine in the issues (the Raspberry Pi 5 is the project's own reference machine,
`docs/RASPBERRY.md`).

**By kind**

| Kind | Machines |
|---|---|
| Laptop | 14 (Apple 4, Dell 3, HP 3, ASUS 2, Lenovo 2) |
| Desktop | 3 (the maintainer's, #156, #159) |
| 2-in-1 | 1 (Dell Latitude 5289) |
| Tablet | 1 (#127, as the reporter says) |
| Reported as desktop | 2 (TECLAST X98Pro, Hampoo HI-122LP: Cherry Trail machines the reporter calls tablets in #131) |
| Not given | 1 (the ASUS with a Celeron N4000) |

**By install and boot medium**

| How | Machines |
|---|---|
| Installed, internal disk (from the report file) | 8 (seven reports and the maintainer's) |
| Installed, eMMC (from the report file) | 2 |
| Installed, medium not in the issue | 7 (one of them, the ZBook, with `install.sh` over an existing Debian 13) |
| Live USB | 1 (MacBook Pro 2012) |
| Not given | 4 |

**By LordOS version** (the latest version each machine was reported on)

| Version | Machines |
|---|---|
| 0.8.8.4 | 2 |
| 0.8.8 (as written in the issue) | 5 |
| 0.8.8.6 | 2 |
| 0.8.8.7 | 7 |
| 0.8.8.8 | 6 (the maintainer's among them) |

**What works**, each from at least one report: GPU composition on Intel graphics from 2010 (Ironlake) to 2019 (Whiskey
Lake) and on Cherry Trail and Gemini Lake, and on four AMD generations from GCN 2 to RDNA 4; three monitors with
DisplayPort (the RX 580), two monitors at 120 Hz (the RX 7800 XT), a laptop with an external monitor (three reports);
a 15.3" 4K laptop panel at 300 %; Alps I2C touchpads (since 0.8.8.6) and Apple trackpads; automatic rotation and the
touchscreen on Cherry Trail tablets; Wi-Fi on Broadcom BCM43602 and Intel 3165 and 8265; a Bluetooth headset on three
laptops; the volume, brightness and keyboard light keys on two MacBook Pros; a 2 GB Celeron N4000 laptop, smooth.

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
driver LordOS can't ship (#21).

**Untested combinations**: any NVIDIA card; AMD integrated graphics (APUs) on bare metal (a Radeon 780M only as a
virtual machine's host, #92); Intel Arc and Intel graphics newer than Whiskey Lake (Ice Lake, Tiger Lake, Alder Lake
and later); hybrid laptops with two GPUs; DisplayPort MST hubs; 4K and HiDPI external monitors; suspend and resume
(unchecked in every report file; only #14's Wi-Fi after waking is reported); ARM machines other than the reference
Raspberry Pi 5.

## Thanks

Thank you to everyone who took the time to report how LordOS runs on their machine, and to those who came back to
confirm that a fix works:

- [ootada](https://github.com/ootada): #42, #43
- [F-iatt](https://github.com/F-iatt): #105
- [pct06](https://github.com/pct06): #129 and the MacBook Pro threads #3, #8, #12, #14, #21, #23, #78; confirmed #8,
  #12 and #78
- [Touchgg](https://github.com/Touchgg): #135, #137, #60, #131; confirmed #60
- [wfxdesign](https://github.com/wfxdesign): #136, #25; confirmed #12 and #78 on a second MacBook Pro, tested #97 and
  #98 on it
- [brokochan](https://github.com/brokochan): #149, #132
- [MacNB](https://github.com/MacNB): #155, #21
- [toulzemaurice-blip](https://github.com/toulzemaurice-blip): #156
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
