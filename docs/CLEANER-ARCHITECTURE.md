# Cleaner: architecture

**Status: design for 0.8.7, with Phase 1 built (`CLEANER-ROADMAP.md`).**
Checked against the code on branch `v087` on 2026-10-30. Each feature's real state lives in `CLEANER-ROADMAP.md`; a line
here is a plan, not a claim.

Cleaner is a native app that explains what uses the disk and helps remove what can safely go. The rule is
**analyse, review, plan, confirm, execute, verify**. A scan never changes anything, and nothing is selected for the
user in categories that hold personal data.

## What already exists, and what it can't do

| Need | Existing | Gap |
|---|---|---|
| Walk a tree | `lord.fs.du`, `list`, `search` | Synchronous on the compositor's thread, no progress or cancel, `du` sums `st_size`, counts hard links twice and crosses mounts |
| File identity | `lord.fs.stat` | No device, inode, link count or allocated blocks |
| Volumes | `lord.fs.volumes`, `free` | Only the user's mounts, not `/` or the home folder |
| Send to the Trash | `trash`, `trashList`, `restore`, `removeFromTrash`, `emptyTrash` | Good. Other disks refuse (`fs.trashOtherDisk`); `emptyTrash` is synchronous |
| Hashing | `pkg/crypto.h` streaming SHA-256, hardware accelerated | Good; no new dependency |
| Helper lifecycle | `bench/lbrun.c` (one job, thread, poll, kill), `doc/doc.c` (JSON channel, sealed memfds) | Good |
| Read-only folders in a sandbox | `SandboxHelperCfg.ro_dir_fds` | Covers a whole subtree, with no exclusions |
| Remove packages | `lord.pkg.plan` (read-only), `orphans`, admin `pkg-remove`, `pkg-autoremove` | Good: Cleaner calls these and adds no package logic |
| Thumbnails | `media.thumb`, `decodeImage` | Good |
| Drawing | `Image pixels`, `lord.getLayout` | Monitor's helpers are app-local: the treemap has its own |
| Long lists | Logs' `VirtualList` | App-local |
| Settings, notices, Show in Files | `useSettings`, `notify`, `revealPath` | Good |

Two facts change the original proposal. The package engine deletes its `.deb` files after installation, so it leaves no
download cache to clean. And `lord.pkg.plan` already gives the read-only removal plan.

## Pieces

```
apps/cleaner/            the interface (JSX, @lordos/sdk)
sdk/cleaner.js           the SDK module over lord.clean
server/src/ui/js_clean.c lord.clean: start, poll, cancel, results, plan, execute, status
server/src/clean/        the lord-server side: job thread, rule table, plans, execution
server/src/clean/helper/ lord-clean, the sandboxed scanner
assets/locales/*/cleaner.json
```

### The app runs in the session

Cleaner is a built-in app, not an isolated one. An isolated app only has its data folder, so it would need a new family
of broker operations for all of this, while the authority here belongs in C anyway. The JS holds no power beyond
`lord.clean`, and that module can be brokered later without changing the app.

### `lord-clean`, the scanner

A helper in the pattern of `lord-bench` and `lord-doc`: fixed path next to `lord-server`, no arguments, minimal
environment, channel on fd 3, its own scope (`lordos-cleaner`). lord-server opens each root as an `O_DIRECTORY` fd and
passes it over `SCM_RIGHTS`. The helper lists them in `ro_dir_fds` and calls `sandbox_apply_helper` before reading.

It has no network, no exec and no write access to anything. A bug in parsing a hostile file name can't reach further
than reading. It walks with `openat2` (`RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV`) from those fds and uses
`statx` for identity and allocated blocks.

Landlock can't exclude a subtree, so the helper can read everything under a root. Skipping `fsops_protected` is a rule of
the walk, repeated in lord-server for every record it accepts.

Memory is bounded: a file count limit, a depth limit and paged results. A scan that hits a limit says so, and is marked
incomplete. Progress is real counters, and a percentage appears only when the total is known.

### Results

The helper reports records with a kind, a rule id, the path relative to a root, device, inode, link count, size,
allocated bytes and modification time. lord-server validates each one and keeps them in memory for that scan. JS gets
pages with a `candidate id` and a display path, never anything it can use as authority.

