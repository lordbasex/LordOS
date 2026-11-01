# Cleaner: roadmap

The real state of each feature. The design is in `CLEANER-ARCHITECTURE.md`; this file only says what exists.
Last updated 2026-11-01. Released in 0.8.7-beta.

## State key

- **Built**: the code exists and its automated test passes.
- **Partly**: part of it exists; the row says which part.
- **Not started**: no code exists.
- **Waiting for approval**: not built on purpose, until the maintainer decides.

## Phase 1, the core

| Feature | State | Notes |
|---|---|---|
| `lord-clean` scanner | Built | Rules, tree and duplicates modes |
| Compiled rule table | Built | 61 rules; `CLEANER.md` lists them. The 18 added on 2026-10-31 are in the rows below |
| `lord.clean` | Built | The history holds no paths, by design. The kept storage map does hold folder names: see its row |
| Plan in C with expiry | Built | Ten minutes, runs once |
| Execution to the Trash | Built | Restore per item in the result |
| Real delete for caches | Built | Approved 2026-10-30. Regenerable rules only |
| Trash (size, emptying) | Built | The real empty, on a thread |
| Large and old files | Built | Files of at least 1 MiB; 50,000 at most |
| Storage map | Built | Up to 100,000 folders, 5,000,000 entries; 28 areas per level |
| Duplicates | Built | 1,024 files shown per group |
| History | Built | Counts and sizes, 100 entries |
| Preferences | Built | Exclusions, threshold, look, item by the clock |
| Item by the clock and panel | Built | Still item; reads only while open |
| Looks | Built | Aurora (default), Noche, Cristal |
| Scan dashboard and review board | Built | Cleanup, Storage and Duplicates in turn; the board's cards show real parts |
| Motion budget | Built | Frame times on VM 116 and a Raspberry Pi 5 in `CLEANER-TESTS.md`: 5 to 7 % of a core idle, 12 to 18 % scanning. Aurora's pauses of up to a second while scanning are not explained yet |
| Bubble view | Built | Glass spheres with an icon per kind of folder. Selecting a folder reviews its large files; no folder action |
| Similar images | Built | Decoded only in `lord-clean`; JPEG, PNG, GIF and BMP; 20,000 images at most; to the Trash, one image of each group always stays |
| Apps | Built | Installed apps by size, removed by the package engine or `.lpk` removal, with a row in Activity; leftovers of removed LordOS apps. A real removal of an installed Linux app and its Activity row ran on VM 125 on 2026-10-31 |
| System files | Built | Three closed admin actions plus `clean-status`. Not run on the VM; all four ran as root in a throwaway emulated arm64 system on 2026-10-30 |
| Status word: updates | Built | The panel reads the `lordos` package's state when it opens. The word itself was seen only with that state stubbed: `lordos` isn't a package on the VM |
| Translated Downloads folders | Built | From `XDG_DOWNLOAD_DIR`, only a `$HOME/…` value that passes as a rule's root |
| Old kernels | Built | Chosen in `sdk/oldkernels.js`, shown through `lord.pkg.plan`, run by `pkg-remove`. The real plan was read on the VM; the removal was not run |
| System temporary folder | Built | Your own files over a week old, measured only |
| Code project outputs | Built | Eight kinds, proved by evidence, checked again at plan and execution, to the Trash. Covered in `clean-test` |
| Tool and installer rules | Built | Playwright, Puppeteer, Go module downloads, JetBrains, Dart pub, Maven repository, Flatpak app caches, old installers, big videos. Covered in `clean-test` |
| Bands and summary on the map | Built | Covered in `clean-test` |
| What each rule finds, during a scan | Partly | Code exists; no case in `clean-test` yet |
| Kept storage map | Built | `~/.cache/lordos/cleaner-map.txt`, 0600, on by default, with a switch in Settings. Covered in `clean-test` |
| Minimum time per scan step | Partly | 2.6 s a step of a full scan, 1.8 s a section's scan. Code exists; no case in `clean-test` yet |

## Phases 2 and 3: what has no row above

| Feature | State | Notes |
|---|---|---|
| Performance page reusing Monitor and Disks | Not started | |
| Privacy recommendations | Not started | Needs a written list of what is recommended first |
| Scheduled scans that only notify | Waiting for approval | A timer in the session; nothing resident exists today |
| Configuration audit | Waiting for approval | |
| Browser, mail and cloud modules | Waiting for approval | |
| Optional malware scanner | Waiting for approval | Would be a new dependency; Cleaner makes no such claim today |

## Limits of what exists

- A hard link's other location is found only through its link count.
- A candidate is a regular file, or a whole folder for a model, a leftover or a project output. Empty folders left behind stay.
- A name that isn't valid UTF-8 or holds control characters is never offered, and marks the scan incomplete.
- A root that is a symbolic link or a mount is not read: someone with `~/.cache` on another disk gets nothing from those rules.
- Similar images doesn't cover WebP, HEIC or camera raw files, and ignores EXIF rotation, so a turned copy isn't found. Different screenshots of the same desktop can group together: that is why nothing is selected and every image is shown.
- A full scan doesn't include similar images: it reads every picture, so it is started on its own.
- A leftover is only a removed LordOS app's data folder. What a Linux app leaves in `~/.config` or `~/.cache` can't be tied to the app with certainty, so it isn't offered.
- `/tmp` is measured, never offered: which of the files there a program still uses can't be shown, and the Trash can't take them from another disk.
- An absolute `XDG_DOWNLOAD_DIR` or `XDG_DESKTOP_DIR` isn't followed: those rules then use `~/Downloads` or `~/Desktop`.
- A `.deb` cache is often empty on LordOS: the package engine deletes its own downloads.
- Closing the app cancels a running job and drops its results. The kept storage map stays, by design.
- A project's evidence is a name of a file, not proof that it can be built again: that is why outputs are chosen by name and go to the Trash.
- A scan of another disk removes the kept map and saves none.
- The walk's cost on a large home folder has not been measured.

## Tested

See `CLEANER-TESTS.md`. On 2026-10-30, in a throwaway emulated arm64 machine (`ISO.md`, "The arm64 ISO"), the scope appeared under `lord-sessiond` in a real session and the four admin actions ran as root (`CLEANER-TESTS.md`). On 2026-10-31 a full test with a real pointer ran on VM 125, and the session's cost and the display's real flips were measured on a Raspberry Pi 5 with its monitor (`CLEANER-TESTS.md`). Not tested: a real desktop computer, removing an app or a kernel for real. Not explained yet: Aurora's pauses of up to a second.
