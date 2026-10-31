# Cleaner: roadmap

The real state of each feature. The design is in `CLEANER-ARCHITECTURE.md`; this file only says what exists.
Last updated 2026-10-30, branch `v087`, in development. The latest release is 0.8.6-beta and contains none of this.

## State key

- **Built**: the code exists and its automated test passes.
- **Partly**: part of it exists; the row says which part.
- **Not started**: no code exists.
- **Waiting for approval**: not built on purpose, until the maintainer decides.

## Phase 1, the core

| Feature | State | Notes |
|---|---|---|
| `lord-clean` scanner | Built | Rules, tree and duplicates modes |
| Compiled rule table | Built | 41 rules; `CLEANER.md` lists them |
| `lord.clean` | Built | No history of paths, by design |
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
| Looks | Built | Noche, Aurora, Cristal |
| Scan dashboard and review board | Built | Cleanup, Storage and Duplicates in turn |
| Bubble view | Built | Selecting a folder reviews its large files; no folder action |
| Similar images | Not started | Phase 2 |
| Apps | Not started | Phase 2 |
| System files | Built | Three closed admin actions, not run on the VM |
| Status word: updates | Partly | The rule exists; the panel doesn't read the update state |
| Translated Downloads folders | Not started | Only `Downloads` is covered |
| Old kernels | Not started | Goes through `lord.pkg.plan`, never custom logic |

## Limits of what exists

- A hard link's other location is found only through its link count.
- A candidate is a regular file, or a whole folder for a model. Empty folders left behind stay.
- A name that isn't valid UTF-8 or holds control characters is never offered, and marks the scan incomplete.
- A root that is a symbolic link or a mount is not read: someone with `~/.cache` on another disk gets nothing from those rules.
- Lists draw a fixed window of rows and don't yet know the box's real height, so the last screenful of a long list needs a little extra scrolling room.
- A `.deb` cache is often empty on LordOS: the package engine deletes its own downloads.
- Closing the app cancels a running job and drops its results.

## Tested

See `CLEANER-TESTS.md`. On 2026-10-30, in a throwaway emulated arm64 machine (`ISO.md`, "The arm64 ISO"), the scope appeared under `lord-sessiond` in a real session and the four admin actions ran as root (`CLEANER-TESTS.md`). Not tested: the Raspberry Pi, a real desktop computer, paint cost and on-screen frame times.
