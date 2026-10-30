# Cleaner

Cleaner explains what uses the disk and helps remove what can safely go. In development for 0.8.7: the latest release,
0.8.6-beta, contains none of it. The design is in `CLEANER-ARCHITECTURE.md`, the safety model in `CLEANER-SECURITY.md`, the
tests in `CLEANER-TESTS.md` and each feature's state in `CLEANER-ROADMAP.md`.

The rule is **analyse, review, plan, confirm, execute, verify**. A scan only reads.

## Sections

| Section | What it does |
|---|---|
| Scan | Disk usage and the last scan; runs the rule scan |
| Cleanup | The rules' findings by category, each with its reason, risk, action, count and size; the system's own files |
| Large files | Files found by a tree scan, filtered by size, age by modification time and type |
| Storage | Volumes (`/`, the home folder, the user's mounts); a map of folders, a folder list with logical and allocated sizes, a summary by type |
| Duplicates | Exact copies in chosen folders; nothing is selected for the user |
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
| Information | Downloads over 90 days, Rust registry, Gradle caches, container storage | review | shown only |
| Trash | the home Trash, the Trash on mounted disks | review | measured; the real empty is one confirmed action |

Rules never cover `~/.cache` as a whole, cookies, sessions, history or passwords. Only `Downloads` is scanned under its
English name; a translated folder name gets no download rules yet.

## System files

Three closed admin actions, for administrators, each confirmed: `clean-journal`, `clean-apt-cache`, `clean-var-tmp`.
`clean-status` reads sizes for anyone. Old kernels stay with the Store's package plan. See `CLEANER-SECURITY.md`.

## The item by the clock

On by default, because it costs nothing when idle: a still image with no timer. Everything in its panel is read only while the
panel is open, every two seconds: disk, memory, processor load, temperature if `lord-fand` reports it, network rates and
connected disks. It makes no monitoring claim, has no memory "free up" and no speed test. Its one recommendation is the last
scan's real size. Turn it off in Cleaner → Settings.

**The status word** (`statusWord` in `sdk/cleanerstatus.jsx`) has one written rule:

- under 10 % of the home disk free: low on space;
- otherwise, with a LordOS update waiting: an update is waiting. The panel doesn't read the update state yet, so this word is not shown today;
- otherwise: good;
- with no disk reading: no word.

SMART is not part of it: reading it needs the admin helper and can wake a resting disk.

## Looks

Themes are data, one token object each (`sdk/cleanertheme.js`). Noche is the default and follows the system's light or dark mode live.
Aurora and Cristal, chosen in Settings, have one fixed look. The app and its panel use the same one.

The engine has no radial or conic gradient. So the night sky is a stretched image, the progress ring is an `Image pixels`, and the
storage map is up to 28 views rather than one image, so its areas can rise in. A scan has no known total, so it shows real
counters and a spinner; a percentage appears only while a plan runs.

Motion uses one timer and `lord.setStyle`, runs only while the window is in front, and stops entirely with Reduce motion.

## Files

`apps/cleaner/`, `sdk/cleaner.js`, `sdk/cleanerstatus.jsx`, `sdk/cleanertheme.js`, `server/src/clean/`, `server/src/ui/js_clean.c`,
`server/mk/cleaner.mk`, `assets/cleaner/` (rendered by `tools/gen-cleaner-art.py` from `docs/brand/cleaner/`).
