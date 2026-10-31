# Cleaner: tests

Automated tests use throwaway folders only, never a real home folder.

## `build/clean-test` (`server/tests/clean_test.c`, in `make test`)

Runs against the real `build/lord-clean`, with a home folder made by `mkdtemp` under `/tmp`. It skips itself without Landlock.

- **Rules:** the real table; every kind of bad entry; a delete only for regenerable rules; no root touching cookies, history or passwords.
- **Walk:** read-only; symbolic links to files and folders; a root that is a link; `..` and absolute paths in records; bad names; file and depth limits.
- **Records:** every field lord-server must refuse.
- **Protected paths:** skipped by the walk, dropped by lord-server, refused at plan and execution.
- **Execution:** a file changed, touched, replaced or removed after the plan; a folder swapped for a link; a name repointed at the last moment; cancel; restore from the Trash.
- **Real delete:** the same swaps; no private name left behind; a fingerprint of everything outside the root; a mount inside the root and one placed after the scan, in a user and mount namespace.
- **Browsers:** a live and a stale lock, for both families; a browser starting between plan and cleanup; cookies and history untouched.
- **Models:** whole-folder items, refused by category, moved, restored; Ollama offers nothing.
- **System actions:** their folder rules on throwaway folders: links, subfolders, age, owner, hard links, a file held open.
- **Tree:** filters, order, logical against allocated with a sparse file, hard links once, exclusions, bad volumes, history without paths.
- **Duplicates:** equal size with other content, equal partial hash, empty files, hard links, a group fully selected, a kept copy changed with its date restored, a kept copy removed, overlapping folders.
- **Plans:** bad ids, repeated ids, expiry, one run. **Cancel:** the helper is killed and reaped.

## `build/fsops-test`

The parent-of-protected cases. Run against `fsops.c` with the checks removed they fail: 60 of 98 pass. With them, 98 of 98.

## `build/clean-bench <folder>`

Times scans on a throwaway folder and reports the helper's peak memory, polled every 20 ms. Not part of `make test`.

Measured on VM 116 on 2026-10-30 (Xeon Silver 4108, 8 cores, virtual disk, warm cache, throwaway tree of 122,000 files; the helper's peak is a poll, so a lower bound):

| Step | Time | Helper peak | lord-server RSS |
|---|---|---|---|
| Rule scan, 20,000 candidates | 125 ms | 1 MB | 4 MB |
| Plan of 20,000 files | 164 ms | | |
| Real delete of 20,000 files | 1,037 ms | | |
| Tree, 102,653 entries | 446 ms | 1 MB | 5 MB |
| Tree keeping every file: stops at 50,000 kept, incomplete | 552 ms | 1 MB | 13 MB |
| Duplicates, 102,640 entries, 17,936 listed in 20 groups: incomplete, past 1,024 per group | 5,125 ms | 37 MB | 15 MB |

Calls on the compositor's thread: `poll` 1.08 ms at worst, a page of 500 results 1.97 ms at worst, a page of 100 groups 31 ms at worst, on groups of over a thousand files.

### Animation and frame time

Measured on VM 116 on 2026-10-30 in an isolated headless session, 1280×800, Noche dark, the Scan hero, with `--profile` per frame and a video viewer attached. **A headless session paints only with a viewer**:
the figure this section used to give, 100 ms of CPU in 10 s, had none, so nothing was being painted and it did not describe the app. `lord-server` CPU is over 8 s. Both columns were run back to back on the final code on a quiet VM (load under 0.5); an earlier pass under load 8 to 14 gave 27.7 ms and 0.80 ms at the median, so the load changes the numbers and only same-load figures compare.

| | Before (commit 3a97f5c) | After |
|---|---|---|
| `lord-server` CPU in 8 s | 6,690 ms | 970 ms |
| Layout and paint per frame, p50 | 17.5 ms | 0.61 ms |
| p90 | 19.2 ms | 1.9 ms |
| p99 | 22.6 ms | 9.6 ms |
| Worst | 70.1 ms | 17.3 ms |
| Screen repainted per frame, p50 | 87 % | 0.1 % |
| Screen repainted per frame, mean | 86 % | 3.8 % |

After: 845 frames in 12 s. Before: 423. These are the CPU path's numbers. **Not measured: a Raspberry Pi 5, a real display, the GPU path.**

- Reduce motion: 9 repaints in the whole session after opening the app, 80 ms of CPU in 8 s.
- Minimized: 60 ms of CPU in 8 s.
- Another window in front (Notes, Cleaner behind it): 80 ms of CPU in 8 s.
- A walk through a full scan, the review board and the bubbles (17 s, 434 frames): p50 0.60 ms, p99 19.3 ms, worst 24.3 ms, 16 % of the screen on average. Section changes repaint the window, as they should.
- Frames that change nothing: on a section with only the stars (Settings), the frame callback alone kept 5.5 video frames a second over 12 s instead of the 10 it asks for, because such a frame brings no flip and the compositor sleeps until something else wakes it. In that session a viewer's video was the wake-up. The timer kept pending in `apps/cleaner/ui.jsx` restored the rate (264 renders against 173 for the same session). **The real display, where nothing like the viewer wakes the loop, is read from `server/src/main.c`, not run.**

Motion was seen as real frames in sequence, from the session's own video: the dots on their orbits, the object floating, the sparks, the button's pulse, the dashboard's tiles, the busy dots, the cards rising and the bubbles growing.
That proves they run and in what order, not that they feel smooth on a real display at 60 Hz. The hero's dots update 20 times a second, the button and the stars 10, the two busy dots 20.

## Not automated

The admin actions ran as root once, on 2026-10-30, in a real session of an installed arm64 system in a throwaway emulated machine (`ISO.md`, "The arm64 ISO"), from Cleanup's system card with files planted first:

| Action | Result |
|---|---|
| `clean-status` | answered: system log 16 MB, packages 4.8 MB, temporary files 8.6 MB; the sizes changed after each action |
| `clean-journal` | "done", exit 0; 16 MB stayed, under its 200 MB limit, so no journal was over the limit in this run |
| `clean-apt-cache` | removed `t1.deb` and `partial/t2.part`; kept `keep.txt`, the symbolic link `link.deb` and `lock` |
| `clean-var-tmp` | removed only the old root file; kept a file open in a process, a hard-linked pair, a user's file, a new file and a folder |

`/sys/fs/cgroup/system.slice/lordos.service/lordos-session-1000/lordos-cleaner.scope` appeared in both a live and an installed session.

Found in that run: **a scan finds nothing in a live session.** On the live system's overlay a regular file's device differs from its folder's (`1f`, `1e` here), and the walker (`lcmain.c`, `other_dev`) counts it as another mount, so "This scan found nothing". An installed system on ext4 isn't affected. **The system card is unreachable when a scan finds nothing**: `CleanupList` renders it only with rule rows. Both open.

On-screen checks are by hand in an
isolated headless session. In that session a moving screen leaves the mirror's lossless stills stale, so stills are taken with Reduce motion on, and motion is read from the session's video (RP_VFRAME, decoded to frames).
