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
- **Similar images:** real PNG and JPEG files: a resized, a recompressed and a brighter version group, another picture, other proportions and hard links don't. A blank page, a text file with an image's name, and PNG headers claiming 60,000 and 8,000 pixels a side aren't decoded; a small icon and a link aren't read. The whole group is refused, three of four go to the Trash and come back, and when every other image is gone by the time of the action, nothing goes. A group of different pictures, other proportions, a bad hash, impossible sizes or no pixels is refused from the helper.
- **Translated Downloads:** the English name with no settings file, the user's folder, the setting moved after the plan, a real run, and six bad values and a link as the settings file, each falling back to `Downloads` and never reaching a wider folder; a named pipe as the settings file doesn't hold up the scan.
- **Leftovers:** a removed app's folder is offered; an installed app's, a system app's, a name no app can have and a loose file aren't. Refused by category, skipped if the app comes back after the plan, moved whole, restored, and not offered when the system's apps can't be located or their folder is missing.
- **`/tmp`:** age, a link, no plan by rule or by id can act, and the table refuses any other system root and any action on one. It runs on a throwaway stand-in; another user's files were only seen in the session below.

Checked by breaking the code on purpose on 2026-10-30, 823 checks each time: without the installed-app check 6 fail; without lord-server's check of a group of images 2 fail; taking any `XDG_DOWNLOAD_DIR` value 3 fail.

## `oldkernels_test.mjs` (in `run_suites.sh`)

Which kernels are offered: the running one, the newest and one below stay; numbers compare as numbers; other flavours, meta packages and an unknown running kernel offer nothing. Plans that take the meta package, the running kernel, `lordos` or anything but a removal are refused.

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

Similar images on VM 116, 2026-10-30, under a load of about 8 with the helper at its lowest priority: 80 JPEG files, 40 of 12 MP and 40 of 1.9 MP, in 5.4 s, about 67 ms each; helper peak 32 MB, a lower bound. Those test pictures are mostly fine patterns, so most were left out as flat: this is a time, not a result. For results, 60 real images from the repository (wallpapers, artwork and screenshots) in three versions each (JPEG 92, JPEG 40, half size PNG), 180 files, took 1.7 s: 175 files landed in a group with a version of their own picture and 5 in none. 5 of the 54 groups joined different pictures, 11 in all, every one a screenshot of the same desktop or window with small differences.

A 311 KB 16-bit RGBA PNG of 6,200 × 6,200 pixels made the decoder peak at 588 MB, over the scope's 512 MB. With the helper's 480 MiB of address space the same file came back as not decodable, and `clean-bench images` with that file in the folder still finished and found its groups. Measured on VM 116 on 2026-10-30, outside a scope, by hand: that file is not in `clean-test`.

## An isolated session on VM 116

The branch's `lord-server` with its own home and runtime folder, a throwaway theme with the real Cleaner, driven through `remote.sock` with screenshots, on 2026-10-30:

- Similar images from the Duplicates section with real thumbnails, selection and the confirmation; Apps with real packages and icons, leftovers found and named in the confirmation; `~/Descargas` found through `user-dirs.dirs`.
- The real `/tmp`, with other users' folders in it: the scan stayed complete and showed only the session user's file.
- A list scrolled to its end shows its last row at the bottom of the box.
- `lord.pkg`: three kernel names read; `oldKernels` offered nothing on a machine with two; a real plan for the older kernel passed `kernelPlanOk`, and one for the running kernel was refused by the engine and by the check.
- The update word's rule with `lord.pkg.info` stubbed: `lordos` isn't a package on the VM, so its real state there is "not installed".

The session has no admin helper, so nothing was removed: not an app, not a kernel. The Remove buttons, the system card and `OldKernels`' own card were not on screen. The panel by the clock wasn't opened: that theme has no such item.

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
