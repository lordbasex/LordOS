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

## Not automated

The admin actions' own entry points run as root and need a machine to clean: their folder rules are tested, the actions weren't run on the VM. On-screen checks are by hand in an
isolated headless session. In that session a moving screen leaves the mirror's lossless stills stale, so screenshots are taken with Reduce motion on.
