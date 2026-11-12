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
- **Light and dark, live.** LordOS serves the "Settings" desktop portal itself on the session bus, so GTK 4 and libadwaita apps follow the mode, accent colour and button order. GTK 3 apps follow through the GSettings keyfile, and LordOS ships a matching GTK theme. Both change live when you switch modes. From 0.8.8 to 0.8.8.7 GTK 4 apps stayed dark with a blue accent whatever you chose; this is fixed in 0.8.8.8.
- **Open and save with LordOS's dialog** (since 0.8.8): Linux apps with no file dialog of their own ask through the standard FileChooser portal, and LordOS answers with its own dialog, which looks and works like Files, inside the home folder.
- **Administrator rights through LordOS's prompt** (since 0.8.8): an app that asks with `pkexec`, such as a launcher updating itself, gets LordOS's administrator prompt, which names the app and the command, instead of stopping with error 127. Programs that ask polkit over D-Bus still find no agent.
- **Clicks reach the right surface** (since 0.8.8): the compositor now honours the input region an app sets, so Firefox's tabs, toolbar and pages answer the mouse again.
- **One clipboard.** Text copied in a Linux app pastes in LordOS and the other way round; images stay available after the source app closes and appear in the clipboard history (❖V).
- **Select to copy, middle click to paste.** Since 0.8.3 the primary selection works between Wayland apps, and between them and X11 apps. Since 0.8.5 the app that copied also sees its own copy in the clipboard, as the Wayland protocol says. Since 0.8.8.8 LordOS's own text fields and the Terminal take part too.
- **Videos keep the screen on** (since 0.8.5): a Linux video player can ask that the screen not dim or lock while it plays (`zwp_idle_inhibit_manager_v1`), and the idle timers wait while its window is shown.
- **Sharp at any scale.** Each display is a separate `wl_output`, and fractional scaling lets GTK 4, Qt 6 and Chromium render sharp at 125–175 %.
- **Touchpad gestures.** LordOS's own touchpad driver passes pinch, swipe and hold gestures to Linux apps, so pinch-to-zoom works in browsers.
- **Games and GPU apps.** With a GPU, apps can hand LordOS GPU buffers directly (`linux-dmabuf`), use real fullscreen, lock the pointer and get raw mouse motion. In 0.8.7, a lock holds only with the pointer on the window and the keyboard on its app, and ❖Esc always releases it.
- **Notifications** from Linux apps arrive through the standard notifications service, which `lord-server` provides, and can be configured per app in Settings.
- **Passwords:** the standard Secret Service is served by LordOS's keyring, so browsers can keep their secrets there without another keyring daemon.
- **Force Quit** (since 0.8.5): in Lord Flow, the crown menu's Force Quit… lists the open apps and ends the chosen one without asking it. For a Linux app it ends only that app's own process; an X11 app is asked to close instead.
- **Default apps without choosing** (since 0.8.6): when you haven't chosen an app for a file type, the file opens with the system's default or the first installed app that declares its type, shown as "Automatic" in Settings → Default Apps. Apps you chose stay as they are.

[[shot:2]]

[[shot:3]]

## New in 0.8.8.8

Released on 12 November 2026, the latest release.

