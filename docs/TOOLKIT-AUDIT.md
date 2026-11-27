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
lordos-testbench --toolkits --show                # slowed down to watch, each check printed as it is decided
lordos-testbench --toolkits --show --wait         # then "Press Enter to close" (the launcher's Toolkit Audit entry)
lordos-testbench --frameworks                     # the same, watched in the Test Bench's Frameworks screen
```

There is also a launcher entry, **Toolkit Audit** (`lordos-toolkit-audit.desktop`), that runs it in a Terminal
window, and a desktop action of the Test Bench's entry, **Framework tests** (`lordos-testbench.desktop`, action
`frameworks`), that opens the Frameworks screen; System Information opens one or the other for the hardware report
(below).

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

**Shipped with LordOS** (in development after 0.8.9.6.1, asked for by the maintainer on 2026-11-26). The bindings the
harnesses need come with LordOS: `python3-gi`, `gir1.2-gtk-3.0`, `gir1.2-gtk-4.0`, `python3-pyqt5`, `python3-pyqt6`,
`qtwayland5` and `qt6-wayland`, with `qgnomeplatform-qt5` and `qgnomeplatform-qt6` (Qt's platform theme, below). They
are in the ISO and the Raspberry Pi image (`PKGS_TOOLKITS` in `tools/iso/build.sh` and `tools/rpi/build-img.sh`; every
one is in trixie for amd64 and arm64) and Recommends of the `lordos` package (`TOOLKITRECS` in `tools/iso/mkdeb.sh`),
so an installed system gets them with the Store's update, which installs the Recommends a new version adds.
`server/tests/toolkit_pkgs_test.mjs` keeps the three lists in step. The cost, from the trixie index's dependency
closure without Recommends (how the images install): **+142 MiB installed on the ISO** (51 packages: 126 MiB for the
bindings, which bring Qt 5, PyQt and the GObject typelibs; 16 MiB more for QGnomePlatform and adwaita-qt) and **+272 MiB
on the Raspberry Pi image** (114 packages; it had no GTK 3 or Qt before). The import probe still runs first (a person may
remove a package; a Recommends can be left out): a toolkit whose bindings are missing says **not installed**, and
nothing is ever installed by the audit. A harness that reports `{"installed": false}` (for example PyQt5 without
`qtwayland5`'s plugin) is not installed too. `x11.py` needs only Python 3, so it always runs where there is an X
display. No libX11, python3-xlib or xprop is used: LordOS links no X11 library (docs/ARCHITECTURE.md).

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
  dark mode, and its theme check failed for nothing. Likewise `QT_QPA_PLATFORMTHEME=gnome` (`wl/toolkitenv.h`), or the
  caller's own when it is a plain plugin name (lower-case letters and digits, under 32 bytes). Nothing else from the caller's environment passes (no
  `LD_PRELOAD`, no `PYTHONPATH`, no `GDK_BACKEND` of its own).
- A harness exits by itself within 5 s (GTK 4's: up to 2.4 s more when its own first frame came later than 1 s, the
  time it spent compiling its shaders on a cold cache). At **8 s** its whole process group is killed. When it exits, whatever it
  left in its group is killed as well. In show mode (below) it exits within 15 s and is killed at **20 s**.
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

## Show mode (`--show`) and the Frameworks screen

Asked for by the maintainer: when the audit runs from the native interface, the person sees the tests happen.

**The harness.** `--show` (the orchestrator passes it as the harness's last argument, a fixed word) or
`LORDOS_TOOLKIT_AUDIT_SHOW=1` (by hand; the orchestrator's clean environment never passes it) slows every harness
down so a person can follow it:

- the menu, the combo list and the tooltip stay open about 0.9 s after they mapped; the clipboard and drag steps
  stay on screen as long; fullscreen is held on, then off, for as long;
- the text is typed one character at a time, 60 to 70 ms apart;
- a label on the window names the current step by its check id (`map`, `size`, `theme`, `menu`, `combo`,
  `tooltip`, `clipboard`, `text`, `dnd`, `fullscreen`). It is a fixed id, not a text: the harnesses have no
  translations. GTK and Qt draw it with a label widget in a monospace font; `x11.py` draws it with its own 5x7
  bitmap font (lower case and `_`), since it uses no X font;
- the budget rises from 4.6 s to 14.6 s (it exits within 15 s); the orchestrator kills it at 20 s instead of 8 s.

Every hold comes after the check it shows is decided, so **the results and the JSON line are the same as in the
fast mode** (only the `ms` of the text and fullscreen checks may include the typing and the hold).
`tools/toolkit-audit/x11_test.py` checks that on Xvfb, and a headless `build/lord-server-test` gave the same
`x11-xwayland` results both ways.

**Progress lines.** Before its JSON line, a harness in show mode prints on stdout, as it goes:

```
#lordos-ta step menu                 the step on screen now
#lordos-ta check menu pass           a check just decided: pass, fail or skip
```

The orchestrator reads them as untrusted input (`tk_progress_parse`, `tk_progress_feed` in
`server/src/testbench/toolkits.c`): exactly that prefix, `step` or `check`, one of the 14 shared check ids and, for a
check, `pass`, `fail` or `skip`, separated by single spaces, the whole line under 96 bytes. Anything else is ignored
whole: another id (`global.*`, `fullscreen_on`), upper case, control or non-ASCII bytes, a missing or extra word,
extra spaces, and any longer line, even one whose end looks valid. Only indexes are kept, never text, and at most 512
lines per run. They only animate the screen: the result still comes from the JSON line, read and scored as above,
and those lines are not results (the parser skips them).

**In a terminal**, `lordos-testbench --toolkits --show` prints each check as ✓, ✗ or - under the "Testing ..." line
when stderr is a terminal; the table, the JSON and the saved file are the usual ones.

**The Frameworks screen** (`lordos-testbench --frameworks [--only id,...]`, or **F** and the **F: FRAMEWORKS**
button on the test card; `server/src/testbench/fwscreen.c`):

- the nine combinations (or the `--only` ones) as a list: waiting (a ring), running (highlighted, a spinner and its
  seconds), done (✓ and the score, ✗ with `0 %` and the fatal word, or `not-installed`, `no-display`,
  `no-harness`);
- each row's 14 checks as cells under their short codes (`MAP`, `SIZ`, `SCL`, `THM`, `BKE`, `DEC`, `FRM`, `MNU`, `CMB`,
  `TIP`, `CLP`, `DND`, `TXT`, `FUL`), ticking ✓, ✗ or - as the progress lines arrive, the current step's cell
  blinking; when the combination ends, its cells show the final result, which is exactly the JSON's;
- the global score at the top when the run ends, and a details box for the selected row (Up/Down or a click; by
  default the running one, then the first with a problem): the failed and skipped check ids with the harness's
  checked detail (printable ASCII, cut at the first `/`), a crash's signal, the globals bound and failed, the
  recommended variable (`-> GDK_BACKEND=x11`), or for a missing toolkit the packages it needs;
- **R** (or the button) runs it again, **Esc** goes back to the test card.

The run is the same `tk_audit` as `--toolkits --show`, in a thread of its own: the same folder (and
`LORDOS_TOOLKIT_AUDIT_DIR`), `/usr/bin/python3 -I`, deadlines, clean environment and process groups. It does not run
as root (the screen says "not as root"). Closing the window, or a signal, kills the running harness's group. The
result is saved to `~/.cache/lordos/toolkit-audit.json` (0600) like `--toolkits`. The bench's protocol checklist (its
test windows) waits until the person goes back to the card, and the bench's own report is not rewritten by a
Frameworks-only run.

**Where the windows go.** The screen asks for 820x600 so a harness's 600x400 window fits beside it on a common
screen, but a Wayland client can't choose where its window opens (xdg-shell has no position for a toplevel), and an
X11 harness's position is the window manager's as well. Flow opens each new window over the middle of the
desktop, so the harness's window covers part of the screen while it runs (the rows' left part and the details box);
the row, its ticks and the step on the harness's own label stay visible.

**Text.** Like the rest of the test bench, the screen draws with its own 5x7 bitmap font (ASCII; lower case was
added for ids, variables and paths), so it shows only fixed ids, numbers, symbols and a few English words, as the
card does; the bench has no translations. The hardware report's option and the desktop action's name are translated
in every language.

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
| `theme` | 1 | the toolkit's own light or dark scheme (on GTK the colour the window's text is really drawn with, else GTK's settings; Qt 6's colour scheme, else the palette Qt draws with) is the one the portal's Settings says; it passes with no portal preference |
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
  minutes for a newer file. Its second box, **Watch the tests** (off by default), runs the audit again, fresh result
  or not, in the Test Bench's Frameworks screen: `lord.apps.launchAction('lordos-testbench', 'frameworks')`, the
  launcher's own path for a desktop action the installed entry lists (`Exec=/usr/bin/lordos-testbench
  --frameworks`), and waits up to five minutes for the newer file. `apps/sysinfo/toolkits.js` builds the report's optional `toolkits` section from known
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

- **Show mode.** Progress lines: the four valid forms, and 31 hostile or odd lines refused (upper case, extra or
  missing words, double, leading or trailing spaces, `\r`, escape sequences, UTF-8, a `global.*` id, an internal id,
  shell text after the result, a path as an id, JSON), a NUL inside a line, a line of 96 bytes, byte-by-byte
  chunking, an overlong line whose end is a valid line, a flood capped at 512, and 1 MiB without a newline (the
  buffer stays bounded). Then a run of nine fake show-mode harnesses through the Frameworks screen, headless: each
  prints its 14 step and check lines with hostile lines in between (a `fail` after a `pass` hidden behind shell
  text, an escape sequence or 300 bytes of padding), which change no tick; the screen is drawn mid-run (only the
  checks reported so far ticked) and at the end, where it matches the report exactly (scores, the global score, a
  crash, not-installed rows, the details box's choice, a click on a row). `TK_SHOT_DIR=dir` keeps the pictures as
  PPMs. A harness run without `--show` refuses (the orchestrator passes it only in show mode), and the abort flag
  kills a hanging harness and the child it left in under 3 s.
- **`--last`** on the JSON the audit writes (the age in minutes, hours or days, a crash with its signal, a
  failed check), and on hand-edited files: another tool, an unknown combination, a version with shell characters,
  an unknown status, a path as a fatal word, a check id or a result, an `LD_PRELOAD=` recommendation, a text signal
  and another `v`. Each is refused as a whole and none of its text is printed. A link to the file is not followed.

The harnesses have their own checks (`tools/toolkit-audit/x11_test.py` against Xvfb, fast and in show mode). The report side is tested by
`server/tests/hwreport_test.mjs` and `build/hwsnap-test` (docs/HARDWARE-REPORT.md).

## The ws170 QA: `size` on five rows and `fullscreen` on `x11-xwayland` (in development, 2026-11-27)

On the maintainer's three-display desktop `size` failed every time on `qt6-wayland` and `qt6-xcb` (600x421), on
`gtk3-x11` (600x406, after 600 ms) and, with `--show`, on `qt5-wayland` and `qt5-xcb`; `x11-xwayland` failed
`fullscreen` in `--show` mode (BUG-12 and BUG-24 of the QA). Neither the probes' content (Qt 6's minimum is 114x165,
114x194 with the show label, under Inter 10) nor the GTK theme: **LordOS's window memory** (docs/WINDOWS.md,
"Remembered places"). The person's `settings.windowPlaces` held a frame for every harness window (`linux:gtk3.py`
600x441, `linux:Gtk3.py`, `linux:python3`, `linux:lordos-toolkit-audit-qt6` and `desktop:lordos-toolkit-audit` 602x464),
saved from earlier runs, and shared between toolkits: the harnesses' ids were `python3` (Qt 5 on Wayland), the script's
name (GTK) or `lordos-toolkit-audit` (Qt 5 on X11 and the X11 harness, which also matches the menu's
`lordos-toolkit-audit.desktop`). Every run then opened at the remembered size, and `x11-xwayland` came back from
fullscreen to it. With those five entries in a headless server's settings, `lordos-testbench --toolkits --show`
reproduced exactly that row by row (93.8 %).

**Fixed:** the shell neither remembers nor applies a place for `lordos-toolkit-audit*` app ids and X classes, in any case
(`sdk/shellxmodel.js` `remembersPlace`, used by `sdk/windowmemory.js` on save and on restore; `tests/shellx_model_test.mjs`),
and every harness names its window `lordos-toolkit-audit-<toolkit>` (GTK through `GLib.set_prgname` before the display
opens, Qt 5 through `setDesktopFileName`). The check stays what it was: a window that asks for 600x400 must get it from
the compositor. With the same stale entries, the headless run is at 100 % on all nine rows in `--show` mode, and
nothing is saved for the harnesses afterwards. The stale entries in a person's settings are left alone and no longer
read.

**GTK 4 stayed light on a dark desktop, and `theme` passed** (BUG-29 of the same QA). The LordOS portal names the GTK
theme `LordOS-dark` in dark mode, which had only a `gtk-3.0` folder: GTK 4 found no `gtk-4.0` style there and drew its light
default. `gtk4.py` passed `theme` on any portal answer and judged GTK 4 "dark" by the theme's name. Now `LordOS` and
`LordOS-dark` have a `gtk-4.0/gtk.css` that imports GTK 4's own light or dark style (`assets/gtk`, installed by the
package as before), and the GTK harnesses judge light or dark by the colour the window's text is drawn with (GTK 3's style
context, GTK 4.10's `gtk_widget_get_color`), failing when it differs from a dark or light desktop. Headless with a dark
portal: `gtk4-wayland` and `gtk4-x11` fail `theme` with the installed theme (no `gtk-4.0`) and pass with this tree's
(`XDG_DATA_DIRS`); GTK 3 passes both ways.

## The 0.8.9.6.2 QA: 98.5 % and 98.3 % on the maintainer's desktop (in development, 2026-11-27)

Two runs on the maintainer's live session (three 1080p displays, RX 580 and an Intel iGPU, ws169): `qt6-wayland`
93.6 % in both (`combo`: "the popup did not map in 1 s"), and once each `gtk3-x11` 91.6 % (`size`) and `qt5-wayland`
93 % (`combo`: "closed at once"). Every other check passed. What was done (BUG-2 of the QA):

- **Not reproduced headless.** A headless lord-server (one output, the test hooks' pointer over the window or not)
  ran `qt6-wayland`, `qt5-wayland`, `gtk3-wayland` and `gtk4-wayland` repeatedly: `combo` and `size` passed every
  time (only `theme` fails there, with no session bus or portal). So these are the real desktop's: its input (the
  keyboard in the Terminal the audit was started from, the person's own serials), its three outputs, or timing.
- **`size` waits for the configure to settle.** `gtk3.py` read the size once, 150 ms after the map; under XWayland a
  ConfigureNotify can come a frame later. It now reads again every 30 ms for at most 600 ms (as `qt5.py` already
  did), and so does `qt6.py` after its 120 ms; the detail says how long it took.
- **`qt6-wayland` `combo` tries once more and says what it saw.** When the list didn't map in 1 s, the harness
  notes whether Qt had it visible and whether the window had the keyboard, waits until the window has the keyboard
  again (`settled()`) and opens it once more: a list Qt closed because the keyboard moved is not a failure (a person
  opens it with the window in front). A second miss fails with both observations in the detail, and a pass on the
  second try says so in the detail, so the next run on the hardware tells which it is.
- **Needs the hardware:** whether the first `combo` miss on the three-display desktop is the keyboard focus (the
  window launched from a Terminal without an activation token may not get the keyboard at once) or a compositor bug
  with the popup's grab, and whether `qt5-wayland`'s "closed at once" is the same. Run
  `lordos-testbench --toolkits --only qt6-wayland,qt5-wayland,gtk3-x11` a few times there and read the details.

## Measured: every combination at 100 % (in development, 2026-11-26)

The latest release is 0.8.9.6.1; this is in development (`fix/toolkit-100`).

**Before**, on the maintainer's own desktop (Debian 13, RX 580, lord-server 0.8.9.6.1+ws158, its live session), two
runs a few minutes apart:

| Combination | Run 1 | Run 2 | What failed |
|---|---|---|---|
| `gtk3-wayland`, `gtk4-wayland`, `gtk4-x11`, `x11-xwayland` | 100 % | 100 % | |
| `gtk3-x11` | 95.8 % | 83.3 % | `theme` (toolkit light, portal dark); run 2 also `clipboard` ("another text came back") |
| `qt5-wayland` | 88.8 % | 88.8 % | `theme` (palette light); `combo` ("closed at once") |
| `qt5-xcb` | 95.8 % | 95.8 % | `theme` |
| `qt6-wayland` | 90.9 % | 95.4 % | `theme` ("qt unknown, palette light"); run 1 `combo`, run 2 `tooltip` ("did not map in 1 s") |
| `qt6-xcb` | 96.2 % | 96.2 % | `theme` |

The same failures came back on a headless `build/lord-server-test --headless 1280x800 --test-hooks` once it had what a
session has and a test server lacks: a private session bus (`dbus-daemon --session`, so lord-server serves the portal
and the theme check compares something), the first start's screens skipped (`welcomeShown`: the Help window opening
mid-run took the keyboard), and the server's environment cleared (`env -i`: an inherited `LORDOS_SESSIOND` sent its
Xwayland scope to the live session's lord-sessiond, which refused it). Run as `lordos-testbench --toolkits` with this
tree's harnesses, ten times on each of three fresh servers: the `theme` failures every time, and about one run in
five lost a Qt popup, a GTK clipboard check on X11 or GTK 4's budget.

**The causes, all on LordOS's side, and the fixes:**

- `theme` on `gtk3-x11`: GTK 3 on X11 reads only XSETTINGS, and nobody served them. The X window manager is now the
  XSETTINGS manager (`Net/ThemeName`, `Net/IconThemeName`, `Gtk/DecorationLayout`, `Gtk/FontName`; docs/WAYLAND.md,
  "XSETTINGS").
- `theme` on the four Qt rows: Qt 5 has no colour scheme, and Qt 6.8's own `gnome` and generic platform themes don't
  read the portal's (checked: with `XDG_CURRENT_DESKTOP=GNOME` Qt 6 never asked the portal). Tried with the trixie
  packages unpacked into a scratch folder: `xdgdesktopportal` and `gtk3` fix Qt 6 only (Qt 5.15's give no palette);
  QGnomePlatform (`gnome`) fixes both, palette and colour scheme. lord-server now gives apps
  `QT_QPA_PLATFORMTHEME=gnome` and LordOS ships `qgnomeplatform-qt5/-qt6` (above, and docs/WAYLAND.md, "Qt").
- `combo` on `qt5-wayland`, `combo` and `tooltip` on `qt6-wayland`: the `WAYLAND_DEBUG` log of a failed run shows the
  harness's clipboard window closing, Qt opening the tooltip 120 ms later, and the keyboard coming back to the main
  window 132 ms after the close, at which Qt destroys the popup. The keyboard came back only when the theme decided (its
  `unmap` handling, a frame or more later, slower under load); lord-server now gives it back in the same loop iteration
  to the window that had it just before (`focus_back`, wl.c; never when it came from LordOS), and a keyboard change no
  longer dismisses the newly focused window's own menus.
- `clipboard` on `gtk3-x11` ("another text came back") and `gtk4-x11` ("no offer", found headless): with
  `LORD_XWM_DEBUG=1`, the window manager offered LordOS's selection to X again at the same millisecond the app took the
  CLIPBOARD (the focus came back after fullscreen), at CurrentTime, so the app's copy was replaced; giving it up at
  CurrentTime could clear it as well. Now at the selection's last-change time from XFixes, which the X server ignores
  when the app took it since. And a Wayland app's selection still being read when a newer copy came landed afterwards
  and overwrote it (the next X focus then gave X that older text); that read is now dropped.
- `clipboard` and `dnd` timeouts on `gtk4-wayland` on a cold cache: GTK 4 spends 2.5 to 3 s compiling its shaders
  before its first frame on a software renderer (the log: lord-server's configure 5 ms after the commit, the frame
  callback 21 ms after the first buffer). A harness's budget, not LordOS: GTK 4's deadline now moves by that time.

**After**, headless with the same runner and every fix (`QT_QPA_PLATFORMTHEME=gnome` with QGnomePlatform from the
unpacked trixie packages): **every combination at 100 % in 30 of 30 runs** (ten on each of three fresh servers), and in
160 more runs of the GTK 3 and GTK 4 harnesses on both backends in the audit's order (40 rounds), where the X clipboard
race had shown. `make -C server wl-conformance` stays at 48 of 48 protocols complete, mean 100 %, X11 mean 100 %
(as recorded on 2026-11-23). Its headless server now has a private session bus and the welcome already seen
(`tools/wl-conformance.mjs`), so its toolkit section measures the theme too: on the author's PC, without
QGnomePlatform installed, GTK 3, GTK 4 and the plain X11 window score 100 % and the four Qt rows fail only `theme`, as
they should there.

Still to see on a real session: the maintainer's desktop once the `lordos` package with these fixes and its new
Recommends is installed, the Qt rows with the real `qgnomeplatform` packages (headless they came from a scratch folder),
and a GPU session's timing (headless ran on the CPU path).

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
  `tools/wl-conformance.mjs` starts it. It came up with Workspaces. `theme`: a test server has no session
  bus, so no portal. `fullscreen`: the window came back 26 px shorter (the Workspaces bug below); with Flow it
  passes.
- **VM 127** (0.8.9.6+ws148, Flow, dark mode; GTK 3.24.49, GTK 4.18.6, Qt 5.15.15, Qt 6.8.2, Xwayland 24.1.6),
  three identical runs. No crash, timeout or protocol error, and no protocol error attributed to any of the 28
  globals the toolkits bound. The failures are what an app of that toolkit really gets on LordOS:
  - `theme` on `gtk3-x11`: GTK 3 on X11 reads only XSETTINGS, which LordOS didn't serve then (served since
    2026-11-26, above), so it stayed light. GTK 3 on Wayland and GTK 4 on both follow dark mode.
  - `theme` on Qt 5 and Qt 6: neither follows the portal without a platform theme (Qt 5 has no colour scheme at all,
    and Debian's Qt 6 portal theme isn't installed), so their palette stays light. Fixed 2026-11-26 with
    QGnomePlatform (above).
  - `size` on `gtk3-wayland`: LordOS's window memory. The harness's window (`gtk3.py`) was remembered from earlier
    runs made while GTK 3 still had Adwaita's taller title bar, so its frame came back 6 px taller. With a new app
    id it is 600x400. A person whose saved size differs sees the same, by design.
  - Earlier runs also failed `x11-xwayland fullscreen` 2 times in 12 (a compositor bug, below) and `gtk3-x11
    clipboard` once (CopyQ, installed on that VM, took the clipboard).

**Compositor and shell bugs these runs found** (the first two fixed in development, `fix/ta-bugs`; see below):

- **An X11 window that asks for fullscreen before its first frame stays at its own size.** The window manager
  writes `_NET_WM_STATE_FULLSCREEN` but the window is never resized. `enter_fullscreen()` (server/src/wl/wl.c:3887)
  sets the fullscreen size in `conf_w`/`conf_h`; the first commit then maps the surface and overwrites them with the
  window's own size (wl.c:4414-4416, `s->conf_w = w`), and the next `x_configure()` (wl.c:655) sends that size to X
  while the window stays fullscreen. `wl_paint()` never corrects it for the fullscreen surface (wl.c:4676), and the
  theme isn't told either: `emit(s, "fullscreen")` runs only for a mapped surface (wl.c:3908) and the map event
  doesn't carry the state.
- **Classic and Workspaces shrink Linux windows by the menu strip's height.** Those themes give every
  Linux window a 26 px menu strip (the Edit menu with Speech, `sdk/voice.js` `useMenusWithSpeech`, on by default),
  but `sdk/wayland.js` sizes the frame with `windowChrome().h` only: on map (line 142), on the app's own resize
  (line 231) and for a restored window (line 312), never `windowChrome().menu`. The app gets 26 px less than it
  asked for, every fullscreen exit takes another 26 px (400, 374, 348 on the X11 harness), and window memory keeps
  the smaller size for the next start (322, 296).
- **Fixed.** lord-server keeps the display's size for a window whose first frame comes while it is fullscreen
  (`map_fullscreen()` in wl.c: X's own size goes to the map event as its normal size, the one it returns to, and a
  commit while fullscreen never overwrites `conf_w`/`conf_h`), and tells the theme at map (`"fullscreen": true` in
  the map event and a `fullscreen` event, also after a theme switch). The same holds for an
  `xdg_toplevel.set_fullscreen` before the first commit. `sdk/wayland.js` sizes a Linux window's frame with the menu
  strip when the theme has one (`chromeInset(chrome, csd, strip)`, `sdk/voice.js` `menusWithSpeech`) on map, on the
  app's own resize, for a restored window and for a toplevel drag, and Celeste now declares its strip too. With both,
  the X11 harness passes `fullscreen` (on 1280x800, off 600x400) three runs in a row in Flow, Classic,
  Workspaces and Celeste on the headless test server; Workspaces failed it before. Sizes window memory
  already saved smaller stay as they are (they can't be told from a size the person chose) and no longer shrink.
- **Smaller (fixed 2026-11-26, above):** the keyboard comes back to a window 60 to 75 ms after one of its child toplevels closes (VM 127). A
  Qt 6 popup the app opens in that gap is closed by Qt when the focus arrives (the harness now waits for it, as a
  person does).

