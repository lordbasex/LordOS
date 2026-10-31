# Cleaner: security

What protects the user's files, and what doesn't. Design in `CLEANER-ARCHITECTURE.md`, tests in `CLEANER-TESTS.md`.

## Principles

- A scan only reads, and nothing leaves the machine. There is no telemetry.
- JS holds candidate ids and display paths. Authority is never a path from JS.
- What can't be shown to be safe is shown and not offered.
- Each object is checked again right before acting.
- There is no shell, `system()` or `popen()` anywhere in Cleaner.

## The scanner, `lord-clean`

Fixed path next to `lord-server`, no arguments, `LANG` only, channel on fd 3. lord-server opens each root and passes it over
`SCM_RIGHTS`; the helper lists them in `ro_dir_fds` and calls `sandbox_apply_helper` (scope `lordos-cleaner`) before reading. It
refuses to run without Landlock. It has no network, no exec and no write access.

It walks with `openat2` (`RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV`) and `statx`. It follows no link, crosses no mount or device,
and reports no name that isn't valid UTF-8 or holds control characters. It runs at the lowest CPU and I/O priority, within limits on entries,
depth, path length, folders and results. Reaching one marks the scan incomplete.

**What a compromised scanner can do:** read the roots it was given. For a tree of `/` or a duplicates scan, that is what the session user
may read in them, and `fsops_protected` paths are only skipped by rule, as Landlock can't exclude a subtree. It can't send anything anywhere but
to lord-server, which validates every record with `sec/json.c`: types, ranges, the path's shape, device, rule, owner and
`fsops_protected`. A malformed record discards the whole scan. A lying helper can make lord-server hold a false candidate, but acting on it
needs the real object to match.

**Decoding:** similar images are the one place Cleaner reads the *contents* of files as anything but bytes to hash, and it happens only in `lord-clean`, never in
`lord-server`. It uses the vendored `stb_image`, limited to PNG, JPEG, GIF and BMP, with the same sandbox as the walk and no new syscall or folder. A hostile image is a
reason to expect a crash, which fails the scan: a decoder bug reaches what the list above says and no more. Files over 64 MiB and pictures over 40 megapixels are not
decoded, and a similar-images scan runs with 480 MiB of address space, so a small file that asks for more (a 311 KB 16-bit PNG asked for 588 MB) is left out instead of bringing the scope's limit of 512 MB down on the whole scan. A group whose members aren't within reach of the first image is a protocol error.

**A root other users write in:** `/tmp` is walked with `"own":1`, so the helper neither enters nor reports anything of another user, by owner, before opening it.

## Plans

Built in C from ids of a scan, kept in lord-server, valid ten minutes, run once. Category plans refuse rules marked `explicit_only`.

## Acting on an item

Before each action, for every item: the rule's action and risk are read again, the path must not be protected or excluded, a browser lock is
read again, and the object is reopened from its root with `openat2` and compared by device, inode, size and modification time. Anything else
ends as *changed*, *denied*, *skipped* or *unsupported*, and the rest go on.

**Trash:** `fstrash_put_at` renames a name in the folder already opened beneath the root, so a folder swapped for a link can't redirect it. If the
object in the Trash isn't the expected inode, it is put back.

**The real delete** (approved by the maintainer on 2026-10-30): inside `lord.clean` only, as the session user, never root, for `regenerable` rules only. No path
comes from JS. The name moves to a private name in the folder opened beneath the rule's fixed root (`RENAME_NOREPLACE`), the object behind that name is compared,
and only that object is unlinked; otherwise it goes back. It deletes files, never folders. The confirmation says it can't be undone. It does not exist in
`lord.fs`, `admin.c` or the broker.

If the process dies between the rename and the unlink, a file named `.lordos-clean-*` stays in that cache folder.

**Duplicates:** a plan refuses to take every independent copy (hard links are one copy). Right before acting, the copy that stays is checked and compared byte
for byte with the one that goes.

**Browsers:** only named cache folders. A live process behind the profile's lock (`lock` for Firefox, `SingletonLock` for Chromium) blocks the
rule at scan and at action; a link that can't be read blocks it too; a stale pid doesn't.

**Similar images:** to the Trash only, never a real delete. A plan refuses to take every image of a group, and the image that stays is checked right before acting.

**What removed apps left:** the folder must belong to no installed package and no system app. That is checked at the scan and again right before acting, so an app installed in
between keeps its folder. If lord-server doesn't know where the system's apps are, or that folder isn't there, nothing is offered. It goes whole to the Trash, one by one, named in the confirmation, never
in a cleanup by category.

**The download folder's name** comes from a file the user owns, and it can only move rules that keep their narrow patterns and the Trash. A value is taken only if it would pass
as a rule's root, so `.cache` or a protected folder isn't, and a change after the plan makes the items *changed*.

**`/tmp`** is measured only: `clean_rule_check` refuses any action on a `system_root` rule, and any root but `tmp`.

**Old kernels and apps** add nothing here: `lord.pkg.plan` and the admin helper's existing `pkg-remove`, with no new root power.

**AI models:** moved whole to the Trash, one by one, named in the confirmation, never in a cleanup by category. Whether another app uses a model can't be known in
general, so the confirmation says so for every model.

## System actions (approved by the maintainer on 2026-10-30)

In `admin.c`'s table (`clean/clean_admin.c`). None takes an argument: a non-empty one is refused.

| Action | Who | What |
|---|---|---|
| `clean-status` | anyone | Read-only sizes of `/var/log/journal`, `/var/cache/apt/archives`, `/var/tmp` |
| `clean-journal` | admin | argv `/usr/bin/journalctl --vacuum-size=200M` |
| `clean-apt-cache` | admin | No program. Regular `*.deb` directly in `/var/cache/apt/archives`, and regular files directly in `partial/` |
| `clean-var-tmp` | admin | No program. Regular files directly in `/var/tmp`: root's, one link, over 30 days by modification and change time, open in no process |

Folders are opened with `RESOLVE_NO_SYMLINKS` and must be root's and not writable by others, except sticky `/var/tmp`. Nothing recurses, symbolic links are never followed or
removed, and `/var/lib/dpkg` is never touched. "Open in no process" is a write lease on the file, held while it is unlinked.

## Fixed in `lord.fs` along the way

`trash`, `rename` and a move of a *parent* of a protected path passed, taking settings, permissions and vaults with it, and a folder could arrive under such a name.
All now answer `fs.protected`, on both sides (`fsops_rename`, `fsops_protected_below`). `lord.fs.rename` uses `RENAME_NOREPLACE`.

## Limits

- A hard link outside the scanned root is known only by its link count.
- A process can hold a file open while it is deleted from a cache; its space returns when it closes it.
- Between the final comparison and the unlink, the private name is known only to Cleaner, but a process of the same user could still rename it. It would have to
  guess the name in that instant, and it could delete the file anyway.
- Same-user malware is outside the threat model: the session user can already delete these files.