- **Drag and drop.** Drag files from Files into a Linux app (it gets a copy; hold Shift to move them), from a Linux app into a Files folder, or between Linux apps. Only apps that are not in a sandbox can hand files to LordOS. X11 apps (through Xwayland) take part too, built but not yet checked on our test machine.
- **Copied files.** Copy in Files and paste in a Linux file manager, or the other way round. Files copied in another app are pasted only after Files asks you, with Cancel as the default.
- **Your input method inside Linux apps.** LordOS's Korean, Japanese and Chinese input methods now type in GTK and Qt apps, with the candidate list at the caret, and never in password fields.
- **Tray icons.** Linux apps that put an icon in the tray show it next to LordOS's status items, with its menu. An icon that exists only as SVG is drawn too, in the bar's colour (checked with the volume applet).
- **Now Playing and the media keys.** Browsers, video and music players appear in Control Center's Now Playing with their title, cover and controls, and the play/pause, next and previous keys reach them.
- **Dialogs and links.** A Linux app's dialog opens over its window and moves with it, and a link opened from LordOS brings forward the browser that is already open. A window can't take the focus by itself.
- **Share a screen or a window.** A video call in a browser can share a screen or one window: LordOS's own chooser shows the displays and the windows with a picture of each and the name of the app asking, nothing is shared until you press Share, and the sharing indicator's Stop ends it. Checked on our test machine with a browser. Passwords and other protected windows are black in a share.
- **Apps that take the shortcuts.** A virtual machine, a remote desktop or a game can take ❖, ⌥Tab and the rest while it's in front, after you allow it once; ❖Esc always gives them back. The prompt names the app by what LordOS checked it is, not by what it says it is.
- **Global shortcuts.** A call app's push-to-talk or a recorder's hotkeys work while another app is in front. You choose the keys in LordOS's sheet, and change or forget them in Settings → Keyboard.
- **X11 apps.** They take part in drag and drop, and look sharp on HiDPI displays because they scale themselves (Settings → Displays brings back the old, blurry way).
- **Terminal apps.** Programs such as htop open in a Terminal tab from the app launcher.
- **Remove any Linux app.** Store → Installed offers Remove for every app a Debian package brought, a `.deb` installed from a file included, through the same plan sheet. Packages Debian marks essential, protected or required are never removed, nor anything when the plan would remove LordOS itself.
- **X11 apps run as administrator.** An X11 app started with administrator rights, such as GParted through its prompt, now opens on the desktop (checked on our test machine). X11 access now needs a per-session key that LordOS gives the apps it starts; restricted Linux apps and isolated apps never get it, and a program of yours started some other way without the key is refused.
- **Web apps.** The Store's Web apps section installs web apps, each in a window and process of its own with its own accounts, through the optional `lordos-web` package. Web apps follow the desktop's light or dark mode, live, through the page's `matchMedia`; a page that picks its colors only through CSS media queries stays light (choose Dark in its own settings). Tried on our test machine, in light and in dark.

Global shortcuts, drags with X11 apps, sharp X11 apps and Terminal apps are built and tested, but not yet checked on our test machine.

## Which Wayland protocols are there

