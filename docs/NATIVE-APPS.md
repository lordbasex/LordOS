# Installable native apps (.lpk)

Besides the apps bundled with the theme (`apps/`), LordOS installs third-party native apps from
**.lpk** packages:

- they install without root, in the home folder (`~/.local/share/lordos/apps/<id>/`);
- they open like any other LordOS app (app launcher, Dock, start menu);
- they can ship **WebAssembly** modules for code that has to be fast (see `WASM.md`).

They are installed from the Store, or by double-clicking the `.lpk` in Files.

| | |
|---|---|
| ![tienda](store-lpk-apps.png) | ![fractal](wasm-fractal.png) |
| Fractal installed from the Store (a 5 KB `.lpk` in `~/Downloads`) | Fractal open: Mandelbrot computed in WASM (16.9 M iterations in 0.9 s on the VM) and drawn with `<Image pixels>` |

## Format

An `.lpk` is a tar (ustar), compressed with gzip:

```
manifest.json      id, name, version, icon, window size, permissions
app.js             the bundled app (tools/lpk.mjs)
icon.png           required: the app's own icon (PNG, 128×128 or larger)
locales/<lang>.json   the app's texts (optional)
assets/…           what the app reads at runtime: .wasm, images, data (optional)
```

**manifest.json**

```json
{
  "id": "fractal",
  "name": { "es": "Fractal", "en": "Fractal" },
  "description": { "es": "Explorador del conjunto de Mandelbrot", "en": "Mandelbrot set explorer" },
  "version": "1.1.0",
  "icon": "icon.png",
  "size": [660, 520],
  "minSize": [360, 300],
  "permissions": []
}
```

Manifest rules:

- `id`: 2 to 40 characters, using a-z, 0-9 and `-`. It is also the namespace for the texts:
  `t('fractal.reset')` looks it up in the package's `locales/<lang>.json`.
- The id cannot be one of LordOS's own (`files`, `terminal`, `shell`, `store`…).
- `name` and `description` can be a string or an object keyed by language.
- `version` starts with a digit.
- `icon` is required: every app has its own icon. Without it, `tools/lpk.mjs` won't package and
  lord-server won't install.

When installing, lord-server rejects the package if it finds:

- symbolic links, hard links or devices;
- absolute paths, paths with `..` or with components starting with `.`;
- characters outside `A-Za-z0-9._-`;
- more than 2000 files or more than 64 MB uncompressed;
- an invalid `manifest.json`, or a missing `app.js`.

The package is written to a new folder and replaces the previous version with a `rename`: if
something fails halfway, the installed version stays as it was. Code: `server/src/lpk.c`.

## Creating an app

An installable app is just like one in `apps/`: a folder with `manifest.json` and `index.jsx`
that exports the window component and, optionally, `Icon`. It is packaged with:

```sh
node tools/lpk.mjs examples/apps/fractal            # → fractal-1.1.0.lpk
```

`tools/lpk.mjs` bundles `index.jsx` with esbuild, but **without** React or the SDK. The app uses
those of the running theme, which the SDK shares in `globalThis.__lordos.require()`:
`react`, `react/jsx-runtime`, `@lordos/sdk` and `@lordos/runtime`. That keeps the app small
(Fractal weighs 5 KB) and lets it share the SDK's state: windows, language, clipboard. `icon.png`,
`locales/` and `assets/` are added too.

The full example is in `examples/apps/fractal/`. It uses:

- a WASM module compiled from C (`src/mandel.c`; the command is in the file);
- `<Image pixels>` for drawing.

## API for installed apps

| | |
|---|---|
| `loadWasm(appId, 'assets/x.wasm', imports)` | `WebAssembly.instantiate` of a file in the package |
| `packageFile(appId, path)` | a file in the package as an `ArrayBuffer` (up to 32 MB) |
| `<Image pixels={{ width, height, data }} />` | RGBA canvas (`ArrayBuffer`, `Uint8Array` or `Uint8ClampedArray`, up to 4096×4096); redrawn when the object changes |
| `packages.list()`, `packages.inspect(path)`, `packages.install(path)`, `packages.remove(id)` | for the Store |

The rest of the SDK is the same as for the bundled apps (see `apps/README.md`).

## System apps (0.8.2)

The apps in `apps/` are **system apps**: they are built into the theme bundles (or, for those that
run as their own process, `"process": "isolated"`, into `assets/sysapps/<id>/`: Passwords, Mail,
Calendar, Contacts, Reminders and, in 0.8.5, the hidden Sandbox Probe), which LordOS checks
against its signed manifest before running them (see [SIGNING.md](SIGNING.md), "Verified system
code"), and the Store never offers to remove them ("System app — part of LordOS"). An installed
`.lpk` is never a system app, even with the same id: the theme's app wins and the package is not
listed. `isSystemApp(id)` from `@lordos/sdk` tells them apart.

**Each system app has its own version (0.8.5).** Every `apps/<id>/manifest.json` carries a `version`
(all start at `0.8.5`); `tools/build-theme.mjs` refuses an app without a valid semver one
(`MAJOR.MINOR.PATCH`, optionally `-` or `~` and a suffix). Store → Installed shows it next to the app
("LordOS app · 0.8.5") and About <app> shows it too, so the apps can later be versioned and released
apart from LordOS. A system app moves to a new version when it changes: in 0.8.6, System Information and
Settings.

## Permissions and trust (M8)

Since M8 there is **package signing** and a **permissions and isolation** model (see
[SIGNING.md](SIGNING.md) and [PRIVACY.md](PRIVACY.md)):

- **Signing**: each `.lpk` can be signed with Ed25519; LordOS verifies the hashes and the
  signature in C at install time and detects modifications. The Store and the install dialog show
  the trust level (LordOS / Verified author / Signed by <name> / Unsigned), a revoked package
  doesn't install, and there are «Modified by you», «Restore original» and «Share my version».
- **Permissions**: the manifest's `permissions` are declared and shown; the system asks the
  first time (a system-drawn prompt) and stores the decision where the app can't write it; they are
  reviewed in Settings → Privacy.
- **Isolation**: each installed app runs as **its own process** (`lord-server --app <id>`), with
  its own sandbox (Landlock + seccomp + cgroup, `server/src/sec`). It is drawn as a Wayland
  client with the theme's frame and talks to `lord-server` only through the broker, which checks
  every permission. The SDK API is the same: the app doesn't change. Its files live in
  `~/.local/share/lordos/appdata/<id>`.

What **always** limits any app (installed or not): lord-server runs without root, `lord.fs`
writes only in the home folder and the user's mounts (since 0.8.5 it reads the whole tree as the
user, except `/proc`, `/sys`, `/dev` and `/run`; an isolated app's broker keeps only its data
folder), never to the launch/install folders, the settings or `permisos.json`, and JS can't run
commands or create `.desktop` files.

In the isolated process the system file dialog (powerbox), the microphone and camera (as rings),
live theme tokens and language, menus in the menu bar and several windows are done (see
`PRIVACY.md`, «Status»).

Pending:

- a LordOS app repository for the Store, with a signed catalog (see `SIGNING.md`).
