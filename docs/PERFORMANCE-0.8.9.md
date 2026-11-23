# Performance of 0.8.9 and 0.8.9.1

How much memory and processor a LordOS desktop session uses at rest, measured on real hardware for the 0.8.9
release and for the 0.8.9.1 memory work (released 2026-11-19). Every number here comes from a CSV in
[`perf/0.8.9/`](perf/0.8.9/) or [`perf/0.8.9.1/`](perf/0.8.9.1/), so it can be checked and repeated.

How each MB was found, down to the line of code, is in [`perf/MEMORY-HEAPTRACK.md`](perf/MEMORY-HEAPTRACK.md);
this report gives the runs and the comparisons, and doesn't repeat that method.

## Summary

| What | Result |
|---|---|
| 0.8.9, the maintainer's PC, Lord Workspaces with the wallpaper slideshow on, 2 h | `lord-server` 559 MB RSS (median), **19.0 % of one core** |
| The same session with the slideshow turned off | 491 MB, **2.0 %** of one core |
| 0.8.9, the desktop just after login (the floor), 10 min | 177 MB, 0.2 % of one core |
| 0.8.9.1 memory work: 7 widgets and a Terminal running Claude Code at rest | **269 → 183 MB** (−86 MB, −32 %), 2.4 % of one core |
| Raspberry Pi 5, headless session at rest, 1 h: 0.8.8.9 → 0.8.9 | 116 → 113 MB; **3.4 → 6.1 %** of one core |
| Raspberry Pi 5 with 0.8.9.1, no monitor | Lord Flow 113 MB and 6.2 % of one core (as 0.8.9); Lord Workspaces **99 MB** and **0.06 %** |

The slideshow is the biggest cost found at rest: about 17 points of one core on three 1080p displays. The memory
work took the session with a Terminal below the same desktop *without* one before the work (183 against 185 MB).
On the Raspberry Pi the 0.8.9 session uses less memory than 0.8.8.9 but almost twice the processor; what runs at
rest is still being looked for (roadmap, 0.8.9.1).

## Machines

