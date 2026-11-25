## In a few words

**Machines** runs other systems in windows of your LordOS desktop, at native speed with KVM where the hardware allows it. It shipped in **0.8.8.5** (alpha): download cards for Debian, Ubuntu, Fedora and Kali that check their makers' signatures and can install with no questions, snapshots, clones, a migration from other virtualization apps, and since 0.8.8.7 shared folders.

**New in 0.8.9.5 and 0.8.9.6.** Most of what follows arrived in **0.8.9.5**; **0.8.9.6** adds encrypted machines, this computer's USB devices and camera, the Whonix (Tor) group and a tighter sandbox (below). It brings Windows 11 machines, the LordOS tools for Windows, a clipboard that only moves when you paste, fast file transfers both ways, apps mode, sound and microphone, real networks, 3D for Linux machines and a picture at the guest's full rate.

[[shot:0]]

## Windows 11, from Microsoft's own servers

A new card in the New Machine assistant makes a machine Windows 11 accepts as it is, on 64-bit PCs:

- **Two ways in.** *Your Windows 11*: Machines opens Microsoft's own download page, finds the ISO in Downloads when it's whole and checks it against the SHA-256 Microsoft publishes for each of its 38 languages. *Try it for 90 days*: Microsoft's Enterprise Evaluation, in English, Spanish or Portuguese, downloaded from Microsoft's server and kept only if its published hash matches.
- **Nothing of Microsoft's is kept by LordOS.** LordOS doesn't host, ship or activate Windows, and the license is your matter.
- **What Windows 11 asks for:** UEFI with secure boot, a TPM 2.0 of its own for each machine, at least 4 GB, 2 processors and 64 GB, and fast virtio disk and network whose drivers Setup loads by itself, with no "Load driver" step.
- **Install without questions.** Your local account and optional password, the computer's name, the edition with your key (or Microsoft's published key that installs it unactivated), no online account asked, privacy options off, and this computer's language, keyboard and time zone. When it's done, Machines erases the small disk that held the password and takes both discs out. Or use Windows' own installer, guided, with the drivers already there.
- **The TPM goes with the machine**: snapshots keep it, reverts put it back and clones take it along.

## LordOS in a machine

The first card of the New Machine assistant is LordOS itself, since 0.8.9.5:

- **The live ISO** of the release the card names, for 64-bit PCs and arm64, checked with the LordOS release key built into LordOS before a byte of it is trusted.
- **Install without questions**: your account and password (kept only as a hash), the computer's name, time zone, keyboard and your language. Only the machine's own new disk can be erased.
- **4 processors, 4 GB, 32 GB and 3D** by default, and the guest agent running from the first boot, so apps mode works right away (with the next ISO).

## The LordOS tools for Windows

