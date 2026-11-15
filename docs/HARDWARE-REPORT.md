# Hardware reports

What LordOS has been run on, from the maintainer's own machine and from the hardware reports people sent on GitHub
([issues labelled `hardware-report`](https://github.com/lordbasex/LordOS/issues?q=label%3Ahardware-report)). It is a
knowledge base: what works, what was found and fixed, and which combinations nobody has reported yet. Every line comes
from a report, its issue thread or this repository. Last updated 2026-11-15 (9 community reports).

A report is made in LordOS itself: System Information → Report → Share on GitHub saves a `lordos-hwreport-<date>.json`
file and opens GitHub's form; the person attaches the file and submits it with their own account (`docs/BENCHMARK.md`,
the FAQ "How do I share how LordOS runs on my computer?"). The hardware-report workflow (`tools/hwreport/validate.mjs`)
checks the file and labels the issue (`report:valid` or `report:missing`, `kind:*`, `gpu:*`, `cpu:*`, `boot:*`). A
report a maintainer accepts goes to `web/content/hardware/reports/` and from there to lordos.org/hardware and the signed
snapshot System Information's Compatibility panel reads (`tools/hwreport/catalog.mjs`); so far that is #42.

The report has no serial numbers, MAC or IP addresses, host, user or network names, or paths, and this page adds none.

How to read the result column: each report lists the state of `gpu`, `display`, `storage`, `network`, `wifi`,
`bluetooth`, `audio`, `power`, `isolation` and `suspend` as *works*, *limits* (works with a limit, with a reason),
*absent* (no such hardware) or *unchecked*. "All works" means every row present is *works*. **Suspend is *unchecked*
in every report so far**: nobody has sent a report made after a suspend and resume.

## The maintainer's machine

The project's author has used LordOS every day on this PC, installed on bare metal, since 2026-11-07. It has no
GitHub report issue; what it brought is in `CHANGELOG.md`, `docs/DISPLAYS.md`, `docs/GPU.md` and `docs/SESSIONS.md`.

| | |
|---|---|
| Machine | Desktop PC |
| CPU | Intel Core i9-10900 |
| GPU | AMD Radeon RX 580 (`amdgpu`), GPU composition |
| Displays | Three monitors (Samsung S27F350), DisplayPort among them |
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

## Community reports

Nine reports from eight people, 2026-11-06 to 2026-11-15. The kind is the one the report detected from the firmware's
chassis type.

| Issue | Machine | Kind | CPU | GPU (driver) | Boot | LordOS | Result |
|---|---|---|---|---|---|---|---|
| [#42](https://github.com/lordbasex/LordOS/issues/42) | Dell Latitude 7480 | laptop | Intel Core i5-7300U, 4 threads | Intel HD Graphics 620 (i915) | internal | 0.8.8.4 | All works; power *limits* (the battery holds 48 % of its design capacity: wear, not LordOS). The touchpad problem ([#43](https://github.com/lordbasex/LordOS/issues/43)) was fixed in 0.8.8.6. Later, after `firmware-intel-graphics` reached the initrd, the reporter needed `i915.enable_dc=0` to stop hard lockups soon after login (DMC firmware); no separate issue yet. Accepted into the tested-hardware catalogue. |
| [#105](https://github.com/lordbasex/LordOS/issues/105) | ASUS K72F | laptop | Intel Pentium P6200, 2 threads | Intel Core Processor Integrated Graphics (i915) | internal | 0.8.8.7 | All works. Software problems in Files, SMB and archives were reported separately ([#101](https://github.com/lordbasex/LordOS/issues/101)–[#104](https://github.com/lordbasex/LordOS/issues/104)). |
| [#129](https://github.com/lordbasex/LordOS/issues/129) | HP EliteBook 840 G6 | laptop | Intel Core i5-8365U, 8 threads | Intel UHD Graphics 620, WhiskeyLake-U GT2 (i915) | internal | 0.8.8.7 | All works; power *limits*. Installed next to Windows 11. |
| [#135](https://github.com/lordbasex/LordOS/issues/135) | TECLAST X98Pro | desktop (the model is a tablet) | Intel Atom x5-Z8500, 4 threads | Intel Atom x5-E8000/J3xxx/N3xxx integrated (i915) | eMMC | 0.8.8.7 | GPU *limits*: the 1536×2048 panel is rotated, and a rotated display is composed on the CPU. Everything else works. The tablet's buttons: [#131](https://github.com/lordbasex/LordOS/issues/131) (open). |
| [#136](https://github.com/lordbasex/LordOS/issues/136) | Apple MacBookPro11,4 | laptop | Intel Core i7-4770HQ, 8 threads | Intel Crystal Well integrated (i915) | internal | 0.8.8.8 | All works. Includes a LordBench quick run. |
| [#137](https://github.com/lordbasex/LordOS/issues/137) | Hampoo P02BD6_HI-122LP | desktop | Intel Atom x5-Z8350, 4 threads | Intel Atom x5-E8000/J3xxx/N3xxx integrated (i915) | eMMC | 0.8.8.7 | All works in the report, but sound needed the Cherry Trail audio driver installed by hand. The same reporter's Chuwi Hi12 threads (same CPU) cover automatic rotation, fixed in 0.8.8.6, sound (`snd_soc_sst_bytcr_rt5651`) and charging (AXP288) in [#60](https://github.com/lordbasex/LordOS/issues/60), and the buttons in [#131](https://github.com/lordbasex/LordOS/issues/131). |
| [#149](https://github.com/lordbasex/LordOS/issues/149) | ASUS K84L | laptop | Intel Celeron B830, 2 threads | Intel 2nd Generation Core integrated, HD Graphics 2000 (i915) | internal | 0.8.8.7 | **Incomplete** (`report:missing`: the file's text was pasted instead of attached). From that text: GPU composition works; network *limits* (Ethernet not connected) and no Wi-Fi adapter detected ([#132](https://github.com/lordbasex/LordOS/issues/132), open); power *limits* (worn battery). |
| [#155](https://github.com/lordbasex/LordOS/issues/155) | Apple MacBookPro12,1 | laptop | Intel Core i5-5257U, 4 threads | Intel Iris Graphics 6100 (i915) | internal | 0.8.8.8 | All works. |
| [#156](https://github.com/lordbasex/LordOS/issues/156) | Unnamed PC (no vendor in the firmware) | desktop | AMD Ryzen 7 5700X, 16 threads | AMD Radeon RX 9060 XT, Navi 44 (amdgpu) | internal | 0.8.8.8 | All works; GPU composition on radeonsi (gfx1200), one HDMI monitor at 1920×1080, no battery. Includes LordBench (quick, 21 measurements). |

## Comparison

Counting the maintainer's PC with the nine community reports (ten machines; #149 is incomplete but its data is
readable).

**By GPU vendor**

| Vendor | Machines | What is known |
|---|---|---|
| Intel (i915) | 8: from Sandy Bridge (HD 2000), Ironlake, Haswell (Crystal Well), Broadwell (Iris 6100), Kaby Lake and Whiskey Lake (620) to Cherry Trail Atoms | GPU composition works on all of them. The only GPU *limits* is a rotated tablet panel, which LordOS composes on the CPU by design (#135). One report of DMC firmware lockups on HD 620 with `firmware-intel-graphics`, worked around with `i915.enable_dc=0` (#42). |
| AMD (amdgpu) | 2: Radeon RX 580 (Polaris, the maintainer's) and RX 9060 XT (RDNA 4, #156) | Both compose on the GPU. The RX 580 brought the cursor-plane, login-screen and display hand-off fixes above. Older cards on the `radeon` driver had a fix in 0.8.8.8 (noise between sessions) but no report. |
| NVIDIA | 0 | **Untested**: no report. Mesa's nouveau and NVK are included; the proprietary driver is not supported. |

**By CPU**: Intel in 9 machines (Core i9, i7, i5, Pentium, Celeron, Atom), AMD in 1 (Ryzen 7 5700X). All are x86-64;
no ARM report yet (the Raspberry Pi 5 is the project's own reference machine, `docs/RASPBERRY.md`).

**By kind**: 6 laptops (Dell, HP, ASUS ×2, Apple ×2), 2 desktops (the maintainer's and #156) and 2 x86 tablets that
the firmware calls desktops (TECLAST X98Pro, Hampoo HI-122LP). Laptop panels, Wi-Fi, Bluetooth and batteries work in
the laptop reports; three of them show *limits* only for a worn battery. On the Cherry Trail tablets the touchscreen and
automatic rotation work; sound, charging detection and the hardware buttons needed or still need work (#60, #131).

**By boot medium**: 8 from an internal disk and 2 from eMMC, all installed. **Untested in reports**: a live USB session
(`live: yes`), and booting an installed system from USB.

**By version**: 0.8.8.4 (1), 0.8.8.5 onwards (the maintainer's), 0.8.8.7 (5), 0.8.8.8 (3).

**Known good**: Intel integrated graphics from 2010 to 2019 on laptops, including two MacBook Pros (2014–2015); an AMD
desktop with a current Radeon (RX 9060 XT) on one HDMI monitor; a Radeon RX 580 with three monitors, DisplayPort among
them, in daily use.

**Known problems**: Cherry Trail tablets' audio, charging and buttons (#60, #131); Wi-Fi not detected on the ASUS K84L
(#132); DMC lockups on one HD 620 laptop after an Intel firmware update (#42, workaround).

**Untested combinations**: any NVIDIA card; AMD integrated graphics (APUs); Intel Arc and Intel graphics newer than
Whiskey Lake (Ice Lake, Tiger Lake, Alder Lake and later); hybrid laptops with two GPUs; DisplayPort MST hubs; 4K and
HiDPI external monitors; suspend and resume (unchecked in every report); live USB sessions.

## Thanks

Thank you to everyone who took the time to send a report:

- [ootada](https://github.com/ootada): #42
- [F-iatt](https://github.com/F-iatt): #105
- [pct06](https://github.com/pct06): #129
- [Touchgg](https://github.com/Touchgg): #135, #137
- [wfxdesign](https://github.com/wfxdesign): #136
- [brokochan](https://github.com/brokochan): #149
- [MacNB](https://github.com/MacNB): #155
- [toulzemaurice-blip](https://github.com/toulzemaurice-blip): #156
