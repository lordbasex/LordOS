# Web apps: the LordOS Web Runtime (`lord-web`)

Web apps and PWAs (WhatsApp, Telegram, Outlook, Notion, Spotify…: 37 in the catalog) installed as LordOS apps, each one
in its own process, with as many accounts ("profiles") as the person wants. Approved by the maintainer on 2026-11-10 (AGENTS.md, `lord-web`).
In LordOS 0.8.8.8 (released 2026-11-12).

LordOS doesn't change the sites or their PWAs: it runs them as their sites serve them, and the page is the service's
own web interface (no native client of any service). It adds the window, the desktop integration and the security
around it. Where WPE WebKit 2.48 lacks something a desktop needs, `lord-web` adds small scripts of its own, each one
described below: the clipboard's `write`/`writeText` when the page has none and the drop's events (in the page's world,
talking to a bridge in the isolated world `lordos-web-xfer`), the drag-select and copy helpers (the isolated world
`lordos-web-select`, which the page can't see or change), and the `matchMedia` answer for light and dark (the page's
world, with no privileges). None of them changes the site's code or content. The service names in the catalog are trademarks of their owners, used only to
identify the services; LordOS is not affiliated with, endorsed or sponsored by them.

## What it is

- **`lord-web`** (`server/src/web/`, C): one process per app and profile. It is the only binary that links WPE WebKit,
  which brings GStreamer and libsoup, through one of two backends ("The view's backend", below): **WPEPlatform** (WPE
  WebKit 2.54 built with it: LordOS's own backport for Debian 13, [WPEWEBKIT.md](WPEWEBKIT.md), in development) or the **legacy API** (Debian 13's
  `libwpewebkit-2.0` 2.48, `libwpe` 1.16 and `wpebackend-fdo` 1.16). It ships in the optional package `lordos-web`
  (`tools/iso/mkdeb-web.sh`, which depends on WebKit, GStreamer's good and bad plugins, `bubblewrap` and
  `xdg-dbus-proxy`; built only when `pkg-config` finds WPE, `WEB=0` skips it, and the build says why it skipped it).
  Without that package the rest of LordOS works unchanged and the web app features say how to get it.
- **The manager** lives in what LordOS already has:
  - `lord.webapps` in lord-server (`server/src/ui/js_webapps.c`) keeps the registry and starts `lord-web`;
  - the Store has a "Web apps" section;
  - Settings has a "Web Apps" page;
  - the Dock and the launcher show each app with its icon.
- **WebKit's own processes** (web content, network) run under WebKit's sandbox: bubblewrap, seccomp and
  `xdg-dbus-proxy`, always on.

First check (2026-11-10, VM 127, `cog` 0.18 on the same WPE): WhatsApp Web loaded and showed its QR code in a window of
LordOS's compositor, with WebKit's sandbox running. For one account it used about 810 MB PSS in total: the UI process
61 MB, the network process 67 MB, and two web processes of 542 MB and 138 MB, the second one prewarmed.

## The registry (written only by `lord.webapps`, read by `lord-web`)

`~/.config/lordos/web/` is in `fsops_protected`. Each app is one file `apps/<app>.json`:

```json
{
  "schema": 1,
  "id": "whatsapp",
  "name": "WhatsApp",
  "startUrl": "https://web.whatsapp.com/",
  "scope": "https://web.whatsapp.com/",
  "origin": "https://web.whatsapp.com",
  "icon": "icons/whatsapp.png",
  "themeColor": "#25d366",
  "source": "catalog",
  "background": true,
  "profiles": [ { "id": "personal", "name": "Personal" }, { "id": "work", "name": "Work" } ],
  "permissions": { "notifications": "ask", "camera": "ask", "microphone": "ask", "screen": "ask", "location": "deny" }
}
```

- `id` and every profile `id` match `^[a-z0-9][a-z0-9-]{0,39}$`.
- `startUrl` and `scope` are `https:` URLs (an `http:` URL only for `localhost`) whose origin is `origin`. `scope` is
  a prefix of `startUrl`.
- `icon` is relative to `~/.config/lordos/web/`: a PNG, or an SVG drawn by lord-media.
- A permission is `"allow"`, `"ask"` or `"deny"`.
- `source` is `"catalog"` (from the Store's signed list) or `"url"` (added by the person from an address).
- `background`: closing the window keeps the app running (for notifications), until Quit; the Dock and the launcher
  bring the window back (through `Show`, below).
- `tray` (optional, `false` when missing, written only when `true`): an app in the background also shows a tray icon
  (StatusNotifierItem) with Open and Quit. Without it the app still runs in the background; it is just not in the tray.
- `notifySound` (optional, `"auto"` when missing, written only when it isn't): LordOS's sound for the page's
  notifications. `"auto"`: LordOS plays its sound unless the page plays its own (below); `"app"`: never LordOS's sound;
  `"system"`: always LordOS's sound; `"none"`: never LordOS's sound, and lord-web doesn't touch the page's audio
  either (a page that plays its own sound still does).
- `colorScheme` (optional, `"auto"` when missing, written only when it isn't): the app's Appearance, the light or dark
  preference its pages are told. `"auto"` follows the desktop's mode, `"light"` and `"dark"` always say that one
  ("Light and dark", below). Before 0.8.9.6.2 (lord-web on WPEPlatform), `"light"` and `"dark"` were kept but read as
  `"auto"` ("Each app's Appearance", below).
- A profile's `userAgent` (optional, `"chrome"` when missing, written only when it isn't): its Browser identity, the
  browser its pages are told it is: `"chrome"`, `"edge"`, `"firefox"` or `"lordos"` ("Browser identity",
  below). `"experimental"` (removed, 2026-11-15) is still accepted and read as `"chrome"`, so an older file loads; the
  file isn't rewritten for it. Anything else, and the key at the app's level, is refused.
- Unknown keys are refused. The file is at most 64 KB.

Data, per app and profile, under `~/.local/share/lordos/web/<app>/<profile>/` (also in `fsops_protected`): `data/`,
`cache/`, `cookies.sqlite` and `state.json`, lord-web's own (what it learned about the page: `{ "ownSound": true }`).
Nothing of one profile is readable by another.

## The view's backend: WPEPlatform or the legacy API (issue #142, in development)

WPE WebKit has two ways for an application to show a web view. The legacy one (libwpe and wpebackend-fdo) is
deprecated upstream and already missing on some distributions; Debian 13 keeps it at 2.48 for its whole life.
WPEPlatform is its replacement (WPE WebKit 2.50 and later). The maintainer decided on 2026-11-23 that lord-web moves to
WPEPlatform, with LordOS's own build of wpewebkit 2.54 for Debian 13 that has both APIs on.

Everything lord-web does with WebKit's frames and input goes through one small interface, `server/src/web/webback.h`;
the rest of lord-web (the window, the clipboard, drops, the input method, the page's scripts, the bus) uses WebKit's
own `webkit_web_view_*` API and is the same on both. Exactly one backend is built:

- **`webback_platform.c` (WPEPlatform):** lord-web brings its own platform instead of WPEPlatform's Wayland one, which
  would open a window of its own; lord-web keeps its window (`webwin.c`) and hands lord-server its frames itself, as
  before. `LordWebDisplay` (a `WPEDisplay`) has no connection of its own: its DRM device is the compositor's GPU (the
  render and primary node `webwin.c` reads from EGL, `EGL_EXT_device_query`), its preferred buffer formats the ARGB and
  XRGB formats and modifiers lord-server announces on `zwp_linux_dmabuf_v1`, its keymap WPEPlatform's xkb keymap fed
  with the compositor's, its clipboard WPEPlatform's in-process `WPEClipboard` (`webwin.c` mirrors the session's
  clipboard into it, as it did into libwpe's pasteboard; it reads UTF-8 back as UTF-8), and one `WPEScreen` (the output
  the window is on: its size and refresh rate). `LordWebView` (a `WPEView`) gets each frame in `render_buffer`, a
  `WPEBufferDMABuf` (its planes become a `wl_buffer` of the window, no copy) or a `WPEBufferSHM` (copied into one of
  the window's `wl_shm` buffers); `buffer-rendered` is emitted at lord-server's frame callback and `buffer-released`
  when lord-server releases the `wl_buffer` (or right after the copy). The page's cursor arrives by its CSS name and
  becomes a `wp_cursor_shape` shape. `LordWebToplevel` (a `WPEToplevel`) has the view; its size, scale and active state
  are the `xdg_toplevel`'s, and WebKit's own requests to resize, maximize or go fullscreen are refused, as before.
  Input becomes `WPEEvent`s in surface coordinates (WPEPlatform's are logical pixels; libwpe's were buffer pixels).
  The input method stays lord-web's `WebKitInputMethodContext` on text-input-v3, which replaces the one WebKit builds
  on a display's `WPEInputMethodContext`: one input method for both backends. Without a GPU (`LORD_GPU=0`,
  `LORD_WEB_SHM`, no `zwp_linux_dmabuf_v1`) the display has no DRM device and WebKit draws on the CPU into shared memory.
- **`webback_legacy.c` (legacy API):** the code lord-web had, moved behind the interface unchanged: wpebackend-fdo's
  exportable view backend (`webshm.c` reads its own `wl_shm` buffers and watches its `wl_resource`s), libwpe's input
  events and xkb context, libwpe's pasteboard.

`mk/web.mk` picks the backend: WPEPlatform when `pkg-config` finds `wpe-platform-2.0`, else legacy when it finds
`wpe-1.0` and `wpebackend-fdo-1.0`; `LORD_WEB_BACKEND=legacy|platform` forces one (an error when its packages are
missing). Without either, or with `WEB=0`, the build prints why lord-web was skipped. A switch rebuilds lord-web
(`build/gen/web_backend`), `tools/iso/mkdeb-web.sh` writes the matching Depends (no `libwpe`, `wpebackend-fdo` or
`libwayland-server` for WPEPlatform), and lord-web's log names the backend and WebKit's version at start
(`lord-web: platform backend (WPE WebKit 2.54.3), dmabuf`).

**What WPEPlatform changes in lord-web's isolation** (for review; nothing was added to lord-web's own sandbox):

- lord-web no longer runs wpebackend-fdo's nested Wayland compositor: no `libwayland-server`, no socket pair to the
  web process for frames. Frames and their dmabuf or shared memory fds come over WebKit's own IPC, which already
  carried everything else.
- The GPU: lord-web still opens EGL on lord-server's display only to check `EGL_EXT_image_dma_buf_import` and to read
  the GPU's node names (`/dev/dri/renderD*`, `/dev/dri/card*`), as before; it now passes those names to WebKit
  (`WPEDRMDevice`), and the web process opens the render node with GBM, where the legacy web process opened the same
  node through EGL. WebKit's sandbox exposes `/dev/dri` to the web process in both (`--dev-bind-try /dev/dri`); WebKit
  2.54's sandbox also exposes `/dev/udmabuf` (GStreamer 1.28's software decoders), a change of WebKit's version, not of
  the backend. The primary node is passed by name only; nothing in lord-web opens it.
- Shared memory: the web process's `WPEBufferSHM` is mapped by lord-web from an fd WebKit passes (before, a `wl_shm`
  pool of wpebackend-fdo's); lord-web also makes one sealed memfd for the keymap it hands WPEPlatform.
- The environment: WPEPlatform reads `WPE_DISPLAY`, `WPE_DRM_DEVICE` and other `WPE_*` variables, which lord-web
  already drops before WebKit starts (`clean_env`); `LIBGL_ALWAYS_SOFTWARE` is set by lord-web itself on the CPU path.
- No new socket, device, file or D-Bus name; the session's Wayland socket is still lord-web's alone (WebKit's web
  process never gets it on WPE).

**Checked** (2026-11-23): `build/web-backend-test` (`tests/web_backend_test.c`, run by `make -C server test`) drives
the backend alone, headless, with a throwaway HOME and WebKit's sandbox: a local page's pixels in the frame, a click
(button and position) and keys (`a`/`KeyA`, Shift+`B`) reaching the page's listeners and the next frame, a wheel notch
down (`deltaY > 0`), a resize to 500×400, scale 2 (`devicePixelRatio` 2, a 1000×800 frame, a click at the same CSS
position) and hidden then visible (`visibilitychange`). It passes on the legacy backend with Debian 13's 2.48.3 and on
both backends with Debian sid's 2.54.3 (both APIs on, as the backport will be). In a window: lord-web in a headless
weston (sid, 2.54.3), both backends, shared memory (pixman) and dmabuf (GL on the GPU), the page's colour in weston's
screenshot. Still to check on the backport and the VM: every web app feature (`docs/ROADMAP.md`, the WPEPlatform item).

## Starting an app

Only lord-server starts `lord-web`:

- **Path:** fixed, next to the lord-server binary (`<lord-server dir>/lord-web`).
- **argv:** exactly `lord-web --app <app> --profile <profile>`, both checked against the registry first, or the same
  followed by `--inspect` for a window with developer tools ("Developer tools", below).
