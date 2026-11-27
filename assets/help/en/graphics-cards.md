---
title: Graphics cards
app: settings
section: system
order: 68
keywords: graphics card, gpu, nvidia, amd, intel, driver, cuda, compatibility, report, test, nouveau, mesa, vulkan
---
# Graphics cards

[Settings → Graphics cards](settings:gpus) lists every graphics card in the computer, the driver that runs it and which one draws
the desktop. A card that isn't drawing the desktop can be shared with Machines, when it supports it.

## NVIDIA cards: experimental

NVIDIA support is **experimental and verified by the community**. LordOS's team has tested Intel and AMD graphics on
its own computers, but not every NVIDIA model, so an NVIDIA card says *"Experimental — not verified on your card model
yet"*. The free driver (nouveau, from Mesa) is the default. NVIDIA's own driver is an opt-in: Store → For this
computer offers it for an NVIDIA card while the free driver is in use. Its uses are listed as:

- **AI and compute (CUDA):** **planned**; LordOS doesn't offer it yet.
- **Desktop:** **experimental**. On the card that draws your session, Settings shows how the desktop is really composed
  (on the card, or by the processor). With NVIDIA's driver, one community report so far shows the desktop composed on
  the card (a GTX 1070, Pascal, driver 580); an earlier one (an RTX 3060 on LordOS 0.8.8.9) was composed by the
  processor, and another person saw some apps' windows not show after installing the driver. More reports tell us
  which cards and drivers work.

## How to help test it

1. Open **Settings → Graphics cards** and choose **Send a compatibility report**, then **Create Report**.
2. Read the whole report: it shows the cards, their drivers, which one draws the desktop, whether the screen is composed
   on the graphics card, Mesa's and NVIDIA's versions, the Vulkan and video drivers installed and Secure Boot. It never
   holds your name, the computer's name, paths in your home folder, serial numbers or network addresses.
3. Choose **Open the Form on GitHub**. Your browser opens LordOS's report form, already filled in; the whole report is
   also in the clipboard. Say what works and what doesn't (the desktop, video, games, suspend), then submit it.
   Nothing is sent until you do, and reports on GitHub are public.

Without a browser, open **Terminal** and run `lordos-issue --gpu`: it prints the same report, ready to paste.

Each report is read by LordOS's team. A model with good reports moves from *experimental* to *verified* in the list
of tested graphics cards on lordos.org.
