# Web apps: the LordOS Web Runtime (`lord-web`)

Web apps and PWAs (WhatsApp, Telegram, Outlook, Notion, Spotify…: 37 in the catalog) installed as LordOS apps, each one
in its own process, with as many accounts ("profiles") as the person wants. Approved by the maintainer on 2026-11-10 (AGENTS.md, `lord-web`).
In development for 0.8.8.8.

## What it is

- **`lord-web`** (`server/src/web/`, C): one process per app and profile. It is the only binary that links WPE WebKit:
  Debian 13's `libwpewebkit-2.0` 2.48, `libwpe` 1.16 and `wpebackend-fdo` 1.16, which bring GStreamer and libsoup. It
  ships in the optional package `lordos-web` (`tools/iso/mkdeb-web.sh`, which depends on WebKit, GStreamer's good and
  bad plugins, `bubblewrap` and `xdg-dbus-proxy`; built only when `pkg-config` finds WPE, `WEB=0` skips it). Without that package the rest of LordOS works unchanged and the web app
  features say how to get it.
- **The manager** lives in what LordOS already has:
  - `lord.webapps` in lord-server (`server/src/ui/js_webapps.c`) keeps the registry and starts `lord-web`;
  - the Store has a "Web apps" section;
  - Settings has a "Web apps" page;
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
- Unknown keys are refused. The file is at most 64 KB.

Data, per app and profile, under `~/.local/share/lordos/web/<app>/<profile>/` (also in `fsops_protected`): `data/`,
`cache/`, `cookies.sqlite` and `state.json`, lord-web's own (what it learned about the page: `{ "ownSound": true }`).
Nothing of one profile is readable by another.

## Starting an app

Only lord-server starts `lord-web`:

- **Path:** fixed, next to the lord-server binary (`<lord-server dir>/lord-web`).
- **argv:** exactly `lord-web --app <app> --profile <profile>`, both checked against the registry first.
- **Environment:** only the session's (`WAYLAND_DISPLAY`, `XDG_RUNTIME_DIR`, `DBUS_SESSION_BUS_ADDRESS`, `HOME`,
  `LANG`) plus `LORDOS_WEB=1`, and `LORDOS_SESSIOND=1` when lord-server runs under lord-sessiond: lord-web then gets
  its scope from lord-sessiond and refuses to start without it or without Landlock (development runs go on). Once the
  theme has told lord-server the desktop's mode, also `LORDOS_WEB_SCHEME=light` or `dark` ("Light and dark", below).

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
- **Frames:** WebKit's web process draws into wpebackend-fdo's exportable view backend. When the compositor offers
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
  images and writes no image), so images go around it (`webxfer.c`):
  - **Pasting an image:** when the session's clipboard offers `image/png` or `image/jpeg` and no text, Ctrl+V or
    Shift+Insert reads it (at most 32 MB, non-blocking) instead of sending the key, and the page gets a `paste` event
    on its focused element with a `DataTransfer` holding the image as a `File` (`image.png`), `types` `["Files"]`.
    WebKit's pasteboard is emptied when such an image arrives, so it never pastes the text copied before it.
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
    API|image/png)` and `page copy refused (...)` with the reason.
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
  - **The app menu and Help:** Settings… opens Settings → Web apps on that app; Help starts with "<app> on the Web",
    the app's origin in the browser.
- **Light and dark** (`webscheme.h`, `webscheme.c`, `webscheme.js`, `webdark.c`; 0.8.8.8, in development): a web app
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
- **User agent** (`webpage_user_agent`): `Mozilla/5.0 (X11; Linux <machine>) AppleWebKit/605.1.15 (KHTML, like Gecko)
  Version/18.0 Safari/605.1.15 LordOS/<version>`: Linux, WebKit's own (frozen) AppleWebKit and Safari tokens, and
  LordOS's version. WebKit's site-specific quirks are off on the app's own pages and on for any other site in the
  window (a sign-in): one of those quirks hands some sites a Mac user agent whatever the setting, and a messaging web
  app then offers its Mac download. Checked 2026-11-11: that app's sign-in page loads with this user agent and quirks
  off, without the download offer.
- **Unread count:** a title that starts with `(N) ` (N 1..9999, `(99+)` is 99; `webpage_title_badge`) on the app's
  own pages is the app's unread count; lord-web sends it to lord-server with `SetBadge(app, profile, count)` on
  `org.lordos.WebApps1` (0: none) when it changes, without waiting for an answer. WPE 2.48 has no Badging API for an
  embedder (`navigator.setAppBadge` is off and reaches no signal), so the title is the only source.
- **Navigation:** an address inside `scope` stays; a link clicked to an address outside it, a new window outside it
  and `mailto:` or `tel:` go to OpenURI; a new window inside it loads in the same window. A redirect outside (a sign-in
  on another site) stays, and the title then starts with that site's host. No WebKit context menu (LordOS's own,
  above), no developer extras, no windows opened without a click, no page cache, TLS errors fail.
- **Engine:** the profile's own `WebKitNetworkSession` (`data/`, `cache/`, `cookies.sqlite`, no credential storage of
  WebKit's own), memory pressure settings (a web process past 1.5 GB frees its caches), the document cache model.
  WebKit 2.48 has no switch for its sandbox: it is always on, and lord-web refuses the variable that turns it off.
- **Closing:** without `background`, closing the window quits. With it the window is hidden (WebKit is told it is not
  visible and throttles); the Dock, the launcher or a notification shows it again, and with `tray` a
  StatusNotifierItem with the app's icon offers Open and Quit too. When the web process crashes it reloads once; a second crash within 30 s
  quits.

## Adding an app from an address ("probe")

`lord.webapps.probe(url)` starts `lord-web --probe` with the address on fd 3 (one line, at most 2 KB, `https:` only):

- It loads the page in a throwaway profile (a temporary folder, removed after), without a window.
- It reads the page's web app manifest (`<link rel="manifest">`) and the icons it lists.
- It answers one JSON line on fd 3: `{ "ok", "name", "startUrl", "scope", "origin", "themeColor", "icons": [{ "src",
  "sizes", "type" }], "manifest": true|false }`, then exits.
- It is given 20 s; lord-server kills it after that.

Nothing is installed by a probe. The person sees the real origin and confirms. Then lord-server writes the registry
file and saves the icon, chosen and fetched by `lord-web --probe-icon` as a second step, into the registry folder,
after checking it is a PNG or an SVG of at most 1 MB. lord-server chooses it from the answer's `icons`: the largest PNG
up to 512 px, else the smallest one above, else an SVG (`https:` only). The second step starts right after the first
(the sheet then shows the icon), and has its own 20 s.

How the two steps are built (`server/src/web/webprobe.c`, `webmanifest.c`, `webargs.c`):

- **The request** on fd 3 is one line of at most 2 KB ending in a newline, nothing after it, an `https:` address (or
  `http:` on `localhost`, `127.0.0.1` or `[::1]`, for tests). Anything else is answered `web.probe.request` without
  loading anything. lord-web closes every other inherited fd and none of WebKit's children get fd 3.
- **`--probe`** loads the page in an ephemeral network session (nothing written to disk) with WebKit's sandbox, no
  window (its frames are given back at once), no permission, no new window, no download. 1.2 s after the load it runs a
  script in an isolated world (the page's scripts can't see or change it) that reads the `<link rel="manifest">`, the
  `theme-color` meta, the title and the `<link rel="icon">`s, and fetches the manifest with the page's credentials
  rules. lord-web then checks it all against the address WebKit really loaded (after redirects): the origin is the
  loaded page's, a `start_url` on another origin becomes the page itself, a `scope` that doesn't hold the start address
  becomes the start address's folder, the name is cleaned (no control characters, spaces folded, at most 128 bytes),
  colors become `#rrggbb`, at most 16 icons with `https:` addresses. Without a manifest the icons are the page's.
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
- **`--probe-icon`** takes one icon address the same way, downloads it into a temporary folder (removed after) and
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
  the app runs.
  `background` is on for chat, mail, calendar and music (closing the window keeps the music playing).
- **Icons** are LordOS's own letter tiles, never the service's logo: 128×128 RGBA PNG, a rounded square (radius 24)
  in a color close to the service's, its first letter in white DejaVu Sans Bold with a 50 px cap height from y = 39.
  The letter is the product's own word when the name starts with its company's family name ("Google Drive" → D,
  "Microsoft Teams" → T, "YouTube Music" → M). They are the shipped fallback when an app's own icon isn't fetched.

## The Store: Discover, the first install and an app's page

- **Where web apps show:** the Web apps section, a banner and a shelf in Discover (the catalog's first apps; "See all"
  opens the section), Discover's search, and their category's page. Every card has a "Web app" tag, so web apps are
  told apart from LordOS apps and Linux apps (`apps/store/webapps.js`).
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
  program run), the permissions with their state (a link to Settings → Web apps), the profiles (each one's data and
  memory, Open, New profile), background mode and the known limitations.
- **The catalog's data for that page** (all optional, checked by `tests/webapps_model_test.mjs`): `publisher`;
  `verified` (`YYYY-MM-DD`, the day LordOS last checked the app on lord-web: left out until it is, and the page says
  "not tested yet"); `typicalMemoryMB` (one profile's Pss measured on LordOS: WhatsApp's 810 MB from the first check);
  `limitations` (keys of `webapps.limit.*`: `drm`, `calls`, `push`). lord-server's own copy ignores them.