A small program LordOS builds, installed for you after an unattended install (or from the agent's button), running as you, never as a service:

- **The resolution follows the window**, exactly, a quarter to half a second after each resize.
- **Copy and paste** of text, rich text (cleaned of scripts on this computer) and pictures.
- **Apps mode**, **dropped files** and **Open with** (below).
- **The pointer drawn by LordOS**, so moving it no longer waits for the machine. A switch per machine gives it back to the guest.
- **A clone gets its own computer name**, used from its next restart.
- **They stay up to date by themselves.** When this LordOS brings newer tools, Machines installs them while the machine runs and nothing is being copied; a switch in Machines settings and in each machine's Configure turns it off, and then a notification offers **Update**.

## The clipboard, only when you paste

A machine is treated as untrusted: a program inside it could read your clipboard or swap what you copied.

- **On demand.** A machine only learns *that* there is something to paste (text, rich text or a picture), never the contents, until a program in it actually pastes, and only while you are using it: its window in front, or one of its apps mode windows, or within 3 seconds.
- **Background protection.** A machine that tries to read or change your clipboard while you are elsewhere is blocked, the attempt is logged, and you get one notice per burst.
- **Ask before each paste.** Choose it in Configure → Sharing and a small bar shows what is about to cross, with *Paste* and *Don't paste*, or *Bring to LordOS* and *Ignore* for a copy coming out. It pastes only what it showed you, and it can't be clicked through by a bar raised under your pointer.
- **Secrets stay put.** A password copied from a password manager inside a machine doesn't reach your clipboard history.

## Files in and out

- **Drag files onto a machine's window** from Files: they land in the Downloads folder of the person signed in to the guest, with a progress line, and get their name only when their size and SHA-256 match.
- **A transfer folder for each machine**, `~/Machines/Transfer/<name>`, fast both ways. A Linux machine has it at `/mnt/LordOS` and as a `LordOS` folder in each home; a Windows machine gets it as a drive, set up by itself after an unattended install.
- **Open with a machine.** In Files, right-click a document or a program → Open with → the machine: it starts or resumes if needed, the file is copied in and opened there with its usual app, and a program runs in its own LordOS window. You can choose to always open a kind of file with a machine.
- **Drop on an app.** Files dropped on an apps mode window go to that window: into the folder it shows, into an app that takes files, or onto its clipboard.

| Measured on the test PC (2 GB file) | Through the guest agent | Through the transfer folder |
|---|---|---|
| Into an Ubuntu guest's Downloads | about 75 s | **17.6 s**, hash checked |
| Into a Windows 11 guest's Downloads | 78–80 s | **25.6 s**, hash checked |
| Out of the machine, saved into its LordOS folder or drive | not possible | **about 2.5 s** |

## Apps mode

Each window of the machine becomes its own LordOS window, with the guest's title, the machine's name and its app's icon in the Dock, the taskbar and the switcher, while the guest's own desktop and taskbar are hidden. Store apps of a Windows guest each get their own entry and icon. Moving, resizing, minimizing or closing a window does the same in the guest. The machine's window becomes a small controller with the guest's programs, and LordOS can only start a program the guest itself listed.

## Sound, microphone and networks

- **Sound** plays on this computer's speakers, with the guest's own mute and volume. **The microphone** opens only while the machine's Microphone setting is on, LordOS's microphone permission allows Machines, the guest is recording and the machine runs; a light shows it.
- **Each machine its own output and input**, picked in Configure or from the speaker button while it runs. An unplugged device falls back to the system's default and comes back when it's plugged in again.
- **Four network modes.** *Shared*, now with up to 32 **forwarded ports**, open to this computer only by default. *Bridged*: the machine joins your wired network as one more device and gets its address from your router's DHCP. *Host-only*: it reaches this computer and nothing else, with no administrator password. *Isolated*. Configure says what each one means for the firewall: a bridged machine's traffic doesn't pass through LordOS's firewall.

## 3D for Linux machines

With **Graphics 3D** in Configure → Display, a Linux machine draws with your graphics card through virgl (OpenGL), on 64-bit PCs. The machine's frames reach LordOS without it ever waiting for the guest's graphics work. Only that machine may use the graphics card's render device, never the display itself.

**Windows machines stay 2D** unless they get a share of an Intel graphics card (GVT-g, below), where Intel's own driver gives them Direct3D, OpenGL and Vulkan: no stable free 3D driver exists for them. 3D for Windows machines through a dedicated second graphics card is planned, and nothing is offered yet. Machines are also 2D on the Raspberry Pi and on a desktop drawn without a graphics card, and a running 3D machine can't be suspended or snapshotted with its memory.

## A smoother picture

LordOS now reads each machine's frames in its own loop and repaints only the part that changed. Measured on a Windows 11 guest drawing 61 frames a second:

| | Before | After |
|---|---|---|
| Frames a second shown | 29.5 | **60.0** |
| Paint per frame | 1.7 ms | **0.1 ms** |
| LordOS's display server, of one core | 54 % | **16–18 %** |
| A pointer move | a round trip through the guest, 35–39 ms | **drawn by LordOS on the next frame** |

The rest of the desktop got the same care: Settings, the Store, Files and Monitor no longer freeze the pointer while they work (changing a setting with three apps open went from 161 to 20 ms, a folder of 2,220 files opens in 229 ms instead of almost 2 s), and switches that start something slow flip at once with a spinner.

## Also new

- **Boot order**: system disk, CD/DVD and network in the order you choose, and a boot menu at start if you want it.
- **Start resolution**: *Follows the window* or a fixed size, taken by every system at boot.
- **A new look**: sheets over the window, grouped settings and 38 glyphs of LordOS's own that follow your accent color, and a Storage view of where your space goes.
- **The Store, arranged by where apps come from**: Debian, Flathub, Web apps and LordOS apps, and the apps LordOS picked.
- **Keys**: Ctrl, Alt, Shift and Win held down for the next key, and combinations your computer would keep for itself, such as Ctrl+Alt+Del, sent straight to the machine.
- **Apps mode, from the menu bar**: an item by the clock for each machine, whose menu works like the machine's Start menu, and a panel with its open windows and its apps.
- **Capture mouse** for games and 3D programs; ❖Esc or Ctrl+Alt lets it go.
- **A real graphics card for a machine**: a share of an Intel iGPU (Broadwell to Comet Lake) through GVT-g, with LordOS's own start-up driver for it, written from public sources.
- **`machines` in the Terminal**: list, start, stop, snapshot, clone and change machines from a terminal. Since 0.8.9.6: run a program in a Linux machine, copy files in and out, read its logs, make a machine from a short file kept with a project, and a local MCP server for coding agents, only for a machine you allowed in its Configure.

## New in 0.8.9.6

- **Encrypted machines**: every disk, the firmware's variables and the TPM encrypted, with a key kept in your keyring (Passwords shows it as "Machines: <name>").
- **Your USB devices and camera in a machine**, from the window's Devices menu, given back when you pick them again, unplug them or stop the machine. Only the person at the computer can hand one over, and disks, network cards and Bluetooth adapters need an administrator. Checked with a real webcam in a real guest.
- **Whonix (Tor)**: a gateway and a workstation from Whonix's signed images, started and stopped together, the workstation wired only to the gateway, with nine extra isolation layers. Checked on a PC: Tor reached, an onion site answered, nothing left the computer but the gateway's connections to Tor.
- **All Displays**: one machine across up to four of your displays.
- **Start Linux directly**, **Guest can see** (every channel between a machine and your computer in one list), **Vulkan for Linux machines**, **vCPU pinning**, a **performance profile** inside the guest, and **`machines migrate`**, which turns another PC's whole disk into a machine.
- **Send to LordOS** and a **shell inside the machine** over the guest agent's own channel, with no network; Install Agent from the LordOS Guest Tools disc for Linux machines without one.
- **Disposable sessions** for any machine, Export, compressed disks and snapshots that keep the machine's settings and a note.
- **A tighter sandbox**: each machine's QEMU in its own namespaces, unreadable by your other programs, with a filter that allows only the system calls it needs and nothing it writes able to run; each machine its own firewall row.

## What was tested, and what not

Checked on one 64-bit PC with KVM, mostly with a development tool that plays the part of LordOS's display server: the Windows 11 unattended install and its tools (resolution, clipboard both ways, apps mode's window list, icons and actions), the transfer folder's numbers above, the three new network modes against a real LAN, sound and microphone through PipeWire, and 3D with an Ubuntu guest on a Radeon RX 580.

**Not yet:** apps mode drawing on a real display, every Configure pane on screen, an install of Windows on the Raspberry Pi (it runs emulated there), 3D for Windows machines without an Intel GVT-g share, and dragging files out of a machine.

The full design, the security rules and every measurement are in `docs/VIRTUALIZATION.md` and `docs/MACHINES-GPU.md`.
