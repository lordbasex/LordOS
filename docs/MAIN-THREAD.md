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
heap), `natives` (the native calls apps make while rendering, each timed alone), `launcher`.

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
   the admin calls, the helpers' replies).

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

## What remains

- Opening the Store builds its first page (about 1000 nodes) in one run of about 200 ms.
- Monitor redraws its charts in JavaScript four times a second while it has the focus (about 10 % of the main
  thread with the Overview tab shown).
- Components that read `settings.get()` while rendering and rely on a parent's re-render to see a change keep
  `useSettings()` (every change); moving them to selectors is done one component at a time.
