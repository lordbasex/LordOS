## The short version

LordOS is a GNU/Linux distribution based on Debian 13. What makes it different is the desktop: instead of shipping GNOME or KDE, LordOS has a desktop environment of its own, built from its fundamental components on top of Debian and Wayland.

Think of a house. Debian and the Linux kernel are the foundations, the plumbing and the wiring: they are reused, well tested and maintained by a large community. LordOS builds the rooms you actually live in: the screen you see, the windows you move, the apps you open and the guards at the doors.

[[shot:0]]

[[diagram:layers]]

## What is ours and what is reused

Being precise matters here, because "our own" is easy to overstate.

**Written for LordOS (in this repository):**

- **The display server and Wayland compositor** (`lord-server`, in C). It sets display modes with the kernel's DRM/KMS interface directly, without libdrm; reads keyboards, mice, touchpads and touchscreens from evdev, without libinput; composites every window; and speaks the Wayland protocol to Linux apps.
- **The UI engine.** The desktop and the native apps are written in JavaScript with React. They run on QuickJS (a small JavaScript engine, vendored as source) inside `lord-server`, with a native node tree, a flexbox layout engine, a 2D rasterizer and a text engine written in C. There is no web browser engine behind the interface.
- **The native apps:** Files, Settings, Terminal, Store, Mail, Calendar, Contacts, Reminders, Passwords, Monitor, Photos, Player, PDF Viewer, Disks, Backups and the rest, all React apps on the same engine.
- **The security services:** the app sandbox launcher, the permission broker, the per-app permission store, the firewall service (`lord-netperm`), package signing and the password vault (`lord-vault`).
- **System services** such as the session manager and login screen (`lord-sessiond`), the network service (`lord-netd`) and the fan service (`lord-fand`).

**Reused from Debian and the free software world:**

