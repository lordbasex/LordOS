# Cleaner: architecture

**Status: design for 0.8.7, with Phase 1 built (`CLEANER-ROADMAP.md`).**
Checked against the code on branch `v087` on 2026-10-31. Each feature's real state lives in `CLEANER-ROADMAP.md`; a line
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

Nothing of a scan outlives it except one thing: the folder names and sizes of the last tree scan of the home folder or `/`, in `~/.cache/lordos/cleaner-map.txt` (Storage map below). That file holds no candidates.

Hard links count once. Sizes come in two columns: logical and allocated. What a cleanup will free is always an estimate:
reflinks, compression, snapshots and open files change it.

### Rules

A table compiled into lord-server (`clean/rules.c`), with a test that validates every entry. A rule has an id, version,
category, root relative to the home folder, a bounded name pattern, optional minimum age, exclusions, risk (`regenerable`,
`review`, `protected`), allowed action and a text key. There are no commands and no rule files from outside: a rule can
only pick from the fixed list of actions.

Only well-known locations are rules. `~/.cache` as a whole is not one, since applications keep sessions and databases
there. A name like `.tmp` or a folder called `cache` proves nothing by itself.

The one exception to fixed locations is a project output (`project` in `CleanRule`, `clean_project_kind` in `clean/cleanfs.c`). Eight kinds, each with its folder names and what proves it: a file next to the folder, one inside it, or both where a name is too common. `lord-clean` walks the home folder once for all kinds, reports each proved folder whole with its totals, and for a kind with an idle time adds the newest modification time in the project, build outputs apart. `clean_rule_check` refuses a project rule that isn't `review`, to the Trash, `explicit_only` and without any other location. Project outputs are in `CLEANER.md`.

The same evidence is read again in lord-server at the plan and right before acting (`item_check`). A candidate whose evidence is gone ends as *changed*.

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

Other rules (`review`) use the Trash. Project outputs are `review` for that reason: the approval covers fixed roots, and they have none. A rule with `system_root` (`/tmp`) has no action at all, which the table's check enforces.

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

## Similar images

The same walk, kept to image names of at least 8 KiB, then `helper/lcimage.c` inside `lord-clean`: the file is read whole, up to
64 MiB, its size in pixels is read from the header and refused past 40 megapixels before anything is allocated for it, and it is
decoded to grey with the `stb_image` the project already carries, set up as `lord-maild`'s. Nothing is decoded in `lord-server`. The
image's cells give a 64-bit difference hash. The helper sends one group at a time, largest image first, with sizes and hashes;
lord-server checks that every member really is within reach of the first (`clean_similar`, shared with the helper) and throws away a
scan that lies. The plan is the duplicates' plan: one image of each group stays, as another inode. Right before acting, the image
that stays is checked to be the file the scan saw; there is no byte comparison, as the bytes differ.

Thumbnails on the page are `media.thumb`, made by `lord-media`.

## Storage map

One scan builds a bounded tree, drawn three ways: a directory tree, a treemap (squarified, up to 28 views a level so areas can rise in, the rest grouped) and a summary by type. Selecting a rectangle does nothing but select it.

**Bands.** When a tree and a rule scan are both in memory, `mark_reclaim` walks the rule candidates down the tree by name and adds their estimated bytes to each folder, split into `safe` (regenerable) and `review`, then up to its parents. A rule that is only measured counts for neither. It runs inside a scan's job, with only the result written under the lock, so the desktop never waits for it.

**Summary.** Free space, the root's two amounts, and what wasn't scanned: the volume's used space minus the allocated bytes walked, never below zero.

**The kept map.** `save_tree` writes the nodes to a temporary name with `O_EXCL | O_NOFOLLOW`, mode 0600, and renames it over `cleaner-map.txt`. A later `lord.clean.start('tree', { saved: true })` loads them with no helper (`load_tree`): ids must rise, a parent must exist and come first, a name must pass `clean_name_ok`, and sizes can't be negative. Anything else counts as no file. The scan that results has `savedAt` and no candidates, so no plan can be built from it, and it has no bands. `lord.clean.saved()` gives only `{ at, volume }`: folder names stay in C until the tree is paged as usual. On by default (`rememberMap`); a scan with it off removes the file, as does turning it off and clearing the history.

## Where the 2026-10-31 additions came from

These ideas (evidence next to a folder, idle projects, bands on the map, the not scanned figure, live recommendations, keeping the last map) were reimplemented from scratch after reading the open source Vano disk analyzer by Estanislao Pérez Nartallo (Apache-2.0); no source code or text was copied.

Deliberate differences:

- Free space isn't counted for the Trash: what goes there is shown as an amount to check, and is free only once the Trash is emptied.
- Container storage gets no action. It is measured only, as what a container engine keeps can't be shown to be safe.
- The pnpm store stays reviewed, to the Trash, since projects hard-link into it.
- Idleness looks at the whole project, not at the output folder's own date, which says only that its entries changed.

## Phases

| Phase | Contents |
|---|---|
| 1, the core | Overview, the system scan, rules, caches, temporary files, Trash, large and old files, duplicates, storage map, plan, history, preferences, en/es/pt |
| 2, system care | Apps and their leftovers, performance page reusing Monitor and Disks, similar images, developer and AI caches, privacy recommendations. Built and pending rows are in `CLEANER-ROADMAP.md` |
| 3, optional modules | Optional malware scanner, configuration audit, browser, mail and cloud modules, scheduled scans that only notify |

Build order for Phase 1: `lord-clean` and one safe rule end to end, then the rest of the rules, then large files and the
storage map, then duplicates.

Names: the interface uses generic words only ("Scan", "Storage map"), as for every LordOS app.
