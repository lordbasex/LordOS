<!-- DRAFT: not published. Awaiting the maintainer's approval (gpus.meta.json). -->

Which graphics cards LordOS runs on, who checked it and how. NVIDIA support is **experimental and verified by the
community**: we don't own every card, so your compatibility reports are how a model gets verified.

## Status

| Status | What it means |
|---|---|
| ✓ **Verified** | The LordOS team tested it on its own hardware with this release line. |
| ✓ **Community-verified** | At least two independent compatibility reports agree, and the team reviewed them. |
| ◐ **Experimental** | One report or none: it may work, nobody has confirmed it yet. Every NVIDIA card starts here. |
| ✗ **Not working** | Reported broken, with an open issue. |

## Tested cards

| Card | Kernel driver | What was tested | Status | Checked by |
|---|---|---|---|---|
| AMD Radeon RX 580 (Polaris, Ellesmere) | `amdgpu`, Mesa `radeonsi` | Desktop composed on the GPU, two DisplayPort monitors, Vulkan (RADV) | ✓ Verified | LordOS team |
| Intel UHD Graphics 630 (Comet Lake) | `i915` with GVT-g | Shared with Machines as a virtual GPU (GVT-g), next to the RX 580 drawing the desktop | ✓ Verified | LordOS team |
| NVIDIA GeForce RTX 3060 (GA106) | `nvidia` (the proprietary driver) | The desktop works, composed on the CPU: LordOS doesn't compose on NVIDIA's proprietary driver yet | ◐ Experimental | One community report ([#171](https://github.com/lordbasex/LordOS/issues/171)) |
| NVIDIA GeForce RTX 5070 Ti 16 GB (GB203) | `nvidia` 620.73.04 (NVIDIA's open kernel modules) | The desktop works, smooth, with no tearing or glitches; the report doesn't say whether it was composed on the GPU or the CPU. CUDA's libraries were installed and llama.cpp ran on it | ◐ Experimental | One community report ([#235](https://github.com/lordbasex/LordOS/issues/235)) |
| Intel HD Graphics 520 (Skylake GT2) | `i915`, Mesa `iris` | The desktop works, smooth, with no glitches, composed on the GPU | ◐ Experimental | One community report ([#278](https://github.com/lordbasex/LordOS/issues/278)) |
| Intel UHD Graphics (Tiger Lake-H GT1) | `i915`, Mesa `iris` | Draws the desktop, composed on the GPU, on a laptop with a second, NVIDIA GPU; the report says it works with problems it doesn't name | ◐ Experimental | One community report ([#274](https://github.com/lordbasex/LordOS/issues/274)) |
| NVIDIA GeForce GTX 1070 (GP104) | `nvidia` 580.196.11 (the proprietary driver) | The desktop works, smooth, with no glitches, composed on the GPU with NVIDIA's OpenGL ES; video playback | ◐ Experimental | One community report ([#276](https://github.com/lordbasex/LordOS/issues/276)) |
| NVIDIA GeForce GTX 1650 Mobile / Max-Q (TU117M) | `nvidia` 550.163.01 (the proprietary driver, Debian's packages) | The second GPU of a hybrid laptop, next to the Intel card that draws the desktop. Monitor → GPU didn't show its numbers: it doesn't yet find NVML where Debian's packages put it | ◐ Experimental | One community report ([#274](https://github.com/lordbasex/LordOS/issues/274), [#273](https://github.com/lordbasex/LordOS/issues/273)) |

More Intel and AMD graphics run LordOS in the community's
[hardware reports](https://github.com/lordbasex/LordOS/blob/main/docs/HARDWARE-REPORT.md); this page lists the cards
checked for graphics specifically.

## NVIDIA: what works today

- **Any NVIDIA card** shows the desktop: on Mesa's `nouveau` driver, or on NVIDIA's driver with the screen composed on
  the CPU. Settings → Graphics cards marks it *Experimental — not verified on your card model yet*.
- **AI and compute (CUDA)** and **the desktop on NVIDIA's driver** are planned, not available yet: installing NVIDIA's
  driver from Settings and composing the desktop on it come after their system actions and the explicit-sync design
  are approved.

## Send a report

1. On LordOS, open **Settings → Graphics cards** and choose **Send a compatibility report**.
2. Read the whole report. It holds technical data only: the cards, their drivers, which one draws the desktop, how the
   screen is composed, Mesa's and NVIDIA's versions, the Vulkan and video drivers installed and Secure Boot. Never
   names, paths in your home folder, serial numbers or addresses.
3. Choose **Open the Form on GitHub**, say what worked and what didn't, and submit it. Without a browser, run
   `lordos-issue --gpu` in Terminal and paste its output into a
   [new graphics report](https://github.com/lordbasex/LordOS/issues/new?template=gpu-report.yml).

## How reports are reviewed

- The team reads every report and checks that its data hangs together (the PCI ids, the driver and its version, which
  card draws the desktop), and asks in the issue when something is missing.
- A model moves to **Community-verified** when two independent reports for the same model and driver family agree;
  a report that something is broken opens a bug and marks the model **Not working** until it is fixed.
- Only the card, its driver and the outcome reach this page, with a link to the public issue. Nothing else is copied.