- the Linux kernel and Debian's packages (`dpkg` still unpacks and configures every package);
- Mesa, for optional GPU composition;
- PipeWire, for sound;
- the Wayland protocol itself, through `libwayland-server` and `libwayland-client`;
- `libxkbcommon` for keyboard layouts;
- libraries used only inside sandboxed helper processes: FFmpeg (media), Poppler and libarchive (documents), OpenSSL (TLS for mail and remote desktop), Monocypher (the vault's cryptography).

The rule the project follows is written down in `docs/ARCHITECTURE.md`: X11, wlroots, libdrm, libinput, WebKit, GTK and Qt are not used inside LordOS itself. GTK and Qt apps still run, as Wayland clients.

## How a frame reaches the screen

Here is what happens when you move the mouse over the Dock, step by step:

1. The kernel delivers an input event through evdev. `lord-server` reads it and finds which node of the interface is under the pointer.
2. It calls the JavaScript event handler. React updates its state and tells the engine which nodes changed: a style, a child, a text.
3. The engine recomputes the layout and works out the damaged region: only the part of the screen that really changed.
4. It repaints only that region, copies it to the back buffer, draws the cursor and flips the page on the next vertical blank.

If a GPU with 3D acceleration is present, composition goes through Mesa (GBM, EGL and GLES2). If not, or if the only renderer is a software one, the CPU path does the work. The CPU path always has to keep working: it is a hard rule in the code (`LORD_GPU=0` forces it), and grub's "safe mode" boot entry uses it.

[[diagram:frame]]

## Where Linux apps plug in

There are three kinds of windows on a LordOS desktop, and they connect at different layers:

- **Built-in apps and the theme** (menu bar, Dock, Files, Settings…) run inside `lord-server`'s own JavaScript context, on the UI engine. The exceptions are Passwords and, since 0.8.2, Mail, Calendar, Contacts and Reminders, which run as isolated processes like `.lpk` apps.
- **Installed `.lpk` apps** each run as a separate process (`lord-server --app <id>`, the same binary in "app mode"). That process has its own copy of the UI engine, locks itself in a sandbox and draws into a shared-memory buffer, which it hands to the compositor as an ordinary Wayland client.
- **Linux apps** (GTK, Qt, Chromium, Firefox…) connect to the compositor's Wayland socket. They draw themselves; the theme puts its window frame around them.

So the compositor is the meeting point. The UI engine is used by LordOS's own interface and apps, not by Linux apps.

[[shot:3]]

## Light by design

LordOS aims at modest machines. A few real measurements from the project's test logs:

- The final test ISO, idle a minute after login on a 4 GB, 4-core VM: **465 MB of RAM used** for the whole system in the live session and **487 MB** on the installed system, with the CPU **99 % idle**. Boot took 16.3 s (`docs/ISO.md`, "Final ISO test").
- On a Raspberry Pi 5 (16 GB) at the login screen, without a monitor: `lord-server` at **36 MB RSS** and 0.6 % CPU (`docs/RASPBERRY.md`).
- When the machine has no 3D-capable card, `lord-server` doesn't even load Mesa and stays at about 32 MB of RSS (`docs/GPU.md`).
- Since 0.8.5 the login screen no longer stays in memory behind your session: it closes 10 seconds after a session takes the screen and starts again in a few milliseconds when you lock or switch user. On a VMware VM the whole system went from **603 to 548 MB** in use, and locking the screen went from 903 ms to **141 ms**.
- On virtual graphics cards that complete every page flip at once (VMware's `vmwgfx`), 0.8.5 paces frames to the display's refresh instead of drawing 500–580 frames a second: **59.8 frames a second** at 60 Hz, 99th percentile 18.4 ms. Every boot's time is shown in About LordOS → Startup time and kept for comparing versions (`docs/RESULTS.md`).
- **In 0.8.6, measured on pre-release builds:** on a Raspberry Pi 5 with a 1920×1080 monitor, LordBench's desktop test moves a window with a turning square inside at **59.8–60 frames a second** (53–58 before the work on a dragged window's frame, which keeps every pixel the same), and at about 31 (30.5–31.1) while every core is busy with a low-priority load (24–25 before; `docs/BENCHMARK.md`). Drawing the dragged window on the graphics chip and painting the next frame while the previous one waits for the screen were measured there too and were off by default in 0.8.6: the first was exact but gave no measurable gain, the second gave 33–36 frames a second under load but the pointer reached the screen 17–29 ms later. In 0.8.7 the first covers the whole drag and is on by default (see the limits below).

These numbers come from virtual machines and one board, not from a range of real PCs. They are a starting point, not a promise for every machine.

[[shot:2]]

## Desktops

In 0.8.7: several desktops in both Lord Flow and Lord Classic, with an Overview of live windows, a strip of desktops and dragging a window from one to another. Each window comes back on its desktop after a restart, and a window on a hidden desktop never has the keyboard, not even one that asks for fullscreen from there. Checked in an isolated session on a virtual machine, with X11 apps in a full session on another, and the Overview on a Raspberry Pi 5's monitor; a right-to-left language on screen and real trackpad swipes are still to check.

## Files, servers and other disks

All in 0.8.7.

- **Connect to a server.** Network lists each computer like a folder, and its shares unfold in place. Go → Connect to Server takes smb, nfs, sftp, ftp, ftps and WebDAV addresses, and all of them open as folders in Files, with SSH keys and certificates checked first.
- **Remote screens.** Remote Desk, one app for RDP and VNC, comes with the system and draws with LordOS's own engine: no client window, no X11 session, no toolkit.
- **Disks from other systems.** Disk Formats, one Store item, opens NTFS, exFAT and FAT for reading and writing, journaled HFS+ read-only and, on amd64 as an optional part, APFS read-only. These are commercial formats; the drivers are free software at no cost.
- **Your way of working.** Choose, reorder and resize list columns, set Files' own Settings, and open Terminal in the folder you're in.

## Keys, the on-screen keyboard and coming back

All in 0.8.7.

- **Shortcuts per theme.** One dispatcher runs before every key listener, so a key it takes reaches nobody else, and both themes get an app switcher with large icons. Over a Linux app the desktop keeps only a fixed list of keys, chosen by name. Settings → Keyboard lists the shortcuts of the theme in use.
- **Pointer lock.** A lock holds only with the pointer on the window and the keyboard on its app, and ❖Esc always releases it, so a window in the background can't hide the pointer as it passes over.
- **On-screen keyboard.** Opened by hand from the button next to the clock, for a computer with no physical keyboard, in LordOS fields, Linux apps, isolated apps and at the login and lock screens. It takes a closed list of keys, so it can't be a shortcut, nothing keeps what is typed, and its panel is black in screenshots and recordings. No run on a real touchscreen is recorded.
- **Session restore.** Apps open at logout, Restart or Shut Down come back with their windows and desktop, and after a power cut or a crash LordOS asks first. Only app identifiers are saved, never document paths. Tested on a virtual machine, with reboots, a real shutdown and a forced power-off.

## Many displays, sharp text

The compositor drives several displays at once, extended or mirrored, and draws each one at its own scale: 100, 125, 150, 175 or 200 %. Text, vectors and images are rasterized at each display's real resolution, so a 4K panel at 150 % next to a 1080p one at 100 % is sharp on both. Linux apps get one `wl_output` per display and the fractional scale protocol, so GTK 4, Qt 6 and Chromium render sharp too. Since 0.8.5, Settings → Displays numbers each display, Identify shows that number big on the monitor itself, and a dragged display snaps edge to edge without overlapping. The Window menu can move a window to another display, and the Dock can assign an app to one.

[[shot:1]]

## Honest limits

- **Real PC GPUs are not verified yet.** The GPU path runs on the Raspberry Pi 5's V3D and on VMware's virtual GPU (SVGA3D), and was exercised with Mesa's software renderer in virtual machines. Testing on real AMD and Nvidia cards is still pending (`docs/GPU.md`).
- **GPU drag, on by default in 0.8.7, is verified on V3D and SVGA3D only.** On a Raspberry Pi 5, in real drags, it halves the session's CPU alone and takes a memory load from 29 to 35 frames a second. On any other GPU only its self-test guards it, and `LORD_GPU_DRAG=0` turns it off (`docs/GPU.md`).
- **Multiple displays were tested with two virtual heads**, not with real monitors, laptop panels or DisplayPort hubs (`docs/DISPLAYS.md`).
- **The Raspberry Pi 5 has been tested with one HDMI monitor only.** Since 0.8.4.1 the image shows the login screen and the desktop on an HDMI monitor from the first boot, and LordBench measured it there; two monitors and 4K have not been tested on the board yet (`docs/RASPBERRY.md`).
- **On the Raspberry Pi 5 a busy app still halves the desktop's frame rate:** about 31 instead of 60 frames a second in LordBench's desktop test while every core is busy. Under that load the graphics chip takes 14–16 ms per frame instead of 1.5, with no clock or thermal throttling; in 0.8.7, a measurement put the cause on memory: 28 ms a frame with four cores reading memory, 1.4 ms with four cores only spinning. How much of that the compositor can avoid is still open (`docs/GPU.md`).
- **X11 apps run through Xwayland since 0.8.3,** sharp only at 100 % scale and able to see each other, as on any X11 desktop. Drag and drop between Linux apps and LordOS input methods inside Linux apps are pending (`docs/WAYLAND.md`).
- **Accessibility is mostly ahead of us:** today there is a "reduce motion" setting; a screen reader, zoom and high contrast are on the roadmap.

## Read the source

- [docs/ARCHITECTURE.md](https://github.com/lordbasex/LordOS/blob/main/docs/ARCHITECTURE.md): the layers and the dependency policy
- [server/src/kms.c](https://github.com/lordbasex/LordOS/blob/main/server/src/kms.c): modesetting with DRM ioctls, no libdrm
- [server/src/input.c](https://github.com/lordbasex/LordOS/blob/main/server/src/input.c): evdev input
- [server/src/gpu/gpu.c](https://github.com/lordbasex/LordOS/blob/main/server/src/gpu/gpu.c): optional GPU composition with Mesa
- [server/src/ui/layout.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/layout.c) and [server/src/ui/paint.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/paint.c): layout and painting
- [runtime/index.js](https://github.com/lordbasex/LordOS/blob/main/runtime/index.js): the React renderer for the native engine
- [server/src/wl/wl.c](https://github.com/lordbasex/LordOS/blob/main/server/src/wl/wl.c): the Wayland compositor
- [docs/GPU.md](https://github.com/lordbasex/LordOS/blob/main/docs/GPU.md), [docs/DISPLAYS.md](https://github.com/lordbasex/LordOS/blob/main/docs/DISPLAYS.md), [docs/ISO.md](https://github.com/lordbasex/LordOS/blob/main/docs/ISO.md)
