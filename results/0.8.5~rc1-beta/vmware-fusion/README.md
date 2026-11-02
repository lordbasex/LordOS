# vmware-fusion — LordOS 0.8.5~rc1-beta

Collected 2026-10-26T20:20Z with `tools/results/collect.sh vmware-fusion` (docs/RESULTS.md); the report's
own version field says `0.8.5~rc1-beta`.

**Conditions:** VMware Fusion on a Mac (Intel Core i9-10900, 4 cores and 4.7 GB given to the VM), the
virtual display at 1280×800 @ 60 Hz on the `vmwgfx` driver with GPU composition (`SVGA3D`, a virtual GPU on
the host). LordOS 0.8.4.1 installed from the ISO earlier, updated to the 0.8.5~rc1 package with `apt install`
(no migration was needed; code verified, 10 files) and then `lord-server` replaced by the build with
software vblank pacing (below). Measured about one minute after a reboot and sign-in, with Files and About
LordOS open; the login screen's `lord-server` stays running after sign-in (it serves user switching and
locking).

**What it shows:**
- `free -m` "used": 606–622 MB for the whole system; the PSS of every process adds up to 316 MB
  (`memory-all.txt`, taken with sudo): the session's `lord-server` 110 MB, the login screen's 78 MB,
  `systemd-journald` 20 MB, `lord-doc` 15 MB (opened by Files' thumbnails), PipeWire and WirePlumber 16 MB.
  The rest of "used" is the kernel (slab 81 MB, vmalloc 35 MB, stacks and page tables 12 MB) and the
  graphics driver's buffers, which no process's PSS counts.
- The Test Bench: 59.8 frames a second at 60 Hz, p50 16.66 ms, p99 18.4 ms, no frame over budget, 1495
  presented, 0 discarded; 17 Wayland checks passed, 0 failed; 3D on `SVGA3D` (virtual GPU), 0.76 ms a
  frame. Boot: 7.5 s (3.4 kernel + 4.1 userspace).
- `../vmware-fusion-before-pacing/`: the same machine before the fix: `vmwgfx` completes page flips at
  once instead of on vblank, so the compositor and the bench ran at 524–583 frames a second.

Reproduce:

```sh
lordos-sysreport report --json > sysreport.json
lordos-testbench --gl --auto 25 > testbench.json      # inside a LordOS session
free -m                                                  # whole system
sudo sh -c 'for f in /proc/[0-9]*/smaps_rollup; do ...'  # PSS per process: see tools/results/collect.sh
systemd-analyze
```
