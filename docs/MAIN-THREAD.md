# The main thread: pauses, how they are found, and how to avoid them

lord-server draws the desktop and runs the theme and every native app in one QuickJS runtime, on its main
thread (`docs/ARCHITECTURE.md`, "How a frame is made"). The pointer is drawn by that thread too. A JavaScript
task that runs for 200 ms stops the pointer for 200 ms; ten of them in a row make the whole desktop feel slow.
This document covers the stall detector (0.8.9.5), the tools around it, the design rules that keep the main
thread free, and what was measured and fixed with them.

## The stall detector

Always on, in every session (`server/src/ui/js.c`):

- Every entry from C into JavaScript is timed with two clock reads: pointer and key events, gestures, timer
  ticks (`__lordTick`, where React's scheduled work runs), animation frames, replies from helpers
  (`__lordFsReply`, `__lordAdmin`, …), the end of a window move (`__lordWm`). The promise jobs right after a
  call belong to its run: React's synchronous render after a click is one.
- A run longer than 50 ms (`LORD_STALL_MS` overrides it) is written to the session's log:

  ```
  lord-server: stall: 132.4 ms js __lordEvent up [window store; react made 908, changed 81] at xo (theme.bundle.js:26:8524) < …
  ```

  what ran, whose it was (the window of the node the event was for, or of the last node React touched;
  `shell` for the theme's own bars and panels), how many nodes React made and changed, and where the
  JavaScript was when it passed the limit: a stack sampled from QuickJS's interrupt handler.
- Every native `lord.<namespace>.<function>()` call is timed too; one longer than 20 ms is logged by name
  (`stall: 63.1 ms call media.hwDecode`). The core drawing calls (`lord.setStyle`, `lord.appendChild`, …) are
  not wrapped.
- Layout and paint longer than 50 ms are logged too (`stall: 52.3 ms paint display`).
- At most 30 lines per 10 s; the rest are counted (`stall: 12 more not logged`).
- `lord.stats()` carries `stalls`, `stallWorstMs` and `slowCalls`; About shows "Pauses over 50 ms" with the
  count and the longest.

The watchdog (`server/src/wdog.h`) is still what stops a task that runs for seconds; the stall detector is about
the pauses people feel, from 50 ms up.

### Reading a stack

The bundles are minified, so a stack names places like `theme.bundle.js:26:8524`. A source map made from the
same sources turns them into files and lines; the bundle's bytes do not change when the map is made, so it fits
a bundle already installed from those sources:

```sh
LORD_SOURCEMAP=1 node tools/build-theme.mjs default
grep 'stall:' ~/.cache/lordos/compositor.log | node tools/stall-where.mjs
#   … at xo (sdk/collate.js:8) < <anonymous> (sdk/apps.js:144) < sort (native) < hs (sdk/apps.js:144) < … (apps/store/debian.js:206)
```

(`/var/log/lordos/users/<user>/compositor.log` in an installed session.) `LORD_NOMINIFY=1` builds bundles
with readable names, for a diagnosis.

## Measuring: `server/tests/perf_headless.sh`

A headless lord-server (`LORD_HEADLESS_DRAW=1`: it lays out and paints with nobody watching) runs a theme with
`server/tests/perf/driver.js` after it. The driver opens apps, clicks their sidebars by label, changes a
setting, focuses windows in turn, and logs per step (one every 1.5 s) the main thread's JavaScript, layout and
paint time, its longest single run, how many nodes React changed, and the JS heap after a full collection per
scenario (`lord.stats(1)`, `lord.stats(2)`).

```sh
cd server
tests/perf_headless.sh default settingsSet,mixed,store,settings,files,idleMonitor,launcher
PERF_STEPS=1 tests/perf_headless.sh classic mixed          # every step
PERF_BUNDLE=/other/checkout/themes/default/theme.bundle.js tests/perf_headless.sh default mixed   # compare a build
PERF_OUT=/tmp/stalls.txt tests/perf_headless.sh …           # the stall lines too
```

Plans: `idle`, `settingsSet` (one setting changed six times, no window open), `mixed` (Store, Settings and
Files open: a setting changed six times, then each window focused in turn, then idle), `store`, `settings`,
`files` (the app alone, every sidebar item clicked), `idle<App>` (the app alone for 30 s: its background work and
heap), `natives` (the native calls apps make while rendering, each timed alone), `launcher`, `terminal` and
`terminalBg` (`yes | head -c 50M`, then titles changed in a loop, with the Terminal in front or behind a window; the
`__lordTerm` events per step are logged), `titles` (a window's title set four times a step), `settingsPanes` (every
Settings page opened by its id, each followed by a quiet step), `flatpak` (Settings → Flatpak opened twice, with
`PERF_FLATPAK_FILES` files, 200 000 by default, put in the test home's Flatpak folders first) and `settingsNatives`
(the native calls Settings makes, each timed alone). `PERF_MARK=1` logs a line before each step, so the stall lines
of `PERF_OUT` can be put under the step they happened in.

## Rules

1. **A store change re-renders every subscriber, synchronously.** `useStore` is `useSyncExternalStore`, whose
   updates are never time-sliced: whatever re-renders because of a store runs in one go while the pointer
   waits. Subscribe to what the component shows: `useStore(settings, (s) => s.key)`, never the whole settings
   object in a component that holds much (a theme's root: `useTrackedSettings()`, which re-renders only for
   the keys read from it). A selector must return the same value for the same state (a string, a number, an
   object kept in the store), never a new object.
2. **An app renders for its own reasons only.** `AppHost` is memoized and so is the app's element, so a
   theme's root rendering again (a window moved, the notifications changed) stops at each window. The window's
   context (`useAppWindow()`) changes only with its id, args, mode and overlay host; `focused` is followed only by
   the components that read it while rendering (`useAppFocused()` too).
3. **The theme's tokens keep their identity** while they are equal (`ThemeProvider`): a theme makes them again
   on every render of its root, and a new object made every `useTheme()` reader render again.
4. **Expensive work is done once per input**, not per render or per card: lists sorted by name use
   `sortByName` (`sdk/collate.js`, the name's pieces made once per item and kept), `useLauncherApps()` makes and
   sorts the list once for what it depends on and shares it.
5. **An action that may take a while answers the press first.** `afterPaint(fn)` runs `fn` once the frame that
   shows the press is on the screen; `usePending(fn)` gives `[run, busy, error]`; `<Toggle pending>` flips at
   once, shows a spinner on its knob while `onChange` (a promise, or a call that blocks) runs, follows `on`
   when it ends, and flips back with `onError` when it fails; `<Button pending>` and `<Button busy>` show a
   spinner and ignore presses. `<Spinner>` is the ring they use (still with Reduce motion).
6. **No disk, network or process wait on the main thread** when an asynchronous call exists (`lord.fs.*Async`,
   the admin calls, the helpers' replies). A folder's size is `fs.duAsync(path, ms)` (`lord.fs.duAsync`, 2026-11-23:
   `lord.fs.du`'s checks and walk on an `fsasync.c` worker, at most 10 s), never `lord.fs.du` on a folder that can
   hold many files; a page shows "calculating…" until it answers and may keep the answer a little while. Anything in
   a cloud or network folder (`fs.isRemote`) goes through the Async calls or a copy job (`lord.fs.copy`), reads and
   writes included.
7. **Code that runs on every settings change does nothing costly** unless its own settings changed: a subscriber to
   `settings` runs on every write anywhere (each switch, a window moved), so it compares its own part first.

## Measurements (0.8.9.5, development PC, headless 1920 × 1080, CPU path)

Longest single run of the main thread per scenario (ms), before the fixes (commit 795da6cd: the detector only)
and after. The numbers vary by a few ms between runs (other work on the machine); the Store's sidebar and its
categories had been fixed already (0efdde63).

| Scenario | Lord Flow | Lord Classic | Lord Workspaces |
|---|---|---|---|
| A setting changed, Store, Settings and Files open | 161 → 20 | 163 → 18 | 171 → 27 |
| A setting changed, no window open | 29 → 31 (noise) | 51 → 13 | 43 → 4 |
| Focusing a window (Store, Settings, Files open) | 241 → 31 | 173 → 40 | 174 → 39 |
| Opening the Store with two apps open | 509 → 197 | 469 → 232 | 521 → 210 |
| Opening the Store alone | 468 → 152 | | |
| The Store's sidebar (categories, pages) | 168 → 91 | | |
| Files: a folder of 2220 entries (`/usr/share/doc`) | 1986 → 229 (heap +69 MB → +9 MB, 20 497 → 3217 nodes) | | |
| Files: Recents | 662 → 151 | | |
| Settings: every pane | 50 → 69 (noise) | | |
| Monitor alone, 30 s | 12 → 44 (noise: its charts) | | |
| The app launcher (F4) | 62 → 63 | 22 → 11 | 35 → 27 |

Before, the run that changed a setting with three apps open re-rendered all three apps; after, only the
components that read that setting. The heap stays flat over repeated actions (Store, Settings, focus, idle: a
few tens of KB up and down); Monitor's grows by ~600 KB in its first minute (its 60-second sample history) and
then stays.

## The Terminal's titles (0.8.9.5)

A program that animates its title (Claude Code's spinner) re-rendered the whole Terminal window and the theme's root for
every change: 1310 `__lordTerm` pauses in two hours of the maintainer's session, the longest 329 ms. The window now
subscribes only to its active session's profile and title, titles are applied once per frame, and the window manager
gets the title at most four times a second (`apps/terminal/index.jsx`, `state.js`). `tests/perf_headless.sh default
terminal`, title changed about 60 times a second: 1043–1162 ms of JavaScript per 1.5 s step before, 140–160 ms after;
longest run 47 → 19 ms. A window title still costs the theme about 240 React updates (its root follows `useWindows()`);
moving the title out of the root's subscription is left for the themes.

## Settings, page by page (2026-11-23)

The maintainer's report: opening Settings → Flatpak froze the pointer for about 2 seconds. Every page was then opened
by its id in a headless session (`PERF_MARK=1 PERF_FLATPAK_FILES=200000 tests/perf_headless.sh default settingsPanes`:
200 000 small files in the test home's `~/.local/share/flatpak` and `~/.var/app`, each page followed by 1.5 s of
nothing), and the native calls Settings makes were timed alone (`settingsNatives`). Longest single run of the main
thread, ms; "before" is the code of 0.8.9.5-beta, "after" two runs of the fixed code (the machine was shared with
other builds, so a few tens of ms move between runs; the second run was the busier one):

| Page | Opening: before | after | Next 1.5 s: before | after | What it was |
|---|---|---|---|---|---|
| General | 13.8 | 14.7 / 13.8 | 2.3 | 2.2 | |
| Appearance | 14.2 | 19.1 / 17.5 | 12.4 | 11.8 | |
| Wallpaper | 48.9 | 48.2 / 50.7 | 15.2 | 14.4 | rendering (386 nodes made) |
| Desktop & Dock | 22.1 | 21.1 / 20.4 | 14.9 | 15 | |
| Displays | 37.4 | 36.6 / 42.5 | 9.5 | 48 | rendering |
| Network | 13.5 | 14.4 / 42 | 8.5 | 7.7 | |
| Bluetooth | 16.7 | 15 / 35.1 | 7.9 | 7.9 | |
| Sound | 13.3 | 13.2 / 13.6 | 8.4 | 11.8 | |
| Notifications | 14.9 | 14.9 / 14.7 | 12.4 | 44.1 | |
| Printers | 16.7 | 16.6 / 35.4 | 9.1 | 11.2 | |
| Clipboard | 12.7 | 14.2 / 34 | 10.8 | 43.7 | |
| Sharing | 21 | 16.5 / 54.5 | 7.6 | 36.6 | rendering (177 nodes made) |
| Cloud | 35.7 | 21.4 / 24.1 | 8.2 | 7.5 | rendering (the account mock) |
| Language & Region | 17.9 | 15.5 / 53.8 | 13.7 | 31.1 | rendering; the language-pack lookup listed Downloads on every render (now `fs.listAsync`, once) |
| Date & Time | 25.7 | 18.5 / 58.7 | 46.3 | 9.5 | rendering |
| Keyboard | 22.8 | 36.3 / 20.6 | 17.2 | 19.3 | |
| Mouse & Trackpad | 21.2 | 45.9 / 46 | 16.5 | 49.2 | rendering |
| Default apps | 54.3 | 29.1 / 50.5 | 8 | 33.3 | rendering (592 nodes changed) |
| Web apps | 17.1 | 24.3 / 55.3 | 8.1 | 11.6 | rendering; each profile's size was `lord.fs.du` (up to 300 ms each, every 30 s), now `fs.duAsync` |
| **Flatpak** | **147.9** | **13.5 / 37.5** | 78.9 | 31.1 | **`lord.fs.du` of `~/.local/share/flatpak` and `~/.var/app` (133–155 ms here, up to 2 s allowed): now `fs.duAsync`** |
| AI | 34.8 | 30.3 / 29.6 | **125.9** | 19.2 | **a settings write listed the cameras (below)** |
| Battery | 32.3 | 32.5 / 35.4 | **100.5** | 38.1 | the same |
| Power | 14.1 | 13.5 / 47.6 | **73.4** | 13.6 | the same |
| Cooling | 53.2 | 16 / 27.2 | 13.6 | 7.4 | rendering |
| Board firmware | 20.4 | 18.2 / 29.5 | 7.5 | 11.9 | |
| Graphics cards | 53.7 | 81.6 / 87.4 | 7.7 | 35.8 | rendering and layout (21 ms of layout): not a native call (`vm.gpus` 0.4 ms, `sysinfo.gpuReport` 1.5 ms) |
| Privacy & Security | 29.1 | 31 / 68.2 | 9.8 | 39.1 | rendering (436 nodes made) |
| Users | 24.2 | 29.2 / 49 | 8.3 | 34.7 | |
| Software Update | 14.3 | 14 / 51.7 | 12.5 | 14 | |
| Session | 14.1 | 14.3 / 18.8 | 8.8 | 12.9 | |

- **Every settings write cost 60–70 ms on a computer with webcams.** LordOS Studio (`sdk/studio.jsx`) subscribes to the
  settings and called `lord.media.cameras()` (it opens every `/dev/video*`) on each write, whoever made it: a switch
  in any page, a window moved, the session saved. In the run above the session was saved every 3 s, which is the
  "next 1.5 s" column of AI to Software Update before. It now returns at once when its own settings are as last
  applied (rule 7) and lists the cameras only when one was chosen: `settings.set` of one key 65.1 → 0.3 ms.
- **Native calls Settings makes, timed alone** (`settingsNatives`, this PC): `pkg.plan` remove flatpak 14–17 ms (only
  on "Remove Flatpak completely"), `time.zoneOffset` for 313 zones 2–7 ms, `locale.available` 1.9 ms,
  `sysinfo.gpuReport` 1.5 ms, `pkg.upgrades` 1.6 ms, the rest under 1 ms; `lord.fs.du` of the seeded Flatpak folder
  134–155 ms and `media.cameras` 64–67 ms were the two over the line.
- **Actions not run headless:** Cloud → Check This Account (needs a real account) moved off the main thread by
  reading the code: its listings, folder, file write and read-back (a download once the file is freed) all touched the
  FUSE mount on the main thread; they are now `fs.*Async` and copy jobs (`apps/settings/Cloud.jsx`, `sdk/cloudcheck.js`).
  Network → VPN → Add listed seven folders when the sheet opened; now `fs.listAsync`. Bluetooth, power profiles, the
  time zone, printers and the admin actions already answer later (D-Bus replies, `admin.run`).
- What is left over 50 ms on opening a page is React building it (Wallpaper, Sharing, Privacy, Graphics cards, Default
  apps: 200–600 nodes in one run); no Settings page waits on a disk, a device or a process any more.
- Test: `tests/settings_nofreeze.sh` (in `make test`) opens Settings → Flatpak twice with the 200 000 files and fails
  on a `call fs.du` stall line, a page that never shows a size, or an opening over 250 ms.

## What remains

- Opening the Store builds its first page (about 1000 nodes) in one run of about 200 ms.
- Monitor redraws its charts in JavaScript four times a second while it has the focus (about 10 % of the main
  thread with the Overview tab shown).
- Components that read `settings.get()` while rendering and rely on a parent's re-render to see a change keep
  `useSettings()` (every change); moving them to selectors is done one component at a time.
