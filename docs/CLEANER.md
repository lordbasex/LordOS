# Cleaner

Cleaner explains what uses the disk and helps remove what can safely go. In development for 0.8.7: the latest release,
0.8.6-beta, contains none of it. The design is in `CLEANER-ARCHITECTURE.md`, the safety model in `CLEANER-SECURITY.md`, the
tests in `CLEANER-TESTS.md` and each feature's state in `CLEANER-ROADMAP.md`.

The rule is **analyse, review, plan, confirm, execute, verify**. A scan only reads.

## Sections

| Section | What it does |
|---|---|
| Scan | Disk usage and the last scan; a full scan runs Cleanup, Storage and Duplicates one after the other on a dashboard of tiles, then a review board of what was really found |
| Cleanup | The rules' findings by category, each with its reason, risk, action, count and size; the system's own files |
| Large files | Files found by a tree scan, filtered by size, age by modification time and type |
| Storage | Volumes (`/`, the home folder, the user's mounts); bubbles of the largest folders (the default, 14 at most, the rest grouped), a map, a folder list with logical and allocated sizes, a summary by type. A selected folder goes to Large files to be reviewed |
| Duplicates | Exact copies in chosen folders, or similar images: resized and recompressed versions of a picture. Nothing is selected for the user |
| Apps | Installed apps, largest first, each removed by the package engine with its plan shown first; what removed LordOS apps left behind |
| Activity | What ran: counts and sizes, never a path |
| Settings | Excluded folders, the large file threshold, the look, the item by the clock |

## Rules

Compiled in `server/src/clean/rules.c`. Every rule applies only if its folder exists.

| Category | Rules | Risk | Action |
|---|---|---|---|
| Caches | image thumbnails, LordOS thumbnails, font cache, shader cache and database, media plugin list | regenerable | deleted |
| Developer tools | pip, npm, yarn, Go build cache | regenerable | deleted |
| Developer tools | pnpm store | review | Trash: projects hard-link into it |
| Browsers | Firefox and Chromium based browsers: disk cache, startup, script, GPU and shader caches | regenerable | deleted, never while that browser runs |
| Temporary | `.cache/tmp` over 7 days; `*.part` and `*.crdownload` in Downloads over 7 days | review | Trash |
| Reports and logs | crash reports over 7 days; npm logs and old display logs over 30 days | review | Trash |
| AI models | Hugging Face and LM Studio models, a folder each | review | Trash, chosen one by one and named in the confirmation |
| AI models | unfinished Hugging Face downloads | regenerable | deleted, chosen one by one |
| AI models | Ollama | review | shown only: models share their blobs, so moving files can't remove one model |
| Apps | the data folder of a LordOS app that is no longer installed | review | Trash, chosen one by one and named in the confirmation; reviewed in Apps |
| Information | Downloads over 90 days, Rust registry, Gradle caches, container storage | review | shown only |
| Information | your own files in `/tmp` over 7 days | review | shown only: other programs may still use them; left out of the estimate |
| Trash | the home Trash, the Trash on mounted disks | review | measured; the real empty is one confirmed action |

Rules never cover `~/.cache` as a whole, cookies, sessions, history or passwords. The download rules follow the user's
folder: `XDG_DOWNLOAD_DIR` in `~/.config/user-dirs.dirs`, so `~/Descargas` on a Spanish system. Only a `$HOME/…` value that
would pass as a rule's root counts; anything else, or no such file, means `~/Downloads`.

## Similar images

Found by a separate scan of the folders the user chose, from the Duplicates section. `lord-clean` decodes JPEG, PNG, GIF (first
frame) and BMP, reduces each picture to 9 × 8 grey cells and keeps one bit per neighbouring pair: a 64-bit difference hash.
Two images are similar within 6 of those bits and 2 % in proportions, and each image joins the largest one it resembles, so a
chain of slight changes doesn't become one group. Files under 8 KiB, pictures under 32 pixels a side, files over 64 MiB and
pictures over 40 megapixels are left out, as are pictures with almost no contrast at that scale. Those that couldn't be compared
are counted on the page. A scan reads 20,000 images at most.

Similar isn't equal, so the images are shown with thumbnails, sizes and paths, nothing is selected, and one image of each group
always stays.

## Apps

Only apps the package index knows as desktop applications appear, plus LordOS `.lpk` apps. Essential packages and `lordos`
itself are left out, and a plan that would take `lordos` is refused. Without administrator rights, Linux apps are listed with no
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

**The status word** (`statusWord` in `sdk/cleanerstatus.jsx`) has one written rule:

- under 10 % of the home disk free: low on space;
- otherwise, with a newer `lordos` package known to the last update check: an update is waiting. It is read once, as the panel opens, and fetches nothing;
- otherwise: good;
- with no disk reading: no word.

SMART is not part of it: reading it needs the admin helper and can wake a resting disk.

## Looks

Themes are data, one token object each (`sdk/cleanertheme.js`). Noche is the default and follows the system's light or dark mode live.
Aurora and Cristal, chosen in Settings, have one fixed look. The app and its panel use the same one.

Each section tints the window, crossfading over 700 ms. During a full scan the sidebar narrows to a rail with a step badge.

What differs from the mockup: the engine has no radial or conic gradient, so the night sky is a stretched image and the progress ring is an `Image pixels`; the
storage map is up to 28 views rather than one image, so its areas can rise in. A scan has no known total, so it shows real counters, the real path and a spinner; a
percentage appears only while a plan runs. Selecting a folder in Storage doesn't remove the folder: it opens its large files for review, as no action on whole folders
exists outside AI models. Previews are real thumbnails for images and the file's extension otherwise.

Motion uses one timer and `lord.setStyle`, runs only while the window is in front, and stops entirely with Reduce motion.

## Files

`apps/cleaner/`, `sdk/cleaner.js`, `sdk/cleanerstatus.jsx`, `sdk/cleanertheme.js`, `sdk/oldkernels.js`, `server/src/clean/`, `server/src/ui/js_clean.c`,
`server/mk/cleaner.mk`, `assets/cleaner/` (rendered by `tools/gen-cleaner-art.py` from `docs/brand/cleaner/`).
