# Measuring lord-server's memory, down to the line of code

This is how the 0.8.9.1 memory work found where a desktop session's memory goes. It also explains how
anyone can repeat the measurement on their own machine. The tools are three:

1. **`tools/bench/session-sample.sh`**: samples the session's memory and processor time into a CSV. It
   answers *how much*.
2. **The memory report in `compositor.log`**: counters inside lord-server for each big consumer. It
   answers *which part of LordOS*. It is in the test builds of branch `perf/mem-0891`.
3. **heaptrack** (Debian package `heaptrack`): records every allocation with its call stack. It
   answers *which line of code*, including memory that the counters miss.

Use them in this order. Each tool is more precise than the previous one but disturbs the session more.

## The machine and the conditions

The numbers below come from one PC:

- 3 displays at 1920×1080 on an RX 580 (Mesa radeonsi);
- Debian 13 with THP (transparent huge pages) set to `always`;
- the Lord Workspaces theme;
- 7 widgets on one display;
- one Terminal running Claude Code (`claude --continue`), left alone.

Every measurement is a **fresh session**: log out, log in, set the same scene, then don't touch anything.
In LordOS 0.8.9, `systemctl restart lordos` keeps the sessions alive, so it does not give a fresh session.

Things that spoiled our measurements, so avoid them:

- **Opening a window "to check"**, for example About to see the build: +20 MB that stays.
- **The voice reading a reply aloud** (lord-tts) during the sample.
- **Sampling an old session**: the sampler waits for a *new* session's pid.

## 1. How much: session-sample.sh

```sh
sudo tools/bench/session-sample.sh <user> <minutes> out.csv 10
```

It waits for `<user>`'s session, then every 10 s writes one row:

- RSS, anonymous and file-backed memory (`/proc/<pid>/status`);
- processor time (`/proc/<pid>/stat`);
- MemAvailable.

The header records the version, theme, displays, kernel and processor. Let the session settle about 3
minutes before sampling.

## 2. Which part: the memory report

The test builds write a group of `memory:` lines to `/var/log/lordos/users/<user>/compositor.log`. The
reports come 4 and 12 minutes after login, then every 5
minutes. Each report runs after `malloc_trim(0)`, so glibc's free-but-kept pages don't count.

```
memory: rss= anon= file= thp= | malloc arena= inuse= free= mmap=(n) | images= idle= | scenes=
memory: terminals= lines= cells= replay= | blur= | transform= | layer= | glyphs=
memory: nodes= (Node B) | frames=
memory: arenas: #0=…K #1=…K …          glibc's arenas one by one (threads have their own)
memory: transform rasters: #node WxH k v age   each cached raster of a transformed subtree
memory: js malloc= used= objs= funcs=(code pc2line) strs= atoms= shapes= props=
```

This report found two of the four problems on its own:

- **Transform rasters, 57 to 114 MB.** They were freed only after more transform paints. On a desktop at
  rest there are none, so they were never freed.
- **The terminal's history table, 24 MB.** It was allocated up front for a 1,000,000-line history, with
  400 lines in use.

It also showed what it **could not** explain. After those fixes `malloc inuse` was about 100 MB, and the
counters added up to about 70 MB. The arenas line showed that almost everything (109 MB) was in the main
arena, not in threads. That gap is what heaptrack is for.

## 3. Which line of code: heaptrack

heaptrack preloads a library that records every `malloc`, `calloc`, `realloc` and `free` with its stack.
At any moment you can ask it what is still allocated and by whom. Allocations made inside Mesa, QuickJS or
libc are counted too.

### Running a session under it

lord-sessiond starts every session through `/usr/lib/lordos/bin/lord-server`. The script below puts a
one-shot wrapper there: the **next** session of one user runs under heaptrack, and everything else runs
normally.

```sh
sudo apt install heaptrack
make -C server build/lord-server                       # NOT stripped: heaptrack needs the symbols
sudo tools/bench/heaptrack-session.sh install <user> server/build/lord-server
# log <user> out and in; set the scene; wait ~15 minutes without touching anything
```

The session may take a few seconds longer to appear, and it uses more processor while it runs. Don't
compare its CPU or RSS with other runs. Use it only for *who holds the memory*.

Two traps cost us a test run each, and the script handles both:

- lord-sessiond starts lord-server with an **empty environment, without PATH**. A wrapper that doesn't
  set its own PATH can't find heaptrack (a sh script) and silently runs the plain binary.
- The helpers lord-server starts re-execute `/proc/self/exe`, which is the real binary, so they bypass
  the wrapper. heaptrack's library also clears `LD_PRELOAD`, so programs started from the Terminal are
  not traced.

### Reading the result while the session is still running

The trace is written as the session runs. Read it **before** logging out: at logout lord-server frees
things, and the snapshot of the desktop at rest is lost.

```sh
cp /var/tmp/lordos-heaptrack/heaptrack-lord-server.*.gz /tmp/ht.gz
zcat /tmp/ht.gz > /tmp/ht.raw 2>/dev/null       # the last block is incomplete: zcat stops there, that's fine
heaptrack_print -f /tmp/ht.raw --print-leaks 1 --print-peaks 0 --print-allocators 0 \
    --print-temporary 0 --print-histogram 0 --print-flamegraph 0 -n 25 -s 0 > live.txt
```

In `live.txt`, **"leaked" means "still allocated now"**, not a leak. Each entry gives the bytes, the
number of calls and the line that allocated them. Ours is in
[`0.8.9.1/heaptrack-ws06-live.txt`](0.8.9.1/heaptrack-ws06-live.txt) (paths shortened, user name
removed). Afterwards, put the normal binary back:

```sh
sudo tools/bench/heaptrack-session.sh restore
```

### What it showed (session at rest with a Terminal running Claude Code)

| MB | Where | What it is |
|---:|---|---|
| 24.9 | `main.c:900` `out_frames` | One scene per display, 3 × 1080p: needed |
| 28 | `quickjs.c` | The theme and the apps' JavaScript |
| **18.6** | **`apps.c:343` `apps_list`** | **The app table, sized for 1,024 apps** |
| 13.0 | `libgallium` | Mesa, the GPU driver |
| 8.6 | `image.c:85` | The wallpapers |
| 6.3 | `term.c:800` | Instant Replay frames of the Terminal (limit 6 MB per tab) |
| **5.8** | **`fonts.c:175` `fonts_list`** | **The font table, sized for 8,192 fonts** |
| ~10 | sound wave, shadows, firewall, PipeWire guard, packages, cloud | 0.8–3.4 MB each |

### Reserved is not resident, except with huge pages

heaptrack counts bytes **requested**. The RSS counts pages **touched**. A large `calloc` is served by
`mmap`, and its untouched pages normally cost nothing.

With THP set to `always`, though, touching one byte brings in a whole **2 MB** page. So check what is
really resident in `/proc/<pid>/smaps`: each mapping's `Rss` and `AnonHugePages`.

Ours had two anonymous mappings: 10.3 MB resident (8 MB of huge pages) and 6.3 MB (6 MB). That is how the
app table (~100 apps filled, 18.6 MB reserved) and the font table really cost about 16 MB:

```sh
awk '/^[0-9a-f]+-/{n=$6} /^Rss:/{r=$2} /^AnonHugePages:/{if(r>4096)print r, $2, n}' \
    /proc/<pid>/smaps | sort -rn | head
```

## The loop and its results

Each step is one change, built with a label that shows in Settings → About (`LORD_BUILD=…-wsNN`). We
checked the build with `strings /proc/<pid>/exe` so that About didn't have to be opened.

RSS of the session with 7 widgets and a Terminal running Claude Code at rest, 3 × 1080p:

| Build | Change | RSS |
|---|---|---:|
| ws02 | Baseline plus the memory report | 269 MB |
| ws03 | More counters (terminal, rasters, glyphs) | 334 MB* |
| ws04 | Free transform rasters unused for 30 s; clearing the scrollback frees its cells | 262 MB |
| ws05 | The history table grows on demand; raster slots give back oversized buffers | **197 MB** |
| ws06 | Counters for nodes, frames, arenas, plus heaptrack | 205 MB |
| ws07 | App and font tables cut to what the scan found | *in progress* |

\* ws03 was read right after Claude Code started.

For comparison, the same desktop **without** the Terminal measured 185 MB on ws02.

The data (CSVs, reports) is in `docs/perf/0.8.9.1/`. The report on 0.8.9 itself will be
`docs/PERFORMANCE-0.8.9.md`.
