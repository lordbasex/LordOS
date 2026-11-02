# Versioned results

Measurements and checks are published as files, per LordOS version and per machine, with the commands
that produced them, so anyone can repeat them and compare. A screenshot proves one moment; these files
can be checked.

```
results/<version>/<machine>/
    README.md        the conditions (hardware or VM, monitor or headless, idle time, apps open, power)
                     and the exact commands
    sysreport.json   lordos-sysreport report --json: the hardware and what LordOS made of it, with no
                     serial numbers, addresses or user and host names (tests/sysreport_test.c checks it)
    testbench.json   lordos-testbench --gl --auto N: the Wayland checklist, frame times (average, p50,
                     p99, frames that missed the refresh budget), presentation, the 3D renderer, latency
    memory.txt       the whole system (`free -m`) and PSS per process (/proc/<pid>/smaps_rollup)
    boot.txt         systemd-analyze, and the time from the kernel's start to the desktop's first frame
                     (lord-sessiond's log line; the boot splash shows its own timer from a little later)
    lordbench.json   from 0.8.6, LordBench: lordos-sysreport bench --full --json (docs/BENCHMARK.md)
```

`tools/results/collect.sh <machine> [--no-memory] [--no-lordbench] [--bench-seconds N] [--version V]` writes
such a folder; run it as the user inside a LordOS session, and with `sudo` to include root's processes in the PSS
list. Fill in the conditions in its README before committing. Each release attaches its `results/<version>`
folder to the GitHub release (`docs/RELEASING.md`).

## Boot time: check it yourself

LordOS measures every boot the same way, on any machine, with no tool to install:

- **On the screen.** The boot splash shows a timer in its bottom right corner, in seconds and
  milliseconds, from the moment the splash starts. About LordOS → **Startup time** shows this boot's
  result: the seconds from the kernel's start to the first frame on the monitor, and whether that frame
  was the login screen or, with automatic login, the desktop.
- **In the log.** `lord-sessiond` writes one line per boot when the first frame reaches the monitor:

  ```sh
  journalctl -b -u lordos | grep "first frame"
  # first frame on the monitor 7.224 s after the kernel started (LordOS 0.8.5~rc1-beta, build 20261026, session)
  ```

  `-b -1`, `-b -2`… give the previous boots while the journal keeps them.
- **The history.** Every boot is also appended to `/var/lib/lordos/boot-times.jsonl` (root's, readable by
  everyone, the last 200 boots), one JSON object a line, so boots and versions can be compared after the
  journal is gone:

  ```sh
  cat /var/lib/lordos/boot-times.jsonl
  # {"bootId":"e519ea86-…","at":1793052001,"seconds":7.224,"shown":"session","version":"0.8.5~rc1-beta","build":"20261026","kernel":"6.12.113+deb13-amd64"}
  python3 -c 'import json; [print(f"{d[\"version\"]:>16} {d[\"seconds\"]:7.3f} s  {d[\"shown\"]}") for d in map(json.loads, open("/var/lib/lordos/boot-times.jsonl"))]'
  ```

  `seconds` counts from the kernel's start (`CLOCK_BOOTTIME`), so the firmware and the boot loader are not
  included; `shown` is `login` (the login screen), `session` (automatic login) or `none` (no monitor).
- **The pieces.** `systemd-analyze` splits the boot into kernel and userspace, and
  `systemd-analyze critical-chain lordos.service` shows what LordOS waited for. The splash's own timer
  starts when Plymouth starts, a little after the kernel: `journalctl -b -o short-monotonic | grep -E
  "kernel: Linux version|Started plymouth-start"` gives both moments, and the difference.
- **Comparing fairly.** The same machine, the same settings (automatic login on or off changes what the
  first frame is), several boots of each version (the first boot after an update or a new kernel is
  slower: it builds caches); report the median. `tools/results/collect.sh` saves all of the above in
  `boot.txt` and `boot-times.jsonl`.

## Memory: two magnitudes

`free`'s "used" is the whole system's memory (kernel, Debian's services, LordOS's services, the
session), without reclaimable page cache: this is the number to compare between systems, on the same
machine, after the same idle time, with the same apps open. A process's RSS (`ps`, `/proc/<pid>/status`)
counts every shared library page it maps, so RSS figures of several processes overlap and can't be added;
PSS divides each shared page among the processes that map it, so PSS figures add up. The paper's §7.1 keeps
the two apart.

## Results so far

| Version | Machine | Conditions | What |
|---|---|---|---|
| 0.8.6-beta | `pi5-16gb-hdmi` | Raspberry Pi 5 16 GB, HDMI | First frame 7.63 s after the kernel; 489 MB used at idle; sustained 7.0 GFLOP/s from start to end, 50 → 59 °C (`ondemand`, the autologin session, LordBench Full) |
| 0.8.5-beta | `vmware-fusion` | the same VM, updated from 0.8.4.1 to the 0.8.5 package with LordOS's package engine (lordos.org/apt-staging), measured about five minutes after the restart, Chrome, VS Code, Spotify and llama.cpp installed but not running | whole system 592 MB used (`free -m`); 59.8 fps at 60 Hz, p50 16.6 ms, p99 17.9 ms, 1 frame over budget in 30 s; 16 Wayland checks passed; first frame 7.4 s after the kernel started (`systemd-analyze` 7.6 s) |
| 0.8.5~rc1 | `vmware-fusion` | VMware Fusion VM, virtual GPU (`vmwgfx`, SVGA3D), 1280×800 @ 60 Hz, updated from 0.8.4.1 by `apt install` | whole system 606–622 MB used one minute after sign-in, PSS of all processes 316 MB (login screen 78 MB of it); 59.8 fps at 60 Hz, p99 18.4 ms; 17 Wayland checks passed; boot 7.5 s |
| 0.8.5~rc1 | `vmware-fusion-before-pacing` | the same, before software vblank pacing | 524 fps at 60 Hz: the driver completes flips at once |
| 0.8.5-dev | `vm116-headless` | Proxmox VM, no GPU, headless session (remote desktop only), development install | sysreport, testbench (16 Wayland checks passed; `llvmpipe`; no frame presented without a monitor), boot |