`wl_compositor`, `wl_subcompositor`, `wl_shm`, `wl_output` (one per display), `xdg_wm_base` with popups, `wl_seat` (pointer, keyboard, touch), `wl_data_device_manager` (clipboard), `zxdg_decoration_manager_v1`, the KDE server-decoration protocol, `zxdg_output_manager_v1`, `wp_fractional_scale_v1`, `wp_viewporter`, `zwp_linux_dmabuf_v1`, `wp_presentation`, `zwp_relative_pointer_manager_v1`, `zwp_pointer_constraints_v1`, `zwp_pointer_gestures_v1`, `wp_cursor_shape_manager_v1`, `zwp_primary_selection_device_manager_v1` (since 0.8.3), `zwp_idle_inhibit_manager_v1` (since 0.8.5) and, in 0.8.7, `zwp_text_input_manager_v3`, for the on-screen keyboard (since 0.8.8.8 also for LordOS's input methods). Since 0.8.8.8 the compositor also offers `xdg_activation_v1`, `zxdg_exporter_v2` and `zxdg_importer_v2`, and `xdg_wm_dialog_v1`, and `wl_data_device` gains drag and drop; later in 0.8.8.8 also `zwp_keyboard_shortcuts_inhibit_manager_v1` and `wp_security_context_manager_v1`.

**Check it from the outside** (since 0.8.5): the **Test Bench** (`lordos-testbench`) is a test-pattern window that checks the display server as any app sees it: frame timing and when frames really reach the screen, every key of the keyboard, the mouse and wheel, and the Wayland protocols the system offers.

Deliberately absent: any protocol that lets an app capture the screen or other apps' windows. Since 0.8.8.8 an app can only ask through the ScreenCast portal, and gets only the screen or window you pick, while the sharing indicator shows it.

[[shot:4]]

## The skeptical reader's questions

**Are Linux apps sandboxed?** Not by default. This is important: they run as your user, as on any Debian system, and can read your home folder. What LordOS adds is a per-app cgroup, attribution in the Monitor, the kernel-enforced per-app firewall (allow, deny, LAN only, no incoming connections, rules by domain or address), and restrictions you can set per app: no camera, no USB and no Bluetooth sockets (in the kernel), no microphone (LordOS's PipeWire guard plus the sound card's capture nodes), no notifications, and "no access to your files", applied from the next launch. They don't cover the clipboard, Bluetooth through BlueZ on the system bus, or programs started outside the app launcher. Only LordOS's own `.lpk` apps get the full sandbox.

**Can a Linux app see other windows or record the screen?** Not through LordOS. The compositor only gives an app its own surfaces and offers no screen-capture protocol, X11 apps only see other X11 apps (never Wayland apps or LordOS itself), up to 0.8.8.7 there is no screen-cast portal (since 0.8.8.8 an app can ask for one, and gets only what you pick in LordOS's own chooser, never if you restricted it from capturing the screen), and a restricted app is also denied the framebuffer device, so protected windows like Passwords stay private from them too.

**What about X11-only apps?** Since LordOS 0.8.3 Beta they open through Xwayland, started only when one of them connects, with the theme's window frame, their icon in the Dock and the shared clipboard. They get the same per-app identity, firewall and Monitor attribution as Wayland apps; apps with device or file restrictions get no X display. XTerm, a Java Swing app and an SDL game were tested.

**And Flatpak?** LordOS doesn't use or integrate it today. It is not planned for any release: to keep the system stable it is not being built, and a design review for the future, with what Flatpak apps would need to follow LordOS's model (their own identity in the firewall and Monitor, their permissions shown in Settings → Privacy and LordOS's own prompts), is kept in `docs/FLATPAK.md`.

**And apps like Chrome or Spotify?** (since 0.8.5) Store → Commercial installs free-of-charge apps whose source code isn't public from their makers' signed repositories, with the key LordOS pins, and says so before anything is installed.

**Do packages come over HTTPS?** Since LordOS 0.8.5, yes for `https://` sources: a small sandboxed helper, `lord-tls`, checks the server's certificate and name and encrypts the download, so someone on the network can't see which packages you download, and a secure download is never sent back to plain HTTP. Up to 0.8.4.1 they came over `http://`. Either way the safety comes from the signatures and hashes: every index must carry a valid signature from a trusted key, can't be expired or older than the one already stored, and every file is checked against its SHA-256 before it is used, so nobody on the network can change them.

**What is the Artificial Intelligence category?** (since 0.8.5) A Store page with a short list of AI tools chosen by LordOS: coding agents (Claude Code, Codex, goose, opencode), editors (Visual Studio Code, Cursor), the Claude desktop app, llama.cpp to run language models on your computer and whisper.cpp to turn speech into text. The apps whose maker has a signed repository install from it: the Store tells you first which repository it adds, and checks the maker's key against the fingerprint LordOS pins. The others come from LordOS's own signed repository, built by LordOS from verified sources or copied after checking the maker's signature. llama.cpp downloads a model the first time you run it (`llama-server -hf <user>/<model>-GGUF`); it runs on the CPU, and on a GPU through Vulkan, which hasn't been tested on real hardware yet.

## Honest limits

- **X11 apps can see each other,** as on any X11 desktop; they are sharp only at 100 % scale (since 0.8.8.8 they scale themselves and look sharp, unless an app reads no DPI setting).
- **Drags with X11 apps** are built and tested but not yet checked on our test machine. Drag and drop between apps, LordOS input methods (Korean, Japanese, Chinese) inside Linux apps and app activation requests came in 0.8.8.8 (above).
- **SVG-only icons:** up to 0.8.8.7 LordOS doesn't draw SVG and uses the PNG icons Debian publishes for most apps, or the initial; since 0.8.8.8 `lord-media` draws SVG icons in its sandbox.
- **Terminal apps** with a `.desktop` file aren't shown in the app launcher up to 0.8.8.7; start them from the Terminal. Since 0.8.8.8 they open in a Terminal tab.
- **GTK 3 apps keep their own blue accent;** Qt 6's reading of the colour scheme from the portal hasn't been tested.

## Read the source

- [server/src/wl/wl.c](https://github.com/lordbasex/LordOS/blob/main/server/src/wl/wl.c): the Wayland compositor and its protocols
- [server/src/apps.c](https://github.com/lordbasex/LordOS/blob/main/server/src/apps.c): the `.desktop` launcher
- [server/src/ui/portal.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/portal.c): the Settings portal for GTK and libadwaita, and the FileChooser portal
- [sdk/wayland.js](https://github.com/lordbasex/LordOS/blob/main/sdk/wayland.js): Linux windows in the window manager
- [server/src/pkg](https://github.com/lordbasex/LordOS/tree/main/server/src/pkg): the Debian package engine
- [apps/store](https://github.com/lordbasex/LordOS/tree/main/apps/store): the Store
- [docs/WAYLAND.md](https://github.com/lordbasex/LordOS/blob/main/docs/WAYLAND.md), [docs/PACKAGES.md](https://github.com/lordbasex/LordOS/blob/main/docs/PACKAGES.md), [docs/GPU.md](https://github.com/lordbasex/LordOS/blob/main/docs/GPU.md)