Hard links count once. Sizes come in two columns: logical and allocated. What a cleanup will free is always an estimate:
reflinks, compression, snapshots and open files change it.

### Rules

A table compiled into lord-server (`clean/rules.c`), with a test that validates every entry. A rule has an id, version,
category, root relative to the home folder, a bounded name pattern, optional minimum age, exclusions, risk (`regenerable`,
`review`, `protected`), allowed action and a text key. There are no commands and no rule files from outside: a rule can
only pick from the fixed list of actions.

Only well-known locations are rules. `~/.cache` as a whole is not one, since applications keep sessions and databases
there. A name like `.tmp` or a folder called `cache` proves nothing by itself.

### Plans and actions

1. The user selects candidates. JS sends ids.
2. `lord.clean.plan(scan, ids)` builds the plan in C: it re-checks every candidate and returns the list, totals and
   warnings. The plan stays in lord-server, and it expires.
3. `lord.clean.execute(plan)` runs it on a thread. Before each action it reopens the object from its root with
   `openat2`, compares device, inode, size and modification time, and skips it as *changed* if they differ.
4. Each item ends as done, skipped, changed, denied, failed, cancelled or unsupported.
5. Free space is measured again and shown next to the estimate.

Actions:

- **Trash:** the existing `fsops` path. Space isn't reported as freed until the Trash is really emptied.
- **Clear a cache:** the real delete below.
- **Package removal:** the Store's existing path, with `lord.pkg.plan` shown first.

## The real delete for caches: approved by the maintainer on 2026-10-30

`AGENTS.md` says `lord.fs` has no general delete, and the Trash doesn't free space. Caches need one. It lives inside
`lord.clean`, as the session user, never root, for **`regenerable` rules only**.

- No path comes from JS: only a plan id, and the plan's items come from the rule table.
- It deletes only files beneath that rule's fixed root, with `openat2` and `unlinkat`, and never crosses a mount.
- The object is pinned first: its name moves to a private one in the folder already opened beneath the root, the object
  behind that name is compared again, and only that object is unlinked. Anything else goes back to its name.
- The `fsops_protected` list stays in force.
- The same operation does not exist in `lord.fs`, `admin.c` or the broker.
- The confirmation says it can't be undone.

Other rules (`review`) use the Trash.

## Security

`CLEANER-SECURITY.md` holds the full model. The fixed points: read-only scans, no shell or `system()`, root only through closed admin actions (approved 2026-10-30), nothing leaves the machine, and when something can't be shown to be safe to remove, it is shown and not offered.

Tests that delete run only in throwaway folders, never in a real home folder.

Found during this audit and fixed first: `fsops_protected` matched a protected path and what is under it, so trashing a
*parent* such as `~/.config/lordos` passed. `lord.fs.rename`, `trash`, a move and a restore now refuse a parent of a
protected path, as source or destination (`fsops_protected_below`, `fsops-test`).

## Performance

The compositor never waits for a scan: everything is a helper or a thread, polled from JS. Hashing runs only on files
that share a size and a partial hash. Nothing stays running when the app closes: closing cancels the scan and the helper
exits. There is no resident service.

## Duplicates

Regular files in the folders the user chose. Group by size, then a SHA-256 of the first and last 16 KB, then a full
SHA-256. Hard links are one file, not two copies. Before acting, the file is checked again, and a byte comparison against
the copy being kept confirms it. At least one independent copy always stays, and nothing is selected automatically.

## Storage map

One scan builds a bounded tree, drawn three ways: a directory tree, a treemap (squarified, up to 28 views a level so areas can rise in, the rest grouped) and a summary by type. Selecting a rectangle does nothing but select it.

## Phases

| Phase | Contents |
|---|---|
| 1, the core | Overview, the system scan, rules, caches, temporary files, Trash, large and old files, duplicates, storage map, plan, history, preferences, en/es/pt |
| 2, system care | Apps and their leftovers, performance page reusing Monitor and Disks, similar images, developer and AI caches, privacy recommendations |
| 3, optional modules | Optional malware scanner, configuration audit, browser, mail and cloud modules, scheduled scans that only notify |

Build order for Phase 1: `lord-clean` and one safe rule end to end, then the rest of the rules, then large files and the
storage map, then duplicates.

Names: the interface uses generic words only ("Scan", "Storage map"), as for every LordOS app.
