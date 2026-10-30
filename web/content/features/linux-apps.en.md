## The short version

LordOS has a desktop of its own, but it is still Debian underneath. The tens of thousands of programs in Debian's archive are available, and graphical apps built with GTK or Qt, plus browsers like Chromium and Firefox, open as LordOS windows, with the same frame, the same Dock and the same light or dark mode as the native apps.

[[shot:0]]

The trick is a common language. Linux apps speak **Wayland**, a protocol for "here is my window, here are my pixels, send me the mouse and the keyboard". LordOS's display server speaks it too. So an app that knows nothing about LordOS can still live on its desktop, the way a traveller who speaks the local language doesn't need a guide.

[[diagram:linux]]

## Installing them

- **The Store** shows Debian's apps by category, with search, and shows you a plan (what will be installed or removed) before doing anything. It also installs LordOS's own `.lpk` apps and local `.deb` files.
- **LordOS's own package engine** reads your apt sources, verifies the signed indexes with its own OpenPGP code, resolves dependencies and checks every downloaded file against the SHA-256 in the signed index. Only then does `dpkg`, Debian's own tool, unpack and configure the package. The dpkg database is still written only by dpkg, so `apt` and `dpkg` keep working normally in the Terminal.
- **The Terminal** works as on any Debian system. Every tab already has the Wayland variables set, so starting a GTK or Qt program from it just works.

[[shot:1]]

## Opening them

Apps with a `.desktop` file appear in the app launcher, the Lord Classic theme's start menu and Search, with their icons. Clicking one goes through a deliberately narrow door in `lord-server`:

1. The interface can only pass the **id** of a `.desktop` file that is already installed in `/usr/share/applications` or `~/.local/share/applications`, never a command.
2. `lord-server` re-reads that file, checks it is a real, visible, non-terminal application, and builds the command from its `Exec` line by the specification, without a shell and without arguments from the caller. "Open With" in Files may add exactly one file, only if the app declares the file types it handles and the file is in your home folder or on a disk you mounted.
3. The app is started in its own cgroup, `app-lordos-<id>.scope`, created by root, so the Monitor, the per-app firewall and its restrictions know which app each process belongs to. If you restricted the app from your files, this is also where it gets its own view: its own namespaces, an empty home folder with only its own folders, and Landlock.

JavaScript in LordOS can't write `.desktop` files, so the interface can't invent a command: it can only pick an app that is already installed.

## How they look and behave

- **The frame.** Apps that ask the compositor to draw their frame (Qt apps, foot, Chromium with its system title bar) get the theme's frame. GTK 3 apps such as Firefox get it through a second decoration protocol LordOS also speaks. GTK 4 apps that draw their own bar keep it, and LordOS trims their shadow.
- **Browsers with a single bar.** Since 0.8.4, the preinstalled Chromium and Firefox draw one bar with their tabs and the window buttons inside it, on the side the LordOS theme uses (left in Lord Flow, right in Lord Classic), following a theme or light/dark change live. One switch in Settings → Appearance, "Browsers: tabs in the title bar", puts both back under the LordOS frame. The compositor honours whichever frame an app asks for, even when it changes while the window is open.
- **Light and dark, live.** LordOS serves the "Settings" desktop portal itself on the session bus, so GTK 4 and libadwaita apps follow the mode, accent colour and button order. GTK 3 apps follow through the GSettings keyfile, and LordOS ships a matching GTK theme. Both change live when you switch modes.
- **One clipboard.** Text copied in a Linux app pastes in LordOS and the other way round; images stay available after the source app closes and appear in the clipboard history (❖V).
- **Select to copy, middle click to paste.** Since 0.8.3 the primary selection works between Wayland apps, and between them and X11 apps. Since 0.8.5 the app that copied also sees its own copy in the clipboard, as the Wayland protocol says.
- **Videos keep the screen on** (since 0.8.5): a Linux video player can ask that the screen not dim or lock while it plays (`zwp_idle_inhibit_manager_v1`), and the idle timers wait while its window is shown.
- **Sharp at any scale.** Each display is a separate `wl_output`, and fractional scaling lets GTK 4, Qt 6 and Chromium render sharp at 125–175 %.
- **Touchpad gestures.** LordOS's own touchpad driver passes pinch, swipe and hold gestures to Linux apps, so pinch-to-zoom works in browsers.
- **Games and GPU apps.** With a GPU, apps can hand LordOS GPU buffers directly (`linux-dmabuf`), use real fullscreen, lock the pointer and get raw mouse motion.
- **Notifications** from Linux apps arrive through the standard notifications service, which `lord-server` provides, and can be configured per app in Settings.
- **Passwords:** the standard Secret Service is served by LordOS's keyring, so browsers can keep their secrets there without another keyring daemon.
- **Force Quit** (since 0.8.5): in Lord Flow, the crown menu's Force Quit… lists the open apps and ends the chosen one without asking it. For a Linux app it ends only that app's own process; an X11 app is asked to close instead.
- **Default apps without choosing** (since 0.8.6): when you haven't chosen an app for a file type, the file opens with the system's default or the first installed app that declares its type, shown as "Automatic" in Settings → Default Apps. Apps you chose stay as they are.

[[shot:2]]

[[shot:3]]

## Which Wayland protocols are there

