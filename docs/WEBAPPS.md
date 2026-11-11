# Web apps: the LordOS Web Runtime (`lord-web`)

Web apps and PWAs (WhatsApp, Telegram, Outlook, Notion…) installed as LordOS apps, each one in its own process, with
as many accounts ("profiles") as the person wants. Approved by the maintainer on 2026-11-10 (AGENTS.md, `lord-web`).
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
- `background`: closing the window keeps the app running behind its tray icon (for notifications), until Quit.
- Unknown keys are refused. The file is at most 64 KB.

Data, per app and profile, under `~/.local/share/lordos/web/<app>/<profile>/` (also in `fsops_protected`): `data/`,
`cache/` and `cookies.sqlite`. Nothing of one profile is readable by another.

## Starting an app

Only lord-server starts `lord-web`:

- **Path:** fixed, next to the lord-server binary (`<lord-server dir>/lord-web`).
- **argv:** exactly `lord-web --app <app> --profile <profile>`, both checked against the registry first.
- **Environment:** only the session's (`WAYLAND_DISPLAY`, `XDG_RUNTIME_DIR`, `DBUS_SESSION_BUS_ADDRESS`, `HOME`,
  `LANG`) plus `LORDOS_WEB=1`, and `LORDOS_SESSIOND=1` when lord-server runs under lord-sessiond: lord-web then gets
  its scope from lord-sessiond and refuses to start without it or without Landlock (development runs go on).

`lord-web` reads its app's registry file itself, read-only, and refuses to start on any mismatch. A second launch of a
running app and profile raises its window instead: lord-server knows its process, starts nothing, issues an
xdg-activation token and calls `Show(token)` on that lord-web (below); lord-web activates its window with it, or maps it
again when it was hidden behind its tray icon. The shell does not focus it on its own.

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
- **Input:** the compositor's keymap (xkbcommon) and key repeat, the pointer (side buttons go back and forward), the
  wheel in lines for a mouse and in pixels for a touchpad, touch (up to 10 points). Ctrl+R/F5 reload, Alt+Left/Right go
  back and forward, Ctrl+plus/minus/0 zoom. text-input-v3 is WebKit's input method context (preedit, commit, the
  cursor rectangle and the surrounding text).
- **Clipboard:** text, both ways, through `wl_data_device`: the session's selection is copied into WebKit's pasteboard
  while the window has the keyboard; what the page copies is offered to the session as plain text (and HTML when the
  page gave it). Dragging into the page is not passed on yet.
- **Navigation:** an address inside `scope` stays; a link clicked to an address outside it, a new window outside it
  and `mailto:` or `tel:` go to OpenURI; a new window inside it loads in the same window. A redirect outside (a sign-in
  on another site) stays, and the title then starts with that site's host. No context menu, no developer extras, no
  windows opened without a click, no page cache, TLS errors fail.
- **Engine:** the profile's own `WebKitNetworkSession` (`data/`, `cache/`, `cookies.sqlite`, no credential storage of
  WebKit's own), memory pressure settings (a web process past 1.5 GB frees its caches), the document cache model.
  WebKit 2.48 has no switch for its sandbox: it is always on, and lord-web refuses the variable that turns it off.
- **Closing:** without `background`, closing the window quits. With it the window is hidden (WebKit is told it is not
  visible and throttles) and a StatusNotifierItem with the app's icon offers Open and Quit; a click on the icon or on
  a notification shows the window again. When the web process crashes it reloads once; a second crash within 30 s
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

## Permissions (asked once per app, revocable in Settings → Web apps)

The page's requests (notifications, camera, microphone, screen sharing, location) reach `lord-web` as WebKit
permission requests. If the registry says `allow` or `deny`, `lord-web` answers on its own. If it says `ask`,
`lord-web` asks lord-server over the session bus:

- **Call:** `org.lordos.WebApps1.Ask(app, profile, kind) → s` on `/org/lordos/WebApps1`.
- **Answer:** `"allow"`, `"deny"` or `"once"`. lord-server shows the dialog and stores the answer unless it is `once`.

lord-server checks that the caller's pid is the `lord-web` it started for that app and profile. A page never gets an
answer from anything else.

`lord-web` reads its registry file again before each request, so an answer lord-server stored since the app started
counts. No answer (no lord-server, an error, anything but the three words) is `deny`. A camera and microphone request
asks both kinds and needs both. The page's `navigator.permissions` queries get the registry's word (`allow` granted,
`ask` prompt, `deny` denied); the names of cameras and microphones only when one of them is allowed. Storage access,
media keys and anything else are denied. Notifications carry the app's name (with the profile's when it has more than
one) and icon, and a notification with the same tag replaces the one before it.

## What `lord-web` uses of the session (like any Linux app)

- **Files:** WebKit's file chooser goes through the FileChooser portal. Downloads go to the XDG Downloads folder only,
  with a notification when done.
- **Notifications:** `org.freedesktop.Notifications` (`notifyd`). A click opens that app's window.
- **Links outside `scope`:** OpenURI, so they open in the person's browser.
- **Screen sharing:** the ScreenCast portal (`docs/WAYLAND.md`).
- **Background:** a StatusNotifierItem tray icon with Open and Quit when `background` is on.
- **The clipboard and the keyboard:** a normal Wayland client.

## Isolation

- **WebKit's sandbox:** the web content and network processes run under bubblewrap and seccomp, and are never turned
  off.
- **The `lord-web` process:**
  - its own firewall scope `lordos-web-<app>` (`app-lordos-lordos-web-<app>.scope`, placed by lord-sessiond like any
    launched app; `lordos-web…` is reserved, so no `.desktop` can take that identity), a row in Monitor with the app's
    name. Inside a LordOS session it refuses to start when it can't be placed;
  - a cgroup with memory and CPU limits (memory.high 2 GB, memory.max 4 GB, two cores), shared with WebKit's processes,
    which are its children;
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
- **What a page never gets:** no `lord.*`, no broker, no script injected with privileges.
- **What a crash or hang affects:** only that window. lord-server sees the process end and the Dock shows the app as
  closed.

## Not in the first version

- Web push while the app is closed: WPE has no push service outside Apple's platforms, so the tray keeps the app
  running instead.
- DRM video and music (Widevine): not available for WPE on Linux, so services that need it can't play.
- WhatsApp Web's calls: they depend on WPE's WebRTC and on the service. Still to check.