- **Environment:** only the session's (`WAYLAND_DISPLAY`, `XDG_RUNTIME_DIR`, `DBUS_SESSION_BUS_ADDRESS`, `HOME`,
  `LANG`) plus `LORDOS_WEB=1`, and `LORDOS_SESSIOND=1` when lord-server runs under lord-sessiond: lord-web then gets
  its scope from lord-sessiond and refuses to start without it or without Landlock (development runs go on). Once the
  theme has told lord-server the desktop's mode, also `LORDOS_WEB_SCHEME=light` or `dark` ("Light and dark", below).
  (For an app whose Appearance is Light or Dark, always that word once per-app light and dark return with
  WPEPlatform; for now every app gets the desktop's.)

`lord-web` reads its app's registry file itself, read-only, and refuses to start on any mismatch. A second launch of a
running app and profile raises its window instead: lord-server knows its process, starts nothing, issues an
xdg-activation token and calls `Show(token)` on that lord-web (below); lord-web activates its window with it, or maps it
again when it was hidden in the background. The shell does not focus it on its own.

The window's `xdg_toplevel` app id is `lordos-web.<app>.<profile>`. lord-server maps that prefix to the registry
entry, so the Dock and the window switcher show the app's name, its icon and the profile name. The title is the page's
title. The frame is LordOS's (server-side decoration).

How it is built (`server/src/web/webwin.c`, `webengine.c`, `webmain.c`):

- **One process per app and profile:** the profile folder holds a `lock` (flock). A second start finds it taken, calls
  `Show(token)` on `org.lordos.WebApp1` (`/org/lordos/WebApp1`) at the bus name
  `org.lordos.WebApp.app_<app>.profile_<profile>` (`-` as `_`) with its `XDG_ACTIVATION_TOKEN`, and exits with 0.
- **Frames:** WebKit's web process draws through the view's backend (below): into wpebackend-fdo's exportable view
  backend (legacy), or into WPEPlatform buffers handed to lord-web's own `WPEView` (platform). When the compositor offers
  `zwp_linux_dmabuf_v1` and EGL works on its display, a dmabuf frame becomes a `wl_buffer` of the window (no copy); a
  frame the compositor can't import restarts lord-web once on the shared memory path. Otherwise (`LORD_GPU=0`, no GPU,
  VM 127) every frame is copied into one of three `wl_shm` buffers. WebKit's next frame is released only after the
  compositor's frame callback. Scale: `wp_fractional_scale_v1` with `wp_viewport`, or the outputs' integer scale.
- **Input:** the compositor's keymap (xkbcommon) and key repeat, the pointer (side buttons go back and forward), touch
  (up to 10 points). Ctrl+R/F5 reload, Alt+Left/Right go back and forward, Ctrl+plus/minus/0 zoom. text-input-v3 is
  WebKit's input method context (preedit, commit, the cursor rectangle and the surrounding text).
