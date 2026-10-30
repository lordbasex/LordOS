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
| `lord-clean` scanner | Built | Sandboxed, read-only; `openat2` and `statx`; bounded by entries (200,000), depth (32) and path length (1,024 bytes). Refuses to run without Landlock |
| Cancel | Built | Kills the helper; a cancelled or failed scan offers nothing |
| Compiled rule table | Partly | One rule: `thumbnails` (`.cache/thumbnails`, `*.png`, regenerable, Trash). The validator is complete for the fields that exist |
| `lord.clean` | Built | `start`, `poll`, `cancel`, `results`, `plan`, `execute`, `status`. No history |
| Plan in C with expiry | Built | Ten minutes, runs once, checked on a thread; JS holds a plan id only |
| Execution to the Trash | Built | Reopens each object from its root; per-item result; a name swapped at the last moment is put back |
| Hard links counted once | Built | Within one scan. A file with a link outside the scan estimates nothing freed |
| Free space measured after | Built | Before and after the execution. Moving to the Trash doesn't free space and the app says so |
| Overview section | Partly | Scan, counters, progress, list, selection, confirmation, result per item. See "Tested" |
| en, es, pt | Built | `cleaner.json`, `apps.json` and the `clean.*` codes in `errors.json` |
| Real delete for caches | Waiting for approval | "One decision for the maintainer" in `CLEANER-ARCHITECTURE.md`. Until then a cache moved to the Trash still uses its space |
| Other cache and temporary rules | Not started | |
| Trash section (size, emptying) | Not started | |
| Large and old files | Not started | |
| Duplicates | Not started | |
| Storage map | Not started | |
| History | Not started | |
| Preferences | Not started | |
| `CLEANER-SECURITY.md` | Not started | |

## Phases 2 and 3

Nothing is started: apps and their leftovers, the performance page, similar images, developer and AI caches, privacy
recommendations, the optional scanner, configuration audit, browser, mail and cloud modules, scheduled scans.

## Limits of what exists

- A scan reads one root only, so a hard link's other location is found only through its link count.
- A candidate is a regular file. Empty folders left behind in the root stay there.
- A name that isn't valid UTF-8 or holds control characters is never offered, and marks the scan incomplete.
- A root that is a symbolic link or a mount is not read, and marks the scan incomplete: someone with `~/.cache` on another
  disk gets nothing from this rule.
- The list shows 200 rows at a time; "Select all" takes every page.
- The Trash is the home folder's. The rule's root must be on the same disk as the home folder, or the item ends as
  *unsupported*.
- Closing the app cancels a running job and drops its results.

## Tested

See `CHANGELOG.md` for the tests run at each step. Automated: `build/clean-test` (`server/tests/clean_test.c`), in
throwaway folders under `/tmp` only, against the real `lord-clean`.

Seen on screen on VM 116 (2026-10-30), in an isolated headless session with a throwaway home folder: opening from Search,
Scan, the list, selecting two items, the confirmation, the move, the result per item and the Trash on disk. In that session
Landlock and seccomp were active in the helper and the `lordos-cleaner` scope was refused, as no `lord-sessiond` ran it.

Not tested: entering that scope under `lord-sessiond`, a real installed session, the Raspberry Pi, right-to-left layout, Spanish
and Portuguese on screen, themes other than the default, more than a few thousand files, and a package built by `mkdeb.sh`.
