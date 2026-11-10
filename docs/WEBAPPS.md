# Web apps: the LordOS Web Runtime (`lord-web`)

Web apps and PWAs (WhatsApp, Telegram, Outlook, Notion…) installed as LordOS apps, each one in its own process, with
as many accounts ("profiles") as the person wants. Approved by the maintainer on 2026-11-10 (AGENTS.md, `lord-web`).
In development for 0.8.8.8.

## What it is

- **`lord-web`** (`server/src/web/`, C): one process per app and profile. It is the only binary that links WPE WebKit:
  Debian 13's `libwpewebkit-2.0` 2.48, `libwpe` 1.16 and `wpebackend-fdo` 1.16, which bring GStreamer and libsoup. It
  ships in the optional package `lordos-web`. Without that package the rest of LordOS works unchanged and the web app
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
  `LANG`) plus `LORDOS_WEB=1`.

`lord-web` reads its app's registry file itself, read-only, and refuses to start on any mismatch. A second launch of a
running app and profile raises its window instead (xdg-activation token from lord-server).

The window's `xdg_toplevel` app id is `lordos-web.<app>.<profile>`. lord-server maps that prefix to the registry
entry, so the Dock and the window switcher show the app's name, its icon and the profile name. The title is the page's
title. The frame is LordOS's (server-side decoration).

## Adding an app from an address ("probe")

`lord.webapps.probe(url)` starts `lord-web --probe` with the address on fd 3 (one line, at most 2 KB, `https:` only):

- It loads the page in a throwaway profile (a temporary folder, removed after), without a window.
- It reads the page's web app manifest (`<link rel="manifest">`) and the icons it lists.
- It answers one JSON line on fd 3: `{ "ok", "name", "startUrl", "scope", "origin", "themeColor", "icons": [{ "src",
  "sizes", "type" }], "manifest": true|false }`, then exits.
- It is given 20 s; lord-server kills it after that.

Nothing is installed by a probe. The person sees the real origin and confirms. Then lord-server writes the registry
file and saves the icon, chosen and fetched by `lord-web --probe` as a second step, into the registry folder, after
checking it is a PNG or an SVG of at most 1 MB.

## Permissions (asked once per app, revocable in Settings → Web apps)

The page's requests (notifications, camera, microphone, screen sharing, location) reach `lord-web` as WebKit
permission requests. If the registry says `allow` or `deny`, `lord-web` answers on its own. If it says `ask`,
`lord-web` asks lord-server over the session bus:

- **Call:** `org.lordos.WebApps1.Ask(app, profile, kind) → s` on `/org/lordos/WebApps1`.
- **Answer:** `"allow"`, `"deny"` or `"once"`. lord-server shows the dialog and stores the answer unless it is `once`.

lord-server checks that the caller's pid is the `lord-web` it started for that app and profile. A page never gets an
answer from anything else.

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
  - its own firewall scope `lordos-web-<app>`, a row in Monitor with the app's name;
  - a cgroup with memory and CPU limits;
  - Landlock limited to its profile folder, the registry file read-only, the Downloads folder, and the system paths
    WebKit, GStreamer and fonts need.
- **What a page never gets:** no `lord.*`, no broker, no script injected with privileges.
- **What a crash or hang affects:** only that window. lord-server sees the process end and the Dock shows the app as
  closed.

## Not in the first version

- Web push while the app is closed: WPE has no push service outside Apple's platforms, so the tray keeps the app
  running instead.
- DRM video and music (Widevine): not available for WPE on Linux, so services that need it can't play.
- WhatsApp Web's calls: they depend on WPE's WebRTC and on the service. Still to check.