- **The wheel and the touchpad** (`webaxis.c`, per `wl_pointer.frame`): a mouse wheel (`axis_value120`, or
  `axis_discrete` before `wl_seat` 8) is one libwpe line event per notch, on the axis that moved only; a
  high-resolution wheel's partial notches add up across frames (a change of direction drops the partial one), at most
  10 lines per frame. WebKit 2.48 reads a line event's two values by their sign alone (`copysign`), so the 2D event
  lord-web sent before (with a 0 on the other axis) scrolled one line sideways too and pages didn't scroll. A touchpad
  (`axis_source` finger or continuous) is pixels times the scale, an exact 0 on the axis that didn't move, and
  `axis_stop` ends the gesture (a 2D event of 0, 0: WebKit's "ended" phase). Wayland's positive is down, libwpe's up.
- **Clipboard:** text both ways through `wl_data_device`: the session's selection is copied into WebKit's pasteboard
  while the window has the keyboard; what the page copies is offered to the session as plain text (and HTML when the
  page gave it). WebKit 2.48's pasteboard on WPE holds text only (its `Pasteboard` for libwpe reads no files or
  images and writes no image), and it reads its stored bytes back as Latin-1 while it writes them as UTF-8
  (`PlatformPasteboardLibWPE.cpp`, `readString`), so files, images and any text past ASCII go around it
  (`webwin_paste`, `webclip.c`, `webxfer.c`). All of it happens only at the person's own paste (Ctrl+V, Shift+Insert,
  the Edit menu's or the context menu's Paste); nothing is read for the page at any other moment:
  - **Pasting files:** when the session's selection offers `text/uri-list` (or `x-special/gnome-copied-files`; Files
    offers both for its copies, `WAYLAND.md`), the paste reads the list (at most 1 MB, non-blocking) instead of sending
    the key or running WebKit's Paste, and opens its local files with exactly the drop's checks and limits (`webdrop.c`,
    see "Drag and drop": `file://` without a host, the person's places, no hidden folder or file, no link on the
    resolved path, a regular file, at most 64 MB each and 20 files). The page gets a `paste` event on its focused
    element whose `DataTransfer` holds them as `File`s, `types` `["Files"]` and no text. When every file is refused, the
    selection's text is pasted as below (the list itself when the selection has no text). Before, the page got the
    files' paths as text. A copy made by an isolated LordOS app or a sandboxed (Flatpak) client reaches lord-web without
    its file lists: lord-server leaves `text/uri-list` and the other file-list types (`dndg_file_mime`) out of that
    selection's offer to a lord-web client and refuses them at `receive` (`wl.c`, `OfferRef.nofiles`, as for a drag's
    `DOffer.nofiles`), so it pastes as text only.
  - **Pasting text:** the session's text is read as `text/plain;charset=utf-8`, then `UTF8_STRING`, then
    `text/plain` (UTF-8 when it is valid UTF-8, else converted from Latin-1), then `STRING` (Latin-1, X11's rule);
    `TEXT` and `COMPOUND_TEXT` are never read, and a UTF-8 type that isn't valid UTF-8 is refused (`webclip.c`). It
    is mirrored into WebKit's pasteboard as UTF-8. Plain ASCII text is WebKit's own paste. Any other text would come
    out garbled from WebKit (a path in `Imágenes` as `ImÃ¡genes`), so lord-web pastes it itself: a `paste` event on the
    focused element with it as `text/plain` (from a script, so `isTrusted` is false), and when no listener prevents
    the default, the text is inserted at the caret with WebKit's `InsertText` editing command.
  - **Pasting an image:** when the session's clipboard offers `image/png` or `image/jpeg` and no text, the paste
    reads it (at most 32 MB, non-blocking) instead of sending the key, and the page gets a `paste` event
    on its focused element with a `DataTransfer` holding the image as a `File` (`image.png`), `types` `["Files"]`.
    WebKit's pasteboard is emptied when a selection with no text arrives (an image, a file list only), so it never
    pastes the text copied before it.
  - **Copying an image:** WPE 2.48 has no Async Clipboard API (`navigator.clipboard` is undefined), so a script at
    document start gives the page `navigator.clipboard.write` and `writeText` (and `ClipboardItem`) when they are
    missing. They post to lord-web (a script message handler), which takes text (at most 16 MB) or a PNG or JPEG
    (at most 32 MB, checked by its signature) only while the window shows the app's own page and the person pressed
    a key, clicked or touched it less than 5 s before (what browsers call user activation), and offers it to the
    session as that one type. The message handler lives in lord-web's isolated world (`lordos-web-xfer`), where only a
    bridge script runs, in the app's top frame and only in documents of its scope (the scripts' allow-list, made from
    the scope by `weburl_scope_pattern`): no script of the page's world, of any frame or site, can see the handler or
    forge its messages. The page's `navigator.clipboard` asks the bridge through events on the document whose detail
    is JSON text, and builds its own objects only (no page setter or replaced method sees what is written). At each
    message lord-web checks that the document's address (as the isolated world reads it), the main frame's committed
    one and the view's are inside the scope. Anything else rejects the page's promise.
  - **A copy the page handles itself:** many pages (chats among them) handle the `copy` or `cut` event, put their
    own text in the event's `clipboardData` (several messages with their sender and time, emoji pictures as text)
    and prevent the default; `document.execCommand('copy')` after a click with such a listener is the same. In
    WebKit 2.48 for WPE that data goes nowhere: WebCore commits it to the platform's `Pasteboard::writeCustomData`,
    which is empty in `PasteboardLibWPE.cpp`, so nothing reached the session (checked on VM 127 with a page that
    does this: the session's clipboard kept what it had). The isolated script (`lordos-web-select`, every frame)
    watches trusted `copy` and `cut` events: from the window's capture phase it adds a listener of its own to each
    node on the event's path, in both phases, so it runs after the page's listeners there and still sees the data
    when a listener stops the propagation (not `stopImmediatePropagation`). After the event, when the page prevented
    the default and left `text/plain` (or only `text/html`, whose text is taken), it posts the text and the HTML
    (message handler `lordosCopy`, that world only). lord-web offers them to the session (`text/plain` and
    `text/html`), at most 16 MB each, only less than 5 s after the person's own key, click or touch in the window,
    and writes the text into WebKit's pasteboard too, so a paste back into the page pastes it.
  - **Ctrl+C with the focus in a message box:** the isolated script decides by where the selection is, not by the
    focused element: Ctrl+C with the focus in an editable box but the selection on text the page shows runs Copy
    (WebKit's own key binding runs only where the selection can be edited).
  - **What the log says** (lord-web's stderr, the app's log; at most 20 lines in 10 s, sizes and reasons only, never
    what was copied or dropped): `page copy offered (N bytes[ and HTML], editing command|copy event|clipboard
    API|image/png)` and `page copy refused (...)` with the reason; for a paste `paste: N files from the clipboard
    accepted[; others refused: <reason>]`, `paste: no file from the clipboard accepted (N on its list, <reason>);
    pasting its text`, `paste: text (N bytes, <type>[; WebKit's own paste])`, `clipboard text refused (N bytes,
    <type>): not UTF-8` and `paste dispatched (N files, target TAG, a listener took it|no listener took it)`.
- **Drag and drop** (`webwin.c`, `webdrop.c`, `webxfer.c`): libwpe has no drag and drop and WPE's view no drop target,
  so lord-web is the target itself, and the page gets the drop from a script (WPE 2.48 cannot receive an external
  drop any other way). A drag over the window that offers `text/uri-list` (a Files drag, see `WAYLAND.md` "Drag and
  drop") or text is accepted as a copy. At the drop lord-web reads the list (at most 1 MB) and calls `finish`. Only
  local files are taken: `file://` URIs with no host (or `localhost`), percent-decoded, a regular file when opened (a
  link is followed), at most 64 MB each and 20 files; anything else on the list is skipped. Any app can start a drag
  with any list, so the file, where it really is (links resolved), must be in the person's places: under the home
  folder, `/media`, `/mnt`, `/run/media` or `/tmp`, with no hidden folder or file on the way (`~/.ssh`, `~/.config`
  and the like are never sent), and it is opened without following a link put anywhere on that path in between (`webdrop_place_ok`, `webdrop_open`:
  `openat2` with `RESOLVE_NO_SYMLINKS`, else one component at a time with `O_NOFOLLOW`); the page's `File` has the
  name of the file opened, not the list's. A drag started by an isolated LordOS app or a sandboxed (Flatpak) client
  offers lord-web no file list at all: lord-server leaves `text/uri-list` and the other file-list types out of that
  offer (`wl.c`, `DOffer.nofiles`, lord-web known by its pid), so the drop arrives as text only, like the drop on
  LordOS itself that such a drag never makes. lord-web reads the files
  itself and passes them in 1 MB pieces to a script in an isolated world of its own (`lordos-web-xfer`: the page's
  scripts can't see or change it), which makes `File` objects (name, a type from the name, the modification time) and
  `blob:` addresses. While the drag moves over the window, before any drop, the page's world gets `dragenter` on
  the element that comes under the pointer (then `dragleave` on the one it left), `dragover` on it at each move (one
  call at a time, the latest position kept), and `dragleave` when the drag leaves the window or its drop can't be
  handed over; their `DataTransfer` says `types` `["Files"]` for a file drag (`["text/plain"]` for text) and holds no
  data, as a browser's before the drop. That matters: a chat page shows its drop area only after `dragenter`, a
  render a frame later, and puts its only `drop` listener there, so when lord-web sent `dragenter`, `dragover` and
  `drop` together at the drop, the drop landed on the element under an area that didn't exist yet and nothing was
  sent (the area was then left on the screen). At the drop the script in the page's world waits two frames (at most
  150 ms), finds the element at the drop point again, sends `dragenter` (and `dragleave` to the old one) if the
  drag hadn't reached it, then `dragover` and `drop` with the files (in CSS pixels, the zoom taken into account). It has
  to be the page's world because in WebKit 2.48 a script-made `DataTransfer` lists no `"Files"` in `types`, which
  pages check, and only that world can mend it; the `blob:` addresses are revoked right after. A drop with no local
  file gives the page its text (`text/plain`, and `text/uri-list` for a list). One transfer at a time; none while the
  window shows another site. WebKit 2.48 takes only numbers, strings and dictionaries as a called function's arguments
  (an array is "Invalid parameter types"), so the files' addresses, names, types and times go to the page's world as
  one JSON text. Checked on VM 127 (2026-11-11): a JPEG dragged from Files onto a page's drop zone arrives as
  `hacker.jpg image/jpeg 6753` with `types` `["Files"]`; and on a page whose drop area appears a frame after
  `dragenter` and is the only drop target, a JPEG and a text file arrive there, and the area goes away when the drag
  leaves without dropping. The events are made by a script, so `isTrusted` is false (no embedder can make trusted
  ones). The log says `drag over the page (files|text, first target TAG)`, `drop received (text/uri-list, N lines, M
  local files accepted; the others refused: ...)` or `drop refused: <reason>`, and `drop dispatched (N files, target
  TAG, a listener took it|no listener took it)`.
- **Editing** (`webselect.c`, `webedit.c`, `webengine.c` `web_edit`):
  - **Selecting with the mouse:** WPE WebKit 2.48 is built without `ENABLE_DRAG_SUPPORT`, and in WebCore that switch
    also holds the code that extends a selection while the left button is dragged, so dragging across text selected
    nothing. A press still places the caret, a double click selects a word, a triple click the paragraph and
    Shift+click extends (WebKit's own, from the presses' times and places). Dragging is lord-web's: a script in an
    isolated world (`lordos-web-select`, every frame, the page can't see or change it) takes the caret WebKit placed at
    the press as the anchor and extends the page's own selection to the pointer on each move with the button held
    (`document.caretRangeFromPoint` for text and editable content; inside a text field, which that can't reach, the
    character under the pointer measured with the field's font, padding, scroll and wrapping). Not on a link, an image,
    a draggable element or `user-select: none`, and not when the page prevented the press's default. A motion also
    names the button held (as GTK's does), so a page sees `which` 1 during a drag.
  - **Copy from the keyboard:** WPE's WebEditorClient runs its key bindings only where the page can be edited
    (`handleKeyboardEvent` returns when `!canEdit()`), so Ctrl+C on a message's text copied nothing. The isolated
    script reports Ctrl+C, Ctrl+Insert and Ctrl+A outside a text field or editable content (trusted, not prevented
    by the page's listeners) and lord-web runs Copy or Select All.
  - **Clipboard text:** what WebKit's pasteboard gets from the session is written with zeroed strings: libwpe's
    `wpe_pasteboard_string_initialize` leaves a string that already has data alone, and the stack garbage it kept
    made pasting text copied in another app do nothing or crash lord-web.
  - **The Edit menu:** a web app's window gets Undo, Redo, Cut, Copy, Paste and Select All in the menu bar's Edit
    menu (with Speech). The shell calls `lord.webapps.edit(app, profile, command)`; lord-server sends
    `org.lordos.WebApp1.Edit(command)` to that lord-web's bus name (no reply). lord-web asks the bus for the caller's
    pid (`GetConnectionUnixProcessID`, from the socket's credentials) and runs the command only when it is the
    lord-server that started it (its parent): WebKit's editing command (`Cut`, `Copy`, `Paste`, `SelectAll`), Paste
    of an image going the image way. Undo and Redo go to the page as Ctrl+Z and Ctrl+Shift+Z: WPE 2.48's page client
    drops every edit command (no undo stack: `canUndoRedo` is always false), so only a page with its own history (a
    rich text editor) undoes; a plain text field can't, from the menu or the keys. What Copy or Cut put in WebKit's pasteboard is offered to the
    session at once (the window may not have the keyboard while the menu is open). The commands are a closed list
    (`webedit.c`), checked on both sides.
  - **The context menu:** WPE 2.48 never asks its embedder for a menu (the view's `context-menu` signal doesn't
    come) and, once a right click's default asked WebKit for one, the view took no more mouse input. So the same
    isolated script prevents the default of every trusted `contextmenu` event and, when the page's own listeners
    (on the element or the document) left it alone, tells lord-web what is under the pointer: an editable field, a
    selection, a link's address, an image's. lord-web takes it only right after a click in the window and asks
    lord-server for LordOS's own menu: `org.lordos.WebApps1.Menu(app, profile, items) → item`, the items being names
    from the closed list (`cut`, `copy`, `paste`, `selectAll`, `copyLink`, `openLink`, `copyImage`), never text.
    lord-server checks them and the caller's pid as for Ask, and the shell shows the menu at the pointer with its own
    words (a press anywhere else closes it and goes no further). Copy Link Address offers the link's address as text;
    Open Link in Browser goes to OpenURI (web, mail and phone links only); Copy Image reads the image with the page's
    origin in lord-web's isolated world (made a PNG unless it is a PNG or a JPEG, at most 32 MB) and offers it as an
    image.
  - **The app menu and Help:** Settings… opens Settings → Web Apps on that app; in Lord Flow, Browser identity (Chrome on
    Linux, Edge on Linux, Firefox on Linux, LordOS (WebKit)) the window's profile's `userAgent` ("Browser identity", below); Help starts with "<app> on the Web",
    the app's origin in the browser.
- **Light and dark** (`webscheme.h`, `webscheme.c`, `webscheme.js`, `webdark.c`; since 0.8.8.8): a web app
  follows the desktop's light or dark mode as far as WPE WebKit 2.48 lets it.
  - **What WebKit lacks:** WPE 2.48's public API has no way to set the page's `prefers-color-scheme`, so every page
    sees light: `matchMedia('(prefers-color-scheme: dark)').matches` is false and CSS `@media (prefers-color-scheme:
    dark)` rules never apply.
  - **How lord-web learns the mode:** lord-server knows the desktop's mode from the theme (`lord.portal.set`, the same
    call that feeds the Settings portal to Linux apps). It starts lord-web with `LORDOS_WEB_SCHEME=light` or `dark`
    (missing or anything else is light) and, when the mode changes, sends `org.lordos.WebApp1.SetColorScheme(s
    scheme)` to each running lord-web's bus name (no reply). The word is `light` or `dark` only, refused otherwise
    (`InvalidArgs`); lord-web asks the bus for the caller's pid and applies it only when it is the lord-server that
    started it, as for `Edit`. A mode change between a lord-web's start and the moment it takes its bus name would
    reach a name nobody owns yet, so lord-server watches `NameOwnerChanged`: when the name of one of the lord-web it
    started gets an owner, that lord-web gets `SetColorScheme` with the mode now (lord-web ignores a mode it already
    has).
  - **matchMedia:** a script of lord-web's (`webscheme.js`) runs at document start in every frame, in the page's own
    world (the page's scripts must see its answers). A query that names `prefers-color-scheme` is rewritten so the real
    `matchMedia` answers it as the desktop would: when WebKit's answer and the desktop's differ, every
    `(prefers-color-scheme: dark)` becomes `(prefers-color-scheme: light)` and the other way round, and the rest of the
    query (`not`, `and`, `only`, commas, other features) stays WebKit's. The page gets its own `MediaQueryList`
    (`instanceof MediaQueryList`, an `EventTarget`) with `matches`, the original query's `media`, `onchange`,
    `addEventListener`/`removeEventListener` and `addListener`/`removeListener`; it sends `change` when the desktop's
    mode flips (only when its answer changes) and when the real list changes (a width). Any other query gets WebKit's
    own list untouched. A list is kept alive only while the page has `change` listeners on it, as in a browser: a
    `{ once: true }` listener that ran and a `{ signal }` listener whose signal aborted are let go. A new mode replaces the script for the next loads and calls its hook (a random name per
    process) in the main frame, which passes it to same-origin frames; a frame of another origin gets it with its next
    load. The script has no privileges and no message handler: a page that calls the hook only changes what it sees
    itself, and nothing goes from the page to lord-web or lord-server.
  - **Each app's Appearance** (`colorScheme` in the registry; built in 0.8.8.9, held back until WPEPlatform and on
    since 0.8.9.6.2). lord-web on WPEPlatform (WPE 2.54) sets `WPE_SETTING_DARK_MODE` on its display with the word
    lord-server sends (`webback_set_dark`, called from `webdark.c` at start and on every `SetColorScheme`), so CSS
    `@media (prefers-color-scheme)` rules follow it as well as `matchMedia`; a site with its own theme setting
    (WhatsApp, for example) still follows that setting. Settings → Web Apps offers Automatic, Light and Dark, and the
    app menu has the Appearance submenu (`webAppearanceItem`). `WEBSCHEME_PER_APP` is 1 in `webscheme.h` and
    `COLOR_SCHEMES_AVAILABLE` lists all three. A lord-web built on the legacy API (WPE 2.48, `webback_set_dark` does
    nothing there) only has webscheme.js's matchMedia answer. As designed, for then: Automatic (the default), Light or Dark, per app (every profile of it). lord-server picks the word it sends each app
    (`webscheme_for_app`: the app's own for Light or Dark, else the desktop's), in the environment at start, in
    `SetColorScheme` when the desktop's mode changes and when a lord-web's bus name appears. lord-web reads nothing
    new: it applies the word it gets exactly as before, so the script, the `change` events and the dark background
    while loading all follow the app's choice, and the page's content is never changed. Changing it
    (`lord.webapps.setColorScheme(app, value)`, desktop only, not in the broker) writes the registry and sends
    `SetColorScheme` with the new word to each running profile of the app (`webask_app_scheme`), so **open windows
    follow at once**; a page that only reads the scheme at load sees it on its next load. On WPEPlatform it reaches
    CSS `@media` rules too; on the legacy API only scripts that ask `matchMedia`. Where: Settings → Web Apps, the app's page
    (Appearance, a three-way choice with a hint for each), and in Lord Flow the app menu of the web app's window
    (Appearance ▸ Automatic, Light, Dark, the current one checked). Lord Classic flattens submenus, so there it is only
    in Settings. Tests: `build/webreg-test` (the start environment, `webmgr_scheme_of`), `build/webapp-test` (the key
    read, written only when not `auto`, anything else refused;
    `webscheme_for_app` for each pair; while held back, a stored light or dark follows the desktop),
    `tests/webapps_model_test.mjs` (`colorSchemeOf`, `shownColorScheme`). Not yet checked on screen on the development
    VM.
  - **No white flash:** in dark mode the view's background is the desktop's dark window color (`#1f1f23`) from a
    load's start to its end, so nothing white shows before the page paints; then it is WebKit's white again, so a page
    without a background of its own reads as in a browser.
  - **The limit:** a page that decides only through CSS `@media (prefers-color-scheme)` rules stays light until a newer
    WebKit lets lord-web set the scheme. Rewriting those rules from the script was weighed and left out: style sheets
    of another origin (most sites serve theirs from a CDN) can't be read, constructed and shadow-root sheets, `<link
    media>` and `<picture>` sources would each need their own path, and CSS-in-JS libraries that keep rule indices
    would break. A site with a theme setting of its own can be set by hand: for example the messaging app's
    Settings → Theme → Dark keeps it dark whatever the desktop's mode (the Store's page of that app says so, limitation
    `theme`).
  - **Checked on VM 127 (2026-11-11):** a local test page that prints `matchMedia('(prefers-color-scheme: dark)')
    .matches` and logs its `change` events showed `true` when the app started in dark mode and `false` when it started
    in light; switching Dark Mode in Control Center flipped it at once, with one `change` each way, and its same-origin
    frame followed; a reload after a switch started with the new mode; the CSS `@media` rule on the same page stayed
    light (the limit above); a `SetColorScheme` sent with `busctl` from an ssh shell was refused ("not lord-server")
    and a word other than `light` or `dark` got `InvalidArgs`. The messaging app's sign-in page (QR code) stays light
    in dark mode; whether its "System default" theme follows after signing in was not tested (it needs a phone).
    After the review fixes: the app launched in dark with the mode switched to light right away got `SetColorScheme
    ("dark")` from lord-server one millisecond after its name appeared (seen with `dbus-monitor`), then `"light"`, and
    showed light; a later switch to dark flipped the page live with one `change`, the frame following.
  - **Tests:** `tests/webapp_test.c` (the two words, the environment's value, the script's and the hook's text, bad
    keys refused), `tests/webscheme_test.mjs` (the script against a stand-in `matchMedia`: plain, negated, combined
    and listed queries, `media`, listeners and `onchange`, no event when the answer stays, frames, an engine that
    already answers dark, `once` and `signal` listeners let go) and `tests/webreg_test.c` (`LORDOS_WEB_SCHEME` absent
    until the theme tells the mode, `dark` and `light` after it, absent again for any other value).
- **Browser identity** (`webpage_user_agent`; the registry's per-profile `userAgent`; 0.8.8.9):
  the browser a profile's pages are told it is, one of four, all Linux ones (the maintainer's choice: no other
  system's identity), picked from a closed list in Settings (never typed), in this order. `<version>` is always
  `LORD_VERSION`, the installed LordOS's, never written by hand.
  - `chrome`, **Chrome on Linux**, the default (a profile without the key):
    `Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/158.0.0.0 Safari/537.36`,
    exactly what desktop Chrome on Linux sends, nothing added. Chrome's reduced user agent says `Linux x86_64` on
    every Linux, an ARM one too, so this string is the same on `aarch64`. Since the calls work (in development,
    "Calls" below) Chrome is also Chrome in the two other places sites look, from the same table
    (`webua.h`, `webua.c`; Chrome's whole version `WEBPAGE_CHROME_FULL`, `158.0.8127.36`, next to the string's major in
    `webpage.h`): the **User-Agent Client Hints** on the page's requests and **`navigator.userAgentData`**. Both are
    exactly what Chrome 158 on Linux sent and answered on 2026-11-25 (a throwaway profile against a local echo server):
    - every request to a secure origin (https, or http on localhost) carries `Sec-CH-UA: "Chromium";v="158",
      "Not.A/Brand";v="24", "Google Chrome";v="158"`, `Sec-CH-UA-Mobile: ?0` and `Sec-CH-UA-Platform: "Linux"`. The
      third brand ("GREASE") and the order change with each major, worked out as Chromium does
      (`webua_brands`; checked for 120, 124 and 158);
    - after the app's own site sends `Accept-CH` (read on the main frame's responses inside the app's scope and on its
      origin only, `webcall_hints_site`, kept in the profile's `state.json`, `clientHints`, as Chrome keeps it; another
      site's, a sign-in's or a redirect's, is neither kept nor logged, so no trail of other sites is written), that
      site's own requests also carry the ones it named:
      `Sec-CH-UA-Full-Version-List` (`"Chromium";v="158.0.8127.36", "Not.A/Brand";v="24.0.0.0", "Google
      Chrome";v="158.0.8127.36"`), `-Full-Version`, `-Platform-Version` (`""`), `-Arch` (`"x86"`; `"arm"` on
      `aarch64`), `-Bitness` (`"64"`), `-Model` (`""`), `-WoW64` (`?0`), `-Form-Factors` (`"Desktop"`). Another site's
      requests from that page carry only the first three (no Permissions-Policy delegation, a limit);
    - `navigator.userAgentData` in a secure context (brands, `mobile` false, `platform` "Linux", `getHighEntropyValues()`
      with the same values in a dictionary's order, `toJSON()`), the `NavigatorUAData` interface (not constructible), and
      `navigator.vendor` "Google Inc." (WebKit says "Apple Computer, Inc."). `navigator.platform` stays WebKit's
      ("Linux x86_64" on a PC, as Chrome; "Linux aarch64" on a Pi). Not in workers: the script reaches frames only.
    The headers come from **`lord-web-ext.so`** (`webext.c`), WebKit's web process extension, because no API of the
    UI process can add a header to every request: WebKit loads it only from `lord-web-ext/` next to lord-web
    (`/usr/lib/lordos/bin/lord-web-ext/`, a folder lord-web names itself from `/proc/self/exe`), inside the web process's
    sandbox, and it only adds those headers in `send-request`, from what lord-web tells it (Chrome or not, the machine,
    each site's hint bits, all checked again there). The script is `webua.js`, in the page's world with no privileges
    and no message handler (`webhints.c`); its functions read as native code, as Chrome's do. Other identities get
    neither (Firefox sends no hints; Edge's own brands and LordOS's are not made up), and switching identity adds or
    removes both at once. Checked by `build/webua-test` (the table, GREASE, every header, Accept-CH, the state file)
    and `tests/webcalls_check.py` (the real engine against Chrome 158's captured headers and values).
  - `edge`, **Edge on Linux**: `Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko)
    Chrome/158.0.0.0 Safari/537.36 Edg/158.0.0.0`, what desktop Edge on Linux sends, without LordOS's token. For the
    office, mail and team chat sites of Edge's own maker, which expect it.
  - `firefox`, **Firefox on Linux**: `Mozilla/5.0 (X11; Linux <machine>; rv:160.0) Gecko/20100101 Firefox/160.0`.
    Firefox names its processor (`x86_64`, `aarch64`); a machine name that isn't `[A-Za-z0-9_]` becomes `x86_64`.
  - `lordos`, **LordOS (WebKit)**: the string every app had before, `Mozilla/5.0 (X11; Linux <machine>)
    AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Safari/605.1.15 LordOS/<version>`: Linux, WebKit's own
    (frozen) AppleWebKit and Safari tokens, LordOS's version. For a site that misbehaves with the others.

  A version that isn't `[A-Za-z0-9.+~-]` drops the LordOS token. The majors are
  `WEBPAGE_CHROME_MAJOR`, `WEBPAGE_EDGE_MAJOR` and `WEBPAGE_FIREFOX_MAJOR` (`webpage.h`); docs/RELEASING.md bumps
  them each release. JS and the page never pass a string: `lord.webapps.setUserAgent(app, profile, value)` (desktop
  only, not in the broker) takes one of the four words, writes the registry and, when that profile runs, sends its lord-web
  `org.lordos.WebApp1.SetUserAgent(word)` (`webask_user_agent`). lord-web accepts it only from the lord-server that
  started it (the pid check of `Edit` and `SetColorScheme`), maps the word to the string itself, sets WebKit's
  `user-agent` setting and loads the app's page again: a document keeps the `navigator.userAgent` it started with,
  and requests after the change already carry the new one. When the window shows another site (a sign-in half done),
  it is not reloaded and gets the new string with its next load. lord-server sends the profile's word again when a
  lord-web's bus name appears, so a change made while it started is not lost; the same word is ignored. At start
  lord-web reads the key from the registry like the rest; the probe ("Adding an app from an address") always asks as
  Chrome on Linux. Where: Settings → Web Apps, the app's page, a *Browser
  identity* section with one pop-up per profile (with one profile, "Identify as"), a hint for each choice and a
  footer: LordOS still uses its own engine (WebKit), Chrome, Edge and Firefox are names of their owners, some sites
  check the browser and refuse it when something doesn't match (then choose another identity, such as LordOS
  (WebKit)), and a site that already linked this device keeps the name it saw. The texts
  name no company or service (the brand check): Edge's hint says "Edge's own maker". In Lord Flow, the
  app menu of the web app's window (Browser identity ▸ the four, for that window's profile; Lord Classic flattens
  submenus, so there it is in Settings only).
  - **Why Chrome by default, and why the old linked name stays** (2026-11-15): a messaging web app listed LordOS on
    the phone's "Linked devices" as the other system's own browser. That site names a linked computer after the
    browser and system it sees **when it is linked**, and keeps that name. WebKit 2.48's site-specific quirks
    (`UserAgentQuirks`) hand that site's domain another system's browser string *whatever the user-agent setting*,
    (the domain is in the `libWPEWebKit-2.0` 2.48 binary's list), and lord-web turned quirks off on an app's own
    pages only on 2026-11-11, a day after its first build: a device linked with a build before that was linked with
    the quirk's string, and the phone keeps showing it. With
    quirks off, the old LordOS string still says "Safari" on Linux, a browser that doesn't exist there, which sites
    read in their own ways. Chrome on Linux is what most sites expect from a Linux computer. A device linked before
    the change keeps its old name until it is unlinked on the phone and linked again.
  - **Chrome exactly as Chrome, and no "experimental"** (the maintainer's tests, 2026-11-15, with a messaging web
    app): LordOS (WebKit), Firefox on Linux and Edge on Linux link and show on the phone as Safari (Linux), Firefox
    (Linux) and Microsoft Edge (Linux). Chrome's string with LordOS's token after `Safari/537.36` (the first
    `chrome`) was refused ("works with Google Chrome 100 or later"), and LordOS alone (`experimental`) as an
    unsupported browser. Edge's string is Chrome's plus `Edg/158.0.0.0` and works, so the site reads the last
    token as the browser: `chrome` now sends Chrome's string exactly, nothing added, and `experimental` was removed
    (the UI, both C lists, `lord.webapps.setUserAgent`, the model). A profile that has it is read as `chrome` by
    the registry parser, lord-server and the model alike; its file isn't rewritten for it and still loads (an
    unknown value must never make lord-web refuse the app).
  - **WebKit's site-specific quirks** stay as before: off on the app's own pages (a quirk would replace the string
    the person chose) and on for any other site in the window (a sign-in), where WebKit's fixes for big sites help.
  - **The limit:** a Chrome, Edge or Firefox string doesn't make WebKit one of them. A site that trusts the string may
    send code only those engines run; such a site is what *LordOS (WebKit)* is for. Checked 2026-11-11 (the LordOS
    string): the messaging app's sign-in page loads with quirks off, without another system's download offer. The
    other strings are not yet checked on screen on the development VM.
  - **Tests:** `build/webapp-test` (the four strings exactly, Chrome's exactly Chrome's with no LordOS token for
    any machine or version and `Safari/537.36` last, Chrome's and Edge's the same on every machine, the LordOS
    token from the version given and dropped for a strange one, Firefox's processor and its fallback, unknown kinds
    refused, the words with `experimental` not among them; the per-profile key read, written only when not
    `chrome`, `experimental` read as `chrome` and not written back, twelve bad forms refused, refused at the app's
    level), `build/webreg-test` (`webmgr_user_agent_of`, an old file with `experimental` still loads, reads as
    Chrome and isn't rewritten, a new profile is Chrome again) and `tests/webapps_model_test.mjs` (`userAgentOf`,
    `experimental` shown as `chrome`, `USER_AGENTS`).
- **Unread count:** a title that starts with `(N) ` (N 1..9999, `(99+)` is 99; `webpage_title_badge`) on the app's
  own pages is the app's unread count; lord-web sends it to lord-server with `SetBadge(app, profile, count)` on
  `org.lordos.WebApps1` (0: none) when it changes, without waiting for an answer. WPE 2.48 has no Badging API for an
  embedder (`navigator.setAppBadge` is off and reaches no signal), so the title is the only source.
- **Navigation:** an address inside `scope` stays; a link clicked to an address outside it, a new window outside it
  and `mailto:` or `tel:` go to OpenURI; a new window inside it loads in the same window. A redirect outside (a sign-in
  on another site) stays, and the title then starts with that site's host. No WebKit context menu (LordOS's own,
  above), no developer extras, no windows opened without a click, no page cache, TLS errors fail.
- **Engine:** the profile's own `WebKitNetworkSession` (`data/`, `cache/`, `cookies.sqlite`, no credential storage of
  WebKit's own), memory pressure settings (the computer's memory up to 3 GB as the base: rebuildable caches dropped
  past a third, ~1 GB, decoded images and compiled code past half, ~1.5 GB, polled every 30 s; see "Performance"), the
  document cache model.
  WebKit 2.48 has no switch for its sandbox: it is always on, and lord-web refuses the variable that turns it off.
- **Closing:** without `background`, closing the window quits. With it the window is hidden (WebKit is told it is not
  visible and throttles); the Dock, the launcher or a notification shows it again, and with `tray` a
  StatusNotifierItem with the app's icon offers Open and Quit too. When the web process crashes it reloads once; a second crash within 30 s
  quits.

## Adding an app from an address ("probe")

`lord.webapps.probe(url)` starts `lord-web --probe` with the address on fd 3 (one line, at most 2 KB, `https:` only):

- It loads the page in a throwaway profile (a temporary folder, removed after), without a window.
- It reads the page's web app manifest (`<link rel="manifest">`, else one kept at a usual address of the page's own
  origin) and the icons it lists, and the page's own icons.
- It answers one JSON line on fd 3: `{ "ok", "name", "startUrl", "scope", "origin", "themeColor", "icons": [{ "src",
  "sizes", "type", "purpose" }], "manifest": true|false }`, then exits.
- It is given 20 s; lord-server kills it after that.

Nothing is installed by a probe. The person sees the real origin and confirms. Then lord-server writes the registry
file and saves the icon, chosen and fetched by `lord-web --probe-icon` as a second step, into the registry folder,
after checking it is a PNG or an SVG of at most 1 MB. lord-server ranks the answer's `icons` (`https:` only): PNGs
(the largest up to 512 px, else the smallest above), then SVGs, then the same two for icons that are only
`maskable`; a `monochrome`-only icon is never taken. An icon is a PNG or an SVG by its type (`image/png`,
`image/svg+xml`, or `image/svg` as WhatsApp writes it) or, without one, by its extension, and an address that ends in
another image's extension (`.webp`, `.ico`, `.jpg`, `.gif`, `.avif`, …) is skipped whatever its type says (WhatsApp's
manifest labels WebP files `image/png`). The second step tries the best three in that order until one really is a
PNG or an SVG; it starts right after the first (the sheet then shows the icon), and all its tries share one 20 s.

How the two steps are built (`server/src/web/webprobe.c`, `webmanifest.c`, `webargs.c`):

- **The request** on fd 3 is one line of at most 2 KB ending in a newline, nothing after it, an `https:` address (or
  `http:` on `localhost`, `127.0.0.1` or `[::1]`, for tests). Anything else is answered `web.probe.request` without
  loading anything. lord-web closes every other inherited fd and none of WebKit's children get fd 3.
- **`--probe`** loads the page in an ephemeral network session (nothing written to disk) with WebKit's sandbox, no
  window (its frames are given back at once), no permission, no new window, no download. 1.2 s after the load it runs a
  script in an isolated world (the page's scripts can't see or change it) that reads the `<link rel="manifest">`, the
  `theme-color` meta, the title, the `<link rel="icon">`s and apple-touch-icons, and fetches the manifest with the
  page's credentials rules. A page that links no manifest (WhatsApp's sign-in page, where a signed-out probe ends)
  gets one looked for on its own origin at `/manifest.webmanifest`, `/manifest.json`, `/site.webmanifest` and
  `/data/manifest.json` (WhatsApp's): the first that answers 2xx, stayed on the page's origin and is a JSON object with
  `icons` or `name`. lord-web then checks it all against the address WebKit really loaded (after redirects): the origin is the
  loaded page's, a `start_url` on another origin becomes the page itself, a `scope` that doesn't hold the start address
  becomes the start address's folder, the name is cleaned (no control characters, spaces folded, at most 128 bytes),
  colors become `#rrggbb`, at most 16 icons with `https:` addresses: the manifest's first, then the page's own, each
  with its `purpose` when it has one.
- **Where it may go:** the probe keeps the request's scheme and place. A redirect or a navigation of the page from
  `https:` to `http:`, or from a public host to this computer or a local network (localhost and `*.localhost`,
  `*.local`, 127/8, 0/8, 10/8, 100.64/10, 172.16/12, 192.168/16, 169.254/16, `::1`, fc00::/7, fe80::/10, IPv4 written
  the short, hex or octal way browsers accept), is refused (`web.probe.notHttps` for the scheme, `web.probe.load` for
  the place), and the page and the icon are read only when the address they ended on passes the same rule against
  the request (`weburl_probe_hop_ok`, `weburl_probe_final_ok`). lord-server never sends a public page's local icon to
  `--probe-icon`. Names count too: a single label (`router`) and the local networks' suffixes (`.lan`, `.home`,
  `.home.arpa`, `.internal`, `.intranet`, `.localdomain`, `.corp`) are private, and lord-web resolves every other name
  itself (3 s at most, again at every check: nothing is cached) and classifies every address it gets
  (`weburl_addrs_class`): one private address makes the name private, and a name that doesn't resolve is refused. The
  rule holds for every hop (`weburl_probe_step`): a request whose own name is private (the person typed a local
  address) may stay there, but once any hop was public (the request, a redirect, a navigation, a frame), every next one
  and the address the page or icon ended on must resolve to public addresses only, so local → public → local is
  refused too.
- **What is not closed:** DNS rebinding between our check and WebKit's own connection. WebKit resolves the name again
  when it connects, and a name can answer a public address to us and a private one to it. This remains possible for
  the probe only (an installed app never goes through it), which runs in WebKit's sandbox, in an empty ephemeral
  session with none of the person's cookies or credentials, and reads only a manifest and an icon. Pinning the
  address at connect time (a proxy of lord-web's own for the probe's network session) is not done yet.
- **`--probe-icon`** takes one icon address the same way and opens it as a page is, in a view without a window, with
  the same ephemeral session, sandbox and Chrome identity as `--probe` and every hop checked the same way; the
  answer, when it is 2xx, becomes a download (anything else is `web.probe.load`). A download WebKit starts by itself
  goes out with WebKit's own identity and fetch metadata, and WhatsApp's server answers that with 400. It downloads
  the icon into a temporary folder (removed after) and
  answers `{"ok":true,"type":"image/png"|"image/svg+xml","size":N}` and a newline, then exactly N bytes. A PNG needs its
  signature and an `IHDR` chunk; an SVG is text whose first element is `<svg>`. Anything else is
  `web.probe.iconType`; over 1 MB, `web.probe.iconTooBig` (the download is stopped there).
- **Errors** are `{"ok":false,"error":"<code>"}`: `web.probe.request`, `web.probe.load`, `web.probe.tls`,
  `web.probe.notHttps` (the page ended on an address that isn't), `web.probe.page`, `web.probe.timeout` (lord-web
  gives up at 18 s, before lord-server's 20), `web.probe.iconType`, `web.probe.iconTooBig`. Each probe runs in the
  scope `lordos-webprobe`.
- **Memory** (2026-11-10, the main machine, `tests/web_probe_test.py --pss` on a small local page): 224 MB PSS at the
  peak for lord-web and its WebKit processes together, about 11 s for the test's nine probes.

## The catalog (`assets/webapps/catalog.json`)

37 apps (2026-11-11), here by category. The file's order is the Store's: its first four (whatsapp, telegram, element,
discord) make Discover's banner. Outlook's address moved from `outlook.live.com` to `outlook.office.com/mail/`, the one
that was probed.

| Category | Apps (`id`) |
|---|---|
| Internet (`Network`) | whatsapp, telegram, element, discord, teams, gchat, messenger, instagram, x, pinterest, meet |
| Office (`Office`) | outlook, notion, gmail, calendar, drive, docs, sheets, keep, m365, todo, todoist, trello, icloud, chatgpt, claude, gemini, copilot |
| Audio & video (`AudioVideo`) | youtube, ytmusic, spotify, applemusic |
| Graphics (`Graphics`) | photos, canva, excalidraw, photopea |
| Utilities (`Utility`) | uber |

- **How they got in:** every one passed `lord-web --probe` on 2026-11-11 (it loaded and gave a name). Only WhatsApp is
  `tested` (`verified` 2026-11-11: the maintainer used it with two accounts); the others say "not tested yet" until
  someone checks them on LordOS. Left out: one video editor that answers "unsupported browser" to WPE WebKit and one
  coffee shop's site that no longer loads.
- **`startUrl` is the app's own address**, never the marketing page or the sign-in page a signed-out probe lands on
  (`mail.google.com`, `outlook.office.com/mail/`, `to-do.office.com/tasks/`, `app.todoist.com/app/`…). A redirect to
  a sign-in on another site stays in the window (above, "Navigation"), so the first start signs in there and comes
  back. Some services then settle on another origin (`outlook.office.com` → `outlook.cloud.microsoft`,
  `copilot.microsoft.com` → `copilot.com`, `www.pinterest.com` → a country's subdomain, `www.notion.so` →
  `www.notion.com` when signed out): the page works, but a link clicked there counts as outside the scope and opens in
  the browser. Worth checking when each app is tested.
- **`scope` is the origin plus `/`**, except where two apps share an origin: Google's Docs and Sheets are
  `https://docs.google.com/document/` and `https://docs.google.com/spreadsheets/`, so a spreadsheet linked from a
  document opens in the browser rather than inside Docs (the same origin also serves presentations and forms, which
  neither app should keep). `tests/webapps_model_test.mjs` checks that no app's start address falls inside another
  app's scope and that no two apps share a scope.
- **Descriptions are generic** (`webapps.desc.*`: "Music streaming", "Email", "Team chat and spaces"…), translated in
  every language and free of brand names (`tools/brand-check.mjs`); `name` and `publisher` are data.
- **Limitations:** `drm` for the two music services whose catalogs are protected (their songs likely won't play);
  `calls` where calls need WebRTC that WPE WebKit may not give (WhatsApp, Discord, Teams, Messenger, Meet); `push` for
  the apps people keep for their notifications (chat, social, mail, calendar, notes and tasks), which arrive only while
  the app runs; `theme` where the site's CSS-only dark mode stays light and the site has a theme setting of its own
  (WhatsApp).
  `background` is on for chat, mail, calendar and music (closing the window keeps the music playing).
- **Icons** are LordOS's own letter tiles, never the service's logo: 128×128 RGBA PNG, a rounded square (radius 24)
  in a color close to the service's, its first letter in white DejaVu Sans Bold with a 50 px cap height from y = 39.
  The letter is the product's own word when the name starts with its company's family name ("Google Drive" → D,
  "Microsoft Teams" → T, "YouTube Music" → M). They are the shipped fallback when an app's own icon isn't fetched.

## The Store: Discover, the first install and an app's page

- **Where web apps show:** the Web apps section, a banner and a shelf in Discover (the catalog's first apps; "See all"
  opens the section), Discover's search, and their category's page. Every card has a "Web app" tag, so web apps are
  told apart from LordOS apps and Linux apps (`apps/store/webapps.js`).
- **The section's Settings** (in development; the latest release is 0.8.9.5; asked by the maintainer on
  2026-11-23): the second column is Installed, **Settings**, then the categories. Settings has what sat above the shop:
  what a web app is, whether LordOS's web engine is installed yet (the first Get installs it), and Settings → Web apps
  for each app's profiles, permissions and space (`WebSettings` in `apps/store/webapps.js`). The shop opens on "Add from
  an address" (an action, so it stays) and the catalog; the search field says "Search web apps" and searches only the
  catalog. Web apps have nothing to fetch, so the title bar has no update icon.
- **The section's shop and its Installed** (in development; the latest release is 0.8.9.5; asked by the maintainer
  on 2026-11-23): the Web apps section opens on its shop (the subtitle, "Add from an address" and the catalog's
  cards); its second column starts with **Installed** (with how many are installed), then its categories. Installed
  lists the installed web apps with See more, Manage (Settings → Web apps) and Open, filtered by the search (name or
  origin); the shop no longer lists them above the catalog (an installed app's card says Open). `WebInstalled` in
  `apps/store/webapps.js`, the column in `apps/store/origincol.js`. The general Store → Installed lists the web apps
  with every other installed app and has a **Web apps** filter (`apps/store/installedmodel.js`).
- **The first web app installs the engine first.** Without `lordos-web` the list is still shown (only "Add from an
  address" waits, since the probe is lord-web's). Get / Install says LordOS's web engine is needed once and shows the
  package plan's download and disk size (`lord.pkg.plan({ install: ['lordos-web'] })`); its Install runs the Store's
  own package job (`pkg-install lordos-web`: the administrator's password, the progress) and, when the job ends well
  and lord-server finds `lord-web`, installs the app with no other click. From the second app on there is no engine
  step. The steps are `getReducer` in `sdk/webappsmodel.js` (`apps/store/getweb.js`; `tests/webapps_get_scene.mjs`).
- **An app's page ("See more", `apps/store/webdetail.js`)**, laid out like a package's: Install / Open / Remove and the
  tag; the facts row (version "always up to date", the day LordOS tested it, its data on disk, its memory); the
  description; publisher, the verified https origin, where it came from (LordOS's catalog or an address), the test,
  the engine ("LordOS Web (WPE WebKit <version>)", the installed `libwpewebkit-2.0-1` from the package index, no
  program run), the permissions with their state (a link to Settings → Web Apps), the profiles (each one's data and
  memory, Open, New profile), background mode and the known limitations.
- **The trademark notice** (0.8.8.9; `apps/store/trademark.js`, `tests/store_trademark_scene.mjs`):
  under the Web apps section's catalog and at the end of every app's page, a dimmed note in the person's language
  (`webapps.store.trademark`): the product names, logos and icons shown are trademarks of their owners, used only to
  identify the services; LordOS is not affiliated with, endorsed or sponsored by them; and LordOS doesn't change the
  sites, it runs them as they serve them. The same notice as lordos.org's.
- **The catalog's data for that page** (all optional, checked by `tests/webapps_model_test.mjs`): `publisher`;
  `verified` (`YYYY-MM-DD`, the day LordOS last checked the app on lord-web: left out until it is, and the page says
  "not tested yet"); `typicalMemoryMB` (one profile's Pss measured on LordOS: WhatsApp's 810 MB from the first check);
  `limitations` (keys of `webapps.limit.*`: `drm`, `calls`, `push`, `theme`). lord-server's own copy ignores them.
- **Memory now:** `lord.webapps.memory(app)` (read-only) sums `Pss` from `/proc/<pid>/smaps_rollup` of each running
  lord-web of the app and of every process under it (WebKit's web and network processes, bubblewrap), by profile:
  one pass over `/proc` for the parents, at most 64 processes (`server/src/web/webmem.c`, `tests/webmem_test.c` on a
  made-up `/proc`). A process it can't read is counted apart, never guessed. The page reads it every 5 s while open;
  when the app isn't running it shows the catalog's measured figure, if any.

## Permissions (asked once per app, revocable in Settings → Web Apps)

The page's requests (notifications, camera, microphone, screen sharing, location) reach `lord-web` as WebKit
permission requests. If the registry says `allow` or `deny`, `lord-web` answers on its own. If it says `ask`,
`lord-web` asks lord-server over the session bus:

- **Call:** `org.lordos.WebApps1.Ask(app, profile, kind) → s` on `/org/lordos/WebApps1`.
- **Answer:** `"allow"`, `"deny"` or `"once"`. lord-server shows the dialog and stores the answer unless it is `once`.

lord-server checks that the caller's pid is the `lord-web` it started for that app and profile. A page never gets an
answer from anything else.

The registry's words count only for the app's own pages: a request is answered from it only while the window shows an
address inside `scope` (WebKit gives no origin for a request, so it is the main frame's), and a `navigator.permissions`
query only when the origin that asks is `origin` too. A page outside the scope (a sign-in on another site, a redirect,
a navigation inside the window) or a frame of another origin is denied everything, and lord-server is never asked for
it; the check is made again before each question and before the answer (`weburl_perm_allowed`).

`lord-web` reads its registry file again before each request, so an answer lord-server stored since the app started
counts. No answer (no lord-server, an error, anything but the three words) is `deny`. A camera and microphone request
asks both kinds and needs both. The page's `navigator.permissions` queries get the registry's word (`allow` granted,
`ask` prompt, `deny` denied); the names of cameras and microphones only when one of them is allowed. Storage access,
media keys and anything else are denied. Notifications carry the app's name (with the profile's when it has more than
one) and icon, and a notification with the same tag replaces the one before it.

Notifications (`webbus.c`): the app's icon file goes as `app_icon` and `image-path`, and `desktop-entry` is
`lordos-web.<app>.<profile>`. notifyd keeps that entry only when the notification comes from the lord-web lord-server
started for that app and profile (`webmgr_owner` on the sender's PID); the shell then shows it under the web app, with
its name and icon (before, the banner had no icon). A click on the notification itself is its `default` action: the
shell sends it (it never shows it as a button), and lord-web calls `webkit_notification_clicked` first, so the page's
own `onclick` runs (a messaging app opens that chat), and then brings the window forward with the activation token
LordOS sent. WebKit 2.48 gives neither a notification's image (the sender's photo) nor its `silent` option.

**Autoplay** (`webback_policies`, webback.h): every web app's view allows autoplay with sound
(`WEBKIT_AUTOPLAY_ALLOW`), as Chrome does for an installed app. A messaging app's ringtone and new-message sound start
from the network, never from a click, and WebKit's default (`WEBKIT_AUTOPLAY_ALLOW_WITHOUT_SOUND`) kept them silent:
calls rang with no sound and messages arrived quietly. Found by the maintainer on 2026-11-26.

LordOS's sound (`notifySound`, `webpage_sound_suppress`): with `"auto"`, lord-web watches WebKit's
`is-playing-audio`. A page that starts audio less than 1.5 s before or after one of its notifications plays its own
sound: that notification (when it came first) and every later one go with the `suppress-sound` hint, so LordOS
doesn't play a second sound. lord-web remembers it for the profile (`state.json`, `"ownSound": true`), so after the
first time nothing waits or guesses. notifyd passes `suppress-sound` to the shell, which then shows the banner
silently.

**The notification log** (`webpage_notify_line`, `webbus.c`): each notification the page hands lord-web writes one
line to the app's log (`/var/log/lordos/users/<user>/web/<app>-<profile>.log`), in the call log's budget of 60 lines a
minute (`webcall_log_as`, shared with `webcall:`):

    notify: page tag 3f9a0c17 silent unknown renotify unknown shown

Never the title, the body, the icon or an address. The tag (a messaging app's chat id, often a phone number) is only 8
hex digits of a hash keyed with a random number lord-web picks at start and never writes: two lines of the same run can
be matched, a line can't be turned back into the tag, and the next run gives other digits. `none` when there is no
tag. `replaced` when it took the place of one with the same tag (lord-web's own replace, or WebKit withdrawing the old
one less than a second before). `silent` and `renotify` are `unknown`: WPE 2.54's `WebKitNotification` tells neither.
With it, "the sound played but no banner" can be told apart: no `notify:` line means the page never posted one.

**A service worker's notifications** (`registration.showNotification`, a "persistent" notification). WebKit 2.54's
GLib port doesn't give them to the view: `WebKitNotificationProvider::show` gets no page for them (its "pageless"
branch, `UIProcess/API/glib/WebKitNotificationProvider.cpp`), and sends them itself to `org.freedesktop.Notifications`
(`UIProcess/Notifications/glib/NotificationService.cpp`). In LordOS that is notifyd, which still files it under the
web app by lord-web's process, with `notifyd_plain` on its text, and WebKit replaces a same-tag one itself. A click
reaches WebKit (`ActionInvoked`), which fires `notificationclick` in the service worker. What that path lacks: no
`suppress-sound` (LordOS's sound can double the page's), no `silent`, and the window doesn't come forward, because
GLib's WebKit doesn't implement `clients.focus()` or `clients.openWindow()` from a service worker
(`focusFromServiceWorker`), and lord-web never sees these notifications, so they get no `notify:` line either.
lord-web sets GLib's application name to the app's (`g_set_application_name`, webmain.c): before, WebKit sent
`[Invalid UTF-8]` as the app name and GLib logged a critical for each one.

A messaging web app checked on 2026-11-26 (its bundle as cached by lord-web) uses this path only when
`Notification.maxActions` is at least 1, which WebKit 2.54 doesn't implement, so its message notifications are page
notifications; its app log since 2026-11-11 has none of the criticals the service-worker path writes.

Follow-up, not built: a WebKit patch for the backport (`+4.2` or later, `tools/pkg/wpewebkit-chroot.sh`): when a
pageless notification arrives, `WebKitNotificationProvider` looks for a view of the same network session whose page
has the notification's origin and emits that view's `show-notification`, keeping the D-Bus path when there is none.
lord-web's own path would then apply unchanged (`notify:` line, sound rule, tag replace, the window brought forward on
click), and `webkit_notification_clicked` and `webkit_notification_close` would reach the service worker as
`notificationclick` and `notificationclose` through the service-worker notification manager, as they do now from
D-Bus. A page-side shim was set aside: it can't fire `notificationclick` and doesn't see what the worker shows itself.

## Calls (in development; approved by the maintainer on 2026-11-25)

Voice and video calls in web apps (a messaging web app first, also video meeting sites), for the release after
0.8.9.6.2. What a call needs, and where each piece is:

- **WebRTC in the engine.** Debian builds WPE WebKit without WebRTC, so `RTCPeerConnection` didn't exist. LordOS's
  build `2.54.3-1~lordos13+3` turns it on with WebKit's GStreamer backend (GStreamer 1.26's `webrtcbin`; ICE by libnice,
  `gstreamer1.0-nice`; DTLS, SRTP and SCTP from plugins-bad; Opus, VP8, H.264 through OpenH264) ([WPEWEBKIT.md](WPEWEBKIT.md)).
  `lordos-web` depends on `gstreamer1.0-nice`. Checked 2026-11-25 on amd64 (`tests/webcalls_check.py` in the build's test
  chroot, WebKit's sandbox on): `RTCPeerConnection`, `RTCRtpSender.getCapabilities` and `RTCRtpScriptTransform` exist;
  the engine offers audio `opus`, `PCMU`, `PCMA`, `telephone-event` and video `H264`, `VP8`, `VP9` (Chrome 158 also has
  `red`, `G722`, `CN`, `AV1`, `rtx`, `ulpfec`); `createEncodedStreams`, `MediaStreamTrackProcessor`/`Generator`,
  `WebTransport` and `setSinkId` don't exist (Chrome has them).
- **SharedArrayBuffer when the site is cross-origin isolated.** A messaging web app checks, before it offers calls,
  `SharedArrayBuffer`, `Atomics` and `RTCPeerConnection` (its own gating code, read 2026-11-25), and its page is served
  with COOP `same-origin` and COEP `require-corp`. WPE never had SharedArrayBuffer: WebKit gives such a page a web
  process of its own but only its Cocoa ports turned SharedArrayBuffer on there. LordOS's build does the same on Linux
  (`lordos-sharedarraybuffer-coi.patch`), so SharedArrayBuffer exists only in cross-origin-isolated pages, as in
  Chrome and Safari (Spectre: never for a page that isn't isolated). lord-web drops `JSC_*` from its environment, so
  nothing turns it on for every page.
- **Chrome's identity, all of it** ("Browser identity" above): the site reads the User-Agent string, the Client Hints
  and `navigator.userAgentData`, and treats a WebKit it recognises differently (another code path for calls); with the
  Chrome identity all three say Chrome 158 on Linux, from one table.
- **Microphone and camera** ("Permissions" above): the page's `getUserMedia` reaches lord-web as WebKit's permission
  request and is answered from the registry or by LordOS's own prompt (`Ask`, lord-server's dialog), once per app,
  revocable in Settings → Web Apps (Camera, Microphone, Screen). The page lists devices (`enumerateDevices`, with
  names) only once one of them is allowed, and chooses among them with its own picker; LordOS Studio's virtual camera
  is one of them when it is on. WebKit captures the microphone through PulseAudio (pipewire-pulse) from its sandboxed
  web process, which the capture indicator already shows under the web app (PRIVACY.md); the camera WebKit opens through
  V4L2 (`/dev/video*`, which WebKit's sandbox binds for the web process), which the indicator doesn't see
  (PRIVACY.md, "Not covered"): lord-web logs WebKit's own capture state (`capture: camera on`) and a way for it to tell
  the indicator is proposed for the maintainer's review (a new lord-web → lord-server method, not built).
- **The network: ICE in WebKit's network process (librice, `+4`).** WebKit's web process has no network of its own
  (bubblewrap's `--unshare-net`, unchanged). With `+3` WebKit's GStreamer backend ran ICE (libnice) inside it, saw only
  the sandbox's loopback interface, ignored it and gathered no candidate at all, so nothing connected. The maintainer
  chose librice on 2026-11-25: with `-DUSE_LIBRICE=ON` WebCore's ICE agent speaks librice's protocol and every ICE
  socket (UDP, and TCP for ICE-TCP and TURN over TCP/TLS) is opened by WebKit's **network process**, which already has
  the network for every page load and runs in lord-web's firewall scope like the rest of the app; the web process keeps
  none. LordOS builds librice for Debian 13 ([WPEWEBKIT.md](WPEWEBKIT.md), "librice"). Checked 2026-11-25 on amd64 in
  the build's test chroot with WebKit's sandbox on (`tests/webcalls_check.py --webrtc`): the two peer connections of
  one page gather host candidates (4 UDP, 8 TCP) and connect in 300 to 400 ms, with Opus and VP8 (215 to 221 audio
  packets, 64 to 67 frames of 320×240 in 4 s), and again with Opus and H.264; the audio comes from WebKit's mock
  microphone (`WEBCALLS_MOCK_CAPTURE=1`, test only), the video from a canvas.
- **Received video and hardware decoders.** WebKit's GStreamer backend asks its registry scanner whether a hardware
  decoder handles a received video codec. When one does, it leaves the track encoded and decodes it only in the
  playback pipeline of the `<video>` element that shows it; otherwise it decodes it at once in software. On a PC with an
  AMD or Intel GPU and Mesa's VA-API driver, H.264 goes to `vah264dec` (gst-plugins-bad's `va` plugin), which opens the
  GPU's render node inside WebKit's sandboxed web process (WebKit's bubblewrap gives it `/dev/dri`) and decodes there;
  VP8 and VP9 on an RX 580, which has no hardware decoder for them, go to `vp8dec` and `vp9dec`; without a GPU, H.264
  goes to `openh264dec`. A track that no element plays therefore decodes nothing on such a PC, and its `inbound-rtp`
  stats have no `framesDecoded` until an element plays it; every call page shows its video, so calls are unaffected.
  lord-web sets nothing here: forcing early decoding (`WEBKIT_GST_WEBRTC_FORCE_EARLY_VIDEO_DECODING=1`) would decode
  tracks nobody shows, and lowering the VA decoders' rank would give up the GPU that works. Checked 2026-11-26 on the
  maintainer's PC (RX 580, `+4.1` installed): `tests/webcalls_check.py --webrtc` plays the received video in a `<video>`
  element and requires frames shown there (`requestVideoFrameCallback`) as well as decoded, for VP8, VP9 and H.264: 59
  frames of 320×240 shown each in 4 s, H.264 through `vah264dec`; the same with `LIBGL_ALWAYS_SOFTWARE=1` (lord-web's
  shared-memory mode) and with `/dev/dri` hidden (H.264 through `openh264dec`, as in the build's test chroot).
- **TURN URLs with `?transport=`.** WebKit 2.54's `RTCPeerConnection` refused every TURN server URL with a query (an
  inverted check), and sites hand out `turn:host:443?transport=tcp` (RFC 7065): a video meeting site's page threw
  `SyntaxError` when it created its connection. `lordos-turn-transport-query.patch` (`+4`) accepts exactly
  `transport=udp` and `transport=tcp`, which the ICE agent reads; `tests/webcalls_check.py` checks both and that another
  query is still refused.
- **Against Chrome, and where calls stop today** (2026-11-26, amd64, `+4` in the build's test chroot with WebKit's sandbox
  on, Chrome 158 headless on the same machine with its fake camera and microphone; `tests/webcalls_pair.py`, a
  signaling server of its own, and `tests/webcalls_jitsi.py`, by hand):
  - **lord-web's engine offers, one transport (`bundlePolicy: 'max-bundle'`): works.** Connected in 201 ms over
    host/UDP; both directions carry audio and video: lord-web's engine received 552 Opus packets and 221 H.264 frames of
    640×480 and sent 772 and 300 in 15 s; its call log said `connection connected`, `out audio audio/OPUS`, `out video
    video/H264 640x480 30 fps`, `in video …`, `in audio …`.
  - **It offers with WebKit's default (`balanced`, a transport per media):** video flows both ways (about 690 frames of
    640×480 each way) but the audio transport never connects, so neither side hears and both stay `connecting`.
  - **Chrome offers, lord-web's engine answers:** the answer picks PCMU instead of the Opus Chrome offered first, video
    doesn't flow and Chrome receives nothing. WebKit names Opus `OPUS` in its own capabilities; the answer's matching of
    the offer's `opus` is the first suspect.
  - **A public video meeting server (Jitsi Meet, through its video bridge, which offers):** the page joins, ICE
    connects, but the bridge re-offers about 4 times a second (a renegotiation loop: 242 offers in 55 s) and DTLS never
    finishes (WebKit's DTLS client hello goes out six times with no answer), so no media either way. TURN through librice
    fails too (`LongTermClientAuthError { reason: UnsupportedFeature }`: WebKit asks librice for TURN's anonymous-username
    feature as required). Two headless Chromes through the same server, the same way, exchange 720p VP9 and Opus both ways.
  - So a call works today when the page offers with one transport; the answering side, several transports and the
    bridge's renegotiation are WebKit GStreamer WebRTC bugs still to fix (or to take from a newer WebKit) before a
    messaging web app's calls can be expected to work. Not yet tried with the messaging app itself (it needs the
    maintainer's account).
- **The call log** (`webcall.h`, `webcall.c`, `webcalls.c`, `webcall.js`): lines starting `webcall: ` in the app's log
  (`web/<app>-<profile>.log` in the user's log folder, docs/SYSTEM.md "Log files"; the Logs app and `lordos-issue` show
  it), always on, at most 60 a minute. They say what the page has for calls, each peer connection, its ICE gathering,
  ICE connection, connection and signaling states, once connected (and every 5 minutes) the codec, size and frame rate
  of each direction and the kinds of ICE candidates in use, errors by name and the call that failed, `getUserMedia` and
  `getDisplayMedia` (what, the result, the error's name, each device as a closed word: `built-in`, `usb`,
  `bluetooth`, `virtual`, `lordos-studio`, `other`), the permission answers, WebKit's capture state, and the Client
  Hints the app's own site asked for. Never SDP, ICE candidates, addresses, ports, tokens, ids, phone numbers, device
  labels, other sites' addresses or anything said in the call: the page-side script sends only fixed fields, and
  `webcall_line` checks each against a closed list or pattern again and drops the rest. Example:

  ```
  webcall: page: crossOriginIsolated yes, SharedArrayBuffer yes, Atomics yes, RTCPeerConnection yes, getUserMedia yes
  webcall: client hints asked by the app: sec-ch-ua-full-version-list, sec-ch-ua-arch, sec-ch-ua-bitness
  webcall: permission microphone: allowed (asked)
  webcall: getUserMedia audio: granted (audio usb)
  webcall: capture: microphone on
  webcall: peer connection 1 created
  webcall: peer connection 1: ice-gathering gathering
  webcall: peer connection 1: ice-connection checking
  webcall: peer connection 1: connection failed
  ```

  The page-side script (`webcall.js`, the app's main frame, the page's world) only watches: `RTCPeerConnection` stays
  the engine's (a proxy that constructs the same object; `instanceof`, statics and `constructor` unchanged), its
  methods are the engine's called through, every function reads as native code, and nothing is added that the engine
  lacks. It reports through a DOM event with a name random per process to a bridge in lord-web's own isolated world
  (`lordos-web-call`), the only world with the `lordosCall` message handler: the page never sees a
  `webkit.messageHandlers` of lord-web's.
- **Tests:** `build/webua-test` (the line for each kind of report, what is dropped: an address, SDP, a candidate, an
  unknown kind or word, a label; the device words; the rate; only the app's own site's Client Hints kept),
  `tests/webcalls_check.py` with `build/webcalls-check` (the real engine, headless, WebKit's sandbox on: Chrome's
  identity, SharedArrayBuffer only when isolated, and with `--webrtc` the loopback below), also run by
  `tools/pkg/wpewebkit-chroot.sh`'s test on the WebRTC build.

## What `lord-web` uses of the session (like any Linux app)

- **Files:** WebKit's file chooser goes through the FileChooser portal. Downloads go to the XDG Downloads folder only,
  with a notification when done.
- **Notifications:** `org.freedesktop.Notifications` (`notifyd`). A click tells the page, then opens that app's window.
- **Links outside `scope`:** OpenURI, so they open in the person's browser.
- **Screen sharing:** the ScreenCast portal (`docs/WAYLAND.md`).
- **Background:** a StatusNotifierItem tray icon with Open and Quit when `background` and `tray` are on.
- **The clipboard and the keyboard:** a normal Wayland client.

## Isolation

- **WebKit's sandbox:** the web content and network processes run under bubblewrap and seccomp, and are never turned
  off.
- **The `lord-web` process:**
  - its own firewall scope `lordos-web-<app>` (`app-lordos-lordos-web-<app>.scope`, placed by lord-sessiond like any
    launched app; `lordos-web…` is reserved, so no `.desktop` can take that identity), a row in Monitor with the app's
    name. Inside a LordOS session it refuses to start when it can't be placed;
  - a cgroup with memory and CPU limits shared by all the app's profiles and their WebKit processes, which are its
    children (`webpage_scope_limits`): memory.max three quarters of the computer's memory (1 to 16 GB), memory.high an
    eighth below it, half the cores (at least two). Until 0.8.8.7 it was 2 GB high, 4 GB max and two cores: two
    profiles of a messaging app (~800 MB each, plus network processes and the page cache of their files, which counts)
    sat at memory.high, the kernel throttled the whole scope (the maintainer's `memory.events`: `high 25107`), and the
    app froze for moments when switching views. One scope per profile would need the firewall's identity
    (`fw_app_canon`, `desktop:lordos-web-<app>`) to change, so it stays per app;
  - a crash of `lord-web` itself (a fatal signal, or a C++ exception nobody caught in WebKit's UI process, which ends in
    `abort`) writes `lord-web: crashed, stack:` and the stack (`backtrace_symbols_fd`, addresses for `addr2line`) to
    `web/<app>-<profile>.log` of the user's log folder (docs/SYSTEM.md, "Log files") before the signal's default. lord-server waits for an ended `lord-web` from its main loop
    (`webmgr_reap`), so none is left a zombie;
  - Landlock's scoping (no abstract unix socket and no signal outside its own processes) and `no_new_privs`, again
    refusing to start inside a session when they fail. **Not Landlock's file rules:** a process under them can't mount,
    and bubblewrap mounts for every WebKit process it starts (checked 2026-11-10 on kernel 6.12: with only
    `LANDLOCK_ACCESS_FS_WRITE_FILE` handled, `bwrap` fails with "Failed to make / slave: Operation not permitted"). The
    files of the web content are kept by WebKit's sandbox instead; `lord-web` itself is LordOS's own code and only
    opens its registry file (read-only, no link followed), its profile folder and the Downloads folder;
  - it refuses to start on any mismatch: argv other than its forms (`--inspect` only last, once, with an app), a registry file that is a link, over 64 KB,
    with an unknown key, a wrong type, a bad id or address, a scope that isn't a prefix of `startUrl` or not on
    `origin`, or an app or profile that isn't the one asked for (`webargs.c`, `webreg.c`, `weburl.c`, tested by
    `tests/webapp_test.c`); and when `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS` is set. It drops the variables that
    point WebKit or GStreamer at other code (`WEBKIT_*`, `WPE_*`, `GST_PLUGIN*`, `GIO_EXTRA_MODULES`, `LD_PRELOAD`…)
    and JavaScriptCore's options (`JSC_*`: `JSC_useSharedArrayBuffer` would give every page SharedArrayBuffer, "Calls").
- **What a page never gets:** no `lord.*`, no broker, no script injected with privileges. The one script of lord-web's
  a page can see is the clipboard's `write` and `writeText` that WPE lacks, and what it hands over reaches the
  session only right after the person's own key or click in the window. The files of a drop are only the ones the
  person dropped on the window, read by lord-web, never by the page.
- **What a crash or hang affects:** only that window. lord-server sees the process end and the Dock shows the app as
  closed.

## Performance (since 0.8.8.8)

- **Out of sight, WebKit throttles.** WebKit is told the view is visible only while the window is mapped, configured
  and not `suspended` (`webpage_view_visible`). lord-server sends xdg_toplevel's `suspended` (xdg-shell 6) for a window
  that is minimized, on a hidden desktop, or wholly covered: inside an opaque window above it on the showing desktop (a
  LordOS window whose app isn't transparent, or another web app's window; any other Linux window may be translucent,
  so it never covers; never in the overview: `sdk/desktopmodel.js` `coveredIds`, `wl_set_covered`). Closed into the
  background it is not visible either. Then the page stops painting and its `requestAnimationFrame`, its CSS
  animations pause (`HiddenPageCSSAnimationSuspension`) and its timers run at most once a second
  (`HiddenPageDOMTimerThrottling`, both off by default in WPE 2.48). Its network (a chat's WebSocket), its audio and its
  notifications go on. Shown again, it paints at once. lord-server also answers a hidden window's frame callbacks once
  a second (`docs/WAYLAND.md`, "Hidden windows").
- **Memory:** the document browser cache model, set before the web context exists (in WPE 2.48 the model is
  process-wide and the web process cache sizes itself once, when the context is made: set after, the web process a page
  leaves at its first navigation, which any site with Cross-Origin-Opener-Policy causes, stayed cached, ~130 MB PSS per
  profile). Memory pressure (`webengine.c`): the computer's memory up to 3 GB as the base, a third of it (~1 GB) to drop
  rebuildable caches, half (~1.5 GB) to drop decoded images and compiled code too, polled every 30 s. 0.8.8.7's 1.5 GB
  base polled every 10 s put a ~600 MB messaging page above the first mark, so it dropped its font, glyph, line layout
  and style caches every 10 s.
- **Input to screen:** `LORD_WEB_TRACE=1` logs, for each click or key, the time to the frame WebKit exports after it,
  to its commit and to the compositor's frame callback.
- **Measured** 2026-11-11 on VM 127 (4 cores, no GPU, frames by shared memory), a private headless session watched at
  60 Hz, a page with a CSS animation, a `requestAnimationFrame` canvas and a 50 ms timer, 15 to 30 s each:

  | | before (main 9b86e8fe) | now |
  |---|---|---|
  | visible: WebKit processes / lord-server | 116 % / 73 % | 115 % / 74 % |
  | minimized | 115 % / 5 % | 0.4 % / 0.3 % |
  | wholly covered by Settings | (not detected) | 0.3 % / 0.7 % |
  | another desktop | | 0.3 % / 0.3 % |
  | closed into the background | | 0.4 % / 0.3 % |
  | context switches per second, minimized | 3400 | ~30 |

  Shown again, the page went back to 60 frames a second within a second each time. A click to the next frame on
  screen (a menu shown on mousedown): 33 to 54 ms on a still page, 29 to 85 ms on a page with a 3 million object heap,
  1500 contacts with SVG icons and 3000 messages (688 MB of WebKit's footprint, above 0.8.8.7's first mark); the
  memory pressure change made no measurable difference there (29 to 62 ms), so the ~hundreds of ms seen in a real
  messaging app are not explained by it yet.

## The desktop's side: icons, unread counts, sounds and profiles

### More registry keys (all optional; `webreg.c`, `tests/webreg_test.c`)

- `tray` (boolean, `false` when missing, written only when `true`): the app's tray icon (StatusNotifierItem, Open and
  Quit). Settings → Web Apps → "Show in the menu bar", `lord.webapps.setTray(app, on)`. Background mode
  (`background`, "Keep running when closed") stays its own switch.
- `notifySound` (`"auto"` when missing, written only when it isn't): `"auto"` LordOS's sound only when the page plays
  none of its own (lord-web learns it and then sends the `suppress-sound` hint), `"app"` the page's sound only,
  `"system"` always LordOS's, `"none"` no sound from LordOS. `lord.webapps.setNotifySound(app, value)`; Settings → Web
  apps → Notification sound, and the same choice in Settings → Notifications (one value for both pages). LordOS
  controls only its own sound: to silence the page's own sound, use the site's settings.
- `colorScheme` (`"auto"` when missing, written only when it isn't): `"auto"`, `"light"` or `"dark"`, the app's
  Appearance. `lord.webapps.setColorScheme(app, value)`; Settings → Web Apps → Appearance. Until WPEPlatform only
  Automatic can be chosen there and lord-server reads `"light"` and `"dark"` as `"auto"`.
- A profile's `userAgent` (`"chrome"` when missing, written only when it isn't): `"chrome"`, `"edge"`,
  `"firefox"` or `"lordos"` (an old `"experimental"` reads as `"chrome"`), its Browser identity. `lord.webapps.setUserAgent(app, profile, value)`; Settings → Web Apps → Browser
  identity and the app menu.
- A profile is `{ id, name }` plus optional `color` and `userAgent`, `color` one of LordOS's tag colors (Files' tags, Databases'
  favorites): `red`, `orange`, `yellow`, `green`, `blue`, `purple`, `gray`, or `none`. A new profile gets one by
  itself (`webreg_profile_default_color`: the first none, then orange, purple, blue, green, red, yellow, gray, the
  first not taken); a profile without the key reads the same by its place. `lord.webapps.setProfileColor(app,
  profile, color)`.

### The site's own icon

The catalog ships only LordOS's letter tiles (`assets/webapps/icons/`): no third-party logo is in the repository. When
`lordos-web` is installed, `lord.webapps.fetchIcon(app)` runs the probe ("Adding an app from an address") on the
app's own start address (the registry's, or the catalog's for an app not installed yet); `lord.webapps.iconResult(app)`
then keeps the icon `--probe-icon` fetched, only when the probe ended on the app's own origin and the file is the
probe's (`$XDG_RUNTIME_DIR/lordos-web-probe/`), checked again as a PNG or an SVG of at most 1 MB, as
`~/.cache/lordos/web/icons/<app>.png|svg` (`ui/webicon.c`: the name is the app id and the kind, nothing from the site;
the folder is in `fsops_protected`). `list()` and `get()` give it as `iconPath` (with `siteIcon: true`), so the Dock,
the launcher, the switcher and Settings use it; `icons()` lists the cache for the Store. Errors: `webapps.iconOrigin`,
`webapps.noIcon` and the probe's: when no icon was usable, lord-server passes on the runtime's code for the last one
tried (`iconError`, a `web.probe.…` code).

- **When:** the Store's cards ask for it lazily (`wantSiteIcon`), one probe at a time, never twice in a session for an
  app that failed; an installed catalog app without one gets it when it is launched (never blocking the launch);
  Settings → Web Apps → Refresh icon fetches it again: a spinner while the probe runs, then "Icon updated from the site"
  and the new icon, or the reason there is none (`webapps.iconOrigin`, `webapps.noIcon`, the probe's code,
  `webapps.noRuntime`); it never fails silently (`siteIconStatus` in `sdk/webapps.js`). Each failure, from Settings or
  from the Store's lazy fetch, is one line in `shell.log`: `js: [warn] webapps: the site's icon of <app> was not kept:
  <code>`, the app's id and a closed code, never an address from the page. lord-server's compositor log has one line
  per refused icon (`webapps: probe <n>: icon <i> of <k> refused: <code>`), without its address. Without lord-web the
  letter tile stays.
- **WhatsApp** (2026-11-26): the signed-out probe ends on `/login/`, which links no manifest; its manifest is found at
  a usual address (`/manifest.json`), and its 512 px PNG is fetched with the Chrome identity.
  `tests/web_probe_test.py` keeps a local fixture of that pattern (a redirect, no manifest link, a WebP shortcut icon,
  WebPs labelled PNG, a maskable `image/svg` SVG, strict CSP, COOP, COEP and Document-Isolation-Policy headers, and 400
  to a request that isn't Chrome's).
- An address-added app's icon is still the one saved in the registry when it was added; uninstalling it removes its
  cached icon too (a catalog app's stays for the Store's card).

### Unread count on the Dock

`org.lordos.WebApps1.SetBadge(s app, s profile, u count)` on `/org/lordos/WebApps1` (no answer needed; lord-web calls
it without waiting). Only the lord-web lord-server started for that app and profile counts, the same check as `Ask`:
the bus gives the caller's pid, `webmgr_owner` says whose lord-web it is (`ui/webbadge.c`, `tests/webdock_test.c`). A
bus name is never reused, so a sender checked once stays that app and profile until it leaves the bus; one that
turned out to be another process is refused from then on (refused senders give their place when the table of 32 is
full). The count is clamped to 0..9999; it reaches the shell as the event `webapps-badge` `{ app, profile, count }`,
and a lord-web that leaves the bus gets a 0. `lord.webapps.badges()` gives the counts to a new JS context.

The Dock draws a red badge on the app's icon with the sum of its profiles ("99+" past 99), Lord Classic's taskbar
too; the Dock's window list shows each profile's own count. Settings → Web Apps → "Show unread count on the Dock"
(per user, `settings.webAppPrefs[app].badge`, on by default).

### Notifications

notifyd (`hw/notifyd.c`) asks lord-server whose process sent a Notify (`notifyd_set_web_of_pid` → `webmgr_owner`):
only for a lord-web it adds `webApp` and `webProfile` to the event, and the `app_icon` it sent as `appIcon`.
`suppress-sound` is passed on for every app. The shell then files the notification under `lordos-web.<app>` (one row
in Settings → Notifications for all the app's profiles, whatever desktop-entry was sent), names it "<app>
(<profile>)" with more than one profile, and draws the app's icon with the profile's colored dot. It shows the file
named in `appIcon` only when it is exactly one of that app's icons (`~/.config/lordos/web/icons/<app>.png|svg`, the
cache's, or the profile's icon below; `webIconPathOk`); anything else gets the icon LordOS knows. The same
desktop-entry from any other process is a plain Linux app's notification.

Settings → Notifications → an app → Sound: Default, None or one of LordOS's sounds (all synthesized by lord-server,
`sound/snd.c`): the default `notify`, `pop`, `chime`, `ping`, `drop` (`sdk/notifytones.js`; the stored value is
`true`, `false` or the sound's name, so older settings keep their meaning). For a web app the row shows its
Notification sound and, when LordOS's sound plays, which one.

### Profiles told apart

- **Windows:** with more than one profile the frame and the switcher say "<app> — <profile>", the Dock's window list
  "<page title> — <profile>" with a dot in the profile's color (and its unread count); the frame has a thin strip in
  the color along its top. The Dock's "New window: <profile>" items are actions and never carry a check mark.
- **Icons:** the profile's color as a small dot with its initial on the app's icon (windows, notifications, Settings).
- **For lord-web's tray icon and notifications:** lord-server writes each colored profile's icon, the app's PNG (the
  site's own when cached, else the registry's) at 64 px with the dot and the initial (A–Z, 0–9; another letter gives
  the dot alone), as `~/.config/lordos/web/icons/<app>.<profile>.png` (`webicon_profile_write`, 0600, written again
  when the color or the site's icon changes, removed with the profile or for "none"). lord-web uses it when it is
  there, else the app's icon.
- **One Dock icon per profile** (Settings → Web Apps, off by default, `settings.webAppPrefs[app].dockPerProfile`):
  while a profile other than the first has windows, it has an icon of its own (not pinnable), with its own count; the
  first profile stays on the app's icon.
- **Each profile's data** in Settings → Web Apps is measured again when a profile is added or starts running and every
  30 s: before, a profile added during the visit showed "none yet" until Settings was opened again, because its folder
  only exists after its first start.

## Developer tools (0.8.8.9)

Approved by the maintainer on 2026-11-15. For web developers: WebKit's Web Inspector for one web app window, in the
person's browser.

- **The switch:** Settings → Web Apps → Developer → *Developer mode*, one switch for every web app, off by default.
  It is kept in `~/.config/lordos/web/developer.json` as `{"developerMode": true}` (0600), written only by
  `lord.webapps.setDeveloperMode` (the folder is in `fsops_protected`); a missing file, or one that isn't exactly a
  JSON object whose `developerMode` is `true`, is off (`webinspect_parse`). Turning it off ends every window that has
  developer tools on.
- **The menu:** with the switch on, each web app window's Window menu (the menu bar's in Lord Flow, the window's in Lord
  Classic) has *Inspect (developer tools)* (`webInspectItem`, `inspectItem` in `webappsmodel.js`), checked and
  unavailable while that window has them.
- **The question:** every time, Cancel focused (`<WebAppInspectPrompt />`, `sdk/hardware.jsx`): "Developer tools can see
  everything in this app, including messages and sessions. Turn on for this window?", the app's origin, and a line that
  says the window opens again with them on, that they open in the browser, that they stay on until the window closes
  and that meanwhile any program on this computer can connect to them. Only *Turn On* calls
  `lord.webapps.inspect(app, profile)`.
- **Why the window starts again:** WPE WebKit 2.48 has no inspector inside the view (no `WebKitWebInspector` in WPE)
  and no API to turn remote inspection on for a view that runs. Its remote inspector starts only from the environment
  as the process starts: `WEBKIT_INSPECTOR_SERVER=<ip>:<port>` (a socket protocol for another WebKit as the client) or
  `WEBKIT_INSPECTOR_HTTP_SERVER=<ip>:<port>` (an HTTP server that serves the inspector's own pages, from
  `/usr/share/wpe-webkit-2.0/inspector.gresource`, to any browser, with the page over a WebSocket), read when the first
  web context is made. Port 0 is refused ("Failed to start remote inspector server on 127.0.0.1:0: invalid address").
  LordOS uses the HTTP one.
- **How:** `lord.webapps.inspect` (desktop only, not in the broker) → `webmgr_inspect`: refused with
  `webapps.developerOff` while the switch is off; the app and profile must be in the registry; when that profile's
  lord-web runs without developer tools it is ended (as when a profile is removed), then started again as
  `lord-web --app <app> --profile <profile> --inspect`; one that already has them is only brought forward. lord-web:
  - refuses `--inspect` unless the switch's file says on (it reads it itself, so a start by anything else gets
    nothing more), after taking the profile's lock;
  - sets `WEBKIT_INSPECTOR_HTTP_SERVER=127.0.0.1:<port>` before the web context exists, with the first port of
    `WEBGUARD_BACKEND_PORTS` it can bind on `127.0.0.1` (it closes that test socket at once; it drops every `WEBKIT_*`
    variable it was started with first, so nothing else can turn the inspector on); the view gets
    `enable-developer-extras`. Those ports (6665–6669, 6697, 10080, 1719, 1720, 1723, 3659, 4045, 5060, 5061, 6566)
    are on the browsers' "bad port" list (WHATWG Fetch): Chrome, Chromium and Firefox refuse every navigation,
    `fetch`, image and WebSocket to them, whatever a page asks, so no web page can reach WebKit's own server. X11's
    6000 and NFS's 2049 are left out; 4190 and 6679, on the spec's list, reached Chromium 156 in our test and are left
    out too. With all of them taken it doesn't start;
  - every half second for up to 20 s, checks `/proc/self/net/tcp` against its own sockets (`/proc/self/fd`):
    `webinspect_listening` answers 1 only when the listening socket on that port is one of lord-web's and on
    127.0.0.1, -1 when another process listens on that port (the port was taken between the check and WebKit's bind)
    or ours listens on another address;
  - then starts its gate (below) and opens the gate's one-time address through OpenURI, and sends a notification
    ("Developer tools are on", "They opened in your browser and stay on until you close this window.") whose action,
    *Open in browser*, opens them again with a new one-time address. The address is never in the notification, which
    other programs of the session can read. On -1, a gate that can't listen, or after 20 s, only "Developer tools
    couldn't start".
  - WebKit also starts its own inspector server on another random port of 127.0.0.1 inside lord-web, which WebKit
    2.48 picks itself (`WEBKIT_INSPECTOR_SERVER` doesn't move it while the HTTP server is on: tried, nothing listened).
    It speaks WebKit's own socket protocol, not HTTP: each message starts with its length as 4 bytes. A browser's
    request can't be one: it starts with `GET /`, `HEAD` or `POST`, read as a length of about 1.2 GB, so the server
    waits for the rest and answers nothing (checked: no answer, lord-web's memory unchanged after such requests), and
    a page controls none of the first bytes. The web content process doesn't reach it (it only logs that). A program
    on this computer that speaks the protocol can (the residual risk below).
- **The gate** (`webproxy.c`, its checks in `webguard.c`, tests in `webapp-test`): lord-web's own listening socket on
  127.0.0.1 (a port the kernel picks), in its GLib loop (no thread, no other process, no new library). It forwards to
  WebKit's port only one HTTP request per connection, or one WebSocket upgrade, and only when:
  - the request head is complete within 10 s and at most 16 KB, every line ends in CRLF, no control byte, no folded
    line, a `GET` of an origin-form target in HTTP/1.1, no body (`Content-Length` other than 0, `Transfer-Encoding`);
  - `Host` is exactly `127.0.0.1:<gate port>` or `localhost:<gate port>`, once (DNS rebinding: a name that resolves to
    127.0.0.1 carries its own name);
  - `Origin`, when present, is `http://127.0.0.1:<gate port>` or `http://localhost:<gate port>`, once (another site's
    `fetch` or WebSocket; a WebSocket isn't bound by CORS, so this check is what stops it);
  - `Sec-Fetch-Site`, when present, is `same-origin` or `none`: a page on another port of this computer is the same
    *site* as the gate for cookies, and its frames and `no-cors` requests carry no `Origin`;
  - and it carries the cookie `lordos-inspect-<gate port>` with this window's value: 32 random bytes (`getrandom`) in
    base64url, compared in constant time.

  The only way to get the cookie is the one-time address `http://127.0.0.1:<gate port>/<secret>/`, the one OpenURI is
  handed: another 32 random bytes, checked the same way (with the same Host, Origin and Sec-Fetch-Site checks). It
  answers a redirect to `/` with `Set-Cookie: ...; HttpOnly; SameSite=Strict; Path=/` and is then forgotten: it works
  once, so the address in the browser's history is dead; the notification's action makes a new one. The cookie lasts
  as long as the window (a new opening keeps it, so a tab already open goes on working). A cookie rather than the
  secret in every path because WebKit's pages build the WebSocket's address from `location.host` and an absolute
  path (the target list's `window.open('Main.html?ws=' + window.location.host + '/socket/1/1/WebPage')`), and the
  browser sends the cookie on that WebSocket too. Anything else is a `403` and the connection closes. A plain request
  is forwarded without its `Connection`, `Keep-Alive`, `Proxy-Connection` and `Upgrade` lines and with
  `Connection: close`, and nothing the browser sends after its head reaches WebKit: a browser may reuse a connection
  for another page's request, so every request is checked on its own connection. A WebSocket's bytes go both ways only
  after WebKit answered `101`; anything the browser sends before that ends it. At most 16 connections; a plain answer
  must end within 60 s; a WebSocket has no time limit.
- **Window lifetime:** closing that window ends its lord-web even for an app that runs in the background (no hiding
  behind the tray), so the inspector and its gate go with it; the next start is a normal one.
- **What a web page can't do while it is on:** reach WebKit's port (a bad port), reach the inspector server (not
  HTTP, above), or pass the gate: another site gets no cookie (SameSite=Strict) and is refused for its `Origin`,
  `Sec-Fetch-Site` or, through DNS rebinding, its `Host`; a page on another port of this computer is refused for its
  `Origin` or `Sec-Fetch-Site`. Checked with Chromium 156 and Chrome 157 (Firefox's list has the same ports; not
  tested).
- **What a program on this computer can still do (the residual risk):** WebKit's own HTTP port and its inspector
  server have no token, password or secret path, and WPE 2.48 takes only `<ip>:<port>` for them (no unix socket). Any
  process that can reach 127.0.0.1, of any user of this computer, can connect to them directly, read the page and its
  storage and run script in it, as before the gate. A browser can't. Nothing outside the computer can (127.0.0.1
  only). That is why it is off by default, asked every time, per window, ends with the window and with the switch,
  and why the question says so ("any program on this computer can connect to them").
- **What the page can't do:** turn it on. No D-Bus method was added to lord-web; the menu item, the question and the
  switch are the shell's, and lord-server and lord-web each check the switch.
- **Checked** (2026-11-15, development machine, a headless lord-server and a local page): the switch off refuses
  (`exit 2`); with it on, lord-web listens only on 127.0.0.1, the HTTP port's target list names the page with its
  `socket/1/1/WebPage` link, `Main.html` loads, the WebSocket upgrade answers 101 and the check opens it; without
  `--inspect` nothing listens. The gate, end to end (a private session bus with a stand-in portal and notification
  server, a headless lord-server, lord-web with WPE 2.48.3, headless Chromium 156 over the DevTools protocol, and
  curl): the opened address is the gate's with a 43-character secret and the notification carries none; without the
  cookie `/`, `Main.html` and the WebSocket are `403`; the one-time address redirects to `/` with the cookie, the
  target list and `Main.html` load through the gate, the inspector's WebSocket opens and runs script in the app's
  page (it read the page's text and a variable); the cookie isn't readable by the page; the address then answers
  `403`, and so does the one before after the notification's action made a new one; `rebind.test` (mapped to
  127.0.0.1) gets `403`; a page on another port of 127.0.0.1 opened no WebSocket to the gate nor to WebKit's port,
  read nothing and couldn't read its frame; another `Host`, `Origin`, `Sec-Fetch-Site: same-site`, `POST`, a wrong
  cookie, a pipelined second request, a 20 KB head, a head that never ends (closed after 10 s) and 20 connections at
  once (16 held) are refused or bounded, and the gate answers afterwards. curl reaches WebKit's own port directly (the
  residual risk). Chrome 157 and Chromium 156 refused all 15 backend ports (`fetch`, image and WebSocket). The menu,
  the question, Settings and the browser opening on a real session are still to be checked on screen.

## No network (issue #144; 0.8.8.9)

When the app's own page (an address inside its `scope`) can't load for lack of network, `lord-web`
(`webengine.c`, `weboffline.h`) shows its own page instead of WebKit's error page:

- **Which failures:** a main frame load whose error is GResolver's (no DNS), one of GIO's connection errors (timed
  out, host not found, host or network unreachable, connection refused, proxy failed, connection closed, not
  connected) or WebKit's transport error (`weboffline_is_network_error`). A cancelled load, a download, a TLS error,
  an HTTP answer and any address outside the scope (a sign-in on another site) keep WebKit's own handling.
- **The page:** "No connection", that the app will try again by itself, the site's host and "Press Ctrl+R to try now",
  in the person's language (`webruntime.json`: `offline.title`, `offline.body`, `offline.hint`) and the desktop's
  light or dark colors (drawn again when the mode changes). It is `about:blank` HTML whose Content-Security-Policy
  allows no script: it has no origin of the app's, and the window's permissions are denied while it is shown.
- **Trying again:** the failed address loads again after 2, 4, 8, 16 and 32 s, then every 60 s; at once when GIO's
  `GNetworkMonitor` says the network is available (the waits then start over at 2 s); at Ctrl+R or F5; after a crash
  of the web process; and when the window is shown again (`Show` from the Dock or the launcher, the tray's Open). An
  app in the background only hides when its window is closed, so before this opening it again showed the same error
  until the session ended. Only an address inside the scope is ever loaded again, one load at a time.
- **While hidden:** an app in the background keeps trying, so it is connected again (and its notifications come) by
  the time the network is back.

## Not in the first version

- Web push while the app is closed: WPE has no push service outside Apple's platforms, so the tray keeps the app
  running instead.
- DRM video and music (Widevine): not available for WPE on Linux, so services that need it can't play.
- Dark mode through CSS alone: pages that choose their colors only with `@media (prefers-color-scheme)` rules stay
  light (WPE 2.48 has no API for the scheme; "Light and dark" above). Scripts that ask `matchMedia` follow the desktop.
  For the same reason Light and Dark per app are held back until the move to WPEPlatform.
- Calls: in development ("Calls" above).
