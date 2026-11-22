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
yet"*. Its future uses are listed as **planned**:

- **AI and compute (CUDA):** installing NVIDIA's driver and its CUDA libraries from Settings.
- **Desktop:** drawing the desktop with NVIDIA's driver.

Neither can be turned on yet, and LordOS changes nothing on the card until they are ready: both need system actions
that are still being reviewed, and the desktop also needs explicit synchronization in the display server.

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
