# Toolkit audit (`lordos-testbench --toolkits`)

How well the Linux GUI toolkits work on LordOS's compositor, measured on the person's own machine with a command
that ships in the `lordos` package. Nobody has to clone the repository: the harnesses are installed in
`/usr/lib/lordos/toolkit-audit/` and the orchestrator is part of `/usr/bin/lordos-testbench`. The developer-side,
point-by-point protocol scores stay in `tools/wl-conformance.mjs` (docs/WAYLAND.md, "Conformance"); this audit asks
the other question: what does a real GTK or Qt app see here?

```sh
lordos-testbench --toolkits                       # the table, then the matrix of Wayland globals bound
lordos-testbench --toolkits --json                # the result below, one line
lordos-testbench --toolkits --only gtk4-wayland,qt6-xcb
lordos-testbench --toolkits --last                # the saved result, checked and summed up (what lordos-issue adds)
```

There is also a launcher entry, **Toolkit Audit** (`lordos-toolkit-audit.desktop`), that runs it in a Terminal
window; System Information opens it for the hardware report (below).

Run it inside a LordOS session (it needs the session's Wayland socket and, for the X11 rows, `DISPLAY`). Each
harness opens one 600x400 window for a second or two, one after the other, and takes the clipboard for a moment.

## What it runs

Nine combinations, in this order:

| Id | Harness | Backend variable |
|---|---|---|
| `gtk3-wayland`, `gtk3-x11` | `gtk3.py` (PyGObject, Gtk 3.0) | `GDK_BACKEND=wayland` / `x11` |
| `gtk4-wayland`, `gtk4-x11` | `gtk4.py` (PyGObject, Gtk 4.0) | `GDK_BACKEND=wayland` / `x11` |
| `qt5-wayland`, `qt5-xcb` | `qt5.py` (PyQt5) | `QT_QPA_PLATFORM=wayland` / `xcb` |
| `qt6-wayland`, `qt6-xcb` | `qt6.py` (PyQt6) | `QT_QPA_PLATFORM=wayland` / `xcb` |
| `x11-xwayland` | `x11.py` (the X11 core protocol by hand, standard library only) | none |

**No new dependency.** LordOS depends on no toolkit and no binding. Before a harness runs, a quick import probe
(`python3 -I -c "import gi; gi.require_version('Gtk', '4.0')"`, `import PyQt5.QtCore, PyQt5.QtWidgets`, ...) checks
that its bindings are already installed; otherwise the row says **not installed**. Nothing is ever installed. A
harness that reports `{"installed": false}` (for example PyQt5 without `qtwayland5`'s plugin) is not installed too.
`x11.py` needs only Python 3, so it always runs where there is an X display. No libX11, python3-xlib or xprop is
used: LordOS links no X11 library (docs/ARCHITECTURE.md).

**How a harness runs** (`server/src/testbench/tkrun.c`):

- `/usr/bin/python3 -I <harness>` as the person, in its own process group, stdin from `/dev/null`, with no shell.
  Never as root: `lordos-testbench --toolkits` run with `sudo` or `pkexec` stops with a message.
- **Only a harness nobody else could have changed runs.** The folder and the file must not be links and must not
  be writable by group or others; in `/usr/lib/lordos/toolkit-audit` both must be root's (the package installs the
  harnesses 0644 in a 0755 folder), and in a development folder (`LORDOS_TOOLKIT_AUDIT_DIR`) root's or the
  person's. Otherwise the row says the harness is missing.
- A clean environment: `PATH=/usr/local/bin:/usr/bin:/bin`, `LANG=LC_ALL=C.UTF-8` (so the fatal texts below are
  English), `HOME`, the `XDG_*` folders and session type, `DBUS_SESSION_BUS_ADDRESS` (the theme check reads the
  portal), then either `WAYLAND_DISPLAY` (Wayland rows, no `DISPLAY`) or `DISPLAY` and `XAUTHORITY` (X11 rows, no
  `WAYLAND_DISPLAY`; without `XAUTHORITY` it uses `$XDG_RUNTIME_DIR/lord-xauth.<n>` when lord-server wrote it), the
  backend variable, `WAYLAND_DEBUG=1`, `GTK_A11Y=none`, `NO_AT_BRIDGE=1` and `PYTHONDONTWRITEBYTECODE=1`, and
  `GSETTINGS_BACKEND=keyfile`, what lord-server gives every app it starts (`ui/portal.c`; the caller's own value only
  when it is `keyfile`, `dconf` or `memory`). Without it GTK 3 read dconf's defaults instead of LordOS's theme and
  dark mode, and its theme check failed for nothing. Nothing else from the caller's environment passes (no
  `LD_PRELOAD`, no `PYTHONPATH`, no `GDK_BACKEND` of its own).