| Name | Hardware | System | Session |
|---|---|---|---|
| **PC** (the maintainer's) | Intel Core i9-10900, 128 GB, AMD Radeon RX 580 (Mesa radeonsi, GPU composition), 3 displays at 1920×1080 (DP-2, DP-3, HDMI-A-1) | Debian 13, kernel 6.12.117, transparent huge pages `always` | Lord Workspaces |
| **Pi** | Raspberry Pi 5, 16 GB | LordOS's kernel 6.18.57-lordos.3-rpi-2712, aarch64 | the autologin session, **headless** (no display connected), Lord Flow |

The Pi has no display, so its session paints nothing unless someone watches it through the remote desktop
(nobody did). Its numbers measure what a session does at rest besides drawing.

## Method

Every run is one **fresh session**, sampled by [`tools/bench/session-sample.sh`](../tools/bench/session-sample.sh):

```sh
sudo tools/bench/session-sample.sh <user> <minutes> out.csv 10
```

It waits for that user's `lord-server` session (never the login screen's) and every 10 s writes one row: RSS,
anonymous and file-backed memory (`/proc/<pid>/status`), the processor time used so far (`utime + stime` from
`/proc/<pid>/stat`, in clock ticks) and the system's MemAvailable. The header records the LordOS version, the
theme (read from the user's `settings.json`), the connected displays, the kernel, the processor, the session's
pid and when it started, and `clk_tck`.

- **PC:** log out, log in, set the scene, let it settle for 3 minutes (the 0.8.9.1 runs: `next-session.sh` waits
  for a session with a new pid, sleeps 180 s, samples 10 minutes, then saves `smaps_rollup`), then don't touch
  anything.
- **Pi:** each run installs the version, sets the theme in `settings.json`, **reboots** the board (a fresh
  autologin session), waits for ssh and samples 60 minutes.

**What the numbers mean.** MB here are what `/proc` reports divided by 1024 (MiB). The **CPU %** is the processor
time `lord-server` used during the run divided by the run's length: 100 % is one core busy all the time (the PC
has 20 threads, the Pi 4 cores). The RSS counts shared library pages too (`docs/RESULTS.md`, "Memory: two
magnitudes"); this report compares one process with itself across versions, which is what the RSS is good for.
"min / median / max" are over every sample of the run; "@N min" is the sample nearest N minutes after the
sampler started.

### What spoiled runs, and was left out

- **An old session.** In 0.8.9, `systemctl restart lordos` keeps the sessions alive (`docs/SESSIONS.md`), so a
  sampler started after it found the session of the build before (`INVALID-old-ws68-session.csv`: a 0.8.8.10 test
  build at 543 MB, not 0.8.9). The sampler now waits for a *new* session's pid; the CSV is not kept.
- **Opening a window "to check".** Opening About to see which build ran added about 20 MB that stayed. The 0.8.9.1
  runs checked the build with `strings /proc/<pid>/exe` instead.
- **The slideshow.** Lord Workspaces' wallpaper slideshow was on in the 2-hour 0.8.9 run. It costs 17 points of
  one core (below), so runs meant to measure something else must have it off.
- **The scene being set up.** The first minutes of the 2-hour session, while its windows were placed
  (`*-setup.csv`, 20–24 % of one core), are not part of the run. A first slideshow-off run
  (`pc-0.8.9-workspaces-slideshow-off.csv`) still had the slideshow going for its first 20 minutes (34 and 24 %
  of one core, then 1.9 %); the clean one is `-2018`.
- **The voice reading a reply aloud** (`lord-tts`) during a sample, and a run under **heaptrack**: both use more
  processor and memory, so neither is compared with the others.

## The PC with 0.8.9

### Lord Workspaces in daily use, slideshow on (2 hours)

`pc-0.8.9-workspaces.csv`: the 0.8.9 package, a session started after a reboot, sampled for 120 minutes once its
windows were in place. The slideshow was on; what else was open on the desktop was not recorded the way the later
runs' scenes were, so compare this run's processor with the others, not its RSS.

| | RSS | Anonymous | File-backed |
|---|---:|---:|---:|
| @1 min | 546 MB | 442 MB | 104 MB |
| @20 min | 583 MB | 479 MB | 104 MB |
| @60 min | 558 MB | 455 MB | 104 MB |
| @120 min | 551 MB | 447 MB | 104 MB |
| min / median / max | 512 / **559** / 621 MB | 408 / 455 / 517 MB | 104 MB |

**CPU: 19.0 % of one core** over the two hours, steady: 17.8 to 19.8 % in each 10-minute window. The RSS moves up
and down by about 110 MB with the slideshow's pictures (each one decoded at its display's size, the one that left
going to the image cache's idle budget of 64 MB; `docs/WORKSPACES.md`, "Wallpaper and slideshow").

### The slideshow's cost

`pc-0.8.9-workspaces-slideshow-off-2018.csv`: **the same session** about three hours later, with the slideshow
turned off in Settings → Wallpaper and nothing else changed, 30 minutes.

| | Slideshow on (2 h) | Slideshow off (30 min) |
|---|---:|---:|
| CPU, % of one core | **19.0** | **2.0** (2.3, 1.9, 1.9 in each 10 minutes) |
| RSS median (min–max) | 559 MB (512–621) | 491 MB (489–500) |
| Anonymous, median | 455 MB | 387 MB |
| File-backed | 104 MB | 104 MB |

So on three 1080p displays the slideshow kept `lord-server` busy about **17 points of one core** more, and its
pictures about 68 MB. Each change is a 2.4 s transition repainting a whole display (144 frames at 60 Hz) with
the bars' blurs made again; the bench measured 9.6–10 ms per frame on the CPU path (`docs/WORKSPACES.md`).
The run doesn't record the interval chosen. Not measured yet: the slideshow on 0.8.9.1, at 4K, or on the Pi.

The session's RSS with the slideshow off (491 MB) is still far above the floor (177 MB, below). This session
had been in use for hours; the 0.8.9.1 work found that the cached rasters of animated windows were never freed at
rest (57 to 114 MB in its scenes), which is consistent with that gap but was not measured on this session.

### The floor and what widgets and a Terminal add

10-minute runs, each a fresh session after 3 minutes of settling. **Floor** is the desktop just after login with
nothing opened; **widgets** adds 7 desk widgets on one display; **Terminal** adds a Terminal running Claude Code
(`claude --continue`), left alone.

| Run | Build | Scene | RSS min / median / max | Anonymous | File | CPU |
|---|---|---|---:|---:|---:|---:|
| `pc-0.8.9-floor-base` | 0.8.9 | floor | 177.0 / **177.1** / 177.2 | 110 | 67 | 0.2 % |
| `pc-ws01-floor` | ws01 (no huge pages for the whole process) | floor | 228.5 / 229.5 / 230.6 | 161 | 68 | 0.5 % |
| `pc-ws01-floor-b` | ws01, again | floor | 248.7 / 248.9 / 248.9 | 181 | 68 | 0.2 % |
| `pc-ws02-floor` | ws02 (huge pages again, the memory report) | floor | 190.4 / **190.5** / 190.6 | 124 | 67 | 0.2 % |
| `pc-ws02-widgets` | ws02 | widgets | 183.8 / **184.7** / 189.3 | 118 | 67 | 1.0 % |
| `pc-ws02-widgets-terminal` | ws02 | widgets + Terminal (same session, 22 min) | 265.9 / **268.5** / 275.1 | 200 | 69 | 4.3 % |
| `pc-ws03-widgets` | ws03 | widgets (17 min) | 244.2 / 246.4 / 249.6 | 179 | 67 | 1.0 % |

(MB; anonymous and file-backed are medians.)

What they say:

- **The floor is about 177–191 MB**, of which about 67 MB are file-backed (shared libraries, mostly libLLVM for
  Mesa) and 110–124 MB anonymous. At rest it uses **0.2 % of one core**.
- **Turning off transparent huge pages for the whole process (ws01) made it bigger**, 229–249 MB against 177:
  the heap benefits from huge pages. ws02 went back. What ws08 did instead is in "The 0.8.9.1 memory work".
- **7 widgets** cost about 1 % of one core (they read their data once per value and only while visible) and no
  memory you can tell from the noise between sessions: 185 MB with widgets against 191 MB for ws02's floor.
- **A Terminal running Claude Code** added about 84 MB on ws02 (185 → 269 MB) and 3 points of one core
  (Claude Code keeps updating its screen). The memory report accounts for most of those 84 MB: the cached
  transform rasters went from 57 MB (widgets) to 114 MB (with the Terminal) and the history table took 24 MB;
  the 0.8.9.1 work freed both.
- **Sessions differ.** ws03's widgets-only session read 246 MB, 62 MB more than ws02's, with no change between
  the builds that adds memory at rest. Before ws04 the cached rasters of whatever animated before the sample
  stayed for the whole session, so the same scene could read differently from one login to the next. That is
  why every step was measured in a fresh session, and why a single run says less than the loop's direction.

## The 0.8.9.1 memory work

Seven builds, one change each, each in a fresh session with 7 widgets and a Terminal running Claude Code at rest.
The changes, the memory report in `compositor.log` and heaptrack are explained in
[`perf/MEMORY-HEAPTRACK.md`](perf/MEMORY-HEAPTRACK.md); the CSVs are in [`perf/0.8.9.1/`](perf/0.8.9.1/).

| Build | Change | Published | CSV: RSS min / median / max | Anonymous | File | CPU |
|---|---|---:|---:|---:|---:|---:|
| ws02 | 0.8.9 plus the memory report | **269 MB** | 265.9 / 268.5 / 275.1 | 200 | 69 | 4.3 % |
| ws03 | more counters | 334 MB* | (not sampled with the Terminal) | | | |
| ws04 | transform rasters freed after 30 s at rest; clearing the scrollback frees its cells | 262 MB | 258.8 / 260.3 / 265.8 | 191 | 69 | 2.4 % |
| ws05 | the history table grows as it fills; raster slots give back oversized buffers | **197 MB** | 194.3 / 196.8 / 203.1 | 128 | 69 | 2.3 % |
| ws06 | counters for nodes, frames, arenas | 205 MB | 201.2 / 204.5 / 210.7 | 135 | 69 | 2.2 % |
| ws07 | app and font tables cut to what the scan found | 197 MB | 194.7 / 197.1 / 203.5 | 128 | 69 | 2.3 % |
| ws08 | the static arrays (`.bss`) opt out of huge pages | **183 MB** | 180.3 / 182.9 / 191.7 | 114 | 69 | 2.4 % |

\* ws03 was read right after Claude Code started (a snapshot, not a run); its CSV, `pc-ws03-widgets.csv`, is the
widgets-only session above. The published figures are the ones in `MEMORY-HEAPTRACK.md` and the CHANGELOG; the
CSV medians match them to the MB except ws04 (published 262, CSV median 260.3, last sample 262.8). Runs of
17 minutes (ws02: 22), after 3 minutes of settling.

The same figures are drawn as a chart on lordos.org (the paper's §7.1 and Features → Own desktop:
`web/content/charts/memory-0891.json`).

**Result: 269 → 183 MB, 86 MB less (−32 %).** With the Terminal and Claude Code open the session now uses less than
the same desktop used without them before (185 MB). The processor at rest didn't change with the memory work
(2.2–2.4 % of one core with Claude Code's status line running; ws02's 4.3 % includes the Terminal's first minutes).

What is left at rest after ws08: about 105 MB of heap (QuickJS 34 MB, one scene per display 24 MB, free space
glibc keeps 15 MB, Mesa 13 MB, wallpapers 8 MB) and 69 MB of file-backed pages (libLLVM 46 MB of them). Why each
stays: `perf/MEMORY-HEAPTRACK.md`, "What is left, and why it stays" (chart: `web/content/charts/memory-left-0891.json`).

Since 0.8.9.1 every session writes the same report to `/var/log/lordos/users/<user>/compositor.log` 10 minutes
after login and then every 6 hours (`grep memory: …/compositor.log`), so a report on any machine can say where its
memory goes.

## The Raspberry Pi 5: 0.8.8.9 against 0.8.9

Three runs of 60 minutes, each after a reboot into the autologin session (`pi-*.csv` in
[`perf/0.8.9/`](perf/0.8.9/)).

| | 0.8.8.9, Lord Flow | 0.8.9, Lord Flow | 0.8.9, second run |
|---|---:|---:|---:|
| RSS @1 min | 108.4 MB | 110.0 MB | 111.6 MB |
| RSS @20 min | 115.6 MB | 112.5 MB | 112.7 MB |
| RSS @60 min | 115.7 MB | 112.5 MB | 112.8 MB |
| RSS min / median / max | 105.8 / **115.7** / 115.7 | 102.9 / **112.6** / 112.8 | 104.3 / 112.7 / 112.8 |
| Anonymous, median | 75.6 MB | 72.2 MB | 72.3 MB |
| File-backed, median | 40.1 MB | 40.4 MB | 40.4 MB |
| CPU, % of one core | **3.4** (3.4–3.5) | **6.1** (6.0–6.1) | 6.1 (6.0–6.1) |

- **Memory:** 0.8.9's session settles about 3 MB lower than 0.8.8.9's (113 against 116 MB), and both are flat
  after the first 10 minutes: nothing grows over the hour.
- **Processor:** 0.8.9 uses almost twice as much at rest, **6.1 % against 3.4 %** of one core, steady in every
  10-minute window, so it is something that runs all the time, not a burst at login. Nothing is painted (no
  display). Finding it is on the roadmap for 0.8.9.1.
- **The "workspaces" run is a second Lord Flow run.** It was meant to measure Lord Workspaces
  (`pi-0.8.9-workspaces.csv`), but its header says `theme default`: the sampler reads the theme from the user's
  `settings.json` when the session starts, and the change written before the reboot was not there any more. Its
  numbers match the Lord Flow run within 0.2 MB and 0.02 points, so it is reported as a second 0.8.9 Lord Flow run.
  Lord Workspaces has not been measured on the Pi yet.

## Raspberry Pi with 0.8.9.1

Two runs of 60 minutes after a reboot, with `lord-server` 0.8.9.1 (built on the Pi from `main`, stripped) on top of
the installed 0.8.9 package, so the themes are 0.8.9's (`pi-0.8.9.1-*.csv` in [`perf/0.8.9.1/`](perf/0.8.9.1/); the
header's `lordos 0.8.9` is the package's version). The theme is now written with the session stopped and checked after
the reboot (`theme workspaces` in the second file's header and in the session's command line).

| | 0.8.9.1, Lord Flow | 0.8.9.1, Lord Workspaces |
|---|---:|---:|
| RSS @1 min | 109.5 MB | 99.2 MB |
| RSS @20 min | 112.5 MB | 99.4 MB |
| RSS @60 min | 112.5 MB | 99.4 MB |
| RSS min / median / max | 102.9 / **112.6** / 112.6 | 99.2 / **99.4** / 107.9 |
| Anonymous, median | 72.2 MB | 59.5 MB |
| File-backed, median | 40.4 MB | 39.9 MB |
| CPU, % of one core | **6.2** (6.0–6.2) | **0.06** (0.05–0.06) |

- **Lord Flow: the same as 0.8.9**, to 0.1 MB and 0.1 points. The memory work freed the cached rasters of
  animated windows, the Terminal's history and the huge pages of `.bss` on x86-64; a session with no monitor, no
  window and no Terminal has none of them.
- **Lord Workspaces: 99 MB and 0.06 % of one core**, 13 MB and 6 points below Lord Flow on the same server. So the
  processor Lord Flow uses at rest on the Pi comes from its theme (what its JavaScript keeps doing with nothing on
  screen), not from `lord-server`; it is still on the roadmap.

## Repeating it

1. Install the version, log out and back in (or reboot), and don't open anything you don't want measured.
2. `sudo tools/bench/session-sample.sh <user> 60 out.csv 10` from another machine over ssh, or from a console.
3. Let the session settle 3 minutes before the samples you compare (or drop the first rows).
4. RSS: `rss_kb / 1024`. CPU: `(last cpu_ticks − first cpu_ticks) / clk_tck / (last t_s − first t_s) × 100`.
5. For where the memory goes, read the `memory:` lines in `compositor.log` and, if needed, heaptrack
   (`perf/MEMORY-HEAPTRACK.md`).

## Files

| Folder | Runs |
|---|---|
| [`perf/0.8.9/`](perf/0.8.9/) | `pc-0.8.9-workspaces.csv`, `pc-0.8.9-workspaces-slideshow-off-2018.csv`, `pc-0.8.9-floor-base.csv`, `pc-ws01-floor.csv`, `pc-ws01-floor-b.csv`, `pc-ws02-floor.csv`, `pc-ws02-widgets.csv`, `pi-0.8.8.9-flow.csv`, `pi-0.8.9-flow.csv`, `pi-0.8.9-workspaces.csv` (the second Lord Flow run) |
| [`perf/0.8.9.1/`](perf/0.8.9.1/) | `pc-ws02-widgets-terminal.csv` … `pc-ws08-widgets-terminal.csv`, `pc-ws03-widgets.csv`, `heaptrack-ws06-live.txt`, `pi-0.8.9.1-flow.csv`, `pi-0.8.9.1-workspaces.csv` |

The `ws01`–`ws08` builds report themselves as `0.8.9` in the CSV header (the package's version); their build label
was checked with `strings /proc/<pid>/exe`.
