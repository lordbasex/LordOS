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
the figure this section used to give, 100 ms of CPU in 10 s, had none, so nothing was being painted and it did not describe the app. `lord-server` CPU is over 8 s, on a VM other jobs were using (load 8 to 14).

| | Before (commit 3a97f5c) | After |
|---|---|---|
| `lord-server` CPU in 8 s | 6,100 ms | 940 ms |
| Layout and paint per frame, p50 | 27.7 ms | 0.80 ms |
| p90 | 36.9 ms | 3.1 ms |
| p99 | 48.0 ms | 11.9 ms |
| Worst | 60.0 ms | 18.4 ms |
| Screen repainted per frame, p50 | 87 % | 0.1 % |
| Screen repainted per frame, mean | 86 % | 3.7 % |

After: 640 frames in 12 s. Before: 386. These are the CPU path's numbers. **Not measured: a Raspberry Pi 5, a real display, the GPU path.**

- Reduce motion: 9 repaints in the whole session after opening the app, 110 ms of CPU in 8 s.
- Minimized: 90 ms of CPU in 8 s.
- Another window in front: **not tested**; `useLive` stops the loops then by its code, not by a run.
- A walk through a full scan, the review board and the bubbles (17 s, 316 frames): p50 0.86 ms, p99 27.7 ms, worst 32.7 ms, 17 % of the screen on average. Section changes repaint the window, as they should.

Motion was seen as real frames in sequence, from the session's own video: the dots on their orbits, the object floating, the sparks, the button's pulse, the dashboard's tiles, the busy dots, the cards rising and the bubbles growing.
That proves they run and in what order, not that they feel smooth on a real display at 60 Hz. The hero's dots update 20 times a second, the button and the stars 10.

## Not automated

The admin actions' own entry points run as root and need a machine to clean: their folder rules are tested, the actions weren't run on the VM. On-screen checks are by hand in an
isolated headless session. In that session a moving screen leaves the mirror's lossless stills stale, so stills are taken with Reduce motion on, and motion is read from the session's video (RP_VFRAME, decoded to frames).