- A harness exits by itself within 5 s. At **8 s** its whole process group is killed. When it exits, whatever it
  left in its group is killed as well.
- `LORDOS_TOOLKIT_AUDIT_DIR` (an absolute folder) replaces `/usr/lib/lordos/toolkit-audit` for development.

**No test hooks.** The compositor runs as it always does. The harness checks what it can see from the inside, and
the orchestrator reads two streams:

- **stdout:** the harness's one JSON line (the last valid line wins; at most 64 KiB are kept, and a line cut there is
  ignored).
- **stderr:** read line by line, at most 4096 bytes of each line.
  - **The globals bound:** `WAYLAND_DEBUG` lines `-> wl_registry#N.bind(name, "iface", version, new id ...#M)`
    (libwayland 1.23; `@` instead of `#` before it). Only names among the 48 of `expected_globals.h` count; others
    are counted as `extra`, never named.
  - **Protocol errors:** `wl_display#1.error(object#id, code, "...")` and libwayland's `object#id: error ...`. The
    error is attributed to the global its object came from: every `new id` in a request or event inherits the
    global of the object it was sent on, starting from the `bind`.
  - **Fatal texts**, in lines that are not `WAYLAND_DEBUG` lines:
    - `protocol-error`: "protocol error".
    - `connection-lost`: "Lost connection to Wayland compositor", "Wayland connection broke", "Error reading events
      from display", "Error flushing display".
    - `egl`: "EGL_BAD_", "EGL Error", "Failed to initialize EGL", "Failed to create EGL", "Could not create the egl",
      "Could not initialize EGL", "eglInitialize failed".
    - `vulkan`: "VK_ERROR_DEVICE_LOST", "VK_ERROR_OUT_OF_DEVICE_MEMORY", "VK_ERROR_SURFACE_LOST".
    - `platform-plugin`: Qt's "Could not load the Qt platform plugin", "no Qt platform plugin could be initialized".

  Warnings (`libEGL warning`, `Gtk-WARNING`, a renderer falling back from Vulkan) are not fatal.

## The score

The same shape as `tools/wl-conformance.mjs`. Each check has a weight; a skipped check is not counted. The score
is passed points / counted points x 100, one decimal, rounded down.