- **Memory now:** `lord.webapps.memory(app)` (read-only) sums `Pss` from `/proc/<pid>/smaps_rollup` of each running
  lord-web of the app and of every process under it (WebKit's web and network processes, bubblewrap), by profile:
  one pass over `/proc` for the parents, at most 64 processes (`server/src/web/webmem.c`, `tests/webmem_test.c` on a
  made-up `/proc`). A process it can't read is counted apart, never guessed. The page reads it every 5 s while open;
  when the app isn't running it shows the catalog's measured figure, if any.

## Permissions (asked once per app, revocable in Settings → Web apps)

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

LordOS's sound (`notifySound`, `webpage_sound_suppress`): with `"auto"`, lord-web watches WebKit's
`is-playing-audio`. A page that starts audio less than 1.5 s before or after one of its notifications plays its own
sound: that notification (when it came first) and every later one go with the `suppress-sound` hint, so LordOS
doesn't play a second sound. lord-web remembers it for the profile (`state.json`, `"ownSound": true`), so after the
first time nothing waits or guesses. notifyd passes `suppress-sound` to the shell, which then shows the banner
silently.

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
  - it refuses to start on any mismatch: argv other than the three forms, a registry file that is a link, over 64 KB,
    with an unknown key, a wrong type, a bad id or address, a scope that isn't a prefix of `startUrl` or not on
    `origin`, or an app or profile that isn't the one asked for (`webargs.c`, `webreg.c`, `weburl.c`, tested by
    `tests/webapp_test.c`); and when `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS` is set. It drops the variables that
    point WebKit or GStreamer at other code (`WEBKIT_*`, `WPE_*`, `GST_PLUGIN*`, `GIO_EXTRA_MODULES`, `LD_PRELOAD`…).
- **What a page never gets:** no `lord.*`, no broker, no script injected with privileges. The one script of lord-web's
  a page can see is the clipboard's `write` and `writeText` that WPE lacks, and what it hands over reaches the
  session only right after the person's own key or click in the window. The files of a drop are only the ones the
  person dropped on the window, read by lord-web, never by the page.
- **What a crash or hang affects:** only that window. lord-server sees the process end and the Dock shows the app as
  closed.

## Performance (0.8.8.8, in development)

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
  Quit). Settings → Web apps → "Show in the menu bar", `lord.webapps.setTray(app, on)`. Background mode
  (`background`, "Keep running when closed") stays its own switch.
- `notifySound` (`"auto"` when missing, written only when it isn't): `"auto"` LordOS's sound only when the page plays
  none of its own (lord-web learns it and then sends the `suppress-sound` hint), `"app"` the page's sound only,
  `"system"` always LordOS's, `"none"` no sound from LordOS. `lord.webapps.setNotifySound(app, value)`; Settings → Web
  apps → Notification sound, and the same choice in Settings → Notifications (one value for both pages). LordOS
  controls only its own sound: to silence the page's own sound, use the site's settings.
- A profile is `{ id, name }` or `{ id, name, color }`, `color` one of LordOS's tag colors (Files' tags, Databases'
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
`webapps.noIcon` and the probe's.

- **When:** the Store's cards ask for it lazily (`wantSiteIcon`), one probe at a time, never twice in a session for an
  app that failed; an installed catalog app without one gets it when it is launched (never blocking the launch);
  Settings → Web apps → Refresh icon fetches it again. Without lord-web the letter tile stays.
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
too; the Dock's window list shows each profile's own count. Settings → Web apps → "Show unread count on the Dock"
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
- **One Dock icon per profile** (Settings → Web apps, off by default, `settings.webAppPrefs[app].dockPerProfile`):
  while a profile other than the first has windows, it has an icon of its own (not pinnable), with its own count; the
  first profile stays on the app's icon.
- **Each profile's data** in Settings → Web apps is measured again when a profile is added or starts running and every
  30 s: before, a profile added during the visit showed "none yet" until Settings was opened again, because its folder
  only exists after its first start.

## Not in the first version

- Web push while the app is closed: WPE has no push service outside Apple's platforms, so the tray keeps the app
  running instead.
- DRM video and music (Widevine): not available for WPE on Linux, so services that need it can't play.
- Dark mode through CSS alone: pages that choose their colors only with `@media (prefers-color-scheme)` rules stay
  light (WPE 2.48 has no API for the scheme; "Light and dark" above). Scripts that ask `matchMedia` follow the desktop.
- WhatsApp Web's calls: they depend on WPE's WebRTC and on the service. Still to check.
