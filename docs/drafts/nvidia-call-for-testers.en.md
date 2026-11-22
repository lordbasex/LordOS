<!-- DRAFT for the maintainer's approval (2026-11-22). Not posted anywhere. Publish only after the OK, once the
     release with "Send a compatibility report" is out and the "Tested graphics cards" page is live. -->

# Call for testers: LordOS on NVIDIA graphics cards

**Have an NVIDIA graphics card? Help us make LordOS work on it.**

LordOS composes its desktop on the graphics card through Mesa, and that is tested on Intel and AMD graphics. NVIDIA is
different: we don't have every NVIDIA card, and so far one community report (an RTX 3060 on NVIDIA's driver) is all we
know. So NVIDIA support in LordOS is now officially **experimental and verified by the community**, and it grows with
your reports.

## What works today

- The desktop shows on any NVIDIA card, with Mesa's `nouveau` driver or with NVIDIA's own driver (then the screen is
  composed on the processor, which is slower but works).
- **Settings → Graphics cards** marks an NVIDIA card *Experimental — not verified on your card model yet* and lists
  what's coming: **AI and compute (CUDA)** and **the desktop on NVIDIA's driver**. Both are planned, not available yet,
  and LordOS changes nothing on your card until they are.

## What we're asking

1. Start LordOS on your computer: the live USB is enough, nothing needs installing.
2. Open **Settings → Graphics cards → Send a compatibility report → Create Report**.
3. Read the report: it holds technical data only (the cards, their drivers, which one draws the desktop, Mesa's and
   NVIDIA's versions, Vulkan and video drivers, Secure Boot). Never names, paths in your home folder, serial numbers or
   addresses.
4. **Open the Form on GitHub**, tell us what worked and what didn't (the desktop, video, games, suspend, external
   monitors), and submit it. No browser? Run `lordos-issue --gpu` in Terminal and paste its output.

Especially welcome: GTX 10 series (Pascal), RTX 20 (Turing), RTX 30 (Ampere), RTX 40 (Ada) and RTX 50 (Blackwell)
cards, laptops with NVIDIA plus Intel or AMD graphics, and computers with Secure Boot on.

## What happens next

Every report is read by the LordOS team. Models with matching reports move to *community-verified* on
[Tested graphics cards](https://lordos.org/hardware/gpus/) (link live once that page is published). The reports also
shape the next steps: installing NVIDIA's driver and CUDA from Settings (with a guided Secure Boot enrolment), and,
after that, composing the desktop on NVIDIA's driver.

Thank you for testing!
