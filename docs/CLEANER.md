# Cleaner

Cleaner explains what uses the disk and helps remove what can safely go. Released in 0.8.7-beta. The design is in `CLEANER-ARCHITECTURE.md`, the safety model in `CLEANER-SECURITY.md`, the
tests in `CLEANER-TESTS.md` and each feature's state in `CLEANER-ROADMAP.md`.

The rule is **analyse, review, plan, confirm, execute, verify**. A scan only reads.

## Sections

| Section | What it does |
|---|---|
| Scan | Disk usage and the last scan; a full scan runs Cleanup, Storage and Duplicates one after the other on a dashboard of tiles, then a review board: the total, a bar of what it is made of, and a card per module with its real rules, files or folders. During a scan the three rules with the most found so far appear, each with its size. Each step of a full scan stays on screen at least 2.6 s, and a scan started from a section at least 1.8 s, so its animation finishes on a nearly empty disk |
| Cleanup | The rules' findings by category, each with its reason, risk, action, count and size; the system's own files |
| Large files | Files found by a tree scan, filtered by size, age by modification time and type |
| Storage | Volumes (`/`, the home folder, the user's mounts); bubbles of the largest folders (the default, 14 at most, the rest grouped), each a glass sphere with an icon and colour for what the folder is, a map, a folder list with logical and allocated sizes, a summary by type. A selected folder goes to Large files to be reviewed. In the map, bands in each tile show what rebuilds by itself and what to check first, and a line above gives free space, both amounts and what wasn't scanned (`Storage map` below) |
| Duplicates | Exact copies in chosen folders, or similar images: resized and recompressed versions of a picture. Nothing is selected for the user |
| Apps | Installed apps, largest first, each removed by the package engine with its plan shown first; what removed LordOS apps left behind |
| Activity | What ran: counts and sizes, never a path |
| Settings | Excluded folders, the large file threshold, the look, the item by the clock, whether the storage map is kept |

## Rules

Compiled in `server/src/clean/rules.c`: 61 rules. Every rule applies only if its folder exists.

| Category | Rules | Risk | Action |
|---|---|---|---|
| Caches | image thumbnails, LordOS thumbnails, font cache, shader cache and database, media plugin list | regenerable | deleted |
| Caches | Flatpak app caches (`.var/app/*/cache`) | review | Trash: no lock of the running app can be read |
| Developer tools | pip, npm, yarn, Go build cache, browsers kept by Playwright and Puppeteer, Go module download cache (only `go/pkg/mod/cache/download`: extracted modules are read-only) | regenerable | deleted |
| Developer tools | pnpm store | review | Trash: projects hard-link into it |
| Developer tools | JetBrains caches, Dart pub packages, Maven repository | review | Trash: no lock can be read for the first, and a repository can hold the only copy of what you built |
| Browsers | Firefox and Chromium based browsers: disk cache, startup, script, GPU and shader caches | regenerable | deleted, never while that browser runs |
| Temporary | `.cache/tmp` over 7 days; `*.part` and `*.crdownload` in Downloads over 7 days | review | Trash |
| Reports and logs | crash reports over 7 days; npm logs and old display logs over 30 days | review | Trash |
| Old installers and recordings | installers and disk images in Downloads over 30 days (`*.deb`, `*.rpm`, `*.AppImage`, `*.iso`, `*.dmg`, `*.pkg`, `*.exe`, `*.msi`) | review | Trash |
| Old installers and recordings | videos of 100 MiB or more (`*.mp4`, `*.mov`, `*.mkv`, `*.webm`) on the Desktop, over 7 days, and in `Videos/Screencasts`, at any age | review | Trash, chosen one by one and named in the confirmation |
| Code projects | build outputs found in the home folder, eight kinds (below) | review | Trash, chosen one by one and named in the confirmation |
| AI models | Hugging Face and LM Studio models, a folder each | review | Trash, chosen one by one and named in the confirmation |
| AI models | unfinished Hugging Face downloads | regenerable | deleted, chosen one by one |
| AI models | Ollama | review | shown only: models share their blobs, so moving files can't remove one model |
| Apps | the data folder of a LordOS app that is no longer installed | review | Trash, chosen one by one and named in the confirmation; reviewed in Apps |
| Information | Downloads over 90 days, Rust registry, Gradle caches, container storage | review | shown only |
| Information | your own files in `/tmp` over 7 days | review | shown only: other programs may still use them; left out of the estimate |
| Trash | the home Trash, the Trash on mounted disks | review | measured; the real empty is one confirmed action |

Rules never cover `~/.cache` as a whole, cookies, sessions, history or passwords. The download rules follow the user's
folder: `XDG_DOWNLOAD_DIR` in `~/.config/user-dirs.dirs`, so `~/Descargas` on a Spanish system. Only a `$HOME/…` value that
would pass as a rule's root counts; anything else, or no such file, means `~/Downloads`. The Desktop rule follows
`XDG_DESKTOP_DIR` the same way, falling back to `~/Desktop`. `Videos/Screencasts` is a fixed name.

## Code projects

A folder's name proves nothing, so a rules scan walks the home folder, in the sandboxed `lord-clean`, for build outputs that prove themselves by what is next to them or inside them.

| Kind | Folders | Evidence |
|---|---|---|
| Node dependencies | `node_modules` | `package.json` next to it, and no change in the project for 60 days |
| Rust | `target` | `Cargo.toml` next to it, or `CACHEDIR.TAG` inside |
| Maven | `target` | `pom.xml` next to it |
| JavaScript | `.next`, `.turbo`, `.parcel-cache`, `.nuxt`, `.svelte-kit` | `package.json` next to it |
| Python tool caches | `.pytest_cache`, `.mypy_cache`, `.ruff_cache` | `CACHEDIR.TAG` inside |
| Python environments | `.venv`, `venv` | `pyvenv.cfg` inside, and `pyproject.toml`, `requirements.txt`, `setup.py` or `Pipfile` next to it |
| Gradle | `build`, `.gradle` | `build.gradle`, `build.gradle.kts`, `settings.gradle` or `settings.gradle.kts` next to it |
| Dart | `build`, `.dart_tool` | `pubspec.yaml` next to it |

Node's 60 days are judged over the whole project: the newest change in any file or folder of it, leaving build outputs, `.git`, `dist`, `coverage` and `__pycache__` out. A project with nothing else in it proves nothing and is not offered.

Each folder is one candidate, moved whole. They are chosen one by one, never by category, and go to the Trash and not to the real delete, because that approval covers fixed roots only. The confirmation names them and says the project needs its install or build step again. `lord-server` checks the evidence again at the plan and right before acting.

The walk doesn't enter folders starting with a dot, an unproved `node_modules`, or `go`, `snap`, `Android` and `flutter` directly in the home folder, where tools unpack other people's sources. It stops at a proved output and at 5,000,000 entries, then marks the scan incomplete.

## Similar images

Found by a separate scan of the folders the user chose, from the Duplicates section. `lord-clean` decodes JPEG, PNG, GIF (first
frame) and BMP, reduces each picture to 9 × 8 grey cells and keeps one bit per neighbouring pair: a 64-bit difference hash.
Two images are similar within 6 of those bits and 2 % in proportions, and each image joins the largest one it resembles, so a
chain of slight changes doesn't become one group. Files under 8 KiB, pictures under 32 pixels a side, files over 64 MiB and
pictures over 40 megapixels are left out, as are pictures with almost no contrast at that scale. Those that couldn't be compared
are counted on the page. A scan reads 20,000 images at most.

Similar isn't equal, so the images are shown with thumbnails, sizes and paths, nothing is selected, and one image of each group
always stays.

## Storage map

The map view colours a band rising from the bottom of each tile: what rebuilds by itself first, what to check first above it, each in proportion to the folder. They come from the rule scan's candidates laid over the tree, so they need a Cleanup scan and a Storage scan in the same session, as a full scan does. Rules that are only measured count for neither.

The line above the views gives the free space, both amounts, and what wasn't scanned: the disk's used space minus what this scan walked, so other folders, other users' files and what the system reserves. Space that goes to the Trash isn't free until the Trash is emptied, so it is never added to the free space.

**Kept between sessions.** A tree scan of the home folder or `/` writes `~/.cache/lordos/cleaner-map.txt`, mode 0600: folder names and sizes, for the session user. It is on by default. Turning off the switch in Settings removes the file, and Clear in Activity removes it with the history. Opening Storage with no scan yet shows it with its date. It holds no candidates, so nothing can be acted on from it: Large files, the summary by type, the bands, the line above the views (its date takes that place) and Review are not there until a new scan. Files in Show still opens the folder.

The file is replaced at each scan. A scan of another disk doesn't save a map, and removes the one there was.

## Apps

Only apps the package index knows as desktop applications appear, plus LordOS `.lpk` apps. Essential packages and `lordos`
itself are left out, and a plan that would take `lordos` is refused. So is a removal the engine won't do alone because other packages need that app: Cleaner shows a message and doesn't offer to take them too. Without administrator rights, Linux apps are listed with no
Remove button.

A removed `.lpk` app keeps its data folder (`~/.local/share/lordos/appdata/<id>`). That folder is a leftover only while no installed
app and no system app has that id, checked at the scan and again right before acting. Removing it is a choice by name, to the
Trash, since it may hold what the user made there.

## System files

Three closed admin actions, for administrators, each confirmed: `clean-journal`, `clean-apt-cache`, `clean-var-tmp`.
`clean-status` reads sizes for anyone. See `CLEANER-SECURITY.md`.

**Old kernels** appear for administrators when there is something to remove. Only packages named `linux-image-<version>-<flavour>`
of the running kernel's flavour count, and three stay: the running kernel, the newest, and the newest below the running one as a
way back. With a running kernel that isn't one of those packages, nothing is offered. `lord.pkg.plan` builds the removal; it is
shown only if it stays within kernel packages and leaves the running one alone, and `pkg-remove` runs it, as in the Store.

## The item by the clock

On by default, because it costs nothing when idle: a still image with no timer. Everything in its panel is read only while the
panel is open, every two seconds: disk, memory, processor load, temperature if `lord-fand` reports it, network rates and
connected disks. It makes no monitoring claim, has no memory "free up" and no speed test. Its one recommendation is the last
scan's real size. Turn it off in Cleaner → Settings.

The panel has one way into the app, the button beside the last scan (`cleanerPanelAction` in `sdk/menubarmodel.js`,
GitHub #116): **Free up** opens the cleanup when the last scan found something, and **Open Cleaner** opens the scan when
there is no scan yet. Cleaner's settings are reached from inside the app.

**The status word** (`statusWord` in `sdk/cleanerstatus.jsx`) has one written rule:

- under 10 % of the home disk free: low on space;
- otherwise, with a newer `lordos` package known to the last update check: an update is waiting. It is read once, as the panel opens, and fetches nothing;
- otherwise: good;
- with no disk reading: no word.

SMART is not part of it: reading it needs the admin helper and can wake a resting disk.

## Looks

Three looks, as data, one token object each (`sdk/cleanertheme.js`): Aurora (the default) and Cristal have one fixed look;
Noche follows the system's light or dark mode live. Chosen in Cleaner → Settings; the app and its panel use the same one.

Each section tints the window, crossfading over 700 ms. During a full scan the sidebar narrows to a rail with a step badge.

What differs from the mockup: the engine has no radial or conic gradient, so the night sky is a stretched image and the progress ring is an `Image pixels`; the
storage map is up to 28 views rather than one image, so its areas can rise in. A scan has no known total, so it shows real counters, the real path and a spinner; a
percentage appears only while a plan runs. Selecting a folder in Storage doesn't remove the folder: it opens its large files for review, as no action on a folder
you point at exists: the whole-folder candidates are AI models, leftovers and project outputs, each found by its rule. Previews are real thumbnails for images and a type icon with the extension otherwise. The hero's rings stay still and only their dots turn, the round
button sits 24 px higher, its glow is still and only its inside breathes, and busy it is circled by two dots: all for the repaint rules below. The bubble view and the review board
are not in the mockup.

A section's opening draws its pane and its object as two images (`glass-*.png`, `object-*.png`), so the object floats over the pane with a soft shadow under it. Scan screens and the
dashboard use the two together, `hero-*.png`. A folder's icon comes from its name, `folderKind` in `apps/cleaner/storage.jsx`: in the home folder the user's own folders count under their
translated names; anything else is a plain folder.

## Motion

One frame callback (`requestAnimationFrame`, `apps/cleaner/ui.jsx`) and `lord.setStyle`, no React render per frame. It runs only while the window is in front and not minimized, and
stops entirely with Reduce motion.

The engine repaints one box around all of a frame's changes, and a box that reaches a blurred surface, within 49 px of the Dock or the menu bar, grows to all of it. Stars in the
corners of the window, the hero and the round button moving in one frame repainted 87 % of the screen on every frame. So:

- Each loop has its own frames in a cycle of six (`SLOT`): the far dot, then the near dot with the object, then the button, then a star. Never two far-apart things in one frame.
- One star per update, not all of them.
- What barely moves sets a value only when a whole pixel changes.
- A frame callback that changes nothing brings no flip, and with no flip the compositor sleeps until something else wakes it. So a do-nothing timer stays pending while any job runs. Don't remove it because a headless run looks fine: there a viewer's video wakes the loop.
- Nothing that moves reaches the Dock's margin in the default window position: the button's pulse is limited for that. Moved next to the Dock, the window pays for the Dock's repaint.

Keep these when adding motion. `docs/CLEANER-TESTS.md` has the numbers.

## Files

`apps/cleaner/`, `sdk/cleaner.js`, `sdk/cleanerstatus.jsx`, `sdk/cleanertheme.js`, `sdk/oldkernels.js`, `server/src/clean/`, `server/src/ui/js_clean.c`,
`server/mk/cleaner.mk`, `assets/cleaner/` (rendered by `tools/gen-cleaner-art.py` from `docs/brand/cleaner/`).