| Check id | Weight | What the harness checks |
|---|---|---|
| `map` | 2 | the window is mapped |
| `size` | 2 | its size is the 600x400 it asked for |
| `backend` | 2 | the toolkit really uses the backend asked for (`x11.py`: the server is Xwayland) |
| `first_frame` | 2 | the first frame is drawn (ms from the harness's start) |
| `decoration` | 2 | client-side or server-side decorations, as the toolkit expects |
| `menu` | 3 | a menu opens and stays mapped |
| `combo` | 3 | a combo box's list opens and stays mapped |
| `clipboard` | 3 | a clipboard round trip within itself, through the compositor or X server |
| `dnd` | 3 | a drag and drop within itself (often skipped: it needs a real pointer) |
| `scale` | 1 | the scale factor |
| `theme` | 1 | the toolkit's own light or dark scheme (GTK's settings, Qt 6's colour scheme, else the palette Qt draws with) is the one the portal's Settings says; it passes with no portal preference |
| `tooltip` | 1 | a tooltip popup maps |
| `text` | 1 | a text field receives text typed through the toolkit's own synthetic input |
| `fullscreen` | 1 | fullscreen on (the monitor's size) and off (600x400 again) |

On Wayland, **each of the 48 globals the toolkit bound is one more check** (`global.<interface>`, weight 1). It
passes when it was bound at a version of 1 or more and no protocol error was attributed to it. A global the toolkit
didn't bind isn't counted.

**0 %:** a crash (a signal), a kill at 8 s, a fatal text or protocol error in stderr, or no valid line. The
row then names the reason.

**Global score:** the mean of the combinations that ran (scored or 0 %), one decimal, rounded down. Rows not
installed or without a display don't count.

**Recommended variable:** when a toolkit's Wayland row is at least 20 points under its X11 row, and the X11 row ran
with 50 % or more, the table says so and names the variable, for example `GDK_BACKEND=x11` or
`QT_QPA_PLATFORM=xcb`.

## Privacy

A result carries only toolkit names and versions, the backend, numbers and check ids. The harness's free text
(`detail`) is shown in the table, printable ASCII and cut at the first `/`, but never written to the JSON. The
`WAYLAND_DEBUG` log (which holds the window title) is parsed in memory and never copied. Versions keep only
`[0-9A-Za-z.+~-]`.

## The JSON (`--json`, and the saved file)

The last result is saved to `$XDG_CACHE_HOME/lordos/toolkit-audit.json` (or `~/.cache/lordos/toolkit-audit.json`),
mode 0600 in 0700 folders, written whole and renamed into place, for `lordos-issue` and the hardware report. Keys
are only ever added (`v` changes if one changes meaning).

```json
{
  "tool": "lordos-testbench", "v": 1, "mode": "toolkits", "at": 1764072000000,
  "source": "docs/wayland-protocols.json", "expected": 48,
  "weights": { "map": 2, "size": 2, "scale": 1, "theme": 1, "backend": 2, "decoration": 2, "first_frame": 2,
               "menu": 3, "combo": 3, "tooltip": 1, "clipboard": 3, "dnd": 3, "text": 1, "fullscreen": 1, "global": 1 },
  "score": 97.2,
  "ran": 5,
  "combos": [
    {
      "id": "gtk4-wayland", "toolkit": "gtk4", "backend": "wayland",
      "status": "ran",
      "fatal": null,
      "version": "4.18.6", "backendSeen": "wayland", "exit": 0, "ms": 1450,
      "score": 98.0, "points": 49, "total": 50, "skipped": 1,
      "checks": [
        { "id": "map", "weight": 2, "result": "pass", "ms": 115 },
        { "id": "dnd", "weight": 3, "result": "skip" },
        { "id": "global.xdg_wm_base", "weight": 1, "result": "pass", "version": 6 }
      ],
      "protocol": { "bound": 26, "failed": 0, "extra": 0, "errors": 0, "unattributed": 0 },
      "recommend": null
    }
  ],
  "matrix": {
    "globals": [ "wl_compositor", "wl_shm", "..." ],
    "rows": [ { "combo": "gtk4-wayland", "cells": [ 6, 1, "..." ] } ]
  }
}
```

| Key | Meaning |
|---|---|
| `at` | when it ran, ms since the epoch |
| `expected` | the globals of LordOS's protocol table (`expected_globals.h`, generated from `docs/wayland-protocols.json`) |
| `score` | the global score, or `null` when nothing ran |
| `ran` | how many combinations ran (scored or 0 %) |
| `combos[].status` | `ran`, `fatal` (0 %), `not-installed`, `no-display` or `no-harness` (a broken install) |
| `combos[].fatal` | `timeout`, `crash` (with `signal`), `protocol-error`, `connection-lost`, `egl`, `vulkan`, `platform-plugin`, `no-result`, or `null` |
| `combos[].version` | the toolkit's version (`x11`: the X server's), `""` when unknown |
| `combos[].backendSeen` | the backend the harness says it got: `wayland`, `x11` or `""` |
| `combos[].exit` | the exit code, `-1` when it was signalled or didn't run |
| `combos[].ms` | how long the harness took |
| `combos[].score` | 0 to 100, `null` when not scored (not installed, or every check skipped) |
| `combos[].points`, `total`, `skipped` | the weights passed and counted, and the shared checks skipped |
| `combos[].checks` | the 14 shared checks (`result`: `pass`, `fail` or `skip`; `ms` when measured), then on Wayland one `global.<interface>` per global bound, with the `version` bound. Empty when it didn't run |
| `combos[].protocol` | Wayland rows that ran: globals bound and failed, binds outside the table (`extra`), protocol errors, and errors that couldn't be attributed |
| `combos[].recommend` | Wayland rows: the variable to start that toolkit's apps with, or `null` |
| `matrix.globals` | the 48 interfaces, in the table's order |
| `matrix.rows[].cells` | one per global: the version bound, `0` not bound, negative (minus the version) when a protocol error was attributed to it |

## Where the result goes

The saved file (`~/.cache/lordos/toolkit-audit.json`) is read by three things, and each treats it as the person's own
file that may have been edited:

- **The hardware report.** System Information → Report → Share on GitHub → **Include framework compatibility**
  (off by default, never remembered). A result from the last day is used as it is; an older one or none opens the
  Toolkit Audit entry in a Terminal window (System Information runs no command itself) and waits up to three
  minutes for a newer file. `apps/sysinfo/toolkits.js` builds the report's optional `toolkits` section from known
  ids and numbers, and the schema drops anything else: see docs/HARDWARE-REPORT.md, "Toolkits".
- **`lordos-issue`** runs `lordos-testbench --toolkits --last` as the person. It reads the file without following a
  link (a regular file of theirs, at most 256 KiB), parses it strictly, and prints a few English lines (the age,
  the global score, each combination's score, version and failed check ids, a recommendation) only when every
  value is a known id, status, fatal word, check id, `[0-9A-Za-z.+~-]` version or number. Anything else gives one
  line, "the saved result is unreadable".
- **`tools/wl-conformance.mjs`** (developers) runs `build/lordos-testbench --toolkits --json` with this tree's
  harnesses (`LORDOS_TOOLKIT_AUDIT_DIR=tools/toolkit-audit`) against its own headless lord-server, with a private
  `XDG_CACHE_HOME`, and adds the result as its `toolkits` section (`--no-toolkits` skips it). It changes none of the
  48 globals' scores.

## Tests

`make -C server test` builds and runs `build/toolkit-audit-test` (`server/tests/toolkit_audit_test.c`), with
ASan and UBSan. It covers:

- **The log reader.** It reads both libwayland formats and keeps the highest version of a global bound twice. It
  ignores discarded messages, overflowing or zero versions, odd names and a fake `new id` inside a quoted string.
  It attributes errors through the object chain, recognises every fatal text and none of the warnings, reads only
  the first 4096 bytes of an oversized line, and copes with control characters, chunking and 30 000 objects.
- **The harness's line.** It rejects another toolkit's line, bad JSON, raw control characters, duplicate keys and
  deep nesting. It handles hostile values: shell characters in the version, an unknown backend, `ok` of the wrong
  type, odd `ms`, and a detail with escapes and a path. A 64 KiB stdout keeps the result before its cut line, and
  `installed: false` is read.
- **The score.** The weights, rounding down, skipped checks, and globals counted only on Wayland. A protocol
  error, a crash, a timeout and no result each give 0 %. It also tests the global mean and the recommendation's
  thresholds.
- **The output.** The JSON parses, carries no detail text, and has the matrix. The table and the placeholders
  are checked too.
- **The environment.** Only the listed variables pass, and Wayland and X11 rows get the right display and
  backend variables. `GSETTINGS_BACKEND` is `keyfile` unless the caller has another GLib backend (a path is not
  one). The Xwayland cookie is found, a missing display gives no-display, and a full buffer is refused.
- **Which harness runs.** A group-writable harness, a world-writable folder, a link and a relative folder are
  refused (the row says no harness).
- **Real runs** through a fake `python3` and fake harnesses (shell scripts in a temporary folder):
  - fixed JSON on both backends, with a 100 KiB line after the result;
  - a protocol error attributed to `xdg_wm_base`;
  - noise and a half line after the result;
  - a crash (SIGSEGV);
  - a hang with a child of its own, killed at the (shortened) deadline with its whole group;
  - a failed import probe, `installed: false`, a missing harness and a missing `python3`;
  - the saved file's modes and contents.

- **`--last`** on the JSON the audit writes (the age in minutes, hours or days, a crash with its signal, a
  failed check), and on hand-edited files: another tool, an unknown combination, a version with shell characters,
  an unknown status, a path as a fatal word, a check id or a result, an `LD_PRELOAD=` recommendation, a text signal
  and another `v`. Each is refused as a whole and none of its text is printed. A link to the file is not followed.

The harnesses have their own checks (`tools/toolkit-audit/x11_test.py` against Xvfb). The report side is tested by
`server/tests/hwreport_test.mjs` and `build/hwsnap-test` (docs/HARDWARE-REPORT.md).

## Measured (in development, 2026-11-25)

Two real runs of `lordos-testbench --toolkits` from `feat/ta-report`, with this tree's harnesses
(`LORDOS_TOOLKIT_AUDIT_DIR`). The latest release is 0.8.9.6.1; this is in development.

| Combination | Maintainer's PC (headless test server) | VM 127 (installed LordOS, its own session) |
|---|---|---|
| `gtk3-wayland` | not installed | 95.2 % (`size`, see below) |
| `gtk3-x11` | not installed | 95.8 % (`theme`) |
| `gtk4-wayland` | not installed | 100 % |
| `gtk4-x11` | not installed | 100 % |
| `qt5-wayland` | not installed | 97.2 % (`theme`) |
| `qt5-xcb` | not installed | 95.8 % (`theme`) |
| `qt6-wayland` | not installed | 97.7 % (`theme`) |
| `qt6-xcb` | not installed | 96.2 % (`theme`) |
| `x11-xwayland` | 92.5 % (`theme`, `fullscreen`) | 100 % |
| **Global** | **92.5 %** (1 of 9 ran) | **97.5 %** (9 of 9 ran) |

- **The maintainer's PC** (Debian 13, Xwayland 24.1.6, python3-gi without the GTK typelibs, no PyQt): a headless
  `build/lord-server-test --headless 1280x800 --test-hooks --user` with a private `XDG_RUNTIME_DIR` and `HOME`, as
  `tools/wl-conformance.mjs` starts it. It came up with Lord Workspaces. `theme`: a test server has no session
  bus, so no portal. `fullscreen`: the window came back 26 px shorter (the Workspaces bug below); with Lord Flow it
  passes.
- **VM 127** (0.8.9.6+ws148, Lord Flow, dark mode; GTK 3.24.49, GTK 4.18.6, Qt 5.15.15, Qt 6.8.2, Xwayland 24.1.6),
  three identical runs. No crash, timeout or protocol error, and no protocol error attributed to any of the 28
  globals the toolkits bound. The failures are what an app of that toolkit really gets on LordOS:
  - `theme` on `gtk3-x11`: GTK 3 on X11 reads only XSETTINGS, which LordOS doesn't serve (docs/WAYLAND.md), so it
    stays light. GTK 3 on Wayland and GTK 4 on both follow dark mode.
  - `theme` on Qt 5 and Qt 6: neither follows the portal without a platform theme (Qt 5 has no colour scheme at all,
    and Debian's Qt 6 portal theme isn't installed), so their palette stays light. A toolkit limitation.
  - `size` on `gtk3-wayland`: LordOS's window memory. The harness's window (`gtk3.py`) was remembered from earlier
    runs made while GTK 3 still had Adwaita's taller title bar, so its frame came back 6 px taller. With a new app
    id it is 600x400. A person whose saved size differs sees the same, by design.
  - Earlier runs also failed `x11-xwayland fullscreen` 2 times in 12 (a compositor bug, below) and `gtk3-x11
    clipboard` once (CopyQ, installed on that VM, took the clipboard).

**Compositor and shell bugs these runs found** (to fix separately):

- **An X11 window that asks for fullscreen before its first frame stays at its own size.** The window manager
  writes `_NET_WM_STATE_FULLSCREEN` but the window is never resized. `enter_fullscreen()` (server/src/wl/wl.c:3887)
  sets the fullscreen size in `conf_w`/`conf_h`; the first commit then maps the surface and overwrites them with the
  window's own size (wl.c:4414-4416, `s->conf_w = w`), and the next `x_configure()` (wl.c:655) sends that size to X
  while the window stays fullscreen. `wl_paint()` never corrects it for the fullscreen surface (wl.c:4676), and the
  theme isn't told either: `emit(s, "fullscreen")` runs only for a mapped surface (wl.c:3908) and the map event
  doesn't carry the state.
- **Lord Classic and Lord Workspaces shrink Linux windows by the menu strip's height.** Those themes give every
  Linux window a 26 px menu strip (the Edit menu with Speech, `sdk/voice.js` `useMenusWithSpeech`, on by default),
  but `sdk/wayland.js` sizes the frame with `windowChrome().h` only: on map (line 142), on the app's own resize
  (line 231) and for a restored window (line 312), never `windowChrome().menu`. The app gets 26 px less than it
  asked for, every fullscreen exit takes another 26 px (400, 374, 348 on the X11 harness), and window memory keeps
  the smaller size for the next start (322, 296).
- **Smaller:** the keyboard comes back to a window 60 to 75 ms after one of its child toplevels closes (VM 127). A
  Qt 6 popup the app opens in that gap is closed by Qt when the focus arrives (the harness now waits for it, as a
  person does).