`wl_compositor`, `wl_subcompositor`, `wl_shm`, `wl_output` (one per display), `xdg_wm_base` with popups, `wl_seat` (pointer, keyboard, touch), `wl_data_device_manager` (clipboard), `zxdg_decoration_manager_v1`, the KDE server-decoration protocol, `zxdg_output_manager_v1`, `wp_fractional_scale_v1`, `wp_viewporter`, `zwp_linux_dmabuf_v1`, `wp_presentation`, `zwp_relative_pointer_manager_v1`, `zwp_pointer_constraints_v1`, `zwp_pointer_gestures_v1`, `wp_cursor_shape_manager_v1`, `zwp_primary_selection_device_manager_v1` (since 0.8.3) and `zwp_idle_inhibit_manager_v1` (since 0.8.5).

**Check it from the outside** (since 0.8.5): the **Test Bench** (`lordos-testbench`) is a test-pattern window that checks the display server as any app sees it: frame timing and when frames really reach the screen, every key of the keyboard, the mouse and wheel, and the Wayland protocols the system offers.

Deliberately absent: any protocol that lets an app capture the screen or other apps' windows.

[[shot:4]]

## The skeptical reader's questions

**Are Linux apps sandboxed?** Not by default. This is important: they run as your user, as on any Debian system, and can read your home folder. What LordOS adds is a per-app cgroup, attribution in the Monitor, the kernel-enforced per-app firewall (allow, deny, LAN only, no incoming connections, rules by domain or address), and restrictions you can set per app: no camera, no USB and no Bluetooth sockets (in the kernel), no microphone (LordOS's PipeWire guard plus the sound card's capture nodes), no notifications, and "no access to your files", applied from the next launch. They don't cover the clipboard, Bluetooth through BlueZ on the system bus, or programs started outside the app launcher. Only LordOS's own `.lpk` apps get the full sandbox.

**Can a Linux app see other windows or record the screen?** Not through LordOS. The compositor only gives an app its own surfaces and offers no screen-capture protocol, X11 apps only see other X11 apps (never Wayland apps or LordOS itself), there is no screen-cast portal, and a restricted app is also denied the framebuffer device, so protected windows like Passwords stay private from them too.

**What about X11-only apps?** Since LordOS 0.8.3 Beta they open through Xwayland, started only when one of them connects, with the theme's window frame, their icon in the Dock and the shared clipboard. They get the same per-app identity, firewall and Monitor attribution as Wayland apps; apps with device or file restrictions get no X display. XTerm, a Java Swing app and an SDL game were tested.

**And Flatpak?** LordOS doesn't use or integrate it today. It is not planned for any release: to keep the system stable it is not being built, and a design review for the future, with what Flatpak apps would need to follow LordOS's model (their own identity in the firewall and Monitor, their permissions shown in Settings → Privacy and LordOS's own prompts), is kept in `docs/FLATPAK.md`.

**And apps like Chrome or Spotify?** (since 0.8.5) Store → Commercial installs free-of-charge apps whose source code isn't public from their makers' signed repositories, with the key LordOS pins, and says so before anything is installed.

**Do packages come over HTTPS?** Since LordOS 0.8.5, yes for `https://` sources: a small sandboxed helper, `lord-tls`, checks the server's certificate and name and encrypts the download, so someone on the network can't see which packages you download, and a secure download is never sent back to plain HTTP. Up to 0.8.4.1 they came over `http://`. Either way the safety comes from the signatures and hashes: every index must carry a valid signature from a trusted key, can't be expired or older than the one already stored, and every file is checked against its SHA-256 before it is used, so nobody on the network can change them.

**What is the Artificial Intelligence category?** (since 0.8.5) A Store page with a short list of AI tools chosen by LordOS: coding agents (Claude Code, Codex, goose, opencode), editors (Visual Studio Code, Cursor), the Claude desktop app, llama.cpp to run language models on your computer and whisper.cpp to turn speech into text. The apps whose maker has a signed repository install from it: the Store tells you first which repository it adds, and checks the maker's key against the fingerprint LordOS pins. The others come from LordOS's own signed repository, built by LordOS from verified sources or copied after checking the maker's signature. llama.cpp downloads a model the first time you run it (`llama-server -hf <user>/<model>-GGUF`); it runs on the CPU, and on a GPU through Vulkan, which hasn't been tested on real hardware yet.

## Honest limits

- **X11 apps can see each other,** as on any X11 desktop; they are sharp only at 100 % scale.
- **Not yet:** drag and drop between apps, LordOS input methods (Korean, Japanese, Chinese) inside Linux apps, and app activation requests.
- **SVG-only icons:** LordOS doesn't draw SVG yet. It uses the PNG icons Debian publishes for most apps, or the initial.
- **Terminal apps** with a `.desktop` file aren't shown in the app launcher; start them from the Terminal.
- **GTK 3 apps keep their own blue accent;** Qt 6's reading of the colour scheme from the portal hasn't been tested.

## Read the source

- [server/src/wl/wl.c](https://github.com/lordbasex/LordOS/blob/main/server/src/wl/wl.c): the Wayland compositor and its protocols
- [server/src/apps.c](https://github.com/lordbasex/LordOS/blob/main/server/src/apps.c): the `.desktop` launcher
- [server/src/ui/portal.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/portal.c): the Settings portal for GTK and libadwaita
- [sdk/wayland.js](https://github.com/lordbasex/LordOS/blob/main/sdk/wayland.js): Linux windows in the window manager
- [server/src/pkg](https://github.com/lordbasex/LordOS/tree/main/server/src/pkg): the Debian package engine
- [apps/store](https://github.com/lordbasex/LordOS/tree/main/apps/store): the Store
- [docs/WAYLAND.md](https://github.com/lordbasex/LordOS/blob/main/docs/WAYLAND.md), [docs/PACKAGES.md](https://github.com/lordbasex/LordOS/blob/main/docs/PACKAGES.md), [docs/GPU.md](https://github.com/lordbasex/LordOS/blob/main/docs/GPU.md)
